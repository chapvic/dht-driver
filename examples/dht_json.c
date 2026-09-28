/*
 * dht_json.c - Read DHT sensor data from procfs and output JSON
 *
 * Reads /proc/sensors/dht/ directory, finds all gpio<pin> subdirectories,
 * reads per-sensor proc files (value, status_code, status_text, info,
 * timestamp, pin, interval), and outputs a structured JSON object.
 *
 * Compile:  cc -O2 -Wall -Wextra -o dht_json dht_json.c
 * Run:      ./dht_json
 *
 * Copyright (c) 2026, Chapvic
 * License: GPLv3
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>

#define PROC_BASE "/proc/sensors/dht"
#define MAX_SENSORS 32
#define LINE_BUF 512

/* ---- Helpers ---------------------------------------------------- */

/* Read entire content of a file into a buffer (truncated to bufsize-1).
 * Returns 0 on success, -1 on error. The buffer is null-terminated.
 * Trailing whitespace/newline is stripped. */
static int read_proc_file(const char *path, char *buf, size_t bufsize)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    size_t n = fread(buf, 1, bufsize - 1, fp);
    int err = ferror(fp);
    fclose(fp);

    if (err)
        return -1;

    buf[n] = '\0';

    /* Strip trailing whitespace (newline, spaces, etc.) */
    while (n > 0 && isspace((unsigned char)buf[n - 1]))
        buf[--n] = '\0';

    return 0;
}

/* Read first line of a file as an integer.
 * Returns 0 on success, -1 on error. */
static int read_proc_int(const char *path, int *out)
{
    char buf[64];
    if (read_proc_file(path, buf, sizeof(buf)) < 0)
        return -1;
    if (strlen(buf) == 0)
        return -1;
    char *end;
    long val = strtol(buf, &end, 10);
    if (end == buf)
        return -1;
    *out = (int)val;
    return 0;
}

/* Read first line of a file as a long long.
 * Returns 0 on success, -1 on error. */
static int read_proc_ll(const char *path, long long *out)
{
    char buf[64];
    if (read_proc_file(path, buf, sizeof(buf)) < 0)
        return -1;
    if (strlen(buf) == 0)
        return -1;
    char *end;
    long long val = strtoll(buf, &end, 10);
    if (end == buf)
        return -1;
    *out = val;
    return 0;
}

/* ---- Value parsing ---------------------------------------------- */

/* Parse the "value" proc file which has the format:
 *   H=<humidity>\n
 *   T=<temperature>\n
 * Humidity and temperature are decimal numbers with one fractional digit.
 * Temperature can be negative (e.g., "T=-5.3").
 * Returns 0 on success, -1 on error. */
static int parse_value(const char *content, double *humidity, double *temperature)
{
    const char *h_line = content;
    const char *t_line = strstr(content, "\nT=");

    if (!h_line || strncmp(h_line, "H=", 2) != 0)
        return -1;

    char *end;
    *humidity = strtod(h_line + 2, &end);
    if (end == h_line + 2)
        return -1;

    if (!t_line)
        return -1;
    t_line += 3; /* skip "\nT=" */
    *temperature = strtod(t_line, &end);
    if (end == t_line)
        return -1;

    return 0;
}

/* Parse the "info" proc file which has the format:
 *   Sensor type: DHT11\n
 *   Register time: 2026-01-15T14:30:00Z\n
 * Returns 0 on success, -1 if info is empty or unparseable. */
static int parse_info(const char *content, char *type_buf, size_t type_sz,
                      char *reg_time_buf, size_t reg_time_sz)
{
    /* info is empty when sensor type is unknown (no successful measurement) */
    if (strlen(content) == 0)
        return -1;

    /* Extract sensor type: "Sensor type: DHT11" or "Sensor type: DHT22" */
    const char *p = strstr(content, "Sensor type: ");
    if (p) {
        p += strlen("Sensor type: ");
        size_t i = 0;
        while (*p && *p != '\n' && i < type_sz - 1)
            type_buf[i++] = *p++;
        type_buf[i] = '\0';
    }

    /* Extract registration time: "Register time: 2026-01-15T14:30:00Z" */
    p = strstr(content, "Register time: ");
    if (p) {
        p += strlen("Register time: ");
        size_t i = 0;
        while (*p && *p != '\n' && i < reg_time_sz - 1)
            reg_time_buf[i++] = *p++;
        reg_time_buf[i] = '\0';
    }

    return 0;
}

/* ---- JSON output helpers ---------------------------------------- */

/* Output a JSON string with proper escaping. */
static void json_escape(const char *s)
{
    putchar('"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout);  break;
        case '\r': fputs("\\r", stdout);  break;
        case '\t': fputs("\\t", stdout);  break;
        default:
            if ((unsigned char)*p < 0x20)
                printf("\\u%04x", (unsigned char)*p);
            else
                putchar(*p);
        }
    }
    putchar('"');
}

/* ---- Sensor structure ------------------------------------------- */

typedef struct {
    int pin;
    int interval;
    int status_code;
    char status_text[128];
    char value_raw[256];
    char info_raw[256];
    long long timestamp;

    /* parsed fields */
    int has_value;
    double humidity;
    double temperature;

    int has_info;
    char sensor_type[16];
    char reg_time[40];
} sensor_t;

/* Read all proc files for one sensor directory. */
static void read_sensor(sensor_t *s, const char *dirpath)
{
    char path[512];

    /* pin */
    snprintf(path, sizeof(path), "%s/pin", dirpath);
    if (read_proc_int(path, &s->pin) < 0)
        s->pin = -1;

    /* interval */
    snprintf(path, sizeof(path), "%s/interval", dirpath);
    if (read_proc_int(path, &s->interval) < 0)
        s->interval = -1;

    /* status_code */
    snprintf(path, sizeof(path), "%s/status_code", dirpath);
    if (read_proc_int(path, &s->status_code) < 0)
        s->status_code = -1;

    /* status_text */
    snprintf(path, sizeof(path), "%s/status_text", dirpath);
    if (read_proc_file(path, s->status_text, sizeof(s->status_text)) < 0)
        s->status_text[0] = '\0';

    /* value */
    snprintf(path, sizeof(path), "%s/value", dirpath);
    if (read_proc_file(path, s->value_raw, sizeof(s->value_raw)) < 0) {
        s->value_raw[0] = '\0';
        s->has_value = 0;
    } else {
        s->has_value = (parse_value(s->value_raw, &s->humidity, &s->temperature) == 0);
    }

    /* info */
    snprintf(path, sizeof(path), "%s/info", dirpath);
    if (read_proc_file(path, s->info_raw, sizeof(s->info_raw)) < 0) {
        s->info_raw[0] = '\0';
        s->has_info = 0;
    } else {
        s->has_info = (parse_info(s->info_raw, s->sensor_type, sizeof(s->sensor_type),
                                  s->reg_time, sizeof(s->reg_time)) == 0);
    }

    /* timestamp */
    snprintf(path, sizeof(path), "%s/timestamp", dirpath);
    if (read_proc_ll(path, &s->timestamp) < 0)
        s->timestamp = 0;
}

/* Output one sensor as a JSON object. */
static void print_sensor_json(const sensor_t *s, int is_last)
{
    printf("    {\n");
    printf("      \"pin\": %d,\n", s->pin);
    printf("      \"interval\": %d,\n", s->interval);
    printf("      \"status_code\": %d,\n", s->status_code);
    printf("      \"status_text\": ");
    json_escape(s->status_text);
    printf(",\n");

    printf("      \"timestamp\": %lld,\n", s->timestamp);

    if (s->has_value) {
        printf("      \"humidity\": %.1f,\n", s->humidity);
        printf("      \"temperature\": %.1f,\n", s->temperature);
    } else {
        printf("      \"humidity\": null,\n");
        printf("      \"temperature\": null,\n");
    }

    if (s->has_info) {
        printf("      \"sensor_type\": ");
        json_escape(s->sensor_type);
        printf(",\n");
        printf("      \"registered_at\": ");
        json_escape(s->reg_time);
        printf("\n");
    } else {
        printf("      \"sensor_type\": null,\n");
        printf("      \"registered_at\": null\n");
    }

    printf("    }%s\n", is_last ? "" : ",");
}

/* ---- Main ------------------------------------------------------- */

int main(void)
{
    DIR *dir = opendir(PROC_BASE);
    if (!dir) {
        fprintf(stderr, "Error: cannot open %s: %s\n",
                PROC_BASE, strerror(errno));
        return 1;
    }

    /* Collect sensor directory names */
    char *dirs[MAX_SENSORS];
    int n_sensors = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "gpio", 4) != 0)
            continue;

        /* Verify the rest is a number */
        char *end;
        long pin = strtol(ent->d_name + 4, &end, 10);
        if (*end != '\0' || end == ent->d_name + 4 || pin < 0 || pin > 27)
            continue;

        if (n_sensors >= MAX_SENSORS)
            break;

        dirs[n_sensors] = strdup(ent->d_name);
        if (!dirs[n_sensors]) {
            fprintf(stderr, "Error: out of memory\n");
            closedir(dir);
            return 1;
        }
        n_sensors++;
    }
    closedir(dir);

    /* Sort by pin number for deterministic output */
    for (int i = 0; i < n_sensors - 1; i++) {
        for (int j = i + 1; j < n_sensors; j++) {
            long pi = strtol(dirs[i] + 4, NULL, 10);
            long pj = strtol(dirs[j] + 4, NULL, 10);
            if (pi > pj) {
                char *tmp = dirs[i];
                dirs[i] = dirs[j];
                dirs[j] = tmp;
            }
        }
    }

    /* Read global entries */
    char gpath[512];
    char version_buf[64] = "";
    int debug_flag = 0;
    int auto_interval = -1;

    snprintf(gpath, sizeof(gpath), "%s/version", PROC_BASE);
    read_proc_file(gpath, version_buf, sizeof(version_buf));

    snprintf(gpath, sizeof(gpath), "%s/debug", PROC_BASE);
    read_proc_int(gpath, &debug_flag);

    snprintf(gpath, sizeof(gpath), "%s/auto_interval", PROC_BASE);
    read_proc_int(gpath, &auto_interval);

    /* Read all sensors */
    sensor_t sensors[MAX_SENSORS];
    for (int i = 0; i < n_sensors; i++) {
        char dpath[512];
        snprintf(dpath, sizeof(dpath), "%s/%s", PROC_BASE, dirs[i]);
        memset(&sensors[i], 0, sizeof(sensor_t));
        sensors[i].interval = -1;
        sensors[i].pin = -1;
        read_sensor(&sensors[i], dpath);
    }

    /* ---- Output JSON ---- */
    printf("{\n");
    printf("  \"driver\": \"dht\",\n");
    printf("  \"version\": ");
    json_escape(version_buf);
    printf(",\n");
    printf("  \"procfs_path\": ");
    json_escape(PROC_BASE);
    printf(",\n");
    printf("  \"debug\": %s,\n", debug_flag ? "true" : "false");
    printf("  \"auto_interval\": %d,\n", auto_interval);
    printf("  \"sensor_count\": %d,\n", n_sensors);
    printf("  \"sensors\": [\n");

    for (int i = 0; i < n_sensors; i++)
        print_sensor_json(&sensors[i], i == n_sensors - 1);

    printf("  ]\n");
    printf("}\n");

    /* Cleanup */
    for (int i = 0; i < n_sensors; i++)
        free(dirs[i]);

    return 0;
}
