#pragma once

#include <stdint.h>

void sensors_init(void);
float measure_distance(void);
uint8_t measure_battery(uint8_t *voltage_out);
