/**
 * Water Softener Salt Level Detector
 *
 * Hardware:
 *   ESP32-C6 SuperMini (MakerGO)
 *   VL53L0X time-of-flight distance sensor (I2C)
 *   Li-Ion battery via 1:1 voltage divider on ADC pin
 *
 * Behaviour:
 *   - Wakes from deep sleep on a dynamic schedule that ramps from 5 minutes
 *     to 12 hours over the first 12 hours after a non-wake reboot
 *   - Measures distance to salt surface (cm) and battery level (%)
 *   - Reports both values via Zigbee as an end device
 *   - Rejoins existing network from NVS on every boot/wake
 *   - Hold BOOT button (GPIO9) at power-on to erase credentials and re-commission
 *
 * Deep sleep note: esp_deep_sleep_start() is a full reboot. setup() runs on
 * every wake, re-establishing the Zigbee connection before loop() is called.
 */

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode must be selected in Tools -> Zigbee mode"
#endif

#include "Zigbee.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include <Wire.h>
#include <VL53L0X.h>

// ---------------------------------------------------------------------------
// Configuration — adjust for hardware changes
// ---------------------------------------------------------------------------
#define PIN_SDA               13
#define PIN_SCL               12
#define PIN_VL53_XSHUT        14    // HIGH = sensor on, LOW = sensor off (saves power)
#define PIN_BATT_ADC          1    // ADC pin behind 1:1 voltage divider
#define PIN_CLEAR_CREDS       9    // BOOT button — hold at power-on to re-commission

#define INITIAL_SLEEP_SEC     60ULL                    // 1 minute — used for first N wakes
#define INITIAL_SLEEP_COUNT   4                        // # of short sleeps before ramp begins
#define MIN_SLEEP_SEC         (5ULL * 60ULL)           // 5 minutes — ramp start
#define MAX_SLEEP_SEC         (12ULL * 60ULL * 60ULL)  // 12 hours  — ramp end
#define RAMP_WINDOW_SEC       (12ULL * 60ULL * 60ULL)  // reach max after 12 hours of ramp time
#define SEC_TO_US(s)          ((s) * 1000000ULL)

#define BATT_DIVIDER_RATIO    2.0f   // Two equal resistors → multiply ADC reading by 2
#define BATT_MAX_MV           4200
#define BATT_MIN_MV           3300

#define SENSOR_READINGS       5     // Successful readings to collect; drop high+low, avg rest
#define MAX_SENSOR_ATTEMPTS   (SENSOR_READINGS * 5) // Max read attempts to gather SENSOR_READINGS valid samples
#define ZIGBEE_ENDPOINT       1
#define ZIGBEE_JOIN_TIMEOUT   30000 // ms — time allowed to connect before giving up
#define REPORT_TIMEOUT_MS     10000 // ms — time to wait for each attribute report ACK

//#define DEBUG
#undef DEBUG
#if defined(DEBUG)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINTLN
#define DEBUG_PRINTF
#endif

ZigbeeAnalog zbSensor = ZigbeeAnalog(ZIGBEE_ENDPOINT);
VL53L0X vl53;

volatile int8_t _pendingReports = 0;
RTC_DATA_ATTR uint8_t  _rtcInitialSleepsDone = 0;
RTC_DATA_ATTR uint64_t _rtcRampElapsedSec = 0;
RTC_DATA_ATTR uint64_t _rtcLastSleepSec = MIN_SLEEP_SEC;
RTC_DATA_ATTR uint8_t  _rtcLastBattPct = 0xFF;  // 0xFF = never reported

static uint64_t nextSleepIntervalUs()
{
  if (_rtcInitialSleepsDone < INITIAL_SLEEP_COUNT) {
    _rtcLastSleepSec = INITIAL_SLEEP_SEC;
    return SEC_TO_US(INITIAL_SLEEP_SEC);
  }

  if (_rtcRampElapsedSec >= RAMP_WINDOW_SEC) {
    _rtcLastSleepSec = MAX_SLEEP_SEC;
    return SEC_TO_US(MAX_SLEEP_SEC);
  }

  const uint64_t span = MAX_SLEEP_SEC - MIN_SLEEP_SEC;
  _rtcLastSleepSec = MIN_SLEEP_SEC + (_rtcRampElapsedSec * span / RAMP_WINDOW_SEC);
  if (_rtcLastSleepSec > MAX_SLEEP_SEC) _rtcLastSleepSec = MAX_SLEEP_SEC;
  return SEC_TO_US(_rtcLastSleepSec);
}

static void enterTimedDeepSleep()
{
  uint64_t sleepUs = nextSleepIntervalUs();

  if (_rtcInitialSleepsDone < INITIAL_SLEEP_COUNT) {
    _rtcInitialSleepsDone++;
  } else if (_rtcRampElapsedSec < RAMP_WINDOW_SEC) {
    _rtcRampElapsedSec += _rtcLastSleepSec;
    if (_rtcRampElapsedSec > RAMP_WINDOW_SEC) _rtcRampElapsedSec = RAMP_WINDOW_SEC;
  }

  DEBUG_PRINTF("[SLEEP] next=%llu sec elapsed=%llu sec\n", _rtcLastSleepSec, _rtcRampElapsedSec);
  esp_sleep_enable_timer_wakeup(sleepUs);
  esp_deep_sleep_start();
}

void onZigbeeResponse(zb_cmd_type_t cmd, esp_zb_zcl_status_t status, uint8_t ep, uint16_t cluster)
{
  if (cmd == ZB_CMD_REPORT_ATTRIBUTE && ep == ZIGBEE_ENDPOINT) {
    if (status == ESP_ZB_ZCL_STATUS_SUCCESS) {
      _pendingReports--;
    }
    else {
      DEBUG_PRINTF("[ZB] Report failed cluster=0x%04x status=%d\n", cluster, status);
    }
  }
}

static float trimmedAverageF(float *vals, int n)
{
  for (int i = 1; i < n; i++) {
    float key = vals[i];
    int j = i - 1;
    while (j >= 0 && vals[j] > key) {
      vals[j + 1] = vals[j];
      j--;
    }
    vals[j + 1] = key;
  }
  float sum = 0;
  for (int i = 1; i < n - 1; i++) {
    sum += vals[i];
  }
  return sum / (n - 2);
}

static uint8_t measureBattery(uint8_t *voltageOut)
{
  float vals[SENSOR_READINGS];

  // Let the ADC input settle after wake before taking the averaged readings.
  (void) analogReadMilliVolts(PIN_BATT_ADC);
  delay(10);

  for (int i = 0; i < SENSOR_READINGS; i++) {
    vals[i] = (float) analogReadMilliVolts(PIN_BATT_ADC);
    delay(10);
  }

  float avgMv = trimmedAverageF(vals, SENSOR_READINGS);
  float vbatMv = avgMv * BATT_DIVIDER_RATIO;
  float pct = (vbatMv - BATT_MIN_MV) / (float) (BATT_MAX_MV - BATT_MIN_MV) * 100.0f;
  pct = constrain(pct, 0.0f, 100.0f);

  *voltageOut = (uint8_t) (vbatMv / 100.0f + 0.5f);  // e.g. 42 = 4.2V

  DEBUG_PRINTF("Vadc=%.0fmV Vbat=%.0fmV pct=%.1f%% zb_voltage=%u\n", avgMv, vbatMv, pct, *voltageOut);
  return (uint8_t) pct;
}

static float measureDistance()
{
  pinMode(PIN_VL53_XSHUT, OUTPUT);
  digitalWrite(PIN_VL53_XSHUT, HIGH);
  delay(10);  // Tboot per datasheet

  Wire.begin(PIN_SDA, PIN_SCL);

  if (!vl53.init()) {
    DEBUG_PRINTLN("VL53 initialization failed. Reporting 0.");
    Wire.end();
    digitalWrite(PIN_VL53_XSHUT, LOW);
    return 0.0f;
  }
  vl53.setTimeout(500);
  vl53.startContinuous();
  delay(20);

  float validVals[SENSOR_READINGS];
  int validCount = 0;
  int attempts = 0;
  while (validCount < SENSOR_READINGS && attempts < MAX_SENSOR_ATTEMPTS) {
    float r = (float) vl53.readRangeContinuousMillimeters();
    attempts++;
    if (r < 65535.0f) {
      validVals[validCount++] = r;
    }
    delay(20);
  }

  if (validCount < SENSOR_READINGS) {
    DEBUG_PRINTLN("[VL53] Not enough valid samples after attempts — reporting 0");
    vl53.stopContinuous();
    Wire.end();
    digitalWrite(PIN_VL53_XSHUT, LOW);
    return 0.0f;
  }

  float avgMm = trimmedAverageF(validVals, validCount);

  vl53.stopContinuous();
  Wire.end();

  digitalWrite(PIN_VL53_XSHUT, LOW);

  float distCm = avgMm / 10.0f;
  DEBUG_PRINTF("[VL53] avg=%.1fmm -> %.1fcm\n", avgMm, distCm);
  return distCm;
}

static void waitForReport(const char *label) {
  unsigned long t0 = millis();
  while (_pendingReports > 0 && (millis() - t0) < REPORT_TIMEOUT_MS) {
    delay(50);
  }
  if (_pendingReports > 0) {
    DEBUG_PRINTF("[ZB] %s report timed out\n", label);
  }
}

void setup() {
#if defined(DEBUG)
  Serial.begin(921600);
  delay(200);
  DEBUG_PRINTLN("\nWater Softener Salt Level Distance Sensor is starting..");
#endif

  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
    _rtcInitialSleepsDone = 0;
    _rtcRampElapsedSec = 0;
    _rtcLastSleepSec = MIN_SLEEP_SEC;
    _rtcLastBattPct = 0xFF;
    DEBUG_PRINTLN("[SLEEP] Non-wake reboot detected. Ramp reset.");
  }

  analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db);
  delay(10);

  pinMode(PIN_VL53_XSHUT, OUTPUT);
  digitalWrite(PIN_VL53_XSHUT, LOW);

  // BOOT button held at power-on → erase Zigbee credentials
  pinMode(PIN_CLEAR_CREDS, INPUT_PULLUP);
  delay(50);
  bool clearCreds = (digitalRead(PIN_CLEAR_CREDS) == LOW);
  if (clearCreds) {
    DEBUG_PRINTLN("BOOT button held — credentials will be erased");
  }

  zbSensor.setManufacturerAndModel("talusan", "talusan.softener.salt-level");
  zbSensor.addAnalogInput();
  zbSensor.setAnalogInputDescription("Salt distance cm");
  zbSensor.setAnalogInputMinMax(0.0f, 200.0f);
  zbSensor.setAnalogInputResolution(0.1f);
  zbSensor.setPowerSource(ZB_POWER_SOURCE_BATTERY);

  Zigbee.onGlobalDefaultResponse(onZigbeeResponse);
  Zigbee.addEndpoint(&zbSensor);

  esp_zb_cfg_t zbCfg = ZIGBEE_DEFAULT_ED_CONFIG();
  zbCfg.nwk_cfg.zed_cfg.keep_alive = 45000000;  // 12.5 hours in ms — max sleep + margin
  Zigbee.setTimeout(ZIGBEE_JOIN_TIMEOUT);

  if (!Zigbee.begin(&zbCfg, false)) {
    DEBUG_PRINTLN("Failed to start! Rebooting..");
    delay(500);
    ESP.restart();
  }

  // Disable any NVS-stored auto-reporting config left over from a previous
  // converter that called configureReporting. Without this, the ZCL layer
  // fires an extra attribute report on every wake before we send ours.
  {
    esp_zb_zcl_attr_location_info_t loc = {};
    loc.endpoint_id  = ZIGBEE_ENDPOINT;
    loc.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    loc.manuf_code   = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    loc.cluster_id   = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT;
    loc.attr_id      = ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID;
    esp_zb_zcl_stop_attr_reporting(loc);
    loc.cluster_id   = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
    loc.attr_id      = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID;
    esp_zb_zcl_stop_attr_reporting(loc);
  }

  if (clearCreds) {
    DEBUG_PRINTLN("Erasing credentials. Commissioning required!");
    Zigbee.factoryReset(false);
  }

  DEBUG_PRINTLN("Connecting...");
  unsigned long tStart = millis();
  while (!Zigbee.connected()) {
    if (millis() - tStart > ZIGBEE_JOIN_TIMEOUT) {
      DEBUG_PRINTLN("Zigbee join timeout. Sleeping to retry next cycle...");
      enterTimedDeepSleep();
    }
    delay(200);
  }
  DEBUG_PRINTLN("Connected!");
}

void loop() {
  float distanceCm  = measureDistance();
  uint8_t voltageZb = 0;
  uint8_t battPct   = measureBattery(&voltageZb);

  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);

  if (distanceCm > 0.0f) {
    zbSensor.setAnalogInput(distanceCm);
    _pendingReports = 1;
    zbSensor.reportAnalogInput();
    waitForReport("Distance");
  }

  zbSensor.setBatteryPercentage(battPct);
  if (battPct != _rtcLastBattPct) {
    _pendingReports = 1;
    zbSensor.reportBatteryPercentage();
    waitForReport("Battery");
    _rtcLastBattPct = battPct;
  }

#if defined(DEBUG)
  Serial.flush();
#endif
  enterTimedDeepSleep();
}
