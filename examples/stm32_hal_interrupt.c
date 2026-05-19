/**
 * @file stm32_hal_interrupt.c
 * @brief STM32 HAL interrupt-based RX adapter example for uartbinlib.
 *
 * @example stm32_hal_interrupt.c
 *
 * This example shows a conservative, robust way to connect uartbinlib to an
 * STM32 UART using one-byte receive interrupts. It is intentionally written as
 * an adapter sketch instead of portable library code.
 *
 * Typical Cube/HAL wiring:
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
 * STM32 HAL interrupt RX example.
 *
 * In a real project include your device HAL header, for example:
 * @code
 * #include "stm32f4xx_hal.h"
 * @endcode
 *
 * TX note: uartbin_send calls the write hook more than once per frame. The
 * write hook must finish the write or copy bytes into a TX queue before
 * returning. This example uses blocking HAL_UART_Transmit for that reason.
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
 * @brief Application-side state for one interrupt-driven uartbin UART.
 */
typedef struct stm32_uartbin_it {
    /** HAL UART handle associated with this adapter. */
    UART_HandleTypeDef *huart;

    /** Portable uartbin parser/sender context. */
    uartbin_t link;

    /** Single-byte RX storage used by HAL_UART_Receive_IT. */
    uint8_t rx_byte;

    /** Complete packet payload buffer. Increase/decrease for your product. */
    uint8_t rx_payload[4096];

    /** Count of HAL-level RX/start/abort errors observed by the adapter. */
    volatile uint32_t hal_rx_errors;

    /** Count of protocol-level parser errors reported by uartbinlib. */
    volatile uint32_t protocol_errors;
} stm32_uartbin_it_t;

/**
 * @brief Start the next one-byte interrupt receive operation.
 *
 * @param port Adapter instance.
 * @return 0 when HAL accepted the RX request, otherwise -1.
 */
static int stm32_uartbin_it_start_rx(stm32_uartbin_it_t *port)
{
    return HAL_UART_Receive_IT(port->huart, &port->rx_byte, 1u) == HAL_OK ? 0 : -1;
}

/**
 * @brief uartbin TX hook using blocking HAL transmit.
 *
 * Blocking transmit is used because uartbin_send() may call the write hook
 * multiple times per frame. For non-blocking TX, replace this hook with a
 * static application TX queue.
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
 * @brief Application packet handler.
 *
 * This function is called only after the frame CRC succeeds. Firmware update
 * code should process or copy @p packet here, not before CRC validation.
 */
static void stm32_uartbin_it_on_packet(const uartbin_packet_t *packet, void *user)
{
    (void)user;

    /*
     * packet->payload is CRC-checked and valid until the next uartbin_feed call.
     * For firmware/update commands, copy/process it here after CRC passes.
     */
    switch (packet->type) {
    default:
        break;
    }
}

/**
 * @brief Protocol error callback.
 *
 * Typical production code increments counters, records diagnostics, and may
 * notify a supervisory task. The parser has already reset itself.
 */
static void stm32_uartbin_it_on_error(uartbin_error_t error, void *user)
{
    stm32_uartbin_it_t *port = (stm32_uartbin_it_t *)user;
    (void)error;

    port->protocol_errors++;
}

/**
 * @brief Initialize the interrupt adapter and arm the first RX byte.
 *
 * @param port Adapter instance owned by the application.
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
 * @brief Forward HAL_UART_RxCpltCallback() into uartbinlib.
 *
 * The function feeds the received byte, then immediately arms the next byte.
 * If re-arming fails, it resets the protocol parser so no partial frame leaks
 * into the next successful receive operation.
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
 * @brief Forward HAL_UART_ErrorCallback() into the adapter.
 *
 * On parity/framing/noise/overrun errors, discard the partial protocol frame,
 * abort the HAL receive path, and try to arm reception again.
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
 * @brief Forward HAL_UART_AbortReceiveCpltCallback() into the adapter.
 *
 * Some HAL versions complete abort asynchronously. This callback ensures RX is
 * re-armed after the abort path finishes.
 */
void stm32_uartbin_it_abort_complete_callback(stm32_uartbin_it_t *port, UART_HandleTypeDef *huart)
{
    if (huart == port->huart) {
        (void)stm32_uartbin_it_start_rx(port);
    }
}

/**
 * @brief Periodic timeout service for the protocol parser.
 *
 * Call this from a main loop, RTOS task, or periodic timer context.
 */
void stm32_uartbin_it_poll(stm32_uartbin_it_t *port)
{
    uartbin_poll(&port->link, HAL_GetTick());
}
