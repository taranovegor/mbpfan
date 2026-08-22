/**
 *  mbpfan.c - automatically control fan for MacBook Pro
 *  Copyright (C) 2010  Allan McRae <allan@archlinux.org>
 *  Modifications by Rafael Vega <rvega@elsoftwarehamuerto.org>
 *  Modifications (2012) by Ismail Khatib <ikhatib@gmail.com>
 *  Modifications (2012-present) by Daniel Graziotin <daniel@ineed.coffee> [CURRENT MAINTAINER]
 *  Modifications (2017-present) by Robert Musial <rmusial@fastmail.com>
 *  Modifications (2018-present) by Ati Sharma <ati.sharma@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *
 *  Notes:
 *    Assumes any number of processors, cores, sensors and fans
 *    (as defined in NUM_PROCESSORS, NUM_HWMONS, NUM_TEMP_INPUTS and NUM_FANS)
 *    It uses only the temperatures from the processors as input.
 *    Requires coretemp and applesmc kernel modules to be loaded.
 *    Requires root use
 *
 *  Tested models: see README.md
 */

#include <linux/limits.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <syslog.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/errno.h>
#include "daemon.h"
#include "mbpfan.h"
#include "global.h"
#include "settings.h"
#include "util.h"

/* lazy min/max... */
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define CORETEMP_PATH "/sys/devices/platform/coretemp.0"
#define APPLESMC_PATH "/sys/devices/platform/applesmc.768"
#define ALT_APPLESMC_PATH "/sys/bus/acpi/drivers/applesmc"

// default temp_c -> percent curve, linearly interpolated and clamped at the ends
t_curve_point curve[MAX_CURVE_POINTS] = {
    {55, 23}, {72, 24}, {80, 31}, {85, 51}, {90, 65}, {95, 83}, {100, 100},
};
int curve_count = 7;

int up_rate = 350;   // rpm/s ramping up
int down_rate = 600; // rpm/s ramping down
int temp_alpha_percent = 10;
int hard_max_temp = 103;
int hard_max_hold = 3;

// EMA-smoothed temperature and the emergency hold counter
static double ema_temp = -1;
static int hard_max_counter = 0;

// maximum number of processors etc supported
#define NUM_PROCESSORS 6
#define NUM_HWMONS 12
#define NUM_TEMP_INPUTS 64
#define NUM_FANS 10
// sane defaults when user provides unexpected values
#define MIN_FAN_SPEED_DEFAULT 500
#define MAX_FAN_SPEED_DEFAULT 6500

int polling_interval = 1;

t_sensors *sensors = NULL;
t_fans *fans = NULL;
char applesmc_path[PATH_MAX];
char applesmc_fan_path[PATH_MAX];


char *smprintf(const char *fmt, ...)
{
    char *buf;
    int cnt;
    va_list ap;

    // find buffer length
    va_start(ap, fmt);
    cnt = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (cnt < 0) {
        return NULL;
    }

    // create and write to buffer
    buf = malloc(cnt + 1);
    va_start(ap, fmt);
    vsnprintf(buf, cnt + 1, fmt, ap);
    va_end(ap);
    return buf;
}

bool is_modern_sensors_path()
{
    struct utsname kernel;
    uname(&kernel);

    char *str_kernel_version;
    str_kernel_version = strtok(kernel.release, ".");

    if (atoi(str_kernel_version) < 3) {
        mbp_log(LOG_ERR, "mbpfan detected a pre-3.x.x linux kernel. Detected version: %s. Exiting.\n", kernel.release);
        exit(EXIT_FAILURE);
    }

    int counter;

    for (counter = 0; counter < NUM_HWMONS; counter++) {
        int temp;
        for (temp = 1; temp < NUM_TEMP_INPUTS; ++temp) {
            char *path = smprintf("/sys/devices/platform/coretemp.0/hwmon/hwmon%d/temp%d_input", counter, temp);
            int res = access(path, R_OK);
            free(path);
            if (res == 0) {
                return 1;
            }
        }
    }

    return 0;
}

t_sensors *retrieve_sensors()
{
    t_sensors *sensors_head = NULL;
    t_sensors *s = NULL;

    char *path = NULL;
    char *path_begin = NULL;

    const char *path_end = "_input";
    int sensors_found = 0;

    if (!is_modern_sensors_path()) {
        if (verbose) {
            mbp_log(LOG_INFO, "Using legacy path for kernel < 3.15.0");
        }

        path_begin = strdup("/sys/devices/platform/coretemp.0/temp");

    } else {

        if (verbose) {
            mbp_log(LOG_INFO, "Using new sensor path for kernel >= 3.15.0 or some CentOS versions with kernel 3.10.0 ");
        }

        // loop over up to 6 processors
        int processor;
        for (processor = 0; processor < NUM_PROCESSORS; processor++) {

            if (path_begin != NULL) {
                free(path_begin);
            }
            path_begin = smprintf("/sys/devices/platform/coretemp.%d/hwmon/hwmon", processor);

            int counter;
            for (counter = 0; counter < NUM_HWMONS; counter++) {

                char *hwmon_path = smprintf("%s%d", path_begin, counter);

                int res = access(hwmon_path, R_OK);
                if (res == 0) {

                    free(path_begin);
                    path_begin = smprintf("%s/temp", hwmon_path);

                    if (verbose) {
                        mbp_log(LOG_INFO, "Found hwmon path at %s", path_begin);
                    }

                    free(hwmon_path);
                    break;
                }

                free(hwmon_path);
            }

            int core = 0;
            for (core = 0; core < NUM_TEMP_INPUTS; core++) {
                path = smprintf("%s%d%s", path_begin, core, path_end);

                FILE *file = fopen(path, "r");

                if (file != NULL) {
                    s = (t_sensors *)malloc(sizeof(t_sensors));
                    s->path = strdup(path);
                    fscanf(file, "%u", &s->temperature);

                    if (sensors_head == NULL) {
                        sensors_head = s;
                        sensors_head->next = NULL;

                    } else {
                        t_sensors *tmp = sensors_head;

                        while (tmp->next != NULL) {
                            tmp = tmp->next;
                        }

                        tmp->next = s;
                        tmp->next->next = NULL;
                    }

                    s->file = file;
                    sensors_found++;
                }

                free(path);
                path = NULL;
            }
        }
    }

    if (verbose) {
        mbp_log(LOG_INFO, "Found %d sensors", sensors_found);
    }

    if (sensors_found == 0) {
        mbp_log(LOG_CRIT, "mbpfan could not detect any temp sensor. Please contact the developer.");
        exit(EXIT_FAILURE);
    }

    free(path_begin);
    path_begin = NULL;

    return sensors_head;
}

static int read_value(const char *path)
{
    int value = -1;
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        fscanf(file, "%d", &value);
        fclose(file);
    }
    return value;
}

static void read_value_str(const char *path, char *str, size_t len)
{
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        fgets(str, len, file);
        fclose(file);
    }
}

static void trim_trailing_whitespace(char *str)
{
    for (ssize_t i = strlen(str) - 1; i >= 0; --i) {
        if (isspace(str[i]) || str[i] == '\n') {
            str[i] = '\0';
        }
    }
}

t_fans *retrieve_fans()
{
    t_fans *fans_head = NULL;
    t_fans *fan = NULL;

    char *path_output = NULL;
    char *path_label = NULL;
    char *path_manual = NULL;
    char *path_fan_max = NULL;
    char *path_fan_min = NULL;

    const char *path_begin = (const char *) &applesmc_fan_path;
    const char *path_output_end = "_output";
    const char *path_label_end = "_label";
    const char *path_man_end = "_manual";
    const char *path_max_speed = "_max";
    const char *path_min_speed = "_min";

    int counter = 0;
    int fans_found = 0;

    for (counter = 0; counter < NUM_FANS; counter++) {

        path_output = smprintf("%s%d%s", path_begin, counter, path_output_end);
        path_label = smprintf("%s%d%s", path_begin, counter, path_label_end);
        path_manual = smprintf("%s%d%s", path_begin, counter, path_man_end);
        path_fan_min = smprintf("%s%d%s", path_begin, counter, path_min_speed);
        path_fan_max = smprintf("%s%d%s", path_begin, counter, path_max_speed);

        FILE *file = fopen(path_output, "w");

        if (file != NULL) {
            fan = (t_fans *)malloc(sizeof(t_fans));
            fan->fan_output_path = strdup(path_output);
            fan->fan_manual_path = strdup(path_manual);
            fan->fan_id = counter;

            int fan_speed = read_value(path_fan_min);
            if (fan_speed == -1 || fan_speed < MIN_FAN_SPEED_DEFAULT)
                fan->fan_min_speed = MIN_FAN_SPEED_DEFAULT;
            else
                fan->fan_min_speed = fan_speed;

            fan_speed = read_value(path_fan_max);
            if (fan_speed == -1 || fan_speed > MAX_FAN_SPEED_DEFAULT)
                fan->fan_max_speed = MAX_FAN_SPEED_DEFAULT;
            else
                fan->fan_max_speed = fan_speed;

            size_t max_label_len = 64;
            fan->label = malloc(max_label_len);
            read_value_str(path_label, fan->label, max_label_len);
            trim_trailing_whitespace(fan->label);

            fan->old_speed = 0;

            if (fans_head == NULL) {
                fans_head = fan;
                fans_head->next = NULL;

            } else {
                t_fans *tmp = fans_head;

                while (tmp->next != NULL) {
                    tmp = tmp->next;
                }

                tmp->next = fan;
                tmp->next->next = NULL;
            }

            fan->file = file;
            fans_found++;
        }
        free(path_fan_min);
        path_fan_min = NULL;
        free(path_label);
        path_label = NULL;
        free(path_fan_max);
        path_fan_max = NULL;
        free(path_output);
        path_output = NULL;
        free(path_manual);
        path_manual = NULL;
    }

    if (verbose) {
        mbp_log(LOG_INFO, "Found %d fans", fans_found);
    }

    if (fans_found == 0) {
        mbp_log(LOG_CRIT, "mbpfan could not detect any fan. Please contact the developer.");
        exit(EXIT_FAILURE);
    }

    return fans_head;
}

static void set_fans_mode(t_fans *fans, int mode)
{
    t_fans *tmp = fans;
    FILE *file;

    while (tmp != NULL) {
        file = fopen(tmp->fan_manual_path, "rw+");

        if (file != NULL) {
            fprintf(file, "%d", mode);
            fclose(file);
        }

        tmp = tmp->next;
    }
}

void set_fans_man(t_fans *fans)
{

    set_fans_mode(fans, 1);
}

void set_fans_auto(t_fans *fans)
{

    set_fans_mode(fans, 0);
}

t_sensors *refresh_sensors(t_sensors *sensors)
{
    t_sensors *tmp = sensors;

    while (tmp != NULL) {
        if (tmp->file != NULL) {
            char buf[16];
            int len = pread(fileno(tmp->file), buf, sizeof(buf), /*offset=*/0);
            buf[len] = '\0';
            tmp->temperature = strtod(buf, NULL);
        }

        tmp = tmp->next;
    }

    return sensors;
}

/* Controls the speed of a fan */
void set_fan_speed(t_fans *fan, int speed)
{
    if (fan != NULL && fan->file != NULL && fan->old_speed != speed) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", speed);
        int res = pwrite(fileno(fan->file), buf, len, /*offset=*/0);
        if (res == -1) {
            perror("Could not set fan speed");
        }
        fan->old_speed = speed;
    }
}

void set_fan_minimum_speed(t_fans *fans)
{
    t_fans *tmp = fans;

    while (tmp != NULL) {
        set_fan_speed(tmp, tmp->fan_min_speed);
        tmp = tmp->next;
    }
}
unsigned short get_temp(t_sensors *sensors)
{
    sensors = refresh_sensors(sensors);
    unsigned int temp = 0;

    t_sensors *tmp = sensors;

    while (tmp != NULL) {
        temp = max(temp, tmp->temperature);
        tmp = tmp->next;
    }

    return temp / 1000;
}

// linearly interpolates the temp_c -> percent curve, clamped at the ends
int curve_interpolate(double temp)
{
    if (temp <= curve[0].temp_c) {
        return curve[0].percent;
    }

    if (temp >= curve[curve_count - 1].temp_c) {
        return curve[curve_count - 1].percent;
    }

    for (int i = 0; i < curve_count - 1; i++) {
        if (temp >= curve[i].temp_c && temp <= curve[i + 1].temp_c) {
            double frac = (temp - curve[i].temp_c) / (double)(curve[i + 1].temp_c - curve[i].temp_c);
            return (int) lround(curve[i].percent + frac * (curve[i + 1].percent - curve[i].percent));
        }
    }

    return curve[curve_count - 1].percent; /* unreachable */
}

// parses "temp:percent,temp:percent,..." into the curve array;
// keeps the existing curve untouched on any parse error
void parse_curve(const char *str)
{
    t_curve_point parsed[MAX_CURVE_POINTS];
    int count = 0;

    char *buf = strdup(str);
    char *saveptr_pair = NULL;
    char *pair = strtok_r(buf, ",", &saveptr_pair);

    while (pair != NULL) {
        if (count >= MAX_CURVE_POINTS) {
            mbp_log(LOG_ERR, "curve has more than %d points, ignoring the rest", MAX_CURVE_POINTS);
            break;
        }

        int temp_c, percent;
        if (sscanf(pair, "%d:%d", &temp_c, &percent) != 2) {
            mbp_log(LOG_ERR, "Invalid curve point '%s', keeping previous curve", pair);
            free(buf);
            return;
        }

        parsed[count].temp_c = temp_c;
        parsed[count].percent = percent;
        count++;

        pair = strtok_r(NULL, ",", &saveptr_pair);
    }

    free(buf);

    if (count < 2) {
        mbp_log(LOG_ERR, "curve needs at least 2 points, keeping previous curve");
        return;
    }

    for (int i = 1; i < count; i++) {
        if (parsed[i].temp_c <= parsed[i - 1].temp_c) {
            mbp_log(LOG_ERR, "curve points must be sorted by strictly increasing temp_c, keeping previous curve");
            return;
        }
    }

    memcpy(curve, parsed, sizeof(t_curve_point) * count);
    curve_count = count;
}

void retrieve_settings(const char *settings_path, t_fans *fans)
{
    Settings *settings = NULL;
    int result = 0;
    FILE *f = NULL;

    if (settings_path == NULL) {
        f = fopen("/etc/mbpfan.conf", "r");

    } else {
        f = fopen(settings_path, "r");
    }

    if (f == NULL) {
        /* Could not open configfile */
        if (verbose) {
            mbp_log(LOG_INFO, "Couldn't open configfile, using defaults");
        }

    } else {
        settings = settings_open(f);
        fclose(f);

        if (settings == NULL) {
            /* Could not read configfile */
            if (verbose) {
                mbp_log(LOG_WARNING, "Couldn't read configfile");
            }

        } else {

            t_fans *fan = fans;

            while (fan != NULL) {

                char *config_key;
                config_key = smprintf("min_fan%d_speed", fan->fan_id);
                /* Read configfile values */
                result = settings_get_int(settings, "general", config_key);
                if (result != 0) {
                    fan->fan_min_speed = result;
                }
                free(config_key);

                config_key = smprintf("max_fan%d_speed", fan->fan_id);
                result = settings_get_int(settings, "general", config_key);

                if (result != 0) {
                    fan->fan_max_speed = result;
                }
                free(config_key);
                fan = fan->next;
            }
            result = settings_get_int(settings, "general", "polling_interval");

            if (result != 0) {
                polling_interval = result;
            }

            result = settings_get_int(settings, "general", "up_rate");

            if (result != 0) {
                up_rate = result;
            }

            result = settings_get_int(settings, "general", "down_rate");

            if (result != 0) {
                down_rate = result;
            }

            result = settings_get_int(settings, "general", "temp_alpha_percent");

            if (result != 0) {
                temp_alpha_percent = result;
            }

            result = settings_get_int(settings, "general", "hard_max_temp");

            if (result != 0) {
                hard_max_temp = result;
            }

            result = settings_get_int(settings, "general", "hard_max_hold");

            if (result != 0) {
                hard_max_hold = result;
            }

            char curve_str[512];
            if (settings_get(settings, "general", "curve", curve_str, sizeof(curve_str))) {
                parse_curve(curve_str);
            }

            /* Destroy the settings object */
            settings_delete(settings);
        }
    }
}

void check_requirements(const char *program_path)
{

    /**
     * Check for root
     */

    uid_t uid = getuid(), euid = geteuid();

    if (uid != 0 || euid != 0) {
        mbp_log(LOG_ERR, "%s needs root privileges. Please run %s as root. Exiting.", program_path, program_path);
        exit(EXIT_FAILURE);
    }

    /**
     * Check for coretemp and applesmc modules
     */
    DIR *dir = opendir(CORETEMP_PATH);

    if (dir == NULL) {
        mbp_log(LOG_ERR, "%s needs coretemp support. Please either load it or build it into the kernel. Exiting.", program_path);
        exit(EXIT_FAILURE);
    }

    closedir(dir);
    memset(&applesmc_path, 0, PATH_MAX);
    memset(&applesmc_fan_path, 0, PATH_MAX);

    dir = opendir(APPLESMC_PATH);

    if (dir != NULL) {
        strncpy((char *) &applesmc_path, APPLESMC_PATH, PATH_MAX);
        closedir(dir);
    } else {
        /**
        * Check for alternate ACPI device path for newer macbooks
        */
        dir = opendir(ALT_APPLESMC_PATH);
        if (dir != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strncmp("APP", (const char *) &ent->d_name, 3) == 0) {
                    snprintf((char *) &applesmc_path, PATH_MAX, "%s/%s", ALT_APPLESMC_PATH, (char *) &ent->d_name);
                    break;
                }
            }
            closedir(dir);
        }
    }

    if (strlen(applesmc_path) != 0) {
        strncpy((char *) &applesmc_fan_path, (char *) &applesmc_path, PATH_MAX);
        strcat((char *) &applesmc_fan_path, "/fan");
        if (verbose) mbp_log(LOG_INFO, "applesmc device path: %s", (char *) &applesmc_path);

    } else {
        mbp_log(LOG_ERR, "%s needs applesmc support. Please either load it or build it into the kernel. Exiting.", program_path);
        exit(EXIT_FAILURE);
    }
}

int get_max_mhz(void)
{
    int max_mhz = -1;
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (dir == NULL) {
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "cpu", 3) != 0 || strcmp(ent->d_name, "cpufreq") == 0 || strcmp(ent->d_name, "cpuidle") == 0) {
            continue;
        }
        char *path = smprintf("/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq", ent->d_name);
        max_mhz = max(max_mhz, read_value(path) / 1000);
        free(path);
    }
    closedir(dir);
    return max_mhz;
}

void mbpfan()
{
    sensors = retrieve_sensors();
    fans = retrieve_fans();

    retrieve_settings(NULL, fans);

    t_fans *fan = fans;
    while (fan != NULL) {

        if (fan->fan_min_speed > fan->fan_max_speed) {
            mbp_log(LOG_ERR, "Invalid fan speeds: %d %d", fan->fan_min_speed, fan->fan_max_speed);
            exit(EXIT_FAILURE);
        }
        fan = fan->next;
    }

    if (curve_count < 2) {
        mbp_log(LOG_ERR, "curve needs at least 2 points");
        exit(EXIT_FAILURE);
    }

    set_fans_man(fans);
    set_fan_minimum_speed(fans);

    while (1) {
        if (do_exit) {
            syslog(LOG_WARNING, "Received SIGTERM, SIGQUIT, or SIGINT signal.");
            cleanup_and_exit(EXIT_SUCCESS);
        }

        if (do_reload) {
            syslog(LOG_WARNING, "Received SIGHUP signal.");
            retrieve_settings(NULL, fans);
            do_reload = 0;
        }

        int new_temp = get_temp(sensors);

        if (ema_temp < 0) {
            ema_temp = new_temp;
        } else {
            ema_temp += (temp_alpha_percent / 100.0) * (new_temp - ema_temp);
        }

        // emergency slam reacts to the raw reading, independent of the
        // EMA - a real runaway shouldn't wait for the smoothing to catch up
        if (new_temp >= hard_max_temp) {
            hard_max_counter++;
        } else {
            hard_max_counter = 0;
        }

        bool emergency = hard_max_counter >= hard_max_hold;
        int target_percent = emergency ? 100 : curve_interpolate(ema_temp);

        // re-assert manual mode every poll - firmware can silently drop
        // back to auto mode across suspend/resume
        set_fans_man(fans);

        fan = fans;
        while (fan != NULL) {
            int target_rpm = fan->fan_min_speed +
                              (int) lround(target_percent / 100.0 * (fan->fan_max_speed - fan->fan_min_speed));
            int current = fan->old_speed > 0 ? fan->old_speed : fan->fan_min_speed;
            int new_speed;

            if (emergency) {
                new_speed = target_rpm; // bypass slew for a real runaway
            } else if (target_rpm > current) {
                new_speed = min(target_rpm, current + up_rate * polling_interval);
            } else if (target_rpm < current) {
                new_speed = max(target_rpm, current - down_rate * polling_interval);
            } else {
                new_speed = current;
            }

            if (verbose) {
                mbp_log(LOG_INFO, "New Temp: %d EMA: %.1f Fan: %s Target: %d%% Speed: %d Max MHz: %d%s",
                        new_temp, ema_temp, fan->label, target_percent, new_speed, get_max_mhz(),
                        emergency ? " EMERGENCY" : "");
            }

            set_fan_speed(fan, new_speed);
            fan = fan->next;
        }

        if (verbose) {
            mbp_log(LOG_INFO, "Sleeping for %d seconds", polling_interval);
        }

        // call nanosleep instead of sleep to avoid rt_sigprocmask and
        // rt_sigaction
        struct timespec ts;
        ts.tv_sec = polling_interval;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
    }
}
