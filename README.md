# DHT11/DHT22/AM2302 Linux Kernel Driver

**Version 2.5**  
**Author: Chapvic**  
**License: GPL v3**

A Linux kernel module for reading temperature and humidity from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins. The driver exposes a procfs interface under `/proc/sensors/dht/` for sensor registration, configuration, and data retrieval — no device tree overlay required.

---

## Table of Contents

- [Supported Hardware](#supported-hardware)
- [Building and Installing](#building-and-installing)
- [Procfs Interface](#procfs-interface)
- [Quick Start](#quick-start)
- [Usage Examples](#usage-examples)
- [Configuration](#configuration)
- [Debug Mode](#debug-mode)
- [Error Codes](#error-codes)
- [Technical Details](#technical-details)
- [Limitations and Notes](#limitations-and-notes)
- [Troubleshooting](#troubleshooting)

---

## Supported Hardware

### Sensors

| Sensor | Temperature Range | Humidity Range | Resolution | Data Format |
|--------|-------------------|----------------|------------|-------------|
| DHT11  | 0–50 °C           | 20–90 % RH     | 1 °C / 1 % | 8-bit integer, no decimals |
| DHT22  | −40–80 °C         | 0–100 % RH     | 0.1 °C / 0.1 % | 16-bit, signed temp |
| AM2302 | −40–80 °C         | 0–100 % RH     | 0.1 °C / 0.1 % | Same as DHT22 (wired version) |

The driver auto-detects the sensor type based on the data format returned by the sensor:
- If humidity value exceeds 1000 (i.e. raw 16-bit value > 1000), the sensor is treated as **DHT11** (8-bit integer format).
- Otherwise, the sensor is treated as **DHT22/AM2302** (16-bit format with decimal precision).

### Raspberry Pi Models

| Model | SoC | GPIO Chip | Notes |
|-------|-----|-----------|-------|
| Pi 3 / Pi 3+ | BCM2837 | `pinctrl-bcm2835` | Direct GPIO lookup, base = 0 |
| Pi 4 / Pi 4+ | BCM2711 | `pinctrl-bcm2711` | Direct GPIO lookup, base = 0 |
| Pi 5 | BCM2712 | `pinctrl-rp1` | Requires full scan, base ≠ 0 |

The driver caches the GPIO chip base number after the first successful sensor registration, so subsequent registrations are instant.

---

## Building and Installing

### Prerequisites

- Linux kernel headers matching your running kernel
- `gcc` and `make`

On Raspberry Pi OS:

```bash
sudo apt install linux-headers-$(uname -r) build-essential
```

### Build

```bash
cd dht_driver
make
```

### Install

```bash
sudo make install
sudo modprobe dht
```

Or load manually:

```bash
sudo insmod dht.ko
```

### Unload

```bash
sudo rmmod dht
```

### Load at Boot (optional)

```bash
echo "dht" | sudo tee /etc/modules-load.d/dht.conf
sudo cp dht.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
```

---

## Procfs Interface

The driver creates entries under `/proc/sensors/dht/`.

### Global Entries

| Path | Mode | Description |
|------|------|-------------|
| `/proc/sensors/dht/debug` | rw (0666) | Debug logging: `0` = off (default), `1` = on |
| `/proc/sensors/dht/version` | r (0444) | Driver version string |
| `/proc/sensors/dht/export` | w (0222) | Write BCM pin number to register a new sensor |
| `/proc/sensors/dht/unexport` | w (0222) | Write BCM pin number to unregister a sensor |
| `/proc/sensors/dht/auto_interval` | rw (0666) | Global auto-poll interval in seconds (2–60). Write `-1` to disable. |

### Per-Sensor Entries

Each registered sensor gets a directory at `/proc/sensors/dht/gpio<pin>/`:

| Path | Mode | Description |
|------|------|-------------|
| `pin` | r (0444) | BCM GPIO pin number |
| `interval` | rw (0644) | Auto-poll interval in seconds (2–60, `-1` = off) |
| `measure` | w (0222) | Write `1` to trigger a manual measurement. Ignored if auto-poll is active. |
| `status_code` | r (0444) | Numeric error code (0 = success) |
| `status_text` | r (0444) | Human-readable error description |
| `value` | r (0444) | Sensor readings: `H=<humidity>\nT=<temperature>\n` |
| `info` | r (0444) | Sensor type and registration timestamp |
| `timestamp` | r (0444) | Unix timestamp of the last successful measurement |

---

## Quick Start

```bash
# 1. Load the driver
sudo insmod dht.ko

# 2. Register a sensor on GPIO 23
echo 23 | sudo tee /proc/sensors/dht/export

# 3. Read temperature and humidity
cat /proc/sensors/dht/gpio23/value

# 4. Unregister the sensor
echo 23 | sudo tee /proc/sensors/dht/unexport
```

---

## Usage Examples

### Register a Sensor

```bash
echo 23 | sudo tee /proc/sensors/dht/export
```

Expected dmesg output:

```
[dht_gpio_23]: found on 'pinctrl-bcm2835' (base=512, global=535)
[dht_gpio_23]: registered successfully
```

### Read Current Values

```bash
cat /proc/sensors/dht/gpio23/value
```

Output:

```
H=45.2
T=23.1
```

- `H` — humidity with one decimal place
- `T` — temperature in °C with one decimal place (negative for sub-zero)

### Manual Single Measurement

```bash
echo 1 | sudo tee /proc/sensors/dht/gpio23/measure
cat /proc/sensors/dht/gpio23/value
```

Manual measurement is only available when auto-poll is disabled (interval = `-1` and global auto_interval = `-1`). A minimum gap of 2 seconds is enforced between manual measurements.

### Enable Auto-Polling (Per-Sensor)

```bash
echo 5 | sudo tee /proc/sensors/dht/gpio23/interval
```

This starts a background kernel thread that reads the sensor every 5 seconds. To stop:

```bash
echo -1 | sudo tee /proc/sensors/dht/gpio23/interval
```

### Enable Global Auto-Polling

```bash
echo 10 | sudo tee /proc/sensors/dht/auto_interval
```

This enables auto-polling for **all** registered sensors with a shared 10-second interval. Sensors that were not polling will start automatically. Per-sensor intervals are ignored while global auto-poll is active.

To disable:

```bash
echo -1 | sudo tee /proc/sensors/dht/auto_interval
```

### Check Sensor Status

```bash
cat /proc/sensors/dht/gpio23/status_code
cat /proc/sensors/dht/gpio23/status_text
```

Output:

```
0
SUCCESS
```

### View Sensor Info

```bash
cat /proc/sensors/dht/gpio23/info
```

Output:

```
Sensor type: DHT11
Register time: 2026-09-18T14:32:05Z
```

### Check Last Measurement Time

```bash
cat /proc/sensors/dht/gpio23/timestamp
```

Output (Unix timestamp):

```
1726667525
```

### Register Multiple Sensors

```bash
echo 23 | sudo tee /proc/sensors/dht/export
echo 24 | sudo tee /proc/sensors/dht/export
echo 25 | sudo tee /proc/sensors/dht/export

# Enable global polling for all at once
echo 5 | sudo tee /proc/sensors/dht/auto_interval

# Read each sensor
cat /proc/sensors/dht/gpio23/value
cat /proc/sensors/dht/gpio24/value
cat /proc/sensors/dht/gpio25/value
```

### Unregister a Sensor

```bash
echo 23 | sudo tee /proc/sensors/dht/unexport
```

Expected dmesg output:

```
[dht_gpio_23]: unregistered
```

---

## Configuration

### Module Parameters

| Parameter | Default | Description |
|----------|---------|-------------|
| `dht_debug` | 0 | Enable debug logging at load time (`insmod dht.ko dht_debug=1`) |

### Compile-Time Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_SENSORS` | 32 | Maximum number of simultaneously registered sensors |
| `MAX_PIN_NUM` | 27 | Maximum BCM GPIO pin number |
| `MIN_INTERVAL` | 2 | Minimum poll interval in seconds |
| `MAX_INTERVAL` | 60 | Maximum poll interval in seconds |
| `MEAS_MIN_GAP` | 2 | Minimum seconds between manual measurements |
| `MAX_RETRIES` | 3 | Read attempts before giving up |
| `RETRY_DELAY_MS` | 100 | Delay between retry attempts |
| `BIT_THRESHOLD` | 40000 ns | Pulse width threshold: < 40 µs = bit 0, > 40 µs = bit 1 |
| `PULSE_TIMEOUT_NS` | 200000 ns | Maximum pulse width before timeout (200 µs) |
| `MAX_TIMINGS` | 100 | Maximum edge transitions to capture |

---

## Debug Mode

Enable debug logging to see measurement results, retry attempts, and diagnostic information in dmesg.

### Enable via procfs (runtime):

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
```

### Enable at module load:

```bash
sudo insmod dht.ko dht_debug=1
```

### Disable:

```bash
echo 0 | sudo tee /proc/sensors/dht/debug
```

### Debug Output Example

```
[dht_gpio_23]: found on 'pinctrl-bcm2835' (base=512, global=535)
[dht_gpio_23]: measurement OK - H=45.2% T=23.1 C
[dht_gpio_23]: registered successfully
[dht_gpio_23]: auto-poll enabled (interval=5)
[DHT]: debug enabled
[dht_gpio_23]: measurement OK - H=45.1% T=23.0 C
[dht_gpio_23]: measurement OK - H=45.3% T=23.1 C
```

If a read fails, debug output shows diagnostic data:

```
[dht_gpio_23]: read attempt 1 failed - j=39, data=[35,0,48,0,59]
[dht_gpio_23]: read attempt 2 failed - j=38, data=[34,0,48,0,58]
[dht_gpio_23]: read attempt 3 failed - j=40, data=[35,0,48,0,59]
[dht_gpio_23]: read failed after 3 attempts - j=40, data=[35,0,48,0,59]
[dht_gpio_23]: measurement failed - Sensor data read failed
```

- `j` — number of bits successfully read (40 = complete frame)
- `data[0..3]` — raw humidity and temperature bytes
- `data[4]` — checksum byte

---

## Error Codes

| Code | Constant | Description |
|------|----------|-------------|
| 0 | `ERR_SUCCESS` | Measurement succeeded |
| 1 | `ERR_PIN_INVALID` | Pin number out of range (0–27) |
| 2 | `ERR_GPIO_REQUEST` | GPIO descriptor lookup or direction set failed |
| 3 | `ERR_READ_FAILED` | Sensor data read failed after all retries |
| 4 | `ERR_AUTO_MODE` | Manual measure rejected because auto-poll is active |
| 5 | `ERR_TOO_SOON` | Manual measure rejected: less than 2 seconds since last attempt |

### Reading Error Status

```bash
cat /proc/sensors/dht/gpio23/status_code   # numeric code
cat /proc/sensors/dht/gpio23/status_text   # text description
```

---

## Technical Details

### DHT Protocol

The DHT11/DHT22/AM2302 protocol uses a single-wire bidirectional interface:

1. **Start signal**: MCU pulls the data line low for ≥18 ms (driver uses 20 ms), then releases it.
2. **Sensor response**: Sensor pulls low for ~80 µs, then high for ~80 µs.
3. **Data transmission**: 40 bits (5 bytes), MSB first. Each bit is preceded by a ~50 µs low pulse. Bit value is determined by the duration of the high pulse:
   - **Bit 0**: ~26 µs high
   - **Bit 1**: ~70 µs high
4. **Checksum**: Sum of the first 4 bytes modulo 256 must equal the 5th byte.

### Data Format

**DHT11** (8-bit integer):
```
Byte 0: Humidity integer part
Byte 1: Humidity decimal part (always 0)
Byte 2: Temperature integer part
Byte 3: Temperature decimal part (always 0)
Byte 4: Checksum
```

**DHT22/AM2302** (16-bit):
```
Byte 0: Humidity high byte
Byte 1: Humidity low byte
Byte 2: Temperature high byte (MSB = sign bit)
Byte 3: Temperature low byte
Byte 4: Checksum
```

### Nanosecond Precision Timing

The driver uses `ktime_get_ns()` for nanosecond-precision pulse measurement instead of a counter-based approach. This ensures reliable reads across all Raspberry Pi models regardless of CPU clock speed:

- **Bit threshold**: 40 µs — cleanly separates bit 0 (~26 µs) from bit 1 (~70 µs)
- **Pulse timeout**: 200 µs — prevents infinite loops if the sensor disconnects
- **No dependency on CPU frequency or loop overhead**

### GPIO Chip Detection

On Raspberry Pi 3/4, GPIO pins are directly accessible at global numbers matching the BCM pin number (base = 0). On Raspberry Pi 5, the `pinctrl-rp1` chip has a non-zero base offset, so the driver performs a scan of up to 2048 global GPIO numbers to find the chip.

After the first successful detection, the chip base is cached (`cached_chip_base`). Subsequent sensor registrations use the cached value for instant lookup — no scanning required.

The driver recognizes Pi GPIO chips by label:
- `rp1` (Pi 5)
- `bcm2835` (Pi 3)
- `bcm2711` (Pi 4)
- `bcm2712` (Pi 5 alternate)

### Polling Architecture

Each sensor with active auto-polling runs in its own kernel thread (`dht_poll_<pin>`). The thread:

1. Performs a measurement via `dht_do_measurement()`
2. Determines the effective interval (global or per-sensor)
3. Sleeps for the interval duration (in 1-second increments for responsive shutdown)
4. Repeats until `kthread_should_stop()` is set

Global auto-poll (`auto_interval`) overrides per-sensor intervals. When global auto-poll is active, all registered sensors poll at the global interval regardless of their individual settings.

### Locking Strategy

- **`list_lock`** — protects the global sensor list and `sensor_count`
- **`sensor->lock`** — protects per-sensor state (readings, status, interval)
- **`dht_do_measurement()`** manages `sensor->lock` internally: locks for rate-limit check and result storage, but performs the actual GPIO read without the lock to avoid deadlocks
- **`READ_ONCE` / `WRITE_ONCE`** used for `global_auto_interval`, `dht_debug`, and `cached_chip_base` — these are write-once or rarely-changed values accessed from multiple threads

### Rate Limiting

Manual measurements enforce a minimum 2-second gap (`MEAS_MIN_GAP`) between attempts. This prevents sensor overload — DHT sensors require at least 1–2 seconds between reads.

Auto-poll measurements are not rate-limited (the interval itself serves as the limiter).

### Retry Logic

Each measurement attempt tries up to 3 times (`MAX_RETRIES`) with a 100 ms delay between retries. DHT sensors frequently fail to respond on the first attempt, especially immediately after module load or after a long idle period.

### Auto-Detection of Sensor Type

The driver distinguishes DHT11 from DHT22/AM2302 by examining the raw humidity value:

- If `humidity_raw > 1000` → **DHT11** (8-bit format: `data[0]` is the integer part, `data[1]` is decimal, always 0)
- Otherwise → **DHT22/AM2302** (16-bit format: `data[0] << 8 + data[1]` is the full value with 0.1 resolution)

This works because DHT11's maximum humidity is 90% (raw byte = 90, 16-bit value = 90), while DHT22's raw 16-bit value for typical room humidity (45.2%) would be 452 — well below 1000. The threshold of 1000 cleanly separates the two formats.

### Procfs Compatibility

The driver supports both old (`file_operations`) and new (`proc_ops`) procfs APIs via compile-time macros, and handles the `PDE_DATA` → `pde_data` rename in kernel 5.17+.

---

## Limitations and Notes

- **No device tree overlay needed** — the driver uses GPIO descriptors directly, no DT binding required.
- **Single-wire protocol is timing-sensitive** — reads may occasionally fail, especially under heavy CPU load. The retry mechanism mitigates this.
- **Not suitable for Precise real-time applications** — the driver uses standard kernel GPIO API, not raw register access. For mission-critical applications, consider a userspace library with direct register access.
- **Maximum 32 sensors** — the driver supports up to 32 simultaneously registered sensors (`MAX_SENSORS`).
- **Pull-up resistor** — DHT11 modules usually include an onboard pull-up. For bare sensors, a 4.7 kΩ – 10 kΩ pull-up resistor between DATA and VCC is required.
- **No IRQ-based reading** — polling only. The DHT protocol's tight timing requirements make interrupt-based reading impractical without hardware timer support.
- **Temperature unit** — all values are in Celsius.
- **No sysfs or char device** — procfs is the only interface.
- **Kernel 5.x+ required** — the driver uses `ktime_get_ns()`, `gpiod_*` API, and `proc_ops`.

---

## Troubleshooting

### Sensor registration fails with "Input/output error"

1. Check wiring: VCC (3.3V), GND, DATA pin
2. Verify the pull-up resistor (4.7–10 kΩ) is present if using a bare sensor
3. Enable debug and check dmesg:

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
echo 23 | sudo tee /proc/sensors/dht/export
dmesg | tail -20
```

4. Look for `read failed` messages with `data=[...]` — if `j < 40`, bits are being lost. Ensure the sensor is a genuine DHT11/DHT22, not a counterfeit.
5. If `j=0`, the sensor is not responding at all — check power and wiring.

### "GPIO descriptor not found"

The driver could not find a Pi GPIO chip. This may happen on non-Raspberry Pi boards. The driver only supports Raspberry Pi GPIO controllers (`pinctrl-bcm2835`, `pinctrl-bcm2711`, `pinctrl-rp1`).

### Readings are always the same

DHT11 has a 1-second minimum sampling rate. If you read too frequently, the sensor returns cached data. Ensure your poll interval is ≥ 2 seconds.

### Negative temperature reads as positive

If using a DHT22 in sub-zero conditions, ensure the sensor supports negative temperatures. DHT11 does not measure below 0 °C.

### "Too soon since last measurement"

Manual measurements are rate-limited to one every 2 seconds. Wait and try again, or use auto-polling instead.

### dmesg shows no output at all

The driver only prints to dmesg for registration/unregistration events and errors. Measurement results are only logged when debug mode is enabled:

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
```

### Module fails to compile

Ensure you have the correct kernel headers installed:

```bash
sudo apt install linux-headers-$(uname -r)
```

The driver requires kernel 5.x or newer for `proc_ops` and `gpiod_*` API support.

---

## File Structure

```
dht_driver/
├── dht.c          # Driver source code
├── Makefile       # Build configuration
└── README.md      # This file
```

---

## License

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 3 as published by the Free Software Foundation.
