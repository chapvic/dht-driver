# Release v2.9 - DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver

**Date:** 2026-09-23  
**License:** GPLv3  
**Kernel:** 5.0 - 6.18+

---

## Summary

Release v2.9 includes security hardening, concurrency fixes, and corrected documentation. All procfs write permissions are now restricted to root. The `kthread_stop()` sleeping-under-lock issue is resolved. A TOCTOU race condition in sensor registration is closed. Documentation now accurately reflects actual driver behavior and dmesg output.

---

## Security

- **procfs permissions hardened**
  - `debug`, `auto_interval`: `0666` -> `0644` (root-only write)
  - `export`, `unexport`, `measure`: `0222` -> `0200` (root-only write)
  - Previously, any user could register/unregister sensors and change global settings

---

## Fixes

### kthread_stop() out of list_lock

`sensor_interval_write()` previously called `dht_stop_poll()` under `list_lock`, which calls `kthread_stop()` — a sleeping function. This could cause scheduling issues under load.

**Fix:** The thread pointer is saved and `poll_thread` is cleared to NULL under the lock. `kthread_stop()` is called after `mutex_unlock()`. This is safe because:
- If a new poll thread is started between unlock and kthread_stop, it gets a new `task_struct` — the old thread stops independently
- `kthread_run()` (used by `dht_start_poll`) does not sleep, so it remains safe under `list_lock`

### NULL guard in dht_sensor_release

Added `if (sensor->proc_dir)` check before `proc_remove()` in the kref release callback. During module unload, `proc_dir` is set to NULL in Phase 1 (before `dht_sensor_put`), so the release path must handle this.

### TOCTOU race in sensor registration

Added a duplicate pin check under `list_lock` immediately before creating procfs entries. This closes the window between the initial check (at the start of `export`) and `proc_mkdir()`, where two threads could simultaneously pass the initial check and both create `gpio<pin>/` directories.

---

## Documentation

- **dmesg output corrected** to match actual code:
  - `[DHT]: DHT Driver (c) 2026, Chapvic (v2.9)`
  - `[DHT]: driver loaded - /proc/sensors/dht/ (max 32 sensors)`
- **Pre-build Checks** section: removed false claims about CONFIG checks; added runtime requirements note
- **Files in This Repository**: added LICENSE, changelog.txt, changelog_ru.txt, ReleaseNotes.md
- **Make Targets**: removed "config" from `make check` description
- **Version History**: added intermediate versions 2.8.1 through 2.8.5
- **Troubleshooting**: corrected CONFIG_GPIOLIB row and version references
- **README_RU.md**: full Russian translation (28 sections)

---

## Infrastructure

- `LICENSE` file added (GPLv3)
- `changelog.txt` / `changelog_ru.txt` created
- `ReleaseNotes.md` created

---

## Files

| File | Description |
|------|-------------|
| `dht.c` | Driver source (v2.9, ~2400 lines) |
| `Makefile` | Build system (tabs, not spaces) |
| `dkms.conf` | DKMS configuration (v2.9) |
| `LICENSE` | GPLv3 |
| `README.md` | Documentation (English, 28 sections) |
| `README_RU.md` | Documentation (Russian, 28 sections) |
| `changelog.txt` | Change log (English) |
| `changelog_ru.txt` | Change log (Russian) |
| `ReleaseNotes.md` | This file |

---

## Version History

| Version | Date | Key Changes |
|---------|------|-------------|
| 2.9 | 2026-09-23 | Security, kthread_stop fix, TOCTOU fix, docs |
| 2.8.5 | 2026-09-22 | Security, NULL guard, kthread_stop, TOCTOU |
| 2.8.4 | 2026-09-20 | DKMS (dkms.conf, AUTOINSTALL) |
| 2.8.3 | 2026-09-18 | Patches #13, #15, #16 |
| 2.8.2 | 2026-09-16 | Kernel 5.0+ compatibility (proc_ops, pde_data) |
| 2.8.1 | 2026-09-14 | Makefile (obj-m, check, uninstall, cross-build), DKMS |
| 2.8 | 2026-01-10 | Initial public release |

---

## Upgrade from 2.8.x

```bash
# If using DKMS:
sudo dkms remove dht/2.8.5 --all  # or your current version
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.9/
sudo dkms install dht/2.9

# If using insmod:
sudo rmmod dht
sudo insmod dht.ko
```

## Known Limitations

- Max 32 simultaneous sensors
- BCM pins 0-27 (Raspberry Pi)
- Min 2 seconds between measurements (hardware DHT constraint)
- Bit-bang read disables preemption (~4 ms)
- No GPIO chip hot-plug support
