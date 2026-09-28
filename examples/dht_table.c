/*
 * dht_table.c — Reads DHT sensor data from procfs and prints a table.
 *
 * Compile:  cc -O2 -Wall -Wextra -o dht_table dht_table.c
 * Run:      ./dht_table
 *
 * Procfs layout (from dht.c):
 *   /proc/sensors/dht/
 *     version       (r)  — "2.9.2\n"
 *     debug         (rw) — "0\n" or "1\n"
 *     auto_interval (rw) — "-1\n" or "2".."60\n"
 *   /proc/sensors/dht/gpio<pin>/
 *     pin           (r)  — "4\n"
 *     value         (r)  — "H=45.2\nT=23.1\n"  (negative temp: "T=-5.3\n")
 *     info          (r)  — "Sensor type: DHT22\nRegister time: 2026-01-15T14:30:00Z\n"
 *                          (empty if no successful measurement yet)
 *     timestamp     (r)  — Unix seconds of last measurement, e.g. "1727529000\n"
 *
 * Field widths are derived from header lengths (or the date-string length
 * for the last-measurement column).  Data that is wider than its header
 * expands the column so nothing is truncated.
 *
 * Copyright (c) 2026, Chapvic
 * License: GPLv3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

#define PROC_BASE "/proc/sensors/dht"
#define MAX_SENSORS 32
#define BUF_SIZE 512

/* ── helpers ─────────────────────────────────────────────── */

/* Read a procfs file into a buffer; returns 1 on success, 0 on failure. */
static int read_proc_file(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0;
}

/* Trim trailing whitespace/newlines in place. */
static void trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t'))
        s[--len] = '\0';
}

/* ── sensor record ───────────────────────────────────────── */

typedef struct {
    int  pin;
    char type[8];          /* "DHT11", "DHT22", or "" */
    char humidity[16];     /* "45.2 %" or "N/A" */
    char temperature[16];  /* "23.1 C" or "N/A" */
    char datetime[32];     /* "28.09.2026 15:11:11" or "N/A" */
} sensor_t;

/* Parse "H=45.2\nT=-5.3\n" → humidity="45.2", temperature="-5.3". */
static void parse_value(const char *raw, char *hum, char *temp)
{
    hum[0] = temp[0] = '\0';
    const char *h = strstr(raw, "H=");
    const char *t = strstr(raw, "T=");
    if (h) {
        h += 2;
        const char *eol = strchr(h, '\n');
        size_t len = eol ? (size_t)(eol - h) : strlen(h);
        if (len > 15) len = 15;
        memcpy(hum, h, len);
        hum[len] = '\0';
    }
    if (t) {
        t += 2;
        const char *eol = strchr(t, '\n');
        size_t len = eol ? (size_t)(eol - t) : strlen(t);
        if (len > 15) len = 15;
        memcpy(temp, t, len);
        temp[len] = '\0';
    }
}

/* Parse info: "Sensor type: DHT22\nRegister time: ..." → type="DHT22". */
static void parse_info(const char *raw, char *type)
{
    type[0] = '\0';
    if (!raw[0])
        return;  /* no successful measurement yet */
    const char *p = strstr(raw, "Sensor type: ");
    if (p) {
        p += 13;  /* skip "Sensor type: " */
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 7) len = 7;
        memcpy(type, p, len);
        type[len] = '\0';
    }
}

/* Convert Unix timestamp → "dd.MM.yyyy HH:MM:SS" (local time). */
static void format_datetime(time_t ts, char *out, size_t outsz)
{
    struct tm *lt = localtime(&ts);
    if (!lt || ts <= 0) {
        strncpy(out, "N/A", outsz);
        return;
    }
    strftime(out, outsz, "%d.%m.%Y %H:%M:%S", lt);
}

/* ── field-width logic ────────────────────────────────────── */

/* Column definitions — width = max(header_len, widest_data). */
typedef struct {
    const char *header;
    int width;
} col_t;

/* Build a row string from up to 5 fields, 2-space gaps, each field
   right-justified within its column width. */
static void print_row(col_t *cols, const char *f1, const char *f2,
                      const char *f3, const char *f4, const char *f5)
{
    const char *vals[5] = {f1, f2, f3, f4, f5};
    for (int i = 0; i < 5; i++) {
        if (i > 0)
            printf("  ");
        int w = cols[i].width;
        int vlen = (int)strlen(vals[i]);
        if (vlen >= w)
            printf("%s", vals[i]);
        else
            printf("%*s%s", w - vlen, "", vals[i]);
    }
    printf("\n");
}

/* Print a separator line: dashes matching each column width. */
static void print_separator(col_t *cols)
{
    for (int i = 0; i < 5; i++) {
        if (i > 0)
            printf("  ");
        for (int j = 0; j < cols[i].width; j++)
            putchar('-');
    }
    printf("\n");
}

/* ── main ─────────────────────────────────────────────────── */

static int cmp_sensor(const void *a, const void *b)
{
    return ((const sensor_t *)a)->pin - ((const sensor_t *)b)->pin;
}

int main(void)
{
    char buf[BUF_SIZE];
    char path[256];

    /* ── Driver info block ──────────────────────────────── */

    char version[32]   = "N/A";
    char auto_int[32]  = "off";
    char debug_mode[8] = "off";

    snprintf(path, sizeof(path), "%s/version", PROC_BASE);
    if (read_proc_file(path, buf, sizeof(buf))) {
        trim(buf);
        snprintf(version, sizeof(version), "%s", buf);
    }

    snprintf(path, sizeof(path), "%s/auto_interval", PROC_BASE);
    if (read_proc_file(path, buf, sizeof(buf))) {
        trim(buf);
        int v = atoi(buf);
        if (v == -1)
            strncpy(auto_int, "off", sizeof(auto_int));
        else
            snprintf(auto_int, sizeof(auto_int), "%d sec", v);
    }

    snprintf(path, sizeof(path), "%s/debug", PROC_BASE);
    if (read_proc_file(path, buf, sizeof(buf))) {
        trim(buf);
        snprintf(debug_mode, sizeof(debug_mode), "%s",
                 atoi(buf) ? "on" : "off");
    }

    /* ── Collect sensors ───────────────────────────────── */

    sensor_t sensors[MAX_SENSORS];
    int nsensors = 0;

    DIR *dir = opendir(PROC_BASE);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "gpio", 4) != 0)
                continue;
            int pin = atoi(de->d_name + 4);
            if (nsensors >= MAX_SENSORS)
                break;

            sensor_t *s = &sensors[nsensors];
            s->pin = pin;
            s->type[0] = '\0';
            strcpy(s->humidity, "N/A");
            strcpy(s->temperature, "N/A");
            strcpy(s->datetime, "N/A");

            /* info → type */
            snprintf(path, sizeof(path), "%s/%s/info", PROC_BASE, de->d_name);
            if (read_proc_file(path, buf, sizeof(buf)))
                parse_info(buf, s->type);
            if (s->type[0] == '\0')
                strcpy(s->type, "N/A");

            /* value → humidity, temperature */
            snprintf(path, sizeof(path), "%s/%s/value", PROC_BASE, de->d_name);
            if (read_proc_file(path, buf, sizeof(buf))) {
                char hum[16], temp[16];
                parse_value(buf, hum, temp);
                if (hum[0])
                    snprintf(s->humidity, sizeof(s->humidity), "%s %%", hum);
                if (temp[0])
                    snprintf(s->temperature, sizeof(s->temperature), "%s C", temp);
            }

            /* timestamp → datetime */
            snprintf(path, sizeof(path), "%s/%s/timestamp", PROC_BASE, de->d_name);
            if (read_proc_file(path, buf, sizeof(buf))) {
                trim(buf);
                time_t ts = (time_t)atoll(buf);
                if (ts > 0)
                    format_datetime(ts, s->datetime, sizeof(s->datetime));
            }

            nsensors++;
        }
        closedir(dir);
    }

    if (nsensors > 1)
        qsort(sensors, nsensors, sizeof(sensor_t), cmp_sensor);

    /* ── Print driver info ─────────────────────────────── */

    printf("DHT Driver Status\n");
    printf("=================\n");
    printf("Version         %s\n", version);
    printf("Procfs path     %s\n", PROC_BASE);
    printf("Auto-interval   %s\n", auto_int);
    printf("Debug mode      %s\n", debug_mode);
    printf("Sensors         %d\n", nsensors);

    if (nsensors == 0)
        return 0;

    /* ── Compute column widths ─────────────────────────── */

    /*
     * Field widths per user request:
     *   — width = length of header for each column
     *   — date column: width = length of "dd.MM.yyyy HH:MM:SS" = 19
     *   — if data is wider than header, expand to fit
     */

    const char *h_type  = "Type";
    const char *h_pin   = "Pin";
    const char *h_hum   = "Humidity";
    const char *h_temp  = "Temperature";
    const char *h_date  = "Last measurement";
    int date_str_len = (int)strlen("dd.MM.yyyy HH:MM:SS");  /* 19 */

    int w_type = (int)strlen(h_type);    /* 4 */
    int w_pin  = (int)strlen(h_pin);     /* 3 */
    int w_hum  = (int)strlen(h_hum);     /* 8 */
    int w_temp = (int)strlen(h_temp);    /* 11 */
    int w_date = date_str_len;           /* 19 */

    for (int i = 0; i < nsensors; i++) {
        int t = (int)strlen(sensors[i].type);
        if (t > w_type) w_type = t;
        int p = snprintf(NULL, 0, "%d", sensors[i].pin);
        if (p > w_pin) w_pin = p;
        int h = (int)strlen(sensors[i].humidity);
        if (h > w_hum) w_hum = h;
        int tp = (int)strlen(sensors[i].temperature);
        if (tp > w_temp) w_temp = tp;
        int d = (int)strlen(sensors[i].datetime);
        if (d > w_date) w_date = d;
    }

    col_t cols[5] = {
        { h_type, w_type },
        { h_pin,  w_pin  },
        { h_hum,  w_hum  },
        { h_temp, w_temp },
        { h_date, w_date },
    };

    printf("\n");

    /* Header row */
    print_row(cols, h_type, h_pin, h_hum, h_temp, h_date);

    /* Separator — dashes = field width for each column */
    print_separator(cols);

    /* Data rows */
    for (int i = 0; i < nsensors; i++) {
        char pinbuf[8];
        snprintf(pinbuf, sizeof(pinbuf), "%d", sensors[i].pin);
        print_row(cols,
                   sensors[i].type,
                   pinbuf,
                   sensors[i].humidity,
                   sensors[i].temperature,
                   sensors[i].datetime);
    }

    return 0;
}
