# Patch: v2.8.4 → v2.8.5

**Date:** 2026-09-21  
**File:** `dht.c`  
**Patches:** #15, #16

---

## Patch #15 — `last_attempt_time` updated before `cmpxchg`

### Problem

The rate-limit timestamp `last_attempt_time` was written **before**
checking the `measuring` flag. When a measurement was skipped because
another thread already held the flag, the timestamp was still updated —
consuming the rate-limit budget without performing any work.

Under concurrent access (manual measure vs auto-poll, or multiple
auto-poll threads), a skipped attempt could delay the next real
measurement by up to `MEAS_MIN_GAP` seconds (2 s by default).

### Root Cause

In `dht_do_measurement()`, the timestamp was set unconditionally:

```c
/* BUG (v2.8.4): */
    sensor->last_attempt_time = ktime_get_real_seconds();

    if (atomic_cmpxchg(&sensor->measuring, 0, 1)) {
        /* Another measurement in progress — skip */
        ...
        return;          /* ← timestamp already consumed */
    }
    /* proceed with measurement */
```

Timeline showing the race:

| Time   | Thread A (auto-poll)      | Thread B (manual)        | `last_attempt_time` | `measuring` |
|--------|---------------------------|--------------------------|----------------------|-------------|
| 0.0 s  | set `last_attempt_time`   | —                        | 0 s                  | 0           |
| 0.0 s  | `cmpxchg` → 0, got flag   | —                        | 0 s                  | 1           |
| 0.0 s  | begin bit-bang (~20 ms)   | set `last_attempt_time`  | 0 s                  | 1           |
| 0.0 s  | (busy)                    | `cmpxchg` → 1, skip      | 0 s (wasted!)        | 1           |
| 0.02 s | finish, reset flag        | —                        | 0 s                  | 0           |
| 1.0 s  | next poll cycle           | —                        | 0 s                  | 0           |
| 1.0 s  | `now - last < MIN_GAP`    | —                        | —                    | —           |
| 1.0 s  | **skip (rate-limited!)**  | —                        | —                    | —           |

Thread B's skipped attempt at t=0 set `last_attempt_time` to 0 s.
Thread A's next cycle at t=1 s sees `now - 0 = 1 < 2` → rate-limited,
even though the last **real** measurement was at t=0 and 1 s has passed.

### Fix

Move the timestamp update **after** the successful `cmpxchg` — only
when the flag is actually claimed and a measurement will proceed:

```c
/* FIX (v2.8.5): */
    if (atomic_cmpxchg(&sensor->measuring, 0, 1)) {
        /* Another measurement in progress — skip.
         * last_attempt_time is NOT updated. */
        ...
        return;
    }
    /* Flag claimed — now record the attempt time */
    sensor->last_attempt_time = ktime_get_real_seconds();
    /* proceed with measurement */
```

Skipped attempts no longer consume rate-limit budget. The next
caller sees the timestamp of the last **real** measurement, not the
last failed try.

### Diff

```diff
--- dht_v2.8.4.c
+++ dht_v2.8.5.c
@@ -1125,14 +1125,14 @@
-    sensor->last_attempt_time = ktime_get_real_seconds();
-
     if (atomic_cmpxchg(&sensor->measuring, 0, 1)) {
         /* Another measurement is already in progress */
         if (manual) {
             ...
         }
         return;
     }
+    sensor->last_attempt_time = ktime_get_real_seconds();
+
     /* proceed with measurement */
```

### Affected Function

`dht_do_measurement()` — called from:
- `export_write()` during sensor registration
- `sensor_measure_write()` for user-triggered measurements
- `dht_poll_thread_fn()` for background auto-poll

### Impact

- Skipped (busy-flag) attempts no longer consume rate-limit budget
- Auto-poll intervals are no longer stretched by concurrent manual reads
- No behavioral change for the normal (non-contended) path

---

## Patch #16 — Unchecked `iterate_dir` return value

### Problem

In `dht_proc_dir_is_empty()`, the return value of `iterate_dir()` was
not checked. If `iterate_dir` returned an error (extremely unlikely
for procfs, but possible on memory pressure or corrupted state),
`dctx.count` remained 0 — and the function reported the directory
as empty. This could cause `dht_driver_exit()` to remove `/proc/sensors`
even when other drivers had entries there.

### Root Cause

```c
/* BUG (v2.8.4): */
    iterate_dir(filp, &dctx.ctx);
    empty = (dctx.count == 0);   /* ← error → count=0 → "empty" → wrong */
```

If `iterate_dir` fails, `count` is never incremented, so `empty = true`.
The caller then removes `/proc/sensors`, destroying other drivers' proc
entries.

### Fix

Check the return value. On error, conservatively treat the directory
as **non-empty** (do not remove it):

```c
/* FIX (v2.8.5): */
    if (iterate_dir(filp, &dctx.ctx) < 0) {
        filp_close(filp, NULL);
        return false;   /* error — treat as non-empty, do not remove */
    }
    empty = (dctx.count == 0);
```

| `iterate_dir` result | Old behavior             | New behavior             |
|-----------------------|--------------------------|--------------------------|
| 0 (success, empty)    | `empty = true`           | `empty = true`           |
| 0 (success, entries)  | `empty = false`          | `empty = false`          |
| < 0 (error)           | `empty = true` (wrong!)  | `return false` (safe)     |

### Diff

```diff
--- dht_v2.8.4.c
+++ dht_v2.8.5.c
@@ -1865,7 +1865,11 @@
-    iterate_dir(filp, &dctx.ctx);
+    if (iterate_dir(filp, &dctx.ctx) < 0) {
+        filp_close(filp, NULL);
+        return false;
+    }
     empty = (dctx.count == 0);
```

### Affected Function

`dht_proc_dir_is_empty()` — called from `dht_driver_exit()` during
module unload to decide whether to remove the `/proc/sensors` parent
directory.

### Impact

- Module unload no longer removes `/proc/sensors` if the directory
  check fails
- Other drivers' proc entries under `/proc/sensors` are protected
- No behavioral change for the normal (success) path

---

## Summary

| Patch | Severity | Lines changed | Architecture change |
|-------|----------|---------------|---------------------|
| #15   | Medium   | ~5 (moved 1 line, added 2 blank lines) | None |
| #16   | Low      | ~4 (added 3 lines, removed 1)         | None |

## Files Changed

| File              | Change                                         |
|-------------------|------------------------------------------------|
| `dht.c`           | 2 patches: moved `last_attempt_time`, added `iterate_dir` check |
| `changelog.txt`    | Added Patch #15, #16 entries (EN)             |
| `changelog_ru.txt` | Added Patch #15, #16 entries (RU)             |
| `README.md`        | Version bump to 2.8.5 (EN)                     |
| `README_RU.md`     | Version bump to 2.8.5 (RU)                     |
