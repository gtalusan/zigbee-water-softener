#pragma once

#include <stdint.h>

void sensors_init(void);
float measure_distance(void);
uint8_t measure_battery(uint8_t *voltage_zb_out, uint16_t *mv_out, uint16_t *adc_out);
