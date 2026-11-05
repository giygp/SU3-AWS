/**
 * 传感器 port4 OTA：对齐协议 4.2 与 ESP_BC sensor_ota_bc 帧格式。
 *
 * 流程：CLI set mode 3 → 0x00+包数(清盘) → 0x01+序号+128B → 0x02(结束)
 * 帧：src=0x47 dst=0x43/0x46，mf=0x01，PDU 裸二进制（非 protobuf）
 */
#include "su3_ota.h"
#include "su3_client.h"
#include "su3_frame.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "su3_ota";

#define SU3_OTA_CHUNK           128U
#define SU3_OTA_DATA_PDU_LEN    0x83U /* 0x01 + seq(2) + 128 */
#define SU3_OTA_ERASE_TIMEOUT_MS 12000U
#define SU3_OTA_DATA_TIMEOUT_MS  800U
#define SU3_OTA_FINISH_TIMEOUT_MS 5000U
#define SU3_OTA_PKT_RETRY        5
#define SU3_OTA_ERASE_RESTART    2

/* 由 su3_client 提供：组帧+UART 发送 */
extern esp_err_t su3_send_pdu(uint8_t src, uint8_t dst,
                              const uint8_t *pdu, size_t pdu_len);
extern uint8_t su3_self_addr(void);

typedef struct {
    bool              active;
    su3_addr_t        expect_src; /* 期望/已学习的 port4 源，如 0x43/0x46 */
    uint8_t           expect_cmd; /* 0x00 / 0x01 / 0x02 */
    uint8_t           status;
    uint16_t          seq;
    SemaphoreHandle_t done;
} su3_ota_pending_t;

static su3_ota_pending_t s_ota;
static SemaphoreHandle_t s_ota_mutex;
static bool s_busy;
/** 清盘应答后锁定的实际 OTA 目标地址（单传感器可能回 0x46 而非 0x43） */
static su3_addr_t s_ota_peer;

void su3_ota_init(void)
{
    if (s_ota.done == NULL) {
        s_ota.done = xSemaphoreCreateBinary();
    }
    if (s_ota_mutex == NULL) {
        s_ota_mutex = xSemaphoreCreateMutex();
    }
    s_ota.active = false;
    s_busy = false;
}

bool su3_ota_is_busy(void)
{
    return s_busy;
}

void su3_ota_feed(su3_addr_t src, const uint8_t *pdu, size_t len)
{
    size_t i;
    size_t mpos = (size_t)-1;
    uint8_t cmd;

    if (pdu == NULL || len < 2U) {
        return;
    }
    /* 只收 port4；不强制左右地址（实机可能对 0x43 发、从 0x46 回） */
    if (SU3_PORT_OF(src) != SU3_PORT_OTA) {
        return;
    }
    for (i = 0; i + 1U < len; i++) {
        if (pdu[i] == (uint8_t)'M') {
            mpos = i;
            break;
        }
    }
    if (mpos == (size_t)-1 || mpos + 1U >= len) {
        return;
    }
    if (s_ota_mutex == NULL || s_ota.done == NULL) {
        return;
    }
    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (!s_ota.active) {
        xSemaphoreGive(s_ota_mutex);
        return;
    }

    cmd = pdu[mpos + 1U];
    if (cmd != s_ota.expect_cmd) {
        xSemaphoreGive(s_ota_mutex);
        return;
    }

    s_ota.status = 0;
    s_ota.seq = 0;
    if (cmd == 0x01U) {
        if (mpos + 4U >= len) {
            xSemaphoreGive(s_ota_mutex);
            return;
        }
        s_ota.status = pdu[mpos + 2U];
        s_ota.seq = (uint16_t)pdu[mpos + 3U] | ((uint16_t)pdu[mpos + 4U] << 8);
    } else if (cmd == 0x02U) {
        if (mpos + 2U >= len) {
            xSemaphoreGive(s_ota_mutex);
            return;
        }
        s_ota.status = pdu[mpos + 2U];
        if (s_ota.status != 0U && mpos + 4U < len) {
            s_ota.seq = (uint16_t)pdu[mpos + 3U] | ((uint16_t)pdu[mpos + 4U] << 8);
        }
    }

    /* 记下真实应答地址，后续分包改发往该地址 */
    s_ota.expect_src = src;
    s_ota_peer = src;

    s_ota.active = false;
    xSemaphoreGive(s_ota_mutex);
    xSemaphoreGive(s_ota.done);
}

static esp_err_t ota_arm_wait(uint8_t expect_cmd)
{
    if (s_ota.done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(s_ota.done, 0) == pdTRUE) {
    }

    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ota.expect_cmd = expect_cmd;
    s_ota.status = 0xFFU;
    s_ota.seq = 0;
    s_ota.active = true;
    xSemaphoreGive(s_ota_mutex);
    return ESP_OK;
}

static esp_err_t ota_wait_armed(uint32_t timeout_ms,
                                uint8_t *out_status, uint16_t *out_seq)
{
    if (xSemaphoreTake(s_ota.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_ota.active = false;
            xSemaphoreGive(s_ota_mutex);
        }
        return ESP_ERR_TIMEOUT;
    }

    if (out_status) {
        *out_status = s_ota.status;
    }
    if (out_seq) {
        *out_seq = s_ota.seq;
    }
    return ESP_OK;
}

static esp_err_t ota_tx_and_wait(uint8_t src_ota, uint8_t dst_ota,
                                 const uint8_t *pdu, size_t pdu_len,
                                 uint8_t expect_cmd, uint32_t timeout_ms,
                                 uint8_t *out_status, uint16_t *out_seq)
{
    esp_err_t err;

    /* 先武装应答，再发送，避免快应答竞态丢失 */
    err = ota_arm_wait(expect_cmd);
    if (err != ESP_OK) {
        return err;
    }
    err = su3_send_pdu(src_ota, dst_ota, pdu, pdu_len);
    if (err != ESP_OK) {
        if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_ota.active = false;
            xSemaphoreGive(s_ota_mutex);
        }
        return err;
    }
    return ota_wait_armed(timeout_ms, out_status, out_seq);
}

static esp_err_t ota_set_mode3(su3_addr_t biz_dest)
{
    char rsp[64];
    esp_err_t err;
    int i;
    su3_addr_t other;

    for (i = 0; i < 40; i++) {
        memset(rsp, 0, sizeof(rsp));
        err = su3_cli_exec(biz_dest, "set mode 3", rsp, sizeof(rsp), 1500);
        if (err == ESP_ERR_INVALID_STATE) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (err == ESP_OK && strncmp(rsp, "ok", 2) == 0) {
            printf("su3_ota: set mode 3 ok\n");
            /* 单总线双地址：另一侧也进升级态（忽略失败） */
            other = (biz_dest == su3_side_to_addr(SU3_SIDE_LEFT))
                        ? su3_side_to_addr(SU3_SIDE_RIGHT)
                        : su3_side_to_addr(SU3_SIDE_LEFT);
            memset(rsp, 0, sizeof(rsp));
            (void)su3_cli_exec(other, "set mode 3", rsp, sizeof(rsp), 1500);
            vTaskDelay(pdMS_TO_TICKS(1000));
            su3_uart_flush_rx();
            return ESP_OK;
        }
        ESP_LOGW(TAG, "set mode 3 fail dest=0x%02X rsp=\"%.16s\"",
                 biz_dest, rsp);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return ESP_FAIL;
}

static esp_err_t ota_transfer(su3_addr_t biz_dest, const uint8_t *fw, size_t fw_len)
{
    uint8_t src_ota = SU3_ADDR_TO_OTA(su3_self_addr());
    uint8_t dst_ota = SU3_ADDR_TO_OTA(biz_dest);
    uint16_t pkt_total;
    uint16_t pkt_idx;
    uint8_t pdu[SU3_OTA_DATA_PDU_LEN];
    uint8_t status;
    uint16_t ack_seq;
    esp_err_t err;
    int erase_round;
    int retry;
    size_t off;
    size_t copy_len;
    uint32_t last_log_pct = 0;

    if (fw == NULL || fw_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    pkt_total = (uint16_t)((fw_len + SU3_OTA_CHUNK - 1U) / SU3_OTA_CHUNK);
    if (pkt_total == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ota.expect_src = dst_ota;
    s_ota_peer = dst_ota;
    printf("su3_ota: flash start len=%u pkts=%u\n",
           (unsigned)fw_len, (unsigned)pkt_total);

    for (erase_round = 0; erase_round <= SU3_OTA_ERASE_RESTART; erase_round++) {
        /* c. 清盘：0x00 + 包数(LE) */
        pdu[0] = 0x00;
        pdu[1] = (uint8_t)(pkt_total & 0xFFU);
        pdu[2] = (uint8_t)((pkt_total >> 8) & 0xFFU);
        status = 0xFF;
        err = ESP_FAIL;
        for (retry = 0; retry < SU3_OTA_PKT_RETRY; retry++) {
            err = ota_tx_and_wait(src_ota, dst_ota, pdu, 3,
                                  0x00, SU3_OTA_ERASE_TIMEOUT_MS,
                                  &status, &ack_seq);
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "erase timeout retry=%d", retry + 1);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "erase fail");
            return err;
        }
        if (s_ota_peer != 0 && s_ota_peer != dst_ota) {
            dst_ota = s_ota_peer;
        }
        printf("su3_ota: erase ok peer=0x%02X\n", dst_ota);

        /* d. 数据包：0x01 + seq(LE) + 128B（不足填 0xFF） */
        for (pkt_idx = 0; pkt_idx < pkt_total; pkt_idx++) {
            off = (size_t)pkt_idx * SU3_OTA_CHUNK;
            copy_len = fw_len - off;
            if (copy_len > SU3_OTA_CHUNK) {
                copy_len = SU3_OTA_CHUNK;
            }
            memset(pdu, 0xFF, sizeof(pdu));
            pdu[0] = 0x01;
            pdu[1] = (uint8_t)(pkt_idx & 0xFFU);
            pdu[2] = (uint8_t)((pkt_idx >> 8) & 0xFFU);
            memcpy(&pdu[3], &fw[off], copy_len);

            for (retry = 0; retry < SU3_OTA_PKT_RETRY; retry++) {
                status = 0xFF;
                ack_seq = 0xFFFF;
                err = ota_tx_and_wait(src_ota, dst_ota, pdu, SU3_OTA_DATA_PDU_LEN,
                                      0x01, SU3_OTA_DATA_TIMEOUT_MS,
                                      &status, &ack_seq);
                if (err == ESP_ERR_TIMEOUT) {
                    continue;
                }
                if (err != ESP_OK) {
                    return err;
                }
                if (status == 0x00U) {
                    break; /* 正确 */
                }
                if (status == 0x01U) {
                    continue;
                }
                if (status == 0x02U) {
                    goto restart_erase;
                }
            }
            if (err != ESP_OK || status != 0x00U) {
                ESP_LOGE(TAG, "pkt %u fail", (unsigned)pkt_idx);
                return ESP_FAIL;
            }

            {
                uint32_t pct = ((uint32_t)(pkt_idx + 1U) * 100U) / (uint32_t)pkt_total;
                if (pct >= last_log_pct + 25U || (pkt_idx + 1U) == pkt_total) {
                    last_log_pct = pct;
                    printf("su3_ota: flash %u%%\n", (unsigned)pct);
                }
            }
        }

        /* e. 结束：0x02 */
        pdu[0] = 0x02;
        for (retry = 0; retry < SU3_OTA_PKT_RETRY; retry++) {
            status = 0xFF;
            ack_seq = 0;
            err = ota_tx_and_wait(src_ota, dst_ota, pdu, 1,
                                  0x02, SU3_OTA_FINISH_TIMEOUT_MS,
                                  &status, &ack_seq);
            if (err == ESP_ERR_TIMEOUT) {
                continue;
            }
            if (err != ESP_OK) {
                return err;
            }
            if (status == 0x00U) {
                printf("su3_ota: flash finish ok\n");
                return ESP_OK;
            }
            if (status == 0x01U) {
                goto restart_erase;
            }
        }
        return ESP_FAIL;

restart_erase:
        ESP_LOGW(TAG, "restart erase");
        continue;
    }

    return ESP_FAIL;
}

esp_err_t su3_ota_flash_side(su3_addr_t biz_dest, const uint8_t *fw, size_t fw_len)
{
    esp_err_t err;
    char rsp[32];

    if (!su3_is_ready() || fw == NULL || fw_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.done == NULL) {
        su3_ota_init();
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    s_busy = true;
    err = ota_set_mode3(biz_dest);
    if (err != ESP_OK) {
        s_busy = false;
        return err;
    }
    err = ota_transfer(biz_dest, fw, fw_len);
    s_busy = false;

    if (err != ESP_OK) {
        memset(rsp, 0, sizeof(rsp));
        (void)su3_cli_exec(biz_dest, "set mode 2", rsp, sizeof(rsp), 1500);
        ESP_LOGE(TAG, "sensor ota fail, restore mode2");
    }
    return err;
}
