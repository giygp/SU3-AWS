/*
 * @Author: zhong chenjian zhongcj@softide.cn
 * @Date: 2021-10-07 23:03:40
 * @LastEditors: zhong chenjian zhongcj@softide.cn
 * @LastEditTime: 2022-06-22 00:01:41
 * @FilePath: /smart-air-bed-board-program/components/use_wifi/use_ota.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef USE_OTA_H_
#define USE_OTA_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

void advanced_ota_example_task(void *pvParameter);
void ota_start(void);

/** 云端 version 是否为传感器固件（SU2/SU3），走下载落盘而非板端分区 OTA */
bool ota_upgrade_is_sensor_fw(const char *version);
/** 启动传感器固件：版本比对 → HTTPS 下载 → port4 串口刷写 → 写 sensor_ver */
void sensor_ota_start(void);
/** 开机创建常驻 OTA 启动器任务（MQTT 指令到达时零内存触发） */
void ota_launcher_init(void);

/**
 * MQTT 连上时上报传感器当前版本（module=sensor）。
 * 云端需在产品下创建同名 OTA 模块，传感器包挂到该模块。
 */
void sensor_ota_report_version_on_mqtt(void);

bool sensor_ota_image_ready(void);
const uint8_t *sensor_ota_image_data(void);
size_t sensor_ota_image_len(void);
const char *sensor_ota_image_version(void);
void sensor_ota_image_free(void);

/** 切换 boot 分区：0=ota_0，1=ota_1，-1=当前非运行槽（OTA 回退推荐） */
#define OTA_ROLLBACK_SLOT_OTHER  (-1)

/** otaRollback 应答：回退前运行槽 / 回退目标槽及镜像版本 */
typedef struct {
    char running_part[16];
    char running_ver[32];
    char target_part[16];
    char target_ver[32];
} ota_rollback_result_t;

esp_err_t ota_rollback_to_partition(int slot, ota_rollback_result_t *result,
                                    char *out_msg, size_t out_len);
void ota_rollback_restart(void);
/** 上电打印当前运行分区、下次启动分区及镜像版本 */
void ota_log_boot_partition_info(void);
/** HTTPS 下载期间 MQTT 已 pause，GOT_IP 勿再 start（避免双 TLS） */
bool ota_mqtt_is_paused(void);

#define ca_root_cert "\
-----BEGIN CERTIFICATE-----\n\
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsFADA5MQswCQYD\n\
VQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6b24gUm9vdCBDQSAxMB4XDTE1\n\
MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTELMAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpv\n\
bjEZMBcGA1UEAxMQQW1hem9uIFJvb3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoC\n\
ggEBALJ4gHHKeNXjca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgH\n\
FzZM9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qwIFAGbHrQ\n\
gLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6VOujw5H5SNz/0egwLX0t\n\
dHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L93FcXmn/6pUCyziKrlA4b9v7LWIbxcce\n\
VOF34GfID5yHI9Y/QCB/IIDEgEw+OyQmjgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB\n\
/zAOBgNVHQ8BAf8EBAMCAYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3\n\
DQEBCwUAA4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDIU5PM\n\
CCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUsN+gDS63pYaACbvXy\n\
8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vvo/ufQJVtMVT8QtPHRh8jrdkPSHCa\n\
2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2\n\
xJNDd2ZhwLnoQdeXeGADbkpyrqXRfboQnoZsG4q5WTP468SQvvG5\n\
-----END CERTIFICATE-----\n\
" 
#endif