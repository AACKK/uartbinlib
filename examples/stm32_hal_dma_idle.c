/**
 * @file stm32_hal_dma_idle.c
 * @brief uartbinlib icin STM32 HAL DMA idle-line RX adapter ornegi.
 *
 * @example stm32_hal_dma_idle.c
 *
 * Bu ornek uartbinlib'in HAL_UARTEx_ReceiveToIdle_DMA ile nasil beslenecegini
 * gosterir. DMA byte'lari kucuk bir transport buffer'ina alir; uartbinlib ise
 * dogrulanmis her cerceve payload'unu daha buyuk uygulama RX payload buffer'ina
 * kopyalar.
 *
 * Tipik Cube/HAL baglantisi:
 *
 * @code
 * static stm32_uartbin_dma_t g_uartbin;
 *
 * void app_init(void)
 * {
 *     stm32_uartbin_dma_init(&g_uartbin, &huart1, hdma_usart1_rx);
 * }
 *
 * void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
 * {
 *     stm32_uartbin_dma_rx_event_callback(&g_uartbin, huart, size);
 * }
 *
 * void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
 * {
 *     stm32_uartbin_dma_error_callback(&g_uartbin, huart);
 * }
 *
 * void main_loop(void)
 * {
 *     stm32_uartbin_dma_poll(&g_uartbin);
 * }
 * @endcode
 */
#include "uartbin.h"
#include <string.h>

/**
 * STM32 HAL DMA + idle-line RX ornegi.
 *
 * HAL_UARTEx_ReceiveToIdle_DMA kullanir ve alinan her blogu uartbin'e besler.
 * TX, blocking HAL_UART_Transmit kullanir; cunku uartbin_send tek cerceve icin
 * write hook'u birkac kez cagirir. Non-blocking transmit gerekiyorsa bunu kendi
 * TX queue yapin ile degistir.
 */

typedef struct UART_HandleTypeDef UART_HandleTypeDef;
typedef struct DMA_HandleTypeDef DMA_HandleTypeDef;
extern uint32_t HAL_GetTick(void);
extern int HAL_UART_Transmit(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size, uint32_t timeout);
extern int HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size);
extern int HAL_UART_AbortReceive(UART_HandleTypeDef *huart);
extern void __HAL_DMA_DISABLE_IT(DMA_HandleTypeDef *hdma, uint32_t interrupt);

#ifndef HAL_OK
#define HAL_OK 0
#endif

#ifndef DMA_IT_HT
#define DMA_IT_HT 0x00000004u
#endif

/**
 * @brief Tek DMA idle-line uartbin UART icin uygulama tarafi durum.
 */
typedef struct stm32_uartbin_dma {
    /** Bu adapter ile iliskili HAL UART handle. */
    UART_HandleTypeDef *huart;

    /** Half-transfer interrupt'larini kapatmak icin kullanilan HAL DMA RX handle. */
    DMA_HandleTypeDef *hdma_rx;

    /** Portable uartbin parser/sender context. */
    uartbin_t link;

    /** DMA transport buffer; tam paket sigdirmasi gerekmez. */
    uint8_t dma_rx_buffer[256];

    /** uartbinlib tarafindan kullanilan tam paket payload buffer'i. */
    uint8_t rx_payload[4096];

    /** Adapter'in gordugu HAL seviyeli RX/start/abort hata sayisi. */
    volatile uint32_t hal_rx_errors;

    /** uartbinlib tarafindan bildirilen protokol seviyeli parser hata sayisi. */
    volatile uint32_t protocol_errors;
} stm32_uartbin_dma_t;

/**
 * @brief ReceiveToIdle DMA reception baslat veya yeniden baslat.
 *
 * Bu adapter yalnizca idle veya buffer-complete event istedigi icin
 * half-transfer interrupt'lari kapatilir. Uygulamanin half-transfer event'e
 * ihtiyaci varsa yalnizca yeni alinan byte'lari besle ve DMA offset'lerini
 * dikkatli takip et.
 */
static int stm32_uartbin_dma_start_rx(stm32_uartbin_dma_t *port)
{
    int status = HAL_UARTEx_ReceiveToIdle_DMA(port->huart,
                                              port->dma_rx_buffer,
                                              sizeof(port->dma_rx_buffer));
    if (status == HAL_OK) {
        __HAL_DMA_DISABLE_IT(port->hdma_rx, DMA_IT_HT);
        return 0;
    }

    return -1;
}

/**
 * @brief Blocking HAL transmit kullanan uartbin TX hook.
 *
 * uartbin_send() coklu write sozlesmesi ile dogru calismasi icin blocking
 * transmit kullanilir. Non-blocking sistemlerde statik TX queue ile degistir.
 */
static int stm32_uartbin_dma_write(const uint8_t *data, size_t len, void *user)
{
    stm32_uartbin_dma_t *port = (stm32_uartbin_dma_t *)user;

    if (len > 0xFFFFu) {
        return -1;
    }

    return HAL_UART_Transmit(port->huart, (uint8_t *)data, (uint16_t)len, 100u) == HAL_OK ? 0 : -1;
}

/**
 * @brief Uygulama packet handler'i.
 *
 * Tam cerceve CRC'den gectikten sonra cagrilir. Payload pointer adapter RX
 * payload buffer'ini isaret eder ve sonraki feed'e kadar gecerlidir.
 */
static void stm32_uartbin_dma_on_packet(const uartbin_packet_t *packet, void *user)
{
    (void)packet;
    (void)user;
    /* Payload burada tamamlanmis ve CRC kontrolunden gecmistir. */
}

/**
 * @brief Protokol hata callback'i.
 *
 * Bunu diagnostik, sayaclar ve opsiyonel link-level recovery politikasi icin
 * kullan. HAL recovery, stm32_uartbin_dma_error_callback() ile yapilir.
 */
static void stm32_uartbin_dma_on_error(uartbin_error_t error, void *user)
{
    stm32_uartbin_dma_t *port = (stm32_uartbin_dma_t *)user;
    (void)error;

    port->protocol_errors++;
}

/**
 * @brief DMA idle-line adapter'i baslat ve DMA RX'i arm et.
 *
 * @param port Uygulamanin sahip oldugu adapter instance.
 * @param huart STM32 HAL UART handle.
 * @param hdma_rx UART RX icin STM32 HAL DMA handle.
 */
void stm32_uartbin_dma_init(stm32_uartbin_dma_t *port,
                            UART_HandleTypeDef *huart,
                            DMA_HandleTypeDef *hdma_rx)
{
    uartbin_config_t cfg;

    port->huart = huart;
    port->hdma_rx = hdma_rx;
    port->hal_rx_errors = 0u;
    port->protocol_errors = 0u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.write = stm32_uartbin_dma_write;
    cfg.on_packet = stm32_uartbin_dma_on_packet;
    cfg.on_error = stm32_uartbin_dma_on_error;
    cfg.user = port;
    cfg.rx_payload_buffer = port->rx_payload;
    cfg.rx_payload_capacity = sizeof(port->rx_payload);
    cfg.rx_timeout_ms = 50u;

    uartbin_init(&port->link, &cfg);
    (void)stm32_uartbin_dma_start_rx(port);
}

/**
 * @brief HAL_UARTEx_RxEventCallback() cagrimini uartbinlib'e ilet.
 *
 * @param port Adapter instance.
 * @param huart HAL tarafindan verilen UART handle.
 * @param size dma_rx_buffer icindeki gecerli byte sayisi.
 *
 * Size kontrolu beklenmeyen HAL/kullanici hatalarina karsi korur. Blok
 * beslendikten sonra fonksiyon ReceiveToIdle DMA'i hemen yeniden baslatir.
 */
void stm32_uartbin_dma_rx_event_callback(stm32_uartbin_dma_t *port,
                                         UART_HandleTypeDef *huart,
                                         uint16_t size)
{
    if (huart == port->huart) {
        if (size <= sizeof(port->dma_rx_buffer)) {
            uartbin_feed_at(&port->link, port->dma_rx_buffer, size, HAL_GetTick());
        } else {
            port->hal_rx_errors++;
            uartbin_reset(&port->link);
        }

        if (stm32_uartbin_dma_start_rx(port) != 0) {
            port->hal_rx_errors++;
            uartbin_reset(&port->link);
        }
    }
}

/**
 * @brief HAL_UART_ErrorCallback() cagrimini DMA adapter'a ilet.
 *
 * UART noise/framing/parity/overrun hatalarinda partial protokol cercevesini
 * at, HAL receive yolunu abort et ve DMA idle-line reception'i yeniden baslat.
 */
void stm32_uartbin_dma_error_callback(stm32_uartbin_dma_t *port, UART_HandleTypeDef *huart)
{
    if (huart == port->huart) {
        port->hal_rx_errors++;
        uartbin_reset(&port->link);
        (void)HAL_UART_AbortReceive(port->huart);
        (void)stm32_uartbin_dma_start_rx(port);
    }
}

/**
 * @brief Protokol parser icin periyodik timeout servisi.
 *
 * Bunu main loop, RTOS task veya periyodik timer context icinden cagir.
 */
void stm32_uartbin_dma_poll(stm32_uartbin_dma_t *port)
{
    uartbin_poll(&port->link, HAL_GetTick());
}
