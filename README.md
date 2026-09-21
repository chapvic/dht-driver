# DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver

**Version:** 2.8.5  
**Author:** (c) 2026, Chapvic  
**License:** GNU General Public License v3

A Linux kernel module for reading temperature and humidity data from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins. The driver creates a procfs interface under `/proc/sensors/dht/` for managing sensor registration, configuration, and data retrieval.

---

## Table of Contents

1. [Overview](#overview)
2. [DHT Protocol Architecture](#dht-protocol-architecture)
3. [Supported Hardware](#supported-hardware)
4. [Wiring](#wiring)
5. [Quick Start](#quick-start)
6. [Build & Install](#build--install)
7. [DKMS Installation](#dkms-installation)
8. [Kernel Compatibility](#kernel-compatibility)
9. [Module Loading](#module-loading)
10. [Configuration File](#configuration-file)
11. [Procfs Interface](#procfs-interface)
12. [Sensor Registration](#sensor-registration)
13. [Reading Data](#reading-data)
14. [Auto-Polling](#auto-polling)
15. [Manual Measurement](#manual-measurement)
16. [Sensor Type Detection](#sensor-type-detection)
17. [systemd Service](#systemd-service)
18. [Bash Examples](#bash-examples)
19. [Python Examples](#python-examples)
20. [Error Codes](#error-codes)
21. [Configuration Parameters](#configuration-parameters)
22. [Debug Mode](#debug-mode)
23. [Architecture](#architecture)
24. [Version History](#version-history)
25. [Known Limitations](#known-limitations)
26. [Troubleshooting](#troubleshooting)
27. [Files in This Repository](#files-in-this-repository)
28. [License](#license)

---

## Overview

The DHT driver provides a procfs-based interface for reading temperature and humidity from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins.

Key features:

- Dynamic sensor registration via procfs (`export`/`unexport`)
- Per-sensor proc entries for temperature, humidity, status, and configuration
- Background polling thread with configurable interval (per-sensor or global)
- Manual measurement trigger via procfs
- Nanosecond-precision pulse timing for reliable reads across all Pi models
- GPIO chip base caching for fast multi-sensor registration on Pi 3/4/5
- Rate limiting for all measurements (manual and auto-poll)
- Safe module unload with module reference counting (`kref`)
- Shared `/proc/sensors` directory: coexists with other sensor drivers
- Configuration file: optional `/etc/default/dht` for auto-registration at load
- Kernel compatibility: 5.0 through 6.18+ (with compile-time API shims)
- DKMS support: automatic rebuild on kernel updates

---

## DHT Protocol Architecture

The DHT11/DHT22/AM2302 sensors use a proprietary single-wire bidirectional protocol. The MCU (host) initiates communication, and the sensor responds with 40 bits of data (5 bytes).

### Communication Sequence

1. **Host start signal**: MCU pulls the data line low for at least 18 ms (driver uses 20 ms)
2. **Host release**: MCU switches the line to input; pull-up resistor brings it high
3. **Sensor response** (20-40 us after release):
   - Low pulse for ~80 us
   - High pulse for ~80 us
4. **Data transmission** (40 bits):
   - Each bit: ~50 us low pulse, then a high pulse whose duration encodes the value
   - **0 bit**: high pulse ~26 us
   - **1 bit**: high pulse ~70 us
5. **End of frame**: line returns to idle high state

### Bit Encoding

| Bit value | Low pulse | High pulse |
|-----------|-----------|------------|
| 0         | ~50 us    | ~26 us     |
| 1         | ~50 us    | ~70 us     |

The driver uses a **40 us threshold** (`BIT_THRESHOLD = 40000 ns`) to distinguish 0 from 1.

### 40-bit Data Frame

| Byte | Field              | DHT11 format        | DHT22 format        |
|------|--------------------|---------------------|---------------------|
| 0    | Humidity integer   | 0-100 (integer)     | RH high byte        |
| 1    | Humidity decimal   | 0 (always zero)     | RH low byte         |
| 2    | Temperature integer| 0-50 (integer)      | T high byte (bit 7 = sign) |
| 3    | Temperature decimal| 0 (always zero)     | T low byte          |
| 4    | Checksum           | (byte0+byte1+byte2+byte3) & 0xFF | Same |

**DHT11** sends integer values only (decimal bytes are always 0). Values are scaled x10 in the driver for consistent units.

**DHT22/AM2302** sends 16-bit values scaled x10: humidity as `(byte0 << 8) | byte1`, temperature as `((byte2 & 0x7F) << 8) | byte3`. Bit 7 of byte 2 is the sign bit for negative temperatures.

### Timing Parameters

| Parameter                | Value        | Description                                |
|--------------------------|--------------|--------------------------------------------|
| Start signal low         | 20 ms        | Host pulls line low                        |
| Sensor response delay    | 20-40 us     | After host releases line                   |
| Bit low pulse            | ~50 us       | Fixed low before each bit                  |
| Bit high pulse (0)       | ~26 us       | Below `BIT_THRESHOLD` (40 us)              |
| Bit high pulse (1)       | ~70 us       | Above `BIT_THRESHOLD` (40 us)              |
| Pulse timeout            | 200 us       | `PULSE_TIMEOUT_NS` -- abort if exceeded     |
| Total frame duration     | ~4 ms        | 40 bits + handshake                        |
| Min time between reads   | 2 s          | `MEAS_MIN_GAP` -- sensor recovery time      |

The driver disables preemption (`preempt_disable`/`preempt_enable`) during the bit-bang read to prevent timing corruption from context switches. Interrupts remain enabled to avoid system latency impact.

---

## Supported Hardware

### Sensors

| Sensor   | Temperature range | Humidity range | Accuracy (T) | Accuracy (H) | Type     |
|----------|-------------------|----------------|--------------|--------------|----------|
| DHT11    | 0-50 °C           | 20-90% RH      | ±2 °C        | ±5% RH       | Integer  |
| DHT22    | -40 to 80 °C      | 0-100% RH      | ±0.5 °C      | ±2% RH       | Decimal  |
| AM2302   | -40 to 80 °C      | 0-100% RH      | ±0.5 °C      | ±2% RH       | Decimal  |

AM2302 is a wired version of DHT22 with the same protocol.

### Raspberry Pi Models

| Model     | GPIO chip label       | GPIO base | Max BCM pins | Notes                          |
|-----------|----------------------|-----------|--------------|--------------------------------|
| Pi 5      | `pinctrl-rp1`         | 512+      | 0-27         | RP1 south bridge, large offset  |
| Pi 4      | `pinctrl-bcm2711`    | 0         | 0-27         | Direct BCM = global GPIO        |
| Pi 3/Zero | `pinctrl-bcm2835`     | 0         | 0-27         | Direct BCM = global GPIO        |
| Pi 1/2    | `pinctrl-bcm2835`    | 0         | 0-27         | Direct BCM = global GPIO        |

### Kernel Requirements

- **Minimum kernel version**: 5.0
- **Required config options**: `CONFIG_GPIOLIB`, `CONFIG_PROC_FS`, `CONFIG_MODULES`
- **Tested on**: 5.0 through 6.18+ (Raspberry Pi OS, Ubuntu, Debian)

---

## Wiring

### Raspberry Pi GPIO Header

Connect the sensor's data pin to any available GPIO pin (BCM numbering). The driver accepts BCM pin numbers 0-27.

```
DHT Sensor       Raspberry Pi
---------        ------------
VCC (pin 1)  →   3.3V (pin 1 or 17)
DATA         →   GPIO pin of your choice (e.g., GPIO4 = pin 7)
GND (pin 4)  →   GND (pin 6 or 9)
```

### Pull-up Resistor

Most DHT sensor modules include an on-board pull-up resistor (4.7k-10k). If using a bare sensor:

- Connect a 4.7kΩ-10kΩ resistor between DATA and VCC (3.3V)
- Without the pull-up, readings will be unreliable or fail entirely

### Pin Selection

- Any BCM GPIO pin 0-27 is supported
- Avoid pins with special functions (e.g., GPIO14/15 = UART, GPIO3 = I2C SDA) if those interfaces are in use
- GPIO4 is a common choice for DHT sensors on Raspberry Pi

---

## Quick Start

```bash
# 1. Clone or copy the driver files to a directory
cd dht-driver

# 2. Build
make

# 3. Load the module
sudo insmod dht.ko

# 4. Register a sensor on GPIO4
echo 4 | sudo tee /proc/sensors/dht/export

# 5. Read temperature and humidity
cat /proc/sensors/dht/gpio4/value

# 6. Enable auto-polling every 5 seconds
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval

# 7. Or enable global auto-polling for all sensors
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# 8. Check driver version
cat /proc/sensors/dht/version

# 9. Unload when done
sudo rmmod dht
```

---

## Build & Install

### Prerequisites

```bash
# Raspberry Pi OS / Debian / Ubuntu
sudo apt install build-essential linux-headers-$(uname -r)
```

### Native Build

```bash
make            # runs pre-build checks, then builds dht.ko
make check      # run pre-build checks only
make modules    # build without checks
make clean      # remove build artifacts
```

### Install (Native)

```bash
make install    # builds, installs to /lib/modules/$(uname -r)/, runs depmod
sudo modprobe dht
```

The module is installed to `/lib/modules/$(uname -r)/updates/` by Kbuild. This is the standard location for out-of-tree modules and takes priority over `/kernel/` in `modprobe` lookup.

### Uninstall

```bash
make uninstall   # searches all module directories for dht.ko and removes it
sudo modprobe -r dht
```

The `uninstall` target uses `find` to search all subdirectories under `/lib/modules/$(uname -r)/` (including `updates/`, `kernel/`, `extra/`) so it correctly removes the module regardless of where Kbuild placed it.

### Cross-Compilation

```bash
# Example: build on x86 for Raspberry Pi 5 (arm64)
make ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     KDIR=/path/to/rpi-kernel/build

# Install to a mounted target filesystem
make ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     KDIR=/path/to/rpi-kernel/build \
     INSTALL_MOD_PATH=/mnt/rpi-root \
     install
```

### Make Targets

| Target       | Description                                          |
|--------------|------------------------------------------------------|
| `make`       | Run pre-build checks, then build the module          |
| `make check` | Pre-build checks only (headers, version, config)    |
| `make modules` | Build `dht.ko` without checks                      |
| `make clean` | Remove build artifacts                               |
| `make install` | Build, install to `/lib/modules/...`, run `depmod` |
| `make uninstall` | Search and remove `dht.ko` from all directories   |
| `make help`  | Show available targets and variables                |

---

## DKMS Installation

DKMS (Dynamic Kernel Module Support) automatically rebuilds the module when the kernel is updated. This eliminates the need to manually rebuild after each `apt upgrade`.

### Prerequisites

```bash
sudo apt install dkms
```

### Installation

```bash
# 1. Copy driver files to the DKMS source tree
sudo mkdir -p /usr/src/dht-2.8.5
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.8.5/

# 2. Register the module with DKMS
sudo dkms add dht/2.8.5

# 3. Build and install
sudo dkms install dht/2.8.5

# 4. Load the module
sudo modprobe dht
```

### Verification

```bash
# Check DKMS status
sudo dkms status
# Expected output: dht/2.8.5: installed

# Verify the module is loaded
lsmod | grep dht
cat /proc/sensors/dht/version
```

### Automatic Rebuild

With `AUTOINSTALL="yes"` in `dkms.conf`, the module is automatically rebuilt when a new kernel is installed. No manual intervention needed.

### Updating to a New Driver Version

```bash
# 1. Remove the old version
sudo dkms remove dht/2.8.5 --all

# 2. Copy the new files
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.8.5/

# 3. Reinstall
sudo dkms install dht/2.8.5
```

### Complete Removal

```bash
sudo dkms remove dht/2.8.5 --all
sudo rm -rf /usr/src/dht-2.8.5
```

### dkms.conf

```ini
PACKAGE_NAME="dht"
PACKAGE_VERSION="2.8.5"
BUILT_MODULE_NAME[0]="dht"
DEST_MODULE_LOCATION[0]="/updates"
AUTOINSTALL="yes"
MAKE[0]="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build modules"
CLEAN="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build clean"
```

---

## Kernel Compatibility

The driver supports Linux kernels from 5.0 through 6.18+. API differences between kernel versions are handled at compile time using preprocessor macros -- no `configure` script is needed.

### API Compatibility Shims

#### proc_ops / file_operations (kernel 5.6)

Starting with kernel 5.6, `proc_create()` accepts `const struct proc_ops *` instead of `const struct file_operations *`. Field names also changed: `.read` → `.proc_read`, etc.

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
  #define DHT_PROC_OPS     struct proc_ops
  #define DHT_PROC_READ    .proc_read
  #define DHT_PROC_WRITE   .proc_write
  #define DHT_PROC_OPEN    .proc_open
  #define DHT_PROC_RELEASE .proc_release
#else
  #define DHT_PROC_OPS     struct file_operations
  #define DHT_PROC_READ    .read
  #define DHT_PROC_WRITE   .write
  #define DHT_PROC_OPEN    .open
  #define DHT_PROC_RELEASE .release
#endif
```

All `proc_ops`/`file_operations` structures in the driver use `DHT_PROC_OPS` instead of the raw type name.

#### pde_data / PDE_DATA (kernel 5.17)

Starting with kernel 5.17, `PDE_DATA()` was renamed to `pde_data()`.

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
  #define DHT_PDE_DATA(inode)  pde_data(inode)
#else
  #define DHT_PDE_DATA(inode)  PDE_DATA(inode)
#endif
```

### API Stability Table

| API                          | Used for              | Stable since | Notes                          |
|------------------------------|----------------------|--------------|--------------------------------|
| `proc_ops` / `file_operations` | procfs operations  | 5.6 / 5.0    | Shim via `DHT_PROC_OPS`        |
| `pde_data()` / `PDE_DATA()`   | Per-sensor data      | 5.17 / 5.0   | Shim via `DHT_PDE_DATA`        |
| `gpio_to_desc()`              | BCM pin lookup       | 3.x          | Stable, not removed            |
| `gpiod_to_chip()`             | GPIO chip detection  | 3.x          | Stable API                      |
| `gpiod_direction_output/input`| DHT protocol         | 3.x          | Stable                          |
| `gpiod_get_value()`           | Bit-bang read        | 3.x          | Stable                          |
| `gpiod_cansleep()`            | Sleep check          | 3.x          | Stable                          |
| `ktime_get_ns()`              | Pulse timing         | 4.x          | Stable                          |
| `ktime_get_real_seconds()`    | Timestamp            | 4.x          | Stable                          |
| `iterate_dir()`               | Procfs empty check   | 3.11         | Stable                          |
| `kref` / `kref_put`           | Reference counting   | 2.6.x        | Stable                          |
| `atomic_cmpxchg`              | Measuring flag       | 2.6.x        | Stable                          |
| `preempt_disable/enable`     | Bit-bang protection  | 2.6.x        | Stable                          |
| `filp_open` / `kernel_read`   | Config file reading  | 2.6.x        | Stable                          |

### Pre-build Checks

The Makefile `check` target verifies before compilation:

- Kernel build directory exists and contains a valid Kbuild tree
- Kernel version >= 5.0
- `CONFIG_GPIOLIB=y` in kernel `.config`
- `CONFIG_PROC_FS=y` in kernel `.config`
- `CONFIG_MODULES=y` in kernel `.config`
- Warns if `ARCH=` is set without `CROSS_COMPILE=` (or vice versa)

---

## Module Loading

### insmod (direct)

```bash
sudo insmod dht.ko
sudo insmod dht.ko dht_debug=1
```

### modprobe (after install)

```bash
sudo modprobe dht
sudo modprobe dht dht_debug=1
```

### Verify in dmesg

```bash
dmesg | grep DHT
# Expected:
# [DHT]: DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver (v2.8.5)
# [DHT]: procfs interface at /proc/sensors/dht/
```

### Unload

```bash
sudo rmmod dht
```

### Module Parameters

| Parameter   | Type | Default | Description                    |
|-------------|------|---------|--------------------------------|
| `dht_debug` | int  | 0       | Debug logging (0=off, 1=on)    |

---

## Configuration File

The driver reads an optional configuration file at `/etc/default/dht` during module load. If the file is missing, the driver loads with defaults.

### Format

Lines starting with `#` and empty lines are ignored. Unknown options and invalid values produce warnings in `dmesg`.

### Options

**Global options:**

| Option                | Description                                          |
|-----------------------|------------------------------------------------------|
| `DEBUG`              | Enable debug logging (same as `DEBUG=1`)              |
| `DEBUG=0|1`          | Explicitly disable/enable debug logging               |
| `AUTO_INTERVAL=n`    | Global auto-poll interval in seconds (2-60)           |

**Sensor registration:**

| Option               | Description                                          |
|----------------------|------------------------------------------------------|
| `SENSOR=<pin>`       | Register a sensor on the given BCM pin                |
| `SENSOR=<pin>,<n>`   | Register with per-sensor auto-poll interval (2-60s)   |

### Example

```bash
# /etc/default/dht
DEBUG=1
AUTO_INTERVAL=10
SENSOR=4              # DHT22 on GPIO4, uses global interval (10s)
SENSOR=17,5           # DHT11 on GPIO17, polls every 5 seconds
SENSOR=22             # Sensor on GPIO22, no auto-poll
```

### Processing Order

1. `DEBUG` is applied first
2. `AUTO_INTERVAL` is applied next
3. `SENSOR=` entries are processed in order

---

## Procfs Interface

### Global Entries (`/proc/sensors/dht/`)

| File           | Perm | Format                          | Description                          |
|----------------|------|---------------------------------|--------------------------------------|
| `debug`        | rw   | `0` or `1`                      | Debug logging on/off                 |
| `version`     | r    | `2.8.5`                         | Driver version                       |
| `export`       | w    | write `<pin>`                   | Register a new sensor                |
| `unexport`     | w    | write `<pin>`                   | Unregister a sensor                  |
| `auto_interval` | rw | `<n>` (2-60) or `-1`            | Global auto-poll interval             |

### Per-Sensor Entries (`/proc/sensors/dht/gpio<pin>/`)

| File           | Perm | Format                                    | Description                          |
|----------------|------|-------------------------------------------|--------------------------------------|
| `pin`          | r    | `<pin>`                                   | BCM GPIO pin number                  |
| `interval`     | rw   | `<n>` (2-60) or `-1`                      | Per-sensor auto-poll interval         |
| `measure`      | w    | write `1`                                 | Trigger manual measurement           |
| `status_code`  | r    | `0`-`5`                                   | Error code (0 = success)             |
| `status_text`  | r    | `SUCCESS` / error text                    | Human-readable status                |
| `value`        | r    | `H=<humidity>\nT=<temperature>\n`         | Last reading (scaled x10)            |
| `info`         | r    | `Sensor type: DHT11\nRegister time: <ISO>`| Sensor type + registration time      |
| `timestamp`    | r    | `<unix_timestamp>`                        | Timestamp of last successful reading |

### File Format Details

**`value`**: `H=<h>.<d>\nT=<t>.<d>\n` where values are scaled x10 (e.g., `H=45.2` means 45.2% RH). Negative temperatures show as `T=-23.1`.

**`info`:** `Sensor type: DHT11\nRegister time: 2026-01-15T14:30:00Z\n` or `Sensor type: DHT22\n...`. Empty output (EOF) if sensor type is not yet determined (no successful measurement).

**`timestamp`:** Unix timestamp (seconds since epoch) of the last **successful** measurement. `0` if no measurement has succeeded.

**`status_code`:** `0` = success, `1` = invalid pin, `2` = GPIO error, `3` = read failed, `4` = auto mode active, `5` = too soon.

---

## Sensor Registration

### Register a Sensor

```bash
echo 4 | sudo tee /proc/sensors/dht/export
```

This:
1. Validates the pin number (0-27)
2. Checks for duplicate registration
3. Resolves the GPIO descriptor (with chip base caching)
4. Requests the GPIO line
5. Creates procfs entries under `/proc/sensors/dht/gpio4/`
6. Performs an initial measurement

### Unregister a Sensor

```bash
echo 4 | sudo tee /proc/sensors/dht/unexport
```

This:
1. Finds the sensor by pin number
2. Stops the poll thread (if running)
3. Removes procfs entries
4. Releases the GPIO line
5. Frees the sensor struct (after all open file references are closed)

### Multiple Sensors

```bash
echo 4  | sudo tee /proc/sensors/dht/export
echo 17 | sudo tee /proc/sensors/dht/export
echo 22 | sudo tee /proc/sensors/dht/export
cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio17/value
cat /proc/sensors/dht/gpio22/value
```

Maximum: 32 sensors (`MAX_SENSORS`).

---

## Reading Data

### Cached Values

The `value`, `status_code`, `status_text`, and `timestamp` entries return cached data from the last measurement. They do not trigger a new read. The driver performs an initial measurement during registration.

### Reading in Shell

```bash
cat /proc/sensors/dht/gpio4/value
# Output: H=45.2
#         T=23.1

cat /proc/sensors/dht/gpio4/status_code
# Output: 0

cat /proc/sensors/dht/gpio4/timestamp
# Output: 1736946600
```

### Parsing in Shell

```bash
data=$(cat /proc/sensors/dht/gpio4/value)
humidity=$(echo "$data"  | grep '^H=' | cut -d= -f2)
temperature=$(echo "$data" | grep '^T=' | cut -d= -f2)
echo "Humidity: $humidity%"
echo "Temperature: $temperature°C"
```

### Stale Data

If the last measurement failed, `value` still returns the last successful reading. Check `status_code` to verify data freshness:

```bash
status=$(cat /proc/sensors/dht/gpio4/status_code)
if [ "$status" = "0" ]; then
    cat /proc/sensors/dht/gpio4/value
else
    cat /proc/sensors/dht/gpio4/status_text
fi
```

---

## Auto-Polling

The driver supports background polling threads that periodically measure sensors without manual intervention.

### Per-Sensor Auto-Poll

```bash
# Poll GPIO4 every 5 seconds
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval

# Disable per-sensor polling
echo -1 | sudo tee /proc/sensors/dht/gpio4/interval
```

### Global Auto-Poll

```bash
# Poll ALL registered sensors every 10 seconds
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# Disable global polling (per-sensor intervals take over)
echo -1 | sudo tee /proc/sensors/dht/auto_interval
```

### How It Works

- Each sensor with an active interval gets its own kernel thread (`dht_poll_<pin>`)
- Global interval takes priority over per-sensor interval
- The thread sleeps in 1-second increments for responsive shutdown
- Rate limiting (2s minimum) applies to all measurements
- If both global and per-sensor intervals are -1, the thread idles until re-enabled or stopped

---

## Manual Measurement

### Trigger a Measurement

```bash
echo 1 | sudo tee /proc/sensors/dht/gpio4/measure
```

### Limitations

- Manual measurement is rejected if auto-poll is active (per-sensor or global). `status_code` = 4 (`ERR_AUTO_MODE`)
- Rate limited: minimum 2 seconds between measurements. `status_code` = 5 (`ERR_TOO_SOON`)
- Cannot run concurrently with another measurement on the same sensor (atomic flag)

### Reading the Result

After triggering a measurement, read the result from the cached entries:

```bash
echo 1 | sudo tee /proc/sensors/dht/gpio4/measure
cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio4/status_code
```

---

## Sensor Type Detection

The driver automatically detects the sensor type (DHT11 or DHT22) from the data format on the first successful measurement.

### Detection Heuristic

1. If the combined 16-bit humidity value > 1000: **DHT11** (integer format, byte 0 > 100)
2. If decimal bytes are 0 and values are within DHT11 ranges (H ≤ 100%, T ≤ 50°C): **DHT11**
3. Otherwise: **DHT22/AM2302**

### Edge Case: DHT11 with Low Humidity

DHT11 sensors reporting ≤ 3% humidity produce a combined value ≤ 1000, which would match the DHT22 pattern. The driver handles this with an additional check: if decimal bytes are 0 and values are within DHT11 ranges, it's classified as DHT11.

### Type Locking

Once detected, the type is locked and does not change between measurements. This prevents spurious type flips from borderline readings.

### Viewing the Type

```bash
cat /proc/sensors/dht/gpio4/info
# Output:
# Sensor type: DHT22
# Register time: 2026-01-15T14:30:00Z
```

### Negative Temperatures

Both DHT11 and DHT22 support negative temperatures (DHT11 only below 0°C in some clones). The sign bit is bit 7 of byte 2 (or bit 15 of the combined 16-bit temperature value). The driver correctly masks the sign bit before negating.

---

## systemd Service

### Method 1: /etc/modules (simplest)

```bash
echo "dht" | sudo tee -a /etc/modules
echo "dht" | sudo tee -a /etc/modules-load.d/dht.conf
```

### Method 2: Module with parameters via systemd

Create `/etc/modprobe.d/dht.conf`:

```
options dht dht_debug=0
```

Create `/etc/systemd/system/dht-driver.service`:

```ini
[Unit]
Description=DHT sensor driver
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe dht
ExecStop=/sbin/rmmod dht

[Install]
WantedBy=multi-user.target
```

### Method 3: Full service with config file

Create `/etc/systemd/system/dht-sensors.service`:

```ini
[Unit]
Description=DHT Temperature and Humidity Sensor Driver
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe dht
ExecStart=/bin/sh -c 'for pin in 4 17 22; do echo $$pin > /proc/sensors/dht/export; done'
ExecStart=/bin/sh -c 'echo 10 > /proc/sensors/dht/auto_interval'
ExecStop=/bin/sh -c 'for pin in 4 17 22; do echo $$pin > /proc/sensors/dht/unexport; done'
ExecStop=/sbin/rmmod dht

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now dht-sensors.service
```

---

## Bash Examples

### 1. Basic Reading

```bash
#!/bin/bash
PIN=4
data=$(cat /proc/sensors/dht/gpio$PIN/value)
humidity=$(echo "$data"  | grep '^H=' | cut -d= -f2)
temperature=$(echo "$data" | grep '^T=' | cut -d= -f2)
echo "GPIO$PIN: H=${humidity}% T=${temperature}C"
```

### 2. Manual Measurement

```bash
#!/bin/bash
PIN=4
echo 1 | sudo tee /proc/sensors/dht/gpio$PIN/measure > /dev/null
sleep 1
status=$(cat /proc/sensors/dht/gpio$PIN/status_code)
if [ "$status" = "0" ]; then
    cat /proc/sensors/dht/gpio$PIN/value
else
    echo "Measurement failed: $(cat /proc/sensors/dht/gpio$PIN/status_text)"
fi
```

### 3. CSV Logging

```bash
#!/bin/bash
PIN=4
INTERVAL=10
LOGFILE=/tmp/dht_log.csv
echo "timestamp,humidity,temperature" > "$LOGFILE"
while true; do
    data=$(cat /proc/sensors/dht/gpio$PIN/value)
    ts=$(cat /proc/sensors/dht/gpio$PIN/timestamp)
    h=$(echo "$data" | grep '^H=' | cut -d= -f2)
    t=$(echo "$data" | grep '^T=' | cut -d= -f2)
    echo "$ts,$h,$t" >> "$LOGFILE"
    sleep "$INTERVAL"
done
```

### 4. Read All Sensors

```bash
#!/bin/bash
for dir in /proc/sensors/dht/gpio*/; do
    [ -d "$dir" ] || continue
    pin=$(cat "${dir}pin")
    data=$(cat "${dir}value")
    status=$(cat "${dir}status_code")
    echo "GPIO$pin (status=$status): $data"
done
```

### 5. Threshold Monitoring

```bash
#!/bin/bash
PIN=4
TEMP_MAX=30.0
HUM_MAX=70.0
while true; do
    data=$(cat /proc/sensors/dht/gpio$PIN/value)
    h=$(echo "$data" | grep '^H=' | cut -d= -f2)
    t=$(echo "$data" | grep '^T=' | cut -d= -f2)
    # Compare using awk for float support
    if awk "BEGIN{exit !($t > $TEMP_MAX)}"; then
        echo "WARNING: Temperature $t°C exceeds threshold $TEMP_MAX°C"
    fi
    if awk "BEGIN{exit !($h > $HUM_MAX)}"; then
        echo "WARNING: Humidity $h% exceeds threshold $HUM_MAX%"
    fi
    sleep 5
done
```

### 6. Auto-Registration Script

```bash
#!/bin/bash
# Register sensors from /etc/default/dht format
CONFIG=/etc/default/dht
if [ ! -f "$CONFIG" ]; then
    echo "Config file not found: $CONFIG"
    exit 1
fi
while IFS= read -r line; do
    case "$line" in
        \#*|"") continue ;;
        SENSOR=*)
            spec="${line#SENSOR=}"
            pin="${spec%%,*}"
            echo "Registering sensor on GPIO$pin..."
            echo "$pin" | sudo tee /proc/sensors/dht/export > /dev/null
            ;;
    esac
done < "$CONFIG"
```

---

## Python Examples

### 1. Basic Reading

```python
#!/usr/bin/env python3
PIN = 4
with open(f"/proc/sensors/dht/gpio{PIN}/value") as f:
    data = f.read().strip()
values = dict(line.split("=") for line in data.splitlines())
print(f"GPIO{PIN}: H={values['H']}%  T={values['T']}C")
```

### 2. Continuous Monitoring with CSV

```python
#!/usr/bin/env python3
import time, csv, sys
PIN = 4
INTERVAL = 10
with open(f"/proc/sensors/dht/gpio{PIN}/value") as devnull:
    pass
writer = csv.writer(sys.stdout)
writer.writerow(["timestamp", "humidity", "temperature", "status"])
try:
    while True:
        with open(f"/proc/sensors/dht/gpio{PIN}/value") as f:
            data = dict(l.split("=") for l in f.read().strip().splitlines())
        with open(f"/proc/sensors/dht/gpio{PIN}/status_code") as f:
            status = f.read().strip()
        with open(f"/proc/sensors/dht/gpio{PIN}/timestamp") as f:
            ts = f.read().strip()
        writer.writerow([ts, data["H"], data["T"], status])
        sys.stdout.flush()
        time.sleep(INTERVAL)
except KeyboardInterrupt:
    pass
```

### 3. Manual Measurement

```python
#!/usr/bin/env python3
import time, subprocess
PIN = 4
subprocess.run(f"echo 1 > /proc/sensors/dht/gpio{PIN}/measure", shell=True)
time.sleep(1)
with open(f"/proc/sensors/dht/gpio{PIN}/value") as f:
    print(f.read())
with open(f"/proc/sensors/dht/gpio{PIN}/status_code") as f:
    print(f"Status: {f.read().strip()}")
```

### 4. Read All Sensors

```python
#!/usr/bin/env python3
import os, glob
for d in sorted(glob.glob("/proc/sensors/dht/gpio*/")):
    pin = open(os.path.join(d, "pin")).read().strip()
    value = open(os.path.join(d, "value")).read().strip()
    status = open(os.path.join(d, "status_code")).read().strip()
    print(f"GPIO{pin} (status={status}): {value}")
```

### 5. systemd Daemon with syslog

```python
#!/usr/bin/env python3
"""dht-monitor.py -- systemd daemon that logs DHT readings to syslog."""
import time, syslog, glob, os, signal, sys

INTERVAL = 60
syslog.openlog("dht-monitor", syslog.LOG_PID | syslog.LOG_NDELAY, syslog.LOG_DAEMON)
running = True

def handle_sigterm(signum, frame):
    global running
    running = False
signal.signal(signal.SIGTERM, handle_sigterm)

while running:
    for d in sorted(glob.glob("/proc/sensors/dht/gpio*/")):
        pin = open(os.path.join(d, "pin")).read().strip()
        data = dict(l.split("=") for l in open(os.path.join(d, "value")).read().strip().splitlines())
        msg = f"GPIO{pin}: H={data['H']}% T={data['T']}C"
        syslog.syslog(syslog.LOG_INFO, msg)
    time.sleep(INTERVAL)
syslog.syslog(syslog.LOG_INFO, "dht-monitor stopping")
```

Unit file `/etc/systemd/system/dht-monitor.service`:

```ini
[Unit]
Description=DHT Sensor Monitor
After=systemd-modules-load.service

[Service]
Type=simple
ExecStart=/usr/local/bin/dht-monitor.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

---

## Error Codes

### Status Codes

| Code | Constant            | Description                              |
|------|---------------------|------------------------------------------|
| 0    | `ERR_SUCCESS`       | Operation completed successfully          |
| 1    | `ERR_PIN_INVALID`   | GPIO pin out of range (0-27)             |
| 2    | `ERR_GPIO_REQUEST`  | Failed to request or find GPIO descriptor|
| 3    | `ERR_READ_FAILED`   | Sensor data read failed (checksum/timeout)|
| 4    | `ERR_AUTO_MODE`     | Manual measure while auto-poll active     |
| 5    | `ERR_TOO_SOON`      | Rate limited (minimum 2s between reads)   |

### errno Values

| errno  | Meaning              | When                                   |
|--------|----------------------|----------------------------------------|
| `-EINVAL` | Invalid argument  | Bad pin number, bad interval value     |
| `-ENODEV` | No such device    | Driver unloading, sensor not found     |
| `-EFAULT` | Bad address       | `copy_from_user`/`copy_to_user` failed |
| `-EBUSY`  | Device busy        | Pin already registered                  |
| `-ENOMEM` | Out of memory     | `kzalloc` for sensor struct failed      |

---

## Configuration Parameters

### Module Parameters

| Parameter   | Type | Default | Range | Description                    |
|-------------|------|---------|-------|--------------------------------|
| `dht_debug` | int  | 0       | 0-1   | Debug logging via insmod/procfs|

### Compile-Time Constants

| Constant           | Value  | Description                                |
|--------------------|--------|--------------------------------------------|
| `MAX_SENSORS`      | 32     | Maximum simultaneously registered sensors   |
| `MAX_PIN_NUM`      | 27     | Highest valid BCM GPIO pin number           |
| `MIN_INTERVAL`     | 2      | Minimum auto-poll interval (seconds)        |
| `MAX_INTERVAL`     | 60     | Maximum auto-poll interval (seconds)        |
| `MEAS_MIN_GAP`     | 2      | Minimum seconds between measurements         |
| `MAX_RETRIES`      | 3      | Read attempts before giving up              |
| `RETRY_DELAY_MS`   | 100    | Delay between retries (milliseconds)        |
| `BIT_THRESHOLD`    | 40000  | Nanosecond 0/1 threshold (40 us)             |
| `PULSE_TIMEOUT_NS` | 200000 | Max nanoseconds per pulse (200 us)          |
| `CONFIG_PATH`      | `/etc/default/dht` | Configuration file path          |

---

## Debug Mode

### Enabling Debug

**Method 1: Module parameter**

```bash
sudo insmod dht.ko dht_debug=1
# or
sudo modprobe dht dht_debug=1
```

**Method 2: procfs at runtime**

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
```

**Method 3: Configuration file**

```
# /etc/default/dht
DEBUG=1
```

### Disabling Debug

```bash
echo 0 | sudo tee /proc/sensors/dht/debug
```

### Example Debug Output

```
[DHT]: debug enabled
[dht_gpio_4]: poll thread started
[dht_gpio_4]: measurement OK - H=45.2% T=23.1 C
[dht_gpio_4]: read attempt 1 failed - j=38, data=[45,0,23,0,68]
[dht_gpio_4]: measurement OK - H=45.2% T=23.1 C
[DHT]: debug disabled
```

---

## Architecture

### Reference Counting (`kref`)

Each sensor struct has a `kref` refcount. The initial reference is created during registration. Additional references are taken in `dht_proc_open()` for each open procfs file. This ensures the sensor struct is not freed while a user has a procfs file open, even if the sensor is unexported.

### Preemption Control

During the bit-bang read loop (~4 ms), preemption is disabled to prevent context switches from corrupting pulse timing. Interrupts remain enabled to avoid system latency impact.

### Sensor Type Detection

The driver detects DHT11 vs DHT22 on the first successful measurement using a heuristic based on the data format. Once detected, the type is locked.

### GPIO Chip Base Caching

On Pi 5, the GPIO chip has a large base offset (512+). The driver caches the chip base after the first successful lookup, so subsequent registrations use a fast path (`gpio_to_desc(base + pin)`) instead of a full scan.

### Rate Limiting

All measurements (manual and auto-poll) are rate-limited to at least 2 seconds apart. Manual measurements exceeding the rate limit get `ERR_TOO_SOON`; auto-poll measurements are silently skipped.

### Measurement Flag (`atomic_cmpxchg`)

An atomic flag (`sensor->measuring`) prevents parallel bit-bang reads on the same GPIO. The flag is claimed with `atomic_cmpxchg(&sensor->measuring, 0, 1)` -- if it was already 1, another measurement is in progress and the request is rejected.

### `last_attempt_time` Ordering

The `last_attempt_time` is updated **after** the `cmpxchg` succeeds and before the actual read begins. This ensures:
- Rate-limited rejections do not extend the rate-limit window
- Only actual measurement attempts (not rejected ones) reset the timer

### Safe Module Unload

Module reference counting (`try_module_get`/`module_put` in procfs open/release) prevents `rmmod` while procfs files are open. The exit function sets `dht_exiting` to reject new opens, then performs a two-phase cleanup: remove procfs entries first, then stop threads.

### Sleeping GPIO Chips

The driver checks `gpiod_cansleep()` and rejects sensors on sleeping GPIO expanders (e.g., I2C GPIO chips). The bit-bang timing loop requires non-sleeping GPIOs.

### Configuration File Loading

At module load, `/etc/default/dht` is read using `filp_open`/`kernel_read`. Unknown options produce warnings in `dmesg`. Missing file is not an error.

### Procfs Empty Check

When the driver is unloaded, it checks whether `/proc/sensors` is empty (via `iterate_dir`) before removing the directory. If other drivers have entries in `/proc/sensors`, the directory is left intact.

### Measurement Flow Diagram

```
User writes "1" to /proc/.../measure
        |
        v
+-------------------+
| dht_do_measurement|
| (manual=true)     |
+-------------------+
        |
        v
+-------------------+     +-------------------+
| Rate-limit check  |---->| ERR_TOO_SOON (5)  |
| (2s min gap)      |     +-------------------+
+-------------------+
        |
        v
+-------------------+     +-------------------+
| atomic_cmpxchg    |---->| ERR_READ_FAILED   |
| (measuring 0->1)  |     | (in progress)     |
+-------------------+     +-------------------+
        |
        v
+-------------------+
| last_attempt_time |
| = now             |
+-------------------+
        |
        v
+-------------------+
| Release lock      |
+-------------------+
        |
        v
+-------------------+
| dht_read_sensor   |
| (20ms + 4ms)      |
+-------------------+
        |
        v
+-------------------+
| measuring = 0     |
+-------------------+
        |
        v
+-------------------+     +-------------------+
| Store results     |---->| ERR_READ_FAILED   |
| under lock        |     | (3) on failure    |
+-------------------+     +-------------------+
        |
        v
  Update: humidity, temperature, type (if first), status, last_meas_time
```

---

## Version History

| Version | Date       | Changes                                                                |
|---------|------------|------------------------------------------------------------------------|
| 2.8.5   | 2026-01-15 | Patch #16: `iterate_dir` for procfs empty check                        |
|         |            | Patch #15: `last_attempt_time` updated after successful `cmpxchg`     |
|         |            | Patch #13: `atomic_cmpxchg` without negation                          |
|         |            | Kernel 5.0+ compatibility (proc_ops, pde_data shims)                   |
|         |            | DKMS support (`dkms.conf`)                                             |
|         |            | Makefile: `check`, `uninstall`, cross-compilation                      |
|         |            | README: 28-section format, DHT protocol, kernel compatibility table    |
| 2.8     | 2026-01-10 | Initial public release                                                 |

---

## Known Limitations

1. **GPIO timing sensitivity**: The bit-bang protocol requires microsecond-precision timing. High system load can cause read failures. Auto-polling mitigates this with retries.
2. **No I2C/SPI**: The DHT protocol is single-wire only. I2C or SPI versions (e.g., SHT3x) require a different driver.
3. **Minimum 2s between reads**: DHT sensors need recovery time. The driver enforces this for all measurements.
4. **No interrupt-based reading**: The driver uses polling (kthread), not GPIO interrupts. This simplifies the code but adds CPU overhead during reads.
5. **Sleeping GPIO chips not supported**: Sensors on I2C GPIO expanders cannot use this driver due to timing requirements.
6. **Max 32 sensors**: The `MAX_SENSORS` limit prevents unbounded memory allocation.
7. **One sensor per GPIO pin**: Each BCM pin can have at most one sensor.
8. **No device tree binding**: Sensors are registered dynamically via procfs, not via device tree overlays.

---

## Troubleshooting

| Symptom                              | Cause                          | Solution                                    |
|--------------------------------------|--------------------------------|---------------------------------------------|
| `make` fails: kernel headers not found | Missing `linux-headers`     | `sudo apt install linux-headers-$(uname -r)`|
| `make` fails: kernel version too old  | Kernel < 5.0                 | Upgrade kernel or use an older driver version|
| `implicit declaration of 'proc_read'` | Kernel < 5.6, missing shim   | Ensure `dht.c` has `DHT_PROC_OPS` macros    |
| `make` produces no `dht.ko`           | `obj-m` not at top level      | Ensure `obj-m += dht.o` is before `ifdef`   |
| `WARNING: ARCH= but CROSS_COMPILE=`    | Cross-compile vars mismatch   | Set both `ARCH` and `CROSS_COMPILE`          |
| DKMS build fails                      | Missing `dkms.conf` or headers | `sudo apt install dkms linux-headers-...`   |
| `CONFIG_GPIOLIB` warning             | GPIO support not in kernel    | Enable `CONFIG_GPIOLIB` in kernel config     |
| Read always fails (status=3)         | Wiring, pull-up, or timing    | Check wiring, add 4.7k-10k pull-up          |
| Read intermittent failures            | System load during bit-bang    | Enable auto-polling, reduce system load      |
| Negative temperature wrong           | Sign bit handling              | Fixed in 2.8.5 (mask before negate)          |
| `modprobe: dht not found`            | Module not installed          | `make install` or DKMS install               |
| `rmmod: Module dht is in use`        | Procfs file still open         | Close all `/proc/sensors/dht/` files        |
| Sensor not detected                  | No successful measurement yet  | Check wiring, try `echo 1 > measure`         |
| `PDE_DATA` implicit declaration      | Kernel >= 5.17, old code       | Use `DHT_PDE_DATA` macro                    |

---

## Files in This Repository

| File             | Description                                                     |
|------------------|-----------------------------------------------------------------|
| `dht.c`          | Driver source code (single file, ~2400 lines)                   |
| `Makefile`       | Build system with pre-build checks, install/uninstall, cross-build|
| `dkms.conf`      | DKMS configuration for automatic rebuild on kernel updates       |
| `LICENSE`        | GNU General Public License v3 text                              |
| `README.md`      | This file (English)                                             |
| `README_RU.md`   | Russian version of this documentation                            |
| `changelog.txt`  | Changelog (English)                                             |
| `changelog_ru.txt` | Changelog (Russian)                                           |
| `ReleaseNotes.md` | Release notes for GitHub                                       |

---

## License

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 3 as published by the Free Software Foundation.

Copyright (c) 2026, Chapvic

### Acknowledgements

The DHT communication protocol implementation is based on principles described in the Adafruit DHT library and various Linux kernel GPIO drivers. The single-wire bit-bang approach with `preempt_disable` follows established patterns in the Linux kernel community.
