/**
 * @file stm32_hal_dma_idle.c
 * @brief STM32 HAL DMA idle-line RX adapter example for uartbinlib.
 *
 * @example stm32_hal_dma_idle.c
 *
 * This example shows how to feed uartbinlib from HAL_UARTEx_ReceiveToIdle_DMA.
 * DMA receives bytes into a small transport buffer; uartbinlib then copies each
 * validated frame payload into the larger application RX payload buffer.
 *
 * Typical Cube/HAL wiring:
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
 * STM32 HAL DMA + idle-line RX example.
 *
 * Uses HAL_UARTEx_ReceiveToIdle_DMA and feeds each received block to uartbin.
 * TX uses blocking HAL_UART_Transmit because uartbin_send calls the write hook
 * several times per frame. Replace it with your own TX queue if you need
 * non-blocking transmit.
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
 * @brief Application-side state for one DMA idle-line uartbin UART.
 */
typedef struct stm32_uartbin_dma {
    /** HAL UART handle associated with this adapter. */
    UART_HandleTypeDef *huart;

    /** HAL DMA RX handle used to disable half-transfer interrupts. */
    DMA_HandleTypeDef *hdma_rx;

    /** Portable uartbin parser/sender context. */
    uartbin_t link;

    /** DMA transport buffer; it does not need to fit a whole packet. */
    uint8_t dma_rx_buffer[256];

    /** Complete packet payload buffer used by uartbinlib. */
    uint8_t rx_payload[4096];

    /** Count of HAL-level RX/start/abort errors observed by the adapter. */
    volatile uint32_t hal_rx_errors;

    /** Count of protocol-level parser errors reported by uartbinlib. */
    volatile uint32_t protocol_errors;
} stm32_uartbin_dma_t;

/**
 * @brief Start or restart ReceiveToIdle DMA reception.
 *
 * Half-transfer interrupts are disabled because this adapter only wants idle or
 * buffer-complete events. If your application needs half-transfer events, feed
 * only the newly received bytes and track DMA offsets carefully.
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
 * @brief uartbin TX hook using blocking HAL transmit.
 *
 * Blocking transmit is used for correctness with uartbin_send()'s multi-call
 * write contract. Replace with a static TX queue for non-blocking systems.
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
 * @brief Application packet handler.
 *
 * This is called after a complete frame passed CRC. The payload pointer aliases
 * the adapter RX payload buffer and remains valid until the next feed.
 */
static void stm32_uartbin_dma_on_packet(const uartbin_packet_t *packet, void *user)
{
    (void)packet;
    (void)user;
    /* Payload is complete and CRC-checked here. */
}

/**
 * @brief Protocol error callback.
 *
 * Use this for diagnostics, counters, and optional link-level recovery policy.
 * HAL recovery is handled by stm32_uartbin_dma_error_callback().
 */
static void stm32_uartbin_dma_on_error(uartbin_error_t error, void *user)
{
    stm32_uartbin_dma_t *port = (stm32_uartbin_dma_t *)user;
    (void)error;

    port->protocol_errors++;
}

/**
 * @brief Initialize the DMA idle-line adapter and arm DMA RX.
 *
 * @param port Adapter instance owned by the application.
 * @param huart STM32 HAL UART handle.
 * @param hdma_rx STM32 HAL DMA handle for UART RX.
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
 * @brief Forward HAL_UARTEx_RxEventCallback() into uartbinlib.
 *
 * @param port Adapter instance.
 * @param huart UART handle passed by HAL.
 * @param size Number of valid bytes in dma_rx_buffer.
 *
 * The size check protects against unexpected HAL/user misuse. After feeding the
 * block, the function immediately restarts ReceiveToIdle DMA.
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
 * @brief Forward HAL_UART_ErrorCallback() into the DMA adapter.
 *
 * On UART noise/framing/parity/overrun errors, discard any partial protocol
 * frame, abort the HAL receive path, and restart DMA idle-line reception.
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
 * @brief Periodic timeout service for the protocol parser.
 *
 * Call this from a main loop, RTOS task, or periodic timer context.
 */
void stm32_uartbin_dma_poll(stm32_uartbin_dma_t *port)
{
    uartbin_poll(&port->link, HAL_GetTick());
}
