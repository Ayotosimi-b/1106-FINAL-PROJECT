/*
 * CSE 1106 Final Project - Option 3: Raspberry Pi Equipment Health Monitor
 *
 * Reads memory and storage usage, converts to percentages, compares each
 * against a warning threshold, prints HEALTHY/WARNING, logs the result,
 * and drives a warning LED on GPIO 17 (physical pin 11).
 *
 * Build on the Pi:      gcc -Wall -o health src/main.c -lpigpio
 * Build on a laptop:    gcc -Wall -DNO_GPIO -o health src/main.c   (LED is simulated)
 * Run from repo root:   sudo ./health        (pigpio needs sudo)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>

#ifdef NO_GPIO
  /* Stand-ins so the program compiles/tests on a normal computer */
  #define PI_OUTPUT 1
  #define gpioInitialise()   0
  #define gpioSetMode(p, m)  ((void)0)
  #define gpioWrite(p, v)    printf("  [LED %s]\n", (v) ? "ON" : "OFF")
  #define gpioTerminate()    ((void)0)
#else
  #include <pigpio.h>
#endif

#define LED_PIN            17
#define DEFAULT_THRESHOLD  80.0
#define LOG_FILE           "data/health_log.csv"
#define STATUS_FILE        "status.txt"

static int led_ready = 0;

/* ---------- Data gathering ---------- */

/* Memory used % from /proc/meminfo. Returns -1 on failure. */
static double get_memory_percent(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    char line[128];
    double total = 0.0, avail = 0.0;

    if (fp == NULL) return -1.0;
    while (fgets(line, sizeof line, fp) != NULL) {
        sscanf(line, "MemTotal: %lf", &total);
        sscanf(line, "MemAvailable: %lf", &avail);
    }
    fclose(fp);

    if (total <= 0.0) return -1.0;
    return (total - avail) / total * 100.0;
}

/* Storage used % of the root filesystem (matches what `df /` shows). */
static double get_storage_percent(void)
{
    struct statvfs fs;
    double used, avail;

    if (statvfs("/", &fs) != 0) return -1.0;
    used  = (double)(fs.f_blocks - fs.f_bfree) * fs.f_frsize;
    avail = (double)fs.f_bavail * fs.f_frsize;
    if (used + avail <= 0.0) return -1.0;
    return used / (used + avail) * 100.0;
}

/* ---------- Logic ---------- */

/* WARNING if at least one resource EXCEEDS the threshold (strictly greater). */
static int is_warning(double mem, double storage, double threshold)
{
    return (mem > threshold) || (storage > threshold);
}

/* ---------- Output ---------- */

static void set_led(int on)
{
    if (led_ready) gpioWrite(LED_PIN, on ? 1 : 0);
}

static void save_results(double mem, double storage, double threshold, const char *status)
{
    FILE *log, *st;
    char stamp[32];
    time_t now = time(NULL);

    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));

    log = fopen(LOG_FILE, "a");
    if (log == NULL) {
        printf("  ERROR: cannot open %s (run from the repo root; data/ must exist)\n", LOG_FILE);
    } else {
        fseek(log, 0, SEEK_END);
        if (ftell(log) == 0)   /* new file -> write header first */
            fprintf(log, "timestamp,memory_pct,storage_pct,threshold_pct,status\n");
        fprintf(log, "%s,%.1f,%.1f,%.1f,%s\n", stamp, mem, storage, threshold, status);
        fclose(log);
    }

    st = fopen(STATUS_FILE, "w");   /* status.txt always holds the latest status */
    if (st != NULL) {
        fprintf(st, "%s\n", status);
        fclose(st);
    }
}

/* Evaluate, display, set LED, save. Used by both live and manual modes. */
static void report(double mem, double storage, double threshold)
{
    int warn = is_warning(mem, storage, threshold);
    const char *status = warn ? "WARNING" : "HEALTHY";

    printf("\n  Memory usage : %5.1f %%\n", mem);
    printf("  Storage usage: %5.1f %%\n", storage);
    printf("  Threshold    : %5.1f %%\n", threshold);
    printf("  System status: %s\n", status);

    set_led(warn);
    printf("  Warning LED  : %s\n", warn ? "ON" : "OFF");
    save_results(mem, storage, threshold, status);
}

/* ---------- Input ---------- */

/* Reads a number 0-100. Returns -1 on end of input. */
static double read_percent(const char *prompt)
{
    char buf[64], *end;
    double v;

    for (;;) {
        printf("%s", prompt);
        if (fgets(buf, sizeof buf, stdin) == NULL) return -1.0;
        v = strtod(buf, &end);
        if (end != buf && v >= 0.0 && v <= 100.0) return v;
        printf("  Please enter a number from 0 to 100.\n");
    }
}

static void print_menu(double threshold)
{
    printf("\n=== Pi Equipment Health Monitor (threshold %.1f%%) ===\n", threshold);
    printf(" 1) Check this Pi now (live readings)\n");
    printf(" 2) Enter memory/storage manually (for testing)\n");
    printf(" 3) Change warning threshold\n");
    printf(" 4) Quit\n");
    printf("Choice: ");
}

int main(void)
{
    double threshold = DEFAULT_THRESHOLD;
    double mem, storage, t;
    char buf[16];

    if (gpioInitialise() < 0) {
        printf("WARNING: GPIO unavailable (did you use sudo?). Continuing without LED.\n");
    } else {
        gpioSetMode(LED_PIN, PI_OUTPUT);
        led_ready = 1;
        set_led(0);
    }

    for (;;) {
        print_menu(threshold);
        if (fgets(buf, sizeof buf, stdin) == NULL) break;

        switch (atoi(buf)) {
        case 1:
            mem = get_memory_percent();
            storage = get_storage_percent();
            if (mem < 0 || storage < 0)
                printf("  ERROR: could not read system usage.\n");
            else
                report(mem, storage, threshold);
            break;
        case 2:
            mem = read_percent("  Memory usage % : ");
            if (mem < 0) goto done;
            storage = read_percent("  Storage usage %: ");
            if (storage < 0) goto done;
            report(mem, storage, threshold);
            break;
        case 3:
            t = read_percent("  New threshold % : ");
            if (t < 0) goto done;
            threshold = t;
            printf("  Threshold set to %.1f%%\n", threshold);
            break;
        case 4:
            goto done;
        default:
            printf("  Invalid choice.\n");
        }
    }

done:
    set_led(0);
    if (led_ready) gpioTerminate();
    printf("Goodbye.\n");
    return 0;
}
