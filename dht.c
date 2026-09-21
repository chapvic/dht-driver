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
 * Version: 2.8.3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Features:
 *   - Dynamic sensor export/unexport via procfs
 *   - Per-sensor proc entries for temperature, humidity, status and configuration
 *   - Background polling thread with configurable interval
 *   - Global auto-poll mode with shared interval
 *   - GPIO chip base caching for fast multi-sensor registration on Pi 3/4/5
 *   - Nanosecond-precision pulse timing for reliable reads across all Pi models
 *   - Rate limiting for all measurements (manual and auto-poll)
 *   - Safe module unload with module reference counting
 *   - Shared /proc/sensors: coexists with other sensor drivers
 *   - Configuration file: optional /etc/default/dht for auto-registration at load
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
 *
 *
 *  Configuration file (optional): /etc/default/dht
 *
 *  Read at module load time. If the file is missing, the driver loads
 *  with defaults. Lines starting with '#' and empty lines are ignored.
 *  Unknown options and invalid values produce warnings in dmesg.
 *
 *  Global options:
 *    DEBUG                - enable debug logging at load (same as DEBUG=1)
 *    DEBUG=0|1            - explicitly disable/enable debug logging
 *    AUTO_INTERVAL=n      - global auto-poll interval in seconds (2-60)
 *                           Starts polling for all registered sensors
 *
 *  Sensor registration:
 *    SENSOR=<pin>         - register a sensor on the given BCM pin
 *    SENSOR=<pin>,<n>     - register with per-sensor auto-poll interval
 *                           (2-60 seconds; -1 disables auto-poll)
 *
 *  Example:
 *    # /etc/default/dht
 *    DEBUG=1
 *    AUTO_INTERVAL=10
 *    SENSOR=4             - DHT22 on GPIO4, uses global interval
 *    SENSOR=17,5          - DHT11 on GPIO17, polls every 5 seconds
 *    SENSOR=22            - Sensor on GPIO22, no auto-poll
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
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/preempt.h>

/* Driver metadata constants used in MODULE_* macros and dmesg output */
#define DHT_DRIVER_AUTHOR        "DHT Driver © 2026, Chapvic"
#define DHT_DRIVER_DESCRIPTION   "DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver"
#define DHT_DRIVER_VERSION       "2.8.3"
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
#define CONFIG_PATH      "/etc/default/dht"  /* Path to the optional configuration file read at module load */
#define CONFIG_BUF_LEN   4096               /* Maximum size of the configuration file buffer */
#define CONFIG_LINE_LEN  256                /* Maximum length of a single config line */

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
    struct mutex lock;                /* Mutex protecting all fields above from concurrent access */
    struct proc_dir_entry *proc_dir;  /* Pointer to the sensor's procfs subdirectory (/proc/sensors/dht/gpio<pin>/) */
    struct task_struct *poll_thread;  /* Kernel thread for background polling (NULL if not running) */
    struct gpio_desc *gpiod;          /* Requested GPIO descriptor (owned by this sensor) */
    atomic_t measuring;               /* Atomic flag: 1 = measurement in progress (prevents parallel bit-bang) */
    struct kref refcount;              /* Reference count: prevents use-after-free during unexport */
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
static struct proc_dir_entry *proc_parent;   /* /proc/sensors directory handle (NULL if not owned by us) */
static struct proc_dir_entry *proc_dir;      /* /proc/sensors/dht directory handle */
static bool we_created_parent = false;       /* True if WE created /proc/sensors (and should remove it if empty) */
static LIST_HEAD(sensor_list);               /* Head of the linked list of all registered sensors */
static DEFINE_MUTEX(list_lock);              /* Mutex protecting sensor_list and sensor_count */
static int sensor_count = 0;                 /* Current number of registered sensors */
static int global_auto_interval = -1;        /* Global auto-poll interval in seconds (-1 = disabled). When active, overrides per-sensor intervals */
static int cached_chip_base = -1;            /* Cached base GPIO number of the detected Pi GPIO chip. -1 = not yet detected */
static atomic_t dht_exiting = ATOMIC_INIT(0); /* Flag set during module unload to reject new procfs operations */

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
 * @pin:  BCM GPIO pin number (used for logging only)
 * @desc: GPIO descriptor obtained during sensor registration
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
static int dht_read_sensor(int pin, struct gpio_desc *desc, int *hum, int *temp, int *type)
{
    int data[5] = {0, 0, 0, 0, 0};   /* 5 bytes: RH_int, RH_dec, T_int, T_dec, checksum */
    int last_state = 1;              /* Current GPIO line state (1 = high, idle) */
    int i, j = 0;                    /* i: transition counter, j: bit counter (0-39) */
    u64 pulse_start, pulse_ns;       /* Nanosecond timestamps for pulse width measurement */
    int attempt;                     /* Current retry attempt number */

    /* Use the GPIO descriptor obtained during registration (sensor->gpiod).
     * This avoids re-resolving the pin on every measurement, which could
     * return a different descriptor if the GPIO subsystem changed. */
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return ERR_GPIO_REQUEST;
    }

    /* Warn if this GPIO is behind a sleeping expander (e.g., I2C GPIO
     * chip). The bit-bang timing loop uses non-sleeping gpiod_get_value
     * in a tight loop and will not work correctly with sleeping GPIOs. */
    if (gpiod_cansleep(desc)) {
        pin_err(pin, "GPIO is on a sleeping chip — timing-critical reads will not work\n");
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
         *   ~26 us = 0, ~70 us = 1.
         *
         * Disable preemption for the duration of the bit-bang read (~4 ms
         * worst case) to prevent timing corruption from context switches.
         * Interrupts are NOT disabled to avoid affecting system latency;
         * the DHT protocol's ~70 us pulses are wide enough that occasional
         * IRQ jitter is tolerable. */
        preempt_disable();
        for (i = 0; i < MAX_TIMINGS && j < 40; i++) {
            /* Initialize pulse_ns to 0 so that if the while loop body
             * never executes (line already changed state), we don't
             * use a stale/garbage value for bit decoding or timeout check. */
            pulse_ns = 0;

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
        preempt_enable();

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
             * which means it's a DHT11 with integer-only format.
             *
             * The humidity check is authoritative: once the type is determined
             * from humidity, the same type is used for temperature interpretation.
             * This avoids a bug where DHT11 temperatures below 5 C were
             * misinterpreted as DHT22 values (c <= 1250, no conversion applied). */
            if (h > 1000) {
                if (type) *type = SENSOR_TYPE_DHT11;
                /* DHT11: use only the integer bytes, scale by 10 for consistent units */
                h = data[0] * 10;
                c = data[2] * 10;
            } else if (data[1] == 0 && data[3] == 0 &&
                       data[0] <= 100 && data[2] <= 50) {
                /* Edge case: DHT11 with low humidity (<= 3%) produces
                 * h <= 1000, which would be misidentified as DHT22.
                 * Additional checks: fractional bytes are 0 (DHT11 never
                 * sends fractional data) and values are within DHT11
                 * ranges (humidity 0-100%, temperature 0-50 C). */
                if (type) *type = SENSOR_TYPE_DHT11;
                h = data[0] * 10;
                c = data[2] * 10;
            } else {
                if (type) *type = SENSOR_TYPE_DHT22;
                /* DHT22: the 16-bit values are already scaled x10 */
            }

            /* Handle negative temperature (bit 7 of data[2] is the sign bit).
             * For DHT22: c = (data[2] << 8) | data[3], so bit 15 is the sign.
             * Mask the sign bit before negating to get the correct magnitude:
             *   c = 0x80FB → -(0x00FB) = -251 → T=-25.1 C (correct)
             * Without the mask: -(0x80FB) = -32763 → wrong magnitude.
             * For DHT11: data[2] is 0-50, bit 7 is never set, no effect. */
            if (data[2] & 0x80)
                c = -(c & 0x7FFF);

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
 * @manual: true if this is a user-triggered manual measurement,
 *          false if this is an automatic background poll
 *
 * This function is the central measurement dispatcher, called from:
 *   - export_write() during sensor registration (manual = false)
 *   - sensor_measure_write() for user-triggered measurements (manual = true)
 *   - dht_poll_thread_fn() for background polling (manual = false)
 *
 * Rate limiting (MEAS_MIN_GAP) applies to ALL measurements. When the gap
 * is too short:
 *   - Manual measurements return ERR_TOO_SOON to the user.
 *   - Auto-poll measurements are silently skipped (last data preserved).
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

    /* Rate limiting: enforce minimum gap between ALL measurements (manual and auto).
     * DHT sensors need at least 2 seconds between reads to recover. */
    now = ktime_get_real_seconds();
    if (sensor->last_attempt_time > 0 &&
        (now - sensor->last_attempt_time) < MEAS_MIN_GAP) {
        if (manual) {
            /* Manual measurement: return error to the user */
            sensor->status_code = ERR_TOO_SOON;
            snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ERR_TOO_SOON));
            pin_dbg(sensor->pin, "manual measurement rejected - only %llds since last\n",
                    (long long)(now - sensor->last_attempt_time));
            mutex_unlock(&sensor->lock);
            return;
        } else {
            /* Auto-poll: silently skip, keep last successful data */
            pin_dbg(sensor->pin, "auto-poll skipped - only %llds since last measurement\n",
                    (long long)(now - sensor->last_attempt_time));
            mutex_unlock(&sensor->lock);
            return;
        }
    }

    /* Record the attempt timestamp for rate limiting */
    sensor->last_attempt_time = ktime_get_real_seconds();

    /* Atomically claim the measuring flag to prevent parallel bit-bang
     * on the same GPIO line (e.g., manual measure vs auto-poll). */
    if (!atomic_cmpxchg(&sensor->measuring, 0, 1)) {
        /* Another measurement is already in progress */
        if (manual) {
            sensor->status_code = ERR_READ_FAILED;
            snprintf(sensor->status_text, STATUS_BUF_LEN,
                     "Measurement already in progress");
            pin_dbg(sensor->pin, "manual measurement skipped - already in progress\n");
        } else {
            pin_dbg(sensor->pin, "auto-poll skipped - measurement in progress\n");
        }
        mutex_unlock(&sensor->lock);
        return;
    }

    /* Release the lock during the actual sensor read to avoid blocking.
     * The read takes 20+ ms, which is too long to hold a mutex. */
    mutex_unlock(&sensor->lock);

    /* Perform the actual GPIO read (may take multiple retries).
     * Pass the descriptor stored during registration to avoid
     * re-resolving the pin on every measurement. */
    ret = dht_read_sensor(sensor->pin, sensor->gpiod, &hum, &temp, &type);

    /* Release the measuring flag */
    atomic_set(&sensor->measuring, 0);

    /* Re-acquire the lock to store the results */
    mutex_lock(&sensor->lock);
    sensor->status_code = ret;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ret));

    if (ret == ERR_SUCCESS) {
        /* Update measurement results on success.
         * Sensor type is determined once and then locked — it does not
         * change between measurements. This prevents spurious type flips
         * caused by borderline humidity readings (e.g., DHT22 reporting
         * 100.1% could briefly trigger the DHT11 heuristic). */
        sensor->humidity_raw = hum;
        sensor->temperature_raw = temp;
        if (sensor->sensor_type == SENSOR_TYPE_UNKNOWN)
            sensor->sensor_type = type;
        sensor->last_meas_time = sensor->last_attempt_time;
        if (temp < 0)
            pin_dbg(sensor->pin, "measurement OK - H=%d.%d%% T=-%d.%d C\n",
                    hum / 10, hum % 10, (-temp) / 10, (-temp) % 10);
        else
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

        /* If both global and per-sensor intervals are disabled (-1),
         * the thread should stop — there is nothing to poll for.
         * This can happen when auto_interval is set to -1 while the
         * thread was started by a previous non-zero global setting. */
        if (effective_interval == -1) {
            pin_dbg(sensor->pin, "both intervals disabled, poll thread idle\n");
            /* Do NOT break out of the main loop here! If the thread exits
             * on its own, task_struct is freed by the kernel but
             * sensor->poll_thread still holds a dangling pointer.
             * A later dht_stop_poll() -> kthread_stop() would be a
             * use-after-free. Instead, sleep in a loop until
             * kthread_should_stop() or until an interval is re-enabled. */
            while (!kthread_should_stop()) {
                mutex_lock(&sensor->lock);
                if (READ_ONCE(global_auto_interval) != -1 ||
                    sensor->interval != -1) {
                    mutex_unlock(&sensor->lock);
                    break;  /* exit idle loop, resume polling */
                }
                mutex_unlock(&sensor->lock);
                ssleep(1);
            }
            continue;  /* re-enter main loop, re-check kthread_should_stop */
        }

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

/* ── Reference counting and unified sensor cleanup ─────────── */

/**
 * dht_sensor_get - Acquire a reference to a sensor
 * @sensor: Pointer to the sensor instance
 *
 * Increments the kref refcount. Returns the sensor pointer on success,
 * or NULL if the refcount was already zero (sensor is being freed).
 *
 * Must be called under list_lock or when a reference is already held.
 *
 * Returns: sensor pointer on success, NULL on failure.
 */
static struct dht_sensor *dht_sensor_get(struct dht_sensor *sensor)
{
    if (!sensor)
        return NULL;
    if (!kref_get_unless_zero(&sensor->refcount))
        return NULL;
    return sensor;
}

/**
 * dht_sensor_release - kref release callback — frees all sensor resources
 * @ref: Pointer to the kref embedded in struct dht_sensor
 *
 * Called when the last reference to the sensor is dropped. This performs
 * the complete teardown: stop poll thread, remove procfs entries,
 * destroy mutex, free memory.
 */
static void dht_sensor_release(struct kref *ref)
{
    struct dht_sensor *sensor = container_of(ref, struct dht_sensor, refcount);

    dht_stop_poll(sensor);              /* Stop the background polling thread */
    if (sensor->gpiod)
        gpio_free(desc_to_gpio(sensor->gpiod));  /* Release the GPIO line */
    proc_remove(sensor->proc_dir);      /* Remove all procfs entries for this sensor */
    mutex_destroy(&sensor->lock);       /* Clean up the mutex */
    kfree(sensor);                      /* Free the sensor struct */
}

/**
 * dht_sensor_put - Drop a reference to a sensor
 * @sensor: Pointer to the sensor instance
 *
 * Decrements the kref refcount. If this was the last reference,
 * dht_sensor_release() is called to free all resources.
 */
static void dht_sensor_put(struct dht_sensor *sensor)
{
    if (sensor)
        kref_put(&sensor->refcount, dht_sensor_release);
}

/**
 * dht_sensor_free - Drop the initial reference, freeing the sensor if last
 * @sensor: Pointer to the sensor instance to free
 *
 * Convenience wrapper around dht_sensor_put(). Used during registration
 * failure and unexport. If no open procfs files hold a reference, the
 * sensor is freed immediately.
 *
 * It is called from:
 *   - dht_do_register() when initial measurement fails after registration
 *   - unexport_write() when the user unregisters a sensor
 *
 * Note: dht_driver_exit() does NOT call this function — it performs its own
 * two-phase cleanup that removes procfs entries before stopping threads,
 * to prevent race conditions during module unload.
 */
static void dht_sensor_free(struct dht_sensor *sensor)
{
    dht_sensor_put(sensor);
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

/* ── Module reference counting for procfs ─────────────────── */

/**
 * dht_proc_open - Generic open handler that increments the module reference count
 * @inode: Inode of the procfs entry being opened
 * @f:     File structure for the opened entry
 *
 * This function is wired to ALL procfs entries (both global and per-sensor).
 * It performs two checks:
 *   1. If the driver is being unloaded (dht_exiting is set), reject with -ENODEV.
 *   2. Increment the module reference count via try_module_get() so that rmmod
 *      cannot remove the module while a procfs file is open.
 *
 * The corresponding dht_proc_release() decrements the reference count when
 * the file is closed.
 *
 * Returns: 0 on success, -ENODEV if the driver is being unloaded.
 */
static int dht_proc_open(struct inode *inode, struct file *f)
{
    struct dht_sensor *sensor;

    /* Reject new opens if the module is being unloaded */
    if (atomic_read(&dht_exiting))
        return -ENODEV;

    /* Increment module reference count to prevent rmmod while file is open */
    if (!try_module_get(THIS_MODULE))
        return -ENODEV;

    /* For per-sensor entries, acquire a reference to the sensor struct
     * to prevent use-after-free if the sensor is unregistered while
     * the procfs file is still open. Global entries have no PDE_DATA. */
    sensor = DHT_PDE_DATA(inode);
    if (sensor) {
        if (!dht_sensor_get(sensor)) {
            module_put(THIS_MODULE);
            return -ENODEV;
        }
    }

    return 0;
}

/**
 * dht_proc_release - Generic release handler that decrements the module reference count
 * @inode: Inode of the procfs entry being closed
 * @f:     File structure for the closed entry
 *
 * This function is the counterpart of dht_proc_open(). It decrements the
 * module reference count so that rmmod can proceed once all procfs files
 * are closed.
 *
 * Returns: 0 always.
 */
static int dht_proc_release(struct inode *inode, struct file *f)
{
    struct dht_sensor *sensor;

    /* Drop the sensor reference acquired in dht_proc_open (if any) */
    sensor = DHT_PDE_DATA(inode);
    if (sensor)
        dht_sensor_put(sensor);

    module_put(THIS_MODULE);
    return 0;
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
    DHT_PROC_OPEN    = dht_proc_open,
    DHT_PROC_READ    = debug_read,
    DHT_PROC_WRITE   = debug_write,
    DHT_PROC_RELEASE = dht_proc_release,
};

/* ── Global procfs: version ────────────────────────────────── */

/**
 * version_read - Read the driver version string
 * @f:     File structure (unused)
 * @buf:   User-space buffer to write the version string to
 * @count: Maximum number of bytes to write
 * @pos:   File offset — used to return 0 on subsequent reads (EOF)
 *
 * Outputs the driver version (e.g., "2.7\n") to the user buffer.
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
    DHT_PROC_OPEN    = dht_proc_open,
    DHT_PROC_READ    = version_read,
    DHT_PROC_RELEASE = dht_proc_release,
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

    /* Set the global interval; -1 disables; out-of-range is rejected */
    if (val == -1) {
        WRITE_ONCE(global_auto_interval, -1);
    } else if (val >= MIN_INTERVAL && val <= MAX_INTERVAL) {
        WRITE_ONCE(global_auto_interval, val);
    } else {
        mutex_unlock(&list_lock);
        dht_err("auto_interval: invalid value %d (must be 2-60 or -1)\n", val);
        return -EINVAL;
    }

    /* If global auto mode is now active, start poll threads for all sensors
     * that don't have one running yet.
     * If global auto mode was just disabled, existing poll threads will
     * detect the change on their next loop iteration and self-terminate
     * if the per-sensor interval is also -1. */
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
    DHT_PROC_OPEN    = dht_proc_open,
    DHT_PROC_READ    = auto_interval_read,
    DHT_PROC_WRITE   = auto_interval_write,
    DHT_PROC_RELEASE = dht_proc_release,
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

    /* Global auto mode is off — the per-sensor setting controls polling.
     * Hold list_lock to prevent races with concurrent dht_start_poll/
     * dht_stop_poll from other paths (auto_interval_write, dht_do_register). */
    mutex_lock(&list_lock);
    if (val == -1) {
        /* Disable polling: stop the thread if running */
        dht_stop_poll(sensor);
        pin_log(sensor->pin, "auto-poll disabled\n");
    } else {
        /* Enable polling: start the thread with the new interval */
        dht_start_poll(sensor);
        pin_log(sensor->pin, "auto-poll enabled (interval=%d)\n", val);
    }
    mutex_unlock(&list_lock);
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
static const DHT_PROC_OPS sensor_pin_fops         = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_READ = sensor_pin_read, DHT_PROC_RELEASE = dht_proc_release };
static const DHT_PROC_OPS sensor_interval_fops    = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_READ = sensor_interval_read, DHT_PROC_WRITE = sensor_interval_write, DHT_PROC_RELEASE = dht_proc_release };
static const DHT_PROC_OPS sensor_measure_fops     = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_WRITE = sensor_measure_write, DHT_PROC_RELEASE = dht_proc_release };
static const DHT_PROC_OPS sensor_status_code_fops = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_READ = sensor_status_code_read, DHT_PROC_RELEASE = dht_proc_release };
static const DHT_PROC_OPS sensor_status_text_fops = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_READ = sensor_status_text_read, DHT_PROC_RELEASE = dht_proc_release };
static const DHT_PROC_OPS sensor_value_fops       = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_READ = sensor_value_read, DHT_PROC_RELEASE = dht_proc_release };
static const DHT_PROC_OPS sensor_info_fops        = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_READ = sensor_info_read, DHT_PROC_RELEASE = dht_proc_release };
static const DHT_PROC_OPS sensor_timestamp_fops   = { DHT_PROC_OPEN = dht_proc_open, DHT_PROC_READ = sensor_last_meas_time_read, DHT_PROC_RELEASE = dht_proc_release };

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
            sensor->proc_dir = NULL;
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
static int dht_do_register(int pin, int interval)
{
    struct dht_sensor *sensor, *s;
    struct gpio_desc *desc;
    struct gpio_chip *chip;
    int ret;

    /* Validate pin range */
    if (pin < 0 || pin > MAX_PIN_NUM)
        return -EINVAL;

    /* Check for duplicate registration and sensor count limit */
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

    /* Resolve the GPIO descriptor for this BCM pin */
    desc = dht_find_desc(pin);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return -ENODEV;
    }

    /* Request (claim) the GPIO line so no other driver can use it.
     * Without this, gpiod_direction_output/input may fail with
     * -EACCES on kernels with enforced request-before-use. */
    ret = gpio_request(desc_to_gpio(desc), "dht");
    if (ret) {
        pin_err(pin, "GPIO request failed (already in use?): %d\n", ret);
        return -EBUSY;
    }

    /* Log which GPIO chip the pin was found on (for diagnostics) */
    chip = gpiod_to_chip(desc);
    if (chip && chip->label)
        pin_log(pin, "found on '%s' (base=%d, global=%d)\n",
                chip->label, chip->base, chip->base + pin);
    else
        pin_log(pin, "found (global=%d)\n", pin);

    /* Allocate and initialize the sensor struct */
    sensor = kzalloc(sizeof(*sensor), GFP_KERNEL);
    if (!sensor) {
        gpio_free(desc_to_gpio(desc));
        return -ENOMEM;
    }

    sensor->gpiod = desc;               /* Store for gpio_free in release */
    atomic_set(&sensor->measuring, 0);
    sensor->pin = pin;
    sensor->interval = interval;                         /* Use provided interval (-1 = disabled) */
    sensor->status_code = ERR_SUCCESS;
    sensor->register_time = ktime_get_real_seconds();
    sensor->sensor_type = SENSOR_TYPE_UNKNOWN;
    sensor->last_attempt_time = 0;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "No measurement taken");
    mutex_init(&sensor->lock);
    kref_init(&sensor->refcount);   /* Start with refcount = 1 (owned by list) */

    /* Create procfs entries for this sensor */
    if (dht_create_sensor_proc(sensor)) {
        /* kref was already initialized above; use put to release it
         * properly rather than kfree (no procfs entries exist yet,
         * so this will immediately call dht_sensor_release). */
        dht_sensor_put(sensor);
        return -ENOMEM;
    }

    /* Perform an initial measurement to verify the sensor is working */
    dht_do_measurement(sensor, false);

    mutex_lock(&sensor->lock);
    ret = sensor->status_code;
    mutex_unlock(&sensor->lock);

    if (ret != ERR_SUCCESS) {
        pin_err(pin, "registration failed - %s\n", error_str(ret));
        dht_sensor_free(sensor);
        return -EIO;
    }

    /* Add the sensor to the global list — re-check for duplicates under
     * the lock to close the TOCTOU window between the initial check
     * and the list_add. */
    mutex_lock(&list_lock);
    list_for_each_entry(s, &sensor_list, list) {
        if (s->pin == pin) {
            mutex_unlock(&list_lock);
            dht_err("pin %d raced with concurrent registration\n", pin);
            dht_sensor_free(sensor);
            return -EBUSY;
        }
    }
    list_add(&sensor->list, &sensor_list);
    sensor_count++;

    /* Start auto-polling: global mode takes priority, then per-sensor interval */
    if (READ_ONCE(global_auto_interval) != -1) {
        dht_start_poll(sensor);
        pin_log(pin, "auto-poll enabled by global setting\n");
    } else if (interval != -1) {
        dht_start_poll(sensor);
        pin_log(pin, "auto-poll enabled (interval=%d)\n", interval);
    }
    mutex_unlock(&list_lock);

    pin_log(pin, "registered successfully\n");
    return 0;
}

static ssize_t export_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int pin;
    int ret;

    ret = dht_parse_int(buf, count, &pin);
    if (ret)
        return ret;

    ret = dht_do_register(pin, -1);
    if (ret)
        return ret;

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
    DHT_PROC_OPEN    = dht_proc_open,
    DHT_PROC_WRITE   = export_write,
    DHT_PROC_RELEASE = dht_proc_release,
};

/* File operations for the /proc/sensors/dht/unexport entry (write-only) */
static const DHT_PROC_OPS unexport_fops = {
    DHT_PROC_OPEN    = dht_proc_open,
    DHT_PROC_WRITE   = unexport_write,
    DHT_PROC_RELEASE = dht_proc_release,
};

/* ── Proc directory emptiness check ─────────────────────── */

/*
 * struct dht_dir_ctx - Context for directory iteration callback
 * @ctx:   Standard dir_context used by iterate_dir
 * @count: Counter for real entries found (excluding . and ..)
 *
 * Used by dht_proc_dir_is_empty() to check whether a /proc directory
 * contains any subdirectories other than the standard . and .. entries.
 */
struct dht_dir_ctx {
    struct dir_context ctx;
    int count;
};

/**
 * dht_dir_filldir - Callback for iterate_dir that counts real directory entries
 * @ctx:     Pointer to the dir_context (embedded in dht_dir_ctx)
 * @name:    Name of the current directory entry
 * @namlen:  Length of the name string
 * @pos:     Position offset (unused)
 * @ino:     Inode number (unused)
 * @d_type:  Directory entry type (unused)
 *
 * This callback is invoked by iterate_dir for each entry in a directory.
 * It skips the standard "." and ".." entries and counts all other entries.
 * Once a real entry is found, it returns false to stop iteration early
 * (we only need to know if the directory is non-empty, not list everything).
 *
 * Returns: true to continue iteration (entry was . or ..), false to stop
 *          (found a real entry — directory is not empty).
 */
static bool dht_dir_filldir(struct dir_context *ctx, const char *name,
                            int namlen, loff_t pos, u64 ino,
                            unsigned int d_type)
{
    struct dht_dir_ctx *dctx;

    dctx = container_of(ctx, struct dht_dir_ctx, ctx);

    /* Skip "." and ".." — they are not real subdirectories */
    if (name[0] == '.' && (namlen == 1 || (namlen == 2 && name[1] == '.')))
        return true;

    /* Found a real entry — increment count and stop iteration */
    dctx->count++;
    return false;
}

/**
 * dht_proc_dir_is_empty - Check whether a /proc directory is empty
 * @path: Full path of the directory to check (e.g., "/proc/sensors")
 *
 * Opens the directory via VFS and iterates over its entries using
 * iterate_dir. Returns true if the directory contains no entries
 * other than "." and "..".
 *
 * This function is needed because struct proc_dir_entry is opaque
 * in newer kernels (5.x+), so we cannot directly inspect the ->subdir
 * linked list. The VFS approach works universally across kernel versions.
 *
 * Returns: true if the directory is empty, false if it contains entries
 *          or if the directory could not be opened (conservative: treat
 *          "can't check" as "not empty" to avoid accidental removal).
 */
static bool dht_proc_dir_is_empty(const char *path)
{
    struct file *filp;
    struct dht_dir_ctx dctx = {
        .ctx.actor = dht_dir_filldir,
        .count = 0,
    };
    bool empty;

    filp = filp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (IS_ERR(filp)) {
        /* Could not open — conservatively assume not empty */
        dht_err("could not open %s for emptiness check\n", path);
        return false;
    }

    /* Iterate over directory entries */
    dctx.ctx.pos = 0;
    iterate_dir(filp, &dctx.ctx);

    empty = (dctx.count == 0);
    filp_close(filp, NULL);

    return empty;
}

/* ── Configuration file parser ────────────────────────────── */

/**
 * dht_config_set_auto_interval - Set global auto-poll interval from config
 * @val: Interval in seconds (2-60), or -1 to disable
 *
 * Called during config file parsing to set the global auto-poll interval.
 * Mirrors the logic of auto_interval_write() but works with kernel-space
 * values instead of user-space buffers.
 */
static void dht_config_set_auto_interval(int val)
{
    if (val != -1 && (val < MIN_INTERVAL || val > MAX_INTERVAL)) {
        dht_err("config: AUTO_INTERVAL=%d out of range (2-60 or -1)\n", val);
        return;
    }

    mutex_lock(&list_lock);
    WRITE_ONCE(global_auto_interval, val);

    if (val != -1) {
        struct dht_sensor *sensor;
        list_for_each_entry(sensor, &sensor_list, list) {
            if (!sensor->poll_thread)
                dht_start_poll(sensor);
        }
    }
    mutex_unlock(&list_lock);

    dht_info("config: AUTO_INTERVAL=%d\n", val);
}

/**
 * dht_parse_config_line - Parse a single line from the config file
 * @line: Null-terminated, trimmed config line (no leading/trailing whitespace)
 *
 * Supported options:
 *   DEBUG               — enable debug logging (equivalent to DEBUG=1)
 *   DEBUG=0|1           — explicitly set debug flag
 *   AUTO_INTERVAL=N    — set global auto-poll interval (2-60, or -1 to disable)
 *   SENSOR=pin          — register a sensor on the given BCM pin (no auto-poll)
 *   SENSOR=pin,N        — register a sensor with per-sensor auto-poll interval
 *
 * Invalid option names and invalid values produce a warning in dmesg.
 */
static void dht_parse_config_line(const char *line)
{
    char *eq;
    char buf[CONFIG_LINE_LEN];
    char *key, *val;

    /* Make a mutable copy for strsep */
    strscpy(buf, line, sizeof(buf));

    /* Split into key=value */
    eq = strchr(buf, '=');
    if (eq) {
        *eq = '\0';
        key = strim(buf);
        val = strim(eq + 1);
    } else {
        key = strim(buf);
        val = NULL;
    }

    if (!key || !*key)
        return;

    /* DEBUG — enable debug logging */
    if (strcmp(key, "DEBUG") == 0) {
        int dbg = 1;
        if (val && *val) {
            if (kstrtoint(val, 10, &dbg) || (dbg != 0 && dbg != 1)) {
                dht_err("config: DEBUG='%s' invalid (expected 0 or 1)\n", val);
                return;
            }
        }
        WRITE_ONCE(dht_debug, dbg);
        dht_info("config: DEBUG=%d\n", dbg);
        return;
    }

    /* AUTO_INTERVAL — global auto-poll interval */
    if (strcmp(key, "AUTO_INTERVAL") == 0) {
        int ival;
        if (!val || !*val) {
            dht_err("config: AUTO_INTERVAL requires a value (e.g. AUTO_INTERVAL=5)\n");
            return;
        }
        if (kstrtoint(val, 10, &ival)) {
            dht_err("config: AUTO_INTERVAL='%s' is not a valid integer\n", val);
            return;
        }
        dht_config_set_auto_interval(ival);
        return;
    }

    /* SENSOR — register a sensor */
    if (strcmp(key, "SENSOR") == 0) {
        int pin = -1, interval = -1;
        char *comma;

        if (!val || !*val) {
            dht_err("config: SENSOR requires a pin number (e.g. SENSOR=4)\n");
            return;
        }

        /* Check for pin[,interval] format */
        comma = strchr(val, ',');
        if (comma) {
            *comma = '\0';
            val = strim(val);
            char *interval_str = strim(comma + 1);
            if (kstrtoint(val, 10, &pin) || pin < 0 || pin > MAX_PIN_NUM) {
                dht_err("config: SENSOR pin '%s' invalid (0-%d)\n", val, MAX_PIN_NUM);
                return;
            }
            if (kstrtoint(interval_str, 10, &interval) ||
                (interval != -1 && (interval < MIN_INTERVAL || interval > MAX_INTERVAL))) {
                dht_err("config: SENSOR interval '%s' invalid (2-%d or -1)\n", interval_str, MAX_INTERVAL);
                return;
            }
        } else {
            if (kstrtoint(val, 10, &pin) || pin < 0 || pin > MAX_PIN_NUM) {
                dht_err("config: SENSOR pin '%s' invalid (0-%d)\n", val, MAX_PIN_NUM);
                return;
            }
        }

        dht_info("config: SENSOR pin=%d interval=%d\n", pin, interval);
        dht_do_register(pin, interval);
        return;
    }

    /* Unknown option */
    dht_err("config: unknown option '%s'\n", key);
}

/**
 * dht_load_config - Read and parse the configuration file at module load
 *
 * Opens /etc/default/dht (if it exists) and parses each line for
 * configuration options (DEBUG, AUTO_INTERVAL, SENSOR).
 *
 * The file is optional — if it does not exist or cannot be read,
 * the driver loads with defaults. Lines starting with '#' and empty
 * lines are ignored.
 *
 * This function is called at the end of dht_driver_init(), after the
 * procfs hierarchy is set up, so that sensors registered from the config
 * file get their proc entries created correctly.
 */
static void dht_load_config(void)
{
    struct file *filp;
    char *buf;
    loff_t pos = 0;
    ssize_t bytes;
    char *line, *next;

    filp = filp_open(CONFIG_PATH, O_RDONLY, 0);
    if (IS_ERR(filp)) {
        dht_dbg("no config file at %s (using defaults)\n", CONFIG_PATH);
        return;
    }

    buf = kmalloc(CONFIG_BUF_LEN, GFP_KERNEL);
    if (!buf) {
        dht_err("could not allocate buffer for config file\n");
        filp_close(filp, NULL);
        return;
    }

    bytes = kernel_read(filp, buf, CONFIG_BUF_LEN - 1, &pos);
    filp_close(filp, NULL);

    if (bytes <= 0) {
        dht_dbg("config file is empty\n");
        kfree(buf);
        return;
    }
    buf[bytes] = '\0';

    dht_info("reading config from %s (%zd bytes)\n", CONFIG_PATH, bytes);

    /* Parse line by line */
    line = buf;
    while (line && *line) {
        /* Find end of current line */
        next = strchr(line, '\n');
        if (next)
            *next = '\0';

        /* Trim and skip empty lines and comments */
        {
            char *trimmed = strim(line);
            if (*trimmed && *trimmed != '#')
                dht_parse_config_line(trimmed);
        }

        line = next ? next + 1 : NULL;
    }

    kfree(buf);
}

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

    /*
     * Create the parent /proc/sensors directory.
     *
     * Two cases:
     *   1. /proc/sensors does not exist yet:
     *      proc_mkdir("sensors", NULL) succeeds → we own it.
     *      proc_parent is set, we_created_parent = true.
     *      Create "dht" as a child: proc_mkdir("dht", proc_parent).
     *
     *   2. /proc/sensors already exists (created by another driver):
     *      proc_mkdir("sensors", NULL) returns NULL.
     *      We create "dht" via full path: proc_mkdir("sensors/dht", NULL).
     *      The kernel's xlate_proc_name() resolves the existing parent.
     *      proc_parent stays NULL — we do NOT own /proc/sensors.
     *      we_created_parent = false — we will NOT remove it on exit.
     */
    proc_parent = proc_mkdir(PROC_PARENT, NULL);
    if (proc_parent) {
        /* We created /proc/sensors — we own it */
        we_created_parent = true;
        proc_dir = proc_mkdir(PROC_DIR_NAME, proc_parent);
    } else {
        /* /proc/sensors already exists — use full path to create our subdir */
        we_created_parent = false;
        proc_dir = proc_mkdir(PROC_PARENT "/" PROC_DIR_NAME, NULL);
    }

    if (!proc_dir) {
        dht_err("failed to create /proc/%s/%s\n", PROC_PARENT, PROC_DIR_NAME);
        if (we_created_parent)
            proc_remove(proc_parent);
        return -ENOMEM;
    }

    /* Create all global proc entries from the table */
    for (i = 0; i < ARRAY_SIZE(global_proc_entries); i++) {
        const struct proc_entry_def *e = &global_proc_entries[i];
        if (!proc_create(e->name, e->mode, proc_dir, e->fops)) {
            dht_err("failed to create proc entry '%s'\n", e->name);
            /* Clean up on failure */
            proc_remove(proc_dir);
            if (we_created_parent)
                proc_remove(proc_parent);
            return -ENOMEM;
        }
    }

    /* Read optional configuration file (/etc/default/dht).
     * This may register sensors, set debug mode, and configure auto-poll
     * intervals. Called after procfs is set up so registered sensors get
     * their proc entries created correctly. */
    dht_load_config();

    /* Log successful initialization with the procfs path and max sensor count */
    dht_info("driver loaded - /proc/%s/%s/ (max %d sensors)\n",
             PROC_PARENT, PROC_DIR_NAME, MAX_SENSORS);
    return 0;
}

/**
 * dht_driver_exit - Module cleanup function
 *
 * Called when the module is unloaded (rmmod). Performs a safe two-phase
 * teardown to prevent race conditions with concurrent procfs operations:
 *
 * Phase 1 — Block new access and remove procfs:
 *   1. Set dht_exiting flag (new dht_proc_open calls return -ENODEV)
 *   2. Disable global auto-poll (prevents new thread launches)
 *   3. Remove ALL procfs entries (proc_remove blocks until open files close)
 *
 * Phase 2 — Stop threads and free memory:
 *   4. Splice sensor list under lock (no new sensors can appear — procfs gone)
 *   5. For each sensor: stop poll thread, destroy mutex, free memory
 *
 * The key insight: after Phase 1, no new file operations can start because
 * all procfs entries are removed. The module reference count (incremented
 * by dht_proc_open) prevents rmmod from proceeding until all open files
 * are closed. By the time Phase 2 runs, it is safe to free sensor memory.
 */
static void __exit dht_driver_exit(void)
{
    LIST_HEAD(tmp_list);
    struct dht_sensor *sensor, *tmp;

    /* ── Phase 1: Block new access and remove procfs ── */

    /* Set the exiting flag so dht_proc_open rejects any new file opens */
    atomic_set(&dht_exiting, 1);

    /* Disable global auto-poll so poll threads won't start new measurements */
    WRITE_ONCE(global_auto_interval, -1);

    /* Remove our procfs subtree. proc_remove(proc_dir) removes
     * /proc/sensors/dht/ and all subdirectories (gpio<pin>/) and files
     * beneath it. This call blocks until all currently open procfs files
     * are closed (their release handlers call module_put, decrementing
     * the module reference count). */
    proc_remove(proc_dir);

    /* Remove /proc/sensors ONLY if we created it AND it is now empty
     * (no other drivers left their subdirectories there).
     * struct proc_dir_entry is opaque in modern kernels, so we use
     * the VFS iterate_dir approach to check emptiness. */
    if (we_created_parent && proc_parent) {
        if (dht_proc_dir_is_empty("/proc/" PROC_PARENT)) {
            proc_remove(proc_parent);
            dht_dbg("/proc/%s removed (was empty)\n", PROC_PARENT);
        } else {
            dht_info("/proc/%s not removed (other drivers using it)\n",
                     PROC_PARENT);
        }
    }

    /* ── Phase 2: Stop threads and free memory ── */

    /* Splice the sensor list under the lock. After proc_remove, no new
     * file operations can reach the sensors, so this is safe. */
    mutex_lock(&list_lock);
    list_splice_init(&sensor_list, &tmp_list);
    sensor_count = 0;
    mutex_unlock(&list_lock);

    /* Free each sensor: stop poll thread (may sleep up to ~400 ms for
     * kthread_stop), destroy mutex, free memory. No lock needed since
     * the list is private (tmp_list) and no procfs access is possible. */
    list_for_each_entry_safe(sensor, tmp, &tmp_list, list) {
        list_del(&sensor->list);
        /* proc_remove(proc_dir) in Phase 1 already removed all gpio<pin>/
         * subdirectories. Clear proc_dir to prevent dht_sensor_release
         * from calling proc_remove on already-freed memory (use-after-free). */
        sensor->proc_dir = NULL;
        /* Drop the list's reference. If no procfs files are open (should
         * be the case after proc_remove), the sensor is freed immediately. */
        dht_sensor_put(sensor);
    }

    dht_info("driver unloaded\n");
}

module_init(dht_driver_init);
module_exit(dht_driver_exit);
