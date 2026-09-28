# Release v2.0

## DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver

A Linux kernel driver for reading temperature and humidity data from DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins (bcm2835), Pi 4 (bcm2711), Pi 5 (bcm2712)

### Requirements

- Linux kernel 5.0+
- `CONFIG_GPIOLIB`, `CONFIG_PROC_FS`, `CONFIG_MODULES`
- Raspberry Pi OS, Ubuntu, Debian (tested on 5.0–6.18+)

### Files

| File | Description |
|------|-------------|
| `dht.c` | Driver source |
| `Makefile` | Build system with pre-build checks |
| `dkms.conf` | DKMS configuration |
| `LICENSE` | License (GPLv3) |
| `README.md` | Documentation (English) |
| `README_RU.md` | Documentation (Russian) |
| `ReleaseNotes.md` | Release notes |
| `VERSION` | Driver version |

### Installation

```bash
make
sudo insmod dht.ko
echo 4 | sudo tee /proc/sensors/dht/export
cat /proc/sensors/dht/gpio4/value
```

### DKMS

```bash
sudo mkdir -p /usr/src/dht-2.0
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.0/
sudo dkms add dht/2.0
sudo dkms install dht/2.0
sudo modprobe dht
```

### License

License: GPLv3

Copyright (c) 2026, Chapvic
