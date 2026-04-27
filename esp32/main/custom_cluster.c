#include "custom_cluster.h"
#include "esp_log.h"
#include "ezbee/core_types.h"

static const char *TAG = "CUSTOM_CLUSTER";
static uint8_t _ep_id = 1;

/* ---------- attribute reporting ---------- */

static void send_attr_report(uint16_t attr_id)
{
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl.dst_addr   = EZB_ADDRESS_SHORT(0x0000),
        .cmd_ctrl.dst_ep     = 1,
        .cmd_ctrl.src_ep     = _ep_id,
        .cmd_ctrl.cluster_id = CUSTOM_CLUSTER_ID,
        .cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE,
        .cmd_ctrl.fc.direction       = 1,
        .cmd_ctrl.fc.dis_default_rsp = 1,
        .payload.attr_id = attr_id,
    };
    ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&cmd);
    if (ret != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "report attr 0x%04X failed: %d", attr_id, ret);
    }
}

static void set_and_report(uint16_t attr_id, void *value)
{
    ezb_zcl_set_attr_value(_ep_id, CUSTOM_CLUSTER_ID, EZB_ZCL_CLUSTER_SERVER,
                           attr_id, EZB_ZCL_STD_MANUF_CODE, value, false);
    send_attr_report(attr_id);
}

void custom_cluster_report_distance(float distance_cm)
{
    set_and_report(ATTR_DISTANCE, &distance_cm);
}

void custom_cluster_report_wake_count(uint32_t count)
{
    set_and_report(ATTR_WAKE_COUNT, &count);
}

void custom_cluster_report_runtime_ms(uint32_t ms)
{
    set_and_report(ATTR_LAST_RUNTIME_MS, &ms);
}

void custom_cluster_report_vl53_error_count(uint32_t count)
{
    set_and_report(ATTR_VL53_ERROR_COUNT, &count);
}

/* ---------- custom cluster handler callbacks ---------- */

static ezb_zcl_status_t check_value_cb(uint16_t attr_id, uint8_t ep_id, void *value)
{
    (void)attr_id;
    (void)ep_id;
    (void)value;
    return EZB_ZCL_STATUS_SUCCESS;
}

static void write_attr_cb(uint8_t ep_id, uint16_t attr_id, void *new_value, uint16_t manuf_code)
{
    ESP_LOGI(TAG, "attr write: ep=%u attr=0x%04X manuf=0x%04X", ep_id, attr_id, manuf_code);
}

static ezb_zcl_status_t process_cmd_cb(const ezb_zcl_cmd_hdr_t *header,
                                        const uint8_t *payload, uint16_t payload_length)
{
    ESP_LOGI(TAG, "cmd: cluster=0x%04X cmd=0x%02X len=%u",
             header->cluster_id, header->cmd_id, payload_length);
    return EZB_ZCL_STATUS_SUCCESS;
}

static uint8_t disc_cmd_cb(bool is_recv, uint8_t **list)
{
    (void)is_recv;
    (void)list;
    return 0;
}

/* ---------- init ---------- */

void custom_cluster_init(uint8_t ep_id, ezb_af_ep_desc_t ep_desc)
{
    _ep_id = ep_id;

    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id  = CUSTOM_CLUSTER_ID,
        .init_func   = NULL,
        .deinit_func = NULL,
    };
    ezb_zcl_cluster_desc_t custom_desc =
        ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);

    float    dist_init    = 0.0f;
    uint32_t wake_init    = 0;
    uint32_t runtime_init = 0;

    ezb_zcl_custom_cluster_desc_add_attr(custom_desc, ATTR_DISTANCE,
        EZB_ZCL_ATTR_TYPE_SINGLE,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        &dist_init);
    ezb_zcl_custom_cluster_desc_add_attr(custom_desc, ATTR_WAKE_COUNT,
        EZB_ZCL_ATTR_TYPE_UINT32,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        &wake_init);
    ezb_zcl_custom_cluster_desc_add_attr(custom_desc, ATTR_LAST_RUNTIME_MS,
        EZB_ZCL_ATTR_TYPE_UINT32,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        &runtime_init);
    uint32_t vl53_err_init = 0;
    ezb_zcl_custom_cluster_desc_add_attr(custom_desc, ATTR_VL53_ERROR_COUNT,
        EZB_ZCL_ATTR_TYPE_UINT32,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        &vl53_err_init);

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, custom_desc));

    ezb_zcl_custom_cluster_handlers_t handlers = {
        .cluster_id     = CUSTOM_CLUSTER_ID,
        .cluster_role   = EZB_ZCL_CLUSTER_SERVER,
        .check_value_cb = check_value_cb,
        .write_attr_cb  = write_attr_cb,
        .process_cmd_cb = process_cmd_cb,
        .cmd_disc_cb    = disc_cmd_cb,
    };
    ezb_zcl_custom_cluster_handlers_register(&handlers);

    ESP_LOGI(TAG, "initialized on ep %u", ep_id);
}
