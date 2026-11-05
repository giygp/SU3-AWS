/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh
 * @LastEditTime: 2026-07-14 18:19:35
 */
#include "su3_frame.h"
#include "su3_crc.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "su3_frame";

#define SU3_REASM_SLOTS  4

typedef struct {
    uint8_t  active;
    uint8_t  src_addr;
    uint8_t  dst_addr;
    uint8_t  next_seq;
    uint16_t len;
    uint8_t  buf[SU3_PDU_REASM_MAX];
} su3_reasm_t;

struct su3_frame_ctx {
    su3_pdu_cb_t on_pdu;
    void *user;
    uint8_t  stream[SU3_RING_BUF_SIZE];
    uint16_t stream_len;
    uint32_t overflow_cnt;
    uint32_t frame_ok_cnt;
    uint32_t crc_fail_cnt;
    su3_reasm_t reasm[SU3_REASM_SLOTS];
};

static void stream_consume(su3_frame_ctx_t *ctx, uint16_t len)
{
    if (len == 0) {
        return;
    }
    if (len >= ctx->stream_len) {
        ctx->stream_len = 0;
        return;
    }
    memmove(ctx->stream, &ctx->stream[len], ctx->stream_len - len);
    ctx->stream_len = (uint16_t)(ctx->stream_len - len);
}

static void reasm_reset_slot(su3_reasm_t *r)
{
    memset(r, 0, sizeof(*r));
}

static su3_reasm_t *reasm_find(su3_frame_ctx_t *ctx, uint8_t src, bool alloc_if_missing)
{
    int i;
    int free_idx = -1;

    for (i = 0; i < SU3_REASM_SLOTS; i++) {
        if (ctx->reasm[i].active && ctx->reasm[i].src_addr == src) {
            return &ctx->reasm[i];
        }
        if (!ctx->reasm[i].active && free_idx < 0) {
            free_idx = i;
        }
    }
    if (alloc_if_missing && free_idx >= 0) {
        return &ctx->reasm[free_idx];
    }
    return NULL;
}

static void emit_pdu(su3_frame_ctx_t *ctx, uint8_t src, uint8_t dst,
                     const uint8_t *pdu, uint16_t pdu_len)
{
    su3_pdu_info_t info;

    if (ctx->on_pdu == NULL || pdu == NULL || pdu_len == 0) {
        return;
    }

    info.src_addr = src;
    info.dst_addr = dst;

    /* port4：裸二进制 OTA（无 topic 字节），整段 PDU 交给上层 */
    if (SU3_PORT_OF(src) == SU3_PORT_OTA || SU3_PORT_OF(dst) == SU3_PORT_OTA) {
        info.topic = 0xFFU;
        info.pb_data = pdu;
        info.pb_len = pdu_len;
        ctx->on_pdu(&info, ctx->user);
        return;
    }

    if (pdu_len < SU3_TOPIC_SIZE) {
        return;
    }
    info.topic = pdu[0];
    info.pb_data = (pdu_len > SU3_TOPIC_SIZE) ? &pdu[SU3_TOPIC_SIZE] : NULL;
    info.pb_len = (pdu_len > SU3_TOPIC_SIZE) ? (size_t)(pdu_len - SU3_TOPIC_SIZE) : 0;
    ctx->on_pdu(&info, ctx->user);
}

static bool reasm_append(su3_reasm_t *r, const uint8_t *data, uint8_t len)
{
    if (r == NULL || data == NULL || len == 0) {
        return false;
    }
    if ((uint32_t)r->len + (uint32_t)len > SU3_PDU_REASM_MAX) {
        ESP_LOGW(TAG, "reasm overflow src=0x%02X", r->src_addr);
        reasm_reset_slot(r);
        return false;
    }
    memcpy(&r->buf[r->len], data, len);
    r->len = (uint16_t)(r->len + len);
    return true;
}

/**
 * 多帧 flag 布局（与 AiTopper RS485 / 协议一致）：
 *   bit0..3 version（须为 0x1）
 *   bit4    segment：1=后续还有分片
 *   bit5..7 seq
 * 单帧：seg=0 seq=0
 */
static void link_dispatch(su3_frame_ctx_t *ctx, const uint8_t *frame, uint16_t frame_len)
{
    uint8_t mf;
    uint8_t pdu_len;
    uint8_t src;
    uint8_t dst;
    uint8_t seg;
    uint8_t seq;
    const uint8_t *pdu;
    su3_reasm_t *r;

    if (frame == NULL || frame_len < SU3_FRAME_MIN_SIZE) {
        return;
    }

    mf = frame[2];
    pdu_len = frame[3];
    src = frame[4];
    dst = frame[5];
    pdu = &frame[SU3_HEADER_SIZE];

    /* 文档写 version=0x1；实机 hello 等帧为 mf=0x00（仍是单帧 seg=0/seq=0），均放行 */
    {
        uint8_t ver = (uint8_t)(mf & SU3_MF_VERSION_MASK);
        if (ver != 0x01U && ver != 0x00U) {
            ESP_LOGW(TAG, "bad mf=0x%02X src=0x%02X", mf, src);
            r = reasm_find(ctx, src, false);
            if (r) {
                reasm_reset_slot(r);
            }
            return;
        }
    }

    seg = (uint8_t)((mf & SU3_MF_SEGMENT_BIT) != 0U ? 1U : 0U);
    seq = (uint8_t)((mf >> 5) & 0x07U);

    /* 单帧 */
    if (seg == 0U && seq == 0U) {
        r = reasm_find(ctx, src, false);
        if (r) {
            reasm_reset_slot(r);
        }
        emit_pdu(ctx, src, dst, pdu, pdu_len);
        return;
    }

    /* 多帧首片 */
    if (seg != 0U && seq == 0U) {
        r = reasm_find(ctx, src, true);
        if (r == NULL) {
            ESP_LOGW(TAG, "no reasm slot for src=0x%02X", src);
            return;
        }
        reasm_reset_slot(r);
        r->active = 1;
        r->src_addr = src;
        r->dst_addr = dst;
        r->next_seq = 1;
        if (!reasm_append(r, pdu, pdu_len)) {
            return;
        }
        return;
    }

    r = reasm_find(ctx, src, false);
    if (r == NULL || !r->active) {
        return;
    }
    if (dst != r->dst_addr) {
        reasm_reset_slot(r);
        return;
    }

    /* 中间片 */
    if (seg != 0U && seq == r->next_seq) {
        if (!reasm_append(r, pdu, pdu_len)) {
            return;
        }
        r->next_seq++;
        return;
    }

    /* 尾片 */
    if (seg == 0U && seq == r->next_seq) {
        if (!reasm_append(r, pdu, pdu_len)) {
            return;
        }
        emit_pdu(ctx, r->src_addr, r->dst_addr, r->buf, r->len);
        reasm_reset_slot(r);
        return;
    }

    ESP_LOGW(TAG, "multi seq err src=0x%02X mf=0x%02X expect=%u",
             src, mf, r->next_seq);
    reasm_reset_slot(r);
}

/** @return true 表示还可继续尝试解析下一帧 */
static bool parse_one(su3_frame_ctx_t *ctx)
{
    uint16_t i = 0;
    uint8_t pdu_len;
    uint16_t frame_len;
    uint16_t crc_calc;
    uint16_t crc_recv;

    while ((i + 1U) < ctx->stream_len) {
        if (ctx->stream[i] == SU3_PREAMBLE_0 && ctx->stream[i + 1U] == SU3_PREAMBLE_1) {
            break;
        }
        i++;
    }
    if (i > 0U) {
        stream_consume(ctx, i);
    }

    if (ctx->stream_len < SU3_FRAME_MIN_SIZE) {
        return false;
    }
    if (ctx->stream[0] != SU3_PREAMBLE_0 || ctx->stream[1] != SU3_PREAMBLE_1) {
        stream_consume(ctx, 1);
        return true;
    }

    pdu_len = ctx->stream[3];
    if (pdu_len > SU3_PDU_MAX_SIZE) {
        stream_consume(ctx, 1);
        return true;
    }

    frame_len = (uint16_t)(SU3_HEADER_SIZE + pdu_len + SU3_CRC_SIZE);
    if (ctx->stream_len < frame_len) {
        return false;
    }

    crc_calc = su3_crc16(ctx->stream, (size_t)(frame_len - SU3_CRC_SIZE));
    crc_recv = (uint16_t)ctx->stream[frame_len - 2U] |
               (uint16_t)((uint16_t)ctx->stream[frame_len - 1U] << 8);
    if (crc_calc != crc_recv) {
        ctx->crc_fail_cnt++;
        ESP_LOGW(TAG, "CRC fail calc=%04X recv=%04X len=%u",
                 crc_calc, crc_recv, frame_len);
        stream_consume(ctx, 1);
        return true;
    }

    ctx->frame_ok_cnt++;
    link_dispatch(ctx, ctx->stream, frame_len);
    stream_consume(ctx, frame_len);
    return true;
}

su3_frame_ctx_t *su3_frame_create(su3_pdu_cb_t on_pdu, void *user)
{
    su3_frame_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->on_pdu = on_pdu;
    ctx->user = user;
    return ctx;
}

void su3_frame_destroy(su3_frame_ctx_t *ctx)
{
    free(ctx);
}

void su3_frame_feed(su3_frame_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (ctx == NULL || data == NULL || len == 0) {
        return;
    }

    if (len > (SU3_RING_BUF_SIZE - ctx->stream_len)) {
        ctx->overflow_cnt++;
        ctx->stream_len = 0;
        if (len > SU3_RING_BUF_SIZE) {
            data += (len - SU3_RING_BUF_SIZE);
            len = SU3_RING_BUF_SIZE;
        }
        ESP_LOGW(TAG, "stream overflow, drop old (cnt=%u)", (unsigned)ctx->overflow_cnt);
    }

    memcpy(&ctx->stream[ctx->stream_len], data, len);
    ctx->stream_len = (uint16_t)(ctx->stream_len + len);

    while (parse_one(ctx)) {
    }
}

size_t su3_frame_build(uint8_t src, uint8_t dst, uint8_t flag,
                       const uint8_t *pdu, size_t pdu_len,
                       uint8_t *out, size_t out_cap)
{
    size_t frame_len;
    uint16_t crc;

    if (pdu == NULL || out == NULL || pdu_len == 0 || pdu_len > SU3_PDU_MAX_SIZE) {
        return 0;
    }

    frame_len = SU3_HEADER_SIZE + pdu_len + SU3_CRC_SIZE;
    if (out_cap < frame_len) {
        return 0;
    }

    out[0] = SU3_PREAMBLE_0;
    out[1] = SU3_PREAMBLE_1;
    out[2] = flag ? flag : SU3_MF_SINGLE;
    out[3] = (uint8_t)pdu_len;
    out[4] = src;
    out[5] = dst;
    memcpy(&out[SU3_HEADER_SIZE], pdu, pdu_len);

    crc = su3_crc16(out, SU3_HEADER_SIZE + pdu_len);
    out[SU3_HEADER_SIZE + pdu_len] = (uint8_t)(crc & 0xFFU);
    out[SU3_HEADER_SIZE + pdu_len + 1U] = (uint8_t)((crc >> 8) & 0xFFU);
    return frame_len;
}

void su3_frame_get_stats(const su3_frame_ctx_t *ctx,
                         uint32_t *frame_ok, uint32_t *crc_fail, uint32_t *overflow)
{
    if (ctx == NULL) {
        if (frame_ok) {
            *frame_ok = 0;
        }
        if (crc_fail) {
            *crc_fail = 0;
        }
        if (overflow) {
            *overflow = 0;
        }
        return;
    }
    if (frame_ok) {
        *frame_ok = ctx->frame_ok_cnt;
    }
    if (crc_fail) {
        *crc_fail = ctx->crc_fail_cnt;
    }
    if (overflow) {
        *overflow = ctx->overflow_cnt;
    }
}
