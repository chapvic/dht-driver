/*
 *  dht.c — Kernel module for DHT11/DHT22 multi-sensor support
 *  Compatible with: Raspberry Pi 3 (bcm2835), Pi 4 (bcm2711), Pi 5 (bcm2712)
 *
 *  /proc/sensors/dht/
 *    debug          (rw) - debug logging: 0 = off (default), 1 = on
 *    version        (r)  - driver version
 *    export         (w)  - write BCM pin number to register a new sensor
 *    unexport       (w)  - write BCM pin number to unregister a sensor
 *    auto_interval  (rw) - global auto-poll interval in seconds (2-60, -1 = off)
 *
 *  /proc/sensors/dht/gpio<pin>/
 *    pin           (r)  - BCM GPIO pin number
 *    interval      (rw) - per-sensor auto-poll interval (2-60, -1 = off)
 *    measure       (w)  - write "1" to trigger manual measurement
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

#define DHT_DRIVER_VERSION  "2.4"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DHT Driver");
MODULE_DESCRIPTION("DHT11/DHT22 multi-sensor kernel module (bcm2712 compatible)");
MODULE_VERSION(DHT_DRIVER_VERSION);

#define MAX_TIMINGS      85
#define PROC_PARENT      "sensors"
#define PROC_DIR_NAME    "dht"
#define STATUS_BUF_LEN   128
#define INFO_BUF_LEN     256
#define MAX_SENSORS      32
#define MAX_PIN_NUM      27
#define MIN_INTERVAL     2
#define MAX_INTERVAL     60
#define MEAS_MIN_GAP     2

#define ERR_SUCCESS       0
#define ERR_PIN_INVALID   1
#define ERR_GPIO_REQUEST  2
#define ERR_READ_FAILED   3
#define ERR_AUTO_MODE     4
#define ERR_TOO_SOON      5

#define SENSOR_TYPE_UNKNOWN  0
#define SENSOR_TYPE_DHT11    1
#define SENSOR_TYPE_DHT22    2

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
  #define DHT_PROC_OPS    struct proc_ops
  #define DHT_PROC_READ   .proc_read
  #define DHT_PROC_WRITE  .proc_write
#else
  #define DHT_PROC_OPS    struct file_operations
  #define DHT_PROC_READ   .read
  #define DHT_PROC_WRITE  .write
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
  #define DHT_PDE_DATA(inode)  pde_data(inode)
#else
  #define DHT_PDE_DATA(inode)  PDE_DATA(inode)
#endif

/* ── Debug flag ──────────────────────────────────────────────────── */
static int dht_debug;

module_param(dht_debug, int, 0644);
MODULE_PARM_DESC(dht_debug, "Debug logging (0 = off, 1 = on)");

/* ── Logging macros (compile-time string concatenation for printk) ─ */
#define dht_err(fmt, ...)  printk(KERN_ERR  "[DHT]: " fmt, ##__VA_ARGS__)
#define dht_info(fmt, ...) printk(KERN_INFO "[DHT]: " fmt, ##__VA_ARGS__)
#define dht_dbg(fmt, ...)  do { if (READ_ONCE(dht_debug)) printk(KERN_INFO "[DHT]: " fmt, ##__VA_ARGS__); } while (0)

#define pin_err(p, fmt, ...)  printk(KERN_ERR  "[dht_gpio_%d]: " fmt, p, ##__VA_ARGS__)
#define pin_log(p, fmt, ...)  printk(KERN_INFO "[dht_gpio_%d]: " fmt, p, ##__VA_ARGS__)
#define pin_dbg(p, fmt, ...) do { if (READ_ONCE(dht_debug)) printk(KERN_INFO "[dht_gpio_%d]: " fmt, p, ##__VA_ARGS__); } while (0)

/* ── Per-sensor state ────────────────────────────────────────────── */
struct dht_sensor {
    int pin;
    int interval;
    int humidity_raw;
    int temperature_raw;
    int status_code;
    char status_text[STATUS_BUF_LEN];
    int sensor_type;
    time64_t register_time;
    time64_t last_meas_time;
    time64_t last_attempt_time;
    struct mutex lock;
    struct proc_dir_entry *proc_dir;
    struct task_struct *poll_thread;
    struct list_head list;
};

/* ── Global state ────────────────────────────────────────────────── */
static struct proc_dir_entry *proc_parent;
static struct proc_dir_entry *proc_dir;

static LIST_HEAD(sensor_list);
static DEFINE_MUTEX(list_lock);
static int sensor_count;
static int global_auto_interval = -1;

/* ── Chip base cache (write-once, read-many) ────────────────────── */
static int cached_chip_base = -1;

/* ── Error code text ─────────────────────────────────────────────── */
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

/* ── Time helper ─────────────────────────────────────────────────── */
static void dht_format_iso_time(time64_t seconds, char *buf, size_t size)
{
    struct tm tm;
    time64_to_tm(seconds, 0, &tm);
    snprintf(buf, size, "%04ld-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ════════════════════════════════════════════════════════════════ */
/*  GPIO DESCRIPTOR RESOLUTION                                       */
/* ════════════════════════════════════════════════════════════════ */

static bool is_pi_gpio_chip(const char *label)
{
    if (!label)
        return false;
    if (strstr(label, "rp1"))     return true;
    if (strstr(label, "bcm2835")) return true;
    if (strstr(label, "bcm2711")) return true;
    if (strstr(label, "bcm2712")) return true;
    return false;
}

static struct gpio_desc *dht_find_desc(int bcm_pin, int *out_base, int *out_global)
{
    struct gpio_desc *desc;
    struct gpio_chip *chip;
    int base, global, i;

    base = READ_ONCE(cached_chip_base);
    if (base >= 0) {
        global = base + bcm_pin;
        desc = gpio_to_desc(global);
        if (desc) {
            if (out_base)  *out_base = base;
            if (out_global) *out_global = global;
            return desc;
        }
    }

    desc = gpio_to_desc(bcm_pin);
    if (desc) {
        chip = gpiod_to_chip(desc);
        if (chip && is_pi_gpio_chip(chip->label) && chip->base == 0) {
            WRITE_ONCE(cached_chip_base, 0);
            if (out_base)  *out_base = 0;
            if (out_global) *out_global = bcm_pin;
            return desc;
        }
    }

    for (i = 0; i <= 2048; i++) {
        desc = gpio_to_desc(i);
        if (!desc)
            continue;
        chip = gpiod_to_chip(desc);
        if (!chip || !chip->label)
            continue;
        if (!is_pi_gpio_chip(chip->label))
            continue;
        if (chip->base >= 0 && (i - chip->base) == bcm_pin) {
            WRITE_ONCE(cached_chip_base, chip->base);
            if (out_base)  *out_base = chip->base;
            if (out_global) *out_global = i;
            return desc;
        }
    }

    return NULL;
}

/* ════════════════════════════════════════════════════════════════ */
/*  SENSOR READ LOGIC                                                */
/* ════════════════════════════════════════════════════════════════ */

static int dht_read_sensor(int pin, int *hum, int *temp, int *type)
{
    struct gpio_desc *desc;
    int data[5] = {0, 0, 0, 0, 0};
    int last_state = 1;
    int counter = 0;
    int i, j = 0;
    time64_t start_time, now;

    desc = dht_find_desc(pin, NULL, NULL);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return ERR_GPIO_REQUEST;
    }

    if (gpiod_direction_output(desc, 0)) {
        pin_err(pin, "failed to set GPIO output\n");
        return ERR_GPIO_REQUEST;
    }

    gpiod_set_value(desc, 0);
    msleep(18);

    if (gpiod_direction_input(desc)) {
        pin_err(pin, "failed to set GPIO input\n");
        return ERR_GPIO_REQUEST;
    }

    udelay(40);

    start_time = ktime_get_seconds();
    for (i = 0; i < MAX_TIMINGS; i++) {
        counter = 0;
        while (gpiod_get_value(desc) == last_state) {
            counter++;
            udelay(1);
            now = ktime_get_seconds();
            if ((now - start_time) > 2) {
                pin_dbg(pin, "Sensor timeout waiting for pulse\n");
                return ERR_READ_FAILED;
            }
            if (counter == 255) break;
        }

        last_state = gpiod_get_value(desc);
        if (counter == 255) break;

        if (i >= 4 && i % 2 == 0) {
            if (j >= 40) break;
            data[j / 8] <<= 1;
            if (counter > 16) data[j / 8] |= 1;
            j++;
        }
    }

    if (j >= 40 &&
        data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        int h = ((data[0] << 8) + data[1]);
        int c = (((data[2] & 0x7F) << 8) + data[3]);

        if (h > 1000) {
            if (type) *type = SENSOR_TYPE_DHT11;
            h = data[0] * 10;
        } else {
            if (type) *type = SENSOR_TYPE_DHT22;
        }
        if (c > 1250)
            c = data[2] * 10;
        if (data[2] & 0x80)
            c = -c;

        *hum  = h;
        *temp = c;
        return ERR_SUCCESS;
    }

    pin_dbg(pin, "read failed - j=%d, data=[%d,%d,%d,%d,%d]\n",
            j, data[0], data[1], data[2], data[3], data[4]);
    return ERR_READ_FAILED;
}

/* ── dht_do_measurement: manages sensor->lock internally ─────────── */
static int dht_do_measurement(struct dht_sensor *sensor, bool manual)
{
    int hum = 0, temp = 0, type = 0;
    int ret;
    time64_t now;

    if (sensor->pin < 0 || sensor->pin > MAX_PIN_NUM) {
        mutex_lock(&sensor->lock);
        sensor->status_code = ERR_PIN_INVALID;
        snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ERR_PIN_INVALID));
        mutex_unlock(&sensor->lock);
        return ERR_PIN_INVALID;
    }

    if (manual) {
        mutex_lock(&sensor->lock);
        now = ktime_get_real_seconds();
        if (sensor->last_attempt_time > 0 &&
            (now - sensor->last_attempt_time) < MEAS_MIN_GAP) {
            sensor->status_code = ERR_TOO_SOON;
            snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ERR_TOO_SOON));
            mutex_unlock(&sensor->lock);
            pin_dbg(sensor->pin, "manual measurement rejected - only %llds since last attempt\n",
                    (long long)(now - sensor->last_attempt_time));
            return ERR_TOO_SOON;
        }
        mutex_unlock(&sensor->lock);
    }

    ret = dht_read_sensor(sensor->pin, &hum, &temp, &type);

    mutex_lock(&sensor->lock);
    sensor->last_attempt_time = ktime_get_real_seconds();
    sensor->status_code = ret;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ret));

    if (ret == ERR_SUCCESS) {
        sensor->humidity_raw = hum;
        sensor->temperature_raw = temp;
        sensor->sensor_type = type;
        sensor->last_meas_time = sensor->last_attempt_time;
        mutex_unlock(&sensor->lock);
        pin_dbg(sensor->pin, "measurement OK - H=%d.%d%% T=%d.%d C\n",
                hum / 10, hum % 10, temp / 10, temp % 10);
        return ERR_SUCCESS;
    }

    mutex_unlock(&sensor->lock);
    pin_dbg(sensor->pin, "measurement failed - %s\n", error_str(ret));
    return ret;
}

/* ════════════════════════════════════════════════════════════════ */
/*  AUTO-POLL THREAD                                                 */
/* ════════════════════════════════════════════════════════════════ */

static int dht_poll_thread_fn(void *data)
{
    struct dht_sensor *sensor = data;
    int effective_interval;

    pin_dbg(sensor->pin, "poll thread started\n");

    while (!kthread_should_stop()) {
        mutex_lock(&sensor->lock);
        if (READ_ONCE(global_auto_interval) != -1)
            effective_interval = READ_ONCE(global_auto_interval);
        else
            effective_interval = sensor->interval;
        mutex_unlock(&sensor->lock);

        dht_do_measurement(sensor, false);

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

static void dht_start_poll(struct dht_sensor *sensor)
{
    if (sensor->poll_thread)
        return;
    sensor->poll_thread = kthread_run(dht_poll_thread_fn, sensor,
                                      "dht_poll_%d", sensor->pin);
    if (IS_ERR(sensor->poll_thread)) {
        pin_err(sensor->pin, "failed to create poll thread\n");
        sensor->poll_thread = NULL;
    }
}

static void dht_stop_poll(struct dht_sensor *sensor)
{
    struct task_struct *thread;
    if (!sensor->poll_thread)
        return;
    thread = sensor->poll_thread;
    sensor->poll_thread = NULL;
    kthread_stop(thread);
}

/* ── dht_sensor_free: unified cleanup (no lock held) ─────────────── */
static void dht_sensor_free(struct dht_sensor *sensor)
{
    dht_stop_poll(sensor);
    if (sensor->proc_dir)
        proc_remove(sensor->proc_dir);
    mutex_destroy(&sensor->lock);
    kfree(sensor);
}

/* ── Input parsing helper ────────────────────────────────────────── */
static int dht_parse_int(const char __user *buf, size_t count, int *out)
{
    char in[16];
    if (count >= sizeof(in))
        count = sizeof(in) - 1;
    if (copy_from_user(in, buf, count))
        return -EFAULT;
    in[count] = '\0';
    return kstrtoint(strim(in), 10, out);
}

/* ════════════════════════════════════════════════════════════════ */
/*  GLOBAL PROCFS HANDLERS                                          */
/* ════════════════════════════════════════════════════════════════ */

static ssize_t debug_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    char out[16];
    int len;
    if (*pos > 0) return 0;
    len = snprintf(out, sizeof(out), "%d\n", READ_ONCE(dht_debug));
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t debug_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret) return ret;
    if (val != 0 && val != 1) return -EINVAL;
    WRITE_ONCE(dht_debug, val);
    dht_info("debug %s\n", val ? "enabled" : "disabled");
    return count;
}

static ssize_t version_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    char out[32];
    int len;
    if (*pos > 0) return 0;
    len = snprintf(out, sizeof(out), "%s\n", DHT_DRIVER_VERSION);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t auto_interval_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    char out[16];
    int len;
    if (*pos > 0) return 0;
    len = snprintf(out, sizeof(out), "%d\n", READ_ONCE(global_auto_interval));
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t auto_interval_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret) return ret;

    mutex_lock(&list_lock);
    if (val >= MIN_INTERVAL && val <= MAX_INTERVAL)
        WRITE_ONCE(global_auto_interval, val);
    else
        WRITE_ONCE(global_auto_interval, -1);

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

/* ── Global fops ─────────────────────────────────────────────────── */
static const DHT_PROC_OPS debug_fops = {
    DHT_PROC_READ  = debug_read,
    DHT_PROC_WRITE = debug_write,
};
static const DHT_PROC_OPS version_fops = {
    DHT_PROC_READ = version_read,
};
static const DHT_PROC_OPS auto_interval_fops = {
    DHT_PROC_READ  = auto_interval_read,
    DHT_PROC_WRITE = auto_interval_write,
};

/* ════════════════════════════════════════════════════════════════ */
/*  PER-SENSOR PROCFS HANDLERS                                      */
/* ════════════════════════════════════════════════════════════════ */

static ssize_t sensor_pin_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[16];
    int len;
    if (*pos > 0) return 0;
    len = snprintf(out, sizeof(out), "%d\n", sensor->pin);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_interval_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[16];
    int len;
    if (*pos > 0) return 0;
    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%d\n", sensor->interval);
    mutex_unlock(&sensor->lock);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_interval_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret) return ret;
    if (val != -1 && (val < MIN_INTERVAL || val > MAX_INTERVAL))
        return -EINVAL;

    mutex_lock(&sensor->lock);
    sensor->interval = val;
    mutex_unlock(&sensor->lock);

    if (READ_ONCE(global_auto_interval) != -1) {
        pin_dbg(sensor->pin, "local interval ignored - global auto active\n");
        return count;
    }

    if (val == -1) {
        dht_stop_poll(sensor);
        pin_dbg(sensor->pin, "auto-poll disabled\n");
    } else {
        dht_start_poll(sensor);
        pin_dbg(sensor->pin, "auto-poll enabled (interval=%d)\n", val);
    }
    return count;
}

static ssize_t sensor_measure_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    int val;
    int ret = dht_parse_int(buf, count, &val);
    if (ret) return ret;
    if (val != 1) return -EINVAL;

    mutex_lock(&sensor->lock);
    if (sensor->interval != -1 || READ_ONCE(global_auto_interval) != -1) {
        sensor->status_code = ERR_AUTO_MODE;
        snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ERR_AUTO_MODE));
        mutex_unlock(&sensor->lock);
        pin_dbg(sensor->pin, "manual measure ignored (auto mode)\n");
        return count;
    }
    mutex_unlock(&sensor->lock);

    pin_dbg(sensor->pin, "manual measure triggered\n");
    dht_do_measurement(sensor, true);
    return count;
}

static ssize_t sensor_status_code_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[16];
    int len;
    if (*pos > 0) return 0;
    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%d\n", sensor->status_code);
    mutex_unlock(&sensor->lock);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_status_text_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[STATUS_BUF_LEN + 2];
    int len;
    if (*pos > 0) return 0;
    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%s\n", sensor->status_text);
    mutex_unlock(&sensor->lock);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_value_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[64];
    int len, hum, temp;
    if (*pos > 0) return 0;
    mutex_lock(&sensor->lock);
    hum = sensor->humidity_raw;
    temp = sensor->temperature_raw;
    mutex_unlock(&sensor->lock);

    if (temp < 0)
        len = snprintf(out, sizeof(out), "H=%d.%d\nT=-%d.%d\n",
                       hum / 10, hum % 10, (-temp) / 10, (-temp) % 10);
    else
        len = snprintf(out, sizeof(out), "H=%d.%d\nT=%d.%d\n",
                       hum / 10, hum % 10, temp / 10, temp % 10);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_info_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[INFO_BUF_LEN];
    char time_buf[32];
    int len, type_copy;
    time64_t reg_time_copy;

    if (*pos > 0) return 0;

    mutex_lock(&sensor->lock);
    if (sensor->sensor_type == SENSOR_TYPE_UNKNOWN) {
        mutex_unlock(&sensor->lock);
        return 0;
    }
    reg_time_copy = sensor->register_time;
    type_copy = sensor->sensor_type;
    mutex_unlock(&sensor->lock);

    dht_format_iso_time(reg_time_copy, time_buf, sizeof(time_buf));
    len = snprintf(out, sizeof(out),
                   "Sensor type: DHT%d\nRegister time: %s\n",
                   (type_copy == SENSOR_TYPE_DHT11 ? 11 : 22), time_buf);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_timestamp_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[32];
    int len;
    if (*pos > 0) return 0;
    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%lld\n", (long long)sensor->last_meas_time);
    mutex_unlock(&sensor->lock);
    if (copy_to_user(buf, out, len)) return -EFAULT;
    *pos += len;
    return len;
}

/* ── Per-sensor fops ─────────────────────────────────────────────── */
static const DHT_PROC_OPS sensor_pin_fops         = { DHT_PROC_READ  = sensor_pin_read };
static const DHT_PROC_OPS sensor_interval_fops    = { DHT_PROC_READ  = sensor_interval_read,
                                                      DHT_PROC_WRITE = sensor_interval_write };
static const DHT_PROC_OPS sensor_measure_fops     = { DHT_PROC_WRITE = sensor_measure_write };
static const DHT_PROC_OPS sensor_status_code_fops = { DHT_PROC_READ  = sensor_status_code_read };
static const DHT_PROC_OPS sensor_status_text_fops = { DHT_PROC_READ  = sensor_status_text_read };
static const DHT_PROC_OPS sensor_value_fops        = { DHT_PROC_READ  = sensor_value_read };
static const DHT_PROC_OPS sensor_info_fops         = { DHT_PROC_READ  = sensor_info_read };
static const DHT_PROC_OPS sensor_timestamp_fops    = { DHT_PROC_READ  = sensor_timestamp_read };

/* ── Per-sensor proc entry table ─────────────────────────────────── */
struct proc_entry_def {
    const char *name;
    umode_t mode;
    const DHT_PROC_OPS *fops;
};

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

static int dht_create_sensor_proc(struct dht_sensor *sensor)
{
    char name[32];
    int i;

    snprintf(name, sizeof(name), "gpio%d", sensor->pin);
    sensor->proc_dir = proc_mkdir(name, proc_dir);
    if (!sensor->proc_dir)
        return -ENOMEM;

    for (i = 0; i < ARRAY_SIZE(sensor_proc_entries); i++) {
        const struct proc_entry_def *e = &sensor_proc_entries[i];
        if (!proc_create_data(e->name, e->mode, sensor->proc_dir, e->fops, sensor)) {
            proc_remove(sensor->proc_dir);
            return -ENOMEM;
        }
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════ */
/*  EXPORT / UNEXPORT                                                */
/* ════════════════════════════════════════════════════════════════ */

static ssize_t export_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int pin;
    struct dht_sensor *sensor, *s;
    int ret;

    ret = dht_parse_int(buf, count, &pin);
    if (ret) return ret;
    if (pin < 0 || pin > MAX_PIN_NUM) return -EINVAL;

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
    sensor = kzalloc(sizeof(*sensor), GFP_KERNEL);
    if (!sensor) {
        mutex_unlock(&list_lock);
        return -ENOMEM;
    }
    sensor->pin = pin;
    sensor->interval = -1;
    sensor->status_code = ERR_SUCCESS;
    sensor->register_time = ktime_get_real_seconds();
    sensor->sensor_type = SENSOR_TYPE_UNKNOWN;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "No measurement taken");
    mutex_init(&sensor->lock);

    list_add(&sensor->list, &sensor_list);
    sensor_count++;
    mutex_unlock(&list_lock);

    if (dht_create_sensor_proc(sensor)) {
        mutex_lock(&list_lock);
        list_del(&sensor->list);
        sensor_count--;
        mutex_unlock(&list_lock);
        kfree(sensor);
        return -ENOMEM;
    }

    {
        int base, global;
        struct gpio_desc *desc = dht_find_desc(pin, &base, &global);
        if (desc) {
            if (base >= 0)
                pin_log(pin, "found on '%s' (base=%d, global=%d)\n",
                        gpiod_to_chip(desc)->label, base, global);
        } else {
            pin_err(pin, "GPIO descriptor not found\n");
        }
    }

    ret = dht_do_measurement(sensor, false);
    if (ret != ERR_SUCCESS) {
        mutex_lock(&list_lock);
        list_del(&sensor->list);
        sensor_count--;
        mutex_unlock(&list_lock);
        dht_sensor_free(sensor);
        pin_err(pin, "registration failed - %s\n", error_str(ret));
        return -EIO;
    }

    if (READ_ONCE(global_auto_interval) != -1) {
        dht_start_poll(sensor);
        pin_dbg(pin, "auto-poll enabled by global setting\n");
    }

    pin_log(pin, "registered successfully\n");
    return count;
}

static ssize_t unexport_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
    int pin;
    struct dht_sensor *sensor, *tmp;
    int ret;

    ret = dht_parse_int(buf, count, &pin);
    if (ret) return ret;

    mutex_lock(&list_lock);
    list_for_each_entry_safe(sensor, tmp, &sensor_list, list) {
        if (sensor->pin == pin) {
            list_del(&sensor->list);
            sensor_count--;
            mutex_unlock(&list_lock);
            dht_sensor_free(sensor);
            pin_dbg(pin, "unregistered\n");
            return count;
        }
    }
    mutex_unlock(&list_lock);
    dht_dbg("pin %d not registered\n", pin);
    return -ENODEV;
}

/* ── Export/unexport fops (must come AFTER function definitions) ─── */
static const DHT_PROC_OPS export_fops = {
    DHT_PROC_WRITE = export_write,
};
static const DHT_PROC_OPS unexport_fops = {
    DHT_PROC_WRITE = unexport_write,
};

/* ════════════════════════════════════════════════════════════════ */
/*  MODULE INIT / EXIT                                               */
/* ════════════════════════════════════════════════════════════════ */

static int __init dht_driver_init(void)
{
    proc_parent = proc_mkdir(PROC_PARENT, NULL);
    if (!proc_parent) {
        remove_proc_subtree(PROC_PARENT, NULL);
        proc_parent = proc_mkdir(PROC_PARENT, NULL);
        if (!proc_parent) {
            dht_err("failed to create /proc/%s\n", PROC_PARENT);
            return -ENOMEM;
        }
    }

    proc_dir = proc_mkdir(PROC_DIR_NAME, proc_parent);
    if (!proc_dir) {
        dht_err("failed to create /proc/%s/%s\n", PROC_PARENT, PROC_DIR_NAME);
        proc_remove(proc_parent);
        return -ENOMEM;
    }

    if (!proc_create("debug",         0666, proc_dir, &debug_fops)         ||
        !proc_create("version",        0444, proc_dir, &version_fops)       ||
        !proc_create("export",         0222, proc_dir, &export_fops)        ||
        !proc_create("unexport",       0222, proc_dir, &unexport_fops)     ||
        !proc_create("auto_interval",  0666, proc_dir, &auto_interval_fops)) {
        dht_err("failed to create proc entries\n");
        proc_remove(proc_dir);
        proc_remove(proc_parent);
        return -ENOMEM;
    }

    dht_info("driver v%s loaded - /proc/%s/%s/ (max %d sensors)\n",
             DHT_DRIVER_VERSION, PROC_PARENT, PROC_DIR_NAME, MAX_SENSORS);
    return 0;
}

static void __exit dht_driver_exit(void)
{
    struct dht_sensor *sensor, *tmp;
    LIST_HEAD(tmp_list);

    mutex_lock(&list_lock);
    list_splice_init(&sensor_list, &tmp_list);
    sensor_count = 0;
    mutex_unlock(&list_lock);

    list_for_each_entry_safe(sensor, tmp, &tmp_list, list) {
        list_del(&sensor->list);
        dht_sensor_free(sensor);
    }

    proc_remove(proc_dir);
    proc_remove(proc_parent);
    dht_info("driver unloaded\n");
}

module_init(dht_driver_init);
module_exit(dht_driver_exit);
