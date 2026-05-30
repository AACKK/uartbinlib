/**
 * @file stm32_app_confirmed.c
 * @brief STM32 HAL interrupt RX ile uartbin_app confirmed mesaj ornegi.
 *
 * @example stm32_app_confirmed.c
 *
 * Bu ornek, ust seviye `uartbin_app.h` katmaninin STM32 tarafinda nasil
 * kullanilacagini gosterir. Uygulama `uartbin_app_send_confirmed()` ile payload
 * gonderir; peer taraf app katmani kullaniyorsa ACK otomatik doner. ACK
 * gelmezse retry `stm32_uartbin_app_it_poll()` icinde surulur.
 *
 * Tipik Cube/HAL baglantisi:
 *
 * @code
 * static stm32_uartbin_app_it_t g_link;
 *
 * void app_init(void)
 * {
 *     stm32_uartbin_app_it_init(&g_link, &huart1);
 * }
 *
 * void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
 * {
 *     stm32_uartbin_app_it_rx_complete_callback(&g_link, huart);
 * }
 *
 * void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
 * {
 *     stm32_uartbin_app_it_error_callback(&g_link, huart);
 * }
 *
 * void main_loop(void)
 * {
 *     stm32_uartbin_app_it_poll(&g_link);
 * }
 * @endcode
 */
#include "uartbin_app.h"

#include <string.h>

typedef struct UART_HandleTypeDef UART_HandleTypeDef;
extern uint32_t HAL_GetTick(void);
extern int HAL_UART_Transmit(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size, uint32_t timeout);
extern int HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size);
extern int HAL_UART_AbortReceive_IT(UART_HandleTypeDef *huart);

#ifndef HAL_OK
#define HAL_OK 0
#endif

enum {
    MSG_APP_PING = 0x10u,
    MSG_APP_STATUS = 0x11u,
    MSG_APP_RESULT = 0x12u
};

/**
 * @brief Tek interrupt-driven STM32 UART icin app katmani durum.
 */
typedef struct stm32_uartbin_app_it {
    /** Bu adapter ile iliskili HAL UART handle. */
    UART_HandleTypeDef *huart;

    /** Ust seviye otomatik ACK/retry context'i. */
    uartbin_app_t app;

    /** HAL_UART_Receive_IT tarafindan kullanilan tek byte RX alani. */
    uint8_t rx_byte;

    /** Tam mesaj payload buffer'i. */
    uint8_t rx_payload[256];

    /** Confirmed mesaj retry icin frame buffer. */
    uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 256u];

    /** HAL seviyeli RX/start/abort hata sayisi. */
    volatile uint32_t hal_rx_errors;

    /** uartbin/app seviyeli hata sayisi. */
    volatile uint32_t protocol_errors;

    /** Gonderilen confirmed mesaj ACK aldiginda artan sayac. */
    volatile uint32_t delivered_count;
} stm32_uartbin_app_it_t;

static int stm32_uartbin_app_it_start_rx(stm32_uartbin_app_it_t *port)
{
    return HAL_UART_Receive_IT(port->huart, &port->rx_byte, 1u) == HAL_OK ? 0 : -1;
}

/**
 * @brief Blocking HAL transmit kullanan app katmani TX hook.
 *
 * Non-blocking TX gerekiyorsa bu hook, byte'lari uygulama TX kuyruguna
 * kopyalayacak sekilde degistirilmelidir.
 */
static int stm32_uartbin_app_it_write(const uint8_t *data, size_t len, void *user)
{
    stm32_uartbin_app_it_t *port = (stm32_uartbin_app_it_t *)user;

    if (len > 0xFFFFu) {
        return -1;
    }

    return HAL_UART_Transmit(port->huart, (uint8_t *)data, (uint16_t)len, 100u) == HAL_OK ? 0 : -1;
}

/**
 * @brief Gercek uygulama mesajlari burada gorulur.
 *
 * Otomatik ACK paketleri bu callback'e dusmez. Gelen confirmed mesajlar icin
 * ACK, callback cagrilmadan once app katmani tarafindan gonderilmistir.
 */
static void stm32_uartbin_app_it_on_message(const uartbin_app_message_t *message, void *user)
{
    stm32_uartbin_app_it_t *port = (stm32_uartbin_app_it_t *)user;

    switch (message->type) {
    case MSG_APP_PING: {
        uint8_t result[] = { 0x01u };

        /*
         * Bu, transport ACK'ten ayri bir uygulama cevabidir. Peer bu sonucu da
         * confirmed almak istiyorsa send_confirmed kullanilabilir.
         */
        (void)uartbin_app_send_confirmed(&port->app, MSG_APP_RESULT, 0, result, sizeof(result));
        break;
    }

    default:
        break;
    }
}

static void stm32_uartbin_app_it_on_delivery(uint16_t seq, void *user)
{
    stm32_uartbin_app_it_t *port = (stm32_uartbin_app_it_t *)user;
    (void)seq;

    port->delivered_count++;
}

static void stm32_uartbin_app_it_on_error(uartbin_error_t error, void *user)
{
    stm32_uartbin_app_it_t *port = (stm32_uartbin_app_it_t *)user;
    (void)error;

    port->protocol_errors++;
}

/**
 * @brief App katmani adapter'ini baslat ve ilk RX byte'i arm et.
 */
void stm32_uartbin_app_it_init(stm32_uartbin_app_it_t *port, UART_HandleTypeDef *huart)
{
    uartbin_app_config_t cfg;

    memset(port, 0, sizeof(*port));
    port->huart = huart;

    memset(&cfg, 0, sizeof(cfg));
    cfg.write = stm32_uartbin_app_it_write;
    cfg.on_message = stm32_uartbin_app_it_on_message;
    cfg.on_delivery = stm32_uartbin_app_it_on_delivery;
    cfg.on_error = stm32_uartbin_app_it_on_error;
    cfg.user = port;
    cfg.rx_payload_buffer = port->rx_payload;
    cfg.rx_payload_capacity = sizeof(port->rx_payload);
    cfg.rx_timeout_ms = 50u;
    cfg.tx_retry_buffer = port->tx_retry_frame;
    cfg.tx_retry_capacity = sizeof(port->tx_retry_frame);
    cfg.tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS;
    cfg.tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES;

    uartbin_app_init(&port->app, &cfg);
    (void)stm32_uartbin_app_it_start_rx(port);
}

/**
 * @brief Confirmed status mesaji gonder.
 */
uartbin_status_t stm32_uartbin_app_it_send_status(stm32_uartbin_app_it_t *port,
                                                  const uint8_t *payload,
                                                  uint16_t payload_len)
{
    return uartbin_app_send_confirmed(&port->app, MSG_APP_STATUS, 0, payload, payload_len);
}

void stm32_uartbin_app_it_rx_complete_callback(stm32_uartbin_app_it_t *port,
                                               UART_HandleTypeDef *huart)
{
    if (huart == port->huart) {
        uartbin_app_feed_byte_at(&port->app, port->rx_byte, HAL_GetTick());
        if (stm32_uartbin_app_it_start_rx(port) != 0) {
            port->hal_rx_errors++;
        }
    }
}

void stm32_uartbin_app_it_error_callback(stm32_uartbin_app_it_t *port, UART_HandleTypeDef *huart)
{
    if (huart == port->huart) {
        port->hal_rx_errors++;
        uartbin_reset(&port->app.link);
        (void)HAL_UART_AbortReceive_IT(port->huart);
        (void)stm32_uartbin_app_it_start_rx(port);
    }
}

/**
 * @brief RX timeout ve confirmed retry servisi.
 */
void stm32_uartbin_app_it_poll(stm32_uartbin_app_it_t *port)
{
    uartbin_app_poll(&port->app, HAL_GetTick());
}
