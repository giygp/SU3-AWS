/*
 * @Author: zhong chenjian zhongcj@softide.cn
 * @Date: 2021-10-07 23:03:40
 * @LastEditors: zhong chenjian zhongcj@softide.cn
 * @LastEditTime: 2022-06-27 14:01:55
 * @FilePath: /smart-air-bed-board-program/components/use_wifi/use_wifi.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef USE_WIFI_H_
#define USE_WIFI_H_
#include <stdint.h>
#include "esp_wifi.h"

int s_retry_num;
wifi_config_t wifi_config;

void initialize_wifi(void);

/**
 * SNTP 时间同步成功后调用：用正确的 UTC 毫秒时间重新计算涂鸦 MQTT 签名并创建/启动客户端。
 * 若在 WiFi 初始化阶段签名，gettimeofday 未校正会导致 timestamp 错误、云端主动断连。
 */
void use_wifi_mqtt_init_and_start_after_time_sync(void);

#endif 