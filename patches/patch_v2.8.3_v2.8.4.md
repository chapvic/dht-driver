# Patch: v2.8.3 → v2.8.4

**Date:** 2026-09-21  
**File:** `dht.c`  
**Patch:** #13 — Inverted `measuring` flag in `dht_do_measurement`

---

## Problem

Every second measurement cycle was silently skipped, reducing the effective
auto-poll rate by ~50%. The dmesg log showed an alternating pattern:

```
[dht_gpio_23]: measurement OK - H=54.6% T=24.3 C
[dht_gpio_23]: auto-poll skipped - measurement in progress
[dht_gpio_23]: measurement OK - H=54.5% T=24.3 C
[dht_gpio_23]: auto-poll skipped - measurement in progress
```

## Root Cause

In `dht_do_measurement()`, the atomic flag check used a negated
`atomic_cmpxchg`:

```c
/* BUG (v2.8.3): */
if (!atomic_cmpxchg(&sensor->measuring, 0, 1)) {
    /* "Another measurement is already in progress" */
    ...
    return;
}
/* proceed with measurement */
```

`atomic_cmpxchg(&v, old, new)` returns the **previous** value of `v`:

| `measuring` before | `cmpxchg` returns | `!result` | Branch taken         | Correct? |
|--------------------|--------------------|-----------|----------------------|----------|
| 0 (free)           | 0                  | true      | skip (wrong!)        | No       |
| 1 (busy)           | 1                  | false     | measure (wrong!)     | No       |

The logic was **inverted**: a free flag was treated as busy (skip), and a
busy flag was treated as free (proceed). After each successful measurement,
the flag was reset to 0 — so the next cycle found it free, skipped, but
`cmpxchg` had set it to 1. The cycle after that found it at 1, proceeded
to measure, and reset it to 0. This produced the alternating pattern.

## Fix

Remove the negation — `atomic_cmpxchg` returning a non-zero value means
the flag was already set (busy), so we skip:

```c
/* FIX (v2.8.4): */
if (atomic_cmpxchg(&sensor->measuring, 0, 1)) {
    /* Another measurement is already in progress */
    ...
    return;
}
/* flag was 0 (free), now set to 1 — proceed with measurement */
```

| `measuring` before | `cmpxchg` returns | Branch taken    | Correct? |
|--------------------|--------------------|-----------------|----------|
| 0 (free)           | 0 (false)         | measure          | Yes      |
| 1 (busy)           | 1 (true)          | skip             | Yes      |

## Diff

```diff
--- dht_v2.8.3.c
+++ dht_v2.8.4.c
@@ -1130,7 +1130,7 @@
     /* Atomically claim the measuring flag to prevent parallel bit-bang
      * on the same GPIO line (e.g., manual measure vs auto-poll). */
-    if (!atomic_cmpxchg(&sensor->measuring, 0, 1)) {
+    if (atomic_cmpxchg(&sensor->measuring, 0, 1)) {
         /* Another measurement is already in progress */
         if (manual) {
```

## Affected Function

`dht_do_measurement()` — the central measurement dispatcher called from:
- `export_write()` during sensor registration
- `sensor_measure_write()` for user-triggered measurements
- `dht_poll_thread_fn()` for background auto-poll

## Impact

- Auto-poll now runs at the full configured interval (no 50% waste)
- Manual measurements no longer risk running in parallel with auto-poll
- Registration-time initial measurement works correctly on first attempt
- No behavioral change for error handling or rate limiting

## Introduced In

v2.8.2, Patch #6 (measuring flag added to prevent parallel bit-bang)

## Files Changed

| File        | Change                              |
|-------------|-------------------------------------|
| `dht.c`     | 1 line: `!atomic_cmpxchg` → `atomic_cmpxchg` |
| `changelog.txt`    | Added Patch #13 entry (EN)    |
| `changelog_ru.txt` | Added Patch #13 entry (RU)    |
| `README.md`        | Version bump to 2.8.4 (EN)    |
| `README_RU.md`     | Version bump to 2.8.4 (RU)    |
