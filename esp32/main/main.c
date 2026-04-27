/*
 * Water Softener Salt Level Sensor — ESP32-C6 Sleepy End Device
 *
 * Wakes from deep sleep, reports hardcoded distance + battery via custom
 * Zigbee cluster 0xFC00, then goes back to sleep.  Sleep interval ramps
 * from 60 s (×4 initial) → 5 min → 12 h over a 12 h window.
 */

#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "esp_zigbee.h"
#include "custom_cluster.h"
#include "vl53l0x.h"
#include "sensors.h"

static const char *TAG = "WATER_SOFTENER";

/* ---------- hardware pin assignments ---------- */
#define PIN_VL53_XSHUT   2
#define PIN_BOOT_BUTTON  9
#define ZIGBEE_ENDPOINT  1

/* ---------- sleep ramp ---------- */
#define INITIAL_SLEEP_SEC   60ULL
#define INITIAL_SLEEP_COUNT 4
#define MIN_SLEEP_SEC       (5ULL  * 60ULL)
#define MAX_SLEEP_SEC       (12ULL * 60ULL * 60ULL)
#define RAMP_WINDOW_SEC     (12ULL * 60ULL * 60ULL)
#define SEC_TO_US(s)        ((s) * 1000000ULL)

/* ---------- Zigbee identity ---------- */
#define ESP_MANUFACTURER_NAME  "\x07""talusan"
#define ESP_MODEL_IDENTIFIER   "\x1E""talusan.softener.salt-level-v2"
#define ESP_ZB_STORAGE_PART    "zb_storage"

/* ---------- Zigbee channel mask (Z2M default ch 11) ---------- */
#define ZB_PRIMARY_CHANNEL_MASK    ((1U << 11))
#define ZB_SECONDARY_CHANNEL_MASK  (0x07FFF800U)

/* ---------- Zigbee device / platform config ---------- */
#define ESP_ZIGBEE_ZED_CONFIG()                          \
    {                                                    \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,   \
        .install_code_policy = false,                    \
        .zed_config = {                                  \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,      \
            .keep_alive = 4000,                          \
        },                                               \
    }

#define ESP_ZIGBEE_PLATFORM_CONFIG()                     \
    {                                                    \
        .storage_partition_name = ESP_ZB_STORAGE_PART,   \
        .radio_config = {                                \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,  \
        },                                               \
    }

#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                    \
        .device_config   = ESP_ZIGBEE_ZED_CONFIG(),      \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(), \
    }

/* ---------- RTC data (survives deep sleep) ---------- */
static RTC_DATA_ATTR uint8_t  _rtc_initial_sleeps;
static RTC_DATA_ATTR uint64_t _rtc_ramp_elapsed;
static RTC_DATA_ATTR uint64_t _rtc_last_sleep_sec = MIN_SLEEP_SEC;
static RTC_DATA_ATTR uint32_t _rtc_wake_count;
static RTC_DATA_ATTR uint32_t _rtc_last_runtime;
static RTC_DATA_ATTR uint32_t _rtc_vl53_error_count;
static RTC_DATA_ATTR uint8_t  _rtc_ctx_valid;     // 1 = RTC state is live (not cold boot)
static uint32_t                _wake_start_ms;      // captured in app_main(), used at sleep

static esp_timer_handle_t s_sleep_timer;  // one-shot timer for deferred sleep
static bool s_needs_interview;             // captured before steering

/* 5 s delay for radio TX — matching Arduino sketch behaviour */
static void wait_for_reports(void)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
}

/* ---------- sleep ramp ---------- */
static uint64_t next_sleep_sec(void)
{
    if (_rtc_initial_sleeps < INITIAL_SLEEP_COUNT) {
        return INITIAL_SLEEP_SEC;
    }
    if (_rtc_ramp_elapsed >= RAMP_WINDOW_SEC) {
        return MAX_SLEEP_SEC;
    }
    uint64_t span = MAX_SLEEP_SEC - MIN_SLEEP_SEC;
    return MIN_SLEEP_SEC + (_rtc_ramp_elapsed * span / RAMP_WINDOW_SEC);
}

static void enter_sleep(void)
{
    uint64_t sec = next_sleep_sec();
    _rtc_last_sleep_sec = sec;

    if (_rtc_initial_sleeps < INITIAL_SLEEP_COUNT) {
        _rtc_initial_sleeps++;
    } else if (_rtc_ramp_elapsed < RAMP_WINDOW_SEC) {
        _rtc_ramp_elapsed += sec;
        if (_rtc_ramp_elapsed > RAMP_WINDOW_SEC) {
            _rtc_ramp_elapsed = RAMP_WINDOW_SEC;
        }
    }

    _rtc_ctx_valid = 1;

    ESP_LOGI(TAG, "Deep sleep %llu s  (elapsed=%llu  wake=%"PRIu32")",
             sec, _rtc_ramp_elapsed, _rtc_wake_count);

    vl53l0x_deinit();
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));  /* let USB CDC drain */
    esp_sleep_enable_timer_wakeup(SEC_TO_US(sec));
    esp_deep_sleep_start();
}

static void sleep_timer_cb(void *arg)
{
    enter_sleep();
}

static void measure_report_and_sleep(void)
{
    if (s_needs_interview) {
        s_needs_interview = false;
        ESP_LOGI(TAG, "Factory new — staying awake 120 s for interview");

        if (s_sleep_timer) {
            esp_timer_stop(s_sleep_timer);
            esp_timer_delete(s_sleep_timer);
            s_sleep_timer = NULL;
        }
        const esp_timer_create_args_t timer_args = {
            .callback = sleep_timer_cb,
            .name     = "sleep_timer",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_sleep_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(s_sleep_timer, 120 * 1000000));
        return;
    }

    uint32_t start_ms   = esp_timer_get_time() / 1000;
    uint32_t wake_count = ++_rtc_wake_count;
    float    dist       = measure_distance();
    uint8_t  voltage_zb = 0;
    uint8_t  batt_pct   = measure_battery(&voltage_zb);

    ESP_LOGI(TAG, "dist=%.1f cm  batt=%u%%  wake=%"PRIu32"  rt=%"PRIu32" ms"
             "  prev_rt=%"PRIu32" ms",
             dist, batt_pct, wake_count,
             (uint32_t)(esp_timer_get_time() / 1000 - start_ms),
             _rtc_last_runtime);

    if (dist > 0.0f) {
        custom_cluster_report_distance(dist);
        _rtc_vl53_error_count = 0;
    } else {
        _rtc_vl53_error_count++;
    }
    custom_cluster_report_vl53_error_count(_rtc_vl53_error_count);
    custom_cluster_report_wake_count(wake_count);

    /* Report previous cycle's full runtime */
    if (_rtc_last_runtime > 0) {
        custom_cluster_report_runtime_ms(_rtc_last_runtime);
    }

    uint8_t batt_zb = batt_pct * 2;
    ezb_zcl_set_attr_value(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                           EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                           EZB_ZCL_STD_MANUF_CODE, &batt_zb, false);
    ezb_zcl_set_attr_value(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                           EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                           EZB_ZCL_STD_MANUF_CODE, &voltage_zb, false);
    {
        ezb_zcl_report_attr_cmd_t cmd = {
            .cmd_ctrl.dst_addr   = EZB_ADDRESS_SHORT(0x0000),
            .cmd_ctrl.dst_ep     = 1,
            .cmd_ctrl.src_ep     = ZIGBEE_ENDPOINT,
            .cmd_ctrl.cluster_id = EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            .cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .cmd_ctrl.fc.direction       = 1,
            .payload.attr_id = EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
        };
        ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&cmd);
        if (ret != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "battery pct report failed: %d", ret);
        }
    }
    {
        ezb_zcl_report_attr_cmd_t cmd = {
            .cmd_ctrl.dst_addr   = EZB_ADDRESS_SHORT(0x0000),
            .cmd_ctrl.dst_ep     = 1,
            .cmd_ctrl.src_ep     = ZIGBEE_ENDPOINT,
            .cmd_ctrl.cluster_id = EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            .cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .cmd_ctrl.fc.direction       = 1,
            .payload.attr_id = EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
        };
        ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&cmd);
        if (ret != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "battery mV report failed: %d", ret);
        }
    }

    wait_for_reports();

    /* Total wake-cycle runtime (app_main entry → now) for reporting next cycle */
    _rtc_last_runtime = (uint32_t)(esp_timer_get_time() / 1000 - _wake_start_ms);

    enter_sleep();
}

/* shared by both debug and production paths */
static void rejoin_measure_cb(void *arg)
{
    measure_report_and_sleep();
}

/* ---------- BDB commissioning signal handler ---------- */
static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack init");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status =
            *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Device %s factory-new", ezb_bdb_is_factory_new() ? "IS" : "is NOT");
            if (ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Starting network steering...");
                s_needs_interview = true;
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Rejoined existing network");
                /* Defer measurement — never send data inside the signal handler.
                   SDK examples all defer to timers/callbacks after rejoin. */
                if (s_sleep_timer) {
                    esp_timer_stop(s_sleep_timer);
                    esp_timer_delete(s_sleep_timer);
                    s_sleep_timer = NULL;
                }
                const esp_timer_create_args_t timer_args = {
                    .callback = rejoin_measure_cb,
                    .name     = "rejoin_timer",
                };
                ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_sleep_timer));
                ESP_ERROR_CHECK(esp_timer_start_once(s_sleep_timer, 5 * 1000000));
            }
        } else {
            ESP_LOGW(TAG, "%s failed (0x%02x), retrying init",
                     ezb_app_signal_to_string(type), status);
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        }
        break;
    }

    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status =
            *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t ext_pan;
            ezb_nwk_get_extended_panid(&ext_pan);
            ESP_LOGI(TAG, "Joined: PAN=0x%04hx EXT=0x%llx CH=%d SA=0x%04hx",
                     ezb_nwk_get_panid(), ext_pan.u64,
                     ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            measure_report_and_sleep();
        } else {
            ESP_LOGW(TAG, "Steering failed (0x%02x), retrying", status);
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }
        break;
    }

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        ESP_LOGI(TAG, "Permit-join: %d s",
                 *(uint8_t *)ezb_app_signal_get_params(app_signal));
        break;

    default:
        ESP_LOGI(TAG, "Signal: %s (0x%02x)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

/* ---------- ZCL core action handler ---------- */
static void zcl_core_action_handler(ezb_zcl_core_action_callback_id_t cb_id, void *msg)
{
    if (cb_id == EZB_ZCL_CORE_DEFAULT_RSP_CB_ID) {
        ezb_zcl_cmd_default_rsp_message_t *rsp =
            (ezb_zcl_cmd_default_rsp_message_t *)msg;
        ESP_LOGD(TAG, "ZCL rsp: status=0x%02x", rsp->in.status_code);
    }
}

/* ---------- device creation ---------- */
static esp_err_t create_device(void)
{
    ezb_af_device_desc_t   dev_desc   = ezb_af_create_device_desc();
    ezb_zcl_cluster_desc_t basic_desc = NULL;
    ezb_zcl_cluster_desc_t identify_desc = NULL;

    /* Basic cluster */
    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version  = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    basic_desc = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc,
        EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ESP_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc,
        EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ESP_MODEL_IDENTIFIER);
    ESP_LOGI(TAG, "Model: %s", ESP_MODEL_IDENTIFIER + 1);

    /* Identify cluster */
    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    identify_desc = ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER);

    /* Power Config cluster (battery) */
    ezb_zcl_cluster_desc_t pwr_desc =
        ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    uint8_t batt_pct_init = 0xFF;
    ezb_zcl_power_config_cluster_desc_add_attr(pwr_desc,
        EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &batt_pct_init);
    {
        uint8_t mv_init = 0xFF;
        ezb_zcl_attr_desc_t mv_attr = ezb_zcl_create_attr_desc(
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
            EZB_ZCL_ATTR_TYPE_UINT8,
            EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
            EZB_ZCL_STD_MANUF_CODE, &mv_init);
        ezb_zcl_cluster_add_attr_desc(pwr_desc, mv_attr);
    }

    /* Endpoint */
    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = ZIGBEE_ENDPOINT,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = 0x000C,  /* Simple Sensor */
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, basic_desc));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, identify_desc));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, pwr_desc));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));

    /* custom cluster MUST be added BEFORE device registration */
    custom_cluster_init(ZIGBEE_ENDPOINT, ep_desc);

    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ezb_zcl_core_action_handler_register(zcl_core_action_handler);

    return ESP_OK;
}

/* ---------- Zigbee stack task ---------- */
static void zigbee_task(void *pv)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    /* Security + channel selection */
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ZB_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ZB_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));

    /* keep radio on — parent delivers directly, no buffering */
    ezb_nwk_set_rx_on_when_idle(true);

    ESP_ERROR_CHECK(create_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    ESP_LOGI(TAG, "Zigbee main loop running");
    esp_zigbee_launch_mainloop();

    ESP_LOGI(TAG, "Zigbee main loop exited");
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ---------- factory reset ---------- */
static void factory_reset(void)
{
    ESP_LOGW(TAG, "BOOT button held — FACTORY RESET");
    vl53l0x_deinit();
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    nvs_flash_erase_partition(ESP_ZB_STORAGE_PART);
    esp_restart();
}

/* ---------- main ---------- */
void app_main(void)
{
    _wake_start_ms = esp_timer_get_time() / 1000;

    /* NVS init */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition(ESP_ZB_STORAGE_PART));

    /* Reset reason — cold boot resets the sleep ramp */
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", reason);

    if (!_rtc_ctx_valid) {
        /* true cold boot or RTC data was garbage */
        _rtc_initial_sleeps = 0;
        _rtc_ramp_elapsed   = 0;
        _rtc_last_sleep_sec = MIN_SLEEP_SEC;
        _rtc_wake_count       = 0;
        _rtc_last_runtime     = 0;
        _rtc_vl53_error_count = 0;
        ESP_LOGI(TAG, "Cold boot — ramp reset");
    }

    /* Sensor (ADC) init */
    sensors_init();

    /* Check BOOT button for factory reset */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = BIT64(PIN_BOOT_BUTTON),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(PIN_BOOT_BUTTON) == 0) {
        factory_reset();
    }

    ESP_LOGI(TAG, "Starting Zigbee stack");
    xTaskCreate(zigbee_task, "zb_task", 4096, NULL, 5, NULL);
}
