/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh
 * @LastEditTime: 2026-07-14 18:19:35
 */
#include "su3_client.h"
#include "su3_frame.h"
#include "su3_proto.h"
#include "su3_ota.h"
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "su3_client";

#define SU3_RX_CHUNK           256
#define SU3_TX_FRAME_MAX       192
#define SU3_CLI_IDLE_MS        100U   /* list 多帧：距上次收包空闲即结束 */
#define SU3_RX_TASK_STACK      16384  /* on_topic/解码+偶发 MQTT 错误路径仍在 RX，8K 断网易溢出 */
#define SU3_RX_TASK_PRIO       10

typedef struct {
    bool              active;
    su3_addr_t        expect_src; /* 期望应答源地址（即下发 dest） */
    su3_resp_kind_t   kind;
    char             *rsp;
    size_t            rsp_cap;
    size_t            rsp_len;
    SemaphoreHandle_t done;
    TickType_t        last_feed_tick;
    bool              got_any;
} su3_pending_t;

static bool s_ready;
static su3_config_t s_cfg;
static su3_handlers_t s_handlers;
static su3_frame_ctx_t *s_frame;
static SemaphoreHandle_t s_tx_mutex;
static SemaphoreHandle_t s_pending_mutex;
static su3_pending_t s_pending;
static TaskHandle_t s_rx_task;
static char s_peer_id[SU3_SIDE_COUNT][24];

static int32_t su3_now_ts(void)
{
    time_t now = time(NULL);
    if (now < 0 || now < 1000000000L) {
        return 0;
    }
    return (int32_t)now;
}

static void su3_store_peer_id(su3_addr_t src, const char *device_id)
{
    su3_side_t side = su3_addr_to_side(src);
    if (side >= SU3_SIDE_COUNT || device_id == NULL || device_id[0] == '\0') {
        return;
    }
    /* 已固定主芯片 id 后不再用回包学习（传感器回包 deviceID 可能截断） */
    if (s_cfg.device_id[0] != '\0') {
        return;
    }
    strncpy(s_peer_id[side], device_id, sizeof(s_peer_id[side]) - 1U);
    s_peer_id[side][sizeof(s_peer_id[side]) - 1U] = '\0';
    ESP_LOGD(TAG, "peer_id learn side=%d src=0x%02X id=\"%s\"",
             (int)side, src, s_peer_id[side]);
}

static const char *su3_device_id_for_dest(su3_addr_t dest)
{
    su3_side_t side = su3_addr_to_side(dest);
    su3_side_t other;

    /* 主芯片统一 id（set devid 后写入），左右共用 */
    if (s_cfg.device_id[0] != '\0') {
        return s_cfg.device_id;
    }
    if (side < SU3_SIDE_COUNT && s_peer_id[side][0] != '\0') {
        return s_peer_id[side];
    }
    if (side < SU3_SIDE_COUNT) {
        other = (side == SU3_SIDE_LEFT) ? SU3_SIDE_RIGHT : SU3_SIDE_LEFT;
        if (s_peer_id[other][0] != '\0') {
            return s_peer_id[other];
        }
    }
    return "UNCONFIGED";
}

void su3_set_cli_device_id(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return;
    }
    strncpy(s_cfg.device_id, device_id, sizeof(s_cfg.device_id) - 1U);
    s_cfg.device_id[sizeof(s_cfg.device_id) - 1U] = '\0';
    for (int i = 0; i < (int)SU3_SIDE_COUNT; i++) {
        strncpy(s_peer_id[i], s_cfg.device_id, sizeof(s_peer_id[i]) - 1U);
        s_peer_id[i][sizeof(s_peer_id[i]) - 1U] = '\0';
    }
    ESP_LOGI(TAG, "cli device_id shared=\"%s\"", s_cfg.device_id);
}

static void pending_clear(void)
{
    s_pending.active = false;
    s_pending.expect_src = 0;
    s_pending.rsp = NULL;
    s_pending.rsp_cap = 0;
    s_pending.rsp_len = 0;
    s_pending.got_any = false;
    s_pending.kind = SU3_RESP_SINGLE;
}

static void pending_feed_text(su3_addr_t src, const char *text)
{
    size_t tlen;

    if (!s_pending.active || text == NULL) {
        return;
    }
    if (s_pending.expect_src != 0 && src != s_pending.expect_src) {
        return;
    }

    tlen = strlen(text);
    if (s_pending.kind == SU3_RESP_SINGLE) {
        if (s_pending.rsp != NULL && s_pending.rsp_cap > 0) {
            strncpy(s_pending.rsp, text, s_pending.rsp_cap - 1U);
            s_pending.rsp[s_pending.rsp_cap - 1U] = '\0';
            s_pending.rsp_len = strlen(s_pending.rsp);
        }
        s_pending.got_any = true;
        s_pending.active = false;
        if (s_pending.done) {
            xSemaphoreGive(s_pending.done);
        }
        return;
    }

    /* MULTIFRAME：拼接文本，空闲超时由 RX 任务结束 */
    if (s_pending.rsp == NULL || s_pending.rsp_cap == 0) {
        return;
    }
    /* list 多帧拼接：仅在两段之间缺分隔时补一个 \n，避免 \n\n */
    if (s_pending.rsp_len > 0 && tlen > 0 &&
        s_pending.rsp_len < s_pending.rsp_cap - 1U) {
        char last = s_pending.rsp[s_pending.rsp_len - 1U];
        char first = text[0];
        if (last != '\n' && first != '\n') {
            s_pending.rsp[s_pending.rsp_len++] = '\n';
            s_pending.rsp[s_pending.rsp_len] = '\0';
        }
    }
    if (s_pending.rsp_len + tlen + 1U > s_pending.rsp_cap) {
        tlen = s_pending.rsp_cap - s_pending.rsp_len - 1U;
    }
    if (tlen > 0) {
        memcpy(&s_pending.rsp[s_pending.rsp_len], text, tlen);
        s_pending.rsp_len += tlen;
        s_pending.rsp[s_pending.rsp_len] = '\0';
    }
    s_pending.got_any = true;
    s_pending.last_feed_tick = xTaskGetTickCount();
}

static void su3_on_pdu(const su3_pdu_info_t *pdu, void *user)
{
    char cmd[128];
    char devid[24];
    (void)user;

    if (pdu == NULL) {
        return;
    }

    /* port4 OTA 应答：禁止当 protobuf topic 旁路 */
    if (SU3_PORT_OF(pdu->src_addr) == SU3_PORT_OTA) {
        su3_ota_feed(pdu->src_addr, pdu->pb_data, pdu->pb_len);
        return;
    }

    /* 升级期间：仅保留 CLI 事务应答（set mode 3）；丢弃业务 topic 与被动 CLI 推送 */
    if (su3_ota_is_busy() && pdu->topic != SU3_TOPIC_CLI) {
        return;
    }

    // ESP_LOGI(TAG,"PDU: src=0x%02X dst=0x%02X topic=%u pb_len=%u",pdu->src_addr,pdu->dst_addr,(unsigned)pdu->topic,(unsigned)pdu->pb_len);
    
    if (pdu->topic == SU3_TOPIC_CLI) /* CLI 消息处理 */
    {
        memset(devid, 0, sizeof(devid));
        if (su3_proto_decode_cli(pdu->pb_data, pdu->pb_len, cmd, sizeof(cmd),
                                 devid, sizeof(devid)) != ESP_OK) {
            printf("cli decode fail src=0x%02X dst=0x%02X pb_len=%u hex:",
                   pdu->src_addr, pdu->dst_addr, (unsigned)pdu->pb_len);
            {
                size_t i;
                size_t n = pdu->pb_len < 32U ? pdu->pb_len : 32U;
                for (i = 0; i < n; i++) {
                    printf(" %02X", pdu->pb_data[i]);
                }
            }
            printf("\n");
            return;
        }
        /* list 等多行文本用 printf，避免 ESP_LOG 把换行拆成多条日志头 */
        printf("cli rx src=0x%02X dst=0x%02X len=%u devid=\"%s\" cmd=\"%s\"\n",
               pdu->src_addr, pdu->dst_addr, (unsigned)pdu->pb_len,
               devid[0] ? devid : "", cmd);
        su3_store_peer_id(pdu->src_addr, devid);

        /* hello / list updata 是被动上报，不能当作 cli_exec 的应答 */
        if (strstr(cmd, "hello") != NULL || strstr(cmd, "list updata") != NULL) {
            if (su3_ota_is_busy()) {
                return;
            }
            if (strstr(cmd, "hello") != NULL && s_handlers.on_hello != NULL) {
                s_handlers.on_hello(pdu->src_addr, s_handlers.user);
            }
            if (s_handlers.on_cli_push != NULL) {
                s_handlers.on_cli_push(pdu->src_addr, cmd, devid, s_handlers.user);
            }
            return;
        }

        if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (s_pending.active &&
                (s_pending.kind == SU3_RESP_SINGLE || s_pending.kind == SU3_RESP_MULTIFRAME)) {
                pending_feed_text(pdu->src_addr, cmd);
                xSemaphoreGive(s_pending_mutex);
                return;
            }
            xSemaphoreGive(s_pending_mutex);
        }

        if (su3_ota_is_busy()) {
            return;
        }
        if (s_handlers.on_cli_push != NULL) {
            s_handlers.on_cli_push(pdu->src_addr, cmd, devid, s_handlers.user);
        }
        return;
    }

    printf("su3 rx topic=%u src=0x%02X dst=0x%02X pb_len=%u\n",
           (unsigned)pdu->topic, pdu->src_addr, pdu->dst_addr, (unsigned)pdu->pb_len);
    if (s_handlers.on_topic != NULL)
    {
        s_handlers.on_topic(pdu->src_addr, pdu->topic, pdu->pb_data, pdu->pb_len, s_handlers.user);
    }
}

static esp_err_t su3_uart_send_frame(const uint8_t *frame, size_t len)
{
    int written;

    if (frame == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    written = uart_write_bytes(s_cfg.uart_port, (const char *)frame, len);
    xSemaphoreGive(s_tx_mutex);
    if (written < 0 || (size_t)written != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** 组帧发送裸 PDU（OTA / 内部）；供 su3_ota 使用 */
esp_err_t su3_send_pdu(uint8_t src, uint8_t dst, const uint8_t *pdu, size_t pdu_len)
{
    uint8_t frame[SU3_TX_FRAME_MAX];
    size_t frame_len;

    if (!s_ready || pdu == NULL || pdu_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    frame_len = su3_frame_build(src, dst, SU3_MF_SINGLE, pdu, pdu_len,
                                frame, sizeof(frame));
    if (frame_len == 0) {
        return ESP_FAIL;
    }
    return su3_uart_send_frame(frame, frame_len);
}

uint8_t su3_self_addr(void)
{
    return s_ready ? s_cfg.self_addr : SU3_ADDR_ESP32_DEFAULT;
}

static esp_err_t su3_send_cli_pdu(su3_addr_t dest, const char *cmd)
{
    uint8_t pdu[160];
    uint8_t frame[SU3_TX_FRAME_MAX];
    size_t pdu_len = 0;
    size_t frame_len;
    esp_err_t err;
    const char *devid = su3_device_id_for_dest(dest);

    ESP_LOGD(TAG, "cli TX dest=0x%02X device_id=\"%s\" cmd=\"%s\"",
             dest, devid ? devid : "", cmd ? cmd : "");

    err = su3_proto_encode_cli(devid, su3_now_ts(), cmd,
                               pdu, sizeof(pdu), &pdu_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cli TX encode fail dest=0x%02X err=%s",
                 dest, esp_err_to_name(err));
        return err;
    }

    frame_len = su3_frame_build(s_cfg.self_addr, dest, SU3_MF_SINGLE, pdu, pdu_len,
                                frame, sizeof(frame));
    if (frame_len == 0) {
        ESP_LOGW(TAG, "cli TX frame build fail dest=0x%02X", dest);
        return ESP_FAIL;
    }
    err = su3_uart_send_frame(frame, frame_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cli TX uart fail dest=0x%02X err=%s",
                 dest, esp_err_to_name(err));
    }
    return err;
}

/** list 空闲结束：RX 与 cli_exec 等待侧都会调用（避免 RX 被 on_topic 占住时永远不结束） */
static void su3_check_list_idle(void)
{
    if (xSemaphoreTake(s_pending_mutex, 0) != pdTRUE) {
        return;
    }
    if (s_pending.active &&
        s_pending.kind == SU3_RESP_MULTIFRAME &&
        s_pending.got_any) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_pending.last_feed_tick) >= pdMS_TO_TICKS(SU3_CLI_IDLE_MS)) {
            s_pending.active = false;
            ESP_LOGI(TAG, "list idle-done src=0x%02X len=%u",
                     s_pending.expect_src, (unsigned)s_pending.rsp_len);
            if (s_pending.done) {
                xSemaphoreGive(s_pending.done);
            }
        }
    }
    xSemaphoreGive(s_pending_mutex);
}

static void su3_rx_task(void *arg)
{
    uint8_t chunk[SU3_RX_CHUNK];
    uint32_t rx_bytes = 0;
    TickType_t last_stat = xTaskGetTickCount();
    (void)arg;

    ESP_LOGI(TAG, "rx task start");
    while (1) {
        int n = uart_read_bytes(s_cfg.uart_port, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (n > 0) {
            rx_bytes += (uint32_t)n;
            su3_frame_feed(s_frame, chunk, (size_t)n);
        }
        su3_check_list_idle();

        /* 每 5s 打印一次 RX 统计，便于判断「无串口数据」还是「CRC/解析失败」 */
        if ((xTaskGetTickCount() - last_stat) >= pdMS_TO_TICKS(5000)) {
            uint32_t ok = 0, crc_fail = 0, overflow = 0;
            su3_frame_get_stats(s_frame, &ok, &crc_fail, &overflow);
           // ESP_LOGI(TAG, "rx stats: bytes=%u frame_ok=%u crc_fail=%u overflow=%u",(unsigned)rx_bytes, (unsigned)ok, (unsigned)crc_fail, (unsigned)overflow);
            last_stat = xTaskGetTickCount();
        }
    }
}

static esp_err_t su3_uart_setup(void)
{
    uart_config_t uc = {
        .baud_rate = s_cfg.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    if (!uart_is_driver_installed(s_cfg.uart_port)) {
        /* RX 环缓 4K；TX 用 0=阻塞写，避免多余 ringbuf、降低异常面 */
        esp_err_t err = uart_driver_install(s_cfg.uart_port, SU3_RING_BUF_SIZE, 0,
                                            0, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
        err = uart_param_config(s_cfg.uart_port, &uc);
        if (err != ESP_OK) {
            return err;
        }
        err = uart_set_pin(s_cfg.uart_port, s_cfg.tx_pin, s_cfg.rx_pin,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        ESP_LOGW(TAG, "uart%d already installed, reuse (ensure baud/pins match)",
                 (int)s_cfg.uart_port);
    }
    return ESP_OK;
}

esp_err_t su3_init(const su3_config_t *cfg)
{
    esp_err_t err;

    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;
    if (s_cfg.self_addr == 0) {
        s_cfg.self_addr = SU3_ADDR_ESP32_DEFAULT;
    }
    if (s_cfg.addr_left == 0) {
        s_cfg.addr_left = SU3_ADDR_LEFT_DEFAULT;
    }
    if (s_cfg.addr_right == 0) {
        s_cfg.addr_right = SU3_ADDR_RIGHT_DEFAULT;
    }
    if (s_cfg.baud_rate == 0) {
        s_cfg.baud_rate = 115200;
    }
    /* UART_NUM_0 合法；用 tx/rx 为 0 且未指定 port 时才默认 NUM_1 */
    if (s_cfg.tx_pin == 0 && s_cfg.rx_pin == 0) {
        s_cfg.tx_pin = 22;
        s_cfg.rx_pin = 26;
        if (cfg->uart_port == 0 && cfg->baud_rate == 0) {
            s_cfg.uart_port = UART_NUM_1;
        }
    }

    s_tx_mutex = xSemaphoreCreateMutex();
    s_pending_mutex = xSemaphoreCreateMutex();
    s_pending.done = xSemaphoreCreateBinary();
    if (s_tx_mutex == NULL || s_pending_mutex == NULL || s_pending.done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pending_clear();
    memset(s_peer_id, 0, sizeof(s_peer_id));

    s_frame = su3_frame_create(su3_on_pdu, NULL);
    if (s_frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = su3_uart_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart setup fail: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(su3_rx_task, "su3_rx", SU3_RX_TASK_STACK,
                                            NULL, SU3_RX_TASK_PRIO, &s_rx_task, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    su3_ota_init();
    s_ready = true;
    ESP_LOGI(TAG, "su3_init ok uart=%d baud=%d self=0x%02X L=0x%02X R=0x%02X pins tx=%d rx=%d",
             (int)s_cfg.uart_port, s_cfg.baud_rate, s_cfg.self_addr,
             s_cfg.addr_left, s_cfg.addr_right, s_cfg.tx_pin, s_cfg.rx_pin);
    return ESP_OK;
}

void su3_set_handlers(const su3_handlers_t *handlers)
{
    if (handlers == NULL) {
        memset(&s_handlers, 0, sizeof(s_handlers));
        return;
    }
    s_handlers = *handlers;
}

/* 发送 CLI 消息，并等待应答 */
esp_err_t su3_cli_exec(su3_addr_t dest, const char *cmd,char *rsp, size_t rsp_len, uint32_t timeout_ms)
{
    esp_err_t err;
    su3_resp_kind_t kind;

    if (!s_ready || cmd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rsp == NULL || rsp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    kind = su3_proto_classify_cmd(cmd);
    if (kind == SU3_RESP_FIRE_FORGET) {
        return su3_cli_fire(dest, cmd);
    }

    if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_pending.active) {
        xSemaphoreGive(s_pending_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* 清空 done 信号量 */
    while (xSemaphoreTake(s_pending.done, 0) == pdTRUE) {
    }

    s_pending.active = true;
    s_pending.expect_src = dest;
    s_pending.kind = kind;
    s_pending.rsp = rsp;
    s_pending.rsp_cap = rsp_len;
    s_pending.rsp_len = 0;
    s_pending.got_any = false;
    s_pending.last_feed_tick = xTaskGetTickCount();
    rsp[0] = '\0';
    xSemaphoreGive(s_pending_mutex);

    err = su3_send_cli_pdu(dest, cmd);
    if (err != ESP_OK) {
        xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
        pending_clear();
        xSemaphoreGive(s_pending_mutex);
        ESP_LOGW(TAG, "cli_exec send fail dest=0x%02X err=%s",
                 dest, esp_err_to_name(err));
        return err;
    }

    /* 分段等待：MULTIFRAME 时等待侧也跑 idle，不依赖 RX 是否空闲 */
    {
        TickType_t t0 = xTaskGetTickCount();
        TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
        bool got_done = false;

        while ((xTaskGetTickCount() - t0) < wait_ticks) {
            if (kind == SU3_RESP_MULTIFRAME) {
                su3_check_list_idle();
            }
            if (xSemaphoreTake(s_pending.done, pdMS_TO_TICKS(20)) == pdTRUE) {
                got_done = true;
                break;
            }
        }
        if (!got_done && kind == SU3_RESP_MULTIFRAME) {
            su3_check_list_idle();
            if (xSemaphoreTake(s_pending.done, 0) == pdTRUE) {
                got_done = true;
            }
        }

        if (!got_done) {
            bool have_partial = false;

            xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
            /* list 已拼到内容：超时也按成功返回，避免把报告头冲成 "timeout" */
            have_partial = (kind == SU3_RESP_MULTIFRAME &&
                            s_pending.got_any && s_pending.rsp_len > 0U);
            pending_clear();
            xSemaphoreGive(s_pending_mutex);
            if (have_partial) {
                ESP_LOGW(TAG, "cli_exec list timeout-but-data dest=0x%02X len=%u",
                         dest, (unsigned)strlen(rsp));
                return ESP_OK;
            }
            ESP_LOGW(TAG, "cli_exec TIMEOUT dest=0x%02X cmd=\"%s\" %ums",
                     dest, cmd, (unsigned)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
    }

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    pending_clear();
    xSemaphoreGive(s_pending_mutex);
    ESP_LOGD(TAG, "cli_exec OK dest=0x%02X cmd=\"%s\" back=\"%.64s\"",
             dest, cmd, rsp);
    return ESP_OK;
}

esp_err_t su3_cli_list(su3_addr_t dest, char *rsp, size_t rsp_len, uint32_t timeout_ms)
{
    return su3_cli_exec(dest, "list", rsp, rsp_len, timeout_ms);
}

esp_err_t su3_cli_fire(su3_addr_t dest, const char *cmd)
{
    if (!s_ready || cmd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return su3_send_cli_pdu(dest, cmd);
}

su3_addr_t su3_side_to_addr(su3_side_t side)
{
    if (side == SU3_SIDE_LEFT) {
        return s_ready ? s_cfg.addr_left : SU3_ADDR_LEFT_DEFAULT;
    }
    if (side == SU3_SIDE_RIGHT) {
        return s_ready ? s_cfg.addr_right : SU3_ADDR_RIGHT_DEFAULT;
    }
    return 0;
}

su3_side_t su3_addr_to_side(su3_addr_t addr)
{
    uint8_t left = s_ready ? s_cfg.addr_left : SU3_ADDR_LEFT_DEFAULT;
    uint8_t right = s_ready ? s_cfg.addr_right : SU3_ADDR_RIGHT_DEFAULT;
    if (addr == left) {
        return SU3_SIDE_LEFT;
    }
    if (addr == right) {
        return SU3_SIDE_RIGHT;
    }
    return SU3_SIDE_COUNT;
}

bool su3_is_ready(void)
{
    return s_ready;
}

esp_err_t su3_sensor_ota(su3_addr_t dest, const uint8_t *payload, size_t len)
{
    return su3_ota_flash_side(dest, payload, len);
}

bool su3_sensor_ota_busy(void)
{
    return su3_ota_is_busy();
}

void su3_uart_flush_rx(void)
{
    if (!s_ready) {
        return;
    }
    uart_flush_input(s_cfg.uart_port);
}
