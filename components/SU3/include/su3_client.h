/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh
 * @LastEditTime: 2026-07-14 18:19:35
 */
/**
 * SU3 传感器通信对外唯一入口。
 * WiFi / BLE / app_control 业务层只依赖本头文件，不直接碰 UART 帧格式。
 *
 * 分层：su3_frame / su3_crc → su3_proto → su3_client（RX + 事务）
 * 参考 AiTopper/RS485 的「单 RX 流缓冲 + 多帧重组 + pending」思路，
 * 但不引入其 0x5A 主机协议、双 USART bus、温湿度与报告 Flash 同步等复杂度。
 */
#ifndef SU3_CLIENT_H_
#define SU3_CLIENT_H_

#include "su3_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 UART、创建 RX 任务与事务资源。
 */
esp_err_t su3_init(const su3_config_t *cfg);

/** 注册被动回调（可传 NULL 表示不使用） */
void su3_set_handlers(const su3_handlers_t *handlers);

/**
 * 同步 CLI：编码 CliCommand → 组帧发送 → 等待应答写入 rsp。
 * @param dest  目标传感器地址（左 0x33 / 右 0x36）
 */
esp_err_t su3_cli_exec(su3_addr_t dest, const char *cmd,
                       char *rsp, size_t rsp_len, uint32_t timeout_ms);

/** list 专用：多帧拼接 + 空闲超时结束 */
esp_err_t su3_cli_list(su3_addr_t dest, char *rsp, size_t rsp_len, uint32_t timeout_ms);

/** 只发不等（如 report N） */
esp_err_t su3_cli_fire(su3_addr_t dest, const char *cmd);

/** 将逻辑侧别转为帧地址 */
su3_addr_t su3_side_to_addr(su3_side_t side);

/** 反查侧别；未知地址返回 SU3_SIDE_COUNT */
su3_side_t su3_addr_to_side(su3_addr_t addr);

/** 是否已 init */
bool su3_is_ready(void);

/**
 * 设置 CLI 共用的 CliCommand.deviceID（短 devid，如 KSSL00002）。
 * 通常在整机 set devid 成功后调用。
 */
void su3_set_cli_device_id(const char *device_id);

/**
 * 传感器固件 OTA（port4）：对 dest 侧执行 set mode 3 → 清盘 → 分包 → 结束。
 * @param dest     业务地址（左 0x33 / 右 0x36）
 * @param payload  完整固件镜像
 * @param len      镜像长度
 */
esp_err_t su3_sensor_ota(su3_addr_t dest, const uint8_t *payload, size_t len);

/** 是否正在串口刷写（升级期应停补传 / 拒 CLI） */
bool su3_sensor_ota_busy(void);

/** 清空 UART RX（mode3 后对齐 ESP_BC flush） */
void su3_uart_flush_rx(void);

#ifdef __cplusplus
}
#endif

#endif /* SU3_CLIENT_H_ */
