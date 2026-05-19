# uartbinlib

`uartbinlib` is a small, platform-independent C library for framed binary UART messages. It is designed for embedded targets such as STM32, but the core has no HAL, RTOS, DMA, interrupt, or heap dependency.

## Frame Format

All multi-byte integers are little-endian.

| Field | Size | Description |
| --- | ---: | --- |
| SOF | 2 | `0xA5 0x5A` |
| Version | 1 | Protocol version, currently `1` |
| Type | 1 | Application-defined packet type |
| Flags | 1 | Application-defined flags |
| Reserved | 1 | Currently `0` |
| Sequence | 2 | Application-defined sequence number |
| Payload length | 2 | Payload byte count |
| Payload | N | User data |
| CRC16 | 2 | CRC-16/CCITT-FALSE over header and payload |

## Design

- No dynamic allocation.
- No platform includes.
- TX uses a user-provided `write` hook.
- RX stores the complete payload in a user-provided buffer.
- `on_packet` is called only after the full frame CRC is verified.
- Parse errors and timeouts are delivered through `on_error`.

## Minimal Usage

```c
#include "uartbin.h"

static int uart_write(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    /* Send data with your UART driver. Return 0 on success. */
    return 0;
}

static void on_packet(const uartbin_packet_t *packet, void *user)
{
    (void)user;
    /* packet->payload is valid until uartbin_feed/feed_byte is called again. */
}

static void on_error(uartbin_error_t error, void *user)
{
    (void)error;
    (void)user;
}

static uartbin_t link;
static uint8_t rx_payload[4096];

void app_init(void)
{
    uartbin_config_t cfg = {
        .write = uart_write,
        .on_packet = on_packet,
        .on_error = on_error,
        .user = 0,
        .rx_payload_buffer = rx_payload,
        .rx_payload_capacity = sizeof(rx_payload),
        .rx_timeout_ms = 50
    };

    uartbin_init(&link, &cfg);
}

void app_on_uart_rx_byte(uint8_t byte)
{
    uartbin_feed_byte_at(&link, byte, system_millis());
}

void app_send_ping(void)
{
    uint8_t payload[] = { 1, 2, 3 };
    (void)uartbin_send(&link, 0x01, 0, 42, payload, sizeof(payload));
}
```

## STM32 Integration Sketch

Keep STM32-specific code outside the library. Feed RX bytes from an interrupt, DMA idle callback, or task loop with `uartbin_feed_at` / `uartbin_feed_byte_at`.

The `write` hook must complete the write or copy the bytes into a TX queue before returning, because `uartbin_send` writes SOF, header, payload, and CRC in separate calls. The examples use blocking `HAL_UART_Transmit` for clarity and safety. If you need non-blocking TX, implement a TX queue in the application adapter.

See:

- `examples/stm32_hal_interrupt.c`
- `examples/stm32_hal_dma_idle.c`

## Large Payloads

For larger packets, allocate a larger static RX payload buffer. This keeps the API simple and ensures the application sees data only after CRC passes.

```c
static uint8_t rx_payload[4096];
```

If an incoming frame advertises a payload larger than `rx_payload_capacity`, the parser rejects it with `UARTBIN_ERROR_BAD_LENGTH` and resynchronizes.

## Timeout

Set `rx_timeout_ms` and call `uartbin_poll(&link, now_ms)` periodically. The `_at` feed functions also check timeout before accepting each byte:

```c
uartbin_feed_at(&link, dma_data, dma_len, HAL_GetTick());
uartbin_poll(&link, HAL_GetTick());
```

On STM32 HAL UART errors, reset the parser, abort/restart RX, and keep an error counter. The example adapters show this pattern in their error callback functions.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Documentation

Generate the HTML documentation with Doxygen:

```sh
doxygen Doxyfile
```

Open `docs/html/index.html` in a browser.
