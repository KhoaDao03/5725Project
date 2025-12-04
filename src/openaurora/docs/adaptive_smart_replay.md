# Adaptive Smart Replay (ASR) Implementation for OpenAurora

## Overview

**Adaptive Smart Replay (ASR)** extends OpenAurora's Smart Replay (SR) with dynamic replay budget adjustment based on runtime metrics. Instead of a static replay rate, ASR periodically monitors system load and adjusts how much replay work is done per time slice to optimize for:

- **Tail latency**: Faster response to read requests under heavy write load
- **CPU efficiency**: Lower replay rates during light load to save power
- **Queue stability**: Prevents replay lag from exploding

### Key Principle

Only the **amount of replay work changes**, not **what** gets replayed or **in what order**. This preserves:
- WAL format and LSN ordering
- MVCC semantics
- Smart Replay's hot-page prioritization
- Page layout

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Storage Server Process                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────┐     ┌─────────────────────────────────┐  │
│  │  WAL Receiver    │     │  RPC GetPage@LSN Handler        │  │
│  │  (walreceiver.c) │────→│  (rpcserver.cpp)                │  │
│  └──────────────────┘     │  Records hot misses             │  │
│   Records WAL bytes       └─────────────────────────────────┘  │
│                                      ↓                          │
│  ┌──────────────────┐     ┌─────────────────────────────────┐  │
│  │  WAL Redo Worker │←─────│  ASR Metrics Module             │  │
│  │  (wal_redo.c)    │      │  (adaptive_sr.c)                │  │
│  │ ApplyXlogUntil   │      │ - Smooths metrics (EWMA)        │  │
│  │ respects budget  │      │ - Computes aggressiveness       │  │
│  └──────────────────┘      │ - Updates budget periodically   │  │
│   Records replay tasks      └─────────────────────────────────┘  │
│                                      ↑                          │
│                            ┌─────────────────┐                  │
│                            │ Controller Loop │                  │
│                            │ (200ms cycles)  │                  │
│                            └─────────────────┘                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Files Modified/Created

### New Files

1. **`src/include/storage/adaptive_sr.h`**
   - Public API header
   - Defines `ASRConfig`, `ASRMetrics` structs
   - Function declarations for metrics, budget management, controller

2. **`src/backend/storage/adaptive_sr.c`**
   - Metrics collection module
   - Thread-safe atomic counters with EWMA smoothing
   - Controller thread with periodic budget updates
   - Hysteresis and step-change limiting

### Modified Files

1. **`src/backend/tcop/wal_redo.c`**
   - Added `#include "storage/adaptive_sr.h"`
   - Modified `ApplyXlogUntil()`: reads budget, counts replayed records, breaks when budget exhausted
   - Records metrics: `ASR_RecordReplayTask(1)` after each log application

2. **`src/backend/tcop/storage_server.c`**
   - Added `#include "storage/adaptive_sr.h"`
   - Added `ASR_Init()` in main after KvStore initialization
   - Added `ASR_StartController()` before entering RPC server loop

3. **`src/backend/storage/rpc/rpcserver.cpp`**
   - Added `#include "storage/adaptive_sr.h"`
   - Added `ASR_RecordHotMiss()` when GetPage@LSN must wait for replay (listSize > 0)

4. **`src/backend/replication/walreceiver.c`**
   - Added `#include "storage/adaptive_sr.h"`
   - Added `ASR_RecordWalIngest(segbytes)` in `XLogWalRcvWrite()` when RPC_REMOTE_DISK

---

## Implementation Details

### 1. Metrics Collection (`adaptive_sr.c`)

**Lock-free atomic counters** updated inline in hot paths:
```c
_Atomic(uint64_t) replay_tasks_count;    /* Replayed records */
_Atomic(uint64_t) hot_misses;            /* Read blocks waiting for replay */
_Atomic(uint64_t) wal_bytes_received;    /* WAL ingest rate */
```

**EWMA Smoothing** (α=0.3) reduces noise:
```c
new_ewma = 0.3 * raw_value + 0.7 * old_ewma
```

**Metrics snapshot** returned via `ASR_ReadMetrics()`:
```c
typedef struct {
    double replay_queue_length;   /* Derived from task rate */
    double hot_miss_rate;         /* Misses / tasks ratio */
    double wal_ingest_bps;        /* Bytes/sec */
    double aggressiveness;        /* Computed control output */
    int    replay_budget;         /* Current limit */
} ASRMetrics;
```

### 2. Controller Algorithm (`asr_update_smoothed_metrics()`)

**Step 1: Normalize pressures** to [0, 1]:
```c
double eq = pressure(queue_ewma, QSTAR);      /* Queue pressure */
double em = pressure(miss_rate_ewma, RSTAR);  /* Read latency pressure */
double ew = pressure(wal_bps_ewma, WSTAR);    /* Write load pressure */
```

Where:
```c
pressure(x, x_star) = clamp((x / x_star) - 1.0, 0.0, 1.0)
```

**Step 2: Weight and combine** (hot miss dominates):
```c
double A = WQ * eq + WM * em + WW * ew;  /* e.g., 0.3*eq + 0.6*em + 0.1*ew */
A = clamp(A, 0.0, 1.0);
```

**Step 3: Apply step limiting** to prevent jitter:
```c
if (|A - A_old| > MAX_STEP) {
    A = A_old ± MAX_STEP;  /* Cap rate of change */
}
```

**Step 4: Map to budget** with hysteresis:
```c
new_budget = BMIN + A * (BMAX - BMIN);
if (|new_budget - old_budget| < HYST) {
    keep old_budget;  /* Avoid thrashing */
}
```

**Running every 200ms** allows responsive adjustment without excessive overhead.

### 3. Budgeted Replay (`wal_redo.c` - `ApplyXlogUntil()`)

Key modifications:
```c
int replay_budget = ASR_GetCurrentBudget();  /* Read current budget */
int records_replayed = 0;

while(reader_state->EndRecPtr < lsn) {
    /* Read and replay one record */
    record = XLogReadRecord(reader_state, &err_msg);
    if (record == NULL) break;
    
    polar_xlog_decode_data(reader_state);
    RmgrTable[record->xl_rmid].rm_redo(reader_state);  /* Apply */
    
    /* Track and check budget */
    records_replayed++;
    ASR_RecordReplayTask(1);  /* Metrics update */
    
    if (records_replayed >= replay_budget) {
        break;  /* Exit, let caller invoke again */
    }
}
```

**Important**: 
- LSN ordering preserved (sequential replay)
- Smart Replay prioritization unchanged
- Only the loop count limit is added

---

## Configuration

### Default Parameters (`adaptive_sr.c`)

```c
QSTAR = 100.0              /* Healthy queue length */
RSTAR = 0.05               /* Healthy hot miss rate (5%) */
WSTAR = 10MB/s             /* Healthy WAL rate */
BMIN = 10                  /* Min 10 records/tick */
BMAX = 2000                /* Max 2000 records/tick */
WQ = 0.3, WM = 0.6, WW = 0.1  /* Weights (miss dominates) */
HYST = 20                  /* Hysteresis threshold */
MAX_STEP = 0.2             /* Max 20% aggressiveness change/tick */
```

### Tuning via Code

To adjust behavior, edit `asr_default_config` in `adaptive_sr.c`:

```c
static ASRConfig asr_default_config = {
    .QSTAR = 150.0,        /* Expect more pending tasks */
    .RSTAR = 0.10,         /* Allow 10% hot miss rate */
    .WSTAR = 20*1024*1024, /* Expect higher WAL rate */
    .BMIN = 20,            /* More aggressive minimum */
    .BMAX = 3000,          /* Higher ceiling */
    .WM = 0.7,             /* Prioritize latency even more */
    .enable_adaptive_sr = true,  /* Enable ASR */
    .verbose_metrics = true,     /* Log metrics */
};
```

Then rebuild:
```bash
cd postgresql-13.0
./configure --prefix=$INSTALL_DIR [other flags...]
make
make install
```

---

## Metrics Logging

When `verbose_metrics = true`, periodic log entries like:

```
[ASR] metrics: queue=45.2 miss_rate=0.0234 wal_bps=8234000.0 
      pressures(q=0.45 m=0.47 w=0.82) agg=0.56 budget=1120
[ASR] metrics: queue=120.5 miss_rate=0.0512 wal_bps=25000000.0 
      pressures(q=1.0 m=1.0 w=1.0) agg=1.0 budget=2000
[ASR] metrics: queue=5.2 miss_rate=0.001 wal_bps=1000000.0 
      pressures(q=0.0 m=0.0 w=0.1) agg=0.01 budget=10
```

Interpretation:
- **Heavy write load**: Queue and miss rates high → aggressiveness increases → budget approaches BMAX
- **Light load**: All pressures low → aggressiveness decreases → budget drops toward BMIN
- **Balanced**: Mix of pressures → budget settles mid-range

---

## Testing

### Test 1: Light Load (Verify Budget Decreases)

```bash
# Start storage node
export PGDATA=$STORAGE_PATH
$INSTALL_DIR/bin/postgres --rpc-server

# In another terminal, light OLTP workload
pgbench -c 5 -j 2 -T 60 postgres
```

**Expected behavior**:
- Queue, miss_rate, wal_bps all low
- Aggressiveness → 0.0
- Budget → BMIN (e.g., 10 records/tick)

### Test 2: Heavy Write Load (Verify Budget Increases)

```bash
# Storage node (same as above)

# Heavy write workload
pgbench -c 50 -j 10 -T 60 -r postgres
```

**Expected behavior**:
- Queue and miss_rate spike
- Wal_bps increases
- Aggressiveness increases
- Budget → BMAX (e.g., 2000 records/tick)

### Test 3: Mixed Workload (Verify Responsiveness)

```bash
# Ramp up gradually, then drop
for i in {1..10}; do
    pgbench -c $((i*5)) -j 2 -T 6 postgres
done
```

**Expected behavior**:
- Budget ramps up smoothly (respects MAX_STEP)
- No jitter (hysteresis prevents oscillation)
- Read latency improves as budget increases

---

## Correctness Guarantees

### 1. LSN Ordering Preserved ✓
- `ApplyXlogUntil()` reads records sequentially from `reader_state`
- Budget only affects **how many** records in one call, not **which ones**
- Next call resumes from `reader_state->EndRecPtr`

### 2. MVCC Semantics Unchanged ✓
- Smart Replay's version map and LogIndex untouched
- Multi-version page store logic unmodified
- Budget acts purely as a loop counter

### 3. Page Layout Intact ✓
- `RmgrTable[].rm_redo()` called identically
- No changes to page reconstruction logic
- Replay produces same page images as before

### 4. Thread Safety ✓
- Metrics use `_Atomic()` for lock-free updates
- Budget read via `ASR_GetCurrentBudget()` with mutex protection
- Controller runs in separate thread, no interference with replay worker

---

## Debugging

### Enable Verbose Logging

Edit `adaptive_sr.c`, change:
```c
.verbose_metrics = true,
```

Rebuild and restart storage server. Check logs:
```bash
tail -f $PGDATA/server.log | grep ASR
```

### Check Current Metrics

Add a test function to query `ASR_ReadMetrics()` from a utility:
```c
ASRMetrics m = ASR_ReadMetrics();
printf("Budget: %d, Agg: %.2f, Queue: %.1f, Misses: %.4f\n",
       m.replay_budget, m.aggressiveness, m.replay_queue_length, m.hot_miss_rate);
```

### Validate Budget Integration

Instrument `ApplyXlogUntil()` with debug prints:
```c
if (records_replayed >= replay_budget) {
    ereport(DEBUG1, (errmsg("Budget hit: %d >= %d", records_replayed, replay_budget)));
    break;
}
```

---

## Summary of Changes

| File | Change | Impact |
|------|--------|--------|
| `adaptive_sr.h` (new) | Metrics API | Defines interface |
| `adaptive_sr.c` (new) | Controller + metrics | Core ASR logic |
| `wal_redo.c` | Budget-aware loop | Enforces replay limit |
| `storage_server.c` | Init + start | Lifecycle management |
| `rpcserver.cpp` | Hot miss tracking | Measures read latency |
| `walreceiver.c` | WAL ingest tracking | Measures write load |

**Total lines added**: ~1200 (mostly in new files)
**Existing code modified**: <50 lines across 4 files
**Correctness risk**: Minimal (budget is orthogonal to replay logic)

---

## Future Enhancements

1. **GUC Integration**: Expose `logdb_enable_adaptive_sr`, thresholds, weights as tunable GUCs
2. **Latency Histograms**: Track p50, p99, p999 read latency directly
3. **Learning Controller**: Gradient-based or ML-based budget optimization
4. **Per-Page Budgets**: Different budgets for hot vs cold pages
5. **Adaptive Thresholds**: Learn QSTAR, RSTAR, WSTAR from workload

---

## References

- PolarDB for PostgreSQL: LogIndex + version map foundation
- Aurora design: Storage disaggregation inspiration
- Classic Control Theory: EWMA + proportional control

---

