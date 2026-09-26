/* file minunit_example.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "../src/global.h"
#include "../src/mbpfan.h"
#include "../src/settings.h"
#include "minunit.h"

int tests_run = 0;

static void free_fans(t_fans* fans)
{
    while (fans != NULL) {
        t_fans* tmp = fans->next;
        if (fans->file != NULL) {
            fclose(fans->file);
        }
        free(fans->fan_manual_path);
        free(fans->fan_output_path);
        free(fans->label);
        free(fans);
        fans = tmp;
    }
}

static void free_sensors(t_sensors* sensors)
{
    while (sensors != NULL) {
        t_sensors* tmp = sensors->next;
        free(sensors->path);
        free(sensors);
        sensors = tmp;
    }
}

static const char *test_sensor_paths()
{
    t_sensors* sensors = retrieve_sensors();
    mu_assert("No sensors found", sensors != NULL);
    t_sensors* tmp = sensors;

    while(tmp != NULL) {
        mu_assert("Sensor does not have a valid path", tmp->path != NULL);

        if(tmp->path != NULL) {
            mu_assert("Sensor does not have valid temperature", tmp->temperature > 0);
        }

        tmp = tmp->next;
    }

    free_sensors(sensors);
    return 0;
}


static const char *test_fan_paths()
{
    t_fans* fans = retrieve_fans();
    mu_assert("No fans found", fans != NULL);
    t_fans* tmp = fans;
    int found_fan_path = 0;

    while(tmp != NULL) {
        if(tmp->fan_output_path != NULL) {
            found_fan_path++;
        }

        tmp = tmp->next;
    }

    mu_assert("No fans found", found_fan_path != 0);
    free_fans(fans);
    return 0;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(content, f);
        fclose(f);
    }
}

static int read_int(const char *path)
{
    int value = -1;
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &value) != 1) {
            value = -1;
        }
        fclose(f);
    }
    return value;
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftwbuf)
{
    return remove(path);
}

static const char *check_resolve_applesmc_fan_paths(const char *base)
{
    char fan_path[PATH_MAX];
    char pwm_path[PATH_MAX];
    char expected[PATH_MAX + 32];

    // legacy layout (< 7.3): attributes sit directly under the device path
    resolve_applesmc_fan_paths(base, fan_path, pwm_path);
    snprintf(expected, sizeof(expected), "%s/fan", base);
    mu_assert("Legacy fan path mismatch", strcmp(fan_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/pwm", base);
    mu_assert("Legacy pwm path mismatch", strcmp(pwm_path, expected) == 0);

    // legacy kernels still register hwmon/hwmonN, just without fan attributes
    char hwmon_dir[PATH_MAX];
    char hwmon_dev[PATH_MAX + 16];
    char fan_min[PATH_MAX + 32];
    snprintf(hwmon_dir, sizeof(hwmon_dir), "%s/hwmon", base);
    snprintf(hwmon_dev, sizeof(hwmon_dev), "%s/hwmon3", hwmon_dir);
    snprintf(fan_min, sizeof(fan_min), "%s/fan1_min", hwmon_dev);
    mu_assert("Could not create hwmon dir", mkdir(hwmon_dir, 0755) == 0);
    mu_assert("Could not create hwmon3 dir", mkdir(hwmon_dev, 0755) == 0);

    resolve_applesmc_fan_paths(base, fan_path, pwm_path);
    snprintf(expected, sizeof(expected), "%s/fan", base);
    mu_assert("Legacy fan path with empty hwmonN mismatch", strcmp(fan_path, expected) == 0);

    // modern layout (>= 7.3): attributes nested under hwmon/hwmonN
    write_file(fan_min, "1000");

    resolve_applesmc_fan_paths(base, fan_path, pwm_path);
    snprintf(expected, sizeof(expected), "%s/fan", hwmon_dev);
    mu_assert("Modern fan path mismatch", strcmp(fan_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/pwm", hwmon_dev);
    mu_assert("Modern pwm path mismatch", strcmp(pwm_path, expected) == 0);

    return 0;
}

static const char *test_resolve_applesmc_fan_paths()
{
    char base[] = "/tmp/mbpfan_test_paths_XXXXXX";
    mu_assert("Could not create temp dir", mkdtemp(base) != NULL);

    const char *message = check_resolve_applesmc_fan_paths(base);

    nftw(base, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
    return message;
}

// Builds a fake applesmc device with a single fan1 under base. Kernels >= 7.3
// put the fan attributes into hwmon/hwmon1 and rename output/manual to
// target/pwm_enable; older ones keep them on the device itself and leave
// hwmon/hwmon1 without any fan attributes
static void make_fake_applesmc(const char *base, bool modern)
{
    char dir[PATH_MAX];
    char path[PATH_MAX + 32];

    snprintf(dir, sizeof(dir), "%s/hwmon", base);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/hwmon/hwmon1", base);
    mkdir(dir, 0755);

    if (modern) {
        snprintf(path, sizeof(path), "%s/fan1_target", dir); write_file(path, "0");
        snprintf(path, sizeof(path), "%s/pwm1_enable", dir); write_file(path, "2");
    } else {
        snprintf(dir, sizeof(dir), "%s", base);
        snprintf(path, sizeof(path), "%s/fan1_output", dir); write_file(path, "0");
        snprintf(path, sizeof(path), "%s/fan1_manual", dir); write_file(path, "0");
    }

    snprintf(path, sizeof(path), "%s/fan1_min", dir); write_file(path, "1000");
    snprintf(path, sizeof(path), "%s/fan1_max", dir); write_file(path, "6000");
    snprintf(path, sizeof(path), "%s/fan1_label", dir); write_file(path, "Left side\n");
}

// Runs discovery and fan control against the fake device the same way
// mbpfan() does, and checks what actually lands in the attribute files
static const char *check_fake_applesmc(const char *base, bool modern, t_fans **fans_out)
{
    char dir[PATH_MAX];
    char expected[PATH_MAX + 32];

    if (modern) {
        snprintf(dir, sizeof(dir), "%s/hwmon/hwmon1", base);
    } else {
        snprintf(dir, sizeof(dir), "%s", base);
    }

    resolve_applesmc_fan_paths(base, applesmc_fan_path, applesmc_pwm_path);
    snprintf(expected, sizeof(expected), "%s/fan", dir);
    mu_assert("Fan attributes resolved to the wrong directory", strcmp(applesmc_fan_path, expected) == 0);

    t_fans *fans = retrieve_fans();
    *fans_out = fans;

    mu_assert("Expected exactly fan1 to be found", fans != NULL && fans->fan_id == 1 && fans->next == NULL);

    snprintf(expected, sizeof(expected), "%s/%s", dir, modern ? "fan1_target" : "fan1_output");
    mu_assert("Fan uses the wrong speed attribute", strcmp(fans->fan_output_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/%s", dir, modern ? "pwm1_enable" : "fan1_manual");
    mu_assert("Fan uses the wrong mode attribute", strcmp(fans->fan_manual_path, expected) == 0);

    mu_assert("fan1_min was not read", fans->fan_min_speed == 1000);
    mu_assert("fan1_max was not read", fans->fan_max_speed == 6000);
    mu_assert("fan1_label was not read", strcmp(fans->label, "Left side") == 0);

    // discovery must not create attributes of the other kernel layout
    snprintf(expected, sizeof(expected), "%s/%s", dir, modern ? "fan1_output" : "fan1_target");
    mu_assert("Discovery created an attribute of the other layout", access(expected, F_OK) != 0);
    snprintf(expected, sizeof(expected), "%s/%s", dir, modern ? "fan0_target" : "fan0_output");
    mu_assert("Discovery created an attribute for a missing fan", access(expected, F_OK) != 0);

    set_fans_man(fans);
    mu_assert("Manual mode should write 1", read_int(fans->fan_manual_path) == 1);

    set_fan_speed(fans, 2500);
    mu_assert("Fan speed was not written", read_int(fans->fan_output_path) == 2500);

    // firmware drops back to auto with a zero target across suspend/resume;
    // the next poll must restore both, even though the speed is unchanged,
    // so set_fan_speed() must not skip writing a speed it already set
    write_file(fans->fan_manual_path, modern ? "2" : "0");
    write_file(fans->fan_output_path, "0");
    set_fans_man(fans);
    set_fan_speed(fans, 2500);
    mu_assert("Manual mode was not restored after resume", read_int(fans->fan_manual_path) == 1);
    mu_assert("Unchanged fan speed was not restored after resume", read_int(fans->fan_output_path) == 2500);

    set_fans_auto(fans);
    if (modern) {
        mu_assert("Auto mode should write 2 to pwmX_enable (0 is rejected)", read_int(fans->fan_manual_path) == 2);
    } else {
        mu_assert("Auto mode should write 0 to fanX_manual", read_int(fans->fan_manual_path) == 0);
    }

    return 0;
}

static const char *run_fake_applesmc(bool modern)
{
    char base[] = "/tmp/mbpfan_test_applesmc_XXXXXX";
    mu_assert("Could not create temp dir", mkdtemp(base) != NULL);

    make_fake_applesmc(base, modern);

    char saved_fan_path[PATH_MAX];
    char saved_pwm_path[PATH_MAX];
    memcpy(saved_fan_path, applesmc_fan_path, PATH_MAX);
    memcpy(saved_pwm_path, applesmc_pwm_path, PATH_MAX);

    t_fans *fans = NULL;
    const char *message = check_fake_applesmc(base, modern, &fans);

    memcpy(applesmc_fan_path, saved_fan_path, PATH_MAX);
    memcpy(applesmc_pwm_path, saved_pwm_path, PATH_MAX);
    free_fans(fans);
    nftw(base, remove_entry, 8, FTW_DEPTH | FTW_PHYS);

    return message;
}

static const char *test_fake_applesmc_legacy()
{
    return run_fake_applesmc(false);
}

static const char *test_fake_applesmc_modern()
{
    return run_fake_applesmc(true);
}

unsigned time_seed()
{
    time_t now = time ( 0 );
    unsigned char *p = (unsigned char *)&now;
    unsigned seed = 0;
    size_t i;

    for ( i = 0; i < sizeof now; i++ ) {
        seed = seed * ( UCHAR_MAX + 2U ) + p[i];
    }

    return seed;
}

// nothing better than a horrible piece of code to
// stress a little bit the CPU
int stress(int n)
{
    int f = n;

    while (f > 0) {
        while(n > 0) {
            srand ( time_seed() );
            n--;
        }

        f--;
        n = f;
    }
    return 0;
}

static const char *test_get_temp()
{
    t_sensors* sensors = retrieve_sensors();
    mu_assert("No sensors found", sensors != NULL);
    unsigned short temp_1 = get_temp(sensors);
    mu_assert("Invalid Global Temperature Found", temp_1 > 1 && temp_1 < 150);
    stress(2000);
    unsigned short temp_2 = get_temp(sensors);
    mu_assert("Invalid Higher temp test (if fan was already spinning high, this is not worrying)", temp_1 < temp_2);
    free_sensors(sensors);
    return 0;
}

static const char *test_config_file()
{
    FILE *f = NULL;
    Settings *settings = NULL;
    f = fopen("/etc/mbpfan.conf", "r");
    mu_assert("No config file found", f != NULL);

    if (f == NULL) {
        return 0;
    }

    settings = settings_open(f);
    fclose(f);
    mu_assert("Could not read settings from config file", settings != NULL);

    if (settings == NULL) {
        return 0;
    }

    mu_assert("Could not read polling_interval from config file", settings_get_int(settings, "general", "polling_interval") != 0);
    mu_assert("Could not read up_rate from config file", settings_get_int(settings, "general", "up_rate") != 0);
    mu_assert("Could not read down_rate from config file", settings_get_int(settings, "general", "down_rate") != 0);
    mu_assert("Could not read temp_alpha_percent from config file", settings_get_int(settings, "general", "temp_alpha_percent") != 0);
    mu_assert("Could not read hard_max_temp from config file", settings_get_int(settings, "general", "hard_max_temp") != 0);
    mu_assert("Could not read hard_max_hold from config file", settings_get_int(settings, "general", "hard_max_hold") != 0);

    char curve_str[512];
    mu_assert("Could not read curve from config file", settings_get(settings, "general", "curve", curve_str, sizeof(curve_str)));

    /* Destroy the settings object */
    settings_delete(settings);

    return 0;
}

static const char *test_settings()
{
    t_fans* fan = (t_fans *) malloc( sizeof( t_fans ) );
    fan->fan_id = 1;
    fan->fan_max_speed = -1; 
    fan->next = NULL;

    retrieve_settings("./mbpfan.conf.test1", fan);
    // choosing the maximum for iMac mid 2011
    mu_assert("max_fan_speed value is not 2600", fan->fan_max_speed == 2600);
    mu_assert("polling_interval is not 2", polling_interval == 2);
    mu_assert("up_rate is not 400", up_rate == 400);
    mu_assert("down_rate is not 700", down_rate == 700);
    mu_assert("temp_alpha_percent is not 15", temp_alpha_percent == 15);
    mu_assert("hard_max_temp is not 100", hard_max_temp == 100);
    mu_assert("hard_max_hold is not 2", hard_max_hold == 2);
    mu_assert("curve_count is not 3", curve_count == 3);
    mu_assert("curve[0] wrong", curve[0].temp_c == 60 && curve[0].percent == 30);
    mu_assert("curve[1] wrong", curve[1].temp_c == 90 && curve[1].percent == 80);
    mu_assert("curve[2] wrong", curve[2].temp_c == 100 && curve[2].percent == 100);

    fan->fan_min_speed = -1;
    retrieve_settings("./mbpfan.conf.test0", fan);
    mu_assert("min_fan_speed value is not 2000", fan->fan_min_speed == 2000);
    mu_assert("polling_interval is not 7", polling_interval == 7);
    
    t_fans* fan2 = (t_fans *)malloc(sizeof(t_fans));
    fan2->fan_id = 2;
    fan2->fan_max_speed = -1;
    fan2->next = NULL;
    fan->next = fan2;

    retrieve_settings("./mbpfan.conf.test2", fan);
    mu_assert("min_fan1_speed value is not 2000", fan->fan_min_speed == 2000);
    mu_assert("min_fan2_speed value is not 2000", fan->next->fan_min_speed == 2000);

    free(fan2);
    fan->next = NULL;
    free(fan);

    return 0;

}
int received = 0;

static void handler(int signal)
{
    t_fans* fan = (t_fans *) malloc( sizeof( t_fans ) );
    fan->fan_id = 1;
    fan->next = NULL;


    switch(signal) {
    case SIGHUP:
        received = 1;
        retrieve_settings("./mbpfan.conf.test1", fan);
	free(fan);
        break;

    default:
        received = 0;
	free(fan);
        break;
    }
}

static const char *test_sighup_receive()
{
    signal(SIGHUP, handler);
    raise(SIGHUP);
    mu_assert("did not receive SIGHUP signal", received == 1);
    return 0;
}

static const char *test_settings_reload()
{
    t_fans* fan = (t_fans *) malloc( sizeof( t_fans ) );
    fan->fan_id = 1;
    fan->fan_min_speed = -1;
    fan->fan_manual_path = NULL;
    fan->fan_output_path = NULL;
    fan->label = NULL;
    fan->file = NULL;
    fan->next = NULL;

    signal(SIGHUP, handler);
    retrieve_settings("./mbpfan.conf", fan);
    printf("Testing the _supplied_ mbpfan.conf (not the one you are using)..\n");
    // cannot tests min_fan_speed since it is not set and thus auto-detected
    mu_assert("polling_interval is not 1 before SIGHUP", polling_interval == 1);
    raise(SIGHUP);
    // cannot tests min_fan_speed since it is not set and thus auto-detected
    mu_assert("polling_interval is not 2 after SIGHUP", polling_interval == 2);
    retrieve_settings("./mbpfan.conf", fan);
    free_fans(fan);
    return 0;
}


static const char *test_curve_interpolate()
{
    t_curve_point saved[MAX_CURVE_POINTS];
    int saved_count = curve_count;
    memcpy(saved, curve, sizeof(t_curve_point) * curve_count);

    curve[0].temp_c = 50;
    curve[0].percent = 20;
    curve[1].temp_c = 100;
    curve[1].percent = 100;
    curve_count = 2;

    mu_assert("Below range should clamp to first point", curve_interpolate(30) == 20);
    mu_assert("Exact first point", curve_interpolate(50) == 20);
    mu_assert("Midpoint should interpolate", curve_interpolate(75) == 60);
    mu_assert("Exact last point", curve_interpolate(100) == 100);
    mu_assert("Above range should clamp to last point", curve_interpolate(150) == 100);

    memcpy(curve, saved, sizeof(t_curve_point) * saved_count);
    curve_count = saved_count;
    return 0;
}

static const char *test_parse_curve()
{
    t_curve_point saved[MAX_CURVE_POINTS];
    int saved_count = curve_count;
    memcpy(saved, curve, sizeof(t_curve_point) * curve_count);

    parse_curve("50:20,75:60,100:100");
    mu_assert("curve_count should be 3", curve_count == 3);
    mu_assert("First point wrong", curve[0].temp_c == 50 && curve[0].percent == 20);
    mu_assert("Second point wrong", curve[1].temp_c == 75 && curve[1].percent == 60);
    mu_assert("Third point wrong", curve[2].temp_c == 100 && curve[2].percent == 100);

    parse_curve("garbage");
    mu_assert("Invalid input should keep previous curve", curve_count == 3 && curve[0].temp_c == 50);

    parse_curve("50:20");
    mu_assert("Single point should be rejected, keeping previous curve", curve_count == 3);

    parse_curve("80:50,50:20");
    mu_assert("Unsorted points should be rejected, keeping previous curve", curve_count == 3);

    memcpy(curve, saved, sizeof(t_curve_point) * saved_count);
    curve_count = saved_count;
    return 0;
}

static const char *all_tests()
{
    mu_run_test(test_sensor_paths);
    mu_run_test(test_fan_paths);
    mu_run_test(test_resolve_applesmc_fan_paths);
    mu_run_test(test_fake_applesmc_legacy);
    mu_run_test(test_fake_applesmc_modern);
    mu_run_test(test_get_temp);
    mu_run_test(test_config_file);
    mu_run_test(test_settings);
    mu_run_test(test_sighup_receive);
    mu_run_test(test_settings_reload);
    mu_run_test(test_curve_interpolate);
    mu_run_test(test_parse_curve);
    return 0;
}

int tests(const char *program_path)
{
    verbose = 1;

    check_requirements(program_path);

    printf("Starting the tests..\n");
    printf("It is normal for them to take a bit to finish.\n");
    
    const char *result = all_tests();

    if (result != 0) {
        printf("Error: %s \n", result);

    } else {
        printf("ALL TESTS PASSED\n");
    }

    printf("Tests run: %d\n", tests_run);

    return result != 0;
}
