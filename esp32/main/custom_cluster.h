#pragma once

#include "esp_zigbee.h"

#define CUSTOM_CLUSTER_ID        0xFC00
#define ATTR_DISTANCE            0x0000
#define ATTR_WAKE_COUNT          0x0001
#define ATTR_LAST_RUNTIME_MS     0x0002
#define ATTR_VL53_ERROR_COUNT    0x0003

void custom_cluster_init(uint8_t ep_id, ezb_af_ep_desc_t ep_desc);
void custom_cluster_report_distance(float distance_cm);
void custom_cluster_report_wake_count(uint32_t count);
void custom_cluster_report_runtime_ms(uint32_t ms);
void custom_cluster_report_vl53_error_count(uint32_t count);
