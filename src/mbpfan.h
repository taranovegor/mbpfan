/**
 *  Copyright (C) 2010  Allan McRae <allan@archlinux.org>
 *  Modifications (2012-present) by Daniel Graziotin <daniel@ineed.coffee>
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
 */

#ifndef _MBPFAN_H_
#define _MBPFAN_H_

#include <limits.h>
#include <stdbool.h>

#include "global.h"

/** Temperature polling interval
 *  Default value was 10 (seconds)
 */
extern int polling_interval;

/**
 * Multi-point curve mapping temp_c to fan speed percent, linearly
 * interpolated and clamped at the ends
 */
#define MAX_CURVE_POINTS 16

typedef struct {
    int temp_c;
    int percent;
} t_curve_point;

extern t_curve_point curve[MAX_CURVE_POINTS];
extern int curve_count;

/**
 * Linearly interpolates the temp_c -> percent curve, clamped at the ends
 */
int curve_interpolate(double temp);

/**
 * Parses "temp:percent,temp:percent,..." into the curve array;
 * keeps the existing curve untouched on any parse error
 */
void parse_curve(const char *str);

/**
 * Max rpm change per second in each direction (asymmetric slew)
 */
extern int up_rate;
extern int down_rate;

/**
 * EMA weight of a new temperature reading, in percent (0-100)
 */
extern int temp_alpha_percent;

/**
 * Emergency threshold in degrees: after hard_max_hold consecutive
 * readings at or above hard_max_temp, the fan jumps straight to max speed
 */
extern int hard_max_temp;
extern int hard_max_hold;

char *smprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Resolve the fan/pwm sysfs attribute path prefixes for the applesmc device
 * at device_path, accounting for the kernel >= 7.3 hwmon/hwmonN nesting
 */
void resolve_applesmc_fan_paths(const char *device_path, char *fan_path_out, char *pwm_path_out);

extern char applesmc_fan_path[PATH_MAX];
extern char applesmc_pwm_path[PATH_MAX];

/**
 * Return true if the kernel is < 3.15.0
 */
bool is_legacy_sensors_path();

/**
 * Tries to use the settings located in
 * /etc/mbpfan.conf
 * If it fails, the default hardcoded settings are used
 */
void retrieve_settings(const char *settings_path, t_fans *fans);

/**
 * Detect the sensors in /sys/devices/platform/coretemp.0/temp
 * and /sys/devices/platform/coretemp.1/temp etc
 * Return a linked list of t_sensors (first temperature detected)
 */
t_sensors *retrieve_sensors();

/**
 * Given a linked list of t_sensors, refresh their detected
 * temperature
 */
t_sensors *refresh_sensors(t_sensors *sensors);

/**
 * Detect the fans in /sys/devices/platform/applesmc.768/
 * Associate each fan to a sensor
 */
t_fans *retrieve_fans();

/**
 * Given a list of sensors with associated fans
 * Set them to manual control
 */
void set_fans_man(t_fans *fans);

/**
 * Given a list of sensors with associated fans
 * Set them to automatic control
 */
void set_fans_auto(t_fans *fans);

/**
 * Given a sensors with associated fans
 * Change their speed
 */
void set_fan_speed(t_fans *fan, int speed);

/**
 * Given a list of fans set their minumum fan speed
 */
void set_fan_minimum_speed(t_fans *fans);
/**
 *  Return maximum CPU temp in degrees
 */
unsigned short get_temp(t_sensors *sensors);

/**
 * Check if user has proper access and that required
 * kernel modules are available
 */
void check_requirements(const char *program_path);

/**
 * Main Program
 */
void mbpfan();

#endif
