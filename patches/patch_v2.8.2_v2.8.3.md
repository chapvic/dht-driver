# Patch: v2.8.2 → v2.8.3 — Critical & Medium Severity Fixes

**Driver:** DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver  
**Base version:** 2.8.2 (includes all fixes from v2.8 → v2.8.1 → v2.8.2)  
**Target version:** 2.8.3  
**Patches:** 4 (2 critical severity, 1 medium severity, 1 low severity)  

---

## Overview

This patch set addresses four issues found during a full code audit of v2.8.2. Two are critical: a use-after-free in the module exit path and a DHT22 negative temperature calculation bug. One is medium: `dht_read_sensor` re-resolves the GPIO descriptor instead of using the one already stored in the sensor struct. One is cosmetic: negative temperatures print incorrectly in the debug log.

| # | Bug | Severity | Impact |
|---|-----|----------|--------|
| 9 | Use-after-free in `dht_driver_exit` — `proc_remove` on freed `proc_dir` | Critical | UAF / crash with KASAN |
| 10 | DHT22 negative temperature: `c = -c` includes sign bit in magnitude | Critical | Wrong temperature (e.g., -3301.9 C instead of -25.1 C) |
| 11 | `dht_read_sensor` re-resolves GPIO descriptor via `dht_find_desc` | Medium | Redundant lookup / potential descriptor mismatch |
| 12 | Debug log: negative temperature prints as `T=-25.-1` instead of `T=-25.1` | Low | Misleading debug output |

---

## Patch 9/12: Fix use-after-free in `dht_driver_exit` — nullify `proc_dir` before `dht_sensor_put`

### Problem

In `dht_driver_exit`, Phase 1 calls `proc_remove(proc_dir)`, which removes the entire `/proc/sensors/dht/` subtree — including every `gpio<pin>/` subdirectory. Each sensor's `proc_dir_entry` is freed by the kernel's procfs code.

Phase 2 then iterates the sensor list and calls `dht_sensor_put(sensor)`. If no open file holds a reference (the normal case after `proc_remove`), `dht_sensor_put` triggers `dht_sensor_release`, which calls `proc_remove(sensor->proc_dir)` — a **use-after-free** on the already-freed `proc_dir_entry`.

`proc_remove` dereferences `de->name` and `de->parent` of the passed entry. With SLUB this typically "works" because the memory is not yet overwritten, but with `CONFIG_KASAN` or `CONFIG_DEBUG_SLAB` it is detected as a use-after-free bug.

**Before (v2.8.2):**
```c
    list_for_each_entry_safe(sensor, tmp, &tmp_list, list) {
        list_del(&sensor->list);
        dht_sensor_put(sensor);
    }
```

### Fix

Set `sensor->proc_dir = NULL` before calling `dht_sensor_put`. `proc_remove(NULL)` is a safe no-op in the kernel.

**After (v2.8.3):**
```c
    list_for_each_entry_safe(sensor, tmp, &tmp_list, list) {
        list_del(&sensor->list);
        /* proc_remove(proc_dir) in Phase 1 already freed the sensor's
         * procfs entries. Nullify proc_dir so dht_sensor_release does
         * not call proc_remove on the already-freed proc_dir_entry
         * (use-after-free). proc_remove(NULL) is a safe no-op. */
        sensor->proc_dir = NULL;
        dht_sensor_put(sensor);
    }
```

### Why this works

- `proc_remove(proc_dir)` in Phase 1 recursively removes `/proc/sensors/dht/` and all `gpio<pin>/` children. Each sensor's `proc_dir_entry` is freed.
- Setting `sensor->proc_dir = NULL` prevents `dht_sensor_release` from calling `proc_remove` on the dangling pointer.
- `proc_remove(NULL)` is explicitly handled in `fs/proc/generic.c` — it returns immediately without dereferencing.
- `dht_stop_poll`, `gpio_free`, `mutex_destroy`, and `kfree` in `dht_sensor_release` are still called correctly — only the redundant `proc_remove` is skipped.
- This is the same pattern already used in `dht_create_sensor_proc` error path: `sensor->proc_dir = NULL` after `proc_remove(sensor->proc_dir)`.

---

## Patch 10/12: Fix DHT22 negative temperature calculation — mask sign bit before negation

### Problem

For DHT22/AM2302, temperature is transmitted as a 16-bit signed value: `c = (data[2] << 8) | data[3]`. Bit 7 of `data[2]` (i.e., bit 15 of the combined value) is the sign bit.

When the sign bit is set, the current code does:
```c
    if (data[2] & 0x80)
        c = -c;
```

But `c` at this point is `0x8000 | (abs_temp * 10)` — the sign bit is part of the integer. Negating this gives `-(32768 + abs_temp * 10)` instead of `-(abs_temp * 10)`.

**Example:** temperature -25.1 C
- Sensor transmits: `data[2] = 0x80`, `data[3] = 0xFB` (251 = 25.1 x 10)
- `c = (0x80 << 8) | 0xFB = 0x80FB = 33019`
- `c = -c = -33019`
- Output: **T=-3301.9 C** (wrong)
- Expected: **T=-25.1 C**

For DHT11, this bug does not manifest: `data[2]` is in range 0-50, so bit 7 is never set.

### Fix

Mask the sign bit before negating, so only the magnitude is negated:

**Before (v2.8.2):**
```c
            /* Handle negative temperature (bit 7 of data[2] is the sign bit) */
            if (data[2] & 0x80)
                c = -c;
```

**After (v2.8.3):**
```c
            /* Handle negative temperature for DHT22/AM2302.
             * Bit 7 of data[2] (bit 15 of the 16-bit value) is the sign bit.
             * Mask it off before negating so only the magnitude is negated.
             * For DHT11, data[2] is 0-50, so bit 7 is never set. */
            if (data[2] & 0x80)
                c = -(c & 0x7FFF);
```

### Why this works

- `c & 0x7FFF` strips the sign bit, leaving the 15-bit magnitude: `0x00FB = 251`.
- `-(c & 0x7FFF) = -251` → output: **T=-25.1 C** (correct).
- For DHT11: `data[2]` is always 0-50, `data[2] & 0x80` is 0 — the branch is never taken.
- For DHT22 positive temperatures: `data[2] & 0x80` is 0 — the branch is not taken.
- For DHT22 negative temperatures: `c = -(c & 0x7FFF)` gives the correct negative value.

---

## Patch 11/12: Pass `sensor->gpiod` to `dht_read_sensor` instead of re-resolving

### Problem

`dht_read_sensor` accepts a BCM pin number and internally calls `dht_find_desc(pin)` to resolve the GPIO descriptor on every measurement. But the sensor struct already holds a valid, `gpio_request`-ed descriptor in `sensor->gpiod` — resolved once during registration.

Re-resolving on every read is:
- **Redundant** — the descriptor was already found and stored.
- **Potentially unsafe** — if the GPIO subsystem state changes between registration and measurement, `dht_find_desc` could return a different descriptor.
- **Wasteful** — `dht_find_desc` iterates GPIO chips to find the matching descriptor.

### Fix

Change `dht_read_sensor` to accept a `struct gpio_desc *` parameter. The caller passes `sensor->gpiod`. The `pin` parameter is kept for diagnostic logging only.

### 11a. Change function signature and remove `dht_find_desc` call

**Before (v2.8.2):**
```c
static int dht_read_sensor(int pin, int *hum, int *temp, int *type)
{
    struct gpio_desc *desc;
    ...
    desc = dht_find_desc(pin);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return ERR_GPIO_REQUEST;
    }
```

**After (v2.8.3):**
```c
static int dht_read_sensor(int pin, struct gpio_desc *desc, int *hum, int *temp, int *type)
{
    ...
    /* The GPIO descriptor is passed from the caller (sensor->gpiod),
     * which was resolved and gpio_request-ed during registration.
     * This avoids a redundant dht_find_desc() call on every measurement
     * and ensures we always use the same descriptor. */
    if (!desc) {
        pin_err(pin, "GPIO descriptor is NULL\n");
        return ERR_GPIO_REQUEST;
    }
```

### 11b. Update the call site in `dht_do_measurement`

**Before (v2.8.2):**
```c
        ret = dht_read_sensor(sensor->pin, &hum, &temp, &type);
```

**After (v2.8.3):**
```c
        ret = dht_read_sensor(sensor->pin, sensor->gpiod, &hum, &temp, &type);
```

### Why this works

- `sensor->gpiod` is set during `dht_do_register` and is valid for the sensor's lifetime (until `dht_sensor_release` calls `gpio_free`).
- The descriptor is `gpio_request`-ed, so it cannot change or be claimed by another driver while the sensor is registered.
- `dht_find_desc` is still used during registration — it is only removed from the per-measurement path.
- The `pin` parameter is retained for `pin_err`/`pin_dbg` diagnostic messages.

---

## Patch 12/12: Fix negative temperature formatting in debug log

### Problem

In `dht_do_measurement`, the success debug log formats temperature as:
```c
    pin_dbg(sensor->pin, "measurement OK - H=%d.%d%% T=%d.%d C\n",
            hum / 10, hum % 10, temp / 10, temp % 10);
```

For negative temperatures (e.g., `temp = -251`):
- `temp / 10 = -25`
- `temp % 10 = -1` (C's `%` preserves the sign of the dividend)
- Output: **`T=-25.-1 C`** instead of **`T=-25.1 C`**

Note: `sensor_value_read` (the user-facing `value` proc entry) already handles this correctly with `(-temp) / 10` and `(-temp) % 10`. The bug is only in the debug log.

### Fix

Branch on `temp < 0` and use the absolute value for the fractional part:

**Before (v2.8.2):**
```c
        pin_dbg(sensor->pin, "measurement OK - H=%d.%d%% T=%d.%d C\n",
                hum / 10, hum % 10, temp / 10, temp % 10);
```

**After (v2.8.3):**
```c
        if (temp < 0)
            pin_dbg(sensor->pin, "measurement OK - H=%d.%d%% T=-%d.%d C\n",
                    hum / 10, hum % 10, (-temp) / 10, (-temp) % 10);
        else
            pin_dbg(sensor->pin, "measurement OK - H=%d.%d%% T=%d.%d C\n",
                    hum / 10, hum % 10, temp / 10, temp % 10);
```

### Why this works

- `(-temp) / 10` and `(-temp) % 10` give the absolute magnitude of each digit.
- The minus sign is printed explicitly in the format string: `T=-%d.%d`.
- For positive temperatures, the original format is unchanged.
- This matches the formatting already used in `sensor_value_read` for the user-facing `value` entry.

---

## Testing Recommendations

After applying all four patches:

1. **Module unload with sensors** — register one or more sensors, then `rmmod dht`. Verify no KASAN report in `dmesg` and clean "driver unloaded" message.
2. **DHT22 sub-zero temperature** — if you have a DHT22/AM2302 in a sub-zero environment, verify the `value` entry and debug log show correct negative temperatures (e.g., `T=-25.1` not `T=-3301.9`).
3. **Debug log with negative temperature** — enable debug (`echo 1 > /proc/sensors/dht/debug`), trigger a measurement in sub-zero conditions, check `dmesg` for `T=-25.1` not `T=-25.-1`.
4. **GPIO descriptor stability** — register a sensor, verify in `dmesg` that no `dht_find_desc` lookup happens during measurements (no "GPIO descriptor not found" errors during normal operation).
