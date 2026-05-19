\page guide_porting Porting Guide

# Porting Guide

uartbinlib is platform independent. A port only needs four things:

- A static `uartbin_t` context.
- Static RX payload storage.
- A write hook that accepts all bytes given to it.
- A way to feed RX bytes/blocks and call `uartbin_poll()`.

## Porting Checklist

1. Allocate one context per UART/protocol link.
2. Allocate an RX payload buffer large enough for the largest application
   payload you accept.
3. Optional: allocate a TX retry frame buffer if you want automatic retry.
4. Implement `uartbin_write_fn`.
5. Feed RX bytes with `uartbin_feed_byte_at()` or blocks with
   `uartbin_feed_at()`.
6. Call `uartbin_poll()` periodically.
7. On UART framing/noise/overrun errors, reset the parser with
   `uartbin_reset()` and restart the platform RX path.

## Write Hook Contract

The write hook must either transmit all bytes before returning or copy all bytes
into an application-owned TX queue. Returning success before the bytes are safe
can corrupt frames, because the library may call the hook several times for one
packet.

Blocking TX is simple:

```c
static int uart_write(const uint8_t *data, size_t len, void *user)
{
    return HAL_UART_Transmit((UART_HandleTypeDef *)user,
                             (uint8_t *)data,
                             (uint16_t)len,
                             100) == HAL_OK ? 0 : -1;
}
```

Queued TX is better for interrupt/RTOS systems, but the queue push must copy
the complete byte range before returning.

## Timing

Use a monotonic millisecond tick for all `_at` feed calls and `uartbin_poll()`.
The tick may wrap if unsigned subtraction works correctly on the target.

```c
uartbin_feed_at(&link, data, len, platform_millis());
uartbin_poll(&link, platform_millis());
```

## Error Handling

Parser and reliable-TX errors arrive through `on_error`:

- `UARTBIN_ERROR_BAD_VERSION`: unsupported protocol version.
- `UARTBIN_ERROR_BAD_LENGTH`: payload larger than RX capacity.
- `UARTBIN_ERROR_CRC`: frame corruption.
- `UARTBIN_ERROR_RX_OVERFLOW`: no RX payload buffer for non-empty payload.
- `UARTBIN_ERROR_TIMEOUT`: partial RX frame timed out.
- `UARTBIN_ERROR_RETRY_EXHAUSTED`: reliable message was not answered in time.
- `UARTBIN_ERROR_RETRY_WRITE`: retry write hook failed.

Keep protocol errors separate from platform UART errors. Platform errors should
restart the UART peripheral and call `uartbin_reset()`.

## Multi-UART Systems

Use one `uartbin_t`, one RX payload buffer, and one optional TX retry buffer per
UART link. The automatic sequence counter and retry state live inside
`uartbin_t`, so links do not share state.

