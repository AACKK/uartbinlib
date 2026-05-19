\page guide_stm32_transport STM32 Interrupt and DMA Usage

# STM32 Interrupt and DMA Usage

The portable library does not depend on STM32 HAL, but the repository includes
two adapter examples:

- `examples/stm32_hal_interrupt.c`
- `examples/stm32_hal_dma_idle.c`

Use these as patterns, not as mandatory library code.

## Interrupt RX

One-byte interrupt RX is simple and robust:

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uartbin_feed_byte_at(&link, rx_byte, HAL_GetTick());
    HAL_UART_Receive_IT(huart, &rx_byte, 1);
}
```

Recommended behavior:

- Arm the next byte immediately after feeding the current one.
- Call `uartbin_reset()` after HAL noise/framing/parity/overrun errors.
- Restart RX after abort/error recovery.
- Call `uartbin_poll()` from the main loop or scheduler.
- Let `uartbin_poll()` drive retry; RX callbacks only feed received bytes.

This model is easy to debug and works well at moderate baud rates.

## DMA Idle-Line RX

DMA idle-line RX receives blocks and feeds them to uartbinlib:

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    uartbin_feed_at(&link, dma_rx_buffer, size, HAL_GetTick());
    HAL_UARTEx_ReceiveToIdle_DMA(huart, dma_rx_buffer, sizeof(dma_rx_buffer));
}
```

Recommended behavior:

- The DMA buffer does not need to hold a complete uartbin frame.
- Feed only valid newly received bytes.
- Restart `HAL_UARTEx_ReceiveToIdle_DMA()` after each idle event.
- Disable DMA half-transfer interrupts unless your adapter tracks offsets.
- On HAL UART errors, abort/restart DMA and call `uartbin_reset()`.

This model is better for high baud rates or bursty traffic.

## TX Choices

The STM32 examples use blocking `HAL_UART_Transmit()` because it satisfies the
write hook contract with minimal code. Production projects often replace this
with:

- A UART TX ring buffer driven by TX-empty interrupt.
- A DMA TX queue.
- An RTOS stream buffer drained by a UART task.

All three are valid as long as the write hook copies or accepts all bytes before
returning success.

## Retry With STM32

Automatic retry needs regular calls to `uartbin_poll()`:

```c
void main_loop(void)
{
    uartbin_poll(&link, HAL_GetTick());
}
```

For RTOS systems, a 1 ms to 10 ms periodic task is usually enough. Choose
`tx_retry_timeout_ms` larger than the normal round-trip time of the peer plus
the expected DALI operation delay if the response is sent after the DALI action
completes.

Do not rely on RX callbacks to drive retry timing. Feed callbacks only service
RX parser timeout; TX retry runs from the explicit poll path.
