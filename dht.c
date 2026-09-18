/*
 * dht.c - DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver
 *
 * A Linux kernel module for reading temperature and humidity data from
 * DHT11, DHT22, and AM2302 sensors connected to Raspberry Pi GPIO pins.
 * The driver creates a procfs interface under /proc/sensors/dht/ for
 * managing sensor registration, configuration, and data retrieval.
 *
 * Copyright (c) 2026, Chapvic
 *
 * Version: 2.5
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Features:
 *   - Dynamic sensor export/unexport via procfs
 *   - Per-sensor proc entries for temperature, humidity, status and configuration
 *   - Background polling thread with configurable interval
 *   - Global auto-poll mode with shared interval
 *   - GPIO chip base caching for fast multi-sensor registration on Pi 3/4/5
 *   - Nanosecond-precision pulse timing for reliable reads across all Pi models
 *   - Rate limiting for manual measurements
 *
 *
 *  /proc/sensors/dht/
 *    debug         (rw) - debug logging: 0 = off (default), 1 = on
 *    version       (r)  - driver version
 *    export        (w)  - write BCM pin number to register a new sensor
 *    unexport      (w)  - write BCM pin number to unregister a sensor
 *    auto_interval (rw) - global auto-poll interval (2-60, -1 = off)
 *
 *  /proc/sensors/dht/gpio<pin>/
 *    pin           (r)  - BCM GPIO pin number
 *    interval      (rw) - auto-poll interval in seconds (2-60, -1 = off)
 *    measure       (w)  - write "1" to trigger measurement
 *                         (ignored if interval != -1 or global auto active)
 *    status_code   (r)  - error code (0 = success)
 *    status_text   (r)  - error description
 *    value         (r)  - "H=<humidity>\nT=<temperature>\n"
 *    info          (r)  - sensor type + registration time
 *    timestamp     (r)  - Unix timestamp of last measurement
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/ktime.h>

/* Driver metadata constants used in MODULE_* macros and dmesg output */
#define DHT_DRIVER_AUTHOR        "DHT Driver © 2026, Chapvic"
#define DHT_DRIVER_DESCRIPTION   "DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver"
#define DHT_DRIVER_VERSION       "2.5"
#define DHT_DRIVER_LICENSE       "GPL"

MODULE_LICENSE(DHT_DRIVER_LICENSE);
MODULE_AUTHOR(DHT_DRIVER_AUTHOR);
MODULE_DESCRIPTION(DHT_DRIVER_DESCRIPTION);
MODULE_VERSION(DHT_DRIVER_VERSION);

/* ── Timing and protocol constants ────────────────────────── */

#define MAX_TIMINGS      100        /* Maximum number of pulse transitions to capture in one read cycle */
#define PROC_PARENT      "sensors"  /* Parent procfs directory name (/proc/sensors) */
#define PROC_DIR_NAME    "dht"      /* Driver procfs directory name (/proc/sensors/dht) */
#define STATUS_BUF_LEN   128        /* Maximum length of the human-readable status text buffer */
#define LOG_BUF_LEN      256        /* Maximum length of log message buffers */
#define INFO_BUF_LEN     256        /* Maximum length of the sensor info text buffer */
#define MAX_SENSORS      32         /* Maximum number of simultaneously registered sensors */
#define MAX_PIN_NUM      27         /* Highest valid BCM GPIO pin number on Raspberry Pi */
#define MIN_INTERVAL     2          /* Minimum auto-poll interval in seconds (DHT sensors need >= 2s between reads) */
#define MAX_INTERVAL     60         /* Maximum auto-poll interval in seconds */
#define MEAS_MIN_GAP     2          /* Minimum seconds between manual measurements (rate limiting) */
#define MAX_RETRIES      3          /* Number of read attempts before giving up */
#define RETRY_DELAY_MS   100        /* Delay in milliseconds between read retries */
#define BIT_THRESHOLD    40000      /* Nanosecond threshold to distinguish 0 (~26 us) from 1 (~70 us) pulses */
#define PULSE_TIMEOUT_NS 200000     /* Maximum nanoseconds to wait for a single pulse before timing out (200 us) */

/* ── Error code definitions ───────────────────────────────── */

#define ERR_SUCCESS       0    /* Operation completed successfully */
#define ERR_PIN_INVALID   1    /* The specified GPIO pin number is out of valid range */
#define ERR_GPIO_REQUEST  2    /* Failed to request or find the GPIO descriptor */
#define ERR_READ_FAILED   3    /* Sensor data read failed (checksum error, timeout, etc.) */
#define ERR_AUTO_MODE     4    /* Manual measurement attempted while auto-poll is active */
#define ERR_TOO_SOON      5    /* Manual measurement rejected due to rate limiting */

/* ── Sensor type identifiers ──────────────────────────────── */

#define SENSOR_TYPE_UNKNOWN  0   /* Sensor type not yet determined (no successful measurement) */
#define SENSOR_TYPE_DHT11    1   /* DHT11 sensor: integer humidity/temperature, lower resolution */
#define SENSOR_TYPE_DHT22    2   /* DHT22/AM2302 sensor: decimal humidity/temperature, higher resolution */

/* ── Kernel API compatibility macros ─────────────────────── */

/*
 * Starting with kernel 5.6, procfs uses struct proc_ops instead of
 * struct file_operations. These macros select the correct type and
 * field names at compile time to maintain backward compatibility.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
  #define DHT_PROC_OPS    struct proc_ops
  #define DHT_PROC_READ   .proc_read
  #define DHT_PROC_WRITE  .proc_write
#else
  #define DHT_PROC_OPS    struct file_operations
  #define DHT_PROC_READ   .read
  #define DHT_PROC_WRITE  .write
#endif

/*
 * Starting with kernel 5.17, PDE_DATA() was renamed to pde_data().
 * This macro selects the correct function name at compile time.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
  #define DHT_PDE_DATA(inode)  pde_data(inode)
#else
  #define DHT_PDE_DATA(inode)  PDE_DATA(inode)
#endif

/* Module parameter: enable/disable debug logging via /proc/sensors/dht/debug or insmod */
static int dht_debug = 0;
module_param(dht_debug, int, 0644);
MODULE_PARM_DESC(dht_debug, "Debug logging (0 = off, 1 = on)");

/* ── Logging macros ─────────────────────────────────────────── */

/*
 * The following macros provide three levels of logging:
 * - dht_info: always printed (informational, KERN_INFO)
 * - dht_err:  always printed (error, KERN_ERR)
 * - dht_dbg:  only printed when dht_debug is set to 1
 * The pin_* variants prefix the log message with the GPIO pin number.
 */

#define dht_info(fmt, ...)  printk(KERN_INFO  "[DHT]: " fmt, ##__VA_ARGS__)
#define dht_err(fmt, ...)   printk(KERN_ERR   "[DHT]: " fmt, ##__VA_ARGS__)
#define dht_dbg(fmt, ...)   do { if (READ_ONCE(dht_debug)) printk(KERN_INFO "[DHT]: " fmt, ##__VA_ARGS__); } while (0)

#define pin_log(p, fmt, ...) printk(KERN_INFO  "[dht_gpio_%d]: " fmt, p, ##__VA_ARGS__)
#define pin_err(p, fmt, ...) printk(KERN_ERR   "[dht_gpio_%d]: " fmt, p, ##__VA_ARGS__)
#define pin_dbg(p, fmt, ...) do { if (READ_ONCE(dht_debug)) printk(KERN_INFO "[dht_gpio_%d]: " fmt, p, ##__VA_ARGS__); } while (0)

/* ── Time helper ───────────────────────────────────────────── */

/**
 * dht_format_iso_time - Convert a Unix timestamp to ISO 8601 string format
 * @seconds: Unix timestamp (seconds since epoch, UTC)
 * @buf:    Output buffer to store the formatted string
 * @size:   Size of the output buffer in bytes
 *
 * Converts a time64_t value into a human-readable ISO 8601 string
 * (e.g., "2026-01-15T14:30:00Z"). Used by the sensor info proc entry
 * to display the sensor registration time.
 */
static void dht_format_iso_time(time64_t seconds, char *buf, size_t size)
{
    struct tm tm;

    /* Convert Unix timestamp to broken-down time structure (UTC) */
    time64_to_tm(seconds, 0, &tm);

    /* Format as ISO 8601: YYYY-MM-DDThh:mm:ssZ */
    snprintf(buf, size, "%04ld-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ── Per-sensor state ──────────────────────────────────────── */

/*
 * struct dht_sensor - Represents one registered DHT sensor
 *
 * Each sensor instance maintains its own GPIO pin number, measurement
 * results, polling configuration, procfs entries, and a mutex for
 * thread-safe access from poll threads and procfs handlers.
 */
struct dht_sensor {
    int pin;                          /* BCM GPIO pin number the sensor is connected to */
    int interval;                     /* Auto-poll interval in seconds (-1 = disabled) */
    int humidity_raw;                 /* Last measured humidity (raw, scaled x10, e.g., 452 = 45.2%) */
    int temperature_raw;              /* Last measured temperature (raw, scaled x10, e.g., 231 = 23.1 C, negative for below zero) */
    int status_code;                  /* Result code of the last measurement attempt (ERR_*) */
    char status_text[STATUS_BUF_LEN]; /* Human-readable status text of the last measurement */
    int sensor_type;                  /* Detected sensor type (SENSOR_TYPE_*) */
    time64_t register_time;           /* Unix timestamp when the sensor was registered */
    time64_t last_meas_time;          /* Unix timestamp of the last successful measurement */
    time64_t last_attempt_time;       /* Unix timestamp of the last measurement attempt (used for rate limiting) */
    char info_text[INFO_BUF_LEN];     /* Pre-formatted info text (currently unused, reserved) */
    struct mutex lock;                /* Mutex protecting all fields above from concurrent access */
    struct proc_dir_entry *proc_dir;  /* Pointer to the sensor's procfs subdirectory (/proc/sensors/dht/gpio<pin>/) */
    struct task_struct *poll_thread;  /* Kernel thread for background polling (NULL if not running) */
    struct list_head list;            /* Linked list node for the global sensor list */
};

/* ── Global state ──────────────────────────────────────────── */

/*
 * Global variables shared across all sensor instances.
 * The list_lock mutex protects sensor_list and sensor_count.
 * cached_chip_base is a write-once variable: set during the first
 * sensor registration, then only read (using READ_ONCE) to avoid
 * repeated GPIO chip scanning on subsequent registrations.
 */
static struct proc_dir_entry *proc_parent;   /* /proc/sensors directory handle */
static struct proc_dir_entry *proc_dir;      /* /proc/sensors/dht directory handle */
static LIST_HEAD(sensor_list);               /* Head of the linked list of all registered sensors */
static DEFINE_MUTEX(list_lock);              /* Mutex protecting sensor_list and sensor_count */
static int sensor_count = 0;                 /* Current number of registered sensors */
static int global_auto_interval = -1;        /* Global auto-poll interval in seconds (-1 = disabled). When active, overrides per-sensor intervals */
static int cached_chip_base = -1;            /* Cached base GPIO number of the detected Pi GPIO chip. -1 = not yet detected */

/* ── Error text ────────────────────────────────────────────── */

/**
 * error_str - Return a human-readable description for an error code
 * @code: One of the ERR_* constants defined above
 *
 * Returns: A static string constant describing the error. This string
 *          is stored in the sensor's status_text field and displayed
 *          via the status_text proc entry.
 */
static const char *error_str(int code)
{
    switch (code) {
    case ERR_SUCCESS:       return "SUCCESS";
    case ERR_PIN_INVALID:   return "Pin must be between 0 and 27";
    case ERR_GPIO_REQUEST:  return "GPIO request/lookup failed";
    case ERR_READ_FAILED:   return "Sensor data read failed";
    case ERR_AUTO_MODE:     return "Manual measure disabled in auto mode";
    case ERR_TOO_SOON:      return "Too soon since last measurement (min 2s)";
    default:                return "Unknown error";
    }
}

/* ── GPIO chip detection ──────────────────────────────────── */

/**
 * is_pi_gpio_chip - Check whether a GPIO chip label belongs to a Raspberry Pi
 * @label: The GPIO chip label string (e.g., "pinctrl-rp1", "pinctrl-bcm2835")
 *
 * Raspberry Pi models use different GPIO controller chips:
 *   - Pi 5:  "pinctrl-rp1"   (RP1 south bridge, base offset >= 512)
 *   - Pi 4:  "pinctrl-bcm2711"
 *   - Pi 3:  "pinctrl-bcm2835"
 *   - Pi 0/1/2: "pinctrl-bcm2835"
 *
 * Returns: true if the label matches a known Raspberry Pi GPIO chip, false otherwise.
 */
static bool is_pi_gpio_chip(const char *label)
{
    if (!label)
        return false;
    if (strstr(label, "rp1"))     return true;   /* Raspberry Pi 5 */
    if (strstr(label, "bcm2835")) return true;   /* Raspberry Pi 1/2/3/Zero */
    if (strstr(label, "bcm2711")) return true;   /* Raspberry Pi 4 */
    if (strstr(label, "bcm2712")) return true;   /* Raspberry Pi 5 (alternate) */
    return false;
}

/**
 * dht_find_desc - Resolve a BCM pin number to a GPIO descriptor
 * @bcm_pin: BCM GPIO pin number (e.g., 23 for GPIO23)
 *
 * This function handles the difference in GPIO numbering between Pi models.
 * On Pi 3/4, the GPIO global numbers are small and match BCM numbers directly.
 * On Pi 5, the GPIO chip has a large base offset (e.g., 512), so the global
 * number for BCM pin 23 is base + 23 = 535.
 *
 * The function uses a three-stage lookup strategy:
 *   1. Fast path: if cached_chip_base is known, compute global = base + bcm_pin
 *   2. Direct lookup: try gpio_to_desc(bcm_pin) — works on Pi 3/4 where base=0
 *   3. Full scan: iterate over all GPIO numbers 0..2048 looking for a Pi chip
 *      where the local offset matches the requested BCM pin
 *
 * On first successful lookup, the chip's base number is cached so subsequent
 * registrations skip the expensive full scan.
 *
 * Returns: Pointer to the GPIO descriptor on success, NULL if not found.
 */
static struct gpio_desc *dht_find_desc(int bcm_pin)
{
    struct gpio_desc *desc;
    struct gpio_chip *chip;
    int i;

    /* Stage 1: Fast path using cached chip base.
     * This is the common case after the first sensor has been registered. */
    if (READ_ONCE(cached_chip_base) >= 0) {
        desc = gpio_to_desc(READ_ONCE(cached_chip_base) + bcm_pin);
        if (desc)
            return desc;
    }

    /* Stage 2: Direct lookup — works on Pi 3/4 where GPIO base = 0.
     * The BCM pin number equals the global GPIO number. */
    desc = gpio_to_desc(bcm_pin);
    if (desc) {
        chip = gpiod_to_chip(desc);
        /* Verify this is actually a Pi GPIO chip and cache its base */
        if (chip && chip->label && is_pi_gpio_chip(chip->label) &&
            chip->base >= 0 && (bcm_pin - chip->base) >= 0) {
            WRITE_ONCE(cached_chip_base, chip->base);
            return desc;
        }
    }

    /* Stage 3: Full scan — needed on Pi 5 where GPIO base is large (e.g., 512).
     * Iterate over all possible global GPIO numbers to find the Pi chip
     * whose local offset matches the requested BCM pin. */
    for (i = 0; i <= 2048; i++) {
        desc = gpio_to_desc(i);
        if (!desc)
            continue;

        chip = gpiod_to_chip(desc);
        if (!chip || !chip->label)
            continue;

        /* Skip non-Pi GPIO chips */
        if (!is_pi_gpio_chip(chip->label))
            continue;

        /* Check if the local offset within this chip matches our BCM pin */
        if (chip->base >= 0 && (i - chip->base) == bcm_pin) {
            /* Cache the chip base so future registrations use the fast path */
            WRITE_ONCE(cached_chip_base, chip->base);
            return desc;
        }
    }

    return NULL;
}

/* ── Sensor read ───────────────────────────────────────────── */

/**
 * dht_read_sensor - Read temperature and humidity from a DHT sensor
 * @pin:  BCM GPIO pin number the sensor is connected to
 * @hum:  Output parameter: humidity value (raw, scaled x10, e.g., 452 = 45.2%)
 * @temp: Output parameter: temperature value (raw, scaled x10, e.g., 231 = 23.1 C, negative if below zero)
 * @type: Output parameter: detected sensor type (SENSOR_TYPE_DHT11 or SENSOR_TYPE_DHT22). May be NULL.
 *
 * This function implements the single-wire DHT communication protocol:
 *   1. Pull the data line low for 20 ms to send the start signal
 *   2. Release the line and wait 40 us for the sensor to respond
 *   3. Read 40 data bits by measuring pulse widths using nanosecond timers
 *   4. Validate the 8-bit checksum
 *   5. Determine sensor type based on data format and extract values
 *
 * The function retries up to MAX_RETRIES times with RETRY_DELAY_MS delay
 * between attempts, as DHT sensors occasionally fail to respond.
 *
 * Bit decoding: a "0" bit has a ~26 us high pulse, a "1" bit has a ~70 us
 * high pulse. The BIT_THRESHOLD (40 us) distinguishes between them.
 *
 * Returns: ERR_SUCCESS on success, ERR_GPIO_REQUEST if GPIO operations fail,
 *          ERR_READ_FAILED if all retry attempts fail.
 */
static int dht_read_sensor(int pin, int *hum, int *temp, int *type)
{
    struct gpio_desc *desc;
    int data[5] = {0, 0, 0, 0, 0};   /* 5 bytes: RH_int, RH_dec, T_int, T_dec, checksum */
    int last_state = 1;              /* Current GPIO line state (1 = high, idle) */
    int i, j = 0;                    /* i: transition counter, j: bit counter (0-39) */
    u64 pulse_start, pulse_ns;       /* Nanosecond timestamps for pulse width measurement */
    int attempt;                     /* Current retry attempt number */

    /* Resolve the BCM pin number to a GPIO descriptor */
    desc = dht_find_desc(pin);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return ERR_GPIO_REQUEST;
    }

    /* Retry loop: DHT sensors sometimes fail to respond on the first attempt */
    for (attempt = 0; attempt < MAX_RETRIES; attempt++) {
        /* Clear data buffer and reset counters for each attempt */
        memset(data, 0, sizeof(data));
        last_state = 1;
        j = 0;

        /* Step 1: Send start signal — pull the data line low for 20 ms */
        if (gpiod_direction_output(desc, 0)) {
            pin_err(pin, "failed to set GPIO output\n");
            return ERR_GPIO_REQUEST;
        }

        gpiod_set_value(desc, 0);
        msleep(20);

        /* Step 2: Release the line — switch to input mode.
         * The pull-up resistor on the data line brings it high.
         * The sensor will respond after 20-40 us. */
        if (gpiod_direction_input(desc)) {
            pin_err(pin, "failed to set GPIO input\n");
            return ERR_GPIO_REQUEST;
        }

        /* Wait for the sensor to begin its response */
        udelay(40);

        /* Step 3: Read the 40 data bits by measuring pulse widths.
         * Each bit consists of a low pulse (~50 us) followed by a high pulse.
         * The high pulse duration determines the bit value:
         *   ~26 us = 0, ~70 us = 1. */
        for (i = 0; i < MAX_TIMINGS && j < 40; i++) {
            /* Record the start time of the current pulse level */
            pulse_start = ktime_get_ns();

            /* Wait until the line changes state or times out */
            while (gpiod_get_value(desc) == last_state) {
                pulse_ns = ktime_get_ns() - pulse_start;
                if (pulse_ns > PULSE_TIMEOUT_NS)
                    break;
            }
            if (pulse_ns > PULSE_TIMEOUT_NS)
                break;

            /* Update the expected line state for the next transition */
            last_state = gpiod_get_value(desc);

            /* Bits start after the first 4 transitions (sensor response handshake).
             * Data bits are captured on even-indexed transitions (high pulses).
             * i >= 4 skips the sensor's initial response signal. */
            if (i >= 4 && i % 2 == 0) {
                /* Shift the current byte left and set the LSB based on pulse width.
                 * If the high pulse is longer than BIT_THRESHOLD (40 us), it's a 1. */
                data[j / 8] <<= 1;
                if (pulse_ns > BIT_THRESHOLD)
                    data[j / 8] |= 1;
                j++;
            }
        }

        /* Step 4: Validate the checksum and extract values.
         * The 5th byte is the sum of the first 4 bytes (mod 256). */
        if (j >= 40 &&
            data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
            int h = ((data[0] << 8) + data[1]);  /* Combined humidity (16-bit) */
            int c = (((data[2] & 0x7F) << 8) + data[3]);  /* Combined temperature (16-bit, sign masked) */

            /* Determine sensor type based on the data format:
             * DHT11 sends integer values in byte 0 and 2, with byte 1 and 3 = 0.
             * When the combined 16-bit humidity > 1000, it indicates the raw
             * byte 0 value is > 100 (e.g., 45*256 + 0 = 11520 > 1000),
             * which means it's a DHT11 with integer-only format. */
            if (h > 1000) {
                if (type) *type = SENSOR_TYPE_DHT11;
                /* DHT11: use only the integer byte, scale by 10 for consistent units */
                h = data[0] * 10;
            } else {
                if (type) *type = SENSOR_TYPE_DHT22;
                /* DHT22: the 16-bit value is already scaled x10 */
            }

            /* Same heuristic for temperature: if the 16-bit value > 1250,
             * it's a DHT11 with integer-only format. */
            if (c > 1250)
                c = data[2] * 10;

            /* Handle negative temperature (bit 7 of data[2] is the sign bit) */
            if (data[2] & 0x80)
                c = -c;

            *hum = h;
            *temp = c;
            return ERR_SUCCESS;
        }

        /* Log the failed attempt for debugging */
        pin_dbg(pin, "read attempt %d failed - j=%d, data=[%d,%d,%d,%d,%d]\n",
                attempt + 1, j, data[0], data[1], data[2], data[3], data[4]);

        /* Wait before retrying (except after the last attempt) */
        if (attempt < MAX_RETRIES - 1)
            msleep(RETRY_DELAY_MS);
    }

    /* All retry attempts exhausted */
    pin_err(pin, "read failed after %d attempts - j=%d, data=[%d,%d,%d,%d,%d]\n",
            MAX_RETRIES, j, data[0], data[1], data[2], data[3], data[4]);
    return ERR_READ_FAILED;
}

/* ── Measurement ───────────────────────────────────────────── */

/**
 * dht_do_measurement - Perform a single sensor measurement and store results
 * @sensor: Pointer to the sensor instance to measure
 * @manual: true if this is a user-triggered manual measurement (subject to rate limiting),
 *          false if this is an automatic background poll (no rate limiting)
 *
 * This function is the central measurement dispatcher, called from:
 *   - export_write() during sensor registration (manual = false)
 *   - sensor_measure_write() for user-triggered measurements (manual = true)
 *   - dht_poll_thread_fn() for background polling (manual = false)
 *
 * It manages locking carefully: acquires sensor->lock for the rate-limit
 * check and for storing results, but releases the lock during the actual
 * GPIO read (which takes 20+ ms) to avoid blocking other operations on
 * the sensor (e.g., procfs reads).
 *
 * On success, updates sensor->humidity_raw, temperature_raw, sensor_type,
 * last_meas_time, status_code, and status_text. On failure, only status
 * fields are updated.
 */
static void dht_do_measurement(struct dht_sensor *sensor, bool manual)
{
    int hum = 0, temp = 0, type = 0;
    int ret;
    time64_t now;

    /* Acquire the sensor lock for the rate-limit check */
    mutex_lock(&sensor->lock);

    /* Validate the pin number is within the allowed range */
    if (sensor->pin < 0 || sensor->pin > MAX_PIN_NUM) {
        sensor->status_code = ERR_PIN_INVALID;
        snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ERR_PIN_INVALID));
        mutex_unlock(&sensor->lock);
        return;
    }

    /* Rate limiting: reject manual measurements that come too soon after
     * the previous attempt. Background polls (manual = false) bypass this. */
    if (manual) {
        now = ktime_get_real_seconds();
        if (sensor->last_attempt_time > 0 &&
            (now - sensor->last_attempt_time) < MEAS_MIN_GAP) {
            sensor->status_code = ERR_TOO_SOON;
            snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ERR_TOO_SOON));
            pin_dbg(sensor->pin, "manual measurement rejected - only %llds since last\n",
                    (long long)(now - sensor->last_attempt_time));
            mutex_unlock(&sensor->lock);
            return;
        }
    }

    /* Record the attempt timestamp for rate limiting */
    sensor->last_attempt_time = ktime_get_real_seconds();

    /* Release the lock during the actual sensor read to avoid blocking.
     * The read takes 20+ ms, which is too long to hold a mutex. */
    mutex_unlock(&sensor->lock);

    /* Perform the actual GPIO read (may take multiple retries) */
    ret = dht_read_sensor(sensor->pin, &hum, &temp, &type);

    /* Re-acquire the lock to store the results */
    mutex_lock(&sensor->lock);
    sensor->status_code = ret;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ret));

    if (ret == ERR_SUCCESS) {
        /* Update measurement results on success */
        sensor->humidity_raw = hum;
        sensor->temperature_raw = temp;
        sensor->sensor_type = type;
        sensor->last_meas_time = sensor->last_attempt_time;
        pin_dbg(sensor->pin, "measurement OK - H=%d.%d%% T=%d.%d C\n",
                hum / 10, hum % 10, temp / 10, temp % 10);
    } else {
        /* Log the failure reason (debug only) */
        pin_dbg(sensor->pin, "measurement failed - %s\n", error_str(ret));
    }
    mutex_unlock(&sensor->lock);
}

/* ── Poll thread ───────────────────────────────────────────── */

/**
 * dht_poll_thread_fn - Background polling thread function
 * @data: Pointer to the dht_sensor struct this thread is polling
 *
 * This kernel thread runs in a loop, calling dht_do_measurement() at
 * the configured interval. The interval is determined by:
 *   - global_auto_interval (if active, takes priority)
 *   - sensor->interval (per-sensor setting, used when global is off)
 *
 * The thread sleeps in 1-second increments so it can respond quickly
 * to kthread_should_stop() (e.g., when the sensor is unregistered or
 * the driver is unloaded).
 *
 * Returns: 0 when the thread exits (always, as the loop only exits
 *          when kthread_should_stop() returns true).
 */
static int dht_poll_thread_fn(void *data)
{
    struct dht_sensor *sensor = data;

    pin_dbg(sensor->pin, "poll thread started\n");

    /* Main polling loop — runs until the thread is asked to stop */
    while (!kthread_should_stop()) {
        int effective_interval;

        /* Perform one measurement cycle */
        dht_do_measurement(sensor, false);

        /* Determine the effective poll interval under the sensor lock */
        mutex_lock(&sensor->lock);
        if (READ_ONCE(global_auto_interval) != -1)
            effective_interval = READ_ONCE(global_auto_interval);
        else
            effective_interval = sensor->interval;
        mutex_unlock(&sensor->lock);

        /* Safety fallback: ensure a minimum interval */
        if (effective_interval < 1)
            effective_interval = MIN_INTERVAL;

        /* Sleep in 1-second increments so we can respond to stop requests promptly.
         * This avoids blocking for the full interval if the sensor is unregistered. */
        {
            int slept = 0;
            while (slept < effective_interval && !kthread_should_stop()) {
                ssleep(1);
                slept++;
            }
        }
    }

    pin_dbg(sensor->pin, "poll thread stopped\n");
    return 0;
}

/**
 * dht_start_poll - Start the background polling thread for a sensor
 * @sensor: Pointer to the sensor instance to start polling
 *
 * Creates and starts a kernel thread named "dht_poll_<pin>" that will
 * periodically call dht_do_measurement(). If a thread is already running,
 * this function is a no-op.
 */
static void dht_start_poll(struct dht_sensor *sensor)
{
    /* Don't start a duplicate thread if one is already running */
    if (sensor->poll_thread)
        return;

    /* Create and start the kernel thread */
    sensor->poll_thread = kthread_run(dht_poll_thread_fn, sensor,
                                      "dht_poll_%d", sensor->pin);
    if (IS_ERR(sensor->poll_thread)) {
        pin_err(sensor->pin, "failed to create poll thread\n");
        sensor->poll_thread = NULL;
    }
}

/**
 * dht_stop_poll - Stop the background polling thread for a sensor
 * @sensor: Pointer to the sensor instance to stop polling
 *
 * Stops the kernel thread associated with this sensor by calling
 * kthread_stop(), which signals the thread to exit and waits for it.
 * The poll_thread pointer is cleared before calling kthread_stop to
 * avoid re-entry issues (the thread checks poll_thread indirectly
 * via kthread_should_stop()).
 */
static void dht_stop_poll(struct dht_sensor *sensor)
{
    struct task_struct *thread;

    if (!sensor->poll_thread)
        return;

    /* Save the thread pointer and clear the field first.
     * kthread_stop() may sleep until the thread exits. */
    thread = sensor->poll_thread;
    sensor->poll_thread = NULL;
    kthread_stop(thread);
}

/* ── Unified sensor cleanup ────────────────────────────────── */

/**
 * dht_sensor_free - Free all resources associated with a sensor
 * @sensor: Pointer to the sensor instance to free
 *
 * This function performs the complete teardown of a sensor:
 *   1. Stops the background polling thread (if running)
 *   2. Removes the sensor's procfs directory and all its entries
 *   3. Destroys the sensor's mutex
 *   4. Frees the sensor struct memory
 *
 * It is called from:
 *   - export_write() when initial measurement fails after registration
 *   - unexport_write() when the user unregisters a sensor
 *   - dht_driver_exit() during module unload
 */
static void dht_sensor_free(struct dht_sensor *sensor)
{
    dht_stop_poll(sensor);              /* Stop the background polling thread */
    proc_remove(sensor->proc_dir);      /* Remove all procfs entries for this sensor */
    mutex_destroy(&sensor->lock);       /* Clean up the mutex */
    kfree(sensor);                      /* Free the sensor struct */
}

/* ── Input parsing helper ──────────────────────────────────── */

/**
 * dht_parse_int - Parse an integer value from user-space input
 * @buf:   User-space buffer containing the input string
 * @count: Number of bytes to read from the buffer
 * @val:   Output parameter: parsed integer value
 *
 * Copies up to 15 bytes from user space, null-terminates the string,
 * trims whitespace, and converts to an integer using kstrtoint().
 *
 * Returns: 0 on success, -EFAULT if copy_from_user fails,
 *          -EINVAL if the string is not a valid integer.
 */
static int dht_parse_int(const char __user *buf, size_t count, int *val)
{
    char in[16];

    /* Truncate input to fit the buffer (leave room for null terminator) */
    if (count >= sizeof(in))
        count = sizeof(in) - 1;

    /* Copy the user-space data into the kernel buffer */
    if (copy_from_user(in, buf, count))
        return -EFAULT;
    in[count] = '\0';

    /* Trim whitespace and convert to a base-10 integer */
    return kstrtoint(strim(in), 10, val) ? -EINVAL : 0;
}

/* ── Global procfs: debug ───────────────────────────────────── */

/**
 * debug_read - Read the current debug flag value
 * @f:     File structure (unused — no per-file state)
 * @buf:   User-space buffer to write the result to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the current value of dht_debug (0 or 1) as a decimal string
 * followed by a newline. The *pos check ensures the output is only
 * returned once per read() call.
 *
 * Returns: Number of bytes written to the user buffer, 0 at EOF.
 */
static ssize_t debug_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    char out[16];
    int len;

    /* Return EOF on subsequent reads (after the first call) */
    if (*pos > 0)
        return 0;

    /* Format the current debug flag value */
    len = snprintf(out, sizeof(out), "%d\n", READ_ONCE(dht_debug));

    /* Copy the formatted string to user space */
    if (copy_to_user(buf, out, len))
        return -EFAULT;

    /* Advance the file offset so subsequent reads return EOF */
    *pos += len;
    return len;
}

/**
 * debug_write - Set the debug flag value
 * @f:     File structure (unused)
 * @buf:   User-space buffer containing the new value ("0" or "1")
 * @count: Number of bytes in the user buffer
 * @pos:   File offset (unused)
 *
 * Parses the input as an integer and validates it is 0 or 1.
 * Uses WRITE_ONCE to safely update the global dht_debug variable,
 * which is checked by the dht_dbg/pin_dbg macros.
 *
 * Returns: Number of bytes consumed on success, or a negative error code.
 */
static ssize_t debug_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int val;

    /* Parse the user input as an integer */
    int ret = dht_parse_int(buf, count, &val);
    if (ret)
        return ret;

    /* Validate the value is 0 (off) or 1 (on) */
    if (val != 0 && val != 1)
        return -EINVAL;

    /* Atomically update the debug flag */
    WRITE_ONCE(dht_debug, val);
    dht_info("debug %s\n", val ? "enabled" : "disabled");
    return count;
}

/* File operations for the /proc/sensors/dht/debug entry */
static const DHT_PROC_OPS debug_fops = {
    DHT_PROC_READ  = debug_read,
    DHT_PROC_WRITE = debug_write,
};

/* ── Global procfs: version ────────────────────────────────── */

/**
 * version_read - Read the driver version string
 * @f:     File structure (unused)
 * @buf:   User-space buffer to write the version string to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the driver version (e.g., "2.5\n") to the user buffer.
 *
 * Returns: Number of bytes written on first call, 0 on subsequent calls.
 */
static ssize_t version_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    char out[32];
    int len;

    if (*pos > 0)
        return 0;

    /* Format the driver version string */
    len = snprintf(out, sizeof(out), "%s\n", DHT_DRIVER_VERSION);
    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/* File operations for the /proc/sensors/dht/version entry */
static const DHT_PROC_OPS version_fops = {
    DHT_PROC_READ = version_read,
};

/* ── Global procfs: auto_interval ──────────────────────────── */

/**
 * auto_interval_read - Read the current global auto-poll interval
 * @f:     File structure (unused)
 * @buf:   User-space buffer to write the value to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the current global_auto_interval value (seconds, or -1 if disabled).
 *
 * Returns: Number of bytes written on first call, 0 on subsequent calls.
 */
static ssize_t auto_interval_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    char out[16];
    int len;

    if (*pos > 0)
        return 0;

    /* Format the current global auto-poll interval */
    len = snprintf(out, sizeof(out), "%d\n", READ_ONCE(global_auto_interval));
    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/**
 * auto_interval_write - Set the global auto-poll interval
 * @f:     File structure (unused)
 * @buf:   User-space buffer containing the new interval value
 * @count: Number of bytes in the user buffer
 * @pos:   File offset (unused)
 *
 * When set to a valid interval (2-60 seconds), all registered sensors
 * start (or continue) background polling at this shared interval.
 * When set to -1 (or any out-of-range value), global auto-poll is
 * disabled. Existing poll threads are NOT stopped — per-sensor
 * intervals take over if they are active.
 *
 * The function also starts poll threads for any registered sensors
 * that don't have one yet when enabling global auto mode.
 *
 * Returns: Number of bytes consumed on success, or a negative error code.
 */
static ssize_t auto_interval_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret)
        return ret;

    /* Acquire the list lock to safely modify global state and sensor list */
    mutex_lock(&list_lock);

    /* Set the global interval or disable it if the value is out of range */
    if (val >= MIN_INTERVAL && val <= MAX_INTERVAL) {
        WRITE_ONCE(global_auto_interval, val);
    } else {
        WRITE_ONCE(global_auto_interval, -1);
    }

    /* If global auto mode is now active, start poll threads for all sensors
     * that don't have one running yet. */
    if (READ_ONCE(global_auto_interval) != -1) {
        struct dht_sensor *sensor;
        list_for_each_entry(sensor, &sensor_list, list) {
            if (!sensor->poll_thread)
                dht_start_poll(sensor);
        }
    }

    mutex_unlock(&list_lock);

    dht_info("auto_interval set to %d\n", READ_ONCE(global_auto_interval));
    return count;
}

/* File operations for the /proc/sensors/dht/auto_interval entry */
static const DHT_PROC_OPS auto_interval_fops = {
    DHT_PROC_READ  = auto_interval_read,
    DHT_PROC_WRITE = auto_interval_write,
};

/* ── Per-sensor procfs handlers ────────────────────────────── */

/*
 * The following two macros generate read handler functions for
 * integer and 64-bit fields of struct dht_sensor. Each generated
 * function reads the field under the sensor mutex and outputs it
 * as a decimal string.
 *
 * SENSOR_INT_READ(field) — generates sensor_<field>_read() for int fields
 * SENSOR_LL_READ(field)  — generates sensor_<field>_read() for time64_t fields
 *
 * Both use DHT_PDE_DATA to retrieve the sensor pointer from the
 * procfs inode, and both respect the *pos offset for sequential reads.
 */

/**
 * SENSOR_INT_READ - Macro to generate a procfs read handler for an integer sensor field
 * @field: Name of the int field in struct dht_sensor
 *
 * Generates a function named sensor_<field>_read() that outputs the
 * field value as "%d\n". The sensor pointer is obtained from the
 * procfs inode's private data (set via proc_create_data).
 */
#define SENSOR_INT_READ(field) \
static ssize_t sensor_##field##_read(struct file *f, char __user *buf, size_t count, loff_t *pos) \
{ \
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f)); \
    char out[16]; \
    int len; \
    if (*pos > 0) return 0; \
    mutex_lock(&sensor->lock); \
    len = snprintf(out, sizeof(out), "%d\n", sensor->field); \
    mutex_unlock(&sensor->lock); \
    if (copy_to_user(buf, out, len)) return -EFAULT; \
    *pos += len; \
    return len; \
}

/**
 * SENSOR_LL_READ - Macro to generate a procfs read handler for a 64-bit sensor field
 * @field: Name of the time64_t field in struct dht_sensor
 *
 * Generates a function named sensor_<field>_read() that outputs the
 * field value as "%lld\n". Used for timestamp fields that require
 * 64-bit representation.
 */
#define SENSOR_LL_READ(field) \
static ssize_t sensor_##field##_read(struct file *f, char __user *buf, size_t count, loff_t *pos) \
{ \
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f)); \
    char out[32]; \
    int len; \
    if (*pos > 0) return 0; \
    mutex_lock(&sensor->lock); \
    len = snprintf(out, sizeof(out), "%lld\n", (long long)sensor->field); \
    mutex_unlock(&sensor->lock); \
    if (copy_to_user(buf, out, len)) return -EFAULT; \
    *pos += len; \
    return len; \
}

/* Generate read handlers for integer fields */
SENSOR_INT_READ(pin)           /* sensor_pin_read() — outputs the BCM GPIO pin number */
SENSOR_INT_READ(status_code)   /* sensor_status_code_read() — outputs the last measurement error code */

/* Generate read handlers for 64-bit timestamp fields */
SENSOR_LL_READ(last_meas_time) /* sensor_last_meas_time_read() — outputs Unix timestamp of last successful measurement */

/**
 * sensor_interval_read - Read the per-sensor auto-poll interval
 * @f:     File structure (unused, sensor data comes from inode)
 * @buf:   User-space buffer to write the value to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the sensor's interval value (seconds, or -1 if disabled).
 * This is the per-sensor setting, which is overridden by global_auto_interval
 * when the latter is active.
 *
 * Returns: Number of bytes written on first call, 0 on subsequent calls.
 */
static ssize_t sensor_interval_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[16];
    int len;

    if (*pos > 0)
        return 0;

    /* Read the interval value under the sensor lock for consistency */
    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%d\n", sensor->interval);
    mutex_unlock(&sensor->lock);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/**
 * sensor_interval_write - Set the per-sensor auto-poll interval
 * @f:     File structure (unused, sensor data comes from inode)
 * @buf:   User-space buffer containing the new interval value
 * @count: Number of bytes in the user buffer
 * @pos:   File offset (unused)
 *
 * Accepts values:
 *   -1:       disable auto-poll for this sensor (stops the poll thread)
 *   2-60:     enable auto-poll with the given interval (starts the poll thread)
 *   other:    rejected with -EINVAL
 *
 * If global auto-poll is active, the per-sensor interval is stored but
 * not used for polling (the global interval takes priority). In that case,
 * the poll thread is not started or stopped — the global mode controls it.
 *
 * Returns: Number of bytes consumed on success, or a negative error code.
 */
static ssize_t sensor_interval_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret)
        return ret;

    /* Validate: -1 (disabled) or within [MIN_INTERVAL, MAX_INTERVAL] */
    if (val != -1 && (val < MIN_INTERVAL || val > MAX_INTERVAL))
        return -EINVAL;

    /* Store the new interval under the sensor lock */
    mutex_lock(&sensor->lock);
    sensor->interval = val;
    mutex_unlock(&sensor->lock);

    /* If global auto mode is active, the local interval is stored but
     * the global setting controls polling. No thread changes needed. */
    if (READ_ONCE(global_auto_interval) != -1) {
        pin_dbg(sensor->pin, "local interval ignored - global auto active\n");
        return count;
    }

    /* Global auto mode is off — the per-sensor setting controls polling */
    if (val == -1) {
        /* Disable polling: stop the thread if running */
        dht_stop_poll(sensor);
        pin_log(sensor->pin, "auto-poll disabled\n");
    } else {
        /* Enable polling: start the thread with the new interval */
        dht_start_poll(sensor);
        pin_log(sensor->pin, "auto-poll enabled (interval=%d)\n", val);
    }
    return count;
}

/**
 * sensor_measure_write - Trigger a manual measurement
 * @f:     File structure (unused, sensor data comes from inode)
 * @buf:   User-space buffer — must contain "1" to trigger
 * @count: Number of bytes in the user buffer
 * @pos:   File offset (unused)
 *
 * Writing "1" to the measure entry triggers an immediate sensor read.
 * This is only allowed when auto-poll is disabled (both per-sensor
 * interval and global auto_interval are -1). If either is active,
 * the manual measurement is rejected with ERR_AUTO_MODE to avoid
 * conflicting with the poll thread.
 *
 * Manual measurements are also subject to rate limiting: at least
 * MEAS_MIN_GAP seconds must pass since the last attempt.
 *
 * Returns: Number of bytes consumed on success, or a negative error code.
 */
static ssize_t sensor_measure_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret)
        return ret;

    /* Only the value 1 is accepted as a trigger command */
    if (val != 1)
        return -EINVAL;

    /* Check if auto-poll is active (per-sensor or global).
     * If so, reject the manual measurement to avoid conflicts. */
    mutex_lock(&sensor->lock);
    if (sensor->interval != -1 || READ_ONCE(global_auto_interval) != -1) {
        sensor->status_code = ERR_AUTO_MODE;
        snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ERR_AUTO_MODE));
        pin_dbg(sensor->pin, "manual measure ignored (auto mode)\n");
        mutex_unlock(&sensor->lock);
        return count;
    }
    mutex_unlock(&sensor->lock);

    /* Auto-poll is off — perform the manual measurement.
     * dht_do_measurement handles rate limiting internally. */
    dht_do_measurement(sensor, true);
    return count;
}

/**
 * sensor_status_text_read - Read the human-readable status text
 * @f:     File structure (unused, sensor data comes from inode)
 * @buf:   User-space buffer to write the status text to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the sensor's status_text field (e.g., "SUCCESS", "Sensor data read failed")
 * followed by a newline. This is the human-readable version of status_code.
 *
 * Returns: Number of bytes written on first call, 0 on subsequent calls.
 */
static ssize_t sensor_status_text_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[STATUS_BUF_LEN + 2];
    int len;

    if (*pos > 0)
        return 0;

    /* Read the status text under the sensor lock */
    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%s\n", sensor->status_text);
    mutex_unlock(&sensor->lock);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/**
 * sensor_value_read - Read formatted temperature and humidity values
 * @f:     File structure (unused, sensor data comes from inode)
 * @buf:   User-space buffer to write the formatted values to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the last measured values in the format:
 *   "H=<humidity>\nT=<temperature>\n"
 * where humidity is in percent and temperature is in degrees Celsius,
 * both with one decimal place. Negative temperatures are prefixed with "-".
 *
 * Returns: Number of bytes written on first call, 0 on subsequent calls.
 */
static ssize_t sensor_value_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[64];
    int len, hum, temp;

    if (*pos > 0)
        return 0;

    /* Read the raw values under the sensor lock */
    mutex_lock(&sensor->lock);
    hum = sensor->humidity_raw;
    temp = sensor->temperature_raw;
    mutex_unlock(&sensor->lock);

    /* Format the output string. Values are scaled x10, so integer part = val/10,
     * decimal part = val%10. Handle negative temperatures separately. */
    if (temp < 0)
        len = snprintf(out, sizeof(out), "H=%d.%d\nT=-%d.%d\n",
                       hum / 10, hum % 10, (-temp) / 10, (-temp) % 10);
    else
        len = snprintf(out, sizeof(out), "H=%d.%d\nT=%d.%d\n",
                       hum / 10, hum % 10, temp / 10, temp % 10);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/**
 * sensor_info_read - Read sensor metadata (type and registration time)
 * @f:     File structure (unused, sensor data comes from inode)
 * @buf:   User-space buffer to write the info text to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the sensor type (DHT11 or DHT22) and registration timestamp
 * in ISO 8601 format. If the sensor type has not been determined yet
 * (no successful measurement), returns an empty output (EOF immediately).
 *
 * Returns: Number of bytes written on first call, 0 if no data or on subsequent calls.
 */
static ssize_t sensor_info_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[INFO_BUF_LEN];
    int len;
    time64_t reg_time_copy;
    int type_copy;

    if (*pos > 0)
        return 0;

    /* Read sensor metadata under the lock.
     * If the sensor type is unknown (no successful measurement yet),
     * return nothing. */
    mutex_lock(&sensor->lock);
    if (sensor->sensor_type == SENSOR_TYPE_UNKNOWN) {
        mutex_unlock(&sensor->lock);
        return 0;
    }

    /* Copy values to local variables before formatting (minimize lock hold time) */
    reg_time_copy = sensor->register_time;
    type_copy = sensor->sensor_type;
    mutex_unlock(&sensor->lock);

    /* Format the registration time as ISO 8601 and build the info string */
    {
        char time_buf[32];
        dht_format_iso_time(reg_time_copy, time_buf, sizeof(time_buf));
        len = snprintf(out, sizeof(out),
                       "Sensor type: DHT%d\nRegister time: %s\n",
                       (type_copy == SENSOR_TYPE_DHT11 ? 11 : 22), time_buf);
    }

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/* ── Per-sensor fops ───────────────────────────────────────── */

/*
 * Each entry below binds a procfs file to its read/write handler functions.
 * These are used by proc_create_data() to set up the per-sensor proc entries.
 */
static const DHT_PROC_OPS sensor_pin_fops         = { DHT_PROC_READ = sensor_pin_read };
static const DHT_PROC_OPS sensor_interval_fops    = { DHT_PROC_READ = sensor_interval_read, DHT_PROC_WRITE = sensor_interval_write };
static const DHT_PROC_OPS sensor_measure_fops     = { DHT_PROC_WRITE = sensor_measure_write };
static const DHT_PROC_OPS sensor_status_code_fops = { DHT_PROC_READ = sensor_status_code_read };
static const DHT_PROC_OPS sensor_status_text_fops = { DHT_PROC_READ = sensor_status_text_read };
static const DHT_PROC_OPS sensor_value_fops       = { DHT_PROC_READ = sensor_value_read };
static const DHT_PROC_OPS sensor_info_fops        = { DHT_PROC_READ = sensor_info_read };
static const DHT_PROC_OPS sensor_timestamp_fops   = { DHT_PROC_READ = sensor_last_meas_time_read };

/* ── Proc entry tables ────────────────────────────────────── */

/*
 * struct proc_entry_def - Describes a procfs entry to be created
 * @name:  File name in the procfs directory
 * @mode:  File permissions (e.g., 0444 = read-only, 0644 = read-write)
 * @fops:  Pointer to the file operations structure for this entry
 *
 * This structure is used by the proc entry creation functions to
 * iterate over a table of entries and create them in a loop, rather
 * than calling proc_create_data() individually for each entry.
 */
struct proc_entry_def {
    const char *name;
    umode_t mode;
    const DHT_PROC_OPS *fops;
};

/* Table of per-sensor proc entries created under /proc/sensors/dht/gpio<pin>/ */
static const struct proc_entry_def sensor_proc_entries[] = {
    { "pin",         0444, &sensor_pin_fops },
    { "interval",    0644, &sensor_interval_fops },
    { "measure",     0222, &sensor_measure_fops },
    { "status_code", 0444, &sensor_status_code_fops },
    { "status_text", 0444, &sensor_status_text_fops },
    { "value",       0444, &sensor_value_fops },
    { "info",        0444, &sensor_info_fops },
    { "timestamp",   0444, &sensor_timestamp_fops },
};

/* ── Export / Unexport ────────────────────────────────────── */

/**
 * dht_create_sensor_proc - Create the per-sensor procfs directory and entries
 * @sensor: Pointer to the sensor instance (must have pin field set)
 *
 * Creates a subdirectory named "gpio<pin>" under the driver's procfs
 * directory, then creates all per-sensor proc entries (pin, interval,
 * measure, status_code, status_text, value, info, timestamp) inside it.
 * Each entry is created with the sensor pointer as its private data,
 * so the read/write handlers can retrieve the sensor context.
 *
 * Returns: 0 on success, -ENOMEM if any proc entry or directory cannot be created.
 *          On failure, any partially created entries are removed.
 */
static int dht_create_sensor_proc(struct dht_sensor *sensor)
{
    char name[32];
    int i;

    /* Create the sensor's subdirectory: /proc/sensors/dht/gpio<pin>/ */
    snprintf(name, sizeof(name), "gpio%d", sensor->pin);
    sensor->proc_dir = proc_mkdir(name, proc_dir);
    if (!sensor->proc_dir)
        return -ENOMEM;

    /* Create all per-sensor proc entries inside the subdirectory */
    for (i = 0; i < ARRAY_SIZE(sensor_proc_entries); i++) {
        const struct proc_entry_def *e = &sensor_proc_entries[i];
        if (!proc_create_data(e->name, e->mode, sensor->proc_dir, e->fops, sensor)) {
            /* If any entry fails, clean up the entire directory */
            proc_remove(sensor->proc_dir);
            return -ENOMEM;
        }
    }
    return 0;
}

/**
 * export_write - Register a new sensor by writing a BCM pin number
 * @f:     File structure (unused)
 * @buf:   User-space buffer containing the BCM pin number as a string
 * @count: Number of bytes in the user buffer
 * @pos:   File offset (unused)
 *
 * This is the handler for writing to /proc/sensors/dht/export.
 * It performs the full sensor registration sequence:
 *   1. Parse and validate the pin number (0-27)
 *   2. Check for duplicate registration and sensor count limit
 *   3. Resolve the GPIO descriptor (with chip base caching)
 *   4. Allocate and initialize the sensor struct
 *   5. Create procfs entries for the sensor
 *   6. Perform an initial measurement to validate the sensor works
 *   7. Add the sensor to the global list
 *   8. Start auto-polling if global auto mode is active
 *
 * If the initial measurement fails, the sensor is not registered and
 * all allocated resources are freed.
 *
 * Returns: Number of bytes consumed on success, or a negative error code:
 *          -EINVAL (invalid pin), -EBUSY (already registered),
 *          -ENOMEM (max sensors or allocation failure),
 *          -ENODEV (GPIO not found), -EIO (measurement failed).
 */
static ssize_t export_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int pin;
    struct dht_sensor *sensor, *s;
    struct gpio_desc *desc;
    struct gpio_chip *chip;
    int ret;

    /* Step 1: Parse the pin number from user input */
    ret = dht_parse_int(buf, count, &pin);
    if (ret)
        return ret;
    if (pin < 0 || pin > MAX_PIN_NUM)
        return -EINVAL;

    /* Step 2: Check for duplicate registration and sensor count limit.
     * These checks are done under list_lock to prevent race conditions. */
    mutex_lock(&list_lock);
    list_for_each_entry(s, &sensor_list, list) {
        if (s->pin == pin) {
            mutex_unlock(&list_lock);
            dht_dbg("pin %d already registered\n", pin);
            return -EBUSY;
        }
    }
    if (sensor_count >= MAX_SENSORS) {
        mutex_unlock(&list_lock);
        dht_err("max sensors (%d) reached\n", MAX_SENSORS);
        return -ENOMEM;
    }
    mutex_unlock(&list_lock);

    /* Step 3: Resolve the GPIO descriptor for this BCM pin.
     * On first call, this may trigger a full GPIO chip scan.
     * Subsequent calls use the cached chip base for fast lookup. */
    desc = dht_find_desc(pin);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return -ENODEV;
    }

    /* Log which GPIO chip the pin was found on (for diagnostics) */
    chip = gpiod_to_chip(desc);
    if (chip && chip->label)
        pin_log(pin, "found on '%s' (base=%d, global=%d)\n",
                chip->label, chip->base, chip->base + pin);
    else
        pin_log(pin, "found (global=%d)\n", pin);

    /* Step 4: Allocate and initialize the sensor struct */
    sensor = kzalloc(sizeof(*sensor), GFP_KERNEL);
    if (!sensor)
        return -ENOMEM;

    /* Initialize sensor fields with default values */
    sensor->pin = pin;
    sensor->interval = -1;                              /* Auto-poll disabled by default */
    sensor->status_code = ERR_SUCCESS;
    sensor->register_time = ktime_get_real_seconds();  /* Record registration timestamp */
    sensor->sensor_type = SENSOR_TYPE_UNKNOWN;          /* Type determined after first measurement */
    sensor->last_attempt_time = 0;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "No measurement taken");
    mutex_init(&sensor->lock);

    /* Step 5: Create procfs entries for this sensor */
    if (dht_create_sensor_proc(sensor)) {
        kfree(sensor);
        return -ENOMEM;
    }

    /* Step 6: Perform an initial measurement to verify the sensor is working.
     * This is done before adding to the list so a failed sensor doesn't appear
     * in the list. The measurement is performed without holding any global lock. */
    dht_do_measurement(sensor, false);

    /* Check the measurement result */
    mutex_lock(&sensor->lock);
    ret = sensor->status_code;
    mutex_unlock(&sensor->lock);

    /* If the initial measurement failed, free resources and return an error */
    if (ret != ERR_SUCCESS) {
        pin_err(pin, "registration failed - %s\n", error_str(ret));
        dht_sensor_free(sensor);
        return -EIO;
    }

    /* Step 7: Add the sensor to the global list */
    mutex_lock(&list_lock);
    list_add(&sensor->list, &sensor_list);
    sensor_count++;

    /* Step 8: Start auto-polling if global auto mode is already active */
    if (READ_ONCE(global_auto_interval) != -1) {
        dht_start_poll(sensor);
        pin_log(pin, "auto-poll enabled by global setting\n");
    }
    mutex_unlock(&list_lock);

    pin_log(pin, "registered successfully\n");
    return count;
}

/**
 * unexport_write - Unregister a sensor by writing its BCM pin number
 * @f:     File structure (unused)
 * @buf:   User-space buffer containing the BCM pin number to unregister
 * @count: Number of bytes in the user buffer
 * @pos:   File offset (unused)
 *
 * This is the handler for writing to /proc/sensors/dht/unexport.
 * It searches the sensor list for the specified pin number, removes
 * the sensor from the list, and frees all its resources (stops the
 * poll thread, removes procfs entries, destroys the mutex, frees memory).
 *
 * The "unregistered" message is always logged (not gated by debug flag)
 * so the user can confirm the sensor was removed even with debug off.
 *
 * Returns: Number of bytes consumed on success, -ENODEV if the pin
 *          is not currently registered, or a negative parse error.
 */
static ssize_t unexport_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int pin;
    struct dht_sensor *sensor, *tmp;
    int ret;

    /* Parse the pin number from user input */
    ret = dht_parse_int(buf, count, &pin);
    if (ret)
        return ret;

    /* Search for the sensor in the list under the list lock */
    mutex_lock(&list_lock);
    list_for_each_entry_safe(sensor, tmp, &sensor_list, list) {
        if (sensor->pin == pin) {
            /* Found the sensor — remove it from the list */
            list_del(&sensor->list);
            sensor_count--;
            mutex_unlock(&list_lock);

            /* Free all resources outside the lock to avoid
             * blocking other list operations during thread stop. */
            dht_sensor_free(sensor);

            /* Always log the unregistration (not gated by debug) */
            pin_log(pin, "unregistered\n");
            return count;
        }
    }
    mutex_unlock(&list_lock);

    /* Sensor was not found in the list */
    dht_dbg("pin %d not registered\n", pin);
    return -ENODEV;
}

/* File operations for the /proc/sensors/dht/export entry (write-only) */
static const DHT_PROC_OPS export_fops = {
    DHT_PROC_WRITE = export_write,
};

/* File operations for the /proc/sensors/dht/unexport entry (write-only) */
static const DHT_PROC_OPS unexport_fops = {
    DHT_PROC_WRITE = unexport_write,
};

/* ── Module init / exit ────────────────────────────────────── */

/* Table of global proc entries created under /proc/sensors/dht/ */
static const struct proc_entry_def global_proc_entries[] = {
    { "debug",         0666, &debug_fops },
    { "version",       0444, &version_fops },
    { "export",        0222, &export_fops },
    { "unexport",      0222, &unexport_fops },
    { "auto_interval", 0666, &auto_interval_fops },
};

/**
 * dht_driver_init - Module initialization function
 *
 * Called when the module is loaded (insmod/modprobe). Creates the procfs
 * hierarchy:
 *   /proc/sensors/
 *     dht/
 *       debug
 *       version
 *       export
 *       unexport
 *       auto_interval
 *
 * The function first attempts to create the /proc/sensors parent directory.
 * If it already exists (e.g., another sensor driver created it), the
 * function reuses it. It then creates the /proc/sensors/dht directory and
 * all global proc entries.
 *
 * Returns: 0 on success, -ENOMEM if any procfs entry cannot be created.
 */
static int __init dht_driver_init(void)
{
    int i;

    /* Print the driver identification line */
    dht_info("%s (v%s)\n",
             DHT_DRIVER_AUTHOR, DHT_DRIVER_VERSION);

    /* Create the parent /proc/sensors directory.
     * If it already exists (created by another driver), retry after cleanup. */
    proc_parent = proc_mkdir(PROC_PARENT, NULL);
    if (!proc_parent) {
        /* The directory may already exist — try removing and recreating it */
        remove_proc_subtree(PROC_PARENT, NULL);
        proc_parent = proc_mkdir(PROC_PARENT, NULL);
        if (!proc_parent) {
            dht_err("failed to create /proc/%s\n", PROC_PARENT);
            return -ENOMEM;
        }
    }

    /* Create the /proc/sensors/dht directory */
    proc_dir = proc_mkdir(PROC_DIR_NAME, proc_parent);
    if (!proc_dir) {
        dht_err("failed to create /proc/%s/%s\n", PROC_PARENT, PROC_DIR_NAME);
        proc_remove(proc_parent);
        return -ENOMEM;
    }

    /* Create all global proc entries from the table */
    for (i = 0; i < ARRAY_SIZE(global_proc_entries); i++) {
        const struct proc_entry_def *e = &global_proc_entries[i];
        if (!proc_create(e->name, e->mode, proc_dir, e->fops)) {
            dht_err("failed to create proc entry '%s'\n", e->name);
            /* Clean up on failure: remove the dht directory and its parent */
            proc_remove(proc_dir);
            proc_remove(proc_parent);
            return -ENOMEM;
        }
    }

    /* Log successful initialization with the procfs path and max sensor count */
    dht_info("driver loaded - /proc/%s/%s/ (max %d sensors)\n",
             PROC_PARENT, PROC_DIR_NAME, MAX_SENSORS);
    return 0;
}

/**
 * dht_driver_exit - Module cleanup function
 *
 * Called when the module is unloaded (rmmod). Performs a clean teardown:
 *   1. Moves all sensors from the global list to a temporary list (under lock)
 *   2. Frees each sensor (stops poll threads, removes procfs entries, frees memory)
 *   3. Removes the driver's procfs directories
 *
 * The temporary list is used so the list_lock mutex is only held during
 * the list splice operation, not during the potentially slow sensor cleanup
 * (which includes kthread_stop that may sleep).
 */
static void __exit dht_driver_exit(void)
{
    LIST_HEAD(tmp_list);
    struct dht_sensor *sensor, *tmp;

    /* Splice the entire sensor list into a temporary list under the lock.
     * This empties the global list atomically, so no new operations can
     * find sensors while we clean them up. */
    mutex_lock(&list_lock);
    list_splice_init(&sensor_list, &tmp_list);
    sensor_count = 0;
    mutex_unlock(&list_lock);

    /* Free each sensor in the temporary list (outside the lock,
     * since dht_stop_poll / kthread_stop may sleep) */
    list_for_each_entry_safe(sensor, tmp, &tmp_list, list) {
        list_del(&sensor->list);
        dht_sensor_free(sensor);
    }

    /* Remove the driver's procfs directories */
    proc_remove(proc_dir);
    proc_remove(proc_parent);

    dht_info("driver unloaded\n");
}

module_init(dht_driver_init);
module_exit(dht_driver_exit);
