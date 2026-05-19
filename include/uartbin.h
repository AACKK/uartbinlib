/**
 * @file uartbin.h
 * @brief Platform-independent binary UART framing library.
 *
 * uartbinlib provides a small C API for sending and receiving variable-length
 * binary packets over byte streams such as UART. The core library has no HAL,
 * RTOS, interrupt, DMA, or dynamic allocation dependency. The application owns
 * all buffers and connects the library to the target platform through hooks and
 * callbacks.
 *
 * @section uartbin_h_overview Overview
 *
 * uartbinlib frames binary payloads as:
 *
 * @code
 * SOF(2) | version(1) | type(1) | flags(1) | reserved(1) |
 * seq(2) | payload_len(2) | payload(N) | crc16(2)
 * @endcode
 *
 * The receiver stores the complete payload in a user-provided static buffer.
 * The packet callback is called only after the complete frame has passed CRC
 * validation. This keeps the API simple and makes it suitable for robust
 * embedded command, telemetry, configuration, and firmware-chunk protocols.
 *
 * Applications can use the low-level uartbin_send() API with an explicit
 * sequence number, or the higher-level request/response/event helpers. The
 * helpers keep an automatic per-context sequence counter, echo sequence numbers
 * for responses, and can optionally retry request/event frames until a matching
 * response arrives.
 *
 * @section uartbin_h_no_alloc Memory Model
 *
 * The library never allocates memory. The application provides:
 *
 * - a persistent ::uartbin_t context,
 * - a static RX payload buffer,
 * - an optional static TX retry frame buffer,
 * - a write hook for TX,
 * - packet and error callbacks.
 *
 * @section uartbin_h_timing Timeout Model
 *
 * Set ::uartbin_config::rx_timeout_ms to a non-zero value and call
 * uartbin_poll() periodically. The timestamped feed functions also check the
 * RX timeout before accepting each byte/block. The same uartbin_poll() call
 * drives optional TX retry timing when ::uartbin_config::tx_retry_timeout_ms
 * is configured.
 *
 * @section uartbin_h_stm32 STM32 Integration
 *
 * The portable core does not include STM32 headers. See the files in
 * @c examples/ for interrupt and DMA idle-line adapter patterns, including HAL
 * error recovery.
 */
#ifndef UARTBIN_H
#define UARTBIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Protocol version stored in every frame header. */
#define UARTBIN_VERSION 1u

/** @brief First start-of-frame byte. */
#define UARTBIN_SOF0 0xA5u

/** @brief Second start-of-frame byte. */
#define UARTBIN_SOF1 0x5Au

/** @brief Header size after the two SOF bytes, in bytes. */
#define UARTBIN_HEADER_SIZE 8u

/** @brief CRC field size, in bytes. */
#define UARTBIN_CRC_SIZE 2u

/** @brief Bytes added around each payload: SOF + header + CRC. */
#define UARTBIN_MAX_FRAME_OVERHEAD (2u + UARTBIN_HEADER_SIZE + UARTBIN_CRC_SIZE)

#ifndef UARTBIN_DEFAULT_RETRY_TIMEOUT_MS
/** @brief Default retry timeout value applications can use in config. */
#define UARTBIN_DEFAULT_RETRY_TIMEOUT_MS 100u
#endif

#ifndef UARTBIN_DEFAULT_RETRY_MAX_RETRIES
/** @brief Default retry count applications can use in config. */
#define UARTBIN_DEFAULT_RETRY_MAX_RETRIES 3u
#endif

/** @brief Flag bit set by uartbin_send_request(). */
#define UARTBIN_FLAG_REQUEST 0x01u

/** @brief Flag bit set by uartbin_send_response(). */
#define UARTBIN_FLAG_RESPONSE 0x02u

/** @brief Flag bit set by uartbin_send_event(). */
#define UARTBIN_FLAG_EVENT 0x04u

/**
 * @brief Return status values for API calls that can fail immediately.
 */
typedef enum uartbin_status {
    /** Operation completed successfully. */
    UARTBIN_OK = 0,

    /** Invalid argument, such as a NULL context or missing payload pointer. */
    UARTBIN_EINVAL = -1,

    /** The configured write hook reported a TX failure. */
    UARTBIN_EWRITE = -2,

    /** Reserved for applications that add stricter TX payload limits. */
    UARTBIN_EPAYLOAD_TOO_LONG = -3,

/** Reserved for configurations without a required buffer. */
    UARTBIN_ENO_BUFFER = -4,

    /** A reliable request/event is already waiting for a response. */
    UARTBIN_EBUSY = -5
} uartbin_status_t;

/**
 * @brief Parser/runtime errors delivered through ::uartbin_error_fn.
 */
typedef enum uartbin_error {
    /** A frame used an unsupported protocol version. */
    UARTBIN_ERROR_BAD_VERSION = 1,

    /** Payload length is larger than the configured RX payload buffer. */
    UARTBIN_ERROR_BAD_LENGTH,

    /** Frame CRC did not match the received header and payload. */
    UARTBIN_ERROR_CRC,

    /** A payload was received but no RX payload buffer was configured. */
    UARTBIN_ERROR_RX_OVERFLOW,

    /** RX state machine stayed mid-frame longer than rx_timeout_ms. */
    UARTBIN_ERROR_TIMEOUT,

    /** Reliable TX retry limit was reached before a response arrived. */
    UARTBIN_ERROR_RETRY_EXHAUSTED,

    /** Reliable TX retry could not be written by the configured hook. */
    UARTBIN_ERROR_RETRY_WRITE
} uartbin_error_t;

/**
 * @brief CRC-checked packet delivered to the application.
 *
 * The payload pointer aliases ::uartbin_config::rx_payload_buffer. It is
 * valid only until the next call to uartbin_feed(), uartbin_feed_at(),
 * uartbin_feed_byte(), uartbin_feed_byte_at(), uartbin_poll(), or
 * uartbin_reset() on the same context.
 */
typedef struct uartbin_packet {
    /** Application-defined packet type. */
    uint8_t type;

    /** Application-defined flag bits. */
    uint8_t flags;

    /** Application-defined sequence number, useful for request/response flows. */
    uint16_t seq;

    /** Pointer to the CRC-validated payload bytes. */
    const uint8_t *payload;

    /** Number of valid bytes at the payload pointer. */
    uint16_t payload_len;
} uartbin_packet_t;

/**
 * @brief Platform write hook used by uartbin_send().
 *
 * The hook must write or copy all @p len bytes before returning. This is
 * important because uartbin_send() may call the hook multiple times for one
 * frame. For non-blocking UART TX, make this hook copy into an application TX
 * queue and return only if the bytes were accepted.
 *
 * @param data Bytes to transmit.
 * @param len Number of bytes to transmit.
 * @param user User pointer from ::uartbin_config.
 * @return 0 on success, non-zero on failure.
 */
typedef int (*uartbin_write_fn)(const uint8_t *data, size_t len, void *user);

/**
 * @brief Called when a complete frame passes CRC validation.
 *
 * @param packet CRC-checked packet metadata and payload pointer.
 * @param user User pointer from ::uartbin_config.
 */
typedef void (*uartbin_packet_fn)(const uartbin_packet_t *packet, void *user);

/**
 * @brief Called when the parser detects a protocol error or timeout.
 *
 * After reporting an error, the parser resets to SOF search state and attempts
 * to resynchronize on subsequent bytes.
 *
 * @param error Error code.
 * @param user User pointer from ::uartbin_config.
 */
typedef void (*uartbin_error_fn)(uartbin_error_t error, void *user);

/**
 * @brief Configuration used to initialize a uartbin context.
 */
typedef struct uartbin_config {
    /** TX hook used by uartbin_send(). May be NULL if the context is RX-only. */
    uartbin_write_fn write;

    /** Packet callback called after CRC succeeds. May be NULL. */
    uartbin_packet_fn on_packet;

    /** Error callback called for parser errors and timeouts. May be NULL. */
    uartbin_error_fn on_error;

    /** Opaque application pointer passed to hooks and callbacks. */
    void *user;

    /** Static payload buffer owned by the application. */
    uint8_t *rx_payload_buffer;

    /** Size of rx_payload_buffer in bytes. This is the max RX payload. */
    uint16_t rx_payload_capacity;

    /**
     * @brief RX timeout in milliseconds.
     *
     * Set to 0 to disable timeout handling. The timestamp source is supplied by
     * the application through uartbin_feed_at(), uartbin_feed_byte_at(), and
     * uartbin_poll().
     */
    uint32_t rx_timeout_ms;

    /** Static frame buffer used by automatic request/event retry. Optional. */
    uint8_t *tx_retry_buffer;

    /** Size of tx_retry_buffer in bytes. */
    uint16_t tx_retry_capacity;

    /** Retry timeout for automatic request/event sends. 0 disables retry. */
    uint32_t tx_retry_timeout_ms;

    /** Number of retransmits before UARTBIN_ERROR_RETRY_EXHAUSTED. */
    uint8_t tx_retry_max_retries;
} uartbin_config_t;

/**
 * @brief Runtime state for one uartbin link.
 *
 * Allocate one context per UART/protocol link. The fields are public so the
 * type can be stack/static allocated without hidden allocation, but application
 * code should treat them as internal implementation details.
 */
typedef struct uartbin {
    /** Copy of the user configuration. */
    uartbin_config_t cfg;

    /** Internal parser state. */
    uint8_t state;

    /** Internal frame header buffer. */
    uint8_t header[UARTBIN_HEADER_SIZE];

    /** Number of header bytes received so far. */
    uint8_t header_pos;

    /** Number of payload bytes received so far. */
    uint16_t payload_pos;

    /** Payload length decoded from the current frame header. */
    uint16_t payload_len;

    /** Internal received CRC bytes. */
    uint8_t crc_bytes[UARTBIN_CRC_SIZE];

    /** Number of CRC bytes received so far. */
    uint8_t crc_pos;

    /** Running CRC over header and payload. */
    uint16_t crc;

    /** Timestamp of the last accepted RX byte/block. */
    uint32_t last_rx_time_ms;

    /** Sequence counter used by automatic send helpers. */
    uint16_t tx_seq;

    /** Non-zero while a reliable request/event is waiting for a response. */
    uint8_t retry_active;

    /** Non-zero after retry timing has been armed by uartbin_poll(). */
    uint8_t retry_timer_started;

    /** Number of retransmits already attempted for the pending frame. */
    uint8_t retry_count;

    /** Sequence number of the pending reliable frame. */
    uint16_t retry_seq;

    /** Number of bytes stored in cfg.tx_retry_buffer. */
    uint16_t retry_frame_len;

    /** Timestamp used to decide when to retransmit the pending frame. */
    uint32_t retry_last_tx_time_ms;
} uartbin_t;

/**
 * @brief Initialize a uartbin context.
 *
 * @param ctx Context to initialize. Must remain valid while the link is used.
 * @param config Configuration copied into @p ctx.
 */
void uartbin_init(uartbin_t *ctx, const uartbin_config_t *config);

/**
 * @brief Reset only the RX parser state.
 *
 * Use this after platform UART errors, manual aborts, or link recovery events.
 * The configuration and callbacks are preserved.
 *
 * @param ctx Context to reset.
 */
void uartbin_reset(uartbin_t *ctx);

/**
 * @brief Cancel any pending automatic retry state.
 *
 * This does not reset RX parsing or transmit anything. It is useful when the
 * application intentionally abandons a request before a response arrives.
 *
 * @param ctx Initialized context.
 */
void uartbin_cancel_retry(uartbin_t *ctx);

/**
 * @brief Build and transmit one framed packet.
 *
 * @param ctx Initialized context with a valid write hook.
 * @param type Application-defined packet type.
 * @param flags Application-defined flags.
 * @param seq Application-defined sequence number.
 * @param payload Payload bytes, or NULL when @p payload_len is 0.
 * @param payload_len Number of payload bytes.
 * @return ::UARTBIN_OK on success, otherwise an error status.
 */
uartbin_status_t uartbin_send(uartbin_t *ctx,
                              uint8_t type,
                              uint8_t flags,
                              uint16_t seq,
                              const uint8_t *payload,
                              uint16_t payload_len);

/**
 * @brief Return the next non-zero TX sequence number for this context.
 *
 * This is mainly useful for applications that need to track an automatically
 * generated request before sending. The request/event helper functions call
 * this internally, so most applications do not need to use it directly.
 *
 * @param ctx Initialized context.
 * @return Next sequence number, or 0 if @p ctx is NULL.
 */
uint16_t uartbin_next_seq(uartbin_t *ctx);

/**
 * @brief Send a new request with an automatically generated sequence number.
 *
 * The helper ORs ::UARTBIN_FLAG_REQUEST into @p flags and uses the context's
 * internal sequence counter. Use the response helper to answer this packet.
 *
 * @param ctx Initialized context with a valid write hook.
 * @param type Application-defined request type.
 * @param flags Application-defined flags to combine with request flag.
 * @param payload Payload bytes, or NULL when @p payload_len is 0.
 * @param payload_len Number of payload bytes.
 * @return ::UARTBIN_OK on success, otherwise an error status.
 */
uartbin_status_t uartbin_send_request(uartbin_t *ctx,
                                      uint8_t type,
                                      uint8_t flags,
                                      const uint8_t *payload,
                                      uint16_t payload_len);

/**
 * @brief Send a response using the sequence number from a received packet.
 *
 * The helper ORs ::UARTBIN_FLAG_RESPONSE into @p flags and echoes
 * @p request->seq. It does not advance the context sequence counter.
 *
 * @param ctx Initialized context with a valid write hook.
 * @param request Packet being answered.
 * @param type Application-defined response type.
 * @param flags Application-defined flags to combine with response flag.
 * @param payload Payload bytes, or NULL when @p payload_len is 0.
 * @param payload_len Number of payload bytes.
 * @return ::UARTBIN_OK on success, otherwise an error status.
 */
uartbin_status_t uartbin_send_response(uartbin_t *ctx,
                                       const uartbin_packet_t *request,
                                       uint8_t type,
                                       uint8_t flags,
                                       const uint8_t *payload,
                                       uint16_t payload_len);

/**
 * @brief Send an unsolicited event with an automatically generated sequence.
 *
 * The helper ORs ::UARTBIN_FLAG_EVENT into @p flags and uses the context's
 * internal sequence counter. If the peer acknowledges the event, it should echo
 * this packet's sequence number in a response.
 *
 * @param ctx Initialized context with a valid write hook.
 * @param type Application-defined event type.
 * @param flags Application-defined flags to combine with event flag.
 * @param payload Payload bytes, or NULL when @p payload_len is 0.
 * @param payload_len Number of payload bytes.
 * @return ::UARTBIN_OK on success, otherwise an error status.
 */
uartbin_status_t uartbin_send_event(uartbin_t *ctx,
                                    uint8_t type,
                                    uint8_t flags,
                                    const uint8_t *payload,
                                    uint16_t payload_len);

/**
 * @brief Feed received bytes without timeout timestamp updates.
 *
 * Prefer uartbin_feed_at() when timeout handling is enabled.
 *
 * @param ctx Initialized context.
 * @param data Received byte buffer.
 * @param len Number of received bytes.
 */
void uartbin_feed(uartbin_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Feed one received byte without timeout timestamp updates.
 *
 * Prefer uartbin_feed_byte_at() when timeout handling is enabled.
 *
 * @param ctx Initialized context.
 * @param byte Received byte.
 */
void uartbin_feed_byte(uartbin_t *ctx, uint8_t byte);

/**
 * @brief Feed received bytes with the current application timestamp.
 *
 * @param ctx Initialized context.
 * @param data Received byte buffer.
 * @param len Number of received bytes.
 * @param now_ms Monotonic millisecond timestamp, such as HAL_GetTick().
 */
void uartbin_feed_at(uartbin_t *ctx, const uint8_t *data, size_t len, uint32_t now_ms);

/**
 * @brief Feed one received byte with the current application timestamp.
 *
 * @param ctx Initialized context.
 * @param byte Received byte.
 * @param now_ms Monotonic millisecond timestamp, such as HAL_GetTick().
 */
void uartbin_feed_byte_at(uartbin_t *ctx, uint8_t byte, uint32_t now_ms);

/**
 * @brief Check for RX timeout and reset the parser if needed.
 *
 * Call this periodically from the main loop or a scheduler when
 * ::uartbin_config::rx_timeout_ms is non-zero.
 *
 * @param ctx Initialized context.
 * @param now_ms Monotonic millisecond timestamp.
 */
void uartbin_poll(uartbin_t *ctx, uint32_t now_ms);

/**
 * @brief Update CRC-16/CCITT-FALSE.
 *
 * Polynomial: 0x1021. The library uses seed 0xFFFF for frames.
 *
 * @param data Input bytes. May be NULL only when @p len is 0.
 * @param len Number of input bytes.
 * @param seed Initial/running CRC value.
 * @return Updated CRC value.
 */
uint16_t uartbin_crc16_ccitt(const uint8_t *data, size_t len, uint16_t seed);

#ifdef __cplusplus
}
#endif

#endif
