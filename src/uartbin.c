/**
 * @file uartbin.c
 * @brief uartbin binary UART cerceve parser ve sender implementasyonu.
 *
 * Implementasyon bilerek kucuk ve deterministik tutulmustur:
 *
 * - dinamik bellek yok,
 * - platform cagrisi yok,
 * - tek gecisli RX parser,
 * - payload byte'lari geldikce CRC guncellenir,
 * - uygulama paketi yalnizca CRC basarili olduktan sonra gorur.
 */
#include "uartbin.h"

#include <string.h>

/**
 * @brief Ic RX parser durumlari.
 *
 * Parser iki byte'lik SOF dizisini arar, sonra sabit header, payload ve CRC
 * alanlarini alir. Her parse hatasinda SOF arama durumuna resetlenir; boylece
 * sonraki byte'lar linki yeniden senkronlayabilir.
 */
enum {
    /** UARTBIN_SOF0 bekleniyor. */
    UARTBIN_STATE_SOF0 = 0,

    /** UARTBIN_SOF0 gorulduktan sonra UARTBIN_SOF1 bekleniyor. */
    UARTBIN_STATE_SOF1,

    /** Sabit boyutlu header aliniyor. */
    UARTBIN_STATE_HEADER,

    /** Payload byte'lari kullanici RX buffer'ina aliniyor. */
    UARTBIN_STATE_PAYLOAD,

    /** Iki byte'lik CRC alani aliniyor. */
    UARTBIN_STATE_CRC
};

/**
 * @brief Little-endian 16-bit deger oku.
 *
 * @param data En az iki byte'a pointer.
 * @return Cozulmus 16-bit deger.
 */
static uint16_t uartbin_get_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/**
 * @brief Little-endian 16-bit deger yaz.
 *
 * @param data En az iki yazilabilir byte'a pointer.
 * @param value Kodlanacak deger.
 */
static void uartbin_put_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)(value >> 8);
}

/**
 * @brief Parser bookkeeping durumunu SOF arama durumuna resetle.
 *
 * Kullanici konfigurasyonunu degistirmez ve uygulama payload buffer'ini
 * temizlemez. Buffer icerigi bir sonraki basarili packet callback'e kadar
 * gecersiz kabul edilir.
 *
 * @param ctx RX durumu resetlenecek context.
 */
static void uartbin_reset_rx(uartbin_t *ctx)
{
    ctx->state = UARTBIN_STATE_SOF0;
    ctx->header_pos = 0u;
    ctx->payload_pos = 0u;
    ctx->payload_len = 0u;
    ctx->crc_pos = 0u;
    ctx->crc = 0xFFFFu;
}

/**
 * @brief Uygulama callback kaydettiyse parser hatasini ilet.
 *
 * @param ctx Hatayi algilayan context.
 * @param error Bildirilecek hata kodu.
 */
static void uartbin_report_error(uartbin_t *ctx, uartbin_error_t error)
{
    if (ctx->cfg.on_error != 0) {
        ctx->cfg.on_error(error, ctx->cfg.user);
    }
}

/**
 * @brief Ayarlanan write hook'u tek bitisik byte araligi icin cagir.
 *
 * @param ctx Baslatilmis context.
 * @param data Yazilacak veri.
 * @param len Yazilacak byte sayisi.
 * @return Basarida UARTBIN_OK veya anlik write/konfigurasyon hatasi.
 */
static uartbin_status_t uartbin_write_all(uartbin_t *ctx, const uint8_t *data, size_t len)
{
    if (len == 0u) {
        return UARTBIN_OK;
    }

    if (ctx->cfg.write == 0 || data == 0) {
        return UARTBIN_EINVAL;
    }

    return (ctx->cfg.write(data, len, ctx->cfg.user) == 0) ? UARTBIN_OK : UARTBIN_EWRITE;
}

/**
 * @brief RX parser'in su anda cerceve icinde olup olmadigini test et.
 *
 * @param ctx Incelenecek context.
 * @return Zaman asimi yonetimi parser'i aktif kabul etmeli ise sifir disi.
 */
static int uartbin_rx_active(const uartbin_t *ctx)
{
    return ctx->state != UARTBIN_STATE_SOF0;
}

static size_t uartbin_frame_len(uint16_t payload_len)
{
    return (size_t)UARTBIN_MAX_FRAME_OVERHEAD + payload_len;
}

static uartbin_status_t uartbin_build_frame(uint8_t *frame,
                                            size_t frame_capacity,
                                            uint8_t type,
                                            uint8_t flags,
                                            uint16_t seq,
                                            const uint8_t *payload,
                                            uint16_t payload_len,
                                            size_t *frame_len)
{
    size_t len = uartbin_frame_len(payload_len);
    uint16_t crc;

    if (frame == 0 || frame_len == 0 || (payload_len > 0u && payload == 0)) {
        return UARTBIN_EINVAL;
    }
    if (len > frame_capacity) {
        return UARTBIN_EPAYLOAD_TOO_LONG;
    }

    frame[0] = UARTBIN_SOF0;
    frame[1] = UARTBIN_SOF1;
    frame[2] = UARTBIN_VERSION;
    frame[3] = type;
    frame[4] = flags;
    frame[5] = 0u;
    uartbin_put_u16_le(&frame[6], seq);
    uartbin_put_u16_le(&frame[8], payload_len);
    if (payload_len > 0u) {
        memcpy(&frame[10], payload, payload_len);
    }

    crc = uartbin_crc16_ccitt(&frame[2], UARTBIN_HEADER_SIZE, 0xFFFFu);
    crc = uartbin_crc16_ccitt(payload, payload_len, crc);
    uartbin_put_u16_le(&frame[10u + payload_len], crc);

    *frame_len = len;
    return UARTBIN_OK;
}

static void uartbin_clear_retry(uartbin_t *ctx)
{
    ctx->retry_active = 0u;
    ctx->retry_timer_started = 0u;
    ctx->retry_count = 0u;
    ctx->retry_seq = 0u;
    ctx->retry_frame_len = 0u;
    ctx->retry_last_tx_time_ms = 0u;
}

static int uartbin_retry_enabled(const uartbin_t *ctx)
{
    return ctx->cfg.tx_retry_timeout_ms != 0u && ctx->cfg.tx_retry_max_retries != 0u;
}

static uartbin_status_t uartbin_check_retry_start(const uartbin_t *ctx, uint16_t payload_len)
{
    if (!uartbin_retry_enabled(ctx)) {
        return UARTBIN_OK;
    }
    if (ctx->retry_active != 0u) {
        return UARTBIN_EBUSY;
    }
    if (ctx->cfg.tx_retry_buffer == 0 || ctx->cfg.tx_retry_capacity == 0u) {
        return UARTBIN_ENO_BUFFER;
    }
    if (uartbin_frame_len(payload_len) > ctx->cfg.tx_retry_capacity) {
        return UARTBIN_EPAYLOAD_TOO_LONG;
    }

    return UARTBIN_OK;
}

static uartbin_status_t uartbin_start_retry(uartbin_t *ctx,
                                            uint8_t type,
                                            uint8_t flags,
                                            uint16_t seq,
                                            const uint8_t *payload,
                                            uint16_t payload_len)
{
    size_t frame_len = 0u;
    uartbin_status_t status;

    status = uartbin_check_retry_start(ctx, payload_len);
    if (status != UARTBIN_OK) {
        return status;
    }
    if (!uartbin_retry_enabled(ctx)) {
        return UARTBIN_OK;
    }

    status = uartbin_build_frame(ctx->cfg.tx_retry_buffer,
                                 ctx->cfg.tx_retry_capacity,
                                 type,
                                 flags,
                                 seq,
                                 payload,
                                 payload_len,
                                 &frame_len);
    if (status != UARTBIN_OK) {
        return status;
    }

    ctx->retry_active = 1u;
    ctx->retry_timer_started = 0u;
    ctx->retry_count = 0u;
    ctx->retry_seq = seq;
    ctx->retry_frame_len = (uint16_t)frame_len;
    ctx->retry_last_tx_time_ms = 0u;
    return UARTBIN_OK;
}

static void uartbin_poll_retry(uartbin_t *ctx, uint32_t now_ms)
{
    uint32_t elapsed;

    if (ctx->retry_active == 0u || !uartbin_retry_enabled(ctx)) {
        return;
    }

    if (ctx->retry_timer_started == 0u) {
        ctx->retry_timer_started = 1u;
        ctx->retry_last_tx_time_ms = now_ms;
        return;
    }

    elapsed = now_ms - ctx->retry_last_tx_time_ms;
    if (elapsed < ctx->cfg.tx_retry_timeout_ms) {
        return;
    }

    if (ctx->retry_count >= ctx->cfg.tx_retry_max_retries) {
        uartbin_report_error(ctx, UARTBIN_ERROR_RETRY_EXHAUSTED);
        uartbin_clear_retry(ctx);
        return;
    }

    if (uartbin_write_all(ctx, ctx->cfg.tx_retry_buffer, ctx->retry_frame_len) != UARTBIN_OK) {
        uartbin_report_error(ctx, UARTBIN_ERROR_RETRY_WRITE);
        uartbin_clear_retry(ctx);
        return;
    }

    ctx->retry_count++;
    ctx->retry_last_tx_time_ms = now_ms;
}

static void uartbin_poll_rx_timeout(uartbin_t *ctx, uint32_t now_ms)
{
    uint32_t elapsed;

    if (ctx->cfg.rx_timeout_ms == 0u || !uartbin_rx_active(ctx)) {
        return;
    }

    elapsed = now_ms - ctx->last_rx_time_ms;
    if (elapsed >= ctx->cfg.rx_timeout_ms) {
        uartbin_report_error(ctx, UARTBIN_ERROR_TIMEOUT);
        uartbin_reset_rx(ctx);
        ctx->last_rx_time_ms = now_ms;
    }
}

void uartbin_init(uartbin_t *ctx, const uartbin_config_t *config)
{
    if (ctx == 0 || config == 0) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *config;
    uartbin_reset_rx(ctx);
}

void uartbin_reset(uartbin_t *ctx)
{
    if (ctx != 0) {
        uartbin_reset_rx(ctx);
    }
}

void uartbin_cancel_retry(uartbin_t *ctx)
{
    if (ctx != 0) {
        uartbin_clear_retry(ctx);
    }
}

uint16_t uartbin_crc16_ccitt(const uint8_t *data, size_t len, uint16_t seed)
{
    uint16_t crc = seed;

    while (len > 0u) {
        crc ^= (uint16_t)(*data++) << 8;
        len--;
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

uartbin_status_t uartbin_send(uartbin_t *ctx,
                              uint8_t type,
                              uint8_t flags,
                              uint16_t seq,
                              const uint8_t *payload,
                              uint16_t payload_len)
{
    uint8_t sof[2] = { UARTBIN_SOF0, UARTBIN_SOF1 };
    uint8_t header[UARTBIN_HEADER_SIZE];
    uint8_t crc_bytes[UARTBIN_CRC_SIZE];
    uint16_t crc;
    uartbin_status_t status;

    if (ctx == 0 || (payload_len > 0u && payload == 0)) {
        return UARTBIN_EINVAL;
    }

    header[0] = UARTBIN_VERSION;
    header[1] = type;
    header[2] = flags;
    header[3] = 0u;
    uartbin_put_u16_le(&header[4], seq);
    uartbin_put_u16_le(&header[6], payload_len);

    crc = uartbin_crc16_ccitt(header, sizeof(header), 0xFFFFu);
    crc = uartbin_crc16_ccitt(payload, payload_len, crc);
    uartbin_put_u16_le(crc_bytes, crc);

    status = uartbin_write_all(ctx, sof, sizeof(sof));
    if (status != UARTBIN_OK) {
        return status;
    }

    status = uartbin_write_all(ctx, header, sizeof(header));
    if (status != UARTBIN_OK) {
        return status;
    }

    status = uartbin_write_all(ctx, payload, payload_len);
    if (status != UARTBIN_OK) {
        return status;
    }

    return uartbin_write_all(ctx, crc_bytes, sizeof(crc_bytes));
}

uint16_t uartbin_next_seq(uartbin_t *ctx)
{
    if (ctx == 0) {
        return 0u;
    }

    ctx->tx_seq++;
    if (ctx->tx_seq == 0u) {
        ctx->tx_seq = 1u;
    }

    return ctx->tx_seq;
}

uartbin_status_t uartbin_send_request(uartbin_t *ctx,
                                      uint8_t type,
                                      uint8_t flags,
                                      const uint8_t *payload,
                                      uint16_t payload_len)
{
    uint8_t tx_flags;
    uint16_t seq;
    uartbin_status_t status;

    if (ctx == 0) {
        return UARTBIN_EINVAL;
    }
    if (payload_len > 0u && payload == 0) {
        return UARTBIN_EINVAL;
    }
    status = uartbin_check_retry_start(ctx, payload_len);
    if (status != UARTBIN_OK) {
        return status;
    }

    tx_flags = (uint8_t)(flags | UARTBIN_FLAG_REQUEST);
    seq = uartbin_next_seq(ctx);
    status = uartbin_start_retry(ctx, type, tx_flags, seq, payload, payload_len);
    if (status != UARTBIN_OK) {
        return status;
    }

    status = uartbin_send(ctx, type, tx_flags, seq, payload, payload_len);
    if (status != UARTBIN_OK) {
        uartbin_clear_retry(ctx);
    }

    return status;
}

uartbin_status_t uartbin_send_response(uartbin_t *ctx,
                                       const uartbin_packet_t *request,
                                       uint8_t type,
                                       uint8_t flags,
                                       const uint8_t *payload,
                                       uint16_t payload_len)
{
    if (ctx == 0 || request == 0) {
        return UARTBIN_EINVAL;
    }

    return uartbin_send(ctx,
                        type,
                        (uint8_t)(flags | UARTBIN_FLAG_RESPONSE),
                        request->seq,
                        payload,
                        payload_len);
}

uartbin_status_t uartbin_send_event(uartbin_t *ctx,
                                    uint8_t type,
                                    uint8_t flags,
                                    const uint8_t *payload,
                                    uint16_t payload_len)
{
    uint8_t tx_flags;
    uint16_t seq;
    uartbin_status_t status;

    if (ctx == 0) {
        return UARTBIN_EINVAL;
    }
    if (payload_len > 0u && payload == 0) {
        return UARTBIN_EINVAL;
    }
    status = uartbin_check_retry_start(ctx, payload_len);
    if (status != UARTBIN_OK) {
        return status;
    }

    tx_flags = (uint8_t)(flags | UARTBIN_FLAG_EVENT);
    seq = uartbin_next_seq(ctx);
    status = uartbin_start_retry(ctx, type, tx_flags, seq, payload, payload_len);
    if (status != UARTBIN_OK) {
        return status;
    }

    status = uartbin_send(ctx, type, tx_flags, seq, payload, payload_len);
    if (status != UARTBIN_OK) {
        uartbin_clear_retry(ctx);
    }

    return status;
}

void uartbin_feed(uartbin_t *ctx, const uint8_t *data, size_t len)
{
    if (ctx == 0 || data == 0) {
        return;
    }

    while (len-- > 0u) {
        uartbin_feed_byte(ctx, *data++);
    }
}

void uartbin_feed_byte(uartbin_t *ctx, uint8_t byte)
{
    uartbin_feed_byte_at(ctx, byte, ctx != 0 ? ctx->last_rx_time_ms : 0u);
}

void uartbin_feed_at(uartbin_t *ctx, const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (ctx == 0 || data == 0) {
        return;
    }

    while (len-- > 0u) {
        uartbin_feed_byte_at(ctx, *data++, now_ms);
    }
}

void uartbin_poll(uartbin_t *ctx, uint32_t now_ms)
{
    if (ctx == 0) {
        return;
    }

    uartbin_poll_retry(ctx, now_ms);
    uartbin_poll_rx_timeout(ctx, now_ms);
}

/**
 * @brief RX state machine'e tek byte besle.
 *
 * Bu internal state machine parser'in merkezidir. Public fonksiyon tum mantigi
 * tek yerde tutar; boylece block ve byte feed yollari ayni davranir.
 */
void uartbin_feed_byte_at(uartbin_t *ctx, uint8_t byte, uint32_t now_ms)
{
    if (ctx == 0) {
        return;
    }

    uartbin_poll_rx_timeout(ctx, now_ms);
    ctx->last_rx_time_ms = now_ms;

    switch (ctx->state) {
    case UARTBIN_STATE_SOF0:
        /* Ilk start-of-frame byte'i gorunene kadar noise ignore edilir. */
        if (byte == UARTBIN_SOF0) {
            ctx->state = UARTBIN_STATE_SOF1;
        }
        break;

    case UARTBIN_STATE_SOF1:
        /*
         * Tekrarlanan SOF0 bizi SOF1 icin hazir tutar. Bu, A5 A5 5A gibi byte
         * akislarinda ikinci A5'i atmadan yeniden senkron olmaya yardim eder.
         */
        if (byte == UARTBIN_SOF1) {
            ctx->state = UARTBIN_STATE_HEADER;
            ctx->header_pos = 0u;
            ctx->crc = 0xFFFFu;
        } else if (byte != UARTBIN_SOF0) {
            ctx->state = UARTBIN_STATE_SOF0;
        }
        break;

    case UARTBIN_STATE_HEADER:
        /* Header sabit boyutludur; uzunluk ve surum erken dogrulanabilir. */
        ctx->header[ctx->header_pos++] = byte;
        if (ctx->header_pos == UARTBIN_HEADER_SIZE) {
            ctx->crc = uartbin_crc16_ccitt(ctx->header, UARTBIN_HEADER_SIZE, ctx->crc);
            ctx->payload_len = uartbin_get_u16_le(&ctx->header[6]);
            ctx->payload_pos = 0u;

            if (ctx->header[0] != UARTBIN_VERSION) {
                uartbin_report_error(ctx, UARTBIN_ERROR_BAD_VERSION);
                uartbin_reset_rx(ctx);
            } else if (ctx->payload_len > ctx->cfg.rx_payload_capacity) {
                uartbin_report_error(ctx, UARTBIN_ERROR_BAD_LENGTH);
                uartbin_reset_rx(ctx);
            } else if (ctx->payload_len > 0u && ctx->cfg.rx_payload_buffer == 0) {
                uartbin_report_error(ctx, UARTBIN_ERROR_RX_OVERFLOW);
                uartbin_reset_rx(ctx);
            } else {
                ctx->state = (ctx->payload_len == 0u) ? UARTBIN_STATE_CRC : UARTBIN_STATE_PAYLOAD;
                ctx->crc_pos = 0u;
            }
        }
        break;

    case UARTBIN_STATE_PAYLOAD:
        /* Bu duruma girmeden uzunluk dogrulandigi icin yazma guvenlidir. */
        ctx->cfg.rx_payload_buffer[ctx->payload_pos] = byte;
        ctx->payload_pos++;
        ctx->crc = uartbin_crc16_ccitt(&byte, 1u, ctx->crc);
        if (ctx->payload_pos == ctx->payload_len) {
            ctx->state = UARTBIN_STATE_CRC;
            ctx->crc_pos = 0u;
        }
        break;

    case UARTBIN_STATE_CRC:
        /* Packet callback yalnizca alinan CRC eslestikten sonra cagrilir. */
        ctx->crc_bytes[ctx->crc_pos++] = byte;
        if (ctx->crc_pos == UARTBIN_CRC_SIZE) {
            uint16_t expected = uartbin_get_u16_le(ctx->crc_bytes);

            if (expected == ctx->crc) {
                uartbin_packet_t packet;

                packet.type = ctx->header[1];
                packet.flags = ctx->header[2];
                packet.seq = uartbin_get_u16_le(&ctx->header[4]);
                packet.payload = ctx->cfg.rx_payload_buffer;
                packet.payload_len = ctx->payload_len;
                if ((packet.flags & UARTBIN_FLAG_RESPONSE) != 0u &&
                    ctx->retry_active != 0u &&
                    packet.seq == ctx->retry_seq) {
                    uartbin_clear_retry(ctx);
                }

                if (ctx->cfg.on_packet != 0) {
                    ctx->cfg.on_packet(&packet, ctx->cfg.user);
                }
            } else {
                uartbin_report_error(ctx, UARTBIN_ERROR_CRC);
            }

            uartbin_reset_rx(ctx);
        }
        break;

    default:
        uartbin_reset_rx(ctx);
        break;
    }
}
