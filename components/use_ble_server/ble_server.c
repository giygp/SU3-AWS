/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh
 * @LastEditTime: 2026-07-30
 * @Description: BLE GATT Server，支持最多 BLE_CONN_MAX 路手机同时连接
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_gatt_common_api.h"
#include "ble_server.h"
#include "common.h"


extern device_info_t *device_info;

#define GATTS_TABLE_TAG "GATTS_TABLE_DEMO"

#define PROFILE_NUM 1
#define PROFILE_APP_IDX 0
#define ESP_APP_ID 0x55
#define SVC_INST_ID 0

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 500
#define PREPARE_BUF_MAX_SIZE 1024
#define CHAR_DECLARATION_SIZE (sizeof(uint8_t))

#define ADV_CONFIG_FLAG (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

static uint8_t adv_config_done = 0;

uint16_t heart_rate_handle_table[HRS_IDX_NB];

static uint8_t service_uuid[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    0x03,
    0x00,
    0x13,
    0xac,
    0x42,
    0x02,
    0x03,
    0x9a,
    0xeb,
    0x11,
    0x39,
    0xf3,
    0xfe,
    0x3c,
    0xb5,
    0x31,
};

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(service_uuid),
    .p_service_uuid = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(service_uuid),
    .p_service_uuid = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

struct gatts_profile_inst
{
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

static struct gatts_profile_inst heart_rate_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

static const uint16_t GATTS_SERVICE_UUID_TEST = 0x00EE;
static const uint16_t GATTS_CHAR_UUID_TEST_A = 0xFF02;
static const uint16_t GATTS_CHAR_UUID_TEST_B = 0xFF03;
static const uint16_t GATTS_CHAR_UUID_TEST_C = 0xFF04;

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t char_prop_read = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t char_prop_read_write_notify = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t heart_measurement_ccc[2] = {0x00, 0x00};
uint8_t char_value[4] = {0x11, 0x12, 0x13, 0x14};

static const esp_gatts_attr_db_t gatt_db[HRS_IDX_NB] =
    {
        [IDX_SVC] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ, sizeof(uint16_t), sizeof(GATTS_SERVICE_UUID_TEST), (uint8_t *)&GATTS_SERVICE_UUID_TEST}},

        [IDX_CHAR_A] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write_notify}},

        [IDX_CHAR_VAL_A] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_A, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

        [IDX_CHAR_CFG_A] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t), sizeof(heart_measurement_ccc), (uint8_t *)heart_measurement_ccc}},

        [IDX_CHAR_B] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read}},

        [IDX_CHAR_VAL_B] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_B, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

        [IDX_CHAR_C] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write}},

        [IDX_CHAR_VAL_C] =
            {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_C, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

};

/* ---------- 多连接表 ---------- */

static void ble_conn_recount(void)
{
    int i;
    uint8_t n = 0;
    if (device_info == NULL || device_info->ble == NULL) {
        return;
    }
    for (i = 0; i < BLE_CONN_MAX; i++) {
        if (device_info->ble->conns[i].in_use) {
            n++;
        }
    }
    device_info->ble->conn_count = n;
}

static int ble_conn_find(uint16_t conn_id)
{
    int i;
    if (device_info == NULL || device_info->ble == NULL) {
        return -1;
    }
    for (i = 0; i < BLE_CONN_MAX; i++) {
        if (device_info->ble->conns[i].in_use &&
            device_info->ble->conns[i].conn_id == conn_id) {
            return i;
        }
    }
    return -1;
}

static int ble_conn_add(uint16_t conn_id)
{
    int i;
    if (device_info == NULL || device_info->ble == NULL) {
        return -1;
    }
    i = ble_conn_find(conn_id);
    if (i >= 0) {
        ble_conn_recount();
        return i;
    }
    for (i = 0; i < BLE_CONN_MAX; i++) {
        if (!device_info->ble->conns[i].in_use) {
            device_info->ble->conns[i].in_use = true;
            device_info->ble->conns[i].conn_id = conn_id;
            /* 兼容旧 App：连上即允许 Notify，CCCD 写入后再精确更新 */
            device_info->ble->conns[i].notify_en = true;
            ble_conn_recount();
            return i;
        }
    }
    ble_conn_recount();
    return -1;
}

static void ble_conn_remove(uint16_t conn_id)
{
    int i = ble_conn_find(conn_id);
    if (i < 0 || device_info == NULL || device_info->ble == NULL) {
        ESP_LOGW(GATTS_TABLE_TAG, "remove miss conn_id=%u", conn_id);
        ble_conn_recount();
        return;
    }
    device_info->ble->conns[i].in_use = false;
    device_info->ble->conns[i].conn_id = 0;
    device_info->ble->conns[i].notify_en = false;
    ble_conn_recount();
}

static void ble_conn_set_notify(uint16_t conn_id, bool en)
{
    int i = ble_conn_find(conn_id);
    if (i >= 0) {
        device_info->ble->conns[i].notify_en = en;
    }
}

static void ble_try_restart_adv(void)
{
    if (device_info == NULL || device_info->ble == NULL) {
        return;
    }
    if (device_info->ble->conn_count < BLE_CONN_MAX) {
        esp_err_t ret = esp_ble_gap_start_advertising(&adv_params);
        if (ret != ESP_OK) {
            ESP_LOGW(GATTS_TABLE_TAG, "restart adv failed: %s", esp_err_to_name(ret));
        }
    } else {
        (void)esp_ble_gap_stop_advertising();
    }
}

uint8_t ble_conn_count(void)
{
    if (device_info == NULL || device_info->ble == NULL) {
        return 0;
    }
    return device_info->ble->conn_count;
}

esp_err_t ble_notify_send(const uint8_t *data, uint16_t len)
{
    int i;
    esp_err_t last = ESP_ERR_INVALID_STATE;
    int sent = 0;

    if (data == NULL || len == 0 || device_info == NULL || device_info->ble == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device_info->ble->conn_count == 0 || !device_info->ble->flag) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < BLE_CONN_MAX; i++) {
        ble_conn_t *c = &device_info->ble->conns[i];
        if (!c->in_use || !c->notify_en) {
            continue;
        }
        last = esp_ble_gatts_send_indicate(device_info->ble->gatts_if,
                                          c->conn_id,
                                          device_info->ble->handle,
                                          len, (uint8_t *)data, false);
        if (last == ESP_OK) {
            sent++;
        } else {
            ESP_LOGW(GATTS_TABLE_TAG, "notify fail conn_id=%u: %s",
                     c->conn_id, esp_err_to_name(last));
        }
    }
    return (sent > 0) ? ESP_OK : last;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~ADV_CONFIG_FLAG);
        if (adv_config_done == 0)
        {
            esp_err_t ret = esp_ble_gap_start_advertising(&adv_params);
            if (ret != ESP_OK) {
                ESP_LOGE(GATTS_TABLE_TAG, "ADV_DATA_SET start adv fail: %d", ret);
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done == 0)
        {
            esp_err_t ret = esp_ble_gap_start_advertising(&adv_params);
            if (ret != ESP_OK) {
                ESP_LOGE(GATTS_TABLE_TAG, "SCAN_RSP start adv fail: %d", ret);
            }
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TABLE_TAG, "advertising start failed");
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TABLE_TAG, "Advertising stop failed");
        }
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        break;

    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
        if (param->local_privacy_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "config local privacy failed, error status = %x",
                     param->local_privacy_cmpl.status);
            break;
        }

        {
            esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
            if (ret)
            {
                ESP_LOGE(GATTS_TABLE_TAG, "config adv data failed, error code = %x", ret);
            }
            else
            {
                adv_config_done |= ADV_CONFIG_FLAG;
            }

            ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
            if (ret)
            {
                ESP_LOGE(GATTS_TABLE_TAG, "config scan rsp failed, error code = %x", ret);
            }
            else
            {
                adv_config_done |= SCAN_RSP_CONFIG_FLAG;
            }
        }
        break;
    default:
        break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        device_info->ble->gatts_if = gatts_if;
        esp_ble_gap_set_device_name(device_info->id);
        esp_ble_gap_config_local_privacy(true);
        esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, HRS_IDX_NB, SVC_INST_ID);
        break;
    case ESP_GATTS_READ_EVT:
        break;
    case ESP_GATTS_WRITE_EVT:
        /* 业务下发：Char 0xFF04；Notify 上报：0xFF02 */
        if (param->write.handle == heart_rate_handle_table[IDX_CHAR_CFG_A] &&
            param->write.len >= 2)
        {
            uint16_t cccd = param->write.value[0] | ((uint16_t)param->write.value[1] << 8);
            ble_conn_set_notify(param->write.conn_id, (cccd & 0x0001) != 0);
            break;
        }
        if (param->write.len > 0 &&
            param->write.handle == heart_rate_handle_table[IDX_CHAR_VAL_C] &&
            !param->write.is_prep)
        {
            uint8_t qbuf[512];
            size_t copy_len = param->write.len;

            if (copy_len >= sizeof(qbuf)) {
                copy_len = sizeof(qbuf) - 1U;
            }
            memset(qbuf, 0, sizeof(qbuf));
            memcpy(qbuf, param->write.value, copy_len);
            if (xQueueSend(device_info->ble->xQueue, qbuf, 0) != pdPASS) {
                ESP_LOGW(GATTS_TABLE_TAG, "BLE RX queue full");
            }
        }
        break;
    case ESP_GATTS_EXEC_WRITE_EVT:
        break;
    case ESP_GATTS_MTU_EVT:
        break;
    case ESP_GATTS_CONF_EVT:
        break;
    case ESP_GATTS_UNREG_EVT:
        break;
    case ESP_GATTS_DELETE_EVT:
        break;
    case ESP_GATTS_START_EVT:
        break;
    case ESP_GATTS_STOP_EVT:
        break;
    case ESP_GATTS_CONNECT_EVT:
    {
        int slot;
        esp_ble_conn_update_params_t conn_params = {0};

        slot = ble_conn_add(param->connect.conn_id);
        if (slot < 0) {
            ESP_LOGW(GATTS_TABLE_TAG, "conn full, reject conn_id=%u",
                     param->connect.conn_id);
            esp_ble_gap_disconnect(param->connect.remote_bda);
            break;
        }
        ESP_LOGI(GATTS_TABLE_TAG, "CONNECT conn_id=%u count=%u",
                 param->connect.conn_id, device_info->ble->conn_count);
        set_ble_status(1);
        device_info->ble->gatts_if = gatts_if;

        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x20;  /* 40ms */
        conn_params.min_int = 0x10;  /* 20ms */
        conn_params.timeout = 400;   /* 4s */
        esp_ble_gap_update_conn_params(&conn_params);

        ble_try_restart_adv();
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT:
        ble_conn_remove(param->disconnect.conn_id);
        ESP_LOGI(GATTS_TABLE_TAG, "DISCONNECT conn_id=%u reason=0x%x count=%u",
                 param->disconnect.conn_id, param->disconnect.reason,
                 device_info->ble->conn_count);
        set_ble_status(device_info->ble->conn_count > 0 ? 1 : 0);
        ble_try_restart_adv();
        break;
    case ESP_GATTS_OPEN_EVT:
        break;
    case ESP_GATTS_CANCEL_OPEN_EVT:
        break;
    case ESP_GATTS_CLOSE_EVT:
        break;
    case ESP_GATTS_LISTEN_EVT:
        break;
    case ESP_GATTS_CONGEST_EVT:
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
    {
        if (param->create.status == ESP_GATT_OK)
        {
            if (param->add_attr_tab.num_handle == HRS_IDX_NB)
            {
                memcpy(heart_rate_handle_table, param->add_attr_tab.handles,
                       sizeof(heart_rate_handle_table));
                device_info->ble->handle = heart_rate_handle_table[IDX_CHAR_VAL_A];
                esp_ble_gatts_start_service(heart_rate_handle_table[IDX_SVC]);
            }
            else
            {
                ESP_LOGE(GATTS_TABLE_TAG,
                         "Create attribute table abnormally, num_handle (%d) != HRS_IDX_NB(%d)",
                         param->add_attr_tab.num_handle, HRS_IDX_NB);
            }
        }
        else
        {
            ESP_LOGE(GATTS_TABLE_TAG, " Create attribute table failed, error code = %x",
                     param->create.status);
        }
        break;
    }
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                               esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT)
    {
        if (param->reg.status == ESP_GATT_OK)
        {
            heart_rate_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
        }
        else
        {
            ESP_LOGE(GATTS_TABLE_TAG, "reg app failed, app_id %04x, status %d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }
    do
    {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++)
        {
            if (gatts_if == ESP_GATT_IF_NONE ||
                gatts_if == heart_rate_profile_tab[idx].gatts_if)
            {
                if (heart_rate_profile_tab[idx].gatts_cb)
                {
                    heart_rate_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

void initialize_ble_server(void)
{
    esp_err_t ret;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "gatts register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "gap register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "gatts app register error, error code = %x", ret);
        return;
    }
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }
}
