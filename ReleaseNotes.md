# DHT Driver v2.8.5

DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver for Raspberry Pi.

---

## What's New

Version 2.8.5 brings kernel compatibility for 5.0–6.18+, DKMS support, build system fixes, and comprehensive documentation.

---

## Bug Fixes

### Build System

- **obj-m placement**: Fixed `obj-m += dht.o` — now at top level before `ifdef KERNELRELEASE`, so Kbuild can find the module
- **depmod after install**: Added manual `depmod -a` after `modules_install` — Kbuild skips depmod when `System.map` is missing, causing `modprobe: Module not found`
- **uninstall compressed modules**: `make uninstall` now searches for `.ko`, `.ko.xz`, `.ko.gz`, `.ko.zst`, `.ko.bz2`, `.ko.lz4` — previously only `.ko` was found, leaving `.ko.xz` behind
- **ARCH/CROSS_COMPILE warnings**: Both default to empty — no spurious warnings on native builds

### Driver Patches

- **Patch #13**: `atomic_cmpxchg` without double negation — `!= 0` instead of `== 1`
- **Patch #15**: `last_attempt_time` updated **after** successful `cmpxchg` — prevents rate-limit bypass on concurrent access
- **Patch #16**: `iterate_dir()` checks if `/proc/sensors` is empty before removal — prevents removing entries from other sensor drivers

---

## Kernel Compatibility

| API Change | Kernel | Solution |
|------------|--------|----------|
| `proc_ops` vs `file_operations` | 5.6 | `DHT_PROC_OPS` macro |
| `pde_data()` vs `PDE_DATA()` | 5.17 | `DHT_PDE_DATA` macro |

Minimum kernel: 5.0. Tested through 6.18+ (Raspberry Pi OS 6.18.50+rpt-rpi-2712).

---

## DKMS Support

`dkms.conf` with `AUTOINSTALL=yes` — module automatically rebuilds on kernel updates.

```bash
sudo dkms add dht/2.8.5
sudo dkms install dht/2.8.5
sudo modprobe dht
```

---

## Documentation

- **README.md** (EN) and **README_RU.md** (RU): 28 sections each
- DHT protocol architecture with timing diagrams
- Kernel compatibility and API stability tables
- 6 Bash examples + 5 Python examples + systemd service (3 methods)
- Architecture section: kref, preemption control, poll thread, safe unload
- Troubleshooting guide

---

## Files in This Release

| File | Description |
|------|-------------|
| `dht.c` | Driver source code (~2360 lines) |
| `Makefile` | Build, install/uninstall, cross-compile, check |
| `dkms.conf` | DKMS configuration |
| `LICENSE` | GNU General Public License v3 |
| `README.md` | Documentation (English) |
| `README_RU.md` | Documentation (Russian) |
| `changelog.txt` | Changelog (English) |
| `changelog_ru.txt` | Changelog (Russian) |
| `ReleaseNotes.md` | This file |

---

## Tested On

- Raspberry Pi 5 (kernel 6.18.50+rpt-rpi-2712)
- Raspberry Pi 4 (kernel 6.1.x)
- Raspberry Pi 3 (kernel 5.15.x)

---

## Installation

### Standard

```bash
make
sudo insmod dht.ko
echo 4 | sudo tee /proc/sensors/dht/export
cat /proc/sensors/dht/gpio4/value
```

### With modprobe

```bash
make install
sudo modprobe dht
```

### With DKMS

```bash
sudo mkdir -p /usr/src/dht-2.8.5
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.8.5/
sudo dkms add dht/2.8.5
sudo dkms install dht/2.8.5
sudo modprobe dht
```

---

## Upgrading from 2.8

### Standard install

```bash
make uninstall
make install
sudo modprobe dht
```

### DKMS install

```bash
sudo dkms remove dht/2.8.5 --all
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.8.5/
sudo dkms install dht/2.8.5
```

---

## Known Issues

- `modules_install` may print `Warning: missing 'System.map' file. Skipping depmod.` — Makefile now runs `depmod -a` manually after install
- On kernels that compress modules (5.12+), `dht.ko.xz` is installed instead of `dht.ko` — `make uninstall` handles all compression variants

---

## License

GNU General Public License v3. See [LICENSE](LICENSE).

Copyright (c) 2026, Chapvic
