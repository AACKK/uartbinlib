/**
 * @file stm32_hal_interrupt.c
 * @brief uartbinlib icin STM32 HAL interrupt tabanli RX adapter ornegi.
 *
 * @example stm32_hal_interrupt.c
 *
 * Bu ornek, uartbinlib'i tek byte receive interrupt kullanarak STM32 UART'a
 * baglamanin temkinli ve robust bir yolunu gosterir. Portable kutuphane kodu
 * degil, bilincli olarak adapter taslagi seklinde yazilmistir.
 *
 * Tipik Cube/HAL baglantisi:
 *
 * @code
 * static stm32_uartbin_it_t g_uartbin;
 *
 * void app_init(void)
 * {
 *     stm32_uartbin_it_init(&g_uartbin, &huart1);
 * }
 *
 * void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
 * {
 *     stm32_uartbin_it_rx_complete_callback(&g_uartbin, huart);
 * }
 *
 * void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
 * {
 *     stm32_uartbin_it_error_callback(&g_uartbin, huart);
 * }
 *
 * void HAL_UART_AbortReceiveCpltCallback(UART_HandleTypeDef *huart)
 * {
 *     stm32_uartbin_it_abort_complete_callback(&g_uartbin, huart);
 * }
 *
 * void main_loop(void)
 * {
 *     stm32_uartbin_it_poll(&g_uartbin);
 * }
 * @endcode
 */
#include "uartbin.h"
#include <string.h>

/**
 * STM32 HAL interrupt RX ornegi.
 *
 * Gercek projede cihaz HAL header'ini include et, ornegin:
 * @code
 * #include "stm32f4xx_hal.h"
 * @endcode
 *
 * TX notu: uartbin_send tek cerceve icin write hook'u birden fazla kez cagirir.
 * Write hook donmeden once yazmayi bitirmeli veya byte'lari TX kuyruguna
 * kopyalamalidir. Bu ornek bu nedenle blocking HAL_UART_Transmit kullanir.
 */

typedef struct UART_HandleTypeDef UART_HandleTypeDef;
extern uint32_t HAL_GetTick(void);
extern int HAL_UART_Transmit(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size, uint32_t timeout);
extern int HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size);
extern int HAL_UART_AbortReceive_IT(UART_HandleTypeDef *huart);

#ifndef HAL_OK
#define HAL_OK 0
#endif

/**
 * @brief Tek interrupt-driven uartbin UART icin uygulama tarafi durum.
 */
typedef struct stm32_uartbin_it {
    /** Bu adapter ile iliskili HAL UART handle. */
    UART_HandleTypeDef *huart;

    /** Portable uartbin parser/sender context. */
    uartbin_t link;

    /** HAL_UART_Receive_IT tarafindan kullanilan tek byte RX alani. */
    uint8_t rx_byte;

    /** Tam paket payload buffer'i. Urunune gore buyut/kucult. */
    uint8_t rx_payload[4096];

    /** Adapter'in gordugu HAL seviyeli RX/start/abort hata sayisi. */
    volatile uint32_t hal_rx_errors;

    /** uartbinlib tarafindan bildirilen protokol seviyeli parser hata sayisi. */
    volatile uint32_t protocol_errors;
} stm32_uartbin_it_t;

/**
 * @brief Sonraki tek byte interrupt receive islemini baslat.
 *
 * @param port Adapter instance.
 * @return HAL RX istegini kabul ettiyse 0, aksi halde -1.
 */
static int stm32_uartbin_it_start_rx(stm32_uartbin_it_t *port)
{
    return HAL_UART_Receive_IT(port->huart, &port->rx_byte, 1u) == HAL_OK ? 0 : -1;
}

/**
 * @brief Blocking HAL transmit kullanan uartbin TX hook.
 *
 * uartbin_send() tek cerceve icin write hook'u birden fazla kez cagirabilecegi
 * icin blocking transmit kullanilir. Non-blocking TX icin bu hook'u statik
 * uygulama TX kuyrugu ile degistir.
 */
static int stm32_uartbin_it_write(const uint8_t *data, size_t len, void *user)
{
    stm32_uartbin_it_t *port = (stm32_uartbin_it_t *)user;

    if (len > 0xFFFFu) {
        return -1;
    }

    return HAL_UART_Transmit(port->huart, (uint8_t *)data, (uint16_t)len, 100u) == HAL_OK ? 0 : -1;
}

/**
 * @brief Uygulama packet handler'i.
 *
 * Bu fonksiyon yalnizca cerceve CRC basarili olduktan sonra cagrilir. Firmware
 * update kodu @p packet verisini CRC dogrulamasindan once degil burada islemeli
 * veya kopyalamalidir.
 */
static void stm32_uartbin_it_on_packet(const uartbin_packet_t *packet, void *user)
{
    (void)user;

    /*
     * packet->payload CRC kontrolunden gecmistir ve sonraki uartbin_feed
     * cagrimina kadar gecerlidir. Firmware/update komutlari icin CRC basarili
     * olduktan sonra burada kopyala/isle.
     */
    switch (packet->type) {
    default:
        break;
    }
}

/**
 * @brief Protokol hata callback'i.
 *
 * Tipik production kod sayaclari artirir, diagnostik kaydeder ve gerekirse
 * supervisor task'i bilgilendirir. Parser kendini zaten resetlemistir.
 */
static void stm32_uartbin_it_on_error(uartbin_error_t error, void *user)
{
    stm32_uartbin_it_t *port = (stm32_uartbin_it_t *)user;
    (void)error;

    port->protocol_errors++;
}

/**
 * @brief Interrupt adapter'i baslat ve ilk RX byte'i arm et.
 *
 * @param port Uygulamanin sahip oldugu adapter instance.
 * @param huart STM32 HAL UART handle.
 */
void stm32_uartbin_it_init(stm32_uartbin_it_t *port, UART_HandleTypeDef *huart)
{
    uartbin_config_t cfg;

    port->huart = huart;
    port->hal_rx_errors = 0u;
    port->protocol_errors = 0u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.write = stm32_uartbin_it_write;
    cfg.on_packet = stm32_uartbin_it_on_packet;
    cfg.on_error = stm32_uartbin_it_on_error;
    cfg.user = port;
    cfg.rx_payload_buffer = port->rx_payload;
    cfg.rx_payload_capacity = sizeof(port->rx_payload);
    cfg.rx_timeout_ms = 50u;

    uartbin_init(&port->link, &cfg);
    (void)stm32_uartbin_it_start_rx(port);
}

/**
 * @brief HAL_UART_RxCpltCallback() cagrimini uartbinlib'e ilet.
 *
 * Fonksiyon alinan byte'i besler ve hemen sonraki byte'i arm eder. Re-arm
 * basarisiz olursa protokol parser'i resetlenir; boylece partial cerceve
 * sonraki basarili receive islemine sizmaz.
 */
void stm32_uartbin_it_rx_complete_callback(stm32_uartbin_it_t *port, UART_HandleTypeDef *huart)
{
    if (huart == port->huart) {
        uartbin_feed_byte_at(&port->link, port->rx_byte, HAL_GetTick());
        if (stm32_uartbin_it_start_rx(port) != 0) {
            port->hal_rx_errors++;
            uartbin_reset(&port->link);
        }
    }
}

/**
 * @brief HAL_UART_ErrorCallback() cagrimini adapter'a ilet.
 *
 * Parity/framing/noise/overrun hatalarinda partial protokol cercevesini at,
 * HAL receive yolunu abort et ve reception'i tekrar arm etmeyi dene.
 */
void stm32_uartbin_it_error_callback(stm32_uartbin_it_t *port, UART_HandleTypeDef *huart)
{
    if (huart == port->huart) {
        port->hal_rx_errors++;
        uartbin_reset(&port->link);
        (void)HAL_UART_AbortReceive_IT(port->huart);
        (void)stm32_uartbin_it_start_rx(port);
    }
}

/**
 * @brief HAL_UART_AbortReceiveCpltCallback() cagrimini adapter'a ilet.
 *
 * Bazi HAL surumleri abort islemini asynchronous tamamlar. Bu callback, abort
 * yolu bittikten sonra RX'in tekrar arm edilmesini saglar.
 */
void stm32_uartbin_it_abort_complete_callback(stm32_uartbin_it_t *port, UART_HandleTypeDef *huart)
{
    if (huart == port->huart) {
        (void)stm32_uartbin_it_start_rx(port);
    }
}

/**
 * @brief Protokol parser icin periyodik timeout servisi.
 *
 * Bunu main loop, RTOS task veya periyodik timer context icinden cagir.
 */
void stm32_uartbin_it_poll(stm32_uartbin_it_t *port)
{
    uartbin_poll(&port->link, HAL_GetTick());
}
