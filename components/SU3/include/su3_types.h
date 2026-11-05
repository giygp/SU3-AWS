/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh
 * @LastEditTime: 2026-07-14 18:19:35
 */
#ifndef SU3_TYPES_H_
#define SU3_TYPES_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 帧层常量（与 SU2/SU3 传感器 UART 协议一致） ---------- */
#define SU3_PREAMBLE_0          0xAAU
#define SU3_PREAMBLE_1          0x55U
#define SU3_MF_SINGLE           0x01U
#define SU3_MF_VERSION_MASK     0x0FU
#define SU3_MF_SEGMENT_BIT      0x10U
#define SU3_HEADER_SIZE         6U
#define SU3_TOPIC_SIZE          1U
#define SU3_CRC_SIZE            2U
#define SU3_FRAME_MIN_SIZE      8U
#define SU3_PDU_MAX_SIZE        160U
#define SU3_PDU_REASM_MAX       2112U
#define SU3_RING_BUF_SIZE       4096U
#define SU3_CLI_CMD_MAX         128U
#define SU3_CLI_RSP_DEFAULT     1024U

/* Topic（与 qs_protobuf / keesoncloud.proto 对齐） */
#define SU3_TOPIC_CLI           4U
#define SU3_TOPIC_SLEEP_CYCLE   5U
#define SU3_TOPIC_STATE_RAW     6U
#define SU3_TOPIC_HEART_RAW     7U
#define SU3_TOPIC_BREATH_RAW    8U
#define SU3_TOPIC_MOVE_RAW      9U
#define SU3_TOPIC_SNORE_RAW     10U
#define SU3_TOPIC_SBP_RAW       11U
#define SU3_TOPIC_DBP_RAW       12U
#define SU3_TOPIC_SENSOR_5SEC   13U
#define SU3_TOPIC_SENSOR_1MIN   14U
#define SU3_TOPIC_SENSOR_SAE    15U
#define SU3_TOPIC_SENSOR_1SEC   16U   /* SU3：1s 实时数据 */

/*
 * 帧内地址字节 = (port<<4)|addr（低 4bit=设备地址，高 4bit=端口）。
 * 业务端口固定 0x3；左固定 0x33、右固定 0x36；ESP32 本机 0x37。
 */
#define SU3_PORT_BIZ                0x03U
#define SU3_PORT_OTA                0x04U /* 固件升级裸二进制端口（非 protobuf） */
#define SU3_ADDR_PORT(addr, port)   ((uint8_t)((((port) & 0x0FU) << 4U) | ((addr) & 0x0FU)))
#define SU3_ADDR_OF(ap)             ((uint8_t)((ap) & 0x0FU))
#define SU3_PORT_OF(ap)             ((uint8_t)(((ap) >> 4U) & 0x0FU))
/** 业务地址 ↔ 升级地址（0x33→0x43，0x37→0x47） */
#define SU3_ADDR_TO_OTA(ap)         SU3_ADDR_PORT(SU3_ADDR_OF(ap), SU3_PORT_OTA)
#define SU3_ADDR_TO_BIZ(ap)         SU3_ADDR_PORT(SU3_ADDR_OF(ap), SU3_PORT_BIZ)

#define SU3_ADDR_ESP32_DEFAULT      SU3_ADDR_PORT(0x07U, SU3_PORT_BIZ) /* 0x37 */
#define SU3_ADDR_LEFT_DEFAULT       SU3_ADDR_PORT(0x03U, SU3_PORT_BIZ) /* 0x33 */
#define SU3_ADDR_RIGHT_DEFAULT      SU3_ADDR_PORT(0x06U, SU3_PORT_BIZ) /* 0x36 */

typedef enum {
    SU3_SIDE_LEFT  = 0,
    SU3_SIDE_RIGHT = 1,
    SU3_SIDE_COUNT = 2,
} su3_side_t;

typedef enum {
    SU3_RESP_SINGLE = 0,      /* version / set mode 等短应答 */
    SU3_RESP_MULTIFRAME,      /* list：多帧/空闲超时拼完 */
    SU3_RESP_FIRE_FORGET,     /* report：只发不等 CLI 文本 */
} su3_resp_kind_t;

/** 传感器侧逻辑地址（帧内 src/dst 低位地址） */
typedef uint8_t su3_addr_t;

typedef struct {
    uart_port_t uart_port;    /* 默认 UART_NUM_1 */
    int tx_pin;               /* 默认 22 */
    int rx_pin;               /* 默认 26 */
    int baud_rate;            /* 默认 115200，以硬件实测为准 */
    uint8_t self_addr;        /* ESP32 本机地址，默认 0x37 */
    uint8_t addr_left;        /* 左侧传感器，默认 0x33 */
    uint8_t addr_right;       /* 右侧传感器，默认 0x36 */
    char device_id[24];       /* CliCommand.deviceID，可空，后续 hello 填充 */
} su3_config_t;

/**
 * 被动 CLI（topic=4 且无 pending）：hello / list updata 等
 * @param src        帧源地址（用于区分左右）
 * @param text       CliCommand.command 文本
 * @param device_id  CliCommand.deviceID，可能为空串
 */
typedef void (*su3_cli_push_cb_t)(su3_addr_t src, const char *text,
                                  const char *device_id, void *user);

/**
 * 异步 topic 数据（1s/1min/sa/report raw 等）
 * @param src       帧源地址
 * @param topic     topic id
 * @param pb_data   protobuf 载荷（不含 topic 字节）
 * @param pb_len    载荷长度
 */
typedef void (*su3_topic_cb_t)(su3_addr_t src, uint8_t topic,
                               const uint8_t *pb_data, size_t pb_len, void *user);

/**
 * hello 便捷回调（也可只用 cli_push 判断 "hello"）
 */
typedef void (*su3_hello_cb_t)(su3_addr_t src, void *user);

typedef struct {
    su3_cli_push_cb_t on_cli_push;
    su3_topic_cb_t    on_topic;
    su3_hello_cb_t    on_hello;
    void             *user;
} su3_handlers_t;

#ifdef __cplusplus
}
#endif

#endif /* SU3_TYPES_H_ */
