#pragma once

#define PIN_I2C_SDA         13
#define PIN_I2C_SCL         12
#define PIN_VL53_XSHUT      14
#define PIN_BOOT_BUTTON     9
#define PIN_BATT_ADC        1
#define PIN_UNUSED_8        8

/* On ESP32-H2 / ESP32-C6, GPIO 0–4 map directly to ADC_CHANNEL_0–4.
   For targets where the mapping differs, override BATT_ADC_CHANNEL here. */
#if CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C6
#define BATT_ADC_CHANNEL    ((adc_channel_t)(PIN_BATT_ADC))
#else
#define BATT_ADC_CHANNEL    ADC_CHANNEL_0
#endif
