/**
 * @file uartbin_app.c
 * @brief uartbin uzerinde otomatik ACK/retry yapan app katmani.
 */
#include "uartbin_app.h"

#include <string.h>

static uint8_t uartbin_app_public_flags(uint8_t flags)
{
    return (uint8_t)(flags & (uint8_t)~(UARTBIN_FLAG_REQUEST |
                                       UARTBIN_FLAG_RESPONSE |
                                       UARTBIN_FLAG_EVENT |
                                       UARTBIN_APP_FLAG_CONFIRMED |
                                       UARTBIN_APP_FLAG_ACK));
}

static void uartbin_app_on_error(uartbin_error_t error, void *user)
{
    uartbin_app_t *app = (uartbin_app_t *)user;

    if (app != 0 &&
        (error == UARTBIN_ERROR_RETRY_EXHAUSTED || error == UARTBIN_ERROR_RETRY_WRITE)) {
        app->tx_confirmed_pending = 0u;
        app->tx_confirmed_seq = 0u;
    }

    if (app != 0 && app->cfg.on_error != 0) {
        app->cfg.on_error(error, app->cfg.user);
    }
}

static int uartbin_app_write(const uint8_t *data, size_t len, void *user)
{
    uartbin_app_t *app = (uartbin_app_t *)user;

    if (app == 0 || app->cfg.write == 0) {
        return -1;
    }

    return app->cfg.write(data, len, app->cfg.user);
}

static void uartbin_app_deliver(const uartbin_app_t *app, const uartbin_packet_t *packet)
{
    uartbin_app_message_t message;

    if (app->cfg.on_message == 0) {
        return;
    }

    message.type = packet->type;
    message.flags = uartbin_app_public_flags(packet->flags);
    message.confirmed = ((packet->flags & UARTBIN_APP_FLAG_CONFIRMED) != 0u) ? 1u : 0u;
    message.seq = packet->seq;
    message.payload = packet->payload;
    message.payload_len = packet->payload_len;
    app->cfg.on_message(&message, app->cfg.user);
}

static int uartbin_app_is_duplicate_confirmed(const uartbin_app_t *app,
                                              const uartbin_packet_t *packet)
{
    return app->last_rx_confirmed_valid != 0u &&
           app->last_rx_confirmed_type == packet->type &&
           app->last_rx_confirmed_seq == packet->seq;
}

static void uartbin_app_on_packet(const uartbin_packet_t *packet, void *user)
{
    uartbin_app_t *app = (uartbin_app_t *)user;

    if (app == 0 || packet == 0) {
        return;
    }

    if ((packet->flags & UARTBIN_FLAG_RESPONSE) != 0u) {
        if ((packet->flags & UARTBIN_APP_FLAG_ACK) != 0u &&
            app->tx_confirmed_pending != 0u &&
            packet->seq == app->tx_confirmed_seq) {
            app->tx_confirmed_pending = 0u;
            app->tx_confirmed_seq = 0u;
            if (app->cfg.on_delivery != 0) {
                app->cfg.on_delivery(packet->seq, app->cfg.user);
            }
        }
        return;
    }

    if ((packet->flags & UARTBIN_APP_FLAG_CONFIRMED) != 0u) {
        int duplicate = uartbin_app_is_duplicate_confirmed(app, packet);

        (void)uartbin_send_response(&app->link, packet, packet->type, UARTBIN_APP_FLAG_ACK, 0, 0u);
        if (duplicate) {
            return;
        }

        app->last_rx_confirmed_valid = 1u;
        app->last_rx_confirmed_type = packet->type;
        app->last_rx_confirmed_seq = packet->seq;
    }

    uartbin_app_deliver(app, packet);
}

void uartbin_app_init(uartbin_app_t *app, const uartbin_app_config_t *config)
{
    uartbin_config_t link_cfg;

    if (app == 0 || config == 0) {
        return;
    }

    memset(app, 0, sizeof(*app));
    app->cfg = *config;

    memset(&link_cfg, 0, sizeof(link_cfg));
    link_cfg.write = uartbin_app_write;
    link_cfg.on_packet = uartbin_app_on_packet;
    link_cfg.on_error = uartbin_app_on_error;
    link_cfg.user = app;
    link_cfg.rx_payload_buffer = config->rx_payload_buffer;
    link_cfg.rx_payload_capacity = config->rx_payload_capacity;
    link_cfg.rx_timeout_ms = config->rx_timeout_ms;
    link_cfg.tx_retry_buffer = config->tx_retry_buffer;
    link_cfg.tx_retry_capacity = config->tx_retry_capacity;
    link_cfg.tx_retry_timeout_ms = config->tx_retry_timeout_ms;
    link_cfg.tx_retry_max_retries = config->tx_retry_max_retries;

    uartbin_init(&app->link, &link_cfg);
}

uartbin_status_t uartbin_app_send(uartbin_app_t *app,
                                  uint8_t type,
                                  uint8_t flags,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
    if (app == 0) {
        return UARTBIN_EINVAL;
    }

    return uartbin_send(&app->link,
                        type,
                        (uint8_t)(uartbin_app_public_flags(flags) | UARTBIN_FLAG_EVENT),
                        uartbin_next_seq(&app->link),
                        payload,
                        payload_len);
}

uartbin_status_t uartbin_app_send_confirmed(uartbin_app_t *app,
                                            uint8_t type,
                                            uint8_t flags,
                                            const uint8_t *payload,
                                            uint16_t payload_len)
{
    uartbin_status_t status;

    if (app == 0) {
        return UARTBIN_EINVAL;
    }

    status = uartbin_send_event(&app->link,
                                type,
                                (uint8_t)(uartbin_app_public_flags(flags) | UARTBIN_APP_FLAG_CONFIRMED),
                                payload,
                                payload_len);
    if (status == UARTBIN_OK) {
        app->tx_confirmed_pending = 1u;
        app->tx_confirmed_seq = app->link.tx_seq;
    }

    return status;
}

void uartbin_app_cancel(uartbin_app_t *app)
{
    if (app != 0) {
        app->tx_confirmed_pending = 0u;
        app->tx_confirmed_seq = 0u;
        uartbin_cancel_retry(&app->link);
    }
}

void uartbin_app_feed(uartbin_app_t *app, const uint8_t *data, size_t len)
{
    if (app != 0) {
        uartbin_feed(&app->link, data, len);
    }
}

void uartbin_app_feed_at(uartbin_app_t *app, const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (app != 0) {
        uartbin_feed_at(&app->link, data, len, now_ms);
    }
}

void uartbin_app_feed_byte(uartbin_app_t *app, uint8_t byte)
{
    if (app != 0) {
        uartbin_feed_byte(&app->link, byte);
    }
}

void uartbin_app_feed_byte_at(uartbin_app_t *app, uint8_t byte, uint32_t now_ms)
{
    if (app != 0) {
        uartbin_feed_byte_at(&app->link, byte, now_ms);
    }
}

void uartbin_app_poll(uartbin_app_t *app, uint32_t now_ms)
{
    if (app != 0) {
        uartbin_poll(&app->link, now_ms);
    }
}
