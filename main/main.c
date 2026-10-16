#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "device_console.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "nvs_flash.h"
#include "ota.h"
#include "provisioning.h"
#include "sdkconfig.h"
#include "stats.h"
#include "settings.h"

#define STATE_MAGIC 0x5248543aU
#define NTP_RETRY_SECONDS (15 * 60)
#define STARTUP_RETRY_SECONDS 60
#if BOARD_BOOT_CAN_WAKE
#define DEEP_SLEEP_WAKE_MASK ((1ULL << BOARD_BUTTON_POWER_GPIO) | \
                              (1ULL << BOARD_BUTTON_BOOT_GPIO))
#else
#define DEEP_SLEEP_WAKE_MASK (1ULL << BOARD_BUTTON_POWER_GPIO)
#endif
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_POLL_MS 20
#define PROVISION_HOLD_MS 5000
#define POWER_OTA_HOLD_MS 10000
#define USB_CONSOLE_WINDOW_SECONDS 30
#define USB_REFRESH_INTERVAL_SECONDS 60

typedef struct {
    uint32_t magic;
    stats_runtime_t stats;
    uint32_t samples_since_update;
    time_t last_ntp_sync;
    time_t last_ntp_attempt;
    time_t last_discovery;
    int8_t last_rssi;
    uint8_t page;
    bool last_wifi_connected;
    bool last_mqtt_connected;
    char discovery_version[OTA_VERSION_MAX_SIZE];
} persistent_state_t;

RTC_DATA_ATTR static persistent_state_t s_state;
static const char *TAG = "rht_sensor";
static volatile bool s_power_sleep_requested;
static volatile bool s_power_press_seen;
static volatile TickType_t s_power_pressed_since;
static volatile bool s_user_press_seen;
static volatile bool s_user_release_seen;
static volatile TickType_t s_user_pressed_since;
static volatile TickType_t s_user_released_at;

typedef struct {
    TaskHandle_t waiter;
    esp_err_t result;
} ota_task_context_t;

static void IRAM_ATTR power_button_isr(void *argument)
{
    (void)argument;
    s_power_pressed_since = xTaskGetTickCountFromISR();
    s_power_press_seen = true;
    s_power_sleep_requested = true;
}

static void IRAM_ATTR user_button_isr(void *argument)
{
    (void)argument;
    const TickType_t now = xTaskGetTickCountFromISR();
    if (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0) {
        s_user_pressed_since = now;
        s_user_press_seen = true;
        s_user_release_seen = false;
    } else if (s_user_press_seen) {
        s_user_released_at = now;
        s_user_release_seen = true;
    }
}

static esp_err_t init_power_button_interrupt(void)
{
    esp_err_t result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(BOARD_BUTTON_POWER_GPIO,
                                           GPIO_INTR_NEGEDGE),
                        TAG, "POWER interrupt type");
    return gpio_isr_handler_add(BOARD_BUTTON_POWER_GPIO, power_button_isr, NULL);
}

static esp_err_t init_user_button_interrupt(void)
{
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(BOARD_BUTTON_BOOT_GPIO,
                                           GPIO_INTR_ANYEDGE),
                        TAG, "USER interrupt type");
    return gpio_isr_handler_add(BOARD_BUTTON_BOOT_GPIO, user_button_isr, NULL);
}

static void ota_progress(int downloaded, int total)
{
    static int last_percent = -5;
    if (total <= 0) last_percent = -5;
    int percent = 0;
    if (total > 0 && downloaded > 0) {
        percent = (downloaded * 100) / total;
        if (percent > 100) percent = 100;
    }
    if (percent < 100 && percent - last_percent < 5) return;
    last_percent = percent;
    ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d)", percent, downloaded, total);
    (void)epaper_show_ota_progress((uint8_t)percent, "downloading");
}

static void ota_task(void *arg)
{
    ota_task_context_t *context = (ota_task_context_t *)arg;
    ESP_LOGI(TAG, "starting GitHub OTA update");
    ESP_LOGI(TAG, "OTA task stack free before download: %u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    ota_progress(0, 0);
    const esp_err_t ota_result = ota_install_from_github(ota_progress);
    context->result = ota_result;
    ESP_LOGI(TAG, "OTA task stack free after download: %u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    if (ota_result == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "GitHub OTA: no newer firmware available");
    } else if (ota_result != ESP_OK) {
        ESP_LOGE(TAG, "GitHub OTA failed: %s", esp_err_to_name(ota_result));
    }
    if (context->waiter) {
        xTaskNotifyGive(context->waiter);
    }
    vTaskDelete(NULL);
}

static esp_err_t run_ota_task(void)
{
    ota_task_context_t context = {
        .waiter = xTaskGetCurrentTaskHandle(),
        .result = ESP_FAIL,
    };
    TaskHandle_t ota_handle = NULL;
    if (xTaskCreate(ota_task, "ota_task", 24576, &context, 5,
                    &ota_handle) != pdPASS) {
        ESP_LOGE(TAG, "cannot start OTA task");
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return context.result;
}

static bool power_button_held_for_ota(void)
{
    if (gpio_get_level(BOARD_BUTTON_POWER_GPIO) != 0) {
        s_power_press_seen = false;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    if (gpio_get_level(BOARD_BUTTON_POWER_GPIO) != 0) {
        s_power_press_seen = false;
        return false;
    }

    const TickType_t pressed_since = s_power_press_seen ?
        s_power_pressed_since : xTaskGetTickCount();
    while (gpio_get_level(BOARD_BUTTON_POWER_GPIO) == 0) {
        if ((xTaskGetTickCount() - pressed_since) >=
            pdMS_TO_TICKS(POWER_OTA_HOLD_MS)) {
            s_power_press_seen = false;
            s_power_sleep_requested = false;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
    s_power_press_seen = false;
    return false;
}

static void run_power_ota(void)
{
    ESP_LOGI(TAG, "POWER held for ten seconds: starting forced OTA check");
    ESP_ERROR_CHECK_WITHOUT_ABORT(epaper_show_ota_progress(0, "ota richiesto"));

    int8_t rssi = -127;
    esp_err_t result = network_connect(&rssi);
    if (result == ESP_OK) {
        s_state.last_rssi = rssi;
        s_state.last_wifi_connected = true;
        result = run_ota_task();
    } else {
        s_state.last_wifi_connected = false;
    }
    network_disconnect();

    if (result == ESP_ERR_NOT_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(epaper_show_ota_result("gia aggiornato"));
    } else if (result != ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(epaper_show_ota_result(
            result == ESP_ERR_TIMEOUT ? "errore wifi" : "errore ota"));
    }
    epaper_shutdown();
}

static void provisioning_completed(void)
{
    s_state.last_discovery = 0;
    s_state.samples_since_update = UINT32_MAX;
    ESP_LOGI(TAG, "provisioning saved: MQTT update and Home Assistant discovery scheduled");
}

static float dew_point(float temperature, float humidity)
{
    const float a = 17.62f;
    const float b = 243.12f;
    humidity = fmaxf(0.1f, fminf(100.0f, humidity));
    const float gamma = logf(humidity / 100.0f) + a * temperature / (b + temperature);
    return b * gamma / (a - gamma);
}

static void init_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

static bool is_time_valid(time_t value)
{
    struct tm utc;
    gmtime_r(&value, &utc);
    return utc.tm_year + 1900 >= 2024;
}

static uint32_t seconds_until_next_sample(time_t now, uint32_t interval)
{
    if (!is_time_valid(now) || interval == 0) {
        return interval;
    }

    struct tm local;
    localtime_r(&now, &local);
    const uint32_t seconds_today = (uint32_t)local.tm_hour * 3600U +
                                   (uint32_t)local.tm_min * 60U +
                                   (uint32_t)local.tm_sec;
    const uint32_t next_seconds =
        (seconds_today / interval + 1U) * interval;
    if (next_seconds < 24U * 3600U) {
        local.tm_hour = (int)(next_seconds / 3600U);
        local.tm_min = (int)((next_seconds % 3600U) / 60U);
        local.tm_sec = (int)(next_seconds % 60U);
    } else {
        ++local.tm_mday;
        local.tm_hour = 0;
        local.tm_min = 0;
        local.tm_sec = 0;
    }
    local.tm_isdst = -1;

    const time_t next = mktime(&local);
    if (next <= now || (uint64_t)(next - now) > UINT32_MAX) {
        return interval;
    }
    return (uint32_t)(next - now);
}

static bool periodic_transmission_due(time_t now, uint32_t sample_interval,
                                      uint32_t update_multiplier)
{
    if (!is_time_valid(now) || sample_interval == 0 || update_multiplier == 0) {
        return false;
    }
    struct tm local;
    localtime_r(&now, &local);
    const uint32_t seconds_today = (uint32_t)local.tm_hour * 3600U +
                                   (uint32_t)local.tm_min * 60U +
                                   (uint32_t)local.tm_sec;
    const uint32_t sample_slot = seconds_today / sample_interval;
    return sample_slot % update_multiplier == 0;
}

static bool capture_startup_boot_press(TickType_t *pressed_since)
{
    const gpio_config_t boot_button = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&boot_button) != ESP_OK ||
        gpio_get_level(BOARD_BUTTON_BOOT_GPIO) != 0) {
        return false;
    }
    *pressed_since = xTaskGetTickCount();
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    return gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0;
}

static bool boot_button_held_for_provisioning(bool startup_press_detected,
                                               TickType_t pressed_since)
{
    if (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) != 0) {
        return false;
    }
    if (!startup_press_detected) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        if (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) != 0) {
            return false;
        }
        pressed_since = xTaskGetTickCount();
    }
    while (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0) {
        if ((xTaskGetTickCount() - pressed_since) >=
            pdMS_TO_TICKS(PROVISION_HOLD_MS)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
    return false;
}

typedef enum {
    USER_BUTTON_NONE,
    USER_BUTTON_PAGE,
    USER_BUTTON_PROVISION,
} user_button_action_t;

typedef enum {
    CYCLE_TRIGGER_SCHEDULED,
    CYCLE_TRIGGER_MANUAL,
    CYCLE_TRIGGER_USB,
} cycle_trigger_t;

static void clear_user_button_state(void)
{
    s_user_press_seen = false;
    s_user_release_seen = false;
}

static user_button_action_t take_user_button_action(void)
{
    if (!s_user_press_seen) return USER_BUTTON_NONE;

    const TickType_t pressed_since = s_user_pressed_since;
    if (!s_user_release_seen &&
        gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0) {
        while (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0) {
            if ((xTaskGetTickCount() - pressed_since) >=
                pdMS_TO_TICKS(PROVISION_HOLD_MS)) {
                clear_user_button_state();
                return USER_BUTTON_PROVISION;
            }
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
    }

    const TickType_t released_at = s_user_release_seen ?
        s_user_released_at : xTaskGetTickCount();
    clear_user_button_state();
    const TickType_t duration = released_at - pressed_since;
    if (duration < pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        return USER_BUTTON_NONE;
    }
    return duration >= pdMS_TO_TICKS(PROVISION_HOLD_MS) ?
        USER_BUTTON_PROVISION : USER_BUTTON_PAGE;
}

static user_button_action_t handle_user_button(void)
{
    const user_button_action_t action = take_user_button_action();
    if (action == USER_BUTTON_PAGE) {
        s_state.page = (s_state.page + 1U) % 2U;
        ESP_LOGI(TAG, "USER interaction: selected page %u; refreshing all data",
                 (unsigned)(s_state.page + 1U));
    } else if (action == USER_BUTTON_PROVISION) {
        ESP_LOGI(TAG, "USER held for five seconds: starting Wi-Fi provisioning");
        const esp_err_t result = provisioning_run();
        if (result == ESP_OK) {
            provisioning_completed();
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(result);
        }
        while (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
        clear_user_button_state();
    }
    return action;
}

static cycle_trigger_t wait_for_next_cycle(time_t now, TickType_t cycle_started_at)
{
    epaper_shutdown();
    network_disconnect();

    const uint32_t interval = settings_get_t_sample();
    uint32_t sleep_seconds = seconds_until_next_sample(now, interval);

    bool force_sleep = false;
    if (s_power_sleep_requested ||
        gpio_get_level(BOARD_BUTTON_POWER_GPIO) == 0) {
        s_power_sleep_requested = false;
        if (power_button_held_for_ota()) {
            run_power_ota();
            while (gpio_get_level(BOARD_BUTTON_POWER_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            }
            return CYCLE_TRIGGER_SCHEDULED;
        }
        force_sleep = true;
    }
    if (!force_sleep) {
        const user_button_action_t action = handle_user_button();
        if (action == USER_BUTTON_PAGE) return CYCLE_TRIGGER_MANUAL;
        if (action == USER_BUTTON_PROVISION) return CYCLE_TRIGGER_SCHEDULED;
    }
    if (!force_sleep && usb_serial_jtag_is_connected()) {
        ESP_LOGI(TAG, "USB host connected: staying awake; full refresh every %d seconds",
                 USB_REFRESH_INTERVAL_SECONDS);
        while (usb_serial_jtag_is_connected()) {
            if (device_console_take_provision_request()) {
                ESP_LOGI(TAG, "console request: starting Wi-Fi provisioning");
                const esp_err_t result = provisioning_run();
                if (result == ESP_OK) {
                    provisioning_completed();
                } else {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(result);
                }
                return CYCLE_TRIGGER_SCHEDULED;
            }
            if (s_power_sleep_requested ||
                gpio_get_level(BOARD_BUTTON_POWER_GPIO) == 0) {
                s_power_sleep_requested = false;
                if (power_button_held_for_ota()) {
                    run_power_ota();
                    while (gpio_get_level(BOARD_BUTTON_POWER_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                    }
                    return CYCLE_TRIGGER_SCHEDULED;
                }
                ESP_LOGI(TAG, "POWER pressed while USB is connected: entering deep sleep");
                while (gpio_get_level(BOARD_BUTTON_POWER_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                }
                force_sleep = true;
                break;
            }
            const user_button_action_t action = handle_user_button();
            if (action == USER_BUTTON_PAGE) return CYCLE_TRIGGER_MANUAL;
            if (action == USER_BUTTON_PROVISION) return CYCLE_TRIGGER_SCHEDULED;
            if (xTaskGetTickCount() - cycle_started_at >=
                pdMS_TO_TICKS(USB_REFRESH_INTERVAL_SECONDS * 1000U)) {
                ESP_LOGI(TAG, "USB refresh interval elapsed: updating readings, display and MQTT");
                return CYCLE_TRIGGER_USB;
            }
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
        if (!force_sleep) {
            ESP_LOGI(TAG, "USB host disconnected: entering deep sleep");
            time(&now);
            sleep_seconds = seconds_until_next_sample(now, interval);
        }
    }

    if (force_sleep) {
        ESP_LOGI(TAG, "POWER sleep request accepted");
    }

    board_prepare_for_sleep();
    for (int i = 0; i < 30; ++i) {
        if (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) && gpio_get_level(BOARD_BUTTON_POWER_GPIO)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "deep sleep for %lu seconds", (unsigned long)sleep_seconds);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL));
    /* Wake from the physical power key. The BOOT key remains available for
     * page selection and provisioning while the CPU is awake. */
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(DEEP_SLEEP_WAKE_MASK,
                                                    ESP_EXT1_WAKEUP_ANY_LOW));
    esp_deep_sleep_start();
    return CYCLE_TRIGGER_SCHEDULED;
}

static void enter_startup_failure_sleep(esp_err_t error)
{
    ESP_LOGE(TAG, "board initialization failed: %s", esp_err_to_name(error));
    if (!usb_serial_jtag_is_connected()) {
        ESP_LOGE(TAG, "retrying in %d seconds; POWER can wake the board sooner",
                 STARTUP_RETRY_SECONDS);
        vTaskDelay(pdMS_TO_TICKS(250));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_sleep_enable_timer_wakeup((uint64_t)STARTUP_RETRY_SECONDS * 1000000ULL));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_sleep_enable_ext1_wakeup_io(DEEP_SLEEP_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW));
        esp_deep_sleep_start();
    }
    ESP_LOGE(TAG, "USB host connected; staying awake for diagnostics");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STARTUP_RETRY_SECONDS * 1000));
    }
}

void app_main(void)
{
    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    cycle_trigger_t cycle_trigger =
        (wakeup_causes & (1U << ESP_SLEEP_WAKEUP_EXT1)) != 0 ?
        CYCLE_TRIGGER_MANUAL : CYCLE_TRIGGER_SCHEDULED;
#if BOARD_BOOT_CAN_WAKE
    const uint64_t gpio_wakeup_status = esp_sleep_get_ext1_wakeup_status();
    const bool boot_button_wakeup =
        (gpio_wakeup_status & (1ULL << BOARD_BUTTON_BOOT_GPIO)) != 0;
#else
    const bool boot_button_wakeup = false;
#endif
    ESP_LOGI(TAG, "reset reason=%d, wakeup causes=0x%08lx",
             (int)esp_reset_reason(), (unsigned long)wakeup_causes);
    ESP_LOGI(TAG, "board=%s ota_id=%s", BOARD_MODEL_NAME, BOARD_OTA_ID);
    TickType_t startup_boot_pressed_since = 0;
    const bool startup_boot_press_detected =
        capture_startup_boot_press(&startup_boot_pressed_since);
    if (s_state.magic != STATE_MAGIC) {
        memset(&s_state, 0, sizeof(s_state));
        s_state.magic = STATE_MAGIC;
        s_state.last_rssi = -127;
    }
    setenv("TZ", CONFIG_RHT_TIMEZONE, 1);
    tzset();

    init_nvs();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(stats_migrate_temperature_offset(600));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(device_console_start());
    esp_err_t board_result = board_init();
    if (board_result != ESP_OK) {
        enter_startup_failure_sleep(board_result);
        return;
    }
    const bool power_ota_requested = power_button_held_for_ota();
    ESP_ERROR_CHECK_WITHOUT_ABORT(init_power_button_interrupt());
    s_power_sleep_requested = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ota_confirm_running_image());
    if (power_ota_requested) {
        run_power_ota();
        while (gpio_get_level(BOARD_BUTTON_POWER_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
    } else if (boot_button_held_for_provisioning(startup_boot_press_detected,
                                          startup_boot_pressed_since)) {
        ESP_LOGI(TAG, "BOOT held during startup: starting Wi-Fi provisioning");
        const esp_err_t result = provisioning_run();
        if (result == ESP_OK) {
            provisioning_completed();
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(result);
        }
        while (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
    } else if (boot_button_wakeup) {
        s_state.page = (s_state.page + 1U) % 2U;
        ESP_LOGI(TAG, "BOOT wake: selected page %u and scheduled display refresh",
                 (unsigned)(s_state.page + 1U));
    }

    clear_user_button_state();
    ESP_ERROR_CHECK_WITHOUT_ABORT(init_user_button_interrupt());

    while (true) {
    const TickType_t cycle_started_at = xTaskGetTickCount();
    const bool transmit_for_manual_wakeup = cycle_trigger == CYCLE_TRIGGER_MANUAL;
    const bool transmit_for_usb_refresh = cycle_trigger == CYCLE_TRIGGER_USB;
    cycle_trigger = CYCLE_TRIGGER_SCHEDULED;
    time_t now = 0;
    bool rtc_valid = board_rtc_get(&now);
    bool time_valid = rtc_valid && is_time_valid(now);

    /* Read before enabling the radio to avoid Wi-Fi self-heating biasing SHTC3. */
    board_sample_t sample;
    if (board_read_environment(&sample) != ESP_OK) {
        ESP_LOGE(TAG, "environment measurement failed");
        float battery_voltage = 0;
        uint8_t battery_percent = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_read_battery(&battery_voltage, &battery_percent));
        const epaper_view_t unavailable_view = {
            .timestamp = now,
            .battery_v = battery_voltage,
            .battery_pct = battery_percent,
            .rssi = s_state.last_rssi,
            .time_valid = time_valid,
            .wifi_connected = s_state.last_wifi_connected,
            .mqtt_connected = network_is_configured() &&
                              s_state.last_wifi_connected &&
                              s_state.last_mqtt_connected,
            .page = s_state.page,
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(epaper_show(&unavailable_view));
        time(&now);
        cycle_trigger = wait_for_next_cycle(now, cycle_started_at);
        continue;
    }
    float battery_voltage = 0;
    uint8_t battery_percent = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_read_battery(&battery_voltage, &battery_percent));

    bool wifi_connected = false;
    bool wifi_attempted = false;
    const bool ntp_resync_required = !time_valid || !is_time_valid(s_state.last_ntp_sync) ||
                                     now < s_state.last_ntp_sync ||
                                     now - s_state.last_ntp_sync >= CONFIG_RHT_NTP_RESYNC_HOURS * 3600;
    const bool ntp_retry_ready = !time_valid || !is_time_valid(s_state.last_ntp_attempt) ||
                                 now < s_state.last_ntp_attempt ||
                                 now - s_state.last_ntp_attempt >= NTP_RETRY_SECONDS;
    const bool ntp_due = ntp_resync_required && ntp_retry_ready;

    if (ntp_due && network_has_wifi_credentials()) {
        wifi_attempted = true;
        if (time_valid) {
            s_state.last_ntp_attempt = now;
        }
        if (network_connect(&s_state.last_rssi) == ESP_OK) {
            wifi_connected = true;
            if (network_sync_time() == ESP_OK) {
                time(&now);
                time_valid = is_time_valid(now);
                if (time_valid) {
                    s_state.last_ntp_sync = now;
                    ESP_ERROR_CHECK_WITHOUT_ABORT(board_rtc_set(now));
                }
            }
        }
    }
    if (!time_valid) {
        time(&now);
        time_valid = is_time_valid(now);
    }

    if (time_valid) {
        ESP_ERROR_CHECK(stats_init(&s_state.stats, now));
        ESP_ERROR_CHECK_WITHOUT_ABORT(stats_add_sample_with_battery(
            &s_state.stats, now, sample.temperature_c, sample.humidity_pct,
            battery_voltage * 1000.0f));
    }
    if (!transmit_for_manual_wakeup && s_state.samples_since_update < UINT32_MAX) {
        ++s_state.samples_since_update;
    }
    float day_min = sample.temperature_c;
    float day_max = sample.temperature_c;
    if (time_valid) {
        stats_today_extremes(&s_state.stats, sample.temperature_c, &day_min, &day_max);
    }
    const float dew = dew_point(sample.temperature_c, sample.humidity_pct);

    const uint32_t configured_update_freq = settings_get_update_freq();
    const uint32_t update_multiplier = configured_update_freq <= 1 ?
        1U : configured_update_freq;
    const bool forced_update_pending = s_state.samples_since_update == UINT32_MAX;
    const bool periodic_due = forced_update_pending || (time_valid ?
        periodic_transmission_due(now, settings_get_t_sample(), update_multiplier) :
        s_state.samples_since_update >= update_multiplier);
    const bool should_transmit = network_is_configured() &&
                                 (transmit_for_manual_wakeup ||
                                  transmit_for_usb_refresh || periodic_due);

    if (transmit_for_manual_wakeup) {
        ESP_LOGI(TAG, "manual button wake: publishing the updated measurement");
    } else if (transmit_for_usb_refresh) {
        ESP_LOGI(TAG, "USB refresh: publishing the updated measurement");
    } else if (periodic_due) {
        ESP_LOGI(TAG, "periodic MQTT slot due from the 00:00 schedule");
    }

    if (should_transmit && !wifi_connected && !wifi_attempted) {
        wifi_attempted = true;
        wifi_connected = network_connect(&s_state.last_rssi) == ESP_OK;
    }
    if (wifi_attempted) {
        s_state.last_wifi_connected = wifi_connected;
    }
    if (should_transmit && wifi_connected) {
        network_measurement_t measurement = {
            .timestamp = now,
            .temperature_c = sample.temperature_c,
            .humidity_pct = sample.humidity_pct,
            .dew_point_c = dew,
            .day_min_c = day_min,
            .day_max_c = day_max,
            .battery_mv = (uint32_t)lroundf(battery_voltage * 1000.0f),
            .battery_pct = battery_percent,
            .rssi = s_state.last_rssi,
        };
        if (time_valid) {
            for (uint8_t page = 0; page < NETWORK_HISTORY_COUNT; ++page) {
                float history_delta = 0;
                const char *history_label = NULL;
                measurement.history_valid[page] =
                    stats_delta_reference(&s_state.stats, page, now,
                                          sample.temperature_c, &history_delta,
                                          &history_label);
                if (measurement.history_valid[page]) {
                    measurement.history_delta_c[page] = history_delta;
                    measurement.history_reference_c[page] =
                        sample.temperature_c - history_delta;
                }
            }
        }
        const esp_app_desc_t *app = esp_app_get_description();
        const bool discovery_due = !is_time_valid(s_state.last_discovery) ||
                                   now < s_state.last_discovery ||
                                   now - s_state.last_discovery >= 24 * 3600 ||
                                   strcmp(s_state.discovery_version, app->version) != 0;
        strlcpy(measurement.installed_version, app->version,
                sizeof(measurement.installed_version));
        strlcpy(measurement.latest_version, app->version,
                sizeof(measurement.latest_version));
        if (discovery_due) {
            (void)ota_get_latest_version(measurement.latest_version,
                                          sizeof(measurement.latest_version));
        }
        if (network_publish(&measurement, discovery_due) == ESP_OK) {
            s_state.last_mqtt_connected = true;
            if (!transmit_for_manual_wakeup || forced_update_pending) {
                s_state.samples_since_update = 0;
            }
            if (discovery_due) {
                s_state.last_discovery = now;
                strlcpy(s_state.discovery_version, app->version,
                        sizeof(s_state.discovery_version));
            }
        } else {
            s_state.last_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT publish failed; it will be retried on the next wake");
        }
        if (network_take_ota_request()) {
            /* Keep Wi-Fi alive until the OTA task has finished. The normal
             * end-of-cycle cleanup must not race the TLS download. */
            (void)run_ota_task();
        }
    }
    if (should_transmit && !wifi_connected) {
        s_state.last_mqtt_connected = false;
    }

    float delta = 0;
    const char *delta_label = "DELTA";
    const bool delta_valid = time_valid &&
        stats_delta_reference(&s_state.stats, 0, now,
                              sample.temperature_c, &delta, &delta_label);
    epaper_view_t view = {
        .timestamp = now,
        .temperature_c = sample.temperature_c,
        .humidity_pct = sample.humidity_pct,
        .dew_point_c = dew,
        .day_min_c = day_min,
        .day_max_c = day_max,
        .delta_c = delta,
        .battery_v = battery_voltage,
        .battery_pct = battery_percent,
        .rssi = s_state.last_rssi,
        .delta_label = delta_label,
        .delta_valid = delta_valid,
        .environment_valid = true,
        .time_valid = time_valid,
        .wifi_connected = s_state.last_wifi_connected,
        .mqtt_connected = network_is_configured() &&
                          s_state.last_wifi_connected &&
                          s_state.last_mqtt_connected,
        .page = s_state.page,
    };
    if (time_valid && s_state.page == 1) {
        for (uint8_t i = 0; i < 4; ++i) {
            const char *ignored_label = NULL;
            view.history_valid[i] = stats_delta_reference(&s_state.stats, i + 1U,
                now, sample.temperature_c, &view.history_delta_c[i], &ignored_label);
        }
        view.chart_count = stats_battery_chart(&s_state.stats, now,
                                               view.chart_values,
                                               EPAPER_CHART_MAX_POINTS,
                                               &view.chart_span_days);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(epaper_show(&view));

    time(&now);
    cycle_trigger = wait_for_next_cycle(now, cycle_started_at);
    }
}
