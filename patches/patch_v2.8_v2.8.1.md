# Patch: v2.8 → v2.8.1 — Critical Bug Fixes

**Driver:** DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver  
**Base version:** 2.8  
**Target version:** 2.8.1  
**Patches:** 4 (all critical severity)  

---

## Overview

This patch set addresses four critical bugs found in version 2.8 of the DHT driver. Each bug can cause data corruption, use-after-free, or kernel crashes. All fixes are minimal and surgical — they change only the buggy code paths without altering the driver's architecture.

| # | Bug | Severity | Impact |
|---|-----|----------|--------|
| 1 | `pulse_ns` uninitialized — garbage in bit decoding | Critical | Corrupted sensor data |
| 2 | Double `proc_remove` in `dht_create_sensor_proc` | Critical | Use-after-free / kernel crash |
| 3 | Poll thread self-exit via `break` — dangling `poll_thread` | Critical | Use-after-free / kernel crash |
| 4 | GPIO not requested via `gpio_request` | High | `-EACCES` on modern kernels; no exclusive GPIO access |

---

## Patch 1/4: Initialize `pulse_ns` in `dht_read_sensor`

### Problem

In `dht_read_sensor()`, the variable `pulse_ns` is declared without initialization:

```c
u64 pulse_start, pulse_ns;       /* Nanosecond timestamps for pulse width measurement */
```

In the bit-bang loop:

```c
preempt_disable();
for (i = 0; i < MAX_TIMINGS && j < 40; i++) {
    /* Record the start time of the current pulse level */
    pulse_start = ktime_get_ns();

    /* Wait until the line changes state or times out */
    while (gpiod_get_value(desc) == last_state) {
        pulse_ns = ktime_get_ns() - pulse_start;
        if (pulse_ns > PULSE_TIMEOUT_NS)
            break;
    }
    if (pulse_ns > PULSE_TIMEOUT_NS)
        break;
```

If the GPIO line has **already changed state** before entering the `while` loop (the body never executes), `pulse_ns` retains its value from the **previous iteration** — or, on the very first iteration, contains **uninitialized stack garbage**.

This garbage value is then used for:
1. The timeout check: `if (pulse_ns > PULSE_TIMEOUT_NS) break;` — may prematurely abort the read.
2. Bit decoding: `if (pulse_ns > BIT_THRESHOLD) data[j / 8] |= 1;` — may set random bits.

A corrupted 40-bit data word can still pass the 8-bit checksum by chance (1 in 256 probability), producing silently wrong temperature/humidity values.

### Fix

Initialize `pulse_ns = 0` at the top of each `for` iteration, before `pulse_start`:

**Before (v2.8):**
```c
        preempt_disable();
        for (i = 0; i < MAX_TIMINGS && j < 40; i++) {
            /* Record the start time of the current pulse level */
            pulse_start = ktime_get_ns();

            /* Wait until the line changes state or times out */
            while (gpiod_get_value(desc) == last_state) {
                pulse_ns = ktime_get_ns() - pulse_start;
                if (pulse_ns > PULSE_TIMEOUT_NS)
                    break;
            }
            if (pulse_ns > PULSE_TIMEOUT_NS)
                break;
```

**After (v2.8.1):**
```c
        preempt_disable();
        for (i = 0; i < MAX_TIMINGS && j < 40; i++) {
            /* Initialize pulse_ns to 0 so that if the while loop body
             * never executes (line already changed state), we don't
             * use a stale/garbage value for bit decoding or timeout check. */
            pulse_ns = 0;

            /* Record the start time of the current pulse level */
            pulse_start = ktime_get_ns();

            /* Wait until the line changes state or times out */
            while (gpiod_get_value(desc) == last_state) {
                pulse_ns = ktime_get_ns() - pulse_start;
                if (pulse_ns > PULSE_TIMEOUT_NS)
                    break;
            }
            if (pulse_ns > PULSE_TIMEOUT_NS)
                break;
```

### Why this works

When the `while` loop body never executes, `pulse_ns` is now `0` instead of garbage. A value of `0` means:
- It will **not** trigger the timeout check (`0 > PULSE_TIMEOUT_NS` is false).
- It will **not** set a `1` bit in decoding (`0 > BIT_THRESHOLD` is false, so the bit remains `0`).

This is the correct behavior for a pulse of negligible width — it's a `0` bit with no timeout.

---

## Patch 2/4: Null `proc_dir` after `proc_remove` in `dht_create_sensor_proc`

### Problem

In `dht_create_sensor_proc()`, when a proc entry creation fails, the function calls `proc_remove(sensor->proc_dir)` to clean up, but does **not** null the pointer:

```c
    for (i = 0; i < ARRAY_SIZE(sensor_proc_entries); i++) {
        const struct proc_entry_def *e = &sensor_proc_entries[i];
        if (!proc_create_data(e->name, e->mode, sensor->proc_dir, e->fops, sensor)) {
            /* If any entry fails, clean up the entire directory */
            proc_remove(sensor->proc_dir);
            return -ENOMEM;
        }
    }
```

The caller `dht_do_register()` then calls:

```c
    if (dht_create_sensor_proc(sensor)) {
        dht_sensor_put(sensor);   /* → kref reaches 0 → dht_sensor_release() */
        return -ENOMEM;
    }
```

`dht_sensor_release()` calls:

```c
    proc_remove(sensor->proc_dir);   /* ← dangling pointer! */
```

This is a **use-after-free**: the `proc_dir_entry` was already freed by the first `proc_remove`, the pointer is dangling, and the second `proc_remove` operates on freed memory. This can corrupt the kernel heap or cause an immediate crash.

### Fix

Set `sensor->proc_dir = NULL` after `proc_remove` in the error path of `dht_create_sensor_proc()`:

**Before (v2.8):**
```c
        if (!proc_create_data(e->name, e->mode, sensor->proc_dir, e->fops, sensor)) {
            /* If any entry fails, clean up the entire directory */
            proc_remove(sensor->proc_dir);
            return -ENOMEM;
        }
```

**After (v2.8.1):**
```c
        if (!proc_create_data(e->name, e->mode, sensor->proc_dir, e->fops, sensor)) {
            /* If any entry fails, clean up the entire directory.
             * Must NULL the pointer so dht_sensor_release() won't
             * call proc_remove() again on the already-freed entry. */
            proc_remove(sensor->proc_dir);
            sensor->proc_dir = NULL;
            return -ENOMEM;
        }
```

### Why this works

`proc_remove(NULL)` is a safe no-op in the Linux kernel — it checks for NULL before proceeding. By nulling the pointer, the subsequent call from `dht_sensor_release()` simply does nothing, avoiding the double-free.

---

## Patch 3/4: Prevent poll thread self-exit via `break`

### Problem

In `dht_poll_thread_fn()`, when both global and per-sensor intervals are disabled (`-1`), the thread exits via `break`:

```c
        if (effective_interval == -1) {
            pin_dbg(sensor->pin, "both intervals disabled, poll thread exiting\n");
            break;
        }
```

When a kernel thread exits, its `task_struct` is freed by the kernel. However, `sensor->poll_thread` **still holds a pointer** to this freed `task_struct`.

When `dht_stop_poll()` is later called (during unexport or module unload):

```c
    thread = sensor->poll_thread;     /* ← dangling pointer */
    sensor->poll_thread = NULL;
    kthread_stop(thread);             /* ← use-after-free! */
```

`kthread_stop()` calls `get_task_struct(thread)` on the already-freed `task_struct` — this is a **use-after-free** that can cause a kernel crash.

### Fix

Replace the `break` with an idle loop that:
1. Sleeps in 1-second increments.
2. Checks `kthread_should_stop()` — exits only when asked by `kthread_stop()`.
3. Re-checks intervals — automatically resumes polling if the user re-enables auto-poll.

**Before (v2.8):**
```c
        /* If both global and per-sensor intervals are disabled (-1),
         * the thread should stop — there is nothing to poll for.
         * This can happen when auto_interval is set to -1 while the
         * thread was started by a previous non-zero global setting. */
        if (effective_interval == -1) {
            pin_dbg(sensor->pin, "both intervals disabled, poll thread exiting\n");
            break;
        }
```

**After (v2.8.1):**
```c
        /* If both global and per-sensor intervals are disabled (-1),
         * the thread should stop — there is nothing to poll for.
         * This can happen when auto_interval is set to -1 while the
         * thread was started by a previous non-zero global setting.
         *
         * IMPORTANT: do NOT break out of the main loop here! If the
         * thread exits on its own, task_struct is freed by the kernel
         * but sensor->poll_thread still holds a dangling pointer.
         * A later dht_stop_poll() -> kthread_stop() would be a
         * use-after-free. Instead, sleep in a loop until
         * kthread_should_stop() or until an interval is re-enabled. */
        if (effective_interval == -1) {
            pin_dbg(sensor->pin, "both intervals disabled, poll thread idle\n");
            /* Sleep in 1-second increments until asked to stop
             * or an interval gets re-enabled by the user. */
            while (!kthread_should_stop()) {
                /* Re-check intervals: user may re-enable auto-poll */
                mutex_lock(&sensor->lock);
                if (READ_ONCE(global_auto_interval) != -1 ||
                    sensor->interval != -1) {
                    mutex_unlock(&sensor->lock);
                    break;  /* exit idle loop, resume polling */
                }
                mutex_unlock(&sensor->lock);
                ssleep(1);
            }
            continue;  /* re-enter main loop, re-check kthread_should_stop */
        }
```

### Why this works

- The thread **never exits on its own** — it only stops when `kthread_stop()` is called from `dht_stop_poll()`.
- `sensor->poll_thread` always points to a **live** `task_struct`.
- The idle loop re-checks intervals every second, so if the user writes a new value to `auto_interval` or `sensor_interval`, the thread automatically resumes polling without needing to be restarted.
- `kthread_should_stop()` is checked in the idle loop, so `kthread_stop()` still works promptly.

---

## Patch 4/4: Request GPIO via `gpio_request` / `gpio_free`

### Problem

The driver uses `gpiod_direction_output`, `gpiod_direction_input`, `gpiod_get_value`, `gpiod_set_value` — but **never calls `gpiod_request`** (or the legacy `gpio_request`) to claim the GPIO line.

On kernels with enforced request-before-use (e.g., `CONFIG_GPIO_CDEV` and strict gpiolib checks, common on kernel 5.x+), these calls may return `-EACCES` or `-EINVAL`. Additionally, another driver could simultaneously use the same pin without any conflict detection.

> **Note:** `gpiod_request` and `gpiod_free` are internal gpiolib functions **not exported to modules**. The legacy `gpio_request()` / `gpio_free()` interface, declared in `<linux/gpio.h>`, works with global GPIO numbers and is `EXPORT_SYMBOL_GPL` on all kernel versions. We use `desc_to_gpio()` to convert the descriptor to a global number.

### Fix

Three parts: add a field to `struct dht_sensor`, request in `dht_do_register()`, free in `dht_sensor_release()`.

### 4a. Add `gpiod` field to `struct dht_sensor`

**Before (v2.8):**
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
    struct kref refcount;
    struct list_head list;
};
```

**After (v2.8.1):**
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
    struct gpio_desc *gpiod;          /* Requested GPIO descriptor (owned by this sensor) */
    struct kref refcount;
    struct list_head list;
};
```

### 4b. Request GPIO in `dht_do_register()`

**Before (v2.8):**
```c
    /* Resolve the GPIO descriptor for this BCM pin */
    desc = dht_find_desc(pin);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return -ENODEV;
    }

    /* Log which GPIO chip the pin was found on (for diagnostics) */
    chip = gpiod_to_chip(desc);
    if (chip && chip->label)
        pin_log(pin, "found on '%s' (base=%d, global=%d)\n",
                chip->label, chip->base, chip->base + pin);
    else
        pin_log(pin, "found (global=%d)\n", pin);

    /* Allocate and initialize the sensor struct */
    sensor = kzalloc(sizeof(*sensor), GFP_KERNEL);
    if (!sensor)
        return -ENOMEM;
```

**After (v2.8.1):**
```c
    /* Resolve the GPIO descriptor for this BCM pin */
    desc = dht_find_desc(pin);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return -ENODEV;
    }

    /* Request (claim) the GPIO line so no other driver can use it.
     * Without this, gpiod_direction_output/input may fail with
     * -EACCES on kernels with enforced request-before-use.
     * Use legacy gpio_request with desc_to_gpio() since gpiod_request
     * is not exported to modules. */
    ret = gpio_request(desc_to_gpio(desc), "dht");
    if (ret) {
        pin_err(pin, "GPIO request failed (already in use?): %d\n", ret);
        return -EBUSY;
    }

    /* Log which GPIO chip the pin was found on (for diagnostics) */
    chip = gpiod_to_chip(desc);
    if (chip && chip->label)
        pin_log(pin, "found on '%s' (base=%d, global=%d)\n",
                chip->label, chip->base, chip->base + pin);
    else
        pin_log(pin, "found (global=%d)\n", pin);

    /* Allocate and initialize the sensor struct */
    sensor = kzalloc(sizeof(*sensor), GFP_KERNEL);
    if (!sensor) {
        gpio_free(desc_to_gpio(desc));
        return -ENOMEM;
    }
```

Also, store the descriptor in the sensor struct:

```c
    sensor->gpiod = desc;   /* Store for gpio_free in release */
    sensor->pin = pin;
    sensor->interval = interval;
```

### 4c. Free GPIO in `dht_sensor_release()`

**Before (v2.8):**
```c
static void dht_sensor_release(struct kref *ref)
{
    struct dht_sensor *sensor = container_of(ref, struct dht_sensor, refcount);

    dht_stop_poll(sensor);
    proc_remove(sensor->proc_dir);
    mutex_destroy(&sensor->lock);
    kfree(sensor);
}
```

**After (v2.8.1):**
```c
static void dht_sensor_release(struct kref *ref)
{
    struct dht_sensor *sensor = container_of(ref, struct dht_sensor, refcount);

    dht_stop_poll(sensor);
    gpio_free(desc_to_gpio(sensor->gpiod));   /* Release the GPIO line */
    proc_remove(sensor->proc_dir);
    mutex_destroy(&sensor->lock);
    kfree(sensor);
}
```

### Why this works

- `gpio_request()` claims the GPIO line exclusively — other drivers calling `gpio_request` on the same pin will get `-EBUSY`.
- On kernels with enforced request-before-use, `gpiod_direction_output/input` now succeed because the line is properly claimed.
- `gpio_free()` in `dht_sensor_release()` releases the line when the sensor is unregistered or the module is unloaded.
- `desc_to_gpio()` converts the `gpio_desc *` to the global GPIO number expected by the legacy API.

---

## Testing Recommendations

After applying all four patches:

1. **Sensor registration** — check `dmesg` for absence of GPIO request errors.
2. **Disable auto-poll** (`echo -1 > /proc/sensors/dht/auto_interval`) — poll thread goes idle, no crash.
3. **Re-enable auto-poll** (`echo 10 > /proc/sensors/dht/auto_interval`) — thread resumes polling automatically.
4. **`rmmod` after idle thread** — clean unload without kernel warnings.
5. **Simulate proc_create failure** (low memory) — `proc_dir = NULL` prevents double `proc_remove`.
6. **Rapid sensor reads** — `pulse_ns = 0` prevents garbage in bit decoding.
