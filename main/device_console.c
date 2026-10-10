#include "device_console.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "board_config.h"
#include "board.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "epaper.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "settings.h"

#define CONSOLE_LINE_SIZE 384
#define CONSOLE_MAX_ARGS  8

static const char *TAG = "console";
static volatile bool s_provision_requested;

bool device_console_take_provision_request(void)
{
    if (!s_provision_requested) return false;
    s_provision_requested = false;
    return true;
}

static void console_write(const char *format, ...)
{
    char output[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(output, sizeof(output), format, arguments);
    va_end(arguments);
    if (length <= 0) return;
    const size_t count = (size_t)length < sizeof(output) ?
                         (size_t)length : sizeof(output) - 1U;
    (void)usb_serial_jtag_write_bytes(output, count, pdMS_TO_TICKS(100));
}

static void print_help(void)
{
    console_write(
        "\r\nAvailable commands:\r\n"
        "  help\r\n"
        "  show\r\n"
        "  provision\r\n"
        "  display test\r\n"
        "  i2c check\r\n"
        "  rtc show\r\n"
        "  rtc set <yyyy-mm-dd> <hh:mm:ss>\r\n"
        "  wifi set <ssid> [password]\r\n"
        "  wifi clear\r\n"
        "  mqtt set <mqtt://host:port> [username] [password]\r\n"
        "  mqtt clear\r\n"
        "  settings\r\n"
        "  settings t_sample <seconds>\r\n"
        "  settings update_freq <samples>\r\n"
        "  reboot\r\n"
        "Use quotes for values containing spaces and \\ for escaping.\r\n"
        "Input is not echoed, so passwords remain hidden.\r\n");
}

static bool parse_local_datetime(const char *date, const char *clock,
                                 time_t *utc)
{
    int year, month, day, hour, minute, second;
    char trailing;
    if (sscanf(date, "%d-%d-%d%c", &year, &month, &day, &trailing) != 3 ||
        sscanf(clock, "%d:%d:%d%c", &hour, &minute, &second, &trailing) != 3 ||
        year < 2024 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    struct tm local = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = -1,
    };
    const time_t parsed = mktime(&local);
    if (parsed == (time_t)-1) {
        return false;
    }
    struct tm verified;
    localtime_r(&parsed, &verified);
    if (verified.tm_year != year - 1900 ||
        verified.tm_mon != month - 1 || verified.tm_mday != day ||
        verified.tm_hour != hour || verified.tm_min != minute ||
        verified.tm_sec != second) {
        return false;
    }
    *utc = parsed;
    return true;
}

static void print_datetime(const char *prefix, time_t utc)
{
    struct tm local;
    char formatted[48];
    localtime_r(&utc, &local);
    if (strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %Z", &local)) {
        console_write("%s%s\r\n", prefix, formatted);
    }
}

static void show_rtc(void)
{
    time_t utc;
    if (board_rtc_get(&utc)) {
        print_datetime("RTC: ", utc);
        return;
    }
    time(&utc);
    struct tm local;
    localtime_r(&utc, &local);
    console_write("ERROR: RTC read failed.\r\n");
    if (local.tm_year + 1900 >= 2024) {
        print_datetime("System clock: ", utc);
    }
}

static void set_rtc(const char *date, const char *clock)
{
    time_t utc;
    if (!parse_local_datetime(date, clock, &utc)) {
        console_write("ERROR: invalid date/time; use rtc set yyyy-mm-dd hh:mm:ss.\r\n");
        return;
    }

    const struct timeval system_time = {.tv_sec = utc, .tv_usec = 0};
    if (settimeofday(&system_time, NULL) != 0) {
        console_write("ERROR: could not set the system clock.\r\n");
        return;
    }
    const esp_err_t result = board_rtc_set(utc);
    if (result != ESP_OK) {
        console_write("WARNING: system clock set, but RTC write failed: %s.\r\n",
                      esp_err_to_name(result));
        return;
    }
    time_t readback;
    if (!board_rtc_get(&readback)) {
        console_write("WARNING: RTC write completed, but readback failed.\r\n");
        return;
    }
    const double difference = difftime(readback, utc);
    if (difference < -1.0 || difference > 1.0) {
        console_write("WARNING: RTC readback differs from requested time.\r\n");
        print_datetime("RTC: ", readback);
        return;
    }
    print_datetime("OK - RTC and system clock set to ", readback);
}

static int split_arguments(char *line, char **arguments, int capacity)
{
    int count = 0;
    char *read = line;
    while (*read) {
        while (*read == ' ' || *read == '\t') ++read;
        if (!*read) break;
        if (count == capacity) return -1;

        const char quote = (*read == '\'' || *read == '"') ? *read++ : '\0';
        char *write = read;
        arguments[count++] = write;
        while (*read && (quote ? *read != quote : (*read != ' ' && *read != '\t'))) {
            if (*read == '\\') {
                ++read;
                if (!*read) return -1;
            }
            *write++ = *read++;
        }
        if (quote) {
            if (*read != quote) return -1;
            ++read;
            if (*read && *read != ' ' && *read != '\t') return -1;
        } else if (*read) {
            ++read;
        }
        *write = '\0';
        while (*read == ' ' || *read == '\t') ++read;
    }
    return count;
}

static void print_configuration(void)
{
    char ssid[33];
    char wifi_password[65];
    char uri[NETWORK_MQTT_URI_SIZE];
    char username[NETWORK_MQTT_USERNAME_SIZE];
    char mqtt_password[NETWORK_MQTT_PASSWORD_SIZE];
    const bool wifi = network_get_wifi_credentials(ssid, sizeof(ssid),
                                                    wifi_password, sizeof(wifi_password));
    const bool mqtt = network_get_mqtt_config(uri, sizeof(uri), username, sizeof(username),
                                              mqtt_password, sizeof(mqtt_password));
    console_write("\r\nWi-Fi: %s\r\n  SSID: %s\r\n  password: %s\r\n"
                  "MQTT: %s\r\n  URI: %s\r\n  username: %s\r\n  password: %s\r\n",
                  wifi ? "configured" : "not configured", wifi ? ssid : "-",
                  wifi && wifi_password[0] ? "********" : "(empty)",
                  mqtt ? "configured" : "not configured", mqtt ? uri : "-",
                  mqtt && username[0] ? username : "(empty)",
                  mqtt && mqtt_password[0] ? "********" : "(empty)");
    console_write("Board: %s\r\nDisplay: %s\r\n"
                  "Settings: t_sample=%lu s, update_freq=%lu samples\r\n",
                  BOARD_MODEL_NAME, BOARD_DISPLAY_NAME,
                  (unsigned long)settings_get_t_sample(),
                  (unsigned long)settings_get_update_freq());
}

static void report_result(esp_err_t result)
{
    if (result == ESP_OK) {
        console_write("OK - configuration saved; it will be used on the next cycle.\r\n");
    } else {
        console_write("ERROR: %s\r\n", esp_err_to_name(result));
    }
}

static void execute_line(char *line)
{
    char *arguments[CONSOLE_MAX_ARGS];
    const int count = split_arguments(line, arguments, CONSOLE_MAX_ARGS);
    if (count < 0) {
        console_write("ERROR: invalid syntax or quotes.\r\n");
    } else if (count == 0) {
        return;
    } else if ((!strcmp(arguments[0], "help") || !strcmp(arguments[0], "?")) &&
               count == 1) {
        print_help();
    } else if (!strcmp(arguments[0], "show") && count == 1) {
        print_configuration();
    } else if (!strcmp(arguments[0], "provision") && count == 1) {
        s_provision_requested = true;
        console_write("Provisioning requested; the QR code will appear after the current display cycle.\r\n");
    } else if (!strcmp(arguments[0], "rtc") && count == 2 &&
               !strcmp(arguments[1], "show")) {
        show_rtc();
    } else if (!strcmp(arguments[0], "rtc") && count == 4 &&
               !strcmp(arguments[1], "set")) {
        set_rtc(arguments[2], arguments[3]);
    } else if (!strcmp(arguments[0], "display") && count == 2 &&
               !strcmp(arguments[1], "test")) {
        console_write("Starting display color test; this can take up to a minute.\r\n");
        const esp_err_t result = epaper_show_test_pattern();
        if (result == ESP_OK) {
            console_write("OK - 4 native colors and 6 dithered blends sent.\r\n");
        } else {
            console_write("ERROR: %s; inspect the epaper log lines above.\r\n",
                          esp_err_to_name(result));
        }
    } else if (!strcmp(arguments[0], "i2c") && count == 2 &&
               !strcmp(arguments[1], "check")) {
        board_log_i2c_devices();
        console_write("I2C diagnostic written to the monitor log.\r\n");
    } else if (!strcmp(arguments[0], "wifi") && count == 2 &&
               !strcmp(arguments[1], "clear")) {
        report_result(network_clear_wifi_credentials());
    } else if (!strcmp(arguments[0], "wifi") && count >= 3 && count <= 4 &&
               !strcmp(arguments[1], "set")) {
        report_result(network_save_wifi_credentials(arguments[2],
                                                     count == 4 ? arguments[3] : ""));
    } else if (!strcmp(arguments[0], "mqtt") && count == 2 &&
               !strcmp(arguments[1], "clear")) {
        report_result(network_clear_mqtt_config());
    } else if (!strcmp(arguments[0], "mqtt") && count >= 3 && count <= 5 &&
               !strcmp(arguments[1], "set")) {
        report_result(network_save_mqtt_config(arguments[2],
                                                count >= 4 ? arguments[3] : "",
                                                count == 5 ? arguments[4] : ""));
    } else if (!strcmp(arguments[0], "settings") && count == 1) {
        console_write("t_sample=%lu s, update_freq=%lu samples\r\n",
                      (unsigned long)settings_get_t_sample(),
                      (unsigned long)settings_get_update_freq());
    } else if (!strcmp(arguments[0], "settings") && count == 3 &&
               (!strcmp(arguments[1], "t_sample") || !strcmp(arguments[1], "update_freq"))) {
        char command[64];
        snprintf(command, sizeof(command), "%s=%s", arguments[1], arguments[2]);
        report_result(settings_apply_command(command) ? ESP_OK : ESP_ERR_INVALID_ARG);
    } else if (!strcmp(arguments[0], "reboot") && count == 1) {
        console_write("Restarting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        console_write("ERROR: unknown command. Type 'help'.\r\n");
    }
}

static void console_task(void *argument)
{
    (void)argument;
    char line[CONSOLE_LINE_SIZE];
    size_t used = 0;
    bool was_connected = false;
    bool previous_was_cr = false;
    while (true) {
        const bool connected = usb_serial_jtag_is_connected();
        if (connected && !was_connected) {
            console_write("\r\nRHT console ready. Type 'help'.\r\nrht> ");
        }
        was_connected = connected;

        uint8_t input[32];
        const int received = usb_serial_jtag_read_bytes(input, sizeof(input),
                                                        pdMS_TO_TICKS(100));
        for (int i = 0; i < received; ++i) {
            const char value = (char)input[i];
            if (value == '\n' && previous_was_cr) {
                previous_was_cr = false;
                continue;
            }
            previous_was_cr = value == '\r';
            if (value == '\r' || value == '\n') {
                console_write("\r\n");
                if (used) {
                    line[used] = '\0';
                    execute_line(line);
                    used = 0;
                }
                console_write("rht> ");
            } else if (value == '\b' || value == 0x7f) {
                if (used) --used;
            } else if ((unsigned char)value >= 0x20 && used + 1U < sizeof(line)) {
                line[used++] = value;
            } else if ((unsigned char)value >= 0x20) {
                used = 0;
                console_write("\r\nERROR: line too long.\r\nrht> ");
            }
        }
    }
}

esp_err_t device_console_start(void)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 512,
    };
    esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(console_task, "usb_console", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB command console started");
    return ESP_OK;
}
