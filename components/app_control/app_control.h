/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh 
 * @LastEditTime: 2026-07-14 18:19:35
 */
#ifndef APP_CONTROL_H_
#define APP_CONTROL_H_
#include <stdio.h>
#include <stdlib.h>
#include "common.h"

void app_control_server(void);
/** MQTT id 是否属于本机（仅侧别码 00/03/06 可不同） */
bool su3_mqtt_id_belong(const char *mqtt_id);
/** sensorCli：MQTT 回调只入队；cli_worker 内执行并 cli/put */
void su3_mqtt_handle_sensor_cli(const char *mqtt_id, const char *cmd);
/** BLE type:17 sensorCli：入队执行，应答经 BLE Notify（type:17） */
void su3_ble_handle_sensor_cli(const char *mqtt_id, const char *cmd);
/** SU3 协议栈是否已启动（可下发 CLI） */
bool su3_stack_ready(void);
/** 缓存的传感器 version 应答（如 SU2-2.0.2,Board:2.0），未拉到则为空串 */
const char *su3_get_sensor_fw_version(void);
/** 成功读到 version / 传感器 OTA 成功后写入 NVS sensor_ver */
void su3_sensor_ver_save(const char *ver);
void set_ota_now_flag(uint8_t data);
uint8_t get_ota_now_flag(void);
/** 双侧传感器 setup 是否已完成（OTA / 补传门禁） */
bool su3_sensors_setup_ready(void);
/** CLI 工作线程是否正在执行 list/补传/sensorCli（OTA 刷写前应等其结束） */
bool su3_cli_worker_busy(void);
/** 请求睡眠报告同步（入队 cli_worker；周期任务 / MQTT 重连 / list updata 调用） */
void su3_report_sync_request(void);
void sensor_reboot_config(void);
/** deviceCli SU3 供电：su3GpioQuery / su3PowerOff / su3PowerOn / su3Enable */
bool su3_device_cli_handle(const char *cmd, char *back, size_t back_len);
int sensor_ota_bc(char *value);
extern bool mqtt_send_mutex;
int mqtt_ble_data_parser_cb(uint8_t *data);
#endif
