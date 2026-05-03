/*
 * Battery ADC measurement + distance wrapper delegating to VL53L0X.
 */

#include "sensors.h"
#include "vl53l0x.h"
#include "pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SENSORS";

#define ADC_ATTEN            ADC_ATTEN_DB_12
#define ADC_BITWIDTH         ADC_BITWIDTH_12
#define ADC_SAMPLES          5
#define ADC_VMAX_MV          3300   /* approximate full-scale mV at ADC_ATTEN_DB_12; only used when calibration unavailable */
#define BATT_DIVIDER_RATIO   2.0f   /* 1M + 1M divider */
#define BATT_MAX_MV          4200
#define BATT_MIN_MV          3300

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

void sensors_init(void)
{
    /* VL53L0X I2C init */
    vl53l0x_init();

    /* ADC for battery */
    ESP_ERROR_CHECK(adc_oneshot_new_unit(
        &(adc_oneshot_unit_init_cfg_t){.unit_id = ADC_UNIT_1}, &s_adc));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL,
        &(adc_oneshot_chan_cfg_t){.atten = ADC_ATTEN, .bitwidth = ADC_BITWIDTH}));

    /* ADC calibration */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = BATT_ADC_CHANNEL,
        .atten    = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable, using linear fallback");
        s_cali = NULL;
    }

    ESP_LOGI(TAG, "ADC init done (GPIO%d, atten=%d, bits=%d)",
             PIN_BATT_ADC, ADC_ATTEN, ADC_BITWIDTH);
}

float measure_distance(void)
{
    return vl53l0x_measure();
}

static float trimmed_average(const int *vals, int n)
{
    int min = vals[0], max = vals[0], sum = 0;
    for (int i = 0; i < n; i++) {
        sum += vals[i];
        if (vals[i] < min) min = vals[i];
        if (vals[i] > max) max = vals[i];
    }
    return (float)(sum - min - max) / (n - 2);
}

uint8_t measure_battery(uint8_t *voltage_zb_out, uint16_t *mv_out, uint16_t *adc_out)
{
    /* dummy read to flush any stale internal state */
    int dummy;
    adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &dummy);

    int raw_vals[ADC_SAMPLES];
    for (int i = 0; i < ADC_SAMPLES; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw_vals[i]));
    }

    /* Convert raw to mV using calibration (or linear fallback) */
    int mv_vals[ADC_SAMPLES];
    for (int i = 0; i < ADC_SAMPLES; i++) {
        if (s_cali) {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_cali, raw_vals[i], &mv_vals[i]));
        } else {
            mv_vals[i] = raw_vals[i] * ADC_VMAX_MV / ((1 << ADC_BITWIDTH) - 1);  /* rough fallback */
        }
    }

    float avg_mv = trimmed_average(mv_vals, ADC_SAMPLES);
    float vbat_mv = avg_mv * BATT_DIVIDER_RATIO;

    /* Clamp and compute percentage */
    float pct = (vbat_mv - BATT_MIN_MV) / (float)(BATT_MAX_MV - BATT_MIN_MV) * 100.0f;
    if (pct > 100.0f) pct = 100.0f;
    if (pct < 0.0f)   pct = 0.0f;

    if (voltage_zb_out)
        *voltage_zb_out = (uint8_t)(vbat_mv / 100.0f + 0.5f);
    if (mv_out)
        *mv_out = (uint16_t)(vbat_mv + 0.5f);
    if (adc_out)
        *adc_out = avg_mv;

    return (uint8_t)(pct + 0.5f);
}
