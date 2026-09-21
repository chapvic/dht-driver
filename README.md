# DHT11/DHT22/AM2303 Temperature and Humidity Sensor Driver

**Version:** 2.8.4  
**Author:** Chapvic  
**License:** GPL v3  
**Compatibility:** Linux kernel 5.10+ (tested on Raspberry Pi OS 64-bit)

## Overview

A Linux kernel module for reading temperature and humidity data from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins. The driver creates a procfs interface under `/proc/sensors/dht/` for managing sensor registration, configuration, and data retrieval.

## Features

- Dynamic sensor export/unexport via procfs
- Per-sensor proc entries for temperature, humidity, status, and configuration
- Background polling thread with configurable interval
- Global auto-poll mode with shared interval
- GPIO chip base caching for fast multi-sensor registration on Pi 3/4/5
- Nanosecond-precision pulse timing for reliable reads across all Pi models
- Rate limiting for all measurements (manual and auto-poll)
- Safe module unload with module reference counting
- Shared `/proc/sensors`: coexists with other sensor drivers
- Configuration file: optional `/etc/default/dht` for auto-registration at load

## Procfs Interface

### Global entries: `/proc/sensors/dht/`

| Entry | Mode | Description |
|-------|------|-------------|
| `debug` | rw | Debug logging: 0 = off (default), 1 = on |
| `version` | r | Driver version |
| `export` | w | Write BCM pin number to register a new sensor |
| `unexport` | w | Write BCM pin number to unregister a sensor |
| `auto_interval` | rw | Global auto-poll interval (2-60, -1 = off) |

### Per-sensor entries: `/proc/sensors/dht/gpio<pin>/`

| Entry | Mode | Description |
|-------|------|-------------|
| `pin` | r | BCM GPIO pin number |
| `interval` | rw | Auto-poll interval in seconds (2-60, -1 = off) |
| `measure` | w | Write "1" to trigger measurement (ignored if auto active) |
| `status_code` | r | Error code (0 = success) |
| `status_text` | r | Error description |
| `value` | r | "H=\<humidity\>\nT=\<temperature\>\n" |
| `info` | r | Sensor type + registration time |
| `timestamp` | r | Unix timestamp of last measurement |

## Quick Start

### Build and install

```bash
make
sudo make install
sudo modprobe dht
```

### Register a sensor

```bash
echo 23 | sudo tee /proc/sensors/dht/export
```

### Read values

```bash
cat /proc/sensors/dht/gpio23/value
# Output: H=54.6
#         T=24.3
```

### Enable auto-polling

```bash
echo 10 | sudo tee /proc/sensors/dht/auto_interval
```

### Unregister a sensor

```bash
echo 23 | sudo tee /proc/sensors/dht/unexport
```

## Configuration File

Optional: `/etc/default/dht` — read at module load time.

```bash
# /etc/default/dht
DEBUG=1
AUTO_INTERVAL=10
SENSOR=4            # DHT22 on GPIO4, uses global interval
SENSOR=17,5         # DHT11 on GPIO17, polls every 5 seconds
SENSOR=22           # Sensor on GPIO22, no auto-poll
```

| Option | Description |
|--------|-------------|
| `DEBUG` | Enable debug logging (same as `DEBUG=1`) |
| `DEBUG=0\|1` | Explicitly disable/enable debug logging |
| `AUTO_INTERVAL=n` | Global auto-poll interval in seconds (2-60) |
| `SENSOR=<pin>` | Register a sensor on the given BCM pin |
| `SENSOR=<pin>,<n>` | Register with per-sensor auto-poll interval |

## Supported Hardware

| Sensor | Temperature | Humidity | Notes |
|--------|-------------|----------|-------|
| DHT11 | 0-50 C, +/-2 C | 20-90%, +/-5% | Integer values, lower resolution |
| DHT22/AM2302 | -40-80 C, +/-0.5 C | 0-100%, +/-2% | Decimal values, higher resolution |

Tested on: Raspberry Pi 3, 4, 5 (all use different GPIO controllers).

## Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | ERR_SUCCESS | Operation completed successfully |
| 1 | ERR_PIN_INVALID | GPIO pin out of valid range (0-27) |
| 2 | ERR_GPIO_REQUEST | Failed to request or find GPIO descriptor |
| 3 | ERR_READ_FAILED | Sensor data read failed (checksum, timeout) |
| 4 | ERR_AUTO_MODE | Manual measurement while auto-poll is active |
| 5 | ERR_TOO_SOON | Rate limit: min 2s between measurements |

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| `status_code=3` | Sensor read failed | Check wiring, pull-up resistor (4.7k-10k) |
| `status_code=4` | Manual measure in auto mode | Disable auto_interval or per-sensor interval |
| `status_code=5` | Measurement too soon | Wait at least 2 seconds between manual reads |
| Negative temperature reads as positive (DHT22) | Fixed in v2.8.3 | Update to latest version |
| Every other poll skipped | Inverted measuring flag, fixed in v2.8.4 | Update to latest version |

## Files

| File | Description |
|------|-------------|
| `Makefile` | Build system |
| `dht.c` | Driver source code |
| `changelog.txt` | Change log (English) |
| `changelog_ru.txt` | Change log (Russian) |
| `README.md` | This file (English) |
| `README_RU.md` | Documentation (Russian) |

## Version History

| Version | Date | Summary |
|---------|------|---------|
| 2.8.4 | 2026-09-21 | Fix #13: inverted measuring flag (50% polls skipped) |
| 2.8.3 | 2026-09-21 | Fix #9-#12: UAF exit, DHT22 negative temp, gpiod, debug log |
| 2.8.2 | 2026-09-21 | Fix #5-#8: list_lock, measuring flag, auto_interval, DHT11 humidity |
| 2.8.1 | 2026-09-21 | Fix #4: poll thread break on rate-limit skip |
| 2.8 | 2026-09-21 | Fix #1-#9: kref, TOCTOU, poll thread, DHT11 cold, preempt, cansleep |
| 2.7 | 2026-09-21 | Configuration file support |
| 2.6 | 2026-09-20 | Shared procfs, safe unload, rate limiting |
| 2.5 | 2026-09-18 | Initial release |
