# DHT11/DHT22/AM2302 Kernel Module

A Linux kernel module for reading temperature and humidity data from
DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins.
The driver creates a procfs interface under `/proc/sensors/dht/` for
managing sensor registration, configuration, and data retrieval.

**Version:** 2.8  
**Author:** Chapvic  
**License:** GPL v3

---

## Table of Contents

1. [Overview](#overview)
2. [Supported Hardware](#supported-hardware)
3. [Wiring](#wiring)
4. [Quick Start](#quick-start)
5. [Configuration File](#configuration-file)
6. [Procfs Interface](#procfs-interface)
7. [Usage Examples](#usage-examples)
8. [Error Codes](#error-codes)
9. [Configuration Parameters](#configuration-parameters)
10. [Debug Mode](#debug-mode)
11. [Architecture](#architecture)
12. [Version History](#version-history)
13. [Known Limitations](#known-limitations)
14. [Troubleshooting](#troubleshooting)
15. [Files in This Repository](#files-in-this-repository)
16. [Build Requirements](#build-requirements)
17. [License](#license)

---

## Overview

The DHT kernel module provides a procfs-based interface for reading
temperature and humidity from DHT-series sensors connected to Raspberry Pi
GPIO pins. Key features:

- Dynamic sensor export/unexport via procfs
- Per-sensor proc entries for temperature, humidity, status, and configuration
- Background polling thread with configurable interval
- Global auto-poll mode with shared interval
- GPIO chip base caching for fast multi-sensor registration on Pi 3/4/5
- Nanosecond-precision pulse timing for reliable reads across all Pi models
- Rate limiting for all measurements (manual and auto-poll)
- Safe module unload with module reference counting and kref-based lifetime
- Preemption protection during timing-critical bit-bang reads
- Shared `/proc/sensors`: coexists with other sensor drivers
- Configuration file: optional `/etc/default/dht` for auto-registration at load

---

## Supported Hardware

### Sensors

| Sensor | Temperature Range | Humidity Range | Accuracy | Data Format |
|--------|-------------------|----------------|----------|-------------|
| DHT11  | 0-50 C            | 20-90%         | ±2 C / ±5% | Integer only |
| DHT22  | -40 to 80 C       | 0-100%         | ±0.5 C / ±2% | 16-bit scaled |
| AM2302 | -40 to 80 C       | 0-100%         | ±0.5 C / ±2% | 16-bit scaled |

AM2302 is the wired/probe version of DHT22 with the same protocol.

### Raspberry Pi Models

| Model | GPIO Chip | Chip Label | Base Offset |
|-------|-----------|------------|-------------|
| Pi 5       | RP1           | `pinctrl-rp1`   | 512+ |
| Pi 4       | BCM2711       | `pinctrl-bcm2711` | 0 |
| Pi 3/Zero 2 | BCM2835     | `pinctrl-bcm2835` | 0 |

### Kernel Compatibility

- **Minimum:** 5.10
- **Tested:** up to 6.18 (Raspberry Pi OS kernel `6.18.50+rpt-rpi-v8`)
- **Architecture:** ARM64 (aarch64)

---

## Wiring

Connect the sensor to any available BCM GPIO pin:

```
DHT Sensor       Raspberry Pi
─────────        ────────────
VCC (pin 1)  ->  3.3V  (pin 1 or 17)
DATA (pin 2) ->  BCM GPIO pin of your choice
GND (pin 4)  ->  GND   (pin 6, 9, 14, 20, 25, ...)
```

**Pull-up resistor:** A 4.7kΩ–10kΩ pull-up resistor between DATA and VCC
is required. Some sensor breakout boards (e.g., AM2302) include this
resistor on the board.

**Pin selection:** Any BCM pin 0-27 can be used. Multiple sensors can be
connected to different pins simultaneously (up to 32 sensors).

**Note:** GPIO pins behind I2C/SPI expanders are not supported — the
driver checks `gpiod_cansleep()` and rejects such pins with an error.

---

## Quick Start

```bash
# 1. Install kernel headers
sudo apt install linux-headers-$(uname -r)

# 2. Build the module
make

# 3. Load the module
sudo insmod dht.ko

# 4. Register a sensor on BCM pin 4
echo 4 | sudo tee /proc/sensors/dht/export

# 5. Read temperature and humidity
cat /proc/sensors/dht/gpio4/value

# 6. Enable auto-polling (every 10 seconds)
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# 7. Read the latest cached values
cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio4/timestamp

# 8. Unregister the sensor
echo 4 | sudo tee /proc/sensors/dht/unexport

# 9. Unload the module
sudo rmmod dht
```

---

## Configuration File

The driver reads an optional configuration file at `/etc/default/dht`
during module load. If the file is missing, the driver loads with defaults.

### Format

- Lines starting with `#` and empty lines are ignored
- Unknown options produce warnings in dmesg
- Invalid values produce warnings in dmesg
- Options are case-sensitive

### Options

| Option | Format | Description |
|--------|--------|-------------|
| `DEBUG` | `DEBUG` or `DEBUG=0\|1` | Enable/disable debug logging at load |
| `AUTO_INTERVAL` | `AUTO_INTERVAL=n` | Global auto-poll interval (2-60 seconds) |
| `SENSOR` | `SENSOR=<pin>` | Register a sensor (no auto-poll) |
| `SENSOR` | `SENSOR=<pin>,<n>` | Register with per-sensor auto-poll interval (2-60) |

### Example

```bash
# /etc/default/dht
DEBUG=1
AUTO_INTERVAL=10
SENSOR=4              # DHT22 on GPIO4, uses global interval
SENSOR=17,5           # DHT11 on GPIO17, polls every 5 seconds
SENSOR=22             # Sensor on GPIO22, no auto-poll
```

### Error Handling

| Situation | Behavior |
|-----------|----------|
| File missing | Silent, debug message in dmesg |
| Unknown option | `config: unknown option 'XXX'` in dmesg |
| Invalid pin | `config: SENSOR pin 'XX' invalid (0-27)` in dmesg |
| Invalid interval | `config: SENSOR interval 'XX' invalid (2-60 or -1)` in dmesg |
| Invalid DEBUG value | `config: DEBUG='XX' invalid (expected 0 or 1)` in dmesg |
| Invalid AUTO_INTERVAL | `config: AUTO_INTERVAL='XX' is not a valid integer` or `out of range` in dmesg |

### Processing Order

Options are processed top-to-bottom. This matters for `AUTO_INTERVAL`
and `SENSOR` interaction:

- If `AUTO_INTERVAL=10` appears **before** `SENSOR=4`, the sensor
  automatically gets global auto-polling at registration.
- If `AUTO_INTERVAL=10` appears **after** `SENSOR=4`, the sensor
  registers first without polling, then `AUTO_INTERVAL` starts polling
  for all registered sensors.

---

## Procfs Interface

### Global entries: `/proc/sensors/dht/`

```
/proc/sensors/dht/
  debug         (rw) - debug logging: 0 = off (default), 1 = on
  version       (r)  - driver version
  export        (w)  - write BCM pin number to register a new sensor
  unexport      (w)  - write BCM pin number to unregister a sensor
  auto_interval (rw) - global auto-poll interval (2-60, -1 = off)
```

### Per-sensor entries: `/proc/sensors/dht/gpio<pin>/`

```
/proc/sensors/dht/gpio4/
  pin           (r)  - BCM GPIO pin number
  interval      (rw) - auto-poll interval in seconds (2-60, -1 = off)
  measure       (w)  - write "1" to trigger measurement
  status_code   (r)  - error code (0 = success)
  status_text   (r)  - error description
  value         (r)  - "H=<humidity>\nT=<temperature>\n"
  info          (r)  - sensor type + registration time
  timestamp     (r)  - Unix timestamp of last measurement
```

### Polling Priority

| Priority | Source | Condition |
|----------|--------|-----------|
| 1 (highest) | Global `auto_interval` | Set to 2-60 |
| 2 | Per-sensor `interval` | Set to 2-60, global is -1 |
| 3 (none) | Manual `measure` | Both intervals are -1 |

---

## Usage Examples

### Manual mode (no auto-poll)

```bash
echo 4 | sudo tee /proc/sensors/dht/export
echo 1 | sudo tee /proc/sensors/dht/gpio4/measure
cat /proc/sensors/dht/gpio4/value
# Output: H=45.2
#         T=23.1
```

### Per-sensor auto-poll

```bash
echo 4 | sudo tee /proc/sensors/dht/export
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval
# Now the sensor is polled every 5 seconds
cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio4/timestamp
```

### Global auto-poll

```bash
echo 4 | sudo tee /proc/sensors/dht/export
echo 17 | sudo tee /proc/sensors/dht/export
echo 10 | sudo tee /proc/sensors/dht/auto_interval
# Both sensors are now polled every 10 seconds
```

### Shell script

```bash
#!/bin/bash
# Read all registered sensors
for dir in /proc/sensors/dht/gpio*/; do
    pin=$(cat "${dir}pin")
    value=$(cat "${dir}value")
    echo "GPIO${pin}: ${value}"
done
```

### Python

```python
import time

def read_sensor(pin):
    with open(f"/proc/sensors/dht/gpio{pin}/value") as f:
        data = f.read()
    h, t = None, None
    for line in data.strip().split("\n"):
        if line.startswith("H="):
            h = float(line[2:])
        elif line.startswith("T="):
            t = float(line[2:])
    return h, t

# Register and poll
with open("/proc/sensors/dht/export", "w") as f:
    f.write("4")

with open("/proc/sensors/dht/gpio4/interval", "w") as f:
    f.write("5")

while True:
    h, t = read_sensor(4)
    print(f"Humidity: {h}%  Temperature: {t}C")
    time.sleep(5)
```

### Configuration file

```bash
sudo tee /etc/default/dht << 'EOF'
DEBUG=1
AUTO_INTERVAL=10
SENSOR=4
SENSOR=17,5
EOF

sudo rmmod dht && sudo insmod dht.ko
# Sensors are auto-registered at load time
```

### Safe unload

```bash
# Unregister all sensors first (optional — module unload handles cleanup)
echo 4  | sudo tee /proc/sensors/dht/unexport
echo 17 | sudo tee /proc/sensors/dht/unexport
sudo rmmod dht
```

---

## Error Codes

### Driver error codes (in `status_code`)

| Code | Constant | Description |
|------|----------|-------------|
| 0 | `ERR_SUCCESS` | Operation completed successfully |
| 1 | `ERR_PIN_INVALID` | GPIO pin number out of range (0-27) |
| 2 | `ERR_GPIO_REQUEST` | GPIO request/lookup failed, or GPIO on sleeping chip |
| 3 | `ERR_READ_FAILED` | Sensor data read failed (checksum, timeout, all retries) |
| 4 | `ERR_AUTO_MODE` | Manual measurement while auto-poll is active |
| 5 | `ERR_TOO_SOON` | Rate-limited: less than 2 s since last measurement |

### errno from export/unexport

| errno | Meaning |
|-------|---------|
| `-EINVAL` | Invalid pin number |
| `-EBUSY` | Pin already registered (or TOCTOU race) |
| `-ENOMEM` | Max sensors reached or allocation failure |
| `-ENODEV` | GPIO descriptor not found |
| `-EIO` | Initial measurement failed |

---

## Configuration Parameters

### Module parameter

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `dht_debug` | int | 0 | Debug logging (0 = off, 1 = on) |

Set at load time: `insmod dht.ko dht_debug=1`  
Or at runtime: `echo 1 > /proc/sensors/dht/debug`

### Compile-time constants

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_SENSORS` | 32 | Maximum simultaneously registered sensors |
| `MAX_PIN_NUM` | 27 | Highest valid BCM GPIO pin |
| `MIN_INTERVAL` | 2 | Minimum auto-poll interval (seconds) |
| `MAX_INTERVAL` | 60 | Maximum auto-poll interval (seconds) |
| `MEAS_MIN_GAP` | 2 | Minimum seconds between measurements |
| `MAX_RETRIES` | 3 | Read attempts before giving up |
| `BIT_THRESHOLD` | 40000 | Nanosecond 0/1 pulse threshold |
| `PULSE_TIMEOUT_NS` | 200000 | Pulse timeout (200 us) |
| `CONFIG_PATH` | `/etc/default/dht` | Configuration file path |
| `CONFIG_BUF_LEN` | 4096 | Max config file size |
| `CONFIG_LINE_LEN` | 256 | Max single config line length |

---

## Debug Mode

Enable debug logging in three ways:

```bash
# 1. At module load
sudo insmod dht.ko dht_debug=1

# 2. Via procfs
echo 1 | sudo tee /proc/sensors/dht/debug

# 3. Via configuration file
echo "DEBUG=1" | sudo tee -a /etc/default/dht
```

Debug output example in dmesg:

```
[DHT]: DHT Driver (c) 2026, Chapvic (v2.8)
[DHT]: driver loaded - /proc/sensors/dht/ (max 32 sensors)
[DHT]: reading config from /etc/default/dht (82 bytes)
[DHT]: config: DEBUG=1
[DHT]: config: AUTO_INTERVAL=10
[dht_gpio_4]: found on 'pinctrl-rp1' (base=512, global=516)
[dht_gpio_4]: registered successfully
[dht_gpio_4]: measurement OK - H=45.2% T=23.1 C
```

---

## Architecture

### Reference Counting (kref)

Each sensor is protected by a `struct kref` reference count. This prevents
use-after-free when a sensor is unregistered while a procfs file is still
open:

- `dht_proc_open` acquires a reference (`kref_get_unless_zero`)
- `dht_proc_release` drops the reference (`kref_put`)
- `dht_sensor_release` (kref callback) frees all resources when the last
  reference is dropped: stop poll thread, remove procfs entries, destroy
  mutex, free memory
- `kref_init` is called after `kzalloc` in `dht_do_register`, starting with
  refcount = 1 (owned by the sensor list)

### Preemption Protection

The 40-bit bit-bang read loop in `dht_read_sensor` is wrapped with
`preempt_disable()` / `preempt_enable()` to prevent timing corruption from
context switches. Interrupts are NOT disabled — the DHT protocol's ~70 us
pulses are wide enough that occasional IRQ jitter is tolerable.

### Sensor Type Detection

The sensor type (DHT11 vs DHT22) is determined from the humidity data
format on the first successful measurement:

- If combined 16-bit humidity > 1000: DHT11 (integer format)
- Otherwise: DHT22/AM2302 (16-bit scaled format)

Once determined, the type is locked permanently (`sensor_type` is only
written when `SENSOR_TYPE_UNKNOWN`). This prevents spurious type flips
from borderline readings.

### GPIO Chip Detection

The driver uses a three-stage lookup to resolve BCM pin numbers to GPIO
descriptors:

1. **Fast path:** If `cached_chip_base` is known, compute
   `global = base + bcm_pin` (common case after first registration)
2. **Direct lookup:** Try `gpio_to_desc(bcm_pin)` — works on Pi 3/4
   where base = 0
3. **Full scan:** Iterate over GPIO numbers 0-2048 looking for a Pi GPIO
   chip where the local offset matches the BCM pin

On first success, the chip base is cached for subsequent registrations.

### Rate Limiting

All measurements (manual and auto-poll) are rate-limited to at least
`MEAS_MIN_GAP` (2) seconds between attempts:

- Manual measurements that violate the gap return `ERR_TOO_SOON`
- Auto-poll measurements that violate the gap are silently skipped
  (last cached data is preserved)

### Safe Module Unload

The driver uses a two-phase teardown:

**Phase 1 — Block new access and remove procfs:**
1. Set `dht_exiting` flag (new `dht_proc_open` calls return `-ENODEV`)
2. Disable global auto-poll (prevents new thread launches)
3. Remove all procfs entries (`proc_remove` blocks until open files close)

**Phase 2 — Stop threads and free memory:**
4. Splice sensor list under lock (no new sensors possible — procfs gone)
5. For each sensor: drop reference via `dht_sensor_put`

The module reference count (incremented by `dht_proc_open`) prevents
`rmmod` from proceeding until all open procfs files are closed.

### Sleeping Chip Detection

`gpiod_cansleep()` is checked before each read. If the GPIO is behind a
sleeping expander (e.g., I2C GPIO chip), the read immediately returns
`ERR_GPIO_REQUEST` with a diagnostic message, instead of attempting
the timing-critical bit-bang loop which would produce corrupted data.

### Configuration File Loading

At module load, after procfs is set up, `dht_load_config()` opens
`/etc/default/dht` via the VFS layer (`filp_open` + `kernel_read`). The
buffer is allocated with `kmalloc` (not on the stack) to avoid stack
overflow warnings. Lines are parsed one at a time by
`dht_parse_config_line()`.

---

## Version History

| Version | Date | Key Changes |
|---------|------|-------------|
| 2.8 | 21.09.2026 | kref reference counting, TOCTOU fix, poll thread exit fix, DHT11 temp < 5 C fix, sensor type locking, preempt protection, gpiod_cansleep check, dead code removal |
| 2.7 | 21.09.2026 | Configuration file `/etc/default/dht` support |
| 2.6 | 20.09.2026 | Shared `/proc/sensors`, safe unload, rate limiting, GPIO chip detection |
| 2.5 | 18.09.2026 | Initial release |

See `changelog.txt` for full details.

---

## Known Limitations

1. **Deprecated GPIO API:** Uses `gpiod_to_chip()` / `gpio_to_desc()`,
   which are deprecated. Migration to `gpiod_get()` via device tree
   requires a full rework of the registration model.
2. **No I2C/SPI GPIO expanders:** The bit-bang timing loop requires
   non-sleeping GPIO access. GPIOs behind I2C/SPI expanders are rejected.
3. **Minimum 2 s between reads:** DHT sensors need at least 2 seconds
   between reads. This is enforced by rate limiting.
4. **Sensor type auto-detection:** The type is determined from the first
   successful measurement and locked permanently. If the wrong sensor
   is connected initially, the type will be wrong until the sensor is
   unregistered and re-registered.
5. **Single-wire protocol timing:** Reading is timing-critical (~4 ms).
   Although preemption is disabled during the read, interrupt jitter
   can occasionally cause read failures (handled by retries).
6. **Max 32 sensors:** Hard limit defined by `MAX_SENSORS`.

---

## Troubleshooting

| Problem | Possible Cause | Solution |
|---------|---------------|----------|
| `GPIO descriptor not found` | Invalid pin or GPIO chip not detected | Check pin number (0-27); check dmesg for chip detection |
| `Sensor data read failed` | Wiring, pull-up resistor, or sensor issue | Verify wiring; ensure 4.7k-10k pull-up; try different pin |
| `GPIO is on a sleeping chip` | GPIO behind I2C/SPI expander | Use native Pi GPIO pins instead |
| `Too soon since last measurement` | Rate limiting (min 2 s) | Wait 2 seconds between manual reads |
| `rmmod: Module dht is in use` | Procfs file still open | Close all files under `/proc/sensors/dht/` |
| Temperature stuck at 0 for DHT11 | DHT11 reads below 5 C (fixed in v2.8) | Update to v2.8 |
| Sensor type keeps changing | Type fluctuation (fixed in v2.8) | Update to v2.8 |
| Compiler warning: frame size | Stack buffer too large (fixed in v2.7) | Update to v2.7+ |

---

## Files in This Repository

| File | Description |
|------|-------------|
| `dht.c` | Driver source code |
| `Makefile` | Build system for the kernel module |
| `changelog.txt` | Changelog (English) |
| `changelog_ru.txt` | Changelog (Russian) |
| `README.md` | Documentation (English) |
| `README_RU.md` | Documentation (Russian) |

---

## Build Requirements

- Linux kernel headers (matching the running kernel)
- GCC cross-compiler for ARM64 (or native compiler on Raspberry Pi)
- Tested combinations:
  - Raspberry Pi 5 + kernel 6.18.50+rpt-rpi-v8
  - Raspberry Pi 4 + kernel 6.1.x
  - Raspberry Pi 3 + kernel 5.15.x

The module is backward-compatible with the procfs interface from v2.5
and v2.6. Existing scripts that read `/proc/sensors/dht/gpio*/value` will
continue to work without changes.

---

## License

Copyright (c) 2026, Chapvic

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 3 as
published by the Free Software Foundation.

---

## Acknowledgements

- **Author:** Chapvic
- **Test hardware:** Raspberry Pi 5 (8 GB), Raspberry Pi 4 (4 GB),
  Raspberry Pi 3 B+
- **Test sensors:** DHT11, DHT22, AM2302 (various breakout boards)
