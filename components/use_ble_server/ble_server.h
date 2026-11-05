/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh
 * @LastEditTime: 2026-07-30
 */
#ifndef USE_BLE_SERVER_H_
#define USE_BLE_SERVER_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "esp_err.h"

/* Attributes State Machine */
enum
{
    IDX_SVC,
    IDX_CHAR_A,
    IDX_CHAR_VAL_A,
    IDX_CHAR_CFG_A,

    IDX_CHAR_B,
    IDX_CHAR_VAL_B,

    IDX_CHAR_C,
    IDX_CHAR_VAL_C,

    HRS_IDX_NB,
};

void initialize_ble_server(void);

/**
 * 向所有已连接手机发送 GATT Notify（多连接广播）。
 * 左右侧业务 id（03/06）由调用方在 JSON 中按 MQTT 协议填写。
 */
esp_err_t ble_notify_send(const uint8_t *data, uint16_t len);

/** 当前已连接手机数量 */
uint8_t ble_conn_count(void);

#endif
