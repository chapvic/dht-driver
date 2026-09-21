# DHT11/DHT22/AM2302 Linux Kernel Driver

**Version:** 2.8.3  
**Author:** Chapvic  
**License:** GPL v3  
**Kernel:** 5.4+ (ARM64, ARM32)  
**Platform:** Raspberry Pi 1/Zero/Zero W/2/3/4/5  

---

## Overview

A Linux kernel module for reading temperature and humidity from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins. The driver provides a procfs interface under `/proc/sensors/dht/` for sensor management and data retrieval.

Key features:
- Dynamic sensor export/unexport via procfs
- Per-sensor proc entries for temperature, humidity, status, and configuration
- Background polling thread with configurable interval
- Global auto-poll mode with shared interval
- GPIO chip base caching for fast multi-sensor registration on Pi 3/4/5
- Nanosecond-precision pulse timing for reliable reads across all Pi models
- Rate limiting for all measurements (manual and auto-poll)
- Safe module unload with module reference counting (kref-based lifetime)
- Shared /proc/sensors: coexists with other sensor drivers
- Configuration file: optional `/etc/default/dht` for auto-registration at load
- Preemption protection during timing-critical GPIO reads
- atomic_t measuring flag prevents concurrent bit-bang on same GPIO
- gpio_request/gpio_free for exclusive GPIO ownership

---

## Supported Hardware

### Sensors

| Sensor | Humidity Range | Temperature Range | Resolution | Protocol |
|--------|---------------|-------------------|------------|----------|
| DHT11 | 20-90% RH | 0-50 C | 1 C / 1% | Single-wire |
| DHT22 (AM2302) | 0-100% RH | -40 to 80 C | 0.1 C / 0.1% | Single-wire |

### Raspberry Pi Models

| Model | SoC | Architecture | GPIO Chip | Notes |
|-------|-----|-------------|-----------|-------|
| Pi 1 / Zero / Zero W | BCM2835 | ARM32 | pinctrl-bcm2835 | Base 0 |
| Pi 2 / 3 | BCM2837 | ARM32/ARM64 | pinctrl-bcm2835 | Base 0 |
| Pi 4 | BCM2711 | ARM64 | pinctrl-bcm2711 | Base 0 |
| Pi 5 | BCM2712 | ARM64 | pinctrl-bcm2712 | Base 0 |

---

## Wiring

Connect the sensor to the Raspberry Pi:

```
Sensor       Raspberry Pi GPIO
-----        ------------------
VCC (pin 1)  3.3V (pin 1 or 17)
DATA         Any BCM GPIO pin (e.g., GPIO4 = pin 7)
GND (pin 4)  GND (pin 6 or 9)
```

Add a 4.7k-10k pull-up resistor between VCC and DATA if not built into the sensor (AM2302 has it built in).

---

## Quick Start

### Build and Load

```bash
# Build the module
make

# Load the module
sudo insmod dht.ko

# Verify it loaded
cat /proc/sensors/dht/version
# Output: 2.8.3
```

### Register and Read a Sensor

```bash
# Register a sensor on GPIO4
echo 4 | sudo tee /proc/sensors/dht/export

# Trigger a manual measurement
echo 1 | sudo tee /proc/sensors/dht/gpio4/measure

# Read the values
cat /proc/sensors/dht/gpio4/value
# Output: H=45.2
#         T=23.1

# Check the status
cat /proc/sensors/dht/gpio4/status_text
# Output: SUCCESS
```

### Auto-Poll Mode

```bash
# Enable global auto-poll every 10 seconds for all sensors
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# Read the latest values (updated automatically)
cat /proc/sensors/dht/gpio4/value

# Disable auto-poll
echo -1 | sudo tee /proc/sensors/dht/auto_interval
```

### Configuration File

Create `/etc/default/dht`:

```bash
# /etc/default/dht
DEBUG=1
AUTO_INTERVAL=10
SENSOR=4
SENSOR=17,5
SENSOR=22
```

The file is read at module load time. If it does not exist, the driver loads with defaults. Lines starting with `#` and empty lines are ignored. Unknown options produce warnings in dmesg.

```bash
sudo insmod dht.ko
# dmesg output:
# [DHT]: DHT Driver (c) 2026, Chapvic (v2.8.3)
# [DHT]: config: DEBUG=1
# [DHT]: config: AUTO_INTERVAL=10
# [DHT]: config: SENSOR pin=4 interval=-1
# [DHT]: config: SENSOR pin=17 interval=5
# [DHT]: config: SENSOR pin=22 interval=-1
```

---

## Procfs Interface

### Global Entries

```
/proc/sensors/dht/
  debug         (rw, 0666) - debug logging: 0 = off (default), 1 = on
  version       (r,  0444) - driver version
  export        (w,  0222) - write BCM pin number to register a sensor
  unexport      (w,  0222) - write BCM pin number to unregister a sensor
  auto_interval (rw, 0666) - global auto-poll interval (2-60, -1 = off)
```

### Per-Sensor Entries

```
/proc/sensors/dht/gpio<pin>/
  pin           (r,  0444) - BCM GPIO pin number
  interval      (rw, 0666) - auto-poll interval in seconds (2-60, -1 = off)
  measure       (w,  0222) - write "1" to trigger measurement
                             (ignored if interval != -1 or global auto active)
  status_code   (r,  0444) - error code (0 = success)
  status_text   (r,  0444) - error description
  value         (r,  0444) - "H=<humidity>\nT=<temperature>\n"
  info          (r,  0444) - sensor type + registration time
  timestamp     (r,  0444) - Unix timestamp of last measurement
```

### Poll Priority

| Priority | Setting | Behavior |
|----------|---------|----------|
| 1 | Global `auto_interval` (if != -1) | All sensors polled at this interval |
| 2 | Per-sensor `interval` (if global is -1) | Only this sensor polled |
| 3 | Manual `measure` (if both are -1) | One-shot read on request |

---

## Examples

### Shell

```bash
# Register, poll, and read
echo 4 | sudo tee /proc/sensors/dht/export
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval
sleep 6
cat /proc/sensors/dht/gpio4/value
# H=45.2
# T=23.1
```

### Python

```python
def read_dht(pin):
    with open(f"/proc/sensors/dht/gpio{pin}/measure", "w") as f:
        f.write("1")
    with open(f"/proc/sensors/dht/gpio{pin}/value") as f:
        data = f.read()
    lines = data.strip().split("\n")
    humidity = float(lines[0].split("=")[1])
    temperature = float(lines[1].split("=")[1])
    return humidity, temperature

h, t = read_dht(4)
print(f"Humidity: {h:.1f}%  Temperature: {t:.1f} C")
```

### Configuration File

```bash
# /etc/default/dht
DEBUG=0
AUTO_INTERVAL=15
SENSOR=4
SENSOR=17,5
```

### Safe Unload

```bash
# Unregister sensors first (optional - rmmod handles cleanup)
echo 4 | sudo tee /proc/sensors/dht/unexport

# Unload the module
sudo rmmod dht
```

---

## Error Codes

### Driver Error Codes (status_code)

| Code | Name | Description |
|------|------|-------------|
| 0 | ERR_SUCCESS | Operation completed successfully |
| 1 | ERR_PIN_INVALID | GPIO pin number out of valid range |
| 2 | ERR_GPIO_REQUEST | Failed to request or find GPIO descriptor |
| 3 | ERR_READ_FAILED | Sensor data read failed (checksum, timeout, etc.) |
| 4 | ERR_AUTO_MODE | Manual measurement while auto-poll is active |
| 5 | ERR_TOO_SOON | Manual measurement rejected (rate limiting) |

### Errno Values (write operations)

| Code | Name | When |
|------|------|------|
| -EINVAL | Invalid argument | Bad pin number, bad interval value |
| -EBUSY | Device busy | Pin already registered (or TOCTOU race) |
| -ENOMEM | Out of memory | Max sensors reached or allocation failure |
| -ENODEV | No such device | GPIO not found or sensor not registered |
| -EIO | I/O error | Initial measurement failed during registration |

---

## Configuration Parameters

### Module Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| dht_debug | int | 0 | Debug logging (0 = off, 1 = on) |

### Compile-Time Constants

| Constant | Value | Description |
|----------|-------|-------------|
| MAX_SENSORS | 32 | Maximum simultaneously registered sensors |
| MAX_PIN_NUM | 27 | Highest valid BCM GPIO pin number |
| MIN_INTERVAL | 2 | Minimum auto-poll interval (seconds) |
| MAX_INTERVAL | 60 | Maximum auto-poll interval (seconds) |
| MEAS_MIN_GAP | 2 | Minimum seconds between measurements |
| MAX_RETRIES | 3 | Read attempts before giving up |
| RETRY_DELAY_MS | 100 | Delay between retries (milliseconds) |
| BIT_THRESHOLD | 40000 | Nanosecond threshold: 0 (~26us) vs 1 (~70us) |

---

## Debug Mode

Enable debug logging to diagnose issues:

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
# or at load time:
sudo insmod dht.ko dht_debug=1
```

Debug messages appear in `dmesg` prefixed with `[DHT]:` or `[dht_gpio_N]:`.

---

## Architecture

### Reference Counting (kref)

Each sensor is protected by a `struct kref refcount`. The lifecycle:
- `dht_do_register`: `kref_init` (refcount = 1, owned by the list)
- `dht_proc_open`: `kref_get_unless_zero` (refcount++ if sensor is alive)
- `dht_proc_release`: `kref_put` (refcount--)
- `dht_sensor_free` / `dht_sensor_put`: `kref_put` (drops list reference)
- `dht_sensor_release` (callback when refcount reaches 0): stop_poll -> gpio_free -> proc_remove -> mutex_destroy -> kfree

This prevents use-after-free when a user unexports a sensor while keeping its procfs files open.

### Preemption Protection

The 40-bit DHT read cycle uses `preempt_disable()` / `preempt_enable()` to prevent CPU migration during timing-critical bit-bang. IRQs are not disabled (IRQ jitter is tolerable for the ~70 us DHT pulses).

### Sensor Type Detection

The sensor type (DHT11 vs DHT22) is detected from the data format on the first successful measurement and locked permanently:
- `h > 1000`: DHT11 (integer humidity > 100% when interpreted as 16-bit)
- `h <= 1000` with `data[1] == 0 && data[3] == 0 && data[0] <= 100 && data[2] <= 50`: DHT11 at low humidity (edge case)
- Otherwise: DHT22/AM2302

### GPIO Ownership

Since v2.8.2, the driver calls `gpio_request` during registration for exclusive GPIO ownership. If the pin is already in use by another driver, registration fails with `-EBUSY`. The GPIO is freed in `dht_sensor_release`.

Since v2.8.3, `dht_read_sensor` receives the GPIO descriptor from `sensor->gpiod` instead of re-resolving it on every measurement.

### Concurrent Measurement Protection

An `atomic_t measuring` flag prevents two simultaneous bit-bang operations on the same GPIO line. `atomic_cmpxchg` is used to claim the flag before reading; manual measurements that conflict get `ERR_READ_FAILED`, auto-poll cycles silently skip.

### Poll Thread Lifecycle

The poll thread runs in a loop, sleeping in 1-second increments. When both global and per-sensor intervals are -1, the thread enters an idle loop (sleeps 1 second, rechecks). If intervals are re-enabled, polling resumes. If `kthread_should_stop()` returns true, the thread exits.

### Rate Limiting

All measurements (manual and auto-poll) are subject to a minimum gap of `MEAS_MIN_GAP` (2 seconds). DHT sensors need recovery time between reads. Manual measurements that violate the gap return `ERR_TOO_SOON`; auto-poll cycles silently skip.

### Safe Module Unload

Two-phase cleanup in `dht_driver_exit`:
- Phase 1: Set exiting flag, disable auto-poll, `proc_remove(proc_dir)` (blocks until all open files are closed)
- Phase 2: Splice sensor list, for each sensor: `proc_dir = NULL` (avoid UAF), `dht_sensor_put` (stop thread, free GPIO, destroy mutex, free memory)

### Sleeping GPIO Check

`gpiod_cansleep()` is checked before the bit-bang loop. If the GPIO is behind a sleeping expander (e.g., I2C GPIO chip), the read returns `ERR_GPIO_REQUEST` immediately with a diagnostic message, instead of silently producing corrupted data.

---

## Known Limitations

- **Deprecated GPIO API**: `gpio_to_desc()` / `desc_to_gpio()` are deprecated in recent kernels. Migration to `gpiod_get()` via device tree requires a complete registration model rework and is planned for a future major version.
- **Sleeping GPIO expanders**: GPIOs behind I2C/SPI expanders are rejected (`gpiod_cansleep`). The bit-bang timing loop requires non-sleeping GPIO access.
- **Single-wire protocol**: Only one sensor per GPIO pin. Multiple sensors require separate pins.
- **Permissions**: `debug` and `auto_interval` entries are world-writable (0666). This is a user/distro choice.

---

## Troubleshooting

| Symptom | Possible Cause | Solution |
|---------|---------------|----------|
| `ERR_GPIO_REQUEST` (status_code 2) | GPIO not found or on sleeping chip | Check pin number; ensure GPIO is directly on Pi SoC |
| `ERR_READ_FAILED` (status_code 3) | Checksum error, wiring issue | Check wiring, pull-up resistor; retry |
| `ERR_AUTO_MODE` (status_code 4) | Manual measure while auto-poll active | Disable auto-poll first, or just read `value` |
| `ERR_TOO_SOON` (status_code 5) | Rate limiting (< 2s since last read) | Wait 2+ seconds before next manual read |
| `Measurement already in progress` | Concurrent read on same GPIO | Wait for the current read to finish |
| Negative temperature shows huge value | Fixed in v2.8.3 (was `c = -c` bug) | Update to v2.8.3 |
| No data after registration | Initial measurement failed | Check wiring; enable debug logging |
| `echo` returns error | Invalid input or device busy | Check `echo $?` and dmesg |

---

## Version History

| Version | Date | Highlights |
|---------|------|------------|
| 2.8.3 | 21.09.2026 | UAF fix in exit, DHT22 negative temp, gpiod passthrough, debug log fix |
| 2.8.2 | 21.09.2026 | list_lock for interval_write, measuring flag, auto_interval validation, DHT11 low humidity |
| 2.8.1 | 21.09.2026 | Poll thread idle loop (fix dangling pointer) |
| 2.8 | 21.09.2026 | kref refcounting, TOCTOU fix, poll thread exit, DHT11 <5C, type locking, preempt, cansleep, dead code cleanup |
| 2.7 | 21.09.2026 | Configuration file support |
| 2.6 | 20.09.2026 | Shared /proc/sensors, safe unload, rate limiting |
| 2.5 | 18.09.2026 | Initial release |

---

## Files

| File | Description |
|------|-------------|
| `dht.c` | Main driver source |
| `Makefile` | Build configuration |
| `/etc/default/dht` | Optional configuration file (user-created) |
| `README.md` | English documentation |
| `README_RU.md` | Russian documentation |
| `changelog.txt` | English changelog |
| `changelog_ru.txt` | Russian changelog |

---

## Build Requirements

- Linux kernel headers (>= 5.4)
- GCC / Clang with kernel build support
- `make`
- Root access for `insmod` / `rmmod`

---

## License

GPL v3. See the source file header for the full text.

---

## Acknowledgments

- DHT11/DHT22 protocol documentation from Aosong (Guangzhou) Electronics
- Raspberry Pi GPIO subsystem documentation
- Linux kernel procfs and GPIO consumer APIs
