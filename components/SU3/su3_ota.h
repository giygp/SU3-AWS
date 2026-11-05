/**
 * 传感器 port4 固件升级（协议 4.2）。
 * 由 su3_client RX 喂入应答；业务层通过 su3_sensor_ota() 触发。
 */
#ifndef SU3_OTA_H_
#define SU3_OTA_H_

#include "su3_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void su3_ota_init(void);

/** RX：port4 裸 PDU 应答（ID[10]+'M'+...） */
void su3_ota_feed(su3_addr_t src, const uint8_t *pdu, size_t len);

bool su3_ota_is_busy(void);

/**
 * 对指定业务地址（0x33/0x36）执行：set mode 3 → 清盘 → 分包 → 结束。
 * @param fw  完整固件镜像（已在 ESP 侧下载）
 */
esp_err_t su3_ota_flash_side(su3_addr_t biz_dest, const uint8_t *fw, size_t fw_len);

#ifdef __cplusplus
}
#endif

#endif /* SU3_OTA_H_ */
