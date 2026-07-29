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
#include <string.h>
#include "use_wifi.h"
#include "esp_wifi.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "sntp.h"
#include <time.h>
#include "use_uart.h"
#include "driver/uart.h"
#include "pb_decode.h"
#include "keesoncloud.pb.h"
#include "qs_protobuf.h"
#include "mqtt_client.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "use_mfp.h"
#include "su3_client.h"

// ==================== 调试配置开关 ====================
// 量产时将下面的宏改为 0，调试时改为 1
#define ENABLE_DEBUG_DISPLAY    0   // 1=启用调试显示任务  0=关闭（量产）
#define ENABLE_PERFORMANCE_MON  0   // 1=启用性能监控任务  0=关闭（量产）
#define ENABLE_MFP_DETAIL_LOG   0   // 1=启用MFP详细日志  0=仅错误日志（量产）
// ======================================================

#define UP_RATIO_60S    12   // 60s上报一次需改为12

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
extern qs_pb_msg_sleep_apnea_info *user_sa_sensor_info;
time_t now;
static uint16_t human_year;
static uint8_t human_mon;
struct tm ti;
bool get_5s_flag = false;
bool mqtt_send_mutex = true;


char json_report_name[32] = {0};

static char device_id[12] = {0},device_version[32] = {0},set_rtc_flag = 0;
static char ota_now_flag = 0;
static snore_parameters_t snore_parameters_demo = {0}; // 打鼾参数
static uint32_t snore_blocked_until_s = 0; // 闹钟指令后 120s 内禁止打鼾干预（开机秒数

#define ECHO_TEST_TXD (19)
#define ECHO_TEST_RXD (25)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (2)
#define ECHO_UART_BAUD_RATE     (38400)
#define ECHO_TASK_STACK_SIZE    (4096)

#define BUF_SIZE (512)

/* SU2(UART1)：0=关 1=每 chunk 打印 ring 前后长度 2=另打本 chunk 十六进制(最多 SU2_UART_HEX_DUMP_MAX) */
#define DEBUG_SU2_UART_RX_LOG   1
#define SU2_UART_HEX_DUMP_MAX   64
/* MFP(UART2) 事件接收：1=每次 UART_DATA 打印 read 长度（流量大，默认 0） */
#define DEBUG_MFP_UART_RX_LOG   0

static QueueHandle_t uart2_queue;

union SyncCommunicationData_t g_Sync_TX;
union SyncCommunicationData_t g_Sync_RX;
union keys_t   g_keys;
g_system_flag_t g_system_flag;

// MFP解析缓冲区 - 独立于接收缓冲区,避免数据被清零
static uint8_t g_MFP_Parsed_Frame[256];
static uint16_t g_MFP_Parsed_Len = 0;
static SemaphoreHandle_t g_MFP_Parsed_Mutex = NULL;

//蓝牙数据解析
int ble_data_parser_cb(uint8_t *data)
{

#if BLE_TEST
if(data[0] == 0x11 && data[1]==0x22 && data[2]==0x33)
{
    //bochuang_test(data[3],data[4]);
}
else if (data[0] == 0x22 && data[1]==0x33 && data[2]==0x44)
{
    memset(device_info->wifi.one_key_config.ssid, 0, 32);
    memcpy(device_info->wifi.one_key_config.ssid, MY_WIFI_SSID, strlen(MY_WIFI_SSID));
    printf("ssid = %s \n",device_info->wifi.one_key_config.ssid);

    memset(device_info->wifi.one_key_config.passwd, 0, 64);
    memcpy(device_info->wifi.one_key_config.passwd, MY_WIFI_PASSWD, strlen(MY_WIFI_PASSWD));
    printf("passwd = %s \n",device_info->wifi.one_key_config.passwd);

    if(get_mqtt_status())
    {
        esp_mqtt_client_stop(client);
        set_mqtt_status(0);
    }
    if (get_wifi_status())
    {
        ESP_ERROR_CHECK(esp_wifi_disconnect());
    }
    set_wifi_status(WIFI_STATUS_WAITING );  // 等待wifi连接成功

    set_one_key_config_wifi_status(1);
    ESP_ERROR_CHECK(esp_wifi_stop());
    memset(wifi_config.sta.ssid, 0, 32);
    memset(wifi_config.sta.password, 0, 64);
    memcpy(wifi_config.sta.ssid, (uint8_t *)device_info->wifi.one_key_config.ssid, strlen(device_info->wifi.one_key_config.ssid));
    memcpy(wifi_config.sta.password, (uint8_t *)device_info->wifi.one_key_config.passwd, strlen(device_info->wifi.one_key_config.passwd));
    printf("ssid = %s \n",wifi_config.sta.ssid);
    printf("passwd = %s \n",wifi_config.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_wifi_start());
}
else if (data[0] == 0x33 && data[1]==0x44 && data[2]==0x55)
{
    char temp[100] = {0};
    /* 传感器 OTA 暂未实现，仅回告 */
    (void)sensor_ota_bc(NULL);
    sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":5,\"sdate\":\"sensor OTA not supported yet\"}",
            device_info->id, device_info->utc.time_stamp);
    printf("sensorUpgrade = %s \n",temp);
    esp_ble_gatts_send_indicate(device_info->ble->gatts_if,
                                device_info->ble->conn_id,
                                device_info->ble->handle,
                                strlen((char *)temp), (uint8_t *)temp, false);
}

#endif

    {
        cJSON *firstItem = NULL;
        cJSON *sencondItem = NULL;
        cJSON *thirdItem = NULL;
        firstItem = cJSON_Parse((char *)data);
    if (firstItem)
    {
        sencondItem = cJSON_GetObjectItem(firstItem, "id");
        if(!sencondItem) return 0;
        // printf("id = %s\n", sencondItem->valuestring);
        if(strcmp(sencondItem->valuestring, device_info->id) != 0) return 0;
        sencondItem = cJSON_GetObjectItem(firstItem, "type");
        if(!sencondItem) return 0;
        if(sencondItem->valueint == 3)  // 配网
        {
            sencondItem = cJSON_GetObjectItem(firstItem, "cmd");
            if(!sencondItem) return 0;

            thirdItem = cJSON_GetObjectItem(sencondItem, "ssid");
            if(!thirdItem) return 0;
            memset(device_info->wifi.one_key_config.ssid, 0, 32);
            memcpy(device_info->wifi.one_key_config.ssid, thirdItem->valuestring, strlen(thirdItem->valuestring));
            printf("ssid = %s \n",device_info->wifi.one_key_config.ssid);

            thirdItem = cJSON_GetObjectItem(sencondItem, "passwd");
            if(!thirdItem) return 0;
            memset(device_info->wifi.one_key_config.passwd, 0, 64);
            memcpy(device_info->wifi.one_key_config.passwd, thirdItem->valuestring, strlen(thirdItem->valuestring));
            printf("passwd = %s \n",device_info->wifi.one_key_config.passwd);
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
            printf("ble set ssid = %s \n",wifi_config.sta.ssid);
            printf("ble set passwd = %s \n",wifi_config.sta.password);
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

            set_one_key_config_wifi_status(1);          // 参数已设置完成后再标记改变
            ESP_ERROR_CHECK(esp_wifi_stop());           // 停 WiFi 前，确保 config 数据已更新

            s_retry_num = 0;
            ESP_ERROR_CHECK(esp_wifi_start());          // 重启wifi模块
        }
        else if(sencondItem->valueint == 4)
        {
            sencondItem = cJSON_GetObjectItem(firstItem, "cmd");
            if(!sencondItem) return 0;

            
            char temp[100] = {0}; 
            if(strcmp(sencondItem->valuestring, "sensorVersion") == 0) 
            {
                sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":4,\"version\":\"%s\"}",
                                                        device_info->id,
                                                        device_info->utc.time_stamp,
                                                        device_version);
                
                printf("sensorVersion = %s \n",temp);
                esp_ble_gatts_send_indicate(device_info->ble->gatts_if,
                                            device_info->ble->conn_id,
                                            device_info->ble->handle,
                                            strlen((char *)temp), (uint8_t *)temp, false);
            }
        }
        else if(sencondItem->valueint == 5)
        {
            sencondItem = cJSON_GetObjectItem(firstItem, "cmd");
            if(!sencondItem) return 0;

            char temp[100] = {0};
            if(strcmp(sencondItem->valuestring, "sensorUpgrade") == 0)
            {
                (void)sensor_ota_bc(NULL);
                sprintf(temp,"{\"id\":\"%s\",\"ts\":%d,\"type\":5,\"sdate\":\"sensor OTA not supported yet\"}",
                        device_info->id, device_info->utc.time_stamp);
                printf("sensorUpgrade = %s \n",temp);
                esp_ble_gatts_send_indicate(device_info->ble->gatts_if,
                                            device_info->ble->conn_id,
                                            device_info->ble->handle,
                                            strlen((char *)temp), (uint8_t *)temp, false);
            }
        }else if(sencondItem->valueint == 12)
        {
            uint8_t cmd_bin[64] = {0}; 
            // printf("BLE: 12\n");
            sencondItem = cJSON_GetObjectItem(firstItem, "cmd");
            if(!sencondItem) return 0;
            // printf("BLE: sencondItem->valuestring = %s\n",sencondItem->valuestring);

            const char *cmd_str = sencondItem->valuestring;  
            int cmd_len = 0;
            // 每2个字符转换为一个字节
            while (*cmd_str && *(cmd_str + 1) && (cmd_len < 64)) {
                char byte_str[3] = { cmd_str[0], cmd_str[1], '\0' };
                cmd_bin[cmd_len++] = (uint8_t)strtol(byte_str, NULL, 16);
                cmd_str += 2;
            }
            // 将转换后的数据通过队列发送出去
            if (cmd_len > 0 && cmd_bin[0] == 0xAA) 
            {
                // printf("\nble_Rev:"); for(int i=0;i<cmd_len;i++){printf("%02X ",cmd_bin[i]);}printf("\n");

                mqtt_ble_data_parser_cb(cmd_bin);
            }else{
                printf("head error\n");
            }
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

            printf("%s\n",temp);
            if(get_ble_status())
            {
                esp_ble_gatts_send_indicate(device_info->ble->gatts_if,
                                                device_info->ble->conn_id,
                                                device_info->ble->handle,
                                                strlen(temp), (uint8_t *)temp, false);
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
            // printf("ble:%s\n",device_info->ble->data_rec.value);
            ble_data_parser_cb(device_info->ble->data_rec.value);
        }
    }
    vTaskDelete(NULL);
}

void report_to_aliyun(uint8_t type, uint8_t *value, uint16_t len)
{
    if (!value || len == 0) return;
    uint16_t one_packet_len = 200;   //256;  //128;    //64;   //修改
    uint16_t packet_num = len / one_packet_len;
    uint16_t i,j;
    cJSON *firstItem = NULL, *secondItem = NULL, *thirdItem = NULL;
    char *p_str = NULL;
    char *json_buff = malloc(1200);     //1024  //修改
    if((len % one_packet_len) != 0)
        packet_num ++;
    printf("%d packet to report_to_aliyun\n", packet_num);
    for(i = 0; i < packet_num; i++)
    {
        printf("i = %d\n",i);
        if((i == (packet_num - 1)) && ((len % one_packet_len) != 0))
        {
            printf("ii = %d\n",i);
            firstItem = cJSON_CreateObject();
            cJSON_AddItemToObject(firstItem,"id",cJSON_CreateString(device_info->id));
            cJSON_AddItemToObject(firstItem,"ts",cJSON_CreateNumber(device_info->utc.time_stamp));
            cJSON_AddItemToObject(firstItem,"type",cJSON_CreateNumber(type));
            cJSON_AddItemToObject(firstItem,"report",cJSON_CreateString(json_report_name));

            secondItem = cJSON_CreateObject();
            cJSON_AddItemToObject(secondItem,"len",cJSON_CreateNumber(len));
            cJSON_AddItemToObject(secondItem,"allPacket", cJSON_CreateNumber(packet_num));
            cJSON_AddItemToObject(secondItem,"packet", cJSON_CreateNumber(i));
            cJSON_AddItemToObject(secondItem,"packetLen",cJSON_CreateNumber(len % one_packet_len));
            
            thirdItem = cJSON_CreateArray();
            
            for(j = 0; j < len % one_packet_len; j ++)
            {
                cJSON_AddItemToArray(thirdItem, cJSON_CreateNumber(value[i*one_packet_len + j]));
            }
            cJSON_AddItemToObject(secondItem,"value",thirdItem);
            cJSON_AddItemToObject(firstItem,"data",secondItem);
            p_str = cJSON_Print(firstItem);
            if(p_str)
            {
                cJSON_Minify(p_str);
                printf("%s\n", p_str); //打印创建的字符串
                if(get_mqtt_status())
                {   
                    
                    if(mqtt_send_mutex == true)
                    {
                        mqtt_send_mutex = false;
                        esp_mqtt_client_publish(client, user_sleep_data_publish_topic, (char *)p_str, strlen((char *)p_str), 0, 0);
                        mqtt_send_mutex = true;                        
                    }
                    else
                    {
                        printf("mqtt_send_mutex1\n");
                    }
                }
                free(p_str);                      //一定要记得释放,不然会导致内存泄漏
                p_str = NULL;
            }
            else printf("p_str fail\n");
            cJSON_Delete(firstItem); 
            continue;
        }
        
        printf("i = %d\n",i);
        firstItem = cJSON_CreateObject();
        cJSON_AddItemToObject(firstItem,"id",cJSON_CreateString(device_info->id));
        cJSON_AddItemToObject(firstItem,"ts",cJSON_CreateNumber(device_info->utc.time_stamp));
        cJSON_AddItemToObject(firstItem,"type",cJSON_CreateNumber(type));
        cJSON_AddItemToObject(firstItem,"report",cJSON_CreateString(json_report_name));

        secondItem = cJSON_CreateObject();
        cJSON_AddItemToObject(secondItem,"len",cJSON_CreateNumber(len));
        cJSON_AddItemToObject(secondItem,"allPacket", cJSON_CreateNumber(packet_num));
        cJSON_AddItemToObject(secondItem,"packet", cJSON_CreateNumber(i));
        cJSON_AddItemToObject(secondItem,"packetLen",cJSON_CreateNumber(one_packet_len));
        
        thirdItem = cJSON_CreateArray();
        
        for(j = 0; j < one_packet_len; j ++)
        {
            cJSON_AddItemToArray(thirdItem, cJSON_CreateNumber(value[i*one_packet_len + j]));
        }

        cJSON_AddItemToObject(secondItem,"value",thirdItem);
        cJSON_AddItemToObject(firstItem,"data",secondItem);
        memset(json_buff, 0, 1200);     //1024
        cJSON_PrintPreallocated(firstItem, json_buff, 1200, 1);   //1024
        printf("%s\n", json_buff); //打印创建的字符串
        if(get_mqtt_status())
        {   
            if(mqtt_send_mutex == true)
            {
                mqtt_send_mutex = false;
                esp_mqtt_client_publish(client, user_sleep_data_publish_topic, (char *)json_buff, strlen((char *)json_buff), 0, 0);
                mqtt_send_mutex = true;                        
            }
            else
            {
                printf("mqtt_send_mutex2\n");
            }
        }
        
        cJSON_Delete(firstItem); 
    }
    free(json_buff);
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

        ESP_LOGI(TAG, "heap_size:%d,version:%s,id:%s,dataUpOn %d,sensorOta %d,current_report:%s",esp_get_free_heap_size(),device_info->ota.running_version,
                                                                                                device_info->id,device_info->data_up_switch,
                                                                                                0,
                                                                                                device_info->report);
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

/* CLI 命令组 JSON 字符串，并发送给云端 */
static void su3_cli_put(const char *id, const char *cmd, const char *back)
{
    char back_esc[1024];
    char json[1400];
    int n;

    su3_json_escape(back, back_esc, sizeof(back_esc));
    n = snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%d,\"type\":\"sensorCli\",\"cmd\":\"%s\",\"back\":\"%s\"}",
                 id, device_info->utc.time_stamp, cmd ? cmd : "", back_esc);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return;
    }
    printf("cli/put %s\n", json);
    if (client != NULL) {
        esp_mqtt_client_publish(client, user_cli_data_publish_topic, json, strlen(json), 0, 0);
    }
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
    }
}

#define SU3_CLI_Q_LEN        4
#define SU3_CLI_JOB_ID_LEN   24
#define SU3_JOB_CLI          0
#define SU3_JOB_SYNC         1
/*
 * report 用 fire：传感器随后异步推 topic5~12。
 * 部分报告不完整、无可靠「发完」信号，固定等待 SU3_REPORT_SETTLE_MS 再拉下一条。
 * 与 5s/60s 实时 MQTT 并行发布，不做互斥（cli_worker 已串行执行 report）。
 */
#define SU3_REPORT_SETTLE_MS  15000U
#define SU3_REPORT_POLL_MS    (5U * 60U * 1000U)
#define SU3_REPORT_NAME_MAX   32
#define SU3_REPORT_LIST_MAX   30

typedef struct {
    uint8_t kind;
    char mqtt_id[SU3_CLI_JOB_ID_LEN];
    char cmd[SU3_CLI_CMD_MAX];
} su3_cli_job_t;

static QueueHandle_t s_cli_q;
static void su3_report_sync_once(void);

static void su3_report_settle(void)
{
    vTaskDelay(pdMS_TO_TICKS(SU3_REPORT_SETTLE_MS));
}

/** 真正执行 CLI；仅由 cli_worker 调用 */
static void su3_mqtt_run_sensor_cli(const char *mqtt_id, const char *cmd)
{
    char rsp[SU3_CLI_RSP_DEFAULT];
    char side_id[24];
    bool do_left;
    bool do_right;
    bool any_ok = false;
    bool is_report;
    uint32_t timeout_ms = 1000;

    if (mqtt_id == NULL || cmd == NULL || !su3_is_ready()) {
        ESP_LOGW(TAG, "cli drop id=%s cmd=\"%s\" ready=%d",
                 mqtt_id ? mqtt_id : "null", cmd ? cmd : "null", (int)su3_is_ready());
        return;
    }

    is_report = (strncmp(cmd, "report", 6) == 0);
    if (strcmp(cmd, "list") == 0) {
        timeout_ms = 3000;
    }
    if (is_report && cmd[6] == ' ') {
        strncpy(json_report_name, cmd + 7, sizeof(json_report_name) - 1U);
        json_report_name[sizeof(json_report_name) - 1U] = '\0';
    }

    do_left = (mqtt_id[8] == '0' || mqtt_id[8] == '3');
    do_right = (mqtt_id[8] == '0' || mqtt_id[8] == '6');
    ESP_LOGI(TAG, "cli exec id=%s cmd=\"%s\" L=%d R=%d",
             mqtt_id, cmd, (int)do_left, (int)do_right);

    if (do_left) {
        su3_make_side_id(side_id, sizeof(side_id), SU3_SIDE_LEFT);
        su3_cli_one_side(SU3_SIDE_LEFT, cmd, rsp, sizeof(rsp), timeout_ms);
        if (strncmp(rsp, "ok", 2) == 0) {
            any_ok = true;
        }
        su3_cli_put(side_id, cmd, rsp);
    }
    if (do_right) {
        su3_make_side_id(side_id, sizeof(side_id), SU3_SIDE_RIGHT);
        su3_cli_one_side(SU3_SIDE_RIGHT, cmd, rsp, sizeof(rsp), timeout_ms);
        if (strncmp(rsp, "ok", 2) == 0) {
            any_ok = true;
        }
        su3_cli_put(side_id, cmd, rsp);
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
        if (job.kind == SU3_JOB_SYNC) {
            su3_report_sync_once();
        } else {
            su3_mqtt_run_sensor_cli(job.mqtt_id, job.cmd);
        }
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

void su3_report_sync_request(void)
{
    su3_cli_job_t job;

    if (s_cli_q == NULL) {
        return;
    }
    memset(&job, 0, sizeof(job));
    job.kind = SU3_JOB_SYNC;
    if (xQueueSend(s_cli_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "report sync queue full");
    }
}

/** MQTT 回调入口：只入队 */
void su3_mqtt_handle_sensor_cli(const char *mqtt_id, const char *cmd)
{
    su3_cli_job_t job;

    if (mqtt_id == NULL || cmd == NULL || s_cli_q == NULL) {
        return;
    }
    memset(&job, 0, sizeof(job));
    job.kind = SU3_JOB_CLI;
    strncpy(job.mqtt_id, mqtt_id, sizeof(job.mqtt_id) - 1U);
    strncpy(job.cmd, cmd, sizeof(job.cmd) - 1U);
    if (xQueueSend(s_cli_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cli queue full, drop \"%s\"", cmd);
        su3_cli_put(mqtt_id, cmd, "busy");
    }
}

int set_bc(uint32_t time_stamp, char *value, uint8_t switch_return,
           uint8_t switch_to_aliyun, char *return_value, uint16_t time_ms)
{
    char tmp[256];
    char rsp2[256];
    char *rsp = (return_value != NULL) ? return_value : tmp;

    (void)time_stamp;
    (void)switch_to_aliyun;

    if (!su3_is_ready() || value == NULL) {
        return -1;
    }

    /* 本地业务默认双侧（等同基座 id=00）；MQTT 侧别路由走 cli_worker */
    if (switch_return == 0) {
        (void)su3_cli_fire(su3_side_to_addr(SU3_SIDE_LEFT), value);
        (void)su3_cli_fire(su3_side_to_addr(SU3_SIDE_RIGHT), value);
        return 0;
    }

    su3_cli_one_side(SU3_SIDE_LEFT, value, rsp, sizeof(tmp), time_ms);
    su3_cli_one_side(SU3_SIDE_RIGHT, value, rsp2, sizeof(rsp2), time_ms);
    return (rsp[0] != '\0') ? 0 : -1;
}

int sensor_ota_bc(char *value)
{
    (void)value;
    return (su3_sensor_ota(su3_side_to_addr(SU3_SIDE_LEFT), NULL, 0) == ESP_OK) ? 0 : -1;
}

uint32_t find_report_time(char *report_name, uint8_t len)
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
static char s_last_report[SU3_SENSOR_SIDE_MAX][SU3_REPORT_NAME_MAX];

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
    su3_cli_one_side((su3_side_t)side, "list", list_rsp, sizeof(list_rsp), 3000);
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
        memset(json_report_name, 0, sizeof(json_report_name));
        strncpy(json_report_name, names[i], sizeof(json_report_name) - 1U);
        su3_cli_one_side((su3_side_t)side, cmd, dummy, sizeof(dummy), 1000);
        su3_report_settle();

        su3_report_nvs_save(side, names[i]);
        last_ts = ts;
    }
}

static void su3_report_sync_once(void)
{
    if (!su3_is_ready() || !get_mqtt_status() || ota_now_flag) {
        return;
    }
    su3_report_sync_side(SU3_SIDE_LEFT);
    su3_report_sync_side(SU3_SIDE_RIGHT);
}

static void su3_report_sync_task(void *pv)
{
    (void)pv;
    vTaskDelay(pdMS_TO_TICKS(20000));
    while (1) {
        if (get_mqtt_status() && get_5s_flag && device_info->utc.flag) {
            su3_report_sync_request();
        }
        vTaskDelay(pdMS_TO_TICKS(SU3_REPORT_POLL_MS));
    }
}

//utc 获取任务
void utc_get_task(void *pv)
{
    static uint8_t utc_first_flag = 1;
    static char time_stamp_to_string[20] = {0};
    static char set_rtc_cmd[100] = {0};
    static char return_value[100] = {0};  
    static char get_version_cmd[] = "version";  
    if(utc_first_flag)
    {
        utc_first_flag=0;

        /* SNTP/MQTT 已在 use_wifi GOT_IP → start_sntp_once → on_sntp_synced 中完成 */
        while (device_info->utc.flag == false) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
        if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
            printf("[UTC] SNTP 同步失败 or 未完成\n");
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            esp_restart();
        } else {
            printf("[UTC] SNTP 同步成功\n");
        }

        /* 等待至少一侧 SU3 setup（set mode + set rtc）完成 */
        while (!g_su3_sensor[SU3_SIDE_LEFT].setup_done &&
               !g_su3_sensor[SU3_SIDE_RIGHT].setup_done) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
        printf("[UTC] su3 setup 完成\n");

        if (set_rtc_flag == 0) {
            time(&now);
            localtime_r(&now, &ti);
            device_info->utc.time_stamp = (uint32_t)(mktime(&ti));
            itoa(device_info->utc.time_stamp, time_stamp_to_string, 10);
            sprintf(set_rtc_cmd, "set rtc %s", time_stamp_to_string);
            printf("[UTC] set_rtc_cmd = %s\n", set_rtc_cmd);
            set_bc(device_info->utc.time_stamp, set_rtc_cmd, 1, 0, return_value, 100);
            set_rtc_flag = 1;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            set_bc(device_info->utc.time_stamp, get_version_cmd, 1, 0, device_version, 100);
            printf("[UTC] get_version_cmd= %s\n", device_version);
        }
    }
    while(1)
    {
/**
调用sntp_init()会立刻请求服务器同步一次时间。
因此，我们需要主动同步时：
先调用sntp_stop()、再调用sntp_init() 即可立刻同步一次时间。
*/
        
        time(&now);
		
		localtime_r(&now, &ti);
        device_info->utc.time_stamp = (uint32_t)(mktime(&ti));

        human_year = ti.tm_year + 1900;
        human_mon  = ti.tm_mon + 1;

		// ti.tm_year = ti.tm_year + 1900;
		// ti.tm_mon = ti.tm_mon + 1;
		device_info->utc.time[0] = human_year;			//年
		device_info->utc.time[1] = human_mon;			//月
		device_info->utc.time[2] = ti.tm_mday ;			//日
		device_info->utc.time[3] = ti.tm_hour ;			//时
		device_info->utc.time[4] = ti.tm_min;			//分
		device_info->utc.time[5] = ti.tm_sec ;			//秒

        if(set_rtc_flag == 0)
        {
            while (!g_su3_sensor[SU3_SIDE_LEFT].setup_done &&
                   !g_su3_sensor[SU3_SIDE_RIGHT].setup_done)
            {
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }

            time(&now);
            localtime_r(&now, &ti);
            device_info->utc.time_stamp = (uint32_t)(mktime(&ti));
            itoa(device_info->utc.time_stamp, time_stamp_to_string, 10);
            memset(set_rtc_cmd,0,100);
            sprintf(set_rtc_cmd, "set rtc %s", time_stamp_to_string);
            printf("set_rtc_cmd\n");
            set_bc(device_info->utc.time_stamp, set_rtc_cmd, 1, 0, return_value, 20);
            set_rtc_flag = 1;
            vTaskDelay(100 / portTICK_PERIOD_MS);

            memset(device_version,0,32);
            set_bc(device_info->utc.time_stamp, get_version_cmd, 1, 0, device_version, 20);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            printf("bc version = %s\n",device_version);

        }

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}
void twd_task(void *arg)
{
    // 为TWDT添加任务，并检查dwt状态看是否添加
    // esp_task_wdt_add(NULL);
    // esp_task_wdt_status(NULL);
 
    while(1){
        // 每2秒重置一次看门狗
        //CHECK_ERROR_CODE(esp_task_wdt_reset(), ESP_OK);  // 喂狗。注释此行以测试触发TWDT超时
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL); 
}

void M_Ctr(uint8_t data);
#include "driver/gpio.h"
void key_task(void)
{
    static uint8_t s_oldkey_data = 0xff;
    uint8_t key_data;
    while(1)
    {
        key_data = gpio_get_level(13);
        if(s_oldkey_data!=key_data)
        {
            if(key_data)
            {
                M_Ctr(0);
            }else
            {
                M_Ctr(50);
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
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

    // BLE上报
    if(get_ble_status()) {
        esp_ble_gatts_send_indicate(device_info->ble->gatts_if,
                                    device_info->ble->conn_id,
                                    device_info->ble->handle,
                                    strlen((char *)json_buf), (uint8_t *)json_buf, false);
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

// 查找有效帧头
static int find_valid_header(const uint8_t *buf, uint16_t count, uint16_t *header_pos)
{
    for (uint16_t i = 0; i < count - 1; i++) {
        uint8_t length = buf[i];
        uint8_t type = buf[i + 1];
        
        // 检查是否为有效的帧头
        if (length >= 5 && length <= 100 && 
            (type == 0x01 || type == 0x02 || type == 0x07)) {
            *header_pos = i;
            return 1; // 找到有效帧头
        }
    }
    return 0; // 未找到有效帧头
}

static uint8_t calc_checksum(const uint8_t *data, uint16_t len)
{
    uint8_t sum = 0xFF;
    for (int i = 0; i < len; i++) sum -= data[i];
    return sum;
}

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

void wifi_check_task(void *pvParameters)
{
     bool need_reconnect = false;

    while (1)
    {
        need_reconnect = false;

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
            ESP_LOGW(TAG, "WiFi not connected.");
            need_reconnect = true;
        }

        tcpip_adapter_ip_info_t ip_info;
        tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
        if (ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "No IP address.");
            need_reconnect = true;
        }

        if (need_reconnect) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));  // 给点延时避免频繁重连
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(30000)); // 每 30 秒检查一次
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

static void su3_on_hello(su3_addr_t src, void *user)
{
    int idx = su3_side_index(src);
    (void)user;
    if (idx < 0) {
        ESP_LOGW(TAG, "hello ignore addr=0x%02X (expect 0x33/0x36)", src);
        return;
    }
    g_su3_sensor[idx].hello_seen = true;
    g_su3_sensor[idx].need_setup = true;   /* 待 set mode */
    g_su3_sensor[idx].setup_done = false;   /* 待 set rtc 完成后置位 */
    ESP_LOGI(TAG, "hello side=%d src=0x%02X", idx, src);
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
        if (device_id[0] == '\0') {
            strncpy(device_id, dev_id, sizeof(device_id) - 1);
            device_id[sizeof(device_id) - 1] = '\0';
        }
    }
    if (text != NULL && strstr(text, "list updata") != NULL) {
        su3_report_sync_request();
    }
}

static void su3_publish_json(const char *topic, const char *json)
{
    if (topic == NULL || json == NULL) {
        return;
    }
    if (get_mqtt_status() && device_info->data_up_switch) {
        if (mqtt_send_mutex) {
            mqtt_send_mutex = false;
            // ESP_LOGI(TAG, "mqtt put %s\n%s\n", topic, json);
            esp_mqtt_client_publish(client, topic, json, strlen(json), 0, 0);
            mqtt_send_mutex = true;
        }
    }
    if (get_ble_status() && device_info->data_up_switch) {
        esp_ble_gatts_send_indicate(device_info->ble->gatts_if,
                                    device_info->ble->conn_id,
                                    device_info->ble->handle,
                                    strlen(json), (uint8_t *)json, false);
    }
}

/** 收满 5 帧 1s → 上报 /user/5s/put（type=1，数组载荷） */
static void su3_publish_5s(int side)  /* 5s 数据上报云端*/
{
    su3_5s_agg_t *a = &s_5s_agg[side];
    char id[24];
    char json[640];
    int n;

    if (a->count == 0) {
        return;
    }
    /* 不足 5 帧的缺位保持 0（结构体初始/清零后即为 0） */
    su3_make_side_id(id, sizeof(id), side);
    /* ts=板端 UTC；sensor_ts=窗口内最后 1 条 1s 传感器时间（标量，非数组） */
    n = snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%d,\"type\":1,"
                 "\"data\":{\"sensor_ts\":%d,"
                 "\"heart\":[%d,%d,%d,%d,%d],"
                 "\"breath\":[%d,%d,%d,%d,%d],"
                 "\"status\":[%d,%d,%d,%d,%d],"
                 "\"sdata\":[%d,%d,%d,%d,%d],"
                 "\"pdata\":[%d,%d,%d,%d,%d],"
                 "\"sign\":[%d,%d,%d,%d,%d]}}",
                 id, device_info->utc.time_stamp, a->sensor_ts[4],
                 a->heart[0], a->heart[1], a->heart[2], a->heart[3], a->heart[4],
                 a->breath[0], a->breath[1], a->breath[2], a->breath[3], a->breath[4],
                 a->status[0], a->status[1], a->status[2], a->status[3], a->status[4],
                 a->sdata[0], a->sdata[1], a->sdata[2], a->sdata[3], a->sdata[4],
                 a->pdata[0], a->pdata[1], a->pdata[2], a->pdata[3], a->pdata[4],
                 a->sign[0], a->sign[1], a->sign[2], a->sign[3], a->sign[4]);
    if (n > 0 && (size_t)n < sizeof(json)) {
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
    char json[384];
    int n;

    if (msg == NULL) {
        return;
    }
    su3_make_side_id(id, sizeof(id), side);
    n = snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"ts\":%d,\"type\":2,"
                 "\"data\":{\"sensor_ts\":%d,\"bed\":%d,\"heart\":%d,\"breath\":%d,"
                 "\"Mmin\":%d,\"Mmean\":%d,\"NSD\":%d,\"NPD\":%d,\"SBP\":%d,\"DBP\":%d}}",
                 id, device_info->utc.time_stamp,
                 (int)msg->timestamp, msg->on_off_bed, msg->heartbeat, msg->breath_rate,
                 msg->Mmin, msg->Mmean, msg->NSD, msg->Npd, msg->SBP, msg->DBP);
    if (n > 0 && (size_t)n < sizeof(json)) {
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

    /* topic16：1s → 缓存，满 5 帧上报 5s/put */
    if (topic == SU3_TOPIC_SENSOR_1SEC) {
        qs_pb_msg_sensor_1sec_info msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_sensor_1sec_info_decode((char *)pb_data, pb_len, &msg) != QS_SUCCESS) {
            ESP_LOGW(TAG, "1s decode fail src=0x%02X len=%u", src, (unsigned)pb_len);
            return;
        }
/*      
        // 打印 1s 数据
        ESP_LOGI(TAG,"1s side=%d src=0x%02X id=%s ts=%d seq=%d status=0x%X ""heart=%d breath=%d sdata=%d pdata=%d sign=0x%X",
                 idx, src, msg.device_id, (int)msg.timestamp, (int)msg.sequence,
                 (unsigned)msg.status, (int)msg.heartbeat, (int)msg.breath_rate,
                 (int)msg.sdata, (int)msg.pdata, (unsigned)msg.sign);
*/
        g_su3_sensor[idx].info_1s = msg;
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

        su3_feed_1s(idx, &msg); /* 缓存 1s 数据 */
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
        if (idx == SU3_SIDE_LEFT && user_60s_sensor_info != NULL) {
            memcpy(user_60s_sensor_info, &msg, sizeof(msg));
        }
        su3_publish_60s(idx, &msg);
        return;
    }

    if (topic == SU3_TOPIC_SENSOR_SAE) {
        qs_pb_msg_sleep_apnea_info msg;
        char id[24];
        char json[256];
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

    /* 睡眠报告 topic 5~12（原 single_cmd_parse） */
    if (topic == SU3_TOPIC_SLEEP_CYCLE) {
        qs_pb_msg_sleep_cycle_repo msg;
        char send_json_value[1024];
        char id[24];
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_sleep_cycle_repo_decode((char *)pb_data, pb_len, &msg) != QS_SUCCESS) {
            ESP_LOGE(TAG, "sleep report topic5 decode fail side=%d", idx);
            return;
        }
        su3_make_side_id(id, sizeof(id), idx);
        snprintf(send_json_value, sizeof(send_json_value),
                 "{\"id\":\"%s\",\"ts\":%d,\"type\":5,\"report\":\"%s\","
                 "\"data\":{\"calResult\":%d,\"startTime\":%d,\"totalSleepTime\":%d,"
                 "\"sleepEfficiency\":%d,\"sleepQuality\":%d,\"turnoverTimes\":%d,"
                 "\"sleepLatency\":%d,\"offBedTimes\":%d,\"cRSD\":%d,\"slop1\":%d,"
                 "\"slop2\":%d,\"osaTimes\":%d,\"avgSA\":%d,\"maxSA\":%d}}",
                 id, device_info->utc.time_stamp, json_report_name,
                 msg.cal_result, msg.start_time, msg.total_sleep_time,
                 msg.sleep_efficiency, msg.sleep_quality, msg.turnover_times,
                 msg.sleep_latency, msg.off_bed_times, msg.cRSD, msg.slop1,
                 msg.slop2, msg.oSA_times, msg.Ave_SA_time, msg.Longest_SA_time);
        su3_publish_json(user_sleep_data_publish_topic, send_json_value);
        return;
    }

    if (topic == SU3_TOPIC_STATE_RAW) {
        qs_pb_msg_state_raw_data msg;
        memset(&msg, 0, sizeof(msg));
        if (qs_pb_state_raw_data_decode((char *)pb_data, pb_len, &msg) == QS_SUCCESS) {
            if (su3_log_report_raw(idx, topic, "sleep_state", msg.device_id,
                                   msg.timestamp, msg.data, msg.data_len)) {
                report_to_aliyun(topic, msg.data, (uint16_t)msg.data_len);
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
                                   msg.timestamp, msg.data, msg.data_len)) {
                report_to_aliyun(topic, msg.data, (uint16_t)msg.data_len);
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
                                   msg.timestamp, msg.data, msg.data_len)) {
                report_to_aliyun(topic, msg.data, (uint16_t)msg.data_len);
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
                                   msg.timestamp, msg.data, msg.data_len)) {
                report_to_aliyun(topic, msg.data, (uint16_t)msg.data_len);
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
                                   msg.timestamp, msg.data, msg.data_len)) {
                report_to_aliyun(topic, msg.data, (uint16_t)msg.data_len);
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
                                   msg.timestamp, msg.data, msg.data_len)) {
                report_to_aliyun(topic, msg.data, (uint16_t)msg.data_len);
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
                                   msg.timestamp, msg.data, msg.data_len)) {
                report_to_aliyun(topic, msg.data, (uint16_t)msg.data_len);
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
 * hello 后初始化（禁止在 RX 回调里 su3_cli_exec）：
 * 1) set mode 4 —— 收到 hello 即可发
 * 2) set devid  —— 仅左路(03)，写入主芯片 aliyun.device_id；双侧 CLI 共用
 * 3) set rtc   —— 等 SNTP(utc.flag) 就绪后发
 * 左右地址固定 0x33 / 0x36，不再 set addr。
 */
static void su3_setup_task(void *pv)
{
    (void)pv;
    while (1) {
        for (int i = 0; i < SU3_SENSOR_SIDE_MAX; i++) {
            su3_addr_t dest;
            char rsp[64];
            char rtc_cmd[40];
            char devid_cmd[48];
            esp_err_t err;
            time_t now_ts;
            struct tm ti_local;

            if (!g_su3_sensor[i].hello_seen || g_su3_sensor[i].setup_done) {
                continue;
            }

            dest = su3_side_to_addr((su3_side_t)i);

            if (g_su3_sensor[i].need_setup) {
                err = su3_cli_exec(dest, "set mode 4", rsp, sizeof(rsp),
                                   SU3_SETUP_CMD_TIMEOUT_MS);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "setup mode fail side=%d", i);
                    continue;
                }
                /* 只对 03(左) set devid；06 与左共用同一 CliCommand.deviceID */
                if (i == (int)SU3_SIDE_LEFT &&
                    device_info->aliyun.device_id[0] != '\0') {
                    snprintf(devid_cmd, sizeof(devid_cmd), "set devid %s",
                             device_info->aliyun.device_id);
                    err = su3_cli_exec(dest, devid_cmd, rsp, sizeof(rsp),
                                       SU3_SETUP_CMD_TIMEOUT_MS);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "setup set devid fail id=%s",
                                 device_info->aliyun.device_id);
                        continue;
                    }
                    su3_set_cli_device_id(device_info->aliyun.device_id);
                }
                g_su3_sensor[i].need_setup = false;
            }

            if (!device_info->utc.flag) {
                continue;
            }

            time(&now_ts);
            localtime_r(&now_ts, &ti_local);
            device_info->utc.time_stamp = (uint32_t)mktime(&ti_local);
            snprintf(rtc_cmd, sizeof(rtc_cmd), "set rtc %lu",
                     (unsigned long)device_info->utc.time_stamp);
            err = su3_cli_exec(dest, rtc_cmd, rsp, sizeof(rsp), SU3_SETUP_CMD_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "setup rtc fail side=%d", i);
                continue;
            }

            g_su3_sensor[i].setup_done = true;
            set_rtc_flag = 1;
            ESP_LOGI(TAG, "setup ok side=%d", i);
        }
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
    // xTaskCreatePinnedToCore(key_task, "key_task", 1024, NULL, 6, NULL, 1);//缩减2048*7

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

    // xTaskCreatePinnedToCore(wifi_check_task, "wifi_check_task", 1024*4, NULL, 5, NULL, 1); 
}

void set_ota_now_flag(uint8_t data)
{
    ota_now_flag = data;
}
uint8_t get_ota_now_flag(void)
{
    return ota_now_flag;
}
/** 至少一侧 setup 完成（供调试/兼容旧接口名） */
uint8_t get_devic_id_flag(void)
{
    return (g_su3_sensor[SU3_SIDE_LEFT].setup_done ||
            g_su3_sensor[SU3_SIDE_RIGHT].setup_done) ? 1U : 0U;
}
void sensor_reboot_config(void)
{
    int i;

    set_rtc_flag = 0;
    for (i = 0; i < SU3_SENSOR_SIDE_MAX; i++) {
        g_su3_sensor[i].setup_done = false;
        if (g_su3_sensor[i].hello_seen) {
            g_su3_sensor[i].need_setup = true;
        }
    }
}

void check_stack_space(void)
{
    TaskHandle_t xTaskHandle = xTaskGetCurrentTaskHandle();
    UBaseType_t remainingStack = uxTaskGetStackHighWaterMark(xTaskHandle);

    printf("Remaining stack space: %u bytes\n", remainingStack);
}
