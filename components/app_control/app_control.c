/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh 
 * @LastEditTime: 2026-07-14 18:19:35
 */
#include "app_control.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "use_wifi.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "use_wifi.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "sntp.h"
#include <time.h>
#include "use_uart.h"
#include "driver/uart.h"
#include "pb_decode.h"
#include "keesoncloud.pb.h"
#include "qs_protobuf.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "use_mfp.h"
#include "su3_client.h"
#include "ble_server.h"

// ==================== 调试配置开关 ====================
// 量产时将下面的宏改为 0，调试时改为 1
#define ENABLE_DEBUG_DISPLAY    1   // 1=启用调试显示任务  0=关闭（量产）
#define ENABLE_PERFORMANCE_MON  0   // 1=启用性能监控任务  0=关闭（量产）
#define ENABLE_MFP_DETAIL_LOG   0   // 1=启用MFP详细日志  0=仅错误日志（量产）
// ======================================================

static const char *TAG = "control";
extern device_info_t *device_info;
extern esp_mqtt_client_handle_t client;
extern char user_5s_data_publish_topic[64];
extern char user_60s_data_publish_topic[64];
extern char user_sa_data_publish_topic[64];
extern char user_sleep_data_publish_topic[64];
extern char user_cli_data_publish_topic[64];
extern char mc_cli_data_publish_topic[64];

extern qs_pb_msg_sensor_1min_info *user_60s_sensor_info;
extern qs_pb_msg_sensor_5sec_info *user_5s_sensor_info;
time_t now;
static uint16_t human_year;
static uint8_t human_mon;
struct tm ti;
bool get_5s_flag = false;
bool mqtt_send_mutex = true;


/* 按侧缓存当前报告名，避免左右串名 */
static char s_json_report_name[SU3_SENSOR_SIDE_MAX][32];
/* 左右最新报告头（NVS report_L/R），上电 load、补传后 save */
static char s_last_report[SU3_SENSOR_SIDE_MAX][32];

static void su3_make_side_id(char *out, size_t out_len, int side);
static void check_stack_space(void);

static char ota_now_flag = 0;
static snore_parameters_t snore_parameters_demo = {0}; // 打鼾参数
static uint32_t snore_blocked_until_s = 0; // 闹钟指令后 120s 内禁止打鼾干预（开机秒数

/** 写入 NVS sensor_ver + 内存缓存（左右同源，只存一份） */
void su3_sensor_ver_save(const char *ver)
{
    nvs_handle h;
    char cleaned[48];
    size_t i;
    size_t n;

    if (ver == NULL || ver[0] == '\0' || device_info == NULL) {
        return;
    }
    /* 去掉应答里的 \\r\\n */
    n = 0;
    for (i = 0; ver[i] != '\0' && n + 1U < sizeof(cleaned); i++) {
        if (ver[i] == '\r' || ver[i] == '\n') {
            break;
        }
        cleaned[n++] = ver[i];
    }
    cleaned[n] = '\0';
    if (cleaned[0] == '\0') {
        return;
    }
    if (strcmp(device_info->sensor_ver, cleaned) == 0) {
        return; /* 未变不写 flash */
    }
    strncpy(device_info->sensor_ver, cleaned, sizeof(device_info->sensor_ver) - 1U);
    device_info->sensor_ver[sizeof(device_info->sensor_ver) - 1U] = '\0';
    if (nvs_open("config_cfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, "sensor_ver", device_info->sensor_ver);
    (void)nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "sensor_ver saved: %s", device_info->sensor_ver);
}
#define ECHO_TEST_TXD (19)
#define ECHO_TEST_RXD (25)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (2)
#define ECHO_UART_BAUD_RATE     (38400)
#define ECHO_TASK_STACK_SIZE    (4096)

#define BUF_SIZE (512)

/* MFP(UART2) 事件接收：1=每次 UART_DATA 打印 read 长度（流量大，默认 0） */
#define DEBUG_MFP_UART_RX_LOG   0

static QueueHandle_t uart2_queue;

union SyncCommunicationData_t g_Sync_TX;
union SyncCommunicationData_t g_Sync_RX;
g_system_flag_t g_system_flag;

// MFP解析缓冲区 - 独立于接收缓冲区,避免数据被清零
static uint8_t g_MFP_Parsed_Frame[256];
static uint16_t g_MFP_Parsed_Len = 0;
static SemaphoreHandle_t g_MFP_Parsed_Mutex = NULL;

//蓝牙数据解析
int ble_data_parser_cb(uint8_t *data)
{
    {
        cJSON *firstItem = NULL;
        cJSON *idItem = NULL;
        cJSON *typeItem = NULL;
        cJSON *cmdItem = NULL;
        cJSON *thirdItem = NULL;
        const char *ble_id;
        int ble_type;

        if (data == NULL) {
            return 0;
        }

        firstItem = cJSON_Parse((char *)data);
        if (!firstItem) {
            return 0;
        }

        idItem = cJSON_GetObjectItem(firstItem, "id");
        typeItem = cJSON_GetObjectItem(firstItem, "type");
        if (!idItem || !idItem->valuestring || !typeItem) {
            cJSON_Delete(firstItem);
            return 0;
        }
        ble_id = idItem->valuestring;
        /* App 若发 "type":"3" 字符串，valueint=0，会进不了配网分支 */
        if (cJSON_IsNumber(typeItem)) {
            ble_type = typeItem->valueint;
        } else if (cJSON_IsString(typeItem) && typeItem->valuestring) {
            ble_type = atoi(typeItem->valuestring);
        } else {
            cJSON_Delete(firstItem);
            return 0;
        }

        /* type:17 SU3 CLI：id 可为 00/03/06，与 MQTT sensorCli 相同 */
        if (ble_type == 17) {
            if (!su3_mqtt_id_belong(ble_id)) {
                cJSON_Delete(firstItem);
                return 0;
            }
            cmdItem = cJSON_GetObjectItem(firstItem, "cmd");
            if (!cmdItem || !cmdItem->valuestring) {
                cJSON_Delete(firstItem);
                return 0;
            }
            if (!su3_stack_ready()) {
                char temp[256];
                snprintf(temp, sizeof(temp),
                         "{\"id\":\"%s\",\"ts\":%d,\"type\":17,\"cmd\":\"%s\",\"back\":\"su3 not ready\"}",
                         ble_id, device_info->utc.time_stamp, cmdItem->valuestring);
                (void)ble_notify_send((uint8_t *)temp, (uint16_t)strlen(temp));
            } else {
                su3_ble_handle_sensor_cli(ble_id, cmdItem->valuestring);
            }
            cJSON_Delete(firstItem);
            return 0;
        }

        /* 其余 BLE 指令仍要求基座 00 id */
        if (strcmp(ble_id, device_info->id) != 0) {
            cJSON_Delete(firstItem);
            return 0;
        }

        if (ble_type == 3)  // 配网
        {
            cmdItem = cJSON_GetObjectItem(firstItem, "cmd");
            if (!cmdItem) {
                cJSON_Delete(firstItem);
                return 0;
            }

            thirdItem = cJSON_GetObjectItem(cmdItem, "ssid");
            if (!thirdItem || !thirdItem->valuestring) {
                cJSON_Delete(firstItem);
                return 0;
            }
            memset(device_info->wifi.one_key_config.ssid, 0, 32);
            memcpy(device_info->wifi.one_key_config.ssid, thirdItem->valuestring, strlen(thirdItem->valuestring));

            thirdItem = cJSON_GetObjectItem(cmdItem, "passwd");
            if (!thirdItem || !thirdItem->valuestring) {
                cJSON_Delete(firstItem);
                return 0;
            }
            memset(device_info->wifi.one_key_config.passwd, 0, 64);
            memcpy(device_info->wifi.one_key_config.passwd, thirdItem->valuestring, strlen(thirdItem->valuestring));
            if(get_mqtt_status())
            {
                esp_mqtt_client_stop(client);
                set_mqtt_status(0);
            }
            if (get_wifi_status())
            {
                ESP_ERROR_CHECK(esp_wifi_disconnect());
            }

            set_wifi_status(WIFI_STATUS_WAITING);
            memset(wifi_config.sta.ssid, 0, 32);
            memset(wifi_config.sta.password, 0, 64);
            strncpy((char *)wifi_config.sta.ssid, device_info->wifi.one_key_config.ssid, sizeof(wifi_config.sta.ssid) - 1);
            strncpy((char *)wifi_config.sta.password, device_info->wifi.one_key_config.passwd, sizeof(wifi_config.sta.password) - 1);
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

            set_one_key_config_wifi_status(1);
            ESP_ERROR_CHECK(esp_wifi_stop());

            s_retry_num = 0;
            ESP_ERROR_CHECK(esp_wifi_start());
        }
        else if (ble_type == 4)
        {
            cmdItem = cJSON_GetObjectItem(firstItem, "cmd");
            if (!cmdItem) {
                cJSON_Delete(firstItem);
                return 0;
            }

            char temp[192] = {0};
            if (cmdItem->valuestring && strcmp(cmdItem->valuestring, "sensorVersion") == 0)
            {
                snprintf(temp, sizeof(temp),
                         "{\"id\":\"%s\",\"ts\":%d,\"type\":4,\"version\":\"%s\"}",
                         device_info->id,
                         device_info->utc.time_stamp,
                         device_info->sensor_ver[0] ? device_info->sensor_ver : "");

                printf("sensorVersion = %s \n",temp);
                (void)ble_notify_send((uint8_t *)temp, (uint16_t)strlen(temp));
            }
        }
        else if (ble_type == 18)
        {
            /* BLE deviceCli：与 MQTT type=deviceCli 对齐，基座 00 */
            char temp[192] = {0};
            char cli_back[16];
            cmdItem = cJSON_GetObjectItem(firstItem, "cmd");
            if (!cmdItem || !cmdItem->valuestring) {
                cJSON_Delete(firstItem);
                return 0;
            }
            if (su3_device_cli_handle(cmdItem->valuestring, cli_back, sizeof(cli_back))) {
                snprintf(temp, sizeof(temp),
                         "{\"id\":\"%s\",\"ts\":%d,\"type\":18,"
                         "\"cmd\":\"%s\",\"back\":\"%s\"}",
                         device_info->id, device_info->utc.time_stamp,
                         cmdItem->valuestring, cli_back);
                printf("ble deviceCli %s back=%s\n", cmdItem->valuestring, cli_back);
                (void)ble_notify_send((uint8_t *)temp, (uint16_t)strlen(temp));
            }
        }
        else if (ble_type == 5)
        {
            cmdItem = cJSON_GetObjectItem(firstItem, "cmd");
            if (!cmdItem) {
                cJSON_Delete(firstItem);
                return 0;
            }

            char temp[100] = {0};
            if (cmdItem->valuestring && strcmp(cmdItem->valuestring, "sensorUpgrade") == 0)
            {
                (void)sensor_ota_bc(NULL);
                sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":5,\"sdate\":\"sensor OTA not supported yet\"}",
                        device_info->id, device_info->utc.time_stamp);
                printf("sensorUpgrade = %s \n",temp);
                (void)ble_notify_send((uint8_t *)temp, (uint16_t)strlen(temp));
            }
        }
        else if (ble_type == 12)
        {
            uint8_t cmd_bin[64] = {0};
            cmdItem = cJSON_GetObjectItem(firstItem, "cmd");
            if (!cmdItem || !cmdItem->valuestring) {
                cJSON_Delete(firstItem);
                return 0;
            }

            const char *cmd_str = cmdItem->valuestring;
            int cmd_len = 0;
            while (*cmd_str && *(cmd_str + 1) && (cmd_len < 64)) {
                char byte_str[3] = { cmd_str[0], cmd_str[1], '\0' };
                cmd_bin[cmd_len++] = (uint8_t)strtol(byte_str, NULL, 16);
                cmd_str += 2;
            }
            if (cmd_len > 0 && cmd_bin[0] == 0xAA)
            {
                mqtt_ble_data_parser_cb(cmd_bin);
            } else {
                printf("head error\n");
            }
        }

        cJSON_Delete(firstItem);
    }
    return 0;
}

void one_key_config_wifi_task(void *pv)
{
    uint8_t enevt_id;
    char temp[100] = {0};
    while (1)
    {
        if (xQueueReceive(device_info->wifi.one_key_config.xQueue, &enevt_id, portMAX_DELAY))
        {
            switch (enevt_id)
            {
            case wifi_ok:
                sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":3,\"state\":\"wifiOk\"}",
                                                                                    device_info->id,
                                                                                    device_info->utc.time_stamp);
                break;
            case wifi_fail:
                sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":3,\"state\":\"wifiFail\"}",
                                                                                    device_info->id,
                                                                                    device_info->utc.time_stamp);
                break;
            case mqtt_ok:
                sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":3,\"state\":\"mqttOk\"}",
                                                                                    device_info->id,
                                                                                    device_info->utc.time_stamp);                                                               
                break;
            case mqtt_fail:
                sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":3,\"state\":\"mqttFail\"}",
                                                                                    device_info->id,
                                                                                    device_info->utc.time_stamp);
                break;
            default:
                break;
            }

            if(get_ble_status())
            {
                (void)ble_notify_send((uint8_t *)temp, (uint16_t)strlen(temp));
            }
        }
    }
    vTaskDelete(NULL);
}

void ble_data_parser_task(void *pv)
{
    while (1)
    {
        if (xQueueReceive(device_info->ble->xQueue, device_info->ble->data_rec.value, portMAX_DELAY))
        {
            device_info->ble->data_rec.value[sizeof(device_info->ble->data_rec.value) - 1U] = '\0';
            ble_data_parser_cb(device_info->ble->data_rec.value);
        }
    }
    vTaskDelete(NULL);
}

/*
 * 睡眠报告 MQTT 异步上报：RX 回调只解码+入队，独立任务再 publish，
 * 避免在 su3_rx 里连发 MQTT 堵串口导致缺 type8~12。
 */
#define SU3_SLEEP_UP_Q_LEN     16
#define SU3_SLEEP_UP_KIND_RAW  0   /* type6~12 原始序列 */
#define SU3_SLEEP_UP_KIND_JSON 1   /* type5 已拼好的 JSON */

typedef struct {
    uint8_t kind;
    int8_t side;
    uint8_t type;
    uint16_t len;
    uint8_t *payload; /* malloc，由 sleep_up worker free */
    char report[32];  /* 入队时快照，避免补传过程中被覆盖 */
} su3_sleep_up_job_t;

static QueueHandle_t s_sleep_up_q;

static bool su3_mqtt_publish_sleep(const char *json)
{
    int retry;

    if (json == NULL ||
        get_wifi_status() != WIFI_STATUS_CONNECTED ||
        !get_mqtt_status()) {
        return false;
    }
    for (retry = 0; retry < 50; retry++) {
        if (mqtt_send_mutex) {
            mqtt_send_mutex = false;
            /* 只打摘要，避免整包 JSON printf 占 CPU/串口导致后续 report topic 丢帧 */
            printf("sleep/put ok bytes=%u\n", (unsigned)strlen(json));
            esp_mqtt_client_publish(client, user_sleep_data_publish_topic,
                                    json, (int)strlen(json), 0, 0);
            mqtt_send_mutex = true;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "sleep mqtt publish busy timeout");
    return false;
}

static bool su3_sleep_up_enqueue(uint8_t kind, int side, uint8_t type,
                                 const uint8_t *data, uint16_t len)
{
    su3_sleep_up_job_t job;
    uint8_t *copy;

    if (s_sleep_up_q == NULL || data == NULL || len == 0) {
        return false;
    }
    if (kind == SU3_SLEEP_UP_KIND_RAW && len > QS_PB_RAW_DATA_LEGNTH) {
        len = QS_PB_RAW_DATA_LEGNTH;
    }
    copy = (uint8_t *)malloc(len + (kind == SU3_SLEEP_UP_KIND_JSON ? 1U : 0U));
    if (copy == NULL) {
        ESP_LOGE(TAG, "sleep_up malloc fail len=%u", (unsigned)len);
        return false;
    }
    memcpy(copy, data, len);
    if (kind == SU3_SLEEP_UP_KIND_JSON) {
        copy[len] = '\0';
    }
    memset(&job, 0, sizeof(job));
    job.kind = kind;
    job.side = (int8_t)side;
    job.type = type;
    job.len = len;
    job.payload = copy;
    if (side >= 0 && side < SU3_SENSOR_SIDE_MAX) {
        strncpy(job.report, s_json_report_name[side], sizeof(job.report) - 1U);
    }
    if (xQueueSend(s_sleep_up_q, &job, 0) != pdTRUE) {
        free(copy);
        ESP_LOGW(TAG, "sleep_up queue full drop kind=%u type=%u side=%d",
                 (unsigned)kind, (unsigned)type, side);
        return false;
    }
    return true;
}

/**
 * 睡眠报告 type6~12 分包上报；id 按侧 03/06。
 * 满包/尾包统一用 cJSON_Print + Minify（动态分配），禁止固定 1200+pretty。
 * 仅由 sleep_up worker 调用（勿在 RX 回调里调）。
 */
static void report_to_aliyun(int side, uint8_t type, uint8_t *value, uint16_t len,
                             const char *report_name)
{
    char side_id[24];
    const uint16_t one_packet_len = 200;
    uint16_t packet_num;
    uint16_t i, j;
    uint16_t offset;
    uint16_t this_len;

    if (!value || len == 0 || side < 0 || side >= SU3_SENSOR_SIDE_MAX) {
        return;
    }
    if (report_name == NULL) {
        report_name = "";
    }
    su3_make_side_id(side_id, sizeof(side_id), side);

    packet_num = (uint16_t)((len + one_packet_len - 1U) / one_packet_len);
    printf("sleep report side=%d type=%u report=\"%s\" len=%u packets=%u\n",
           side, (unsigned)type, report_name, (unsigned)len, (unsigned)packet_num);

    for (i = 0; i < packet_num; i++) {
        cJSON *root;
        cJSON *data;
        cJSON *arr;
        char *p_str;

        offset = (uint16_t)(i * one_packet_len);
        this_len = (uint16_t)(len - offset);
        if (this_len > one_packet_len) {
            this_len = one_packet_len;
        }

        root = cJSON_CreateObject();
        data = cJSON_CreateObject();
        arr = cJSON_CreateArray();
        if (root == NULL || data == NULL || arr == NULL) {
            cJSON_Delete(root);
            cJSON_Delete(data);
            cJSON_Delete(arr);
            ESP_LOGE(TAG, "sleep report cJSON alloc fail");
            return;
        }

        cJSON_AddStringToObject(root, "id", side_id);
        cJSON_AddNumberToObject(root, "ts", device_info->utc.time_stamp);
        cJSON_AddNumberToObject(root, "type", type);
        cJSON_AddStringToObject(root, "report", report_name);

        cJSON_AddNumberToObject(data, "len", len);
        cJSON_AddNumberToObject(data, "allPacket", packet_num);
        cJSON_AddNumberToObject(data, "packet", i);
        cJSON_AddNumberToObject(data, "packetLen", this_len);
        for (j = 0; j < this_len; j++) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(value[offset + j]));
        }
        cJSON_AddItemToObject(data, "value", arr);
        cJSON_AddItemToObject(root, "data", data);

        p_str = cJSON_Print(root);
        cJSON_Delete(root);
        if (p_str == NULL) {
            ESP_LOGE(TAG, "sleep report Print fail type=%u packet=%u",
                     (unsigned)type, (unsigned)i);
            return;
        }
        cJSON_Minify(p_str);
        printf("sleep/put side=%d type=%u packet=%u/%u\n",
               side, (unsigned)type, (unsigned)i, (unsigned)packet_num);
        (void)su3_mqtt_publish_sleep(p_str);
        free(p_str);
    }
}

static void su3_sleep_up_task(void *pv)
{
    su3_sleep_up_job_t job;
    (void)pv;

    while (1) {
        if (xQueueReceive(s_sleep_up_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.payload == NULL) {
            continue;
        }
        if (job.kind == SU3_SLEEP_UP_KIND_JSON) {
            if (get_wifi_status() == WIFI_STATUS_CONNECTED &&
                get_mqtt_status() && device_info->data_up_switch) {
                printf("sleep/put side=%d type=5 report=\"%s\"\n",
                       (int)job.side, job.report);
                (void)su3_mqtt_publish_sleep((const char *)job.payload);
            }
            if (get_ble_status() && device_info->data_up_switch) {
                (void)ble_notify_send(job.payload, job.len);
            }
        } else if (job.kind == SU3_SLEEP_UP_KIND_RAW) {
            report_to_aliyun((int)job.side, job.type, job.payload, job.len, job.report);
        }
        free(job.payload);
    }
}

static void su3_sleep_up_start(void)
{
    if (s_sleep_up_q != NULL) {
        return;
    }
    s_sleep_up_q = xQueueCreate(SU3_SLEEP_UP_Q_LEN, sizeof(su3_sleep_up_job_t));
    if (s_sleep_up_q == NULL) {
        ESP_LOGE(TAG, "sleep_up queue create fail");
        return;
    }
    /* 栈偏大：cJSON 组大包 */
    xTaskCreatePinnedToCore(su3_sleep_up_task, "sleep_up", 8192, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "sleep_up worker started");
}
void snoring_detection_func(void)
{
    uint8_t count_in_block = 0;
    
    // 统计当前5秒数据中，体动值为4的次数
    for (int i = 0; i < 5; i++) {
        if (user_5s_sensor_info->status[i] == 4) {
            count_in_block++;
        }
    }

    // 如果5秒内体动值为4的次数大于等于4，则认为本次数据块为打鼾包，记为1；否则为0
    uint8_t block_value = (count_in_block >= device_info->snore->snore_parameters.threshold_5s) ? 1 : 0;
    
    // 更新循环缓冲区：记录本次5秒数据块的打鼾状态
    // printf("block_index : %d, block_value : %d\n", device_info->snore->snore_state.block_index, block_value);
    device_info->snore->snore_state.snoring_block[device_info->snore->snore_state.block_index] = block_value;
    device_info->snore->snore_state.block_index = (device_info->snore->snore_state.block_index + 1) % device_info->snore->snore_state.block_size;
    
    
    // 计算最近2分钟内（24个数据块）打鼾包的数量
    uint16_t total_count = 0;
    for (int i = 0; i < device_info->snore->snore_state.block_size; i++) {
        total_count += device_info->snore->snore_state.snoring_block[i];
    }
    
    // 若2分钟内打鼾包数量达到阈值，则触发打鼾事件
    if (total_count >= device_info->snore->snore_parameters.threshold) {
        device_info->snore->snore_state.triggered_flag = true;
    }

}

// 系统控制任务（LED指示灯 + 打鼾检测）
// 这个任务独立于调试显示，量产时必须保留
void system_control_task(void *pv)
{
    while(1)
    {
        // LED指示灯控制：WiFi连接时红灯亮，绿灯灭；未连接时相反
        gpio_set_level(LED_RED, (device_info->wifi.flag == 1) ? 1 : 0);
        gpio_set_level(LED_GREEN, (device_info->wifi.flag == 1) ? 0 : 1);

        // 打鼾检测逻辑
        snoring_detection_func();

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

// 参数显示任务（仅用于调试，量产时可注释）
void data_display_task(void *pv)
{
    while(1)
    {
        ESP_LOGI(TAG, "/*======111=====================data display===============%d===========*/",0);
        ESP_LOGI(TAG, "version:%s,id:%s,dataUpOn %d,sensorOta %d,sensor_ver:%s,current_report L:%s R:%s",
                 device_info->ota.running_version,
                 device_info->id,
                 device_info->data_up_switch,
                 (int)get_ota_now_flag(),
                 device_info->sensor_ver[0] ? device_info->sensor_ver : "(empty)",
                 s_last_report[SU3_SIDE_LEFT][0] ? s_last_report[SU3_SIDE_LEFT] : "NONE",
                 s_last_report[SU3_SIDE_RIGHT][0] ? s_last_report[SU3_SIDE_RIGHT] : "NONE");
        ESP_LOGI(TAG, "5s-->num:%d\tstatus:[%d][%d][%d][%d][%d]\theart:%d\tbreath:%d dev-time:%d id:%s",user_5s_sensor_info->sequence, user_5s_sensor_info->status[0],
                                                                                                user_5s_sensor_info->status[1],
                                                                                                user_5s_sensor_info->status[2],
                                                                                                user_5s_sensor_info->status[3],
                                                                                                user_5s_sensor_info->status[4],
                                                                                                user_5s_sensor_info->heartbeat,
                                                                                                user_5s_sensor_info->breathRate,user_5s_sensor_info->timestamp,user_5s_sensor_info->device_id); //test 
        ESP_LOGI(TAG, "60s-->num:%d\ton_off_bed:%d\theart:%d\tbreath:%d\tMmin:%d\tMmean:%d\tNSD:%d\tNpd:%d\tSBP:%d\tDBP:%d", 
                                                                                                user_60s_sensor_info->sequence, 
                                                                                                user_60s_sensor_info->on_off_bed,
                                                                                                user_60s_sensor_info->heartbeat,
                                                                                                user_60s_sensor_info->breath_rate,
                                                                                                user_60s_sensor_info->Mmin,
                                                                                                user_60s_sensor_info->Mmean,
                                                                                                user_60s_sensor_info->NSD,
                                                                                                user_60s_sensor_info->Npd,
                                                                                                user_60s_sensor_info->SBP,
                                                                                                user_60s_sensor_info->DBP);

        ESP_LOGI(TAG, "wifi:%d\tmqtt:%d\tble:%d\ttime:%d\tsetupLR:%d/%d",device_info->wifi.flag, device_info->aliyun.flag,device_info->ble->flag,device_info->utc.flag,
                 (int)g_su3_sensor[0].setup_done, (int)g_su3_sensor[1].setup_done);
        if(device_info->utc.flag)
        {
            ESP_LOGI(TAG, "%d-%02d-%02d %02d:%02d:%02d  %02d %d", human_year ,human_mon, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec , ti.tm_wday, device_info->utc.time_stamp);
        }

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

/* 基座 id → 侧别 id（第 8～9 字符改为 03/06）
 * 例：KSSLBED00000001 → 左 KSSLBED03000001 / 右 KSSLBED06000001 */
static void su3_make_side_id(char *out, size_t out_len, int side)
{
    size_t n;

    if (out == NULL || out_len == 0 || device_info == NULL) {
        return;
    }
    strncpy(out, device_info->id, out_len - 1U);
    out[out_len - 1U] = '\0';
    n = strlen(out);
    if (n >= 9U) {
        out[7] = '0';
        out[8] = (side == SU3_SIDE_LEFT) ? '3' : '6';
    }
}

bool su3_mqtt_id_belong(const char *mqtt_id)
{
    const char *base;
    size_t n, i;

    if (mqtt_id == NULL || device_info == NULL || device_info->id[0] == '\0') {
        return false;
    }
    base = device_info->id;
    n = strlen(base);
    if (n < 9U || strlen(mqtt_id) != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (i == 7U || i == 8U) {
            continue;
        }
        if (mqtt_id[i] != base[i]) {
            return false;
        }
    }
    return (mqtt_id[7] == '0' &&
            (mqtt_id[8] == '0' || mqtt_id[8] == '3' || mqtt_id[8] == '6'));
}

static void su3_json_escape(const char *in, char *out, size_t out_len)
{
    size_t j = 0;

    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }
    for (size_t i = 0; in[i] != '\0' && (j + 2U) < out_len; i++) {
        char c = in[i];
        if (c == '\\' || c == '"') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            continue;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

#define SU3_CLI_REPLY_MQTT  0x01
#define SU3_CLI_REPLY_BLE   0x02

/* CLI 应答：MQTT type=sensorCli 和/或 BLE type=17（list 报告头较长，缓冲加大） */
static void su3_cli_reply(const char *id, const char *cmd, const char *back, uint8_t reply_via)
{
    char back_esc[1600];
    char json[1800];
    int n;

    su3_json_escape(back, back_esc, sizeof(back_esc));

    if (reply_via & SU3_CLI_REPLY_MQTT) {
        n = snprintf(json, sizeof(json),
                     "{\"id\":\"%s\",\"ts\":%d,\"type\":\"sensorCli\",\"cmd\":\"%s\",\"back\":\"%s\"}",
                     id, device_info->utc.time_stamp, cmd ? cmd : "", back_esc);
        if (n > 0 && (size_t)n < sizeof(json) && client != NULL) {
            esp_mqtt_client_publish(client, user_cli_data_publish_topic, json, strlen(json), 0, 0);
            if (cmd != NULL && strcmp(cmd, "list") == 0) {
                ESP_LOGI(TAG, "cli/put list id=%s bytes=%d", id, n);
            }
        } else if (n >= (int)sizeof(json)) {
            ESP_LOGW(TAG, "cli/put drop: json too long n=%d cmd=%s", n, cmd ? cmd : "");
        }
    }

    if (reply_via & SU3_CLI_REPLY_BLE) {
        n = snprintf(json, sizeof(json),
                     "{\"id\":\"%s\",\"ts\":%d,\"type\":17,\"cmd\":\"%s\",\"back\":\"%s\"}",
                     id, device_info->utc.time_stamp, cmd ? cmd : "", back_esc);
        if (n > 0 && (size_t)n < sizeof(json) && get_ble_status()) {
            (void)ble_notify_send((uint8_t *)json, (uint16_t)n);
        }
    }
}

/**
 * list → 只保留报告头行：`idx YYYY-MM-DD HH:MM:SS xxx`，单 \\n 分隔。
 * 去掉空行/\\r，便于 MQTT 一次回完。
 */
static void su3_format_list_headers(char *s)
{
    char *r;
    char *w;
    int prev_nl;

    if (s == NULL || s[0] == '\0') {
        return;
    }
    r = s;
    w = s;
    prev_nl = 1;
    while (*r != '\0') {
        char c = *r++;
        if (c == '\r') {
            if (*r == '\n') {
                r++;
            }
            c = '\n';
        }
        if (c == '\n') {
            if (prev_nl) {
                continue;
            }
            *w++ = '\n';
            prev_nl = 1;
            continue;
        }
        *w++ = c;
        prev_nl = 0;
    }
    while (w > s && *(w - 1) == '\n') {
        w--;
    }
    *w = '\0';
}

static void su3_cli_one_side(su3_side_t side, const char *cmd, char *rsp, size_t rsp_len, uint32_t timeout_ms)
{
    su3_addr_t dest = su3_side_to_addr(side);
    esp_err_t err;

    rsp[0] = '\0';
    if (strncmp(cmd, "report", 6) == 0 && (cmd[6] == '\0' || cmd[6] == ' ')) {
        char fire[24];
        snprintf(fire, sizeof(fire), "report %d", atoi(cmd + 6));
        err = su3_cli_fire(dest, fire);
        ESP_LOGI(TAG, "cli fire side=%d dest=0x%02X err=%s",
                 (int)side, dest, esp_err_to_name(err));
        strncpy(rsp, "ok", rsp_len - 1U);
        rsp[rsp_len - 1U] = '\0';
        return;
    }
    err = su3_cli_exec(dest, cmd, rsp, rsp_len, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cli_exec side=%d dest=0x%02X err=%s cmd=\"%s\"",
                 (int)side, dest, esp_err_to_name(err), cmd ? cmd : "");
        if (err == ESP_ERR_TIMEOUT) {
            strncpy(rsp, "timeout", rsp_len - 1U);
            rsp[rsp_len - 1U] = '\0';
        } else if (err == ESP_ERR_INVALID_STATE) {
            /* pending 已被占用（setup/另一路 CLI/补传等） */
            strncpy(rsp, "busy", rsp_len - 1U);
            rsp[rsp_len - 1U] = '\0';
        } else if (rsp[0] == '\0') {
            strncpy(rsp, "failed", rsp_len - 1U);
            rsp[rsp_len - 1U] = '\0';
        }
    } else if (cmd != NULL && strcmp(cmd, "list") == 0) {
        su3_format_list_headers(rsp);
        ESP_LOGI(TAG, "list headers side=%d len=%u", (int)side, (unsigned)strlen(rsp));
    }
}

#define SU3_CLI_Q_LEN        4
#define SU3_CLI_JOB_ID_LEN   24
#define SU3_JOB_CLI          0
#define SU3_JOB_SYNC         1
#define SU3_JOB_UPDATA       2  /* 传感器主动 list updata → cli/put */
/*
 * report 用 fire：传感器随后异步推 topic5~12。
 * 部分报告不完整、无可靠「发完」信号，固定等待 SU3_REPORT_SETTLE_MS 再拉下一条。
 * 与 5s/60s 实时 MQTT 并行发布，不做互斥（cli_worker 已串行执行 report）。
 */
#define SU3_REPORT_SETTLE_MS  15000U
/** setupLR=1/1 后再等一段时间才 list/补传，避免与对时/补 mode4 抢总线 */
#define SU3_REPORT_SYNC_SETTLE_MS  (30U * 1000U)
/** 左路 list/补传完成后再查右路，避免双侧同时推多份报告撑爆 sleep_up 队列 */
#define SU3_REPORT_SIDE_GAP_MS     (10U * 1000U)
#define SU3_REPORT_POLL_MS    (5U * 60U * 1000U)
#define SU3_REPORT_NAME_MAX   32
#define SU3_REPORT_LIST_MAX   30

typedef struct {
    uint8_t kind;
    uint8_t reply_via;   /* SU3_CLI_REPLY_MQTT / BLE */
    char mqtt_id[SU3_CLI_JOB_ID_LEN];
    char cmd[SU3_CLI_CMD_MAX];
} su3_cli_job_t;

static QueueHandle_t s_cli_q;
static volatile bool s_cli_job_running;
static TickType_t s_setup_both_tick;   /* 双侧 setup 完成时刻；0=未齐 */
static bool s_report_sync_deferred;    /* settle 期间有请求，到期后再跑 */
static void su3_report_sync_once(void);
static bool su3_setup_both_done(void);

/** 双侧 setup 状态变化时更新 settle 起点 */
static void su3_note_setup_progress(void)
{
    if (!su3_setup_both_done()) {
        s_setup_both_tick = 0;
        return;
    }
    if (s_setup_both_tick == 0) {
        s_setup_both_tick = xTaskGetTickCount();
        printf("setup: both ready, defer list/report %us\n",
               (unsigned)(SU3_REPORT_SYNC_SETTLE_MS / 1000U));
    }
}

static bool su3_report_sync_settle_ready(void)
{
    if (!su3_setup_both_done()) {
        return false;
    }
    if (s_setup_both_tick == 0) {
        su3_note_setup_progress();
    }
    return (xTaskGetTickCount() - s_setup_both_tick) >=
           pdMS_TO_TICKS(SU3_REPORT_SYNC_SETTLE_MS);
}

static void su3_report_settle(void)
{
    vTaskDelay(pdMS_TO_TICKS(SU3_REPORT_SETTLE_MS));
}

/** 真正执行 CLI；仅由 cli_worker 调用 */
static void su3_run_sensor_cli(const char *mqtt_id, const char *cmd, uint8_t reply_via)
{
    char rsp[SU3_CLI_RSP_DEFAULT];
    char side_id[24];
    bool do_left;
    bool do_right;
    bool any_ok = false;
    bool is_report;
    uint32_t timeout_ms = 2500;

    if (mqtt_id == NULL || cmd == NULL || !su3_is_ready()) {
        ESP_LOGW(TAG, "cli drop id=%s cmd=\"%s\" ready=%d",
                 mqtt_id ? mqtt_id : "null", cmd ? cmd : "null", (int)su3_is_ready());
        if (mqtt_id != NULL && cmd != NULL && reply_via != 0) {
            su3_cli_reply(mqtt_id, cmd, "su3 not ready", reply_via);
        }
        return;
    }

    is_report = (strncmp(cmd, "report", 6) == 0);
    /* list：多帧空闲约 100ms 即结束；总超时仅作兜底 */
    if (strcmp(cmd, "list") == 0) {
        timeout_ms = 1500;
    }

    do_left = (mqtt_id[8] == '0' || mqtt_id[8] == '3');
    do_right = (mqtt_id[8] == '0' || mqtt_id[8] == '6');
    if (is_report && cmd[6] == ' ') {
        if (do_left) {
            strncpy(s_json_report_name[SU3_SIDE_LEFT], cmd + 7,
                    sizeof(s_json_report_name[0]) - 1U);
            s_json_report_name[SU3_SIDE_LEFT][sizeof(s_json_report_name[0]) - 1U] = '\0';
        }
        if (do_right) {
            strncpy(s_json_report_name[SU3_SIDE_RIGHT], cmd + 7,
                    sizeof(s_json_report_name[0]) - 1U);
            s_json_report_name[SU3_SIDE_RIGHT][sizeof(s_json_report_name[0]) - 1U] = '\0';
        }
    }
    ESP_LOGI(TAG, "cli exec id=%s cmd=\"%s\" L=%d R=%d via=0x%x",
             mqtt_id, cmd, (int)do_left, (int)do_right, reply_via);

    if (do_left) {
        su3_make_side_id(side_id, sizeof(side_id), SU3_SIDE_LEFT);
        su3_cli_one_side(SU3_SIDE_LEFT, cmd, rsp, sizeof(rsp), timeout_ms);
        if (strncmp(rsp, "ok", 2) == 0) {
            any_ok = true;
        }
        if (strcmp(cmd, "version") == 0 && rsp[0] != '\0' &&
            strncmp(rsp, "timeout", 7) != 0 && strncmp(rsp, "busy", 4) != 0 &&
            strncmp(rsp, "failed", 6) != 0) {
            su3_sensor_ver_save(rsp);
        }
        su3_cli_reply(side_id, cmd, rsp, reply_via);
    }
    if (do_right) {
        su3_make_side_id(side_id, sizeof(side_id), SU3_SIDE_RIGHT);
        su3_cli_one_side(SU3_SIDE_RIGHT, cmd, rsp, sizeof(rsp), timeout_ms);
        if (strncmp(rsp, "ok", 2) == 0) {
            any_ok = true;
        }
        if (strcmp(cmd, "version") == 0 && rsp[0] != '\0' &&
            strncmp(rsp, "timeout", 7) != 0 && strncmp(rsp, "busy", 4) != 0 &&
            strncmp(rsp, "failed", 6) != 0) {
            su3_sensor_ver_save(rsp);
        }
        su3_cli_reply(side_id, cmd, rsp, reply_via);
    }

    if (is_report) {
        su3_report_settle();
    }
    if (strcmp(cmd, "reboot") == 0 && any_ok) {
        sensor_reboot_config();
    }
    if (strcmp(cmd, "set mode 0") == 0 && any_ok) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        su3_report_sync_request();
    }
}

static void su3_cli_worker(void *pv)
{
    su3_cli_job_t job;
    (void)pv;

    while (1) {
        if (xQueueReceive(s_cli_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* 升级期：丢弃队列中积压的 CLI/补传，避免与 port4 抢串口 */
        if (get_ota_now_flag() || su3_sensor_ota_busy()) {
            if (job.kind == SU3_JOB_CLI) {
                su3_cli_reply(job.mqtt_id, job.cmd, "ota busy", job.reply_via);
            }
            continue;
        }
        s_cli_job_running = true;
        if (job.kind == SU3_JOB_SYNC) {
            su3_report_sync_once();
        } else if (job.kind == SU3_JOB_UPDATA) {
            const char *payload = job.cmd;
            if (strncmp(payload, "list updata", 11) == 0) {
                payload += 11;
                while (*payload == ' ' || *payload == '\t') {
                    payload++;
                }
            }
            su3_cli_reply(job.mqtt_id, "list updata", payload, job.reply_via);
            ESP_LOGI(TAG, "cli/put list updata id=%s back=\"%s\"", job.mqtt_id, payload);
        } else {
            su3_run_sensor_cli(job.mqtt_id, job.cmd, job.reply_via);
        }
        s_cli_job_running = false;
    }
}

static void su3_cli_worker_start(void)
{
    if (s_cli_q != NULL) {
        return;
    }
    s_cli_q = xQueueCreate(SU3_CLI_Q_LEN, sizeof(su3_cli_job_t));
    if (s_cli_q == NULL) {
        ESP_LOGE(TAG, "cli queue create fail");
        return;
    }
    xTaskCreatePinnedToCore(su3_cli_worker, "su3_cli", 8192, NULL, 4, NULL, 1);
}

bool su3_stack_ready(void)
{
    return su3_is_ready();
}

const char *su3_get_sensor_fw_version(void)
{
    if (device_info == NULL) {
        return "";
    }
    return device_info->sensor_ver;
}

/** 传感器主动 list updata：入队后由 worker 发 MQTT cli/put（勿在 RX 里 publish） */
static void su3_enqueue_list_updata(int side, const char *text)
{
    su3_cli_job_t job;
    char side_id[24];
    uint8_t via = SU3_CLI_REPLY_MQTT;

    if (text == NULL || s_cli_q == NULL || side < 0 || side >= SU3_SENSOR_SIDE_MAX) {
        return;
    }
    if (get_ble_status()) {
        via |= SU3_CLI_REPLY_BLE;
    }
    memset(&job, 0, sizeof(job));
    job.kind = SU3_JOB_UPDATA;
    job.reply_via = via;
    su3_make_side_id(side_id, sizeof(side_id), side);
    strncpy(job.mqtt_id, side_id, sizeof(job.mqtt_id) - 1U);
    strncpy(job.cmd, text, sizeof(job.cmd) - 1U);
    if (xQueueSend(s_cli_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "list updata queue full, drop mqtt put");
    }
}

void su3_report_sync_request(void)
{
    su3_cli_job_t job;

    if (s_cli_q == NULL) {
        return;
    }
    /* setup 未齐或刚完成：先记一笔，settle 后再 list */
    if (!su3_report_sync_settle_ready()) {
        s_report_sync_deferred = true;
        return;
    }
    memset(&job, 0, sizeof(job));
    job.kind = SU3_JOB_SYNC;
    if (xQueueSend(s_cli_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "report sync queue full");
    }
}

static void su3_enqueue_sensor_cli(const char *mqtt_id, const char *cmd, uint8_t reply_via)
{
    su3_cli_job_t job;

    if (mqtt_id == NULL || cmd == NULL || s_cli_q == NULL) {
        return;
    }
    if (get_ota_now_flag() || su3_sensor_ota_busy()) {
        ESP_LOGW(TAG, "cli reject during OTA \"%s\"", cmd);
        su3_cli_reply(mqtt_id, cmd, "ota busy", reply_via);
        return;
    }
    memset(&job, 0, sizeof(job));
    job.kind = SU3_JOB_CLI;
    job.reply_via = reply_via;
    strncpy(job.mqtt_id, mqtt_id, sizeof(job.mqtt_id) - 1U);
    strncpy(job.cmd, cmd, sizeof(job.cmd) - 1U);
    if (xQueueSend(s_cli_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cli queue full, drop \"%s\"", cmd);
        su3_cli_reply(mqtt_id, cmd, "busy", reply_via);
    }
}

/** MQTT 回调入口：只入队，应答走 cli/put */
void su3_mqtt_handle_sensor_cli(const char *mqtt_id, const char *cmd)
{
    su3_enqueue_sensor_cli(mqtt_id, cmd, SU3_CLI_REPLY_MQTT);
}

/** BLE type:17 入口：只入队，应答走 BLE Notify */
void su3_ble_handle_sensor_cli(const char *mqtt_id, const char *cmd)
{
    su3_enqueue_sensor_cli(mqtt_id, cmd, SU3_CLI_REPLY_BLE);
}

int sensor_ota_bc(char *value)
{
    (void)value;
    return (su3_sensor_ota(su3_side_to_addr(SU3_SIDE_LEFT), NULL, 0) == ESP_OK) ? 0 : -1;
}

static uint32_t find_report_time(char *report_name, uint8_t len)
{
    uint32_t report_tim;
    struct tm stm;
    int iY, iM, iD, iH, iMin, iS;

    if (report_name == NULL || len < 23) {
        return 0;
    }
    memset(&stm, 0, sizeof(stm));
    iY = atoi(report_name + len - 23);
    iM = atoi(report_name + len - 18);
    iD = atoi(report_name + len - 15);
    iH = atoi(report_name + len - 12);
    iMin = atoi(report_name + len - 9);
    iS = atoi(report_name + len - 6);
    if (iY < 1900 || iM < 1 || iM > 12 || iD < 1 || iD > 31 ||
        iH < 0 || iH > 23 || iMin < 0 || iMin > 59 || iS < 0 || iS > 59) {
        return 0;
    }
    stm.tm_year = iY - 1900;
    stm.tm_mon = iM - 1;
    stm.tm_mday = iD;
    stm.tm_hour = iH;
    stm.tm_min = iMin;
    stm.tm_sec = iS;
    report_tim = (uint32_t)mktime(&stm);
    if (report_tim <= 1262304000UL || report_tim >= 2147483647UL) {
        return 0;
    }
    return report_tim;
}

/*
 * 睡眠报告同步：补传/发现新报告。
 * 左右各 list → 与 NVS(report_L/R) 比时间 → 落后则 fire report N → settle → 写 NVS。
 * 收包仍走 su3_on_topic topic5~12。
 */
static const char *su3_report_nvs_key(int side)
{
    return (side == SU3_SIDE_LEFT) ? "report_L" : "report_R";
}

static void su3_report_nvs_load(void)
{
    nvs_handle h;
    char buf[SU3_REPORT_NAME_MAX];
    size_t len;
    int i;

    for (i = 0; i < SU3_SENSOR_SIDE_MAX; i++) {
        strncpy(s_last_report[i], "NONE", sizeof(s_last_report[i]) - 1U);
    }
    if (nvs_open("config_cfg", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (i = 0; i < SU3_SENSOR_SIDE_MAX; i++) {
        len = sizeof(buf);
        memset(buf, 0, sizeof(buf));
        if (nvs_get_str(h, su3_report_nvs_key(i), buf, &len) == ESP_OK && buf[0] != '\0') {
            strncpy(s_last_report[i], buf, sizeof(s_last_report[i]) - 1U);
        }
    }
    nvs_close(h);
    strncpy(device_info->report, s_last_report[SU3_SIDE_LEFT], sizeof(device_info->report) - 1U);
    ESP_LOGI(TAG, "report cursor L=\"%s\" R=\"%s\"",
             s_last_report[SU3_SIDE_LEFT], s_last_report[SU3_SIDE_RIGHT]);
}

static void su3_report_nvs_save(int side, const char *name)
{
    nvs_handle h;

    if (side < 0 || side >= SU3_SENSOR_SIDE_MAX || name == NULL) {
        return;
    }
    strncpy(s_last_report[side], name, sizeof(s_last_report[side]) - 1U);
    s_last_report[side][sizeof(s_last_report[side]) - 1U] = '\0';
    if (side == SU3_SIDE_LEFT) {
        strncpy(device_info->report, name, sizeof(device_info->report) - 1U);
        device_info->report[sizeof(device_info->report) - 1U] = '\0';
    }
    if (nvs_open("config_cfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, su3_report_nvs_key(side), s_last_report[side]);
    (void)nvs_commit(h);
    nvs_close(h);
}

/** 解析 list 文本为报告名行，返回条数 */
static int su3_parse_list(const char *text, char names[][SU3_REPORT_NAME_MAX], int max_n)
{
    const char *p;
    int n = 0;

    if (text == NULL || names == NULL || max_n <= 0) {
        return 0;
    }
    if (strstr(text, "NONE") != NULL) {
        return 0;
    }
    p = text;
    while (*p != '\0' && n < max_n) {
        const char *eol;
        size_t len;

        while (*p == '\r' || *p == '\n' || *p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        eol = strchr(p, '\n');
        len = eol ? (size_t)(eol - p) : strlen(p);
        while (len > 0 && (p[len - 1U] == '\r' || p[len - 1U] == ' ')) {
            len--;
        }
        if (len >= SU3_REPORT_NAME_MAX) {
            len = SU3_REPORT_NAME_MAX - 1U;
        }
        if (len > 0) {
            memcpy(names[n], p, len);
            names[n][len] = '\0';
            n++;
        }
        p = eol ? (eol + 1) : (p + strlen(p));
    }
    return n;
}

static void su3_report_sync_side(int side)
{
    char list_rsp[SU3_CLI_RSP_DEFAULT];
    char names[SU3_REPORT_LIST_MAX][SU3_REPORT_NAME_MAX];
    char cmd[24];
    char side_id[24];
    char json[256];
    char dummy[8];
    int count;
    int i;
    uint32_t last_ts = 0;

    if (side < 0 || side >= SU3_SENSOR_SIDE_MAX) {
        return;
    }
    if (!su3_is_ready() || !g_su3_sensor[side].setup_done) {
        return;
    }

    memset(list_rsp, 0, sizeof(list_rsp));
    su3_cli_one_side((su3_side_t)side, "list", list_rsp, sizeof(list_rsp), 1500);
    count = su3_parse_list(list_rsp, names, SU3_REPORT_LIST_MAX);
    if (count <= 0) {
        return;
    }

    if (strcmp(s_last_report[side], "NONE") != 0) {
        last_ts = find_report_time(s_last_report[side], (uint8_t)strlen(s_last_report[side]));
    }

    su3_make_side_id(side_id, sizeof(side_id), side);
    for (i = 0; i < count; i++) {
        uint32_t ts = find_report_time(names[i], (uint8_t)strlen(names[i]));
        int idx;

        if (ts == 0) {
            continue;
        }
        if (last_ts != 0 && ts <= last_ts) {
            continue;
        }

        idx = atoi(names[i]);
        snprintf(cmd, sizeof(cmd), "report %d", idx);
        snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%d,\"type\":4,\"report\":\"%s\"}",
                 side_id, device_info->utc.time_stamp, names[i]);
        if (get_mqtt_status() && device_info->data_up_switch) {
            // ESP_LOGI(TAG, "mqtt put %s\n%s\n", user_sleep_data_publish_topic, json);
            esp_mqtt_client_publish(client, user_sleep_data_publish_topic, json, strlen(json), 0, 0);
        }

        ESP_LOGI(TAG, "report pull side=%d %s", side, names[i]);
        memset(s_json_report_name[side], 0, sizeof(s_json_report_name[side]));
        strncpy(s_json_report_name[side], names[i], sizeof(s_json_report_name[side]) - 1U);
        su3_cli_one_side((su3_side_t)side, cmd, dummy, sizeof(dummy), 1000);
        su3_report_settle();

        su3_report_nvs_save(side, names[i]);
        last_ts = ts;
    }

    /*
     * 部分 SU3 在 report 后停推 1s/60s；补发 mode4 尝试恢复实时（失败不阻断）。
     */
    {
        char rsp[32];
        su3_addr_t dest = su3_side_to_addr((su3_side_t)side);
        memset(rsp, 0, sizeof(rsp));
        if (su3_cli_exec(dest, "set mode 4", rsp, sizeof(rsp), 3000) == ESP_OK &&
            rsp[0] != '\0' && strncmp(rsp, "ok", 2) == 0) {
            printf("report sync: mode4 restore ok side=%d\n", side);
        } else {
            printf("report sync: mode4 restore fail side=%d\n", side);
        }
    }
}

/** setupLR 均为 1 后才允许 list/report 补传，避免与 setup 抢 CLI */
static bool su3_setup_both_done(void)
{
    return g_su3_sensor[SU3_SIDE_LEFT].setup_done &&
           g_su3_sensor[SU3_SIDE_RIGHT].setup_done;
}

bool su3_sensors_setup_ready(void)
{
    return su3_setup_both_done();
}

bool su3_cli_worker_busy(void)
{
    return s_cli_job_running;
}

static void su3_report_sync_once(void)
{
    if (!su3_is_ready() || !get_mqtt_status() || ota_now_flag) {
        return;
    }
    if (!su3_setup_both_done()) {
        ESP_LOGD(TAG, "report sync skip: setupLR=%d/%d",
                 (int)g_su3_sensor[0].setup_done,
                 (int)g_su3_sensor[1].setup_done);
        return;
    }
    su3_report_sync_side(SU3_SIDE_LEFT);
    vTaskDelay(pdMS_TO_TICKS(SU3_REPORT_SIDE_GAP_MS));
    su3_report_sync_side(SU3_SIDE_RIGHT);
}

static void su3_report_sync_task(void *pv)
{
    (void)pv;
    /* 等双侧 setup + settle，再开始 list/补传 */
    while (!su3_setup_both_done()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    while (!su3_report_sync_settle_ready()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "report sync armed (setupLR=1/1, settle %us)",
             (unsigned)(SU3_REPORT_SYNC_SETTLE_MS / 1000U));
    if (s_report_sync_deferred ||
        (get_mqtt_status() && get_5s_flag && device_info->utc.flag)) {
        s_report_sync_deferred = false;
        su3_report_sync_request();
    }
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SU3_REPORT_POLL_MS));
        if (s_report_sync_deferred ||
            (get_mqtt_status() && get_5s_flag && device_info->utc.flag)) {
            s_report_sync_deferred = false;
            su3_report_sync_request();
        }
    }
}

/**
 * 仅维护板端时间（time_stamp / 日历字段）。
 * 传感器 set rtc 只在 su3_setup_task，本任务不发 CLI。
 */
void utc_get_task(void *pv)
{
    (void)pv;

    while (device_info->utc.flag == false) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        printf("[UTC] SNTP 同步失败 or 未完成\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    printf("[UTC] SNTP ok, board clock refresh only\n");

    while (1) {
        time(&now);
        localtime_r(&now, &ti);
        if (now > 0) {
            device_info->utc.time_stamp = (uint32_t)now;
        }
        human_year = (uint16_t)(ti.tm_year + 1900);
        human_mon = (uint8_t)(ti.tm_mon + 1);
        device_info->utc.time[0] = (uint8_t)(human_year % 100U); /* 年低两位，数组为 uint8_t */
        device_info->utc.time[1] = human_mon;
        device_info->utc.time[2] = (uint8_t)ti.tm_mday;
        device_info->utc.time[3] = (uint8_t)ti.tm_hour;
        device_info->utc.time[4] = (uint8_t)ti.tm_min;
        device_info->utc.time[5] = (uint8_t)ti.tm_sec;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/*
// 函数：根据当前 g_Sync_TX.BcPlugInPacket 设置填充发送的数据包（如键值等）
// 参数：type：数据包类型
// 参数：keys：键值 pwm：缓启动速度 tmr：缓启动时间
// 返回：无
*/
void prepare_mfp_command(uint8_t type, uint32_t keys, uint8_t pwm, uint8_t tmr)
{
    // 清零发送数据包缓存
    memset(g_Sync_TX.rawData, 0, sizeof(g_Sync_TX.rawData));

    switch(type) {
        case KEYS_TYPE_NORMAL:
            printf("KEYS_TYPE_NORMAL\n");
            g_Sync_TX.PlugInPacket_Normal.length   = 0x05;  // 数据部分长度
            g_Sync_TX.PlugInPacket_Normal.type     = 0x01;  // 固定类型
            g_Sync_TX.PlugInPacket_Normal.keys     = keys;  // 设置不同的命令键值
            g_Sync_TX.PlugInPacket_Normal.ctrlMode = 0x00;
            g_Sync_TX.PlugInPacket_Normal.checksum = syncCalcCheckSum();
            break;

        case KEYS_TYPE_SOFT_START:
            printf("KEYS_TYPE_SOFT_START\n");
            g_Sync_TX.PlugInPacket_SlowStart.length   = 0x07;  // 数据部分长度
            g_Sync_TX.PlugInPacket_SlowStart.type     = 0x01;  // 固定类型
            g_Sync_TX.PlugInPacket_SlowStart.keys     = keys;  // 设置不同的命令键值
            g_Sync_TX.PlugInPacket_SlowStart.cmd      = 0x00;
            g_Sync_TX.PlugInPacket_SlowStart.pwm      = pwm;   // 设置传入的 pwm
            if(keys == KEY_HEAD_LOWER){
                g_Sync_TX.PlugInPacket_SlowStart.tmr = (tmr + 5 > 0xFF) ? 0xFF : (tmr + 5);  // 头部放下，时间增加5秒
            }else{
                g_Sync_TX.PlugInPacket_SlowStart.tmr  = tmr;  // 使用传入的 tmr
            }
            g_Sync_TX.PlugInPacket_SlowStart.checksum = syncCalcCheckSum();
            break;

        case KEYS_TYPE_MASSAGE:
            printf("KEYS_TYPE_MASSAGE\n");
            g_Sync_TX.PlugInPacket_MASSAGE.length   = 0x0a;     // 数据部分长度
            g_Sync_TX.PlugInPacket_MASSAGE.type     = 0x01;     // 固定类型
            g_Sync_TX.PlugInPacket_MASSAGE.keys     = keys;     // 设置不同的命令键值
            g_Sync_TX.PlugInPacket_MASSAGE.reserve1 = 0x0000;
            g_Sync_TX.PlugInPacket_MASSAGE.cmd      = 0x00;
            g_Sync_TX.PlugInPacket_MASSAGE.ctrcmd   = 0x00;
            g_Sync_TX.PlugInPacket_MASSAGE.reserve2 = 0x0000;
            g_Sync_TX.PlugInPacket_MASSAGE.checksum = syncCalcCheckSum();
            break;

        default:
            return;
    }
}


/*
* 打鼾干预/演示 MQTT上报
*/
void handle_snore_trigger(bool is_demo)
{
    static uint8_t send_attempts = 0;
    snore_parameters_t *params;
    if (is_demo) {
        params = &snore_parameters_demo;  // 使用演示参数
    } else {
        params = &device_info->snore->snore_parameters;  // 使用正常参数
    }

    // 如果未在干预中，则立即发送缓启动抬升命令,并记录时间
    if (!device_info->snore->snore_state.is_intervening) {
        /* 闹钟指令后 120s 内不执行新打鼾干预 */
        if (snore_blocked_until_s != 0) {
            uint32_t now_s = (uint32_t)(xTaskGetTickCount() / pdMS_TO_TICKS(1000));
            if (now_s < snore_blocked_until_s) {
                if (is_demo) {
                    device_info->snore->snore_state.triggered_flag_demo = false;
                } else {
                    device_info->snore->snore_state.triggered_flag = false;
                }
                return;
            }
        }

        printf("触发打鼾干预%s: triggered = %d, 正在干预: in_progress = %d \r\n", 
               is_demo ? "(演示)" : "", 
               is_demo ? device_info->snore->snore_state.triggered_flag_demo : device_info->snore->snore_state.triggered_flag, 
               device_info->snore->snore_state.is_intervening);

        prepare_mfp_command(KEYS_TYPE_SOFT_START, KEY_MEMORY4, params->pwm, params->tmr);
        mfp_dateSend();
        send_attempts++;
        if (send_attempts >= 3) {
            send_attempts = 0;
            device_info->snore->snore_state.is_intervening = true;
            device_info->snore->snore_state.last_triggered_time_s = device_info->utc.time_stamp;


            // 打鼾干预触发时，上报 MQTT 数据
            char send_json_value[512];
            memset(send_json_value, 0, sizeof(send_json_value));
            
            // 构建 JSON 字符串
            sprintf(send_json_value, "{\"id\":\"%s\",\"ts\":%d,\"type\":16,\"report\":\"snore_param\",\"data\":{"
                                      "\"triggered_flag\":%d,"
                                      "\"triggered_flag_demo\":%d,"
                                      "\"upHoldTime\":%d,"
                                      "\"threshold\":%d,"
                                      "\"threshold5s\":%d,"
                                      "\"pwm\":%d,"
                                      "\"tmr\":%d,"
                                      "\"lastTriggeredTime\":%d,"
                                      "\"isIntervening\":%d}}",             
                                        device_info->id,
                                        device_info->utc.time_stamp,
                                        device_info->snore->snore_state.triggered_flag,                  
                                        device_info->snore->snore_state.triggered_flag_demo,
                                        params->up_hold_time_s,
                                        params->threshold,
                                        params->threshold_5s,
                                        params->pwm,
                                        params->tmr,
                                        device_info->snore->snore_state.last_triggered_time_s,
                                        device_info->snore->snore_state.is_intervening);
            
            printf("上报打鼾干预数据: %s\n", send_json_value);

            // MQTT 发送
            if (get_mqtt_status()) {
                if (mqtt_send_mutex == true) {
                    mqtt_send_mutex = false;
                    esp_mqtt_client_publish(client, mc_cli_data_publish_topic, (char *)send_json_value, strlen((char *)send_json_value), 0, 0);
                    mqtt_send_mutex = true;
                }
            }
        }
    } else {
        // 如果已经触发，则等到冷却时间后再发送缓启动降下命令
        uint32_t up_hold_time = is_demo ? snore_parameters_demo.up_hold_time_s : device_info->snore->snore_parameters.up_hold_time_s;

        if ((device_info->utc.time_stamp - device_info->snore->snore_state.last_triggered_time_s) >= up_hold_time) {
            printf("打鼾干预结束%s: triggered = %d, 正在干预: in_progress = %d \r\n", 
                is_demo ? "(演示)" : "", 
                is_demo ? device_info->snore->snore_state.triggered_flag_demo : device_info->snore->snore_state.triggered_flag, 
                device_info->snore->snore_state.is_intervening);

            prepare_mfp_command(KEYS_TYPE_SOFT_START, KEY_ALLFATE, params->pwm, params->tmr);
            mfp_dateSend();

            // 清除打鼾事件标志，结束本次打鼾干预
            send_attempts++;
            if (send_attempts >= 3) {
                device_info->snore->snore_state.is_intervening = false;
                if (is_demo) {
                    device_info->snore->snore_state.triggered_flag_demo = false;
                } else {
                    device_info->snore->snore_state.triggered_flag = false;
                }
                send_attempts = 0;

                // 打鼾干预触发时，上报 MQTT 数据
                char send_json_value[512];
                memset(send_json_value, 0, sizeof(send_json_value));
                
                // 构建 JSON 字符串
                sprintf(send_json_value, "{\"id\":\"%s\",\"ts\":%d,\"type\":16,\"report\":\"snore_param\",\"data\":{"
                                        "\"triggered_flag\":%d,"
                                        "\"triggered_flag_demo\":%d,"
                                        "\"upHoldTime\":%d,"
                                        "\"threshold\":%d,"
                                        "\"threshold5s\":%d,"
                                        "\"pwm\":%d,"
                                        "\"tmr\":%d,"
                                        "\"lastTriggeredTime\":%d,"
                                        "\"isIntervening\":%d}}",             
                                        device_info->id,
                                        device_info->utc.time_stamp,
                                        device_info->snore->snore_state.triggered_flag,                  
                                        device_info->snore->snore_state.triggered_flag_demo,
                                        params->up_hold_time_s,
                                        params->threshold,
                                        params->threshold_5s,
                                        params->pwm,
                                        params->tmr,
                                        device_info->snore->snore_state.last_triggered_time_s,
                                        device_info->snore->snore_state.is_intervening);
                
                printf("上报打鼾干预数据(结束): %s\n", send_json_value);

                // MQTT 发送
                if (get_mqtt_status()) {
                    if (mqtt_send_mutex == true) {
                        mqtt_send_mutex = false;
                        esp_mqtt_client_publish(client, mc_cli_data_publish_topic, (char *)send_json_value, strlen((char *)send_json_value), 0, 0);
                        mqtt_send_mutex = true;
                    }
                }

            }
        }
    }
}

void syncPacket_to_hex_str(const void* packet, size_t size, char* out_buf, size_t buf_size) {
    const uint8_t* data = (const uint8_t*)packet;
    char* p = out_buf;

    for (size_t i = 0; i < size && (p - out_buf) < buf_size - 4; i++) {
        p += snprintf(p, buf_size - (p - out_buf), "%02X ", data[i]);
    }
    *p = '\0';
/*
        const uint8_t* data = (const uint8_t*)packet;
    char* p = out_buf;

    for (size_t i = 0; i < size && (p - out_buf) < buf_size - 3; i++) {
        p += snprintf(p, buf_size - (p - out_buf), "%02X", data[i]);  // 去掉 "%02X " 中的空格
    }
    *p = '\0';
*/
}

/*
* <设备-APP> 主控盒数据上报函数（蓝牙）
*/
void ble_up_controlbox_data(uint8_t up_type) {
    static char json_buf[512];
    static char hex_str[256];
    uint16_t raw_len = 0;
    
    // 从独立的解析缓冲区读取数据（带线程安全保护）
    if (g_MFP_Parsed_Mutex != NULL && 
        xSemaphoreTake(g_MFP_Parsed_Mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        
        raw_len = g_MFP_Parsed_Len;
        
        if (raw_len > 0 && raw_len <= sizeof(g_MFP_Parsed_Frame)) {
            syncPacket_to_hex_str(g_MFP_Parsed_Frame, raw_len, hex_str, sizeof(hex_str));
        } else {
            ESP_LOGW(TAG, "Invalid parsed packet length: %d", raw_len);
            xSemaphoreGive(g_MFP_Parsed_Mutex);
            return;
        }
        
        xSemaphoreGive(g_MFP_Parsed_Mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for reading parsed data");
        return;
    }

    int len = snprintf(json_buf, sizeof(json_buf),
        "{"
            "\"id\":\"%s\",\"ts\":%d,\"type\":12,"
            "\"data_hex\":\"%s\""
        "}",
        device_info->id, device_info->utc.time_stamp, hex_str);
    
    if (len < 0 || (size_t)len >= sizeof(json_buf)) {
        printf("JSON 构建失败或缓冲区不足\n");
        return;
    }

    // BLE上报（双连接：两台手机都能收到）
    if(get_ble_status()) {
        (void)ble_notify_send((uint8_t *)json_buf, (uint16_t)strlen(json_buf));
    }

    // MQTT上报（只在查询的时候上报）
    if(up_type == 1 && get_mqtt_status()) {
        if(mqtt_send_mutex == true) {
            mqtt_send_mutex = false;
            esp_mqtt_client_publish(client, mc_cli_data_publish_topic, (char *)json_buf, strlen((char *)json_buf), 0, 0);
            mqtt_send_mutex = true;
        }
        // printf("mqtt put hex_str: %s\n", hex_str);
    }
}

/*
* <主控盒-设备> 主控盒数据解析函数 （区分不同主控盒、收到主控盒数据后解析）
*/
void MFP_Cmd(uint8_t cmd, uint8_t up_type)
{
    //static uint32_t last_time = 0;
    //static uint32_t now = 0;
    //now = xTaskGetTickCount();
    // for(int i = 0;i<g_Sync_RX.syncPacket.length +3;i++){printf("%x ",g_Sync_RX.rawData[i]);} printf("\r\n");
    // printf("帧头: %d ,长度: %d ,sizeof(g_Sync_RX.syncPacket): %d \r \n",cmd,g_Sync_RX.syncPacket.length,sizeof(g_Sync_RX.syncPacket));
   
    // if((cmd == 0x07) && ((now - last_time >= pdMS_TO_TICKS(2000))))   // 200ms返回一次主控盒数据                // && (g_Sync_RX.syncPacket.length ==	sizeof(g_Sync_TX.syncPacket) - 3) // 不清楚这里为什么 34 ！= 39-3？
	if(cmd == 0x07) // 收到一帧后直接发送
    {
		ble_up_controlbox_data(up_type);// 蓝牙上报函数
        //last_time = now;
	}else if(cmd == 0x02)                   //  蓝牙查询主控盒数据：0x02  立即响应
    {   
        ble_up_controlbox_data(up_type);  // 蓝牙/mqtt都上报
        //last_time = now;
    }
}

void mfp_datareceive_handler(uint8_t *p, uint16_t len)
{
    static uint32_t count = 0;
    uint8_t checkSum = 0, checkSum1 = 0;
    const uint32_t MAX_BUF_SIZE = sizeof(g_Sync_RX.rawData);  // 1024字节

#if ENABLE_MFP_DETAIL_LOG
    static uint32_t rx_count = 0;
    printf("[RX#%u] recv %d bytes, total=%d\n", ++rx_count, len, count + len);
#endif

    // 缓冲区溢出保护（防御性编程）
    if (count + len > MAX_BUF_SIZE) {
        printf("[MFP_RX] ERROR: Buffer overflow! count=%d + len=%d > %d, reset\n", 
               count, len, MAX_BUF_SIZE);
        count = 0;
        memset(g_Sync_RX.rawData, 0, sizeof(g_Sync_RX.rawData));
        return;
    }

    memcpy(g_Sync_RX.rawData + count, p, len);
    count += len;

    // 判断是否至少收到了 "帧头+长度字段"
    if (count >= 2) {
        uint16_t frame_len = g_Sync_RX.Syncdata.length;

        // 长度范围判断，防止乱包
        if (frame_len > 80 || frame_len < 30) {
            printf("[MFP_RX] ERROR: Invalid frame_len=%d! Reset (probable stray byte)\n", frame_len);
            count = 0;
            memset(g_Sync_RX.rawData, 0, sizeof(g_Sync_RX.rawData));
            return;
        }

        // 收够了（长度+校验+帧头等）
        if (count >= frame_len + 3) {
            checkSum  = rxCalcCheckSum();
            checkSum1 = g_Sync_RX.Syncdata.data[frame_len];

            if (checkSum == checkSum1 && g_Sync_RX.rawData[0] != 0) {
                // 校验通过，保存到独立的解析缓冲区（带线程安全保护）
                if (g_MFP_Parsed_Mutex != NULL && 
                    xSemaphoreTake(g_MFP_Parsed_Mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_MFP_Parsed_Len = frame_len + 3;
                    memcpy(g_MFP_Parsed_Frame, g_Sync_RX.rawData, g_MFP_Parsed_Len);
                    xSemaphoreGive(g_MFP_Parsed_Mutex);
#if ENABLE_MFP_DETAIL_LOG
                   //  printf("[RX#%u] CRC OK, saved to parsed buffer, len=%d\n", rx_count, g_MFP_Parsed_Len);
#endif
                } else {
                    printf("[MFP_RX] WARNING: Failed to acquire mutex or mutex not initialized\n");
                }
                
                mfp_queue_pop_send();
            } else {
                printf("[MFP_RX] CRC FAIL! calc=%02X recv=%02X\n", checkSum, checkSum1);
            }
            
            // 清空接收缓冲区，为下一帧做准备
            count = 0;
            memset(g_Sync_RX.rawData, 0, sizeof(g_Sync_RX.rawData));
        }
#if ENABLE_MFP_DETAIL_LOG
        else {
            printf("[RX#%u] Wait more: %d < %d\n", rx_count, count, frame_len + 3);
        }
#endif
    }
}

#define MFP_RX_BUF_SIZE 256

typedef struct {
    uint8_t buf[MFP_RX_BUF_SIZE];
    uint16_t count;
} mfp_rx_buffer_t;

static mfp_rx_buffer_t rx_buf = {0};

static void MFP_DataReceive_task(void *arg)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config={
        .baud_rate = 38400,                // 波特率38400
        .data_bits = UART_DATA_8_BITS,                  // 8位数据位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,          // 无硬件流控制
        .parity = UART_PARITY_EVEN,                     // 偶校验
        .stop_bits = UART_STOP_BITS_1                   // 1位停止位
    };

    uart_event_t event;

    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 512, 20, &uart2_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));    // 设置接收超时，减少数据分帧（主控盒49字节需约12ms，设置为20个字符时间约5ms）
    ESP_ERROR_CHECK(uart_set_rx_timeout(ECHO_UART_PORT_NUM, 20));
    // Configure a temporary buffer for the incoming data
    uint8_t *dtmp  = (uint8_t *) malloc(BUF_SIZE);

    while (1)
    {
        //Waiting for UART event.
        if (xQueueReceive(uart2_queue, (void *)&event, (TickType_t)portMAX_DELAY)) 
        {
            // ESP_LOGI("UART", "UART event received: %d", event.type);
            memset(dtmp, 0, BUF_SIZE);
            //ESP_LOGI(TAG, "uart[%d] event:", ECHO_UART_PORT_NUM);
            switch (event.type)
            {
            //Event of UART receving data
            case UART_DATA:
#if DEBUG_MFP_UART_RX_LOG
                printf("MFP_U2 UART_DATA read_bytes=%d (buf_max=%d)\n", (int)event.size, BUF_SIZE);
#endif
                uart_read_bytes(ECHO_UART_PORT_NUM, dtmp, event.size, portMAX_DELAY);
                mfp_datareceive_handler(dtmp,event.size);   // 一次性收完一帧
               // ESP_LOGI(TAG, "[DATA EVT]:");
                //uart_write_bytes(ECHO_UART_PORT_NUM, (const char*) dtmp, event.size);
                break;
            //Event of HW FIFO overflow detected
            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "hw fifo overflow");
                // If fifo overflow happened, you should consider adding flow control for your application.
                // The ISR has already reset the rx FIFO,
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(ECHO_UART_PORT_NUM);
                xQueueReset(uart2_queue);
                break;
            //Event of UART ring buffer full
            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "ring buffer full");
                // If buffer full happened, you should consider increasing your buffer size
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(ECHO_UART_PORT_NUM);
                xQueueReset(uart2_queue);
                break;
            //Event of UART RX break detected
            case UART_BREAK:
                ESP_LOGI(TAG, "uart rx break");
                break;
            //Event of UART parity check error
            case UART_PARITY_ERR:
                ESP_LOGI(TAG, "uart parity error");
                // uart_flush_input(ECHO_UART_PORT_NUM);  // 清空当前错误帧
                break;
            //Event of UART frame error
            case UART_FRAME_ERR:
                ESP_LOGI(TAG, "uart frame error");
                break;
            //UART_PATTERN_DET
            case UART_PATTERN_DET:
                ESP_LOGI(TAG, "uart pattern detected");
                break;
            //Others
            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }            
    }
}

static void handle_snore_trigger_task(void *arg)
{
    while (1) 
    {       
 
        if (device_info->snore->snore_state.triggered_flag || device_info->snore->snore_state.triggered_flag_demo) // 处理打鼾干预（不重复发就不清 mfp_tx_ready） 
        {
            if (device_info->snore->snore_state.triggered_flag) {
                handle_snore_trigger(false);
            } else {
                // printf("demo triggerd \r\n");
                handle_snore_trigger(true);
            }
            g_system_flag.mfp_tx_ready = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}


/*
 *  解析MQTT/BLE 数据包
 *  data: AA010805010100000000f8XX
 */
int mqtt_ble_data_parser_cb(uint8_t *data)
{
    uint32_t key_value = 0;
    if (data[0] == 0xAA)    // 帧头判断
    {
        // 要加入和校验的（现在还没加）
        nvs_handle nvs_config_handler;
        char *send_json_value = NULL;
        // 解析
        switch (data[1])                            
        {
            case 0x00:
                if(data[4] == 0x01)     // 打鼾干预参数查询
                {
                    send_json_value = (char *)malloc(512);
                    if (send_json_value == NULL) {
                        printf("内存分配失败！\n");
                        return -1;
                    }
                    memset(send_json_value, 0, 512);
                    check_stack_space();
                    // 构建 JSON 字符串
                    sprintf(send_json_value, "{\"id\":\"%s\",\"ts\":%d,\"type\":16,\"report\":\"snore_param\",\"data\":{"
                                            "\"triggered_flag\":%d,"
                                            "\"triggered_flag_demo\":%d,"
                                            "\"upHoldTime\":%d,"
                                            "\"threshold\":%d,"
                                            "\"threshold5s\":%d,"
                                            "\"pwm\":%d,"
                                            "\"tmr\":%d,"
                                            "\"lastTriggeredTime\":%d,"
                                            "\"isIntervening\":%d}}",             
                                            device_info->id,
                                            device_info->utc.time_stamp,
                                            device_info->snore->snore_state.triggered_flag,                  
                                            device_info->snore->snore_state.triggered_flag_demo,
                                            device_info->snore->snore_parameters.up_hold_time_s,
                                            device_info->snore->snore_parameters.threshold,
                                            device_info->snore->snore_parameters.threshold_5s,
                                            device_info->snore->snore_parameters.pwm,
                                            device_info->snore->snore_parameters.tmr,
                                            device_info->snore->snore_state.last_triggered_time_s,
                                            device_info->snore->snore_state.is_intervening);
                    
                    printf("mqtt put打鼾干预参数: %s\n", send_json_value);

                    // MQTT 发送
                    if (get_mqtt_status()) {
                        if (mqtt_send_mutex == true) {
                            mqtt_send_mutex = false;
                            esp_mqtt_client_publish(client, mc_cli_data_publish_topic, (char *)send_json_value, strlen((char *)send_json_value), 0, 0);
                            mqtt_send_mutex = true;
                        }
                    }
                    if (send_json_value != NULL) {
                        free(send_json_value);
                    }
                }else if(data[4] == 0x02) // 主控盒参数查询
                {
                    // printf("查询主控盒参数\n");
                    MFP_Cmd(data[4],1);
                }else if(data[4] == 0x04) // 闹钟指令控制
                {
                    printf("闹钟指令控制 \r\n");
                    /* APP 下发闹钟指令后，120s 内禁止新的打鼾干预触发（不打断当前干预） */
                    snore_blocked_until_s = (uint32_t)(xTaskGetTickCount() / pdMS_TO_TICKS(1000)) + 120;
                }
                break;

            case 0x01:
                // printf("按键控制 \r\n");
                // 提取键值（注意是低字节在前）
                key_value = (data[5] << 24) | (data[6] << 16) | (data[7] << 8) | data[8];
                // printf("键值=0x%08X\n", key_value);
                // 判断是否为指定键值
                if (key_value == 0x00000040 || key_value == 0x00000008)
                {
                    // 清除打鼾干预状态
                    device_info->snore->snore_state.triggered_flag = false;
                    device_info->snore->snore_state.triggered_flag_demo = false;
                    device_info->snore->snore_state.is_intervening = false;
                    device_info->snore->snore_state.last_triggered_time_s = 0;
                    // printf("清除打鼾干预状态\n");
                }
                mfp_queue_push(&data[3], data[2], 2);       // 将数据放入队列
                break;

            case 0x02:
                printf("演示流程 \r\n");
                snore_parameters_demo.up_hold_time_s = ((uint32_t)data[5] << 24) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 8)  | ((uint32_t)data[8]);
                snore_parameters_demo.threshold_5s = data[9];
                snore_parameters_demo.threshold_5s = data[10];
                snore_parameters_demo.pwm = data[11];
                snore_parameters_demo.tmr = data[12];
                
                device_info->snore->snore_state.triggered_flag_demo = true;  // 演示触发标志
                printf("snore_demo: up_hold_time_s = %d, threshold_5s = %d, threshold = %d, pwm = %d, tmr = %d\n",snore_parameters_demo.up_hold_time_s,
                                                                                                snore_parameters_demo.threshold_5s,
                                                                                                snore_parameters_demo.threshold,
                                                                                                snore_parameters_demo.pwm,
                                                                                                snore_parameters_demo.tmr);
                            
                // handle_snore_trigger(device_info->snore->snore_state.triggered_flag_demo);
                break;
                
            case 0x03:
                printf("参数设置 \r\n");
                device_info->snore->snore_parameters.up_hold_time_s = ((uint32_t)data[5] << 24) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 8) | ((uint32_t)data[8]);
                device_info->snore->snore_parameters.threshold_5s  = data[9];
                device_info->snore->snore_parameters.threshold     = data[10];       //打鼾包阈值
                device_info->snore->snore_parameters.pwm           = data[11];
                device_info->snore->snore_parameters.tmr           = data[12];

                printf("snore: up_hold_time_s = %d, threshold_5s = %d, threshold = %d, pwm = %d, tmr = %d\n",device_info->snore->snore_parameters.up_hold_time_s,
                                                                                                            device_info->snore->snore_parameters.threshold_5s,
                                                                                                            device_info->snore->snore_parameters.threshold,
                                                                                                            device_info->snore->snore_parameters.pwm,
                                                                                                            device_info->snore->snore_parameters.tmr);
                                                                                                            
                ESP_ERROR_CHECK(nvs_open("config_cfg", NVS_READWRITE, &nvs_config_handler));                                                                         
                ESP_ERROR_CHECK(nvs_set_blob(nvs_config_handler, "snore_param",  &device_info->snore->snore_parameters, sizeof(snore_parameters_t)));
                ESP_ERROR_CHECK(nvs_commit(nvs_config_handler));
                nvs_close(nvs_config_handler);
                break;

            case 0x04:
            printf("停止命令 \r\n");
                mfp_tx_queue_clear();                       // 为了避免溢出导致未收到停止，清空队列
                mfp_queue_push(&data[3], data[2], 2);       // 将数据放入队列
                break;
            case 0x05:
            printf("停止命令 \r\n");
                mfp_tx_queue_clear();                       // 为了避免溢出导致未收到停止，清空队列
                // mfp_queue_push(&data[3], data[2], 2);       // 将数据放入队列
                break;
            default:
                return -2;
        }
    }
    // check_stack_space();
    return 0;
}




void mqtt_key_parser_task(void *pv)
{
    while (1)
    {
        if (xQueueReceive(device_info->mqtt_key->xQueue, device_info->mqtt_key->data_rec.value, portMAX_DELAY))
        {
            mqtt_ble_data_parser_cb(device_info->mqtt_key->data_rec.value);
        }
    }
    vTaskDelete(NULL);
}

void vlsit_task(void *pv){

    static char task_list_buf[1024];
    static char task_runtime_buf[1024];
    while(1)
    {

        vTaskList(task_list_buf);
        printf("Task Name\tStatus\tPrio\tStack\tTaskNum\n");
        printf("%s\n", task_list_buf);


        vTaskGetRunTimeStats(task_runtime_buf);
        printf("Task Name\tTime\t\tPercent\n");
        printf("%s\n", task_runtime_buf);
        vTaskDelay(30000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

#if SU3_USE_NEW_STACK
#define SU3_SETUP_CMD_TIMEOUT_MS 3000U
#define SU3_5S_SLOTS             5

/** 每侧缓存 5 个 1s 点，满则聚合成 /user/5s/put */
typedef struct {
    int32_t sensor_ts[SU3_5S_SLOTS];
    int32_t heart[SU3_5S_SLOTS];
    int32_t breath[SU3_5S_SLOTS];
    int32_t status[SU3_5S_SLOTS];
    int32_t sdata[SU3_5S_SLOTS];
    int32_t pdata[SU3_5S_SLOTS];
    int32_t sign[SU3_5S_SLOTS];
    uint8_t count;
} su3_5s_agg_t;

static su3_5s_agg_t s_5s_agg[SU3_SENSOR_SIDE_MAX];

static int su3_side_index(su3_addr_t src)
{
    su3_side_t side = su3_addr_to_side(src);
    return (side < SU3_SIDE_COUNT) ? (int)side : -1;
}

/** 整机 setup 完成后才拉 version；re-setup 时清零 */
static bool s_su3_version_cli_done;

static bool su3_any_hello_seen(void)
{
    return g_su3_sensor[SU3_SIDE_LEFT].hello_seen ||
           g_su3_sensor[SU3_SIDE_RIGHT].hello_seen;
}

static bool su3_any_data_seen(void)
{
    return g_su3_sensor[SU3_SIDE_LEFT].data_seen ||
           g_su3_sensor[SU3_SIDE_RIGHT].data_seen;
}

static bool su3_mod_need_setup(void)
{
    return g_su3_sensor[SU3_SIDE_LEFT].need_setup ||
           g_su3_sensor[SU3_SIDE_RIGHT].need_setup;
}

/** 作废整机初始化（hello 中途重上电 / OTA 后等） */
static void su3_module_invalidate_setup(const char *why)
{
    int i;

    for (i = 0; i < SU3_SENSOR_SIDE_MAX; i++) {
        g_su3_sensor[i].setup_done = false;
        g_su3_sensor[i].need_setup = true;
        /* 传感器已重启，旧 data_seen 不可再用于跳过 mode4 */
        g_su3_sensor[i].data_seen = false;
    }
    s_su3_version_cli_done = false;
    su3_note_setup_progress();
    printf("setup: module invalidate (%s)\n", why ? why : "");
}

static void su3_module_mark_setup_done(void)
{
    int i;

    for (i = 0; i < SU3_SENSOR_SIDE_MAX; i++) {
        g_su3_sensor[i].setup_done = true;
        g_su3_sensor[i].need_setup = false;
    }
    su3_note_setup_progress();
}

/**
 * KSSLBED00000002 → KSSL00002（固定前缀 KSSL + 设备号末 5 位）
 * MQTT/涂鸦仍用完整 device_id；仅传感器 set devid / CLI deviceID 用短号。
 */
static bool su3_make_short_devid(char *out, size_t out_len)
{
    const char *id;
    size_t n;

    if (out == NULL || out_len < 10U || device_info == NULL) {
        return false;
    }
    id = device_info->aliyun.device_id;
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    n = strlen(id);
    if (n < 5U) {
        return false;
    }
    snprintf(out, out_len, "KSSL%.5s", id + n - 5U);
    return true;
}

static void su3_on_hello(su3_addr_t src, void *user)
{
    int idx = su3_side_index(src);
    bool first;
    (void)user;
    if (idx < 0) {
        ESP_LOGW(TAG, "hello ignore addr=0x%02X (expect 0x33/0x36)", src);
        return;
    }
    /*
     * hello / mode / rtc / devid 是整机一次，不分左右。
     * 已 setup 完成后再收到 hello → SU3 中途掉电重上电，整机重新初始化。
     * 初始化过程中的重复 hello 不打断（传感器开机期约 500ms 周期）。
     */
    if (su3_setup_both_done()) {
        su3_module_invalidate_setup("hello after ready");
        g_su3_sensor[idx].hello_seen = true;
        ESP_LOGI(TAG, "hello side=%d src=0x%02X (module re-setup)", idx, src);
        return;
    }
    first = !g_su3_sensor[idx].hello_seen;
    g_su3_sensor[idx].hello_seen = true;
    if (!su3_mod_need_setup()) {
        g_su3_sensor[SU3_SIDE_LEFT].need_setup = true;
        g_su3_sensor[SU3_SIDE_RIGHT].need_setup = true;
    }
    if (first) {
        ESP_LOGI(TAG, "hello side=%d src=0x%02X (module setup)", idx, src);
        return;
    }
    ESP_LOGD(TAG, "hello side=%d src=0x%02X (dup, need_setup=%d)",
             idx, src, (int)su3_mod_need_setup());
}

static void su3_on_cli_push(su3_addr_t src, const char *text, const char *dev_id, void *user)
{
    int idx = su3_side_index(src);
    (void)user;
    if (idx < 0) {
        return;
    }
    if (dev_id != NULL && dev_id[0] != '\0') {
        strncpy(g_su3_sensor[idx].device_id, dev_id, sizeof(g_su3_sensor[idx].device_id) - 1);
    }
    if (text != NULL && strstr(text, "list updata") != NULL) {
        ESP_LOGI(TAG, "list updata side=%d src=0x%02X \"%s\"", idx, src, text);
        su3_enqueue_list_updata(idx, text);
        su3_report_sync_request();
    }
}

static void su3_publish_json(const char *topic, const char *json)
{
    if (topic == NULL || json == NULL) {
        return;
    }
    /* 断网时 wifi.flag 会先变 RECONNECTING，勿再进 MQTT write（易在 su3_rx 栈溢出） */
    if (get_wifi_status() == WIFI_STATUS_CONNECTED &&
        get_mqtt_status() && device_info->data_up_switch) {
        if (mqtt_send_mutex) {
            mqtt_send_mutex = false;
            // ESP_LOGI(TAG, "mqtt put %s\n%s\n", topic, json);
            esp_mqtt_client_publish(client, topic, json, strlen(json), 0, 0);
            mqtt_send_mutex = true;
        }
    }
    if (get_ble_status() && device_info->data_up_switch) {
        (void)ble_notify_send((uint8_t *)json, (uint16_t)strlen(json));
    }
}

/** 上报瞬间取板端 epoch；同时回写缓存供其它逻辑使用 */
static uint32_t su3_board_ts_now(void)
{
    time_t t = time(NULL);
    if (t > 0) {
        device_info->utc.time_stamp = (uint32_t)t;
        return (uint32_t)t;
    }
    return device_info->utc.time_stamp;
}

/**
 * 1s 实时数据仅经蓝牙 Notify 上报（type=16，与 UART topic16 对齐）。
 * 不走 MQTT，避免污染 /user/5s/put；MQTT 仍由 5 帧聚合 type=1 上报。
 */
static void su3_ble_publish_1s(int side, const qs_pb_msg_sensor_1sec_info *msg)
{
    char id[24];
    static char json[288]; /* RX 上下文，避免占 su3_rx 栈 */
    int n;

    if (msg == NULL || side < 0 || side >= SU3_SENSOR_SIDE_MAX) {
        return;
    }
    if (!get_ble_status() || !device_info->data_up_switch) {
        return;
    }

    su3_make_side_id(id, sizeof(id), side);
    n = snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%d,\"type\":16,"
                 "\"data\":{\"sensor_ts\":%d,\"heart\":%d,\"breath\":%d,"
                 "\"status\":%d,\"sdata\":%d,\"pdata\":%d,\"sign\":%d}}",
                 id, device_info->utc.time_stamp,
                 (int)msg->timestamp, (int)msg->heartbeat, (int)msg->breath_rate,
                 (int)msg->status, (int)msg->sdata, (int)msg->pdata, (int)msg->sign);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return;
    }
    /* type:16，id 为 03/06；向所有已连接手机 Notify */
    (void)ble_notify_send((uint8_t *)json, (uint16_t)n);
}

/** 收满 5 帧 1s → 上报 /user/5s/put（type=1，数组载荷） */
static void su3_publish_5s(int side)  /* 5s 数据上报云端*/
{
    su3_5s_agg_t *a = &s_5s_agg[side];
    char id[24];
    static char json[768]; /* RX 上下文，勿放栈上 */
    int n;
    uint32_t ts;

    if (a->count == 0) {
        return;
    }
    ts = su3_board_ts_now();
    /* 不足 5 帧的缺位保持 0（结构体初始/清零后即为 0） */
    su3_make_side_id(id, sizeof(id), side);
    /* 老字段兼容：sensor_ts/heart/breath 标量 + status[5]；1s 拼数组进 *_5s */
    n = snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%lu,\"type\":1,"
                 "\"data\":{\"sensor_ts\":%d,\"heart\":%d,\"breath\":%d,"
                 "\"status\":[%d,%d,%d,%d,%d],"
                 "\"sensor_ts_5s\":[%d,%d,%d,%d,%d],"
                 "\"heart_5s\":[%d,%d,%d,%d,%d],"
                 "\"breath_5s\":[%d,%d,%d,%d,%d],"
                 "\"sdata_5s\":[%d,%d,%d,%d,%d],"
                 "\"pdata_5s\":[%d,%d,%d,%d,%d],"
                 "\"sign_5s\":[%d,%d,%d,%d,%d]}}",
                 id, (unsigned long)ts,
                 a->sensor_ts[4], a->heart[4], a->breath[4],
                 a->status[0], a->status[1], a->status[2], a->status[3], a->status[4],
                 a->sensor_ts[0], a->sensor_ts[1], a->sensor_ts[2], a->sensor_ts[3], a->sensor_ts[4],
                 a->heart[0], a->heart[1], a->heart[2], a->heart[3], a->heart[4],
                 a->breath[0], a->breath[1], a->breath[2], a->breath[3], a->breath[4],
                 a->sdata[0], a->sdata[1], a->sdata[2], a->sdata[3], a->sdata[4],
                 a->pdata[0], a->pdata[1], a->pdata[2], a->pdata[3], a->pdata[4],
                 a->sign[0], a->sign[1], a->sign[2], a->sign[3], a->sign[4]);
    if (n > 0 && (size_t)n < sizeof(json)) {
        // printf("5s/put side=%d\n%s", side, json);
        su3_publish_json(user_5s_data_publish_topic, json);
    }
    memset(a, 0, sizeof(*a));
}

static void su3_feed_1s(int side, const qs_pb_msg_sensor_1sec_info *msg)
{
    su3_5s_agg_t *a;
    uint8_t i;

    if (msg == NULL || side < 0 || side >= SU3_SENSOR_SIDE_MAX) {
        return;
    }
    a = &s_5s_agg[side];
    i = a->count;
    if (i >= SU3_5S_SLOTS) {
        memset(a, 0, sizeof(*a));
        i = 0;
    }
    a->sensor_ts[i] = msg->timestamp;
    a->heart[i] = msg->heartbeat;
    a->breath[i] = msg->breath_rate;
    a->status[i] = msg->status;
    a->sdata[i] = msg->sdata;
    a->pdata[i] = msg->pdata;
    a->sign[i] = msg->sign;
    a->count = (uint8_t)(i + 1U);

    if (a->count >= SU3_5S_SLOTS) {
        su3_publish_5s(side);
    }
}

static void su3_publish_60s(int side, const qs_pb_msg_sensor_1min_info *msg)  /* 1min 数据上报云端*/
{
    char id[24];
    static char json[384]; /* RX 上下文，勿放栈上 */
    int n;
    uint32_t ts;

    if (msg == NULL) {
        return;
    }
    ts = su3_board_ts_now();
    su3_make_side_id(id, sizeof(id), side);
    n = snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%lu,\"type\":2,"
                 "\"data\":{\"sensor_ts\":%d,\"bed\":%d,\"heart\":%d,\"breath\":%d,"
                 "\"Mmin\":%d,\"Mmean\":%d,\"NSD\":%d,\"NPD\":%d,\"SBP\":%d,\"DBP\":%d}}",
                 id, (unsigned long)ts,
                 (int)msg->timestamp, msg->on_off_bed, msg->heartbeat, msg->breath_rate,
                 msg->Mmin, msg->Mmean, msg->NSD, msg->Npd, msg->SBP, msg->DBP);
    if (n > 0 && (size_t)n < sizeof(json)) {
        printf("60s/put side=%d\n%s", side, json);
        su3_publish_json(user_60s_data_publish_topic, json);
    }
}

static bool su3_log_report_raw(int side, uint8_t topic, const char *name,
                               const char *device_id, int32_t timestamp,
                               const uint8_t *data, int32_t data_len)
{
    (void)side;
    (void)topic;
    (void)name;
    (void)device_id;
    (void)timestamp;
    (void)data;
    if (data_len < 0 || data_len > QS_PB_RAW_DATA_LEGNTH) {
        return false;
    }
    return true;
}

static void su3_on_topic(su3_addr_t src, uint8_t topic, const uint8_t *pb_data, size_t pb_len, void *user)
{
    int idx = su3_side_index(src);
    (void)user;

    if (idx < 0 || pb_data == NULL) {
        return;
    }

    /* 报告 topic：解码前先打点，用于区分「传感器没发」还是「发了但后面丢/堵」 */
    if (topic >= SU3_TOPIC_SLEEP_CYCLE && topic <= SU3_TOPIC_DBP_RAW) {
        printf("su3 rx report topic=%u side=%d pb_len=%u\n",
               (unsigned)topic, idx, (unsigned)pb_len);
    }

    /* topic16：1s → 缓存，满 5 帧上报 5s/put */
    if (topic == SU3_TOPIC_SENSOR_1SEC) {
        qs_pb_msg_sensor_1sec_info msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_sensor_1sec_info_decode((char *)pb_data, pb_len, &msg) != QS_SUCCESS) {
            ESP_LOGW(TAG, "1s decode fail src=0x%02X len=%u", src, (unsigned)pb_len);
            return;
        }
        /* 打印 1s 数据 */
        // ESP_LOGI(TAG, "1s side=%d src=0x%02X id=%s ts=%d seq=%d status=0x%X heart=%d breath=%d sdata=%d pdata=%d sign=0x%X",
        //          idx, src, msg.device_id, (int)msg.timestamp, (int)msg.sequence,
        //          (unsigned)msg.status, (int)msg.heartbeat, (int)msg.breath_rate,
        //          (int)msg.sdata, (int)msg.pdata, (unsigned)msg.sign);

        g_su3_sensor[idx].info_1s = msg;
        g_su3_sensor[idx].data_seen = true; /* 有实时数据时可跳过卡住的 mode4 */
        if (!get_5s_flag) {
            get_5s_flag = true;
        }
        /* 左路同步旧缓存，供打鼾等本地逻辑 */
        if (idx == SU3_SIDE_LEFT && user_5s_sensor_info != NULL) {
            strncpy(user_5s_sensor_info->device_id, msg.device_id,
                    sizeof(user_5s_sensor_info->device_id) - 1);
            user_5s_sensor_info->timestamp = msg.timestamp;
            user_5s_sensor_info->sequence = msg.sequence;
            user_5s_sensor_info->heartbeat = msg.heartbeat;
            user_5s_sensor_info->breathRate = msg.breath_rate;
            memset(user_5s_sensor_info->status, 0, sizeof(user_5s_sensor_info->status));
            user_5s_sensor_info->status[0] = (msg.status & 0x04) ? 4 : ((msg.status & 0x01) ? 1 : 0);
        }

        su3_ble_publish_1s(idx, &msg); /* 蓝牙 1s type:16 */
        su3_feed_1s(idx, &msg);        /* 缓存，满 5 帧走 MQTT(+BLE) type:1 */
        return;
    }

    /* topic14：1min → 60s/put */
    if (topic == SU3_TOPIC_SENSOR_1MIN) {
        qs_pb_msg_sensor_1min_info msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_sensor_1min_info_decode((char *)pb_data, pb_len, &msg) != QS_SUCCESS) {
            return;
        }
        g_su3_sensor[idx].info_1min = msg;
        g_su3_sensor[idx].data_seen = true;
        if (idx == SU3_SIDE_LEFT && user_60s_sensor_info != NULL) {
            memcpy(user_60s_sensor_info, &msg, sizeof(msg));
        }
        su3_publish_60s(idx, &msg);
        return;
    }

    if (topic == SU3_TOPIC_SENSOR_SAE) {
        qs_pb_msg_sleep_apnea_info msg;
        char id[24];
        static char json[256]; /* RX 上下文 */
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_sleep_apnea_info_decode((char *)pb_data, pb_len, &msg) != QS_SUCCESS) {
            return;
        }
        g_su3_sensor[idx].info_sa = msg;
        su3_make_side_id(id, sizeof(id), idx);
        snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%d,\"type\":15,\"data\":{\"status_flag\":%d}}",
                 id, device_info->utc.time_stamp, msg.status_flag);
        su3_publish_json(user_sa_data_publish_topic, json);
        return;
    }

    /* 睡眠报告 topic 5~12：RX 只解码+入队，MQTT 由 sleep_up 任务发送 */
    if (topic == SU3_TOPIC_SLEEP_CYCLE) {
        qs_pb_msg_sleep_cycle_repo msg;
        static char send_json_value[1024]; /* RX 上下文，勿放栈上 */
        char id[24];
        int n;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_sleep_cycle_repo_decode((char *)pb_data, pb_len, &msg) != QS_SUCCESS) {
            ESP_LOGE(TAG, "sleep report topic5 decode fail side=%d", idx);
            return;
        }
        su3_make_side_id(id, sizeof(id), idx);
        n = snprintf(send_json_value, sizeof(send_json_value),
                     "{\"id\":\"%s\",\"ts\":%d,\"type\":5,\"report\":\"%s\","
                     "\"data\":{\"calResult\":%d,\"startTime\":%d,\"totalSleepTime\":%d,"
                     "\"sleepEfficiency\":%d,\"sleepQuality\":%d,\"turnoverTimes\":%d,"
                     "\"sleepLatency\":%d,\"offBedTimes\":%d,\"cRSD\":%d,\"slop1\":%d,"
                     "\"slop2\":%d,\"osaTimes\":%d,\"avgSA\":%d,\"maxSA\":%d}}",
                     id, device_info->utc.time_stamp, s_json_report_name[idx],
                     msg.cal_result, msg.start_time, msg.total_sleep_time,
                     msg.sleep_efficiency, msg.sleep_quality, msg.turnover_times,
                     msg.sleep_latency, msg.off_bed_times, msg.cRSD, msg.slop1,
                     msg.slop2, msg.oSA_times, msg.Ave_SA_time, msg.Longest_SA_time);
        if (n > 0 && (size_t)n < sizeof(send_json_value)) {
            (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_JSON, idx, 5,
                                       (const uint8_t *)send_json_value, (uint16_t)n);
        }
        return;
    }

    if (topic == SU3_TOPIC_STATE_RAW) {
        qs_pb_msg_state_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_state_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "sleep_state", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len) &&
                msg.data_len > 0) {
                (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_RAW, idx, topic,
                                           msg.data, (uint16_t)msg.data_len);
            }
        } else {
            ESP_LOGE(TAG, "[SU3_REPORT] topic=6 decode failed side=%d len=%u",
                     idx, (unsigned)pb_len);
        }
        return;
    }
    if (topic == SU3_TOPIC_HEART_RAW) {
        qs_pb_msg_heartbeat_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_heartbeat_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "heart_rate", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len) &&
                msg.data_len > 0) {
                (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_RAW, idx, topic,
                                           msg.data, (uint16_t)msg.data_len);
            }
        } else {
            ESP_LOGE(TAG, "[SU3_REPORT] topic=7 decode failed side=%d len=%u",
                     idx, (unsigned)pb_len);
        }
        return;
    }
    if (topic == SU3_TOPIC_BREATH_RAW) {
        qs_pb_msg_breathrate_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_breathrate_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "breath_rate_x10", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len) &&
                msg.data_len > 0) {
                (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_RAW, idx, topic,
                                           msg.data, (uint16_t)msg.data_len);
            }
        } else {
            ESP_LOGE(TAG, "[SU3_REPORT] topic=8 decode failed side=%d len=%u",
                     idx, (unsigned)pb_len);
        }
        return;
    }
    if (topic == SU3_TOPIC_MOVE_RAW) {
        qs_pb_msg_movement_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_movement_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "movement", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len) &&
                msg.data_len > 0) {
                (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_RAW, idx, topic,
                                           msg.data, (uint16_t)msg.data_len);
            }
        } else {
            ESP_LOGE(TAG, "[SU3_REPORT] topic=9 decode failed side=%d len=%u",
                     idx, (unsigned)pb_len);
        }
        return;
    }
    if (topic == SU3_TOPIC_SNORE_RAW) {
        qs_pb_msg_snore_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_snore_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "snore", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len) &&
                msg.data_len > 0) {
                (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_RAW, idx, topic,
                                           msg.data, (uint16_t)msg.data_len);
            }
        } else {
            ESP_LOGE(TAG, "[SU3_REPORT] topic=10 decode failed side=%d len=%u",
                     idx, (unsigned)pb_len);
        }
        return;
    }
    if (topic == SU3_TOPIC_SBP_RAW) {
        qs_pb_msg_sbp_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_sbp_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "sbp", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len) &&
                msg.data_len > 0) {
                (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_RAW, idx, topic,
                                           msg.data, (uint16_t)msg.data_len);
            }
        } else {
            ESP_LOGE(TAG, "[SU3_REPORT] topic=11 decode failed side=%d len=%u",
                     idx, (unsigned)pb_len);
        }
        return;
    }
    if (topic == SU3_TOPIC_DBP_RAW) {
        qs_pb_msg_dbp_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_dbp_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "dbp", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len) &&
                msg.data_len > 0) {
                (void)su3_sleep_up_enqueue(SU3_SLEEP_UP_KIND_RAW, idx, topic,
                                           msg.data, (uint16_t)msg.data_len);
            }
        } else {
            ESP_LOGE(TAG, "[SU3_REPORT] topic=12 decode failed side=%d len=%u",
                     idx, (unsigned)pb_len);
        }
        return;
    }

    ESP_LOGD(TAG, "topic=%u side=%d pb_len=%u ignored", topic, idx, (unsigned)pb_len);
}

/**
 * 整机初始化（hello / mode / rtc / devid 不分左右；实时与睡眠报告仍按 03/06）：
 * 1) 任意一侧 hello（或超时 probe）→ 整机 need_setup
 * 2) 经左地址(0x33)发 set mode 4 → set devid(短号) → 等 SNTP 后 set rtc
 * 3) 成功后左右 setup_done 同时置位；list/补传再等 SU3_REPORT_SYNC_SETTLE_MS
 * SU3 中途掉电重上电：setup 已完成后的 hello → 整机 invalidate 再走一遍。
 * ESP 单独重启且已有实时数据：可跳过卡住的 mode4，先 rtc，再 boost mode4；
 * data_seen 下 rtc 连续失败则软完成（用户无感）。
 */
#define SU3_SETUP_PROBE_DELAY_MS     15000U /* 冷启动后多等 hello（配合电源先关后开） */
#define SU3_SETUP_PROBE_MS           15000U /* 仍无 hello 再探间隔 */
#define SU3_SETUP_RTC_SOFT_FAIL_MAX  3U     /* data_seen 时 rtc 失败次数后软完成 */

static bool su3_rsp_is_ok(const char *rsp)
{
    return (rsp != NULL && strncmp(rsp, "ok", 2) == 0);
}

static void su3_fetch_version_after_setup(void)
{
    char ver[64];
    su3_addr_t dest;

    if (!su3_setup_both_done() || s_su3_version_cli_done) {
        return;
    }
    dest = su3_side_to_addr(SU3_SIDE_LEFT);
    memset(ver, 0, sizeof(ver));
    if (su3_cli_exec(dest, "version", ver, sizeof(ver),
                     SU3_SETUP_CMD_TIMEOUT_MS) == ESP_OK && ver[0] != '\0') {
        su3_sensor_ver_save(ver);
        ESP_LOGI(TAG, "sensor version cli=%s", device_info->sensor_ver);
        s_su3_version_cli_done = true;
    }
}

static void su3_setup_task(void *pv)
{
    TickType_t last_probe = 0;
    bool skipped_mode4 = false;
    uint8_t rtc_fail_cnt = 0;
    TickType_t boot_tick = xTaskGetTickCount();
    /* 初始化 CLI 一律走左业务地址；传感器内部不分左右 */
    const su3_addr_t dest = su3_side_to_addr(SU3_SIDE_LEFT);

    (void)pv;
    while (1) {
        char rsp[64];
        char rtc_cmd[40];
        char short_id[16];
        char devid_cmd[40];
        esp_err_t err;
        time_t now_ts;
        TickType_t now;

        if (su3_setup_both_done()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!su3_any_hello_seen()) {
            /* 主路径：等 hello；超时后低频 probe（补偿 ESP 单独重启、hello 丢失） */
            now = xTaskGetTickCount();
            if ((now - boot_tick) < pdMS_TO_TICKS(SU3_SETUP_PROBE_DELAY_MS)) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (last_probe != 0 &&
                (now - last_probe) < pdMS_TO_TICKS(SU3_SETUP_PROBE_MS)) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            last_probe = now;
            g_su3_sensor[SU3_SIDE_LEFT].need_setup = true;
            g_su3_sensor[SU3_SIDE_RIGHT].need_setup = true;
            ESP_LOGI(TAG, "setup probe module (no hello after %us)",
                     (unsigned)(SU3_SETUP_PROBE_DELAY_MS / 1000U));
        } else if (!su3_mod_need_setup() && !device_info->utc.flag) {
            /* 已 mode/devid，等 UTC */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (su3_mod_need_setup()) {
            err = su3_cli_exec(dest, "set mode 4", rsp, sizeof(rsp),
                               SU3_SETUP_CMD_TIMEOUT_MS);
            if (err == ESP_OK && su3_rsp_is_ok(rsp)) {
                skipped_mode4 = false;
            } else if (su3_any_data_seen()) {
                skipped_mode4 = true;
                printf("setup: skip mode4 (data_seen, err=%s)\n",
                       esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "setup mode fail err=%s rsp=\"%.32s\"",
                         esp_err_to_name(err), rsp);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            g_su3_sensor[SU3_SIDE_LEFT].hello_seen = true;

            /* 短 devid：KSSLBED00000002 → KSSL00002；跳过 mode4 时不强求 */
            if (!skipped_mode4 && su3_make_short_devid(short_id, sizeof(short_id))) {
                snprintf(devid_cmd, sizeof(devid_cmd), "set devid %s", short_id);
                err = su3_cli_exec(dest, devid_cmd, rsp, sizeof(rsp),
                                   SU3_SETUP_CMD_TIMEOUT_MS);
                if (err != ESP_OK || !su3_rsp_is_ok(rsp)) {
                    ESP_LOGW(TAG, "setup set devid fail id=%s rsp=\"%.32s\"",
                             short_id, rsp);
                } else {
                    su3_set_cli_device_id(short_id);
                    ESP_LOGI(TAG, "setup set devid ok %s (from %s)",
                             short_id, device_info->aliyun.device_id);
                }
            }

            g_su3_sensor[SU3_SIDE_LEFT].need_setup = false;
            g_su3_sensor[SU3_SIDE_RIGHT].need_setup = false;
            if (skipped_mode4) {
                printf("setup: mode skipped, try set rtc next\n");
            } else {
                ESP_LOGI(TAG, "setup mode ok (module)");
            }
        }

        if (!device_info->utc.flag) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        time(&now_ts);
        if (now_ts <= 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        device_info->utc.time_stamp = (uint32_t)now_ts;
        snprintf(rtc_cmd, sizeof(rtc_cmd), "set rtc %lu",
                 (unsigned long)device_info->utc.time_stamp);
        err = su3_cli_exec(dest, rtc_cmd, rsp, sizeof(rsp), SU3_SETUP_CMD_TIMEOUT_MS);
        if (err != ESP_OK || !su3_rsp_is_ok(rsp)) {
            ESP_LOGW(TAG, "setup rtc fail err=%s rsp=\"%.32s\"",
                     esp_err_to_name(err), rsp);
            if (su3_any_data_seen()) {
                rtc_fail_cnt++;
                if (rtc_fail_cnt >= SU3_SETUP_RTC_SOFT_FAIL_MAX) {
                    printf("setup: soft ok module (data_seen, rtc fail x%u)\n",
                           (unsigned)rtc_fail_cnt);
                    skipped_mode4 = false;
                    rtc_fail_cnt = 0;
                    su3_module_mark_setup_done();
                    su3_fetch_version_after_setup();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        rtc_fail_cnt = 0;

        if (skipped_mode4) {
            err = su3_cli_exec(dest, "set mode 4", rsp, sizeof(rsp),
                               SU3_SETUP_CMD_TIMEOUT_MS);
            if (err == ESP_OK && su3_rsp_is_ok(rsp)) {
                printf("setup: boost mode4 ok\n");
            } else {
                printf("setup: boost mode4 skip err=%s (keep setup)\n",
                       esp_err_to_name(err));
            }
            skipped_mode4 = false;
        }

        su3_module_mark_setup_done();
        ESP_LOGI(TAG, "setup ok (module)");
        su3_fetch_version_after_setup();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void su3_app_start(void)
{
    su3_config_t cfg = {
        .uart_port = UART_NUM_1,
        .tx_pin = UART1_TXD,
        .rx_pin = UART1_RXD,
        .baud_rate = 115200,
        .self_addr = SU3_ADDR_ESP32_DEFAULT,
        .addr_left = SU3_ADDR_LEFT_DEFAULT,
        .addr_right = SU3_ADDR_RIGHT_DEFAULT,
    };
    su3_handlers_t h = {
        .on_hello = su3_on_hello,
        .on_cli_push = su3_on_cli_push,
        .on_topic = su3_on_topic,
        .user = NULL,
    };

    if (su3_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "su3_init failed");
        return;
    }
    su3_set_handlers(&h);
    su3_cli_worker_start();  /* 启动 CLI 工作线程 */
    su3_sleep_up_start();    /* 睡眠报告 MQTT 异步上报，避免堵 RX */
    xTaskCreatePinnedToCore(su3_setup_task, "su3_setup", 4096, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "SU3 stack started");
}
#endif /* SU3_USE_NEW_STACK */

void app_control_server(void)
{
    // 初始化MFP队列
    mfp_tx_queue_init();
    
    // 创建MFP解析缓冲区的互斥锁（线程安全保护）
    g_MFP_Parsed_Mutex = xSemaphoreCreateMutex();
    if (g_MFP_Parsed_Mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create MFP parsed mutex!");
    }
    
    //ble 数据处理
    xTaskCreatePinnedToCore(ble_data_parser_task, "ble_task", 1024*6, NULL, 6, NULL, 1);   //1024*2//缩减1024*5
    //一键配网
    xTaskCreatePinnedToCore(one_key_config_wifi_task, "one_key_config_wifi_task", 1024*2, NULL, 6, NULL, 1);   //缩减1024*2
    
    su3_app_start();

    //utc 获取
    xTaskCreatePinnedToCore(utc_get_task, "utc_get", 1024*6, NULL, 3, NULL, 1);      //缩减4096*2   ok
    
    //系统控制任务（LED + 打鼾检测）- 量产必须保留
    xTaskCreatePinnedToCore(system_control_task, "sys_ctrl", 1024*2, NULL, 2, NULL, 1);
    
    su3_report_nvs_load();
    xTaskCreatePinnedToCore(su3_report_sync_task, "report_sync", 3072, NULL, 3, NULL, 1);

    xTaskCreate(MFP_DataReceive_task, "MFP_DataReceive_task", ECHO_TASK_STACK_SIZE, NULL, 10, NULL);     // mfp口的接收任务 1024*4

    xTaskCreate(handle_snore_trigger_task, "MFP_DataSend_task ", ECHO_TASK_STACK_SIZE, NULL, 7, NULL); // 打鼾干预任务 1024*4

    xTaskCreatePinnedToCore(mqtt_key_parser_task, "mqtt_key_parser_task", 1024*4, NULL, 5, NULL, 1);

    //调试显示任务 - 通过 ENABLE_DEBUG_DISPLAY 宏控制
#if ENABLE_DEBUG_DISPLAY
    xTaskCreatePinnedToCore(data_display_task, "data_display", 1024*4, NULL, 2, NULL, 1);
#endif
    // 性能监控任务 - 通过 ENABLE_PERFORMANCE_MON 宏控制
#if ENABLE_PERFORMANCE_MON
    xTaskCreatePinnedToCore(vlsit_task, "vlsit_task", 1024*4, NULL, 2, NULL, 1);  // 需要开启 FreeRTOS stats
#endif
}

void set_ota_now_flag(uint8_t data)
{
    ota_now_flag = data;
}
uint8_t get_ota_now_flag(void)
{
    return ota_now_flag;
}
void sensor_reboot_config(void)
{
    /* 传感器 reboot/板端 OTA 冷启动后：整机 re-setup，清 data_seen */
    int i;

    s_su3_version_cli_done = false;
    for (i = 0; i < SU3_SENSOR_SIDE_MAX; i++) {
        g_su3_sensor[i].setup_done = false;
        g_su3_sensor[i].data_seen = false;
    }
    if (su3_any_hello_seen()) {
        g_su3_sensor[SU3_SIDE_LEFT].need_setup = true;
        g_su3_sensor[SU3_SIDE_RIGHT].need_setup = true;
    }
    su3_note_setup_progress();
    printf("setup: sensor_reboot_config → module re-setup\n");
}

bool su3_device_cli_handle(const char *cmd, char *back, size_t back_len)
{
    if (cmd == NULL || back == NULL || back_len == 0) {
        return false;
    }
    if (strcmp(cmd, "su3GpioQuery") == 0) {
        su3_pwr_gpio_query(back, back_len);
        return true;
    }
    if (strcmp(cmd, "su3PowerOff") == 0) {
        su3_pwr_off();
        snprintf(back, back_len, "ok");
        return true;
    }
    if (strcmp(cmd, "su3PowerOn") == 0) {
        su3_pwr_on();
        snprintf(back, back_len, "ok");
        return true;
    }
    if (strcmp(cmd, "su3Enable") == 0) {
        su3_pwr_cycle();
        sensor_reboot_config();
        snprintf(back, back_len, "ok");
        return true;
    }
    return false;
}

static void check_stack_space(void)
{
    TaskHandle_t xTaskHandle = xTaskGetCurrentTaskHandle();
    UBaseType_t remainingStack = uxTaskGetStackHighWaterMark(xTaskHandle);

    printf("Remaining stack space: %u bytes\n", remainingStack);
}
