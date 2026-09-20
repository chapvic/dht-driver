# DHT11/DHT22/AM2302 Linux Kernel Driver

A Linux kernel module for reading temperature and humidity data from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins. The driver exposes a procfs interface under `/proc/sensors/dht/` for managing sensor registration, configuration, and data retrieval.

**Version:** 2.6  
**Author:** Chapvic  
**License:** GPL v3  
**Copyright:** © 2026

---

## Table of Contents

- [Overview](#overview)
- [Supported Hardware](#supported-hardware)
- [Wiring](#wiring)
- [Quick Start](#quick-start)
- [Procfs Interface](#procfs-interface)
- [Usage Examples](#usage-examples)
- [Error Codes](#error-codes)
- [Configuration](#configuration)
- [Debug Mode](#debug-mode)
- [Architecture](#architecture)
- [Version History](#version-history)
- [Known Limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Files in This Release](#files-in-this-release)
- [Build Requirements](#build-requirements)
- [License](#license)
- [Acknowledgements](#acknowledgements)

---

## Overview

The DHT driver is a Linux kernel module that communicates with DHT11, DHT22, and AM2302 temperature and humidity sensors over the single-wire GPIO protocol. It provides:

- **Dynamic sensor registration** via procfs export/unexport mechanism (similar to GPIO sysfs)
- **Per-sensor proc entries** for temperature, humidity, status, configuration, and metadata
- **Background polling** with per-sensor or global auto-poll intervals
- **Nanosecond-precision pulse timing** for reliable reads across all Raspberry Pi models
- **GPIO chip base caching** for fast multi-sensor registration on Pi 3/4/5
- **Rate limiting** for all measurements (manual and auto-poll)
- **Safe module unload** with module reference counting — `rmmod` blocks while procfs files are open
- **Shared `/proc/sensors`** — coexists with other sensor drivers without conflict

---

## Supported Hardware

### Sensors

| Sensor | Humidity Range | Temperature Range | Resolution | Protocol |
|--------|---------------|-------------------|------------|----------|
| DHT11 | 20–90% RH | 0–50 °C | Integer (1% / 1 °C) | Single-wire, 40-bit |
| DHT22 (AM2302) | 0–100% RH | −40–80 °C | Decimal (0.1% / 0.1 °C) | Single-wire, 40-bit |

Sensor type is auto-detected on the first successful measurement based on the data format.

### Raspberry Pi Models

| Model | GPIO Chip Label | Base Offset | Notes |
|-------|----------------|-------------|-------|
| Pi 5 | `pinctrl-rp1` / `pinctrl-bcm2712` | ≥ 512 | RP1 south bridge, large base offset |
| Pi 4 | `pinctrl-bcm2711` | 0 | Standard BCM numbering |
| Pi 3 / Pi 2 | `pinctrl-bcm2835` | 0 | Standard BCM numbering |
| Pi 1 / Pi Zero | `pinctrl-bcm2835` | 0 | Standard BCM numbering |

The driver uses a three-stage GPIO lookup strategy (see [Architecture](#architecture)) to handle the different base offsets across Pi models automatically.

### Valid GPIO Pins

BCM pin numbers **0–27** are supported. These correspond to the 28 GPIO pins exposed on the Raspberry Pi 40-pin header.

---

## Wiring

Connect the DHT sensor to the Raspberry Pi as follows:

```
DHT Sensor          Raspberry Pi GPIO
──────────          ──────────────────
VCC (Pin 1)  ────── 3.3V (Pin 1 or 17)
DATA (Pin 2) ────── BCM GPIO pin of your choice (e.g., GPIO23 = Pin 16)
GND (Pin 4)  ────── GND (Pin 6 or 9)
```

**Pull-up resistor:** A 4.7 kΩ – 10 kΩ pull-up resistor between DATA and VCC is required. Some sensor breakout boards (e.g., AM2302) include this resistor on-board.

```
                    3.3V
                      │
                     ┌─┐
                     │ │ 4.7k–10k
                     │ │
                     └─┘
                      │
    GPIO pin ─────────┼──────── DATA pin
```

> **Note:** Do not use 5 V power for the sensor data line — Raspberry Pi GPIO pins are not 5 V tolerant. Power the sensor from 3.3 V.

---

## Quick Start

### 1. Install kernel headers

```bash
sudo apt install linux-headers-$(uname -r) build-essential
```

### 2. Build the module

```bash
make
```

### 3. Load the driver

```bash
sudo insmod dht.ko
```

Verify in dmesg:

```bash
dmesg | tail -5
# [DHT]: DHT Driver © 2026, Chapvic (v2.6)
# [DHT]: driver loaded - /proc/sensors/dht/ (max 32 sensors)
```

### 4. Register a sensor

```bash
echo 23 | sudo tee /proc/sensors/dht/export
# [dht_gpio_23]: registered successfully
```

### 5. Read temperature and humidity

```bash
cat /proc/sensors/dht/gpio23/value
# H=45.2
# T=23.1
```

### 6. Enable auto-polling (optional)

```bash
# Per-sensor auto-poll every 5 seconds
echo 5 | sudo tee /proc/sensors/dht/gpio23/interval

# Or global auto-poll for all sensors
echo 5 | sudo tee /proc/sensors/dht/auto_interval
```

### 7. Unregister a sensor

```bash
echo 23 | sudo tee /proc/sensors/dht/unexport
# [dht_gpio_23]: unregistered
```

### 8. Unload the driver

```bash
sudo rmmod dht
# [DHT]: driver unloaded
```

---

## Procfs Interface

The driver creates entries under `/proc/sensors/dht/`. If another sensor driver has already created `/proc/sensors/`, the DHT driver reuses it. The parent directory is only removed on unload if the DHT driver created it and no other subdirectories remain.

### Global Entries — `/proc/sensors/dht/`

| Entry | Permissions | Type | Description |
|-------|-------------|------|-------------|
| `debug` | `0666` (rw) | int | Debug logging: `0` = off (default), `1` = on |
| `version` | `0444` (r) | string | Driver version string |
| `export` | `0222` (w) | int | Write BCM pin number to register a new sensor |
| `unexport` | `0222` (w) | int | Write BCM pin number to unregister a sensor |
| `auto_interval` | `0666` (rw) | int | Global auto-poll interval in seconds (`2`–`60`, `-1` = off) |

### Per-Sensor Entries — `/proc/sensors/dht/gpio<pin>/`

| Entry | Permissions | Type | Description |
|-------|-------------|------|-------------|
| `pin` | `0444` (r) | int | BCM GPIO pin number |
| `interval` | `0644` (rw) | int | Per-sensor auto-poll interval (`2`–`60`, `-1` = off) |
| `measure` | `0222` (w) | int | Write `1` to trigger a manual measurement |
| `status_code` | `0444` (r) | int | Error code from last measurement (`0` = success) |
| `status_text` | `0444` (r) | string | Human-readable status description |
| `value` | `0444` (r) | string | `H=<humidity>\nT=<temperature>\n` |
| `info` | `0444` (r) | string | Sensor type and registration timestamp |
| `timestamp` | `0444` (r) | int | Unix timestamp of last successful measurement |

### Directory Structure

```
/proc/sensors/
└── dht/
    ├── debug            (rw)  — debug logging toggle
    ├── version          (r)   — driver version
    ├── export           (w)   — register sensor by BCM pin
    ├── unexport         (w)   — unregister sensor by BCM pin
    ├── auto_interval    (rw)  — global auto-poll interval
    ├── gpio4/           — one directory per registered sensor
    │   ├── pin          (r)
    │   ├── interval     (rw)
    │   ├── measure      (w)
    │   ├── status_code  (r)
    │   ├── status_text  (r)
    │   ├── value        (r)
    │   ├── info         (r)
    │   └── timestamp    (r)
    ├── gpio17/
    │   └── ...
    └── gpio23/
        └── ...
```

### Polling Priority

| Mode | Condition | Behavior |
|------|-----------|----------|
| Global auto-poll | `auto_interval` is `2`–`60` | All sensors polled at the shared interval. Per-sensor `interval` is stored but ignored. |
| Per-sensor auto-poll | `auto_interval` = `-1`, sensor `interval` = `2`–`60` | Only that sensor is polled at its own interval. |
| Manual mode | `auto_interval` = `-1`, sensor `interval` = `-1` | User triggers measurements by writing `1` to `measure`. |

Manual measurements are rejected with `ERR_AUTO_MODE` if either per-sensor or global auto-poll is active.

---

## Usage Examples

### Manual Single Read

```bash
# Register sensor on GPIO23
echo 23 | sudo tee /proc/sensors/dht/export

# Trigger a measurement
echo 1 | sudo tee /proc/sensors/dht/gpio23/measure

# Read the result
cat /proc/sensors/dht/gpio23/value
# H=45.2
# T=23.1

# Check status
cat /proc/sensors/dht/gpio23/status_text
# SUCCESS
```

### Per-Sensor Auto-Poll

```bash
# Register and enable polling every 10 seconds
echo 4 | sudo tee /proc/sensors/dht/export
echo 10 | sudo tee /proc/sensors/dht/gpio4/interval

# Values are updated automatically — just read them
cat /proc/sensors/dht/gpio4/value
# H=52.3
# T=19.7

# Check last measurement timestamp
cat /proc/sensors/dht/gpio4/timestamp
# 1779000123

# Disable polling for this sensor
echo -1 | sudo tee /proc/sensors/dht/gpio4/interval
```

### Global Auto-Poll for All Sensors

```bash
# Register multiple sensors
echo 4  | sudo tee /proc/sensors/dht/export
echo 17 | sudo tee /proc/sensors/dht/export
echo 23 | sudo tee /proc/sensors/dht/export

# Enable global auto-poll every 5 seconds
echo 5 | sudo tee /proc/sensors/dht/auto_interval

# All three sensors now poll every 5 seconds
cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio17/value
cat /proc/sensors/dht/gpio23/value

# Disable global auto-poll
echo -1 | sudo tee /proc/sensors/dht/auto_interval
```

### Parsing Values in a Shell Script

```bash
#!/bin/bash
SENSOR="/proc/sensors/dht/gpio23/value"

read_humidity() {
    awk -F= '/^H=/ {print $2}' "$SENSOR"
}

read_temperature() {
    awk -F= '/^T=/ {print $2}' "$SENSOR"
}

H=$(read_humidity)
T=$(read_temperature)
echo "Humidity: ${H}%, Temperature: ${T}°C"
```

### Python Example

```python
def read_dht(pin):
    with open(f"/proc/sensors/dht/gpio{pin}/value") as f:
        data = {}
        for line in f:
            key, val = line.strip().split("=")
            data[key] = float(val)
    return data["H"], data["T"]

h, t = read_dht(23)
print(f"Humidity: {h}%, Temperature: {t}C")
```

### Safe Unload

```bash
# The driver refuses to unload if any procfs file is open:
tail -f /proc/sensors/dht/gpio23/value &
sudo rmmod dht
# ERROR: Module dht is in use

# Close the file, then unload:
kill %1
sudo rmmod dht
# [DHT]: driver unloaded
```

---

## Error Codes

### Driver Error Codes

These are the values returned in the `status_code` proc entry:

| Code | Constant | Description |
|------|----------|-------------|
| 0 | `ERR_SUCCESS` | Operation completed successfully |
| 1 | `ERR_PIN_INVALID` | The specified GPIO pin number is out of valid range (0–27) |
| 2 | `ERR_GPIO_REQUEST` | Failed to request or find the GPIO descriptor |
| 3 | `ERR_READ_FAILED` | Sensor data read failed (checksum error, timeout, etc.) — all 3 retry attempts exhausted |
| 4 | `ERR_AUTO_MODE` | Manual measurement attempted while auto-poll is active |
| 5 | `ERR_TOO_SOON` | Manual measurement rejected due to rate limiting (minimum 2 s between measurements) |

### Export/Unexport errno Values

When writing to `export` or `unexport`, the kernel may return these standard `errno` values:

| errno | Meaning |
|-------|---------|
| `-EIO` | Initial measurement failed — sensor not registered |
| `-EINVAL` | Invalid pin number or format |
| `-ENODEV` | Pin not currently registered (unexport) |
| `-ENOMEM` | Out of memory or procfs entry creation failed |
| `-EBUSY` | Pin already registered (export) |

---

## Configuration

### Module Parameter

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `dht_debug` | int | `0` | Debug logging: `0` = off, `1` = on. Can also be toggled at runtime via `/proc/sensors/dht/debug`. |

```bash
# Load with debug enabled
sudo insmod dht.ko dht_debug=1

# Toggle at runtime
echo 1 | sudo tee /proc/sensors/dht/debug
echo 0 | sudo tee /proc/sensors/dht/debug
```

### Compile-Time Constants

| Constant | Default | Description |
|----------|---------|-------------|
| `MAX_SENSORS` | 32 | Maximum number of simultaneously registered sensors |
| `MAX_PIN_NUM` | 27 | Highest valid BCM GPIO pin number |
| `MIN_INTERVAL` | 2 | Minimum auto-poll interval in seconds |
| `MAX_INTERVAL` | 60 | Maximum auto-poll interval in seconds |
| `MEAS_MIN_GAP` | 2 | Minimum seconds between manual measurements (rate limiting) |
| `MAX_RETRIES` | 3 | Number of read attempts before giving up |
| `RETRY_DELAY_MS` | 100 | Delay in milliseconds between read retries |
| `BIT_THRESHOLD` | 40000 | Nanosecond threshold to distinguish 0 (~26 µs) from 1 (~70 µs) pulses |
| `PULSE_TIMEOUT_NS` | 200000 | Maximum nanoseconds to wait for a single pulse (200 µs) |
| `MAX_TIMINGS` | 100 | Maximum number of pulse transitions to capture in one read cycle |

---

## Debug Mode

Enable debug logging to see detailed driver activity in `dmesg`:

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
```

Example debug output:

```
[DHT]: debug enabled
[dht_gpio_23]: poll thread started
[dht_gpio_23]: read attempt 1 failed - j=38, data=[72,0,23,1,96]
[dht_gpio_23]: registered successfully
[dht_gpio_23]: auto-poll enabled (interval=5)
[dht_gpio_23]: poll thread stopped
[DHT]: debug disabled
```

Debug messages include:
- GPIO descriptor resolution stages (cache hit, direct lookup, full scan)
- Read attempt details (attempt number, bit count, raw data bytes)
- Poll thread start/stop events
- Rate limiting decisions (manual and auto-poll)
- Procfs directory creation and ownership tracking

---

## Architecture

### GPIO Chip Detection — Three-Stage Lookup

The driver resolves BCM pin numbers to GPIO descriptors using a three-stage strategy:

1. **Fast path (cache hit):** If `cached_chip_base` is known (set on first successful lookup), compute `global = base + bcm_pin` and call `gpio_to_desc()` directly. This is the common case after the first sensor is registered.

2. **Direct lookup:** Try `gpio_to_desc(bcm_pin)` — works on Pi 3/4 where the GPIO chip base is 0 and BCM numbers equal global GPIO numbers. If the descriptor belongs to a known Pi GPIO chip, cache its base.

3. **Full scan:** Iterate over all global GPIO numbers (0–2048) looking for a Pi GPIO chip where the local offset `(global - chip->base)` matches the requested BCM pin. Needed on Pi 5 where the base offset is large (≥ 512). On success, cache the chip base for future fast-path lookups.

Chip identification uses `is_pi_gpio_chip()` which checks the chip label for known Pi controller names: `pinctrl-rp1`, `pinctrl-bcm2835`, `pinctrl-bcm2711`, `pinctrl-bcm2712`.

### Rate Limiting

All measurements (manual and auto-poll) are subject to rate limiting with a minimum gap of `MEAS_MIN_GAP` (2) seconds since the last attempt:

- **Manual measurements:** If the gap is too short, the measurement is rejected with `ERR_TOO_SOON` (code 5). The user sees the error in `status_code` and `status_text`.
- **Auto-poll measurements:** If the gap is too short, the measurement is silently skipped. The last cached data is preserved and returned to the user.

This protects DHT sensors from being polled too frequently, which causes unreliable readings and sensor heating.

### Safe Module Unload — Two-Phase Teardown

The driver uses a two-phase teardown sequence to prevent race conditions during module unload:

**Phase 1 — Block new access and remove procfs:**
1. Set `dht_exiting` atomic flag — all new `dht_proc_open()` calls return `-ENODEV`
2. Disable global auto-poll (`global_auto_interval = -1`)
3. `proc_remove(proc_dir)` removes the entire `/proc/sensors/dht/` subtree. This call blocks until all currently open procfs files are closed (their release handlers call `module_put`, decrementing the module reference count)

**Phase 2 — Stop threads and free memory:**
4. Splice the sensor list under `list_lock` (no new sensors can appear — procfs is gone)
5. For each sensor: `kthread_stop()` (waits for thread to exit), `mutex_destroy()`, `kfree()`

The key insight: after Phase 1, no new file operations can start because all procfs entries are removed. The module reference count (incremented by `try_module_get()` in every `dht_proc_open()`) prevents `rmmod` from proceeding until all open files are closed.

### Shared `/proc/sensors` — Coexistence with Other Drivers

The driver is designed to share the `/proc/sensors` parent directory with other sensor drivers:

**On load (`dht_driver_init`):**
1. Attempt `proc_mkdir("sensors", NULL)` — if it succeeds, the DHT driver created the directory and sets `we_created_parent = true`
2. If it returns `NULL`, the directory already exists (another driver created it). The DHT driver creates its subdirectory via the full path: `proc_mkdir("sensors/dht", NULL)` — the kernel resolves the existing parent automatically

**On unload (`dht_driver_exit`):**
1. Remove only the DHT subtree: `proc_remove(proc_dir)` — removes `/proc/sensors/dht/` and all sensor subdirectories
2. If `we_created_parent` is `true`, check whether `/proc/sensors` is now empty using a VFS directory iteration (`filp_open` + `iterate_dir`)
3. If empty → remove `/proc/sensors`. If other drivers' subdirectories remain → leave it and log: `"/proc/sensors not removed (other drivers using it)"`
4. If `we_created_parent` is `false` — the DHT driver never owned `/proc/sensors`, so it does not touch it

This ensures multiple sensor drivers can coexist without one driver accidentally deleting another's procfs entries.

### DHT Protocol Implementation

The driver implements the single-wire DHT communication protocol:

1. **Start signal:** Pull the data line low for 20 ms, then release (switch to input mode)
2. **Sensor response:** Wait 40 µs for the sensor to pull the line low, then high
3. **Data reading:** Read 40 data bits by measuring high-pulse widths using `ktime_get_ns()`:
   - ~26 µs high pulse = bit `0`
   - ~70 µs high pulse = bit `1`
   - Threshold: `BIT_THRESHOLD` = 40,000 ns (40 µs)
4. **Checksum validation:** 5th byte must equal `(byte1 + byte2 + byte3 + byte4) & 0xFF`
5. **Type detection:** If the combined 16-bit humidity value > 1000, the sensor is a DHT11 (integer format). Otherwise it is a DHT22/AM2302 (decimal format, already scaled ×10).
6. **Retry:** Up to 3 attempts with 100 ms delay between retries

### Poll Thread Architecture

Each polled sensor runs a dedicated kernel thread (`dht_poll_<pin>`). The thread:
- Calls `dht_do_measurement()` once per interval
- Sleeps in 1-second increments to respond quickly to `kthread_should_stop()`
- Uses the global interval if active, otherwise falls back to the per-sensor interval
- Respects rate limiting (silently skips if too soon since last measurement)

---

## Version History

### v2.6 — Shared `/proc/sensors`

- **Problem:** The driver unconditionally created `/proc/sensors` on load and destroyed it (including other drivers' entries) via `remove_proc_subtree` if it already existed. On unload, it unconditionally removed the parent directory.
- **Solution:**
  - Ownership flag `we_created_parent` tracks whether the DHT driver created `/proc/sensors`
  - If the directory already exists, the driver creates its subdirectory via the full path `proc_mkdir("sensors/dht", NULL)`
  - On unload, the driver removes only `/proc/sensors/dht/`, then checks `/proc/sensors` for emptiness via VFS `iterate_dir` before removing it
  - `remove_proc_subtree` is no longer used
- Multiple sensor drivers can now coexist under `/proc/sensors/` without conflict

### v2.5.2 — Safe Module Unload

- Added `try_module_get()` / `module_put()` on all procfs open/release handlers
- Added `atomic_t dht_exiting` flag to reject new procfs operations during unload
- Two-phase teardown: remove procfs first (blocking until files close), then free sensor memory
- `rmmod` now blocks with `EBUSY` while any procfs file is open

### v2.5.1 — Rate Limiting

- Rate limiting (`MEAS_MIN_GAP` = 2 s) applies to **all** measurements, not just manual
- Auto-poll measurements that are too soon since the last attempt are silently skipped (last data preserved)
- Manual measurements that are too soon return `ERR_TOO_SOON`

### v2.5 — Initial Release

- Nanosecond-precision pulse timing via `ktime_get_ns()`
- 3 retry attempts with 100 ms delay
- GPIO chip base caching for fast multi-sensor registration on Pi 3/4/5
- Three-stage GPIO lookup (cache → direct → full scan)
- Per-sensor mutex for thread-safe data access
- Background polling threads with per-sensor and global intervals
- Procfs interface with export/unexport, per-sensor entries, and global configuration
- Auto-detection of sensor type (DHT11 vs DHT22/AM2302)
- 20 ms start signal (increased reliability)
- Kernel API compatibility macros for `struct proc_ops` (5.6+) and `pde_data()` (5.17+)

---

## Known Limitations

1. **Maximum 32 sensors** — The driver supports up to `MAX_SENSORS` (32) simultaneously registered sensors. This limit is compile-time configurable.

2. **BCM pins 0–27 only** — Only the 28 GPIO pins exposed on the Raspberry Pi 40-pin header are supported. Pins with special functions (e.g., GPIO0/1 for I2C) can be used but may conflict with other drivers.

3. **Minimum 2 seconds between reads** — DHT sensors require at least 2 seconds between measurements. The driver enforces this via rate limiting for both manual and auto-poll measurements.

4. **Single-wire protocol timing sensitivity** — The DHT protocol relies on precise microsecond timing. Under heavy CPU load, readings may fail. The driver retries up to 3 times and caches the last successful result.

5. **No hardware interrupts** — The driver uses busy-loop polling with `ktime_get_ns()` for pulse measurement. It does not use GPIO interrupts, which would require a different approach.

6. **No device tree binding** — Sensors are registered dynamically via procfs, not through device tree. This allows runtime flexibility but requires manual configuration.

---

## Troubleshooting

### Sensor registration fails with "GPIO request/lookup failed"

```
[dht_gpio_23]: GPIO descriptor not found
[dht_gpio_23]: registration failed - GPIO request/lookup failed
```

**Causes and solutions:**
- The GPIO pin is already in use by another driver (e.g., I2C, SPI). Check with `sudo cat /sys/kernel/debug/gpio`.
- The pin number is invalid. Valid range is 0–27.
- The kernel does not expose the GPIO chip. Ensure the `pinctrl-bcm2835` (or `pinctrl-rp1` for Pi 5) driver is loaded.

### Read fails with "Sensor data read failed"

```
[dht_gpio_23]: read failed after 3 attempts - j=38, data=[72,0,23,1,96]
```

**Causes and solutions:**
- **Missing or wrong pull-up resistor** — Ensure a 4.7 kΩ – 10 kΩ resistor is between DATA and VCC.
- **Wire too long** — Keep the data wire under 20 cm for reliable readings. For longer runs, use a shielded cable.
- **Sensor powered from 5 V with 3.3 V GPIO** — Use a level shifter or power the sensor from 3.3 V.
- **CPU overload** — Reduce system load or increase the poll interval. The driver retries 3 times automatically.
- **Sensor defective** — Try a different sensor.

### `rmmod` fails with "Module dht is in use"

```bash
sudo rmmod dht
# ERROR: Module dht is in use
```

A procfs file is still open. Close all files under `/proc/sensors/dht/` and try again. This is by design — the module reference counting prevents unsafe unloads.

### `cat /proc/sensors/dht/gpio23/value` shows stale data

If auto-poll is disabled and the last manual measurement failed, the `value` entry shows the last successful reading (or "No measurement taken" if no measurement has succeeded). Trigger a new measurement:

```bash
echo 1 | sudo tee /proc/sensors/dht/gpio23/measure
```

### `/proc/sensors` not removed after unload

```
[DHT]: /proc/sensors not removed (other drivers using it)
```

This is expected behavior when another sensor driver has subdirectories under `/proc/sensors/`. The DHT driver only removes `/proc/sensors` if it created the directory and no other subdirectories remain.

### Enable debug for detailed diagnostics

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
dmesg | grep -E "\[DHT\]|\[dht_gpio"
```

---

## Files in This Release

| File | Description |
|------|-------------|
| `dht.c` | Driver source code (single-file kernel module) |
| `Makefile` | Build configuration for `make` |
| `README.md` | Readme file (English) |
| `README_RU.md` | Readme file (Russian) |
| `changelog.txt` | Detailed changelog (English) |
| `changelog_ru.txt` | Detailed changelog (Russian) |

---

## Build Requirements

### Kernel Version

The driver is compatible with Linux kernel **5.x** and later, including **6.18+**. Compatibility macros handle API changes:

| Kernel Version | API Change | Driver Handling |
|----------------|------------|-----------------|
| 5.6+ | `struct proc_ops` replaces `struct file_operations` for procfs | Compile-time macro `DHT_PROC_OPS` |
| 5.17+ | `PDE_DATA()` renamed to `pde_data()` | Compile-time macro `DHT_PDE_DATA()` |
| 6.x+ | `dir_context.actor` returns `bool` instead of `int` | `dht_dir_filldir` returns `bool` |

### Tested Configurations

| Hardware | Kernel | Result |
|----------|--------|--------|
| Raspberry Pi 5 | 6.18.50+ (rpt-rpi-2712) | Compiles and runs |
| Raspberry Pi 4 | 6.1.x (rpt-rpi-2711) | Compiles and runs |
| Raspberry Pi 3 | 5.15.x (rpt-rpi-bcm2835) | Compiles and runs |

### Build Dependencies

```bash
sudo apt install linux-headers-$(uname -r) build-essential
```

---

## License

This program is free software; you can redistribute it and/or modify it under the terms of the **GNU General Public License version 3** as published by the Free Software Foundation.

```
DHT Driver © 2026, Chapvic
Licensed under GPL v3
```

---

## Acknowledgements

**Author:** Chapvic

**Tested on:**

| Hardware | Sensors Used |
|----------|-------------|
| Raspberry Pi 5 (8 GB) | DHT22, AM2302 |
| Raspberry Pi 4 (4 GB) | DHT11, DHT22 |
| Raspberry Pi 3 (1 GB) | DHT11 |

**GPIO testing equipment:** Logic analyzer for protocol verification, oscilloscope for signal integrity checks.
