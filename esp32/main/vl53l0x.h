#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Power-up, init I2C bus, probe and configure the VL53L0X.
 *  Leaves XSHUT HIGH (sensor active) and continuous mode running.
 *  Returns true on success. */
bool vl53l0x_init(void);

/** Power-down: stop ranging, XSHUT LOW, I2C pins to INPUT.
 *  Matches Arduino sketch — saves 8-12 uA quiescent current
 *  through the ToF sensor while the ESP32 is in deep sleep. */
void vl53l0x_deinit(void);

/** Read distance in cm.  Power-cycles and re-inits the sensor
 *  fresh each call (matching Arduino sketch).  Returns 0.0 on failure. */
float vl53l0x_measure(void);

#ifdef __cplusplus
}
#endif
