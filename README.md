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
- Optional request/event retry is driven by `uartbin_poll`.

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
    (void)uartbin_send_request(&link, 0x01, 0, payload, sizeof(payload));
}
```

## Automatic Sequences

For request/response/event style protocols, use the automatic helpers instead
of managing `seq` manually:

```c
/* Starts a new conversation. The link's internal sequence counter is used. */
uartbin_send_request(&link, MSG_GET_CONFIG, 0, NULL, 0);

/* Answers a received packet. The incoming packet sequence is echoed. */
uartbin_send_response(&link, packet, MSG_CONFIG_VALUE, 0, payload, payload_len);

/* Sends an unsolicited message. The link's internal sequence counter is used. */
uartbin_send_event(&link, MSG_SENSOR_EVENT, 0, payload, payload_len);
```

Each `uartbin_t` has its own sequence counter, so multiple UART links do not
share state. The lower-level `uartbin_send()` API is still available when an
application needs to provide an explicit sequence number.

## Automatic Retry

Automatic retry is optional. Enable it by giving each link a static TX retry
buffer and retry settings:

```c
static uint8_t rx_payload[256];
static uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 256];

uartbin_config_t cfg = {
    .write = uart_write,
    .on_packet = on_packet,
    .on_error = on_error,
    .rx_payload_buffer = rx_payload,
    .rx_payload_capacity = sizeof(rx_payload),
    .rx_timeout_ms = 50,
    .tx_retry_buffer = tx_retry_frame,
    .tx_retry_capacity = sizeof(tx_retry_frame),
    .tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS,
    .tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES
};
```

When retry is enabled, `uartbin_send_request()` and `uartbin_send_event()` store
the encoded frame in the retry buffer. Call `uartbin_poll(&link, now_ms)`
periodically; if no matching `UARTBIN_FLAG_RESPONSE` packet arrives with the
same sequence number before the timeout, the frame is transmitted again. When
the retry limit is reached, `on_error` receives
`UARTBIN_ERROR_RETRY_EXHAUSTED`.

The retry layer tracks one pending reliable request/event per `uartbin_t`.
Sending a second reliable request/event before the first response arrives
returns `UARTBIN_EBUSY`. Use `uartbin_cancel_retry()` if the application wants
to abandon a pending request manually.

For bidirectional protocols, both devices use the same pattern:

```c
/* Sender starts a reliable operation. */
uartbin_send_request(&link, MSG_DALI_COMMAND, 0, payload, payload_len);

/* Receiver answers after validating or completing the operation. */
uartbin_send_response(&link, packet, MSG_DALI_RESULT, 0, result, result_len);

/* Either side can also publish an event and expect an ACK-style response. */
uartbin_send_event(&link, MSG_DALI_EVENT, 0, event, event_len);
uartbin_send_response(&link, packet, MSG_ACK, 0, NULL, 0);
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

For multi-config generators such as Visual Studio, pass the configuration to
CTest:

```sh
ctest --test-dir build -C Debug
```

## Documentation

Generate the HTML documentation with Doxygen:

```sh
doxygen Doxyfile
```

Open `docs/html/index.html` in a browser.

Additional Doxygen guide pages:

- API Usage Guide
- Porting Guide
- Ring Buffer TX Port
- STM32 Interrupt and DMA Usage
