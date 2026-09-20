# DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver

A Linux kernel module for reading temperature and humidity data from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins. The driver exposes a procfs interface under `/proc/sensors/dht/` for sensor management, configuration, and data retrieval.

**Version:** 2.7  
**Author:** Chapvic  
**License:** GPL v3  
**Kernel:** 5.10+ (tested up to 6.18)  
**Architecture:** ARM64 (Raspberry Pi 3/4/5), ARM32 (Pi 1/2/Zero/Zero 2 W)

---

## Table of Contents

- [Overview](#overview)
- [Supported Hardware](#supported-hardware)
- [Wiring](#wiring)
- [Quick Start](#quick-start)
- [Configuration File](#configuration-file)
- [Procfs Interface](#procfs-interface)
- [Usage Examples](#usage-examples)
- [Error Codes](#error-codes)
- [Configuration Parameters](#configuration-parameters)
- [Debug Mode](#debug-mode)
- [Architecture](#architecture)
- [Version History](#version-history)
- [Known Limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Files in This Repository](#files-in-this-repository)
- [Build Requirements](#build-requirements)
- [License](#license)
- [Acknowledgements](#acknowledgements)

---

## Overview

The DHT driver provides a kernel-level interface for reading temperature and humidity from DHT-series single-wire sensors. It is designed specifically for Raspberry Pi and handles the GPIO numbering differences across Pi 3, 4, and 5 models automatically.

### Key Features

- **Dynamic sensor registration** via procfs — export and unexport sensors at runtime
- **Per-sensor proc entries** for temperature, humidity, status, and configuration
- **Background polling threads** with configurable per-sensor or global intervals
- **Global auto-poll mode** with a shared interval across all sensors
- **GPIO chip base caching** for fast multi-sensor registration on Pi 3/4/5
- **Nanosecond-precision pulse timing** for reliable reads across all Pi models
- **Rate limiting** (minimum 2 s between measurements) for all read types
- **Safe module unload** with module reference counting — prevents `rmmod` while procfs files are open
- **Shared `/proc/sensors`** — coexists with other sensor drivers
- **Configuration file** — optional `/etc/default/dht` for auto-registration and settings at load time
- **Automatic sensor type detection** — distinguishes DHT11 from DHT22/AM2302 based on data format

---

## Supported Hardware

### Sensors

| Sensor | Temperature Range | Humidity Range | Resolution | Notes |
|--------|-------------------|----------------|------------|-------|
| DHT11 | 0–50 °C (±2 °C) | 20–90% (±5%) | Integer | Basic, cheapest option |
| DHT22 (AM2302) | −40–80 °C (±0.5 °C) | 0–100% (±2%) | Decimal (1 decimal place) | Higher accuracy, wider range |
| AM2302 | −40–80 °C (±0.5 °C) | 0–100% (±2%) | Decimal (1 decimal place) | DHT22 in a waterproof probe housing |

All three sensors use the same single-wire protocol. The driver auto-detects the sensor type from the data format on the first successful measurement.

### Raspberry Pi Models

| Model | GPIO Chip Label | GPIO Base | Notes |
|-------|----------------|-----------|-------|
| Pi 5 | `pinctrl-rp1` / `pinctrl-bcm2712` | 512+ | RP1 south bridge, large base offset |
| Pi 4 (B) | `pinctrl-bcm2711` | 0 | Direct BCM numbering |
| Pi 3 (B/B+) | `pinctrl-bcm2835` | 0 | Direct BCM numbering |
| Pi 2 (B) | `pinctrl-bcm2835` | 0 | Direct BCM numbering |
| Pi 1 (B/A) | `pinctrl-bcm2835` | 0 | Direct BCM numbering |
| Pi Zero / Zero 2 W | `pinctrl-bcm2835` | 0 | Direct BCM numbering |

The driver handles the GPIO base offset difference automatically via a three-stage lookup with caching (see [Architecture](#architecture)).

### Kernel Compatibility

- **Minimum:** Linux 5.10 (Raspberry Pi OS Bullseye)
- **Tested:** Linux 5.15, 6.1, 6.6, 6.18
- **API compatibility macros** handle `proc_ops` vs `file_operations` (5.6+) and `pde_data()` vs `PDE_DATA()` (5.17+)

---

## Wiring

Connect the sensor's data pin to any available BCM GPIO pin (0–27) on the Raspberry Pi header.

### Wiring Diagram

```
DHT Sensor          Raspberry Pi GPIO
-----------         -----------------
VCC  (Pin 1)  --->  3.3V  (Pin 1 or 17)
DATA (Pin 2)  --->  GPIOx  (any BCM pin 0-27)
NC   (Pin 3)  --->  (not connected)
GND  (Pin 4)  --->  GND   (Pin 6, 9, 14, 20, 25, 30, 34, 39)
```

### Pull-up Resistor

A **4.7 kΩ–10 kΩ pull-up resistor** between VCC (3.3V) and DATA is required:

- Many DHT22/AM2302 breakout boards include an on-board pull-up — check your board.
- If using a bare DHT11 or DHT22 without a breakout board, add an external resistor.
- The Raspberry Pi internal pull-up is too weak for reliable DHT communication at cable lengths > 10 cm.

### Pin Selection Notes

- GPIO pins 0–27 are valid for sensor connection.
- Avoid pins already in use by other devices (SPI, I2C, UART, PWM, HATs).
- Common choices: GPIO4, GPIO17, GPIO22, GPIO23, GPIO24, GPIO25, GPIO27.

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

### 3. Load the module

```bash
sudo insmod dht.ko
```

Verify in dmesg:

```
[DHT]: DHT Driver © 2026, Chapvic (v2.7)
[DHT]: driver loaded - /proc/sensors/dht/ (max 32 sensors)
```

### 4. Register a sensor

```bash
# Register a sensor on BCM GPIO pin 4
echo 4 | sudo tee /proc/sensors/dht/export
```

If the sensor is connected and working, dmesg will show:

```
[dht_gpio_4]: registered successfully
```

### 5. Read sensor data

```bash
cat /proc/sensors/dht/gpio4/value
```

Output:

```
H=45.2
T=23.1
```

### 6. Enable auto-polling (optional)

```bash
# Global auto-poll every 10 seconds for all sensors
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# Or per-sensor auto-poll
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval
```

### 7. Unregister a sensor

```bash
echo 4 | sudo tee /proc/sensors/dht/unexport
```

### 8. Unload the module

```bash
sudo rmmod dht
```

---

## Configuration File

The driver reads an optional configuration file at `/etc/default/dht` during module load. If the file does not exist, the driver loads with defaults.

### Format

- Lines starting with `#` are comments and are ignored.
- Empty lines are ignored.
- Options are case-sensitive and use `KEY=value` syntax.
- Options are processed top-to-bottom — order matters for `AUTO_INTERVAL` and `SENSOR`.
- Unknown option names produce a warning in dmesg.
- Invalid values produce a warning in dmesg.

### Global Options

| Option | Syntax | Description |
|--------|--------|-------------|
| `DEBUG` | `DEBUG` or `DEBUG=1` | Enable debug logging at load time |
| `DEBUG` | `DEBUG=0` | Explicitly disable debug logging |
| `AUTO_INTERVAL` | `AUTO_INTERVAL=N` | Global auto-poll interval in seconds (2–60). Starts polling for all registered sensors. |

### Sensor Registration

| Option | Syntax | Description |
|--------|--------|-------------|
| `SENSOR` | `SENSOR=<pin>` | Register a sensor on the given BCM pin (no auto-poll) |
| `SENSOR` | `SENSOR=<pin>,<interval>` | Register a sensor with per-sensor auto-poll interval (2–60 seconds) |

### Example

```bash
# /etc/default/dht
#
# Enable debug logging
DEBUG=1
#
# Global auto-poll every 10 seconds
AUTO_INTERVAL=10
#
# Register sensors
SENSOR=4              # DHT22 on GPIO4, uses global interval (10s)
SENSOR=17,5           # DHT11 on GPIO17, polls every 5 seconds
SENSOR=22             # Sensor on GPIO22, no auto-poll (manual only)
```

### Option Processing Order

Options are processed sequentially from top to bottom. This affects the interaction between `AUTO_INTERVAL` and `SENSOR`:

- If `AUTO_INTERVAL=10` appears **before** `SENSOR=4`, the sensor is registered and immediately starts polling at the global interval.
- If `AUTO_INTERVAL=10` appears **after** `SENSOR=4`, the sensor is registered without polling first, then `AUTO_INTERVAL` starts polling for all already-registered sensors.

Both scenarios work correctly. The recommended approach is to place `AUTO_INTERVAL` before `SENSOR` entries for predictable behavior.

### Error Handling

| Situation | dmesg Message |
|-----------|---------------|
| File missing | *(silent — debug message only)* |
| Unknown option | `config: unknown option 'XXX'` |
| Invalid pin | `config: SENSOR pin 'XX' invalid (0-27)` |
| Invalid interval | `config: SENSOR interval 'XX' invalid (2-60 or -1)` |
| Invalid DEBUG value | `config: DEBUG='XX' invalid (expected 0 or 1)` |
| Invalid AUTO_INTERVAL | `config: AUTO_INTERVAL='XX' is not a valid integer` or `out of range` |

---

## Procfs Interface

The driver creates a procfs hierarchy under `/proc/sensors/dht/`. The `/proc/sensors` parent directory is shared with other sensor drivers — the driver creates it if it does not exist, and removes it on unload only if no other drivers are using it.

### Global Entries

```
/proc/sensors/dht/
├── debug           (rw, 0666) — debug logging: 0 = off (default), 1 = on
├── version         (r,  0444) — driver version string
├── export          (w,  0222) — write BCM pin number to register a sensor
├── unexport        (w,  0222) — write BCM pin number to unregister a sensor
└── auto_interval   (rw, 0666) — global auto-poll interval (2-60, -1 = off)
```

| Entry | Permissions | Description |
|-------|-------------|-------------|
| `debug` | rw (0666) | Write `0` or `1` to disable/enable debug logging. Read to check current state. |
| `version` | r (0444) | Returns the driver version string (e.g., `2.7`). |
| `export` | w (0222) | Write a BCM pin number (0–27) to register a new sensor. |
| `unexport` | w (0222) | Write a BCM pin number to unregister an existing sensor. |
| `auto_interval` | rw (0666) | Write a value 2–60 to enable global auto-poll, or `-1` to disable. Read to check current value. |

### Per-Sensor Entries

Each registered sensor gets its own subdirectory:

```
/proc/sensors/dht/gpio<pin>/
├── pin           (r,  0444) — BCM GPIO pin number
├── interval      (rw, 0644) — per-sensor auto-poll interval (2-60, -1 = off)
├── measure       (w,  0222) — write "1" to trigger a manual measurement
├── status_code   (r,  0444) — error code from last measurement (0 = success)
├── status_text   (r,  0444) — human-readable error description
├── value         (r,  0444) — "H=<humidity>\nT=<temperature>\n"
├── info          (r,  0444) — sensor type and registration time
└── timestamp     (r,  0444) — Unix timestamp of last successful measurement
```

| Entry | Permissions | Description |
|-------|-------------|-------------|
| `pin` | r (0444) | The BCM GPIO pin number this sensor is connected to. |
| `interval` | rw (0644) | Per-sensor auto-poll interval. Write `2`–`60` to enable polling, or `-1` to disable. Ignored when global auto-poll is active. |
| `measure` | w (0222) | Write `1` to trigger a manual measurement. Ignored if auto-poll is active (per-sensor or global). |
| `status_code` | r (0444) | Numeric error code from the last measurement attempt (see [Error Codes](#error-codes)). |
| `status_text` | r (0444) | Human-readable description of the last measurement result. |
| `value` | r (0444) | Current temperature and humidity in the format `H=<humidity>\nT=<temperature>\n`. Values are decimal (e.g., `45.2`, `23.1`). |
| `info` | r (0444) | Sensor type (`DHT11` or `DHT22/AM2302`) and registration time in ISO 8601 format. |
| `timestamp` | r (0444) | Unix timestamp (seconds since epoch, UTC) of the last successful measurement. |

### Polling Priority

When both global and per-sensor intervals are set, the global interval takes priority:

| Global `auto_interval` | Per-sensor `interval` | Effective Polling |
|------------------------|-----------------------|-------------------|
| `-1` (off) | `-1` (off) | No polling — manual only |
| `-1` (off) | `2`–`60` | Per-sensor polling at the specified interval |
| `2`–`60` | `-1` (off) | Global polling at the global interval |
| `2`–`60` | `2`–`60` | Global polling at the **global** interval (per-sensor value stored but not used) |

---

## Usage Examples

### Manual Mode — Read on Demand

```bash
# Register a sensor on GPIO4
echo 4 | sudo tee /proc/sensors/dht/export

# Read the current value
cat /proc/sensors/dht/gpio4/value
# H=45.2
# T=23.1

# Trigger a fresh measurement (rate-limited: min 2 s between reads)
echo 1 | sudo tee /proc/sensors/dht/gpio4/measure

# Read again
cat /proc/sensors/dht/gpio4/value
# H=45.3
# T=23.0

# Check status
cat /proc/sensors/dht/gpio4/status_code   # 0 (success)
cat /proc/sensors/dht/gpio4/status_text   # SUCCESS
cat /proc/sensors/dht/gpio4/info          # Type: DHT22  Registered: 2026-09-21T10:30:00Z
cat /proc/sensors/dht/gpio4/timestamp     # 1695280200
```

### Per-Sensor Auto-Poll

```bash
# Register and set per-sensor polling every 5 seconds
echo 4 | sudo tee /proc/sensors/dht/export
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval

# The poll thread runs in the background — just read values anytime
cat /proc/sensors/dht/gpio4/value
```

### Global Auto-Poll

```bash
# Register multiple sensors
echo 4  | sudo tee /proc/sensors/dht/export
echo 17 | sudo tee /proc/sensors/dht/export
echo 22 | sudo tee /proc/sensors/dht/export

# Enable global auto-poll every 10 seconds — all sensors start polling
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# Read any sensor's latest value
cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio17/value
cat /proc/sensors/dht/gpio22/value

# Disable global auto-poll (all sensors stop polling)
echo -1 | sudo tee /proc/sensors/dht/auto_interval
```

### Shell Script — Continuous Monitoring

```bash
#!/bin/bash
# monitor.sh — read all registered DHT sensors every 5 seconds

while true; do
    echo "=== $(date '+%Y-%m-%d %H:%M:%S') ==="
    for dir in /proc/sensors/dht/gpio*/; do
        [ -d "$dir" ] || continue
        pin=$(cat "${dir}pin")
        value=$(cat "${dir}value")
        status=$(cat "${dir}status_text")
        echo "GPIO$pin: $value (status: $status)"
    done
    sleep 5
done
```

### Python — Reading Sensor Data

```python
#!/usr/bin/env python3
"""Read DHT sensor data from procfs."""

import os

def read_sensor(pin):
    base = f"/proc/sensors/dht/gpio{pin}"
    try:
        with open(f"{base}/value") as f:
            data = {}
            for line in f:
                key, val = line.strip().split("=")
                data[key] = float(val)
            return data
    except FileNotFoundError:
        return None

# Example: read sensor on GPIO4
sensor = read_sensor(4)
if sensor:
    print(f"Temperature: {sensor['T']}°C")
    print(f"Humidity: {sensor['H']}%")
else:
    print("Sensor not registered on GPIO4")
```

### Using the Configuration File

```bash
# Create the config file
sudo tee /etc/default/dht << 'EOF'
# DHT driver configuration
DEBUG=0
AUTO_INTERVAL=10
SENSOR=4
SENSOR=17,5
EOF

# Load the module — sensors are auto-registered
sudo insmod dht.ko

# Verify
dmesg | grep DHT
# [DHT]: reading config from /etc/default/dht (68 bytes)
# [DHT]: config: AUTO_INTERVAL=10
# [DHT]: config: SENSOR pin=4 interval=-1
# [dht_gpio_4]: registered successfully
# [DHT]: config: SENSOR pin=17 interval=5
# [dht_gpio_17]: registered successfully

cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio17/value
```

### Safe Module Unload

```bash
# The module cannot be unloaded while procfs files are open
# (module reference count prevents this)

# If a process is reading a sensor file, rmmod will wait
# until all files are closed

sudo rmmod dht
# [DHT]: driver unloaded
```

---

## Error Codes

### Driver Error Codes

| Code | Constant | Description |
|------|----------|-------------|
| 0 | `ERR_SUCCESS` | Measurement completed successfully |
| 1 | `ERR_PIN_INVALID` | The specified GPIO pin number is out of range (0–27) |
| 2 | `ERR_GPIO_REQUEST` | Failed to request or find the GPIO descriptor |
| 3 | `ERR_READ_FAILED` | Sensor data read failed (checksum error, timeout, all retries exhausted) |
| 4 | `ERR_AUTO_MODE` | Manual measurement attempted while auto-poll is active |
| 5 | `ERR_TOO_SOON` | Manual measurement rejected due to rate limiting (min 2 s between reads) |

### Errno from `export` and `unexport`

| errno | Meaning |
|-------|---------|
| `-EINVAL` | Invalid pin number (not 0–27) |
| `-EBUSY` | Pin already registered (export only) |
| `-ENOMEM` | Maximum sensor count reached (32) or memory allocation failure |
| `-ENODEV` | GPIO descriptor not found, or pin not registered (unexport) |
| `-EIO` | Initial measurement failed — sensor not registered |
| `-EFAULT` | Copy from user space failed |

---

## Configuration Parameters

### Module Parameter

| Parameter | Type | Default | Access | Description |
|-----------|------|---------|--------|-------------|
| `dht_debug` | int | 0 | 0644 | Debug logging (0 = off, 1 = on). Can be set via `insmod dht.ko dht_debug=1` or `/proc/sensors/dht/debug`. |

### Compile-Time Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_SENSORS` | 32 | Maximum simultaneously registered sensors |
| `MAX_PIN_NUM` | 27 | Highest valid BCM GPIO pin number |
| `MIN_INTERVAL` | 2 | Minimum auto-poll interval in seconds |
| `MAX_INTERVAL` | 60 | Maximum auto-poll interval in seconds |
| `MEAS_MIN_GAP` | 2 | Minimum seconds between measurements (rate limiting) |
| `MAX_RETRIES` | 3 | Read attempts before giving up |
| `RETRY_DELAY_MS` | 100 | Delay between read retries in milliseconds |
| `BIT_THRESHOLD` | 40000 ns | Pulse width threshold: 0 bit (~26 µs) vs 1 bit (~70 µs) |
| `PULSE_TIMEOUT_NS` | 200000 ns | Maximum wait for a single pulse transition (200 µs) |
| `MAX_TIMINGS` | 100 | Maximum pulse transitions to capture per read cycle |
| `CONFIG_PATH` | `/etc/default/dht` | Path to the optional configuration file |
| `CONFIG_BUF_LEN` | 4096 | Maximum configuration file size |
| `CONFIG_LINE_LEN` | 256 | Maximum length of a single config line |

---

## Debug Mode

### Enabling Debug

```bash
# At load time
sudo insmod dht.ko dht_debug=1

# Or via procfs at runtime
echo 1 | sudo tee /proc/sensors/dht/debug

# Or via the config file (put in /etc/default/dht before loading)
DEBUG=1
```

### Debug Output in dmesg

When debug is enabled, the driver produces additional log messages prefixed with `[DHT]:` (global) or `[dht_gpio_<pin>]:` (per-sensor):

```
# Sensor registration
[dht_gpio_4]: found on 'pinctrl-bcm2711' (base=0, global=4)
[dht_gpio_4]: registered successfully

# Read attempts (debug)
[dht_gpio_4]: read attempt 1 failed - j=38, data=[2,56,0,231,53]
[dht_gpio_4]: read attempt 2 success

# Poll thread
[dht_gpio_4]: poll thread started
[dht_gpio_4]: poll thread stopped

# Config file
[DHT]: reading config from /etc/default/dht (68 bytes)
[DHT]: config: DEBUG=1
```

### Disabling Debug

```bash
echo 0 | sudo tee /proc/sensors/dht/debug
```

---

## Architecture

### GPIO Chip Detection (3 Stages)

Raspberry Pi models use different GPIO controllers with different base offsets. The driver resolves BCM pin numbers to GPIO descriptors using a three-stage strategy:

1. **Fast path (cached):** After the first successful lookup, the GPIO chip's base offset is cached. Subsequent registrations compute `global = base + bcm_pin` directly — a single `gpio_to_desc()` call.

2. **Direct lookup:** If no cache exists, try `gpio_to_desc(bcm_pin)` directly. Works on Pi 3/4 where GPIO base = 0 and BCM numbers equal global GPIO numbers.

3. **Full scan:** If direct lookup fails (Pi 5 with large base offset), iterate over all GPIO numbers 0–2048, find the Pi GPIO chip whose local offset matches the requested BCM pin, and cache the base.

### Rate Limiting

All measurements (manual and auto-poll) are rate-limited to a minimum of 2 seconds between attempts (`MEAS_MIN_GAP`):

- **Manual measurements** that arrive too soon return `ERR_TOO_SOON` to the user.
- **Auto-poll measurements** that arrive too soon are silently skipped (last data preserved).

This protects DHT sensors from being read too frequently, which causes unreliable readings.

### Safe Module Unload (2 Phases)

The driver uses a two-phase teardown to prevent race conditions during `rmmod`:

**Phase 1 — Block new access and remove procfs:**
1. Set `dht_exiting` atomic flag — new `dht_proc_open()` calls return `-ENODEV`.
2. Disable global auto-poll — prevents new thread launches.
3. Remove all procfs entries via `proc_remove()` — blocks until all open files are closed (module reference count drops to zero).

**Phase 2 — Stop threads and free memory:**
4. Splice the sensor list under lock — no new sensors can appear (procfs gone).
5. For each sensor: stop the poll thread (`kthread_stop`), destroy mutex, free memory.

The key insight: after Phase 1, no new file operations can reach the sensors. By the time Phase 2 runs, it is safe to free sensor memory.

### Shared `/proc/sensors`

The driver creates `/proc/sensors` if it does not exist. If another sensor driver already created it, the driver reuses the existing directory. On unload:

- The driver checks whether `/proc/sensors` is empty (using VFS `iterate_dir`).
- If empty and the driver created it, the directory is removed.
- If other drivers have subdirectories there, the directory is left in place.

This allows multiple sensor drivers to coexist under `/proc/sensors/`.

### DHT Protocol

The driver implements the DHT single-wire communication protocol:

1. **Start signal:** Pull the data line low for 20 ms, then release (switch to input).
2. **Sensor response:** The sensor pulls the line low for ~80 µs, then high for ~80 µs.
3. **Data transmission:** 40 bits (5 bytes) are transmitted. Each bit consists of a ~50 µs low pulse followed by a high pulse:
   - ~26 µs high = `0`
   - ~70 µs high = `1`
   - The `BIT_THRESHOLD` (40 µs / 40000 ns) distinguishes between them.
4. **Checksum:** The 5th byte is the sum of the first 4 bytes (mod 256).
5. **Sensor type detection:** Based on data format — DHT11 uses integer-only bytes, DHT22 uses 16-bit scaled values.

### Poll Thread

Each sensor with active auto-polling runs its own kernel thread (`kthread`):

- The thread calls `dht_do_measurement()` at the effective interval.
- The effective interval is `global_auto_interval` if set (takes priority), otherwise `sensor->interval`.
- The thread sleeps in 1-second increments to respond quickly to `kthread_should_stop()`.
- On stop (sensor unregistration, interval change to `-1`, or module unload), the thread exits promptly.

### Configuration File Loading

At the end of `dht_driver_init()` — after the procfs hierarchy is fully set up — the driver calls `dht_load_config()`:

1. Opens `/etc/default/dht` via `filp_open()` (kernel-space file I/O).
2. Reads up to 4096 bytes into a `kmalloc`-allocated buffer.
3. Parses line by line, trimming whitespace and skipping comments.
4. Each non-comment line is passed to `dht_parse_config_line()`.
5. `SENSOR` entries call `dht_do_register()` — the same function used by `export_write()`.
6. `AUTO_INTERVAL` entries call `dht_config_set_auto_interval()` — the same logic as `auto_interval_write()`.

This ensures that config-file-registered sensors get the same treatment as runtime-registered ones, including procfs entries, initial measurement, and auto-poll start.

---

## Version History

### v2.7

- **Configuration file support:** Optional `/etc/default/dht` for auto-registration and settings at module load time.
  - Global options: `DEBUG`, `AUTO_INTERVAL`
  - Sensor registration: `SENSOR=<pin>` and `SENSOR=<pin>,<interval>`
  - Comments (`#`) and empty lines ignored
  - Unknown options and invalid values produce warnings in dmesg
- **`dht_do_register()` function:** Sensor registration logic extracted from `export_write()` into a reusable function callable from both procfs and config file paths.
- **`dht_load_config()`:** Reads and parses the config file using kernel VFS API (`filp_open` + `kernel_read`). Buffer allocated via `kmalloc` to avoid stack overflow.
- **`dht_parse_config_line()`:** Parses a single config line, dispatches to `DEBUG`/`AUTO_INTERVAL`/`SENSOR` handlers. Uses a 256-byte stack buffer (not the 4096-byte file buffer).

### v2.6

- **Shared `/proc/sensors`:** The driver now coexists with other sensor drivers under `/proc/sensors/`. Creates the directory if missing, reuses if present, removes on unload only if empty.
- **Safe module unload:** Two-phase teardown with `dht_exiting` atomic flag and module reference counting via `try_module_get()`/`module_put()` on every procfs open/release.
- **Rate limiting:** All measurements (manual and auto-poll) are rate-limited to minimum 2 s between reads.
- **GPIO chip detection:** Three-stage lookup with caching for fast multi-sensor registration on Pi 3/4/5.

### v2.5

- Initial release.
- Dynamic sensor export/unexport via procfs.
- Per-sensor proc entries: pin, interval, measure, status_code, status_text, value, info, timestamp.
- Background polling threads with per-sensor configurable intervals.
- Global auto-poll mode with shared interval.
- Nanosecond-precision pulse timing for reliable reads.
- Automatic sensor type detection (DHT11 vs DHT22/AM2302).
- Kernel API compatibility macros for `proc_ops` (5.6+) and `pde_data()` (5.17+).

---

## Known Limitations

1. **Maximum 32 sensors** can be registered simultaneously (`MAX_SENSORS`).
2. **Minimum 2-second interval** between measurements (`MEAS_MIN_GAP`) — DHT sensors require this for reliable operation.
3. **Single-wire protocol timing** depends on CPU scheduling — very high system load may cause occasional read failures. The driver retries up to 3 times.
4. **No interrupt-driven mode** — the driver uses busy-wait polling for pulse timing. This is inherent to the DHT protocol and the GPIO subsystem API.
5. **Configuration file size limit** — 4096 bytes (`CONFIG_BUF_LEN`). Larger files are truncated.
6. **Config file read at load time only** — changes to `/etc/default/dht` after the module is loaded are not picked up. Reload the module to apply changes.

---

## Troubleshooting

### Sensor registration fails with `-EIO`

The initial measurement failed. Check:

- **Wiring:** Data pin connected to the correct GPIO, VCC to 3.3V, GND to ground.
- **Pull-up resistor:** 4.7 kΩ–10 kΩ between DATA and VCC. Many breakout boards include one.
- **Pin conflicts:** Ensure the GPIO pin is not used by another driver or overlay.
- **dmesg output:** Enable debug (`echo 1 | sudo tee /proc/sensors/dht/debug`) and check for detailed error messages.

### `cat value` shows old data

- If auto-poll is not enabled, the value is from the initial measurement at registration time. Write `1` to `measure` to trigger a fresh read: `echo 1 | sudo tee /proc/sensors/dht/gpio4/measure`.
- If auto-poll is enabled, check that the poll thread is running: `dht_dbg` output in dmesg should show `poll thread started`.

### `rmmod` hangs or fails

- A process has a procfs file open. The module reference count prevents unloading while files are open.
- Find and close the process: `lsof | grep /proc/sensors/dht` or `fuser /proc/sensors/dht/gpio4/value`.
- The `dht_exiting` flag ensures no new opens succeed, but existing opens must be closed.

### `echo 4 > export` returns `-EBUSY`

- The pin is already registered. Check: `ls /proc/sensors/dht/`.
- Unregister first: `echo 4 | sudo tee /proc/sensors/dht/unexport`.

### Read failures on Pi 5

- Pi 5 uses a different GPIO controller (`pinctrl-rp1`) with a large base offset (512+). The driver handles this automatically via the three-stage GPIO chip detection.
- If reads consistently fail, enable debug and check the dmesg output for `found on 'pinctrl-rp1' (base=512, ...)` to verify chip detection.

### Config file not read

- Check the file path: it must be exactly `/etc/default/dht`.
- Check file permissions: the kernel module reads it as root, so permissions should not be an issue.
- Enable debug and look for `no config file at /etc/default/dht` (file missing) or `reading config from /etc/default/dht` (file found).

---

## Files in This Repository

| File | Description |
|------|-------------|
| `dht.c` | Driver source code |
| `Makefile` | Makefile for building the kernel module |
| `changelog.txt` | Change log (English) |
| `changelog_ru.txt` | Change log (Russian) |
| `README.md` | Documentation (English) |
| `README_RU.md` | Documentation (Russian) |

---

## Build Requirements

### Prerequisites

- Raspberry Pi running a Linux kernel 5.10 or later
- Kernel headers installed: `sudo apt install linux-headers-$(uname -r) build-essential`
- GCC compiler (provided by `build-essential`)

### Tested Configurations

| Pi Model | Kernel | Architecture | Status |
|----------|--------|--------------|--------|
| Pi 5 | 6.6, 6.18 | ARM64 (aarch64) | Tested |
| Pi 4 B | 6.1, 6.6 | ARM64 (aarch64) | Tested |
| Pi 3 B+ | 5.15, 6.1 | ARM64 (aarch64) | Tested |
| Pi Zero 2 W | 5.15, 6.1 | ARM64 (aarch64) | Tested |
| Pi Zero W | 5.10, 5.15 | ARM32 (armhf) | Tested |
| Pi 3 B+ | 5.10 | ARM32 (armhf) | Tested |

### Building

```bash
make
```

### Installing

```bash
sudo make install   # Copies dht.ko to /lib/modules/$(uname -r)/extra/
sudo depmod -a
sudo modprobe dht   # Or: sudo insmod dht.ko
```

### Cleaning

```bash
make clean
```

---

## License

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 3 as published by the Free Software Foundation.

```
Copyright (c) 2026, Chapvic

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <https://www.gnu.org/licenses/>.
```

---

## Acknowledgements

- **Author:** Chapvic
- **Test hardware:** Raspberry Pi 3B+, Pi 4B, Pi 5
- **Test sensors:** DHT11, DHT22, AM2302 (probe type)
- **Inspiration:** The DHT protocol implementation is based on the single-wire bus specification from Aosong (Guangzhou) Electronics Co., Ltd.
