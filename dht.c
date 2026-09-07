/*
 *  dht.c — Kernel module for DHT11/DHT22 multi-sensor support
 *  Compatible with: Raspberry Pi 3 (bcm2835), Pi 4 (bcm2711), Pi 5 (bcm2712)
 *
 *  /proc/sensors/dht/
 *    debug         (rw) - debug logging: 0 = off (default), 1 = on
 *    version       (r)  - driver version
 *    export        (w)  - write BCM pin number to register a new sensor
 *    unexport      (w)  - write BCM pin number to unregister a sensor
 *
 *  /proc/sensors/dht/gpio<pin>/
 *    pin           (r)  - BCM GPIO pin number
 *    interval      (rw) - auto-poll interval in seconds (2-60, -1 = off)
 *    measure       (w)  - write "1" to trigger measurement
 *                         (ignored if interval != -1)
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
#include <linux/stdarg.h>

#define DHT_DRIVER_VERSION  "2.1"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DHT Driver");
MODULE_DESCRIPTION("DHT11/DHT22 multi-sensor kernel module (bcm2712 compatible)");
MODULE_VERSION(DHT_DRIVER_VERSION);

#define MAX_TIMINGS      85
#define PROC_PARENT      "sensors"
#define PROC_DIR_NAME    "dht"
#define STATUS_BUF_LEN   128
#define LOG_BUF_LEN      256
#define INFO_BUF_LEN     256
#define MAX_SENSORS      32
#define MAX_PIN_NUM      27
#define MIN_INTERVAL     2
#define MAX_INTERVAL     60

/* ── Error codes ─────────────────────────────────────────────────── */
#define ERR_SUCCESS       0
#define ERR_PIN_INVALID   1
#define ERR_GPIO_REQUEST  2
#define ERR_READ_FAILED   3
#define ERR_AUTO_MODE     4

/* ── Sensor types ────────────────────────────────────────────────── */
#define SENSOR_TYPE_UNKNOWN  0
#define SENSOR_TYPE_DHT11    1
#define SENSOR_TYPE_DHT22    2

/* ── Version compatibility ────────────────────────────────────────── */

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
static int dht_debug = 0;

module_param(dht_debug, int, 0644);
MODULE_PARM_DESC(dht_debug, "Debug logging (0 = off, 1 = on)");

/* ════════════════════════════════════════════════════════════════ */
/*  LOGGING FUNCTIONS                                                */
/* ════════════════════════════════════════════════════════════════ */

static void dht_info(const char *fmt, ...)
{
    va_list args;
    char buf[LOG_BUF_LEN];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk(KERN_INFO "[DHT]: %s", buf);
}

static void dht_err(const char *fmt, ...)
{
    va_list args;
    char buf[LOG_BUF_LEN];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk(KERN_ERR "[DHT]: %s", buf);
}

static void dht_dbg(const char *fmt, ...)
{
    va_list args;
    char buf[LOG_BUF_LEN];

    if (!dht_debug)
        return;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk(KERN_INFO "[DHT]: %s", buf);
}

static void pin_log(int pin, const char *fmt, ...)
{
    va_list args;
    char buf[LOG_BUF_LEN];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk(KERN_INFO "[dht_gpio_%d]: %s", pin, buf);
}

static void pin_err(int pin, const char *fmt, ...)
{
    va_list args;
    char buf[LOG_BUF_LEN];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk(KERN_ERR "[dht_gpio_%d]: %s", pin, buf);
}

static void pin_dbg(int pin, const char *fmt, ...)
{
    va_list args;
    char buf[LOG_BUF_LEN];

    if (!dht_debug)
        return;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk(KERN_INFO "[dht_gpio_%d]: %s", pin, buf);
}

/* ════════════════════════════════════════════════════════════════ */
/*  TIME HELPERS                                                      */
/* ════════════════════════════════════════════════════════════════ */

static void dht_format_iso_time(time64_t seconds, char *buf, size_t size)
{
    struct tm tm;

    time64_to_tm(seconds, 0, &tm);
    snprintf(buf, size, "%04ld-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

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
    char info_text[INFO_BUF_LEN];
    bool info_filled;

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
static int sensor_count = 0;

/* ── Error code → text ───────────────────────────────────────────── */
static const char *error_str(int code)
{
    switch (code) {
    case ERR_SUCCESS:       return "SUCCESS";
    case ERR_PIN_INVALID:   return "Pin must be between 0 and 27";
    case ERR_GPIO_REQUEST:  return "GPIO request/lookup failed";
    case ERR_READ_FAILED:   return "Sensor data read failed";
    case ERR_AUTO_MODE:     return "Manual measure disabled in auto mode";
    default:                return "Unknown error";
    }
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

static struct gpio_desc *dht_find_desc(int bcm_pin)
{
    struct gpio_desc *desc;
    struct gpio_chip *chip;
    int i;

    desc = gpio_to_desc(bcm_pin);
    if (desc) {
        chip = gpiod_to_chip(desc);
        pin_dbg(bcm_pin, "direct match (chip: %s)\n",
                (chip && chip->label) ? chip->label : "unknown");
        return desc;
    }

    pin_dbg(bcm_pin, "direct lookup failed, scanning GPIO chips...\n");

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
            pin_dbg(bcm_pin, "found on '%s' (base=%d, global=%d)\n",
                    chip->label, chip->base, i);
            return desc;
        }

        if (chip->base < 0 && (i % 32) == bcm_pin) {
            pin_dbg(bcm_pin, "found on '%s' (global=%d, modulo heuristic)\n",
                    chip->label, i);
            return desc;
        }
    }

    pin_err(bcm_pin, "could not find BCM pin on any Pi GPIO chip\n");
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
    int global_gpio;
    int pin_requested = 0;
    int ret;

    desc = dht_find_desc(pin);
    if (!desc) {
        pin_err(pin, "GPIO descriptor not found\n");
        return ERR_GPIO_REQUEST;
    }

    global_gpio = desc_to_gpio(desc);

    ret = gpio_request(global_gpio, "dht_sensor");
    if (ret) {
        pin_dbg(pin, "gpio_request(%d) = %d — using descriptor directly\n",
                global_gpio, ret);
    } else {
        pin_requested = 1;
    }

    ret = gpiod_direction_output(desc, 0);
    if (ret) {
        pin_err(pin, "gpiod_direction_output failed: %d\n", ret);
        if (pin_requested)
            gpio_free(global_gpio);
        return ERR_GPIO_REQUEST;
    }
    mdelay(18);

    ret = gpiod_direction_input(desc);
    if (ret) {
        pin_err(pin, "gpiod_direction_input failed: %d\n", ret);
        if (pin_requested)
            gpio_free(global_gpio);
        return ERR_GPIO_REQUEST;
    }
    udelay(40);

    for (i = 0; i < MAX_TIMINGS; i++) {
        counter = 0;
        while (gpiod_get_value(desc) == last_state) {
            counter++;
            udelay(1);
            if (counter == 255)
                break;
        }
        last_state = gpiod_get_value(desc);
        if (counter == 255)
            break;

        if (i >= 4 && i % 2 == 0) {
            data[j / 8] <<= 1;
            if (counter > 16)
                data[j / 8] |= 1;
            j++;
        }
    }

    if (pin_requested)
        gpio_free(global_gpio);

    if (j >= 40 &&
        data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {

        int h = ((data[0] << 8) + data[1]);
        int c = (((data[2] & 0x7F) << 8) + data[3]);

        /* Detect sensor type:
         * DHT11: data[1] and data[3] are always 0, so h > 1000
         * DHT22: decimal bytes are used, so h <= 1000 */
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

    pin_dbg(pin, "read failed — j=%d, data=[%d,%d,%d,%d,%d]\n",
            j, data[0], data[1], data[2], data[3], data[4]);
    return ERR_READ_FAILED;
}

/* ── Perform a measurement (assumes sensor->lock held) ────────────── */
static void dht_do_measurement(struct dht_sensor *sensor)
{
    int hum = 0, temp = 0, type = 0;
    int ret;

    if (sensor->pin < 0 || sensor->pin > MAX_PIN_NUM) {
        sensor->status_code = ERR_PIN_INVALID;
        snprintf(sensor->status_text, STATUS_BUF_LEN, "%s",
                 error_str(ERR_PIN_INVALID));
        return;
    }

    ret = dht_read_sensor(sensor->pin, &hum, &temp, &type);
    sensor->status_code = ret;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "%s", error_str(ret));

    if (ret == ERR_SUCCESS) {
        sensor->humidity_raw = hum;
        sensor->temperature_raw = temp;
        sensor->sensor_type = type;
        sensor->last_meas_time = ktime_get_real_seconds();
        pin_dbg(sensor->pin, "measurement OK — H=%d.%d%% T=%d.%d C\n",
                hum / 10, hum % 10, temp / 10, temp % 10);
    }
}

/* ════════════════════════════════════════════════════════════════ */
/*  AUTO-POLL THREAD (per-sensor)                                     */
/* ════════════════════════════════════════════════════════════════ */

static int dht_poll_thread_fn(void *data)
{
    struct dht_sensor *sensor = data;
    int interval;

    pin_dbg(sensor->pin, "poll thread started (interval=%d)\n",
            sensor->interval);

    while (!kthread_should_stop()) {
        mutex_lock(&sensor->lock);
        interval = sensor->interval;
        dht_do_measurement(sensor);
        mutex_unlock(&sensor->lock);

        {
            int slept = 0;
            while (slept < interval && !kthread_should_stop()) {
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

/* ════════════════════════════════════════════════════════════════ */
/*  GLOBAL PROCFS: debug, version                                    */
/* ════════════════════════════════════════════════════════════════ */

static ssize_t debug_read(struct file *f, char __user *buf,
                          size_t count, loff_t *pos)
{
    char out[16];
    int len;

    if (*pos > 0)
        return 0;

    len = snprintf(out, sizeof(out), "%d\n", dht_debug);
    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t debug_write(struct file *f, const char __user *buf,
                           size_t count, loff_t *pos)
{
    char in[16];
    int val;

    if (count >= sizeof(in))
        count = sizeof(in) - 1;
    if (copy_from_user(in, buf, count))
        return -EFAULT;
    in[count] = '\0';

    if (kstrtoint(strim(in), 10, &val))
        return -EINVAL;

    if (val != 0 && val != 1)
        return -EINVAL;

    dht_debug = val;
    dht_info("debug %s\n", val ? "enabled" : "disabled");
    return count;
}

static const DHT_PROC_OPS debug_fops = {
    DHT_PROC_READ  = debug_read,
    DHT_PROC_WRITE = debug_write,
};

static ssize_t version_read(struct file *f, char __user *buf,
                            size_t count, loff_t *pos)
{
    char out[32];
    int len;

    if (*pos > 0)
        return 0;

    len = snprintf(out, sizeof(out), "%s\n", DHT_DRIVER_VERSION);
    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

static const DHT_PROC_OPS version_fops = {
    DHT_PROC_READ = version_read,
};

/* ════════════════════════════════════════════════════════════════ */
/*  PER-SENSOR PROCFS HANDLERS                                       */
/* ════════════════════════════════════════════════════════════════ */

static ssize_t sensor_pin_read(struct file *f, char __user *buf,
                               size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[16];
    int len;

    if (*pos > 0)
        return 0;

    len = snprintf(out, sizeof(out), "%d\n", sensor->pin);
    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_interval_read(struct file *f, char __user *buf,
                                     size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[16];
    int len;

    if (*pos > 0)
        return 0;

    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%d\n", sensor->interval);
    mutex_unlock(&sensor->lock);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_interval_write(struct file *f, const char __user *buf,
                                     size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char in[16];
    int val;

    if (count >= sizeof(in))
        count = sizeof(in) - 1;
    if (copy_from_user(in, buf, count))
        return -EFAULT;
    in[count] = '\0';

    if (kstrtoint(strim(in), 10, &val))
        return -EINVAL;

    if (val != -1 && (val < MIN_INTERVAL || val > MAX_INTERVAL))
        return -EINVAL;

    mutex_lock(&sensor->lock);
    sensor->interval = val;

    if (val == -1) {
        if (sensor->poll_thread) {
            mutex_unlock(&sensor->lock);
            dht_stop_poll(sensor);
            pin_dbg(sensor->pin, "auto-poll disabled\n");
            return count;
        }
    } else {
        if (!sensor->poll_thread) {
            dht_start_poll(sensor);
            pin_dbg(sensor->pin, "auto-poll enabled (interval=%d)\n", val);
        }
    }

    mutex_unlock(&sensor->lock);
    return count;
}

static ssize_t sensor_measure_write(struct file *f, const char __user *buf,
                                     size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char in[16];
    int val;

    if (count >= sizeof(in))
        count = sizeof(in) - 1;
    if (copy_from_user(in, buf, count))
        return -EFAULT;
    in[count] = '\0';

    if (kstrtoint(strim(in), 10, &val))
        return -EINVAL;
    if (val != 1)
        return -EINVAL;

    mutex_lock(&sensor->lock);

    if (sensor->interval != -1) {
        sensor->status_code = ERR_AUTO_MODE;
        snprintf(sensor->status_text, STATUS_BUF_LEN, "%s",
                 error_str(ERR_AUTO_MODE));
        pin_dbg(sensor->pin, "manual measure ignored (auto mode)\n");
    } else {
        pin_dbg(sensor->pin, "manual measure triggered\n");
        dht_do_measurement(sensor);
    }

    mutex_unlock(&sensor->lock);
    return count;
}

static ssize_t sensor_status_code_read(struct file *f, char __user *buf,
                                        size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[16];
    int len;

    if (*pos > 0)
        return 0;

    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%d\n", sensor->status_code);
    mutex_unlock(&sensor->lock);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_status_text_read(struct file *f, char __user *buf,
                                        size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[STATUS_BUF_LEN + 2];
    int len;

    if (*pos > 0)
        return 0;

    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%s\n", sensor->status_text);
    mutex_unlock(&sensor->lock);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

static ssize_t sensor_value_read(struct file *f, char __user *buf,
                                  size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[64];
    int len, hum, temp;

    if (*pos > 0)
        return 0;

    mutex_lock(&sensor->lock);
    hum = sensor->humidity_raw;
    temp = sensor->temperature_raw;
    mutex_unlock(&sensor->lock);

    if (temp < 0)
        len = snprintf(out, sizeof(out), "H=%d.%d\nT=-%d.%d\n",
                       hum / 10, hum % 10,
                       (-temp) / 10, (-temp) % 10);
    else
        len = snprintf(out, sizeof(out), "H=%d.%d\nT=%d.%d\n",
                       hum / 10, hum % 10,
                       temp / 10, temp % 10);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/* ── info (read-only) ────────────────────────────────────────────── */
static ssize_t sensor_info_read(struct file *f, char __user *buf,
                                 size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[INFO_BUF_LEN];
    int len;

    if (*pos > 0)
        return 0;

    mutex_lock(&sensor->lock);
    if (!sensor->info_filled) {
        mutex_unlock(&sensor->lock);
        return 0;
    }
    len = snprintf(out, sizeof(out), "%s", sensor->info_text);
    mutex_unlock(&sensor->lock);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/* ── timestamp (read-only) ───────────────────────────────────────── */
static ssize_t sensor_timestamp_read(struct file *f, char __user *buf,
                                      size_t count, loff_t *pos)
{
    struct dht_sensor *sensor = DHT_PDE_DATA(file_inode(f));
    char out[32];
    int len;

    if (*pos > 0)
        return 0;

    mutex_lock(&sensor->lock);
    len = snprintf(out, sizeof(out), "%lld\n",
                   (long long)sensor->last_meas_time);
    mutex_unlock(&sensor->lock);

    if (copy_to_user(buf, out, len))
        return -EFAULT;
    *pos += len;
    return len;
}

/* ── Per-sensor fops ──────────────────────────────────────────────── */

static const DHT_PROC_OPS sensor_pin_fops = {
    DHT_PROC_READ = sensor_pin_read,
};

static const DHT_PROC_OPS sensor_interval_fops = {
    DHT_PROC_READ  = sensor_interval_read,
    DHT_PROC_WRITE = sensor_interval_write,
};

static const DHT_PROC_OPS sensor_measure_fops = {
    DHT_PROC_WRITE = sensor_measure_write,
};

static const DHT_PROC_OPS sensor_status_code_fops = {
    DHT_PROC_READ = sensor_status_code_read,
};

static const DHT_PROC_OPS sensor_status_text_fops = {
    DHT_PROC_READ = sensor_status_text_read,
};

static const DHT_PROC_OPS sensor_value_fops = {
    DHT_PROC_READ = sensor_value_read,
};

static const DHT_PROC_OPS sensor_info_fops = {
    DHT_PROC_READ = sensor_info_read,
};

static const DHT_PROC_OPS sensor_timestamp_fops = {
    DHT_PROC_READ = sensor_timestamp_read,
};

/* ════════════════════════════════════════════════════════════════ */
/*  EXPORT / UNEXPORT                                                */
/* ════════════════════════════════════════════════════════════════ */

static int dht_create_sensor_proc(struct dht_sensor *sensor)
{
    char name[32];

    snprintf(name, sizeof(name), "gpio%d", sensor->pin);
    sensor->proc_dir = proc_mkdir(name, proc_dir);
    if (!sensor->proc_dir)
        return -ENOMEM;

    if (!proc_create_data("pin",         0444, sensor->proc_dir,
                          &sensor_pin_fops, sensor)         ||
        !proc_create_data("interval",    0666, sensor->proc_dir,
                          &sensor_interval_fops, sensor)    ||
        !proc_create_data("measure",     0222, sensor->proc_dir,
                          &sensor_measure_fops, sensor)     ||
        !proc_create_data("status_code", 0444, sensor->proc_dir,
                          &sensor_status_code_fops, sensor)  ||
        !proc_create_data("status_text", 0444, sensor->proc_dir,
                          &sensor_status_text_fops, sensor)  ||
        !proc_create_data("value",       0444, sensor->proc_dir,
                          &sensor_value_fops, sensor)        ||
        !proc_create_data("info",        0444, sensor->proc_dir,
                          &sensor_info_fops, sensor)         ||
        !proc_create_data("timestamp",   0444, sensor->proc_dir,
                          &sensor_timestamp_fops, sensor)) {
        proc_remove(sensor->proc_dir);
        return -ENOMEM;
    }

    return 0;
}

static void dht_fill_info(struct dht_sensor *sensor)
{
    char time_buf[32];

    dht_format_iso_time(sensor->register_time, time_buf, sizeof(time_buf));
    snprintf(sensor->info_text, sizeof(sensor->info_text),
             "Sensor type: DHT%d\nRegister time: %s\n",
             sensor->sensor_type == SENSOR_TYPE_DHT11 ? 11 : 22,
             time_buf);
    sensor->info_filled = true;
}

/* ── export (write-only) ──────────────────────────────────────────── */
static ssize_t export_write(struct file *f, const char __user *buf,
                            size_t count, loff_t *pos)
{
    char in[16];
    int pin;
    struct dht_sensor *sensor, *s;
    int ret;

    if (count >= sizeof(in))
        count = sizeof(in) - 1;
    if (copy_from_user(in, buf, count))
        return -EFAULT;
    in[count] = '\0';

    if (kstrtoint(strim(in), 10, &pin))
        return -EINVAL;

    if (pin < 0 || pin > MAX_PIN_NUM)
        return -EINVAL;

    mutex_lock(&list_lock);

    /* Check if already registered */
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
    sensor->info_filled = false;
    snprintf(sensor->status_text, STATUS_BUF_LEN, "No measurement taken");
    mutex_init(&sensor->lock);

    ret = dht_create_sensor_proc(sensor);
    if (ret) {
        kfree(sensor);
        mutex_unlock(&list_lock);
        return ret;
    }

    /* Perform first measurement — sensor is not yet in the list,
     * so unexport cannot race with us. list_lock is held to prevent
     * other export/unexport calls. */
    mutex_lock(&sensor->lock);
    dht_do_measurement(sensor);

    if (sensor->status_code == ERR_SUCCESS) {
        dht_fill_info(sensor);
    }
    mutex_unlock(&sensor->lock);

    if (sensor->status_code != ERR_SUCCESS) {
        /* First measurement failed — cancel registration */
        proc_remove(sensor->proc_dir);
        kfree(sensor);
        mutex_unlock(&list_lock);

        pin_err(pin, "registration failed — first measurement error: %s\n",
                error_str(sensor->status_code));
        return -EIO;
    }

    /* Success — add to list */
    list_add(&sensor->list, &sensor_list);
    sensor_count++;
    mutex_unlock(&list_lock);

    pin_log(pin, "registered → /proc/%s/%s/gpio%d\n",
            PROC_PARENT, PROC_DIR_NAME, pin);
    return count;
}

/* ── unexport (write-only) ────────────────────────────────────────── */
static ssize_t unexport_write(struct file *f, const char __user *buf,
                              size_t count, loff_t *pos)
{
    char in[16];
    int pin;
    struct dht_sensor *sensor, *tmp;

    if (count >= sizeof(in))
        count = sizeof(in) - 1;
    if (copy_from_user(in, buf, count))
        return -EFAULT;
    in[count] = '\0';

    if (kstrtoint(strim(in), 10, &pin))
        return -EINVAL;

    mutex_lock(&list_lock);

    list_for_each_entry_safe(sensor, tmp, &sensor_list, list) {
        if (sensor->pin == pin) {
            list_del(&sensor->list);
            sensor_count--;

            dht_stop_poll(sensor);
            proc_remove(sensor->proc_dir);
            kfree(sensor);

            mutex_unlock(&list_lock);
            pin_dbg(pin, "unregistered\n");
            return count;
        }
    }

    mutex_unlock(&list_lock);
    dht_dbg("pin %d not registered\n", pin);
    return -ENODEV;
}

/* ── Global fops ──────────────────────────────────────────────────── */

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
        dht_dbg("/proc/%s exists, cleaning up stale entries...\n",
                PROC_PARENT);
        remove_proc_subtree(PROC_PARENT, NULL);
        proc_parent = proc_mkdir(PROC_PARENT, NULL);
        if (!proc_parent) {
            dht_err("failed to create /proc/%s after cleanup\n",
                    PROC_PARENT);
            return -ENOMEM;
        }
    }

    proc_dir = proc_mkdir(PROC_DIR_NAME, proc_parent);
    if (!proc_dir) {
        dht_err("failed to create /proc/%s/%s\n",
                PROC_PARENT, PROC_DIR_NAME);
        proc_remove(proc_parent);
        return -ENOMEM;
    }

    if (!proc_create("debug",    0666, proc_dir, &debug_fops)    ||
        !proc_create("version",  0444, proc_dir, &version_fops)  ||
        !proc_create("export",   0222, proc_dir, &export_fops)   ||
        !proc_create("unexport", 0222, proc_dir, &unexport_fops)) {
        dht_err("failed to create proc entries\n");
        proc_remove(proc_dir);
        proc_remove(proc_parent);
        return -ENOMEM;
    }

    dht_info("driver v%s loaded — /proc/%s/%s/ (max %d sensors)\n",
             DHT_DRIVER_VERSION, PROC_PARENT, PROC_DIR_NAME, MAX_SENSORS);
    return 0;
}

static void __exit dht_driver_exit(void)
{
    struct dht_sensor *sensor, *tmp;

    mutex_lock(&list_lock);
    list_for_each_entry_safe(sensor, tmp, &sensor_list, list) {
        list_del(&sensor->list);
        dht_stop_poll(sensor);
        proc_remove(sensor->proc_dir);
        kfree(sensor);
    }
    sensor_count = 0;
    mutex_unlock(&list_lock);

    proc_remove(proc_dir);
    proc_remove(proc_parent);

    dht_info("driver unloaded\n");
}

module_init(dht_driver_init);
module_exit(dht_driver_exit);
