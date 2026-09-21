# Patch: v2.8.1 → v2.8.2 — High & Medium Severity Fixes

**Driver:** DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver  
**Base version:** 2.8.1 (includes all critical fixes from v2.8 → v2.8.1)  
**Target version:** 2.8.2  
**Patches:** 4 (2 high severity, 2 medium severity)  

---

## Overview

This patch set addresses four additional issues found in the DHT driver that were not critical enough to block v2.8.1, but should be fixed for production reliability. These cover race conditions, inconsistent user-facing behavior, and a sensor type detection edge case.

| # | Bug | Severity | Impact |
|---|-----|----------|--------|
| 5 | `dht_start_poll`/`dht_stop_poll` called without `list_lock` | High | Race condition / leaked kernel thread |
| 6 | Manual measurement races with auto-poll on same GPIO | High | Corrupted reads from concurrent bit-bang |
| 7 | `auto_interval_write` silently disables on invalid input | Medium | Confusing user experience |
| 8 | DHT11/DHT22 heuristic fails at low humidity | Medium | Wrong sensor type / incorrect values |

---

## Patch 5/8: Protect `dht_start_poll`/`dht_stop_poll` with `list_lock` in `sensor_interval_write`

### Problem

In `sensor_interval_write()`, the poll thread is started or stopped **without holding `list_lock`**:

```c
    /* Store the new interval under the sensor lock */
    mutex_lock(&sensor->lock);
    sensor->interval = val;
    mutex_unlock(&sensor->lock);

    /* If global auto mode is active, the local interval is stored but
     * the global setting controls polling. No thread changes needed. */
    if (READ_ONCE(global_auto_interval) != -1) {
        pin_dbg(sensor->pin, "local interval ignored - global auto active\n");
        return count;
    }

    /* Global auto mode is off — the per-sensor setting controls polling */
    if (val == -1) {
        /* Disable polling: stop the thread if running */
        dht_stop_poll(sensor);
        pin_log(sensor->pin, "auto-poll disabled\n");
    } else {
        /* Enable polling: start the thread with the new interval */
        dht_start_poll(sensor);
        pin_log(sensor->pin, "auto-poll enabled (interval=%d)\n", val);
    }
    return count;
```

Meanwhile, `auto_interval_write()` and `dht_do_register()` call `dht_start_poll` **under `list_lock`**. `dht_start_poll()` checks `if (sensor->poll_thread) return;` non-atomically — two concurrent callers can both see `NULL` and both call `kthread_run()`. One thread becomes orphaned: its `task_struct` is never stopped, causing a **memory leak** and a zombie kernel thread.

Similarly, two concurrent `dht_stop_poll()` calls can both pass the `if (!sensor->poll_thread) return;` check, both save the same pointer, and both call `kthread_stop()` — the second call is a **use-after-free** on the already-freed `task_struct`.

### Fix

Wrap the `dht_start_poll`/`dht_stop_poll` calls in `sensor_interval_write()` with `list_lock`:

**Before (v2.8.1):**
```c
    /* Validate: -1 (disabled) or within [MIN_INTERVAL, MAX_INTERVAL] */
    if (val != -1 && (val < MIN_INTERVAL || val > MAX_INTERVAL))
        return -EINVAL;

    /* Store the new interval under the sensor lock */
    mutex_lock(&sensor->lock);
    sensor->interval = val;
    mutex_unlock(&sensor->lock);

    /* If global auto mode is active, the local interval is stored but
     * the global setting controls polling. No thread changes needed. */
    if (READ_ONCE(global_auto_interval) != -1) {
        pin_dbg(sensor->pin, "local interval ignored - global auto active\n");
        return count;
    }

    /* Global auto mode is off — the per-sensor setting controls polling */
    if (val == -1) {
        /* Disable polling: stop the thread if running */
        dht_stop_poll(sensor);
        pin_log(sensor->pin, "auto-poll disabled\n");
    } else {
        /* Enable polling: start the thread with the new interval */
        dht_start_poll(sensor);
        pin_log(sensor->pin, "auto-poll enabled (interval=%d)\n", val);
    }
    return count;
```

**After (v2.8.2):**
```c
    /* Validate: -1 (disabled) or within [MIN_INTERVAL, MAX_INTERVAL] */
    if (val != -1 && (val < MIN_INTERVAL || val > MAX_INTERVAL))
        return -EINVAL;

    /* Store the new interval under the sensor lock */
    mutex_lock(&sensor->lock);
    sensor->interval = val;
    mutex_unlock(&sensor->lock);

    /* Acquire list_lock to serialize poll thread start/stop with
     * auto_interval_write() and dht_do_register(), which also call
     * dht_start_poll/dht_stop_poll under this lock. Without this,
     * two concurrent callers could both create (or both stop) a
     * poll thread, causing a leaked task_struct or use-after-free. */
    mutex_lock(&list_lock);

    /* If global auto mode is active, the local interval is stored but
     * the global setting controls polling. No thread changes needed. */
    if (READ_ONCE(global_auto_interval) != -1) {
        pin_dbg(sensor->pin, "local interval ignored - global auto active\n");
        mutex_unlock(&list_lock);
        return count;
    }

    /* Global auto mode is off — the per-sensor setting controls polling */
    if (val == -1) {
        /* Disable polling: stop the thread if running */
        dht_stop_poll(sensor);
        pin_log(sensor->pin, "auto-poll disabled\n");
    } else {
        /* Enable polling: start the thread with the new interval */
        dht_start_poll(sensor);
        pin_log(sensor->pin, "auto-poll enabled (interval=%d)\n", val);
    }
    mutex_unlock(&list_lock);
    return count;
```

### Why this works

`list_lock` is the same mutex already used by `auto_interval_write()` and `dht_do_register()` to protect poll thread lifecycle operations. By acquiring it in `sensor_interval_write()` as well, all three call sites are serialized, eliminating the race condition. The `sensor->lock` is still used for protecting sensor fields — it's released before acquiring `list_lock` to avoid a nested lock ordering issue (list_lock is always acquired before sensor->lock in other paths).

---

## Patch 6/8: Add `measuring` atomic flag to prevent concurrent GPIO reads

### Problem

`sensor_measure_write()` checks if auto-poll is active, releases `sensor->lock`, and then calls `dht_do_measurement()`. In the window between the check and the start of the GPIO read, a user can enable auto-poll via `auto_interval_write()` — the poll thread starts and also begins reading the same GPIO. Two simultaneous bit-bang operations on a single wire **guarantee corrupted data** for both readers.

Additionally, two concurrent manual `echo 1 > measure` writes can both pass the auto-poll check and both call `dht_do_measurement()` simultaneously.

### Fix

Add an `atomic_t measuring` field to `struct dht_sensor`. Before any GPIO read, atomically claim the flag with `atomic_cmpxchg(&sensor->measuring, 0, 1)`. If the flag is already `1`, the second reader skips the measurement.

### 6a. Add `measuring` field to `struct dht_sensor`

**Before (v2.8.1):**
```c
struct dht_sensor {
    int pin;
    int interval;
    int humidity_raw;
    int temperature_raw;
    int status_code;
    char status_text[STATUS_BUF_LEN];
    int sensor_type;
    time64_t register_time;
    time64_t last_meas_time;
    time64_t last_attempt_time;
    struct mutex lock;
    struct proc_dir_entry *proc_dir;
    struct task_struct *poll_thread;
    struct gpio_desc *gpiod;
    struct kref refcount;
    struct list_head list;
};
```

**After (v2.8.2):**
```c
struct dht_sensor {
    int pin;
    int interval;
    int humidity_raw;
    int temperature_raw;
    int status_code;
    char status_text[STATUS_BUF_LEN];
    int sensor_type;
    time64_t register_time;
    time64_t last_meas_time;
    time64_t last_attempt_time;
    struct mutex lock;
    struct proc_dir_entry *proc_dir;
    struct task_struct *poll_thread;
    struct gpio_desc *gpiod;
    atomic_t measuring;               /* Prevents concurrent GPIO reads (cmpxchg guard) */
    struct kref refcount;
    struct list_head list;
};
```

### 6b. Initialize the flag in `dht_do_register()`

**After (v2.8.2), add after `kref_init`:**

```c
    kref_init(&sensor->refcount);
    atomic_set(&sensor->measuring, 0);  /* No measurement in progress */
```

### 6c. Guard the GPIO read in `dht_do_measurement()`

**Before (v2.8.1):**
```c
    /* Record the attempt timestamp for rate limiting */
    sensor->last_attempt_time = ktime_get_real_seconds();

    /* Release the lock during the actual sensor read to avoid blocking.
     * The read takes 20+ ms, which is too long to hold a mutex. */
    mutex_unlock(&sensor->lock);

    /* Perform the actual GPIO read (may take multiple retries) */
    ret = dht_read_sensor(sensor->pin, &hum, &temp, &type);

    /* Re-acquire the lock to store the results */
    mutex_lock(&sensor->lock);
```

**After (v2.8.2):**
```c
    /* Record the attempt timestamp for rate limiting */
    sensor->last_attempt_time = ktime_get_real_seconds();

    /* Release the lock during the actual sensor read to avoid blocking.
     * The read takes 20+ ms, which is too long to hold a mutex. */
    mutex_unlock(&sensor->lock);

    /* Atomically claim the measuring flag to prevent concurrent
     * GPIO reads. Two simultaneous bit-bang operations on the same
     * single-wire line would corrupt data for both readers.
     * This can happen when:
     *   - A manual measurement races with auto-poll being enabled
     *   - Two users simultaneously write to the measure entry
     * If already in progress, skip this measurement cycle. */
    if (!atomic_cmpxchg(&sensor->measuring, 0, 1)) {
        /* Perform the actual GPIO read (may take multiple retries) */
        ret = dht_read_sensor(sensor->pin, &hum, &temp, &type);
        atomic_set(&sensor->measuring, 0);  /* Release the flag */
    } else {
        /* Another reader is in progress — skip this cycle */
        if (manual) {
            /* For manual reads, report the conflict to the user */
            mutex_lock(&sensor->lock);
            sensor->status_code = ERR_READ_FAILED;
            snprintf(sensor->status_text, STATUS_BUF_LEN,
                     "Measurement already in progress");
            mutex_unlock(&sensor->lock);
        }
        return;
    }

    /* Re-acquire the lock to store the results */
    mutex_lock(&sensor->lock);
```

### Why this works

- `atomic_cmpxchg(&sensor->measuring, 0, 1)` atomically checks if the flag is `0` and sets it to `1` in a single operation. If it was already `1`, it returns the old value (`1`), and the caller knows another reader is active.
- The flag is cleared with `atomic_set(&sensor->measuring, 0)` after the read completes.
- Manual measurements that are blocked report an error to the user; auto-poll cycles simply skip.
- This works **without holding any mutex** during the GPIO read, preserving the existing lock-free bit-bang timing.

---

## Patch 7/8: Return `-EINVAL` for invalid `auto_interval_write` values

### Problem

In `auto_interval_write()`, any value outside the valid range silently disables auto-poll and returns success:

```c
    /* Set the global interval or disable it if the value is out of range */
    if (val >= MIN_INTERVAL && val <= MAX_INTERVAL) {
        WRITE_ONCE(global_auto_interval, val);
    } else {
        WRITE_ONCE(global_auto_interval, -1);
    }
    ...
    return count;  /* always "success" */
```

Writing `0` or `100` returns success but silently turns off auto-poll. This is inconsistent with `sensor_interval_write()`, which returns `-EINVAL` for out-of-range values. The behavior confuses users and scripts that check the return code.

### Fix

Return `-EINVAL` for values outside `[MIN_INTERVAL, MAX_INTERVAL]`, except `-1` (explicit disable):

**Before (v2.8.1):**
```c
static ssize_t auto_interval_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret)
        return ret;

    /* Acquire the list lock to safely modify global state and sensor list */
    mutex_lock(&list_lock);

    /* Set the global interval or disable it if the value is out of range */
    if (val >= MIN_INTERVAL && val <= MAX_INTERVAL) {
        WRITE_ONCE(global_auto_interval, val);
    } else {
        WRITE_ONCE(global_auto_interval, -1);
    }

    /* If global auto mode is now active, start poll threads for all sensors
     * that don't have one running yet. */
    if (READ_ONCE(global_auto_interval) != -1) {
        struct dht_sensor *sensor;
        list_for_each_entry(sensor, &sensor_list, list) {
            if (!sensor->poll_thread)
                dht_start_poll(sensor);
        }
    }

    mutex_unlock(&list_lock);

    dht_info("auto_interval set to %d\n", READ_ONCE(global_auto_interval));
    return count;
}
```

**After (v2.8.2):**
```c
static ssize_t auto_interval_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret)
        return ret;

    /* Validate: -1 (disabled) or within [MIN_INTERVAL, MAX_INTERVAL].
     * Unlike the old behavior, invalid values now return -EINVAL
     * instead of silently disabling auto-poll. */
    if (val != -1 && (val < MIN_INTERVAL || val > MAX_INTERVAL))
        return -EINVAL;

    /* Acquire the list lock to safely modify global state and sensor list */
    mutex_lock(&list_lock);

    /* Set the global interval */
    WRITE_ONCE(global_auto_interval, val);

    /* If global auto mode is now active, start poll threads for all sensors
     * that don't have one running yet. */
    if (val != -1) {
        struct dht_sensor *sensor;
        list_for_each_entry(sensor, &sensor_list, list) {
            if (!sensor->poll_thread)
                dht_start_poll(sensor);
        }
    }

    mutex_unlock(&list_lock);

    dht_info("auto_interval set to %d\n", READ_ONCE(global_auto_interval));
    return count;
}
```

### Why this works

- `-1` is the only valid "disable" sentinel — it's handled explicitly.
- Values `0`, `1`, `61`, `100`, etc. now return `-EINVAL` to the user, matching `sensor_interval_write()`'s behavior.
- The valid range check happens **before** acquiring `list_lock`, so no lock is taken on invalid input.
- The `READ_ONCE(global_auto_interval) != -1` check is replaced with the simpler `val != -1` since we just wrote `val`.

---

## Patch 8/8: Improve DHT11/DHT22 sensor type detection at low humidity

### Problem

The sensor type heuristic in `dht_read_sensor()` uses a single threshold:

```c
            if (h > 1000) {
                if (type) *type = SENSOR_TYPE_DHT11;
                h = data[0] * 10;
                c = data[2] * 10;
            } else {
                if (type) *type = SENSOR_TYPE_DHT22;
                /* DHT22: the 16-bit values are already scaled x10 */
            }
```

Where `h = ((data[0] << 8) + data[1])` is the combined 16-bit humidity value.

For a **DHT11** with humidity ≤ 3% (e.g., `data[0] = 3, data[1] = 0`): `h = 3 * 256 + 0 = 768 < 1000`. The sensor is misidentified as a **DHT22**, and the values are used without the `* 10` scaling. The user sees `76.8%` instead of `30%` (if `data[0] = 30`) or `7.6%` instead of `3.0%`.

While the DHT11 datasheet specifies 20–90% operating range, at cold start or in fault conditions it can output any value with a valid checksum. The driver should handle this gracefully.

### Fix

Add a secondary check: if `h <= 1000` but `data[1] == 0 && data[3] == 0` (decimal bytes are zero, which is the DHT11 signature) and the integer bytes are within DHT11 ranges, classify as DHT11:

**Before (v2.8.1):**
```c
            /* Determine sensor type based on the data format:
             * DHT11 sends integer values in byte 0 and 2, with byte 1 and 3 = 0.
             * When the combined 16-bit humidity > 1000, it indicates the raw
             * byte 0 value is > 100 (e.g., 45*256 + 0 = 11520 > 1000),
             * which means it's a DHT11 with integer-only format.
             *
             * The humidity check is authoritative: once the type is determined
             * from humidity, the same type is used for temperature interpretation.
             * This avoids a bug where DHT11 temperatures below 5 C were
             * misinterpreted as DHT22 values (c <= 1250, no conversion applied). */
            if (h > 1000) {
                if (type) *type = SENSOR_TYPE_DHT11;
                /* DHT11: use only the integer bytes, scale by 10 for consistent units */
                h = data[0] * 10;
                c = data[2] * 10;
            } else {
                if (type) *type = SENSOR_TYPE_DHT22;
                /* DHT22: the 16-bit values are already scaled x10 */
            }
```

**After (v2.8.2):**
```c
            /* Determine sensor type based on the data format:
             * DHT11 sends integer values in byte 0 and 2, with byte 1 and 3 = 0.
             * When the combined 16-bit humidity > 1000, it indicates the raw
             * byte 0 value is > 100 (e.g., 45*256 + 0 = 11520 > 1000),
             * which means it's a DHT11 with integer-only format.
             *
             * The humidity check is authoritative: once the type is determined
             * from humidity, the same type is used for temperature interpretation.
             * This avoids a bug where DHT11 temperatures below 5 C were
             * misinterpreted as DHT22 values (c <= 1250, no conversion applied).
             *
             * Additional check: if h <= 1000 but the decimal bytes (data[1]
             * and data[3]) are both zero and the integer bytes are within
             * DHT11 ranges, classify as DHT11. This handles the edge case
             * of a DHT11 reporting very low humidity (data[0] <= 3) at cold
             * start or fault conditions, where h = data[0]*256 < 1000. */
            if (h > 1000) {
                if (type) *type = SENSOR_TYPE_DHT11;
                /* DHT11: use only the integer bytes, scale by 10 for consistent units */
                h = data[0] * 10;
                c = data[2] * 10;
            } else if (data[1] == 0 && data[3] == 0 &&
                       data[0] <= 100 && data[2] <= 50) {
                /* Looks like a DHT11 with low humidity: decimal bytes are
                 * zero, integer bytes are within DHT11 ranges (humidity
                 * 0-100%, temperature 0-50 C). Scale by 10. */
                if (type) *type = SENSOR_TYPE_DHT11;
                h = data[0] * 10;
                c = data[2] * 10;
            } else {
                if (type) *type = SENSOR_TYPE_DHT22;
                /* DHT22: the 16-bit values are already scaled x10 */
            }
```

### Why this works

- **DHT11 signature:** The DHT11 always sends `data[1] = 0` and `data[3] = 0` (no decimal parts). A DHT22 with humidity ≤ 3% would typically have non-zero decimal bytes (e.g., `data[1] = 5` for 3.5%).
- **Range validation:** `data[0] <= 100` (humidity 0–100%) and `data[2] <= 50` (temperature 0–50°C) match the DHT11's operating range. A DHT22 could report humidity below 3% but would likely have a non-zero `data[1]`.
- **No false positives:** A real DHT22 with `data[1] != 0` or `data[3] != 0` still falls through to the DHT22 branch.
- **The original `h > 1000` check remains as the primary path** — the new branch only handles the narrow edge case where `h <= 1000` but the data format is clearly DHT11.

---

## Testing Recommendations

After applying all four patches:

1. **Concurrent interval writes** — open two terminals, rapidly write to `sensor_interval` and `auto_interval` simultaneously. Check `dmesg` for no duplicate thread creation or crash.
2. **Concurrent measurements** — start auto-poll, then immediately write to `measure`. The manual read should get `"Measurement already in progress"`, not corrupted data.
3. **Invalid auto_interval values** — `echo 0 > auto_interval` should return an error (`echo $?` shows non-zero). `echo -1 > auto_interval` should succeed.
4. **DHT11 at low humidity** — if you have a DHT11 in a dry environment (< 3%), verify the `value` entry shows the correct humidity without `x256` scaling.
