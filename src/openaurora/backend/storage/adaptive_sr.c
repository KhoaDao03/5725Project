/*-------------------------------------------------------------------------
 *
 * adaptive_sr.c
 *		Adaptive Smart Replay metrics collection and controller
 *
 * This module implements lightweight metrics tracking with exponential moving
 * averages (EWMAs) and a periodic controller that adjusts replay budget based
 * on system load, queue depth, and read latency.
 *
 * IDENTIFICATION
 *		src/backend/storage/adaptive_sr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>

#include "storage/adaptive_sr.h"
#include "utils/guc.h"
#include "miscadmin.h"

/*
 * Default configuration parameters.
 * These match typical production settings but can be tuned via GUCs.
 */
static ASRConfig asr_default_config = {
	.QSTAR = 100.0,			/* Expect ~100 pending records is healthy */
	.RSTAR = 0.05,			/* 5% hot miss rate is acceptable */
	.WSTAR = 10 * 1024 * 1024,	/* 10 MB/s WAL rate */
	.BMIN = 10,				/* Min 10 records/tick */
	.BMAX = 2000,			/* Max 2000 records/tick */
	.WQ = 0.3,				/* Queue weight */
	.WM = 0.6,				/* Hot miss dominates */
	.WW = 0.1,				/* WAL rate weight */
	.HYST = 20,				/* Hysteresis threshold */
	.MAX_STEP = 0.2,		/* Max 20% change per tick */
	.enable_adaptive_sr = false,	/* Disabled by default */
	.verbose_metrics = false	/* Quiet by default */
};

/* Current config (mutable, protected by asr_config_lock) */
static ASRConfig asr_config;
static pthread_rwlock_t asr_config_lock = PTHREAD_RWLOCK_INITIALIZER;

/*
 * Atomic metrics for lock-free updates.
 * These are updated inline in hot paths (no locks).
 */
typedef struct {
	/* Raw counters */
	_Atomic(uint64_t) replay_tasks_count;	/* Number of replay tasks */
	_Atomic(uint64_t) hot_misses;			/* Number of hot miss events */
	_Atomic(uint64_t) wal_bytes_received;	/* Total WAL bytes received */
	
	/* Smoothed values (EWMA) */
	double		queue_ewma;
	double		miss_rate_ewma;
	double		wal_bps_ewma;
	
	/* Derived state */
	int			current_budget;
	double		current_aggressiveness;
	uint64_t	last_total_misses;
	uint64_t	last_total_tasks;
	uint64_t	last_wal_bytes;
	time_t		last_measurement;
	
	/* Lock for smoothed values and derived state */
	pthread_mutex_t metrics_lock;
} ASRMetricsInternal;

static ASRMetricsInternal asr_metrics = {
	.replay_tasks_count = 0,
	.hot_misses = 0,
	.wal_bytes_received = 0,
	.queue_ewma = 0.0,
	.miss_rate_ewma = 0.0,
	.wal_bps_ewma = 0.0,
	.current_budget = 100,
	.current_aggressiveness = 0.0,
	.last_total_misses = 0,
	.last_total_tasks = 0,
	.last_wal_bytes = 0,
	.last_measurement = 0,
	.metrics_lock = PTHREAD_MUTEX_INITIALIZER
};

/* Flag to signal controller shutdown */
static volatile sig_atomic_t asr_shutdown_requested = 0;

/* PID of controller thread (if running) */
static pid_t asr_controller_pid = 0;
static pthread_t asr_controller_tid = 0;

/*
 * Exponential moving average: new_val = alpha * raw + (1 - alpha) * old_val
 * alpha higher = more responsive to changes, lower = more smoothing.
 */
#define EWMA_ALPHA 0.3

static double
ewma_update(double old_val, double new_val)
{
	return (EWMA_ALPHA * new_val) + ((1.0 - EWMA_ALPHA) * old_val);
}

/*
 * Compute normalized pressure from a raw value vs its expected level.
 * Returns [0.0, 1.0] with 0 = below threshold, 1 = at or above 2x threshold.
 */
static double
compute_pressure(double raw, double expected)
{
	double e;
	
	if (raw <= expected)
		return 0.0;
	
	e = (raw / expected) - 1.0;
	if (e > 1.0)
		e = 1.0;
	
	return e;
}

/*
 * ASR_RecordReplayTask - Record completion of replay tasks (thread-safe).
 * Called from wal_redo.c after each record is applied.
 */
void
ASR_RecordReplayTask(int count)
{
	if (!asr_config.enable_adaptive_sr || count <= 0)
		return;
	
	atomic_fetch_add(&asr_metrics.replay_tasks_count, (uint64_t)count);
}

/*
 * ASR_RecordHotMiss - Record a hot miss event (thread-safe).
 * Called from rpcserver.cpp when GetPage@LSN blocks on incomplete replay.
 */
void
ASR_RecordHotMiss(void)
{
	if (!asr_config.enable_adaptive_sr)
		return;
	
	atomic_fetch_add(&asr_metrics.hot_misses, 1);
}

/*
 * ASR_RecordWalIngest - Record WAL bytes received (thread-safe).
 * Called from walreceiver.c or wherever WAL is appended.
 */
void
ASR_RecordWalIngest(size_t bytes)
{
	if (!asr_config.enable_adaptive_sr || bytes == 0)
		return;
	
	atomic_fetch_add(&asr_metrics.wal_bytes_received, (uint64_t)bytes);
}

/*
 * ASR_GetCurrentBudget - Get current replay budget (thread-safe).
 */
int
ASR_GetCurrentBudget(void)
{
	int budget;
	
	pthread_mutex_lock(&asr_metrics.metrics_lock);
	budget = asr_metrics.current_budget;
	pthread_mutex_unlock(&asr_metrics.metrics_lock);
	
	return budget;
}

/*
 * ASR_SetBudget - Update replay budget (controller only, thread-safe).
 */
void
ASR_SetBudget(int budget)
{
	pthread_mutex_lock(&asr_metrics.metrics_lock);
	asr_metrics.current_budget = budget;
	pthread_mutex_unlock(&asr_metrics.metrics_lock);
}

/*
 * Compute next budget from aggressiveness level.
 * A in [0.0, 1.0] maps to [BMIN, BMAX].
 */
static int
budget_from_aggressiveness(double A, const ASRConfig *cfg)
{
	int budget;
	
	/* Clamp aggressiveness */
	if (A < 0.0) A = 0.0;
	if (A > 1.0) A = 1.0;
	
	budget = (int)floor(cfg->BMIN + A * (cfg->BMAX - cfg->BMIN));
	return budget;
}

/*
 * ASR_ReadMetrics - Get current smoothed metrics snapshot (thread-safe).
 * Called periodically to read controller state.
 */
ASRMetrics
ASR_ReadMetrics(void)
{
	ASRMetrics snapshot = {0};
	
	pthread_mutex_lock(&asr_metrics.metrics_lock);
	
	snapshot.replay_queue_length = asr_metrics.queue_ewma;
	snapshot.hot_miss_rate = asr_metrics.miss_rate_ewma;
	snapshot.wal_ingest_bps = asr_metrics.wal_bps_ewma;
	snapshot.aggressiveness = asr_metrics.current_aggressiveness;
	snapshot.replay_budget = asr_metrics.current_budget;
	snapshot.last_update = asr_metrics.last_measurement;
	
	pthread_mutex_unlock(&asr_metrics.metrics_lock);
	
	return snapshot;
}

/*
 * Update smoothed metrics from raw atomic counters.
 * This is called periodically by the controller.
 */
static void
asr_update_smoothed_metrics(void)
{
	time_t now;
	double dt, new_queue, new_miss_rate, new_wal_bps;
	uint64_t total_misses, total_tasks, total_wal_bytes;
	double eq, em, ew, aggressiveness;
	int new_budget, last_budget;
	int delta;
	const ASRConfig *cfg;
	
	/* Get current config */
	pthread_rwlock_rdlock(&asr_config_lock);
	cfg = &asr_config;
	
	now = time(NULL);
	pthread_mutex_lock(&asr_metrics.metrics_lock);
	
	/* Time since last measurement */
	if (asr_metrics.last_measurement == 0)
		dt = 1.0;
	else
		dt = (double)(now - asr_metrics.last_measurement);
	if (dt < 0.1) dt = 0.1;	/* Minimum granularity */
	
	/* Read atomic counters */
	total_tasks = atomic_load(&asr_metrics.replay_tasks_count);
	total_misses = atomic_load(&asr_metrics.hot_misses);
	total_wal_bytes = atomic_load(&asr_metrics.wal_bytes_received);
	
	/*
	 * Estimate replay queue length from rate of tasks being applied.
	 * This is a rough approximation; ideally we'd read from actual queue.
	 */
	uint64_t tasks_delta = total_tasks - asr_metrics.last_total_tasks;
	new_queue = (dt > 0) ? (double)tasks_delta / dt : 0.0;
	asr_metrics.queue_ewma = ewma_update(asr_metrics.queue_ewma, new_queue);
	asr_metrics.last_total_tasks = total_tasks;
	
	/*
	 * Hot miss rate: fraction of read events that were hot misses.
	 * Approximation: misses / (misses + some baseline read count).
	 * For now, just track smoothed miss count as a proxy.
	 */
	uint64_t misses_delta = total_misses - asr_metrics.last_total_misses;
	if (total_tasks > asr_metrics.last_total_tasks)
	{
		new_miss_rate = (double)misses_delta / (double)(tasks_delta + 1);
	}
	else
	{
		new_miss_rate = 0.0;
	}
	asr_metrics.miss_rate_ewma = ewma_update(asr_metrics.miss_rate_ewma, new_miss_rate);
	asr_metrics.last_total_misses = total_misses;
	
	/*
	 * WAL ingest rate in bytes per second.
	 */
	uint64_t wal_delta = total_wal_bytes - asr_metrics.last_wal_bytes;
	new_wal_bps = (dt > 0) ? (double)wal_delta / dt : 0.0;
	asr_metrics.wal_bps_ewma = ewma_update(asr_metrics.wal_bps_ewma, new_wal_bps);
	asr_metrics.last_wal_bytes = total_wal_bytes;
	
	/*
	 * Compute pressures in [0.0, 1.0].
	 */
	eq = compute_pressure(asr_metrics.queue_ewma, cfg->QSTAR);
	em = compute_pressure(asr_metrics.miss_rate_ewma, cfg->RSTAR);
	ew = compute_pressure(asr_metrics.wal_bps_ewma, cfg->WSTAR);
	
	/*
	 * Weighted aggressiveness: hot miss rate dominates.
	 */
	aggressiveness = cfg->WQ * eq + cfg->WM * em + cfg->WW * ew;
	if (aggressiveness > 1.0) aggressiveness = 1.0;
	if (aggressiveness < 0.0) aggressiveness = 0.0;
	
	/* Apply smoothing/step limits to aggressiveness */
	double delta_a = aggressiveness - asr_metrics.current_aggressiveness;
	if (fabs(delta_a) > cfg->MAX_STEP)
	{
		if (delta_a > 0)
			aggressiveness = asr_metrics.current_aggressiveness + cfg->MAX_STEP;
		else
			aggressiveness = asr_metrics.current_aggressiveness - cfg->MAX_STEP;
	}
	asr_metrics.current_aggressiveness = aggressiveness;
	
	/*
	 * Map aggressiveness to budget.
	 */
	new_budget = budget_from_aggressiveness(aggressiveness, cfg);
	last_budget = asr_metrics.current_budget;
	
	/* Apply hysteresis */
	delta = new_budget - last_budget;
	if (delta < 0) delta = -delta;
	if (delta < cfg->HYST)
		new_budget = last_budget;
	
	asr_metrics.current_budget = new_budget;
	asr_metrics.last_measurement = now;
	
	/*
	 * Verbose logging (if enabled).
	 */
	if (cfg->verbose_metrics)
	{
		ereport(LOG,
				(errmsg("[ASR] metrics: queue=%.2f miss_rate=%.4f wal_bps=%.0f "
						"pressures(q=%.2f m=%.2f w=%.2f) agg=%.2f budget=%d",
						asr_metrics.queue_ewma,
						asr_metrics.miss_rate_ewma,
						asr_metrics.wal_bps_ewma,
						eq, em, ew,
						aggressiveness,
						asr_metrics.current_budget)));
	}
	
	pthread_mutex_unlock(&asr_metrics.metrics_lock);
	pthread_rwlock_unlock(&asr_config_lock);
}

/*
 * Controller thread main loop.
 * Periodically reads metrics and updates budget.
 * Runs independently of replay worker.
 */
static void *
asr_controller_main(void *arg)
{
	/* Controller cycle: 200ms */
	const long CONTROLLER_CYCLE_MS = 200;
	
	while (!asr_shutdown_requested)
	{
		/* Update smoothed metrics and compute new budget */
		asr_update_smoothed_metrics();
		
		/* Sleep for one cycle */
		usleep(CONTROLLER_CYCLE_MS * 1000);
	}
	
	return NULL;
}

/*
 * ASR_Init - Initialize the ASR subsystem.
 * Called once at storage_server startup.
 */
void
ASR_Init(void)
{
	pthread_rwlock_wrlock(&asr_config_lock);
	memcpy(&asr_config, &asr_default_config, sizeof(ASRConfig));
	pthread_rwlock_unlock(&asr_config_lock);
	
	/* Initialize current budget */
	asr_metrics.current_budget = asr_config.BMIN;
	
	ereport(LOG,
			(errmsg("[ASR] initialized, adaptive_sr=%s",
					asr_config.enable_adaptive_sr ? "enabled" : "disabled")));
}

/*
 * ASR_StartController - Start the controller thread.
 * Called from storage_server.c main.
 */
void
ASR_StartController(void)
{
	int ret;
	
	if (!asr_config.enable_adaptive_sr)
	{
		ereport(LOG,
				(errmsg("[ASR] not starting controller (disabled via config)")));
		return;
	}
	
	asr_shutdown_requested = 0;
	ret = pthread_create(&asr_controller_tid, NULL, asr_controller_main, NULL);
	
	if (ret != 0)
	{
		ereport(WARNING,
				(errmsg("[ASR] failed to create controller thread: %s",
						strerror(ret))));
		return;
	}
	
	ereport(LOG,
			(errmsg("[ASR] controller thread started")));
}

/*
 * ASR_Shutdown - Shutdown the ASR subsystem.
 * Called on storage_server shutdown.
 */
void
ASR_Shutdown(void)
{
	int ret;
	
	asr_shutdown_requested = 1;
	
	if (asr_controller_tid != 0)
	{
		ret = pthread_join(asr_controller_tid, NULL);
		if (ret != 0)
		{
			ereport(WARNING,
					(errmsg("[ASR] failed to join controller thread: %s",
							strerror(ret))));
		}
		else
		{
			ereport(LOG,
					(errmsg("[ASR] controller thread shut down")));
		}
		asr_controller_tid = 0;
	}
}

/*
 * ASR_GetConfig - Return pointer to current config (read-lock protected).
 */
const ASRConfig *
ASR_GetConfig(void)
{
	pthread_rwlock_rdlock(&asr_config_lock);
	return &asr_config;
	/* Note: caller must call ASR_ReleaseConfigLock() when done */
}

/*
 * ASR_UpdateConfig - Update config from GUC values or config file.
 * Called when configuration changes.
 */
void
ASR_UpdateConfig(const ASRConfig *new_config)
{
	if (new_config == NULL)
		return;
	
	pthread_rwlock_wrlock(&asr_config_lock);
	memcpy(&asr_config, new_config, sizeof(ASRConfig));
	pthread_rwlock_unlock(&asr_config_lock);
	
	ereport(LOG,
			(errmsg("[ASR] config updated, adaptive_sr=%s",
					asr_config.enable_adaptive_sr ? "enabled" : "disabled")));
}
