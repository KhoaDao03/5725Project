/*-------------------------------------------------------------------------
 *
 * adaptive_sr.h
 *		Adaptive Smart Replay metrics and controller interface
 *
 * Adaptive Smart Replay (ASR) extends OpenAurora's Smart Replay with dynamic
 * replay budget adjustment based on runtime metrics. This header defines the
 * metrics collection API, controller configuration, and budget management.
 *
 * IDENTIFICATION
 *		src/include/storage/adaptive_sr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ADAPTIVE_SR_H
#define ADAPTIVE_SR_H

#include "postgres.h"
#include <stdint.h>
#include <time.h>

/*
 * Configuration parameters for the Adaptive SR controller.
 * These can be tuned via GUCs or config files to adjust behavior.
 */
typedef struct {
	/* Expected "healthy" queue length (approximate WAL records pending) */
	double		QSTAR;
	
	/* Expected "healthy" hot miss rate (fraction, 0.0-1.0) */
	double		RSTAR;
	
	/* Expected "healthy" WAL ingest rate (bytes per second) */
	double		WSTAR;
	
	/* Minimum replay budget (records/pages per tick) */
	int			BMIN;
	
	/* Maximum replay budget (records/pages per tick) */
	int			BMAX;
	
	/* Control weights: hot miss rate is most important */
	double		WQ;		/* Queue length weight */
	double		WM;		/* Hot miss rate weight (highest priority) */
	double		WW;		/* WAL ingest rate weight */
	
	/* Hysteresis: don't update budget if change < HYST */
	int			HYST;
	
	/* Maximum step change per tick (limit aggressiveness changes) */
	double		MAX_STEP;
	
	/* Enable/disable ASR controller */
	bool		enable_adaptive_sr;
	
	/* Verbosity for debug logging */
	bool		verbose_metrics;
} ASRConfig;

/*
 * Smoothed runtime metrics snapshot.
 * Values are exponential moving averages to reduce noise.
 */
typedef struct {
	/* Approximate pending WAL records/tasks */
	double		replay_queue_length;
	
	/* Fraction of reads that blocked waiting for replay [0.0-1.0] */
	double		hot_miss_rate;
	
	/* WAL arrival rate in bytes per second */
	double		wal_ingest_bps;
	
	/* Timestamp of last measurement */
	time_t		last_update;
	
	/* Current computed aggressiveness level [0.0-1.0] */
	double		aggressiveness;
	
	/* Current replay budget (records/pages per tick) */
	int			replay_budget;
} ASRMetrics;

/*
 * Public API for metrics collection and controller
 */

/* Record metrics updates (thread-safe) */
extern void ASR_RecordReplayTask(int count);
extern void ASR_RecordHotMiss(void);
extern void ASR_RecordWalIngest(size_t bytes);

/* Read current smoothed metrics snapshot */
extern ASRMetrics ASR_ReadMetrics(void);

/* Get current replay budget */
extern int ASR_GetCurrentBudget(void);

/* Set current replay budget (controller only) */
extern void ASR_SetBudget(int budget);

/* Initialize ASR subsystem */
extern void ASR_Init(void);

/* Start controller thread (from storage_server.c) */
extern void ASR_StartController(void);

/* Shutdown ASR subsystem */
extern void ASR_Shutdown(void);

/* Get current config */
extern const ASRConfig* ASR_GetConfig(void);

/* Update config (typically from GUC) */
extern void ASR_UpdateConfig(const ASRConfig *new_config);

#endif
