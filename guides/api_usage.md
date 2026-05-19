\page guide_api_usage API Usage Guide

# API Usage Guide

uartbinlib has two layers:

- The framing layer: `uartbin_send()`, `uartbin_feed_at()`, `uartbin_poll()`.
- The reliable message layer: `uartbin_send_request()`,
  `uartbin_send_response()`, `uartbin_send_event()`.

Most applications should use the reliable helpers and keep `uartbin_send()` for
special cases that need an explicit sequence number.

## Basic Setup

Allocate one `uartbin_t` per UART link. Each link owns its RX parser state,
automatic TX sequence counter, and optional retry state.

```c
static uartbin_t link;
static uint8_t rx_payload[256];
static uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 256];

static int uart_write(const uint8_t *data, size_t len, void *user)
{
    /* Write all bytes now, or copy all bytes into a TX queue. */
    return platform_uart_write(data, len, user) == 0 ? 0 : -1;
}

static void on_packet(const uartbin_packet_t *packet, void *user)
{
    (void)user;

    switch (packet->type) {
    case MSG_DALI_COMMAND:
        /* Validate/process, then answer with the same sequence number. */
        uartbin_send_response(&link, packet, MSG_DALI_RESULT, 0, NULL, 0);
        break;

    case MSG_DALI_EVENT:
        /* Event received from peer; acknowledge it. */
        uartbin_send_response(&link, packet, MSG_ACK, 0, NULL, 0);
        break;

    default:
        uartbin_send_response(&link, packet, MSG_NACK, 0, NULL, 0);
        break;
    }
}

static void on_error(uartbin_error_t error, void *user)
{
    (void)user;
    /* Count, log, notify supervisor, or reset the higher-level session. */
}

void protocol_init(void)
{
    uartbin_config_t cfg = {
        .write = uart_write,
        .on_packet = on_packet,
        .on_error = on_error,
        .user = NULL,
        .rx_payload_buffer = rx_payload,
        .rx_payload_capacity = sizeof(rx_payload),
        .rx_timeout_ms = 50,
        .tx_retry_buffer = tx_retry_frame,
        .tx_retry_capacity = sizeof(tx_retry_frame),
        .tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS,
        .tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES
    };

    uartbin_init(&link, &cfg);
}
```

## Sending Messages

Use a request when this device is asking the peer to do something:

```c
uartbin_send_request(&link, MSG_DALI_SET_LEVEL, 0, payload, payload_len);
```

Use a response when answering a received packet:

```c
uartbin_send_response(&link, packet, MSG_DALI_RESULT, 0, result, result_len);
```

Use an event for unsolicited data, such as a DALI bus event reported by the
module:

```c
uartbin_send_event(&link, MSG_DALI_BUS_EVENT, 0, event, event_len);
```

## Receiving Bytes

Feed every received byte or block to the parser with a monotonic millisecond
timestamp:

```c
uartbin_feed_at(&link, rx_data, rx_len, platform_millis());
```

Call `uartbin_poll()` periodically from a main loop, scheduler tick, or RTOS
task. It handles RX parser timeouts and TX retry timing:

```c
uartbin_poll(&link, platform_millis());
```

The feed functions only check RX parser timeout before accepting bytes. They do
not retransmit pending reliable messages. Keep TX retry in your explicit
`uartbin_poll()` path so retry writes do not unexpectedly run inside UART RX
interrupt or DMA callbacks.

## Reliable Message Rules

When retry is enabled:

- `uartbin_send_request()` and `uartbin_send_event()` store the encoded frame.
- If no matching `UARTBIN_FLAG_RESPONSE` arrives with the same `seq`, the frame
  is retransmitted by `uartbin_poll()`.
- When the retry count is exhausted, `on_error` receives
  `UARTBIN_ERROR_RETRY_EXHAUSTED`.
- Only one reliable request/event can be pending per `uartbin_t`; another send
  returns `UARTBIN_EBUSY`.

The peer must answer reliable requests/events using `uartbin_send_response()`.
That function echoes the incoming sequence number automatically.

## Packet Lifetime

`packet->payload` points into the configured RX payload buffer. It remains valid
only until the next feed, poll, reset, or parser operation on the same context.
Copy the payload in `on_packet()` if it must be kept longer.
