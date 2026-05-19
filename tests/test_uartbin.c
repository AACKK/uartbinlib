#include "uartbin.h"

#include <stdio.h>
#include <string.h>

typedef struct test_bus {
    uint8_t tx[512];
    size_t tx_len;
    uint8_t rx_payload[128];
    uint8_t retry_frame[128];
    uartbin_packet_t last_packet;
    uint8_t last_payload[128];
    unsigned packet_count;
    unsigned error_count;
    uartbin_error_t last_error;
} test_bus_t;

static int test_write(const uint8_t *data, size_t len, void *user)
{
    test_bus_t *bus = (test_bus_t *)user;

    if (bus->tx_len + len > sizeof(bus->tx)) {
        return -1;
    }

    memcpy(&bus->tx[bus->tx_len], data, len);
    bus->tx_len += len;
    return 0;
}

static void test_on_packet(const uartbin_packet_t *packet, void *user)
{
    test_bus_t *bus = (test_bus_t *)user;

    bus->last_packet = *packet;
    memcpy(bus->last_payload, packet->payload, packet->payload_len);
    bus->packet_count++;
}

static void test_on_error(uartbin_error_t error, void *user)
{
    test_bus_t *bus = (test_bus_t *)user;

    bus->last_error = error;
    bus->error_count++;
}

static uartbin_t make_link(test_bus_t *bus)
{
    uartbin_t link;
    uartbin_config_t cfg;

    memset(bus, 0, sizeof(*bus));
    memset(&cfg, 0, sizeof(cfg));
    cfg.write = test_write;
    cfg.on_packet = test_on_packet;
    cfg.on_error = test_on_error;
    cfg.user = bus;
    cfg.rx_payload_buffer = bus->rx_payload;
    cfg.rx_payload_capacity = sizeof(bus->rx_payload);
    cfg.rx_timeout_ms = 0u;

    uartbin_init(&link, &cfg);
    return link;
}

static int expect(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        return 1;
    }

    return 0;
}

static int test_roundtrip(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    uint8_t payload[] = { 0x10, 0x20, 0x30, 0x40 };
    int failed = 0;

    failed |= expect(uartbin_send(&link, 0x33, 0x80, 0x1234, payload, sizeof(payload)) == UARTBIN_OK,
                     "send should succeed");
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(bus.packet_count == 1u, "one packet should be received");
    failed |= expect(bus.error_count == 0u, "no errors should be reported");
    failed |= expect(bus.last_packet.type == 0x33u, "type should match");
    failed |= expect(bus.last_packet.flags == 0x80u, "flags should match");
    failed |= expect(bus.last_packet.seq == 0x1234u, "sequence should match");
    failed |= expect(bus.last_packet.payload_len == sizeof(payload), "payload length should match");
    failed |= expect(memcmp(bus.last_payload, payload, sizeof(payload)) == 0, "payload should match");

    return failed;
}

static int test_bad_crc(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    uint8_t payload[] = { 0xAA, 0xBB };
    int failed = 0;

    failed |= expect(uartbin_send(&link, 0x01, 0x00, 1u, payload, sizeof(payload)) == UARTBIN_OK,
                     "send should succeed before crc corruption");
    bus.tx[bus.tx_len - 1u] ^= 0x55u;
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(bus.packet_count == 0u, "bad crc should drop packet");
    failed |= expect(bus.error_count == 1u, "bad crc should report one error");
    failed |= expect(bus.last_error == UARTBIN_ERROR_CRC, "error should be crc");

    return failed;
}

static int test_noise_resync(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    uint8_t payload[] = { 0x01 };
    uint8_t noise[] = { 0x00, 0xA5, 0x00, 0x99 };
    int failed = 0;

    failed |= expect(uartbin_send(&link, 0x07, 0x00, 2u, payload, sizeof(payload)) == UARTBIN_OK,
                     "send should succeed");
    uartbin_feed(&link, noise, sizeof(noise));
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(bus.packet_count == 1u, "parser should resync after noise");
    failed |= expect(bus.last_packet.type == 0x07u, "resynced packet type should match");

    return failed;
}

static int test_too_large_payload_is_rejected(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    uint8_t payload[130];
    int failed = 0;

    for (size_t i = 0u; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)i;
    }

    failed |= expect(uartbin_send(&link, 0x44, 0x00, 3u, payload, sizeof(payload)) == UARTBIN_OK,
                     "oversized test send should succeed");
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(bus.packet_count == 0u, "oversized packet should be rejected");
    failed |= expect(bus.error_count == 1u, "oversized packet should report one error");
    failed |= expect(bus.last_error == UARTBIN_ERROR_BAD_LENGTH, "error should be bad length");

    return failed;
}

static int test_timeout(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    int failed = 0;

    link.cfg.rx_timeout_ms = 10u;
    uartbin_feed_byte_at(&link, UARTBIN_SOF0, 100u);
    uartbin_poll(&link, 111u);

    failed |= expect(bus.error_count == 1u, "timeout should report one error");
    failed |= expect(bus.last_error == UARTBIN_ERROR_TIMEOUT, "error should be timeout");

    return failed;
}

static int test_auto_request_and_event_sequences(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    uint8_t payload[] = { 0x11, 0x22 };
    int failed = 0;

    failed |= expect(uartbin_send_request(&link, 0x10, 0x80, payload, sizeof(payload)) == UARTBIN_OK,
                     "auto request send should succeed");
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(bus.packet_count == 1u, "request should be received");
    failed |= expect(bus.last_packet.type == 0x10u, "request type should match");
    failed |= expect((bus.last_packet.flags & UARTBIN_FLAG_REQUEST) != 0u, "request flag should be set");
    failed |= expect((bus.last_packet.flags & 0x80u) != 0u, "caller flags should be preserved");
    failed |= expect(bus.last_packet.seq == 1u, "first automatic sequence should be 1");

    bus.tx_len = 0u;
    failed |= expect(uartbin_send_event(&link, 0x20, 0x40, payload, sizeof(payload)) == UARTBIN_OK,
                     "auto event send should succeed");
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(bus.packet_count == 2u, "event should be received");
    failed |= expect(bus.last_packet.type == 0x20u, "event type should match");
    failed |= expect((bus.last_packet.flags & UARTBIN_FLAG_EVENT) != 0u, "event flag should be set");
    failed |= expect((bus.last_packet.flags & 0x40u) != 0u, "event caller flags should be preserved");
    failed |= expect(bus.last_packet.seq == 2u, "second automatic sequence should be 2");

    return failed;
}

static int test_auto_response_echoes_sequence(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    uartbin_packet_t request;
    uint8_t payload[] = { 0x33 };
    int failed = 0;

    memset(&request, 0, sizeof(request));
    request.type = 0x10u;
    request.flags = UARTBIN_FLAG_REQUEST;
    request.seq = 0x4567u;

    failed |= expect(uartbin_send_response(&link, &request, 0x11, 0x80, payload, sizeof(payload)) == UARTBIN_OK,
                     "auto response send should succeed");
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(bus.packet_count == 1u, "response should be received");
    failed |= expect(bus.last_packet.type == 0x11u, "response type should match");
    failed |= expect((bus.last_packet.flags & UARTBIN_FLAG_RESPONSE) != 0u, "response flag should be set");
    failed |= expect((bus.last_packet.flags & 0x80u) != 0u, "response caller flags should be preserved");
    failed |= expect(bus.last_packet.seq == 0x4567u, "response should echo request sequence");
    failed |= expect(link.tx_seq == 0u, "response should not advance automatic sequence");

    return failed;
}

static int test_reliable_request_retries_and_exhausts(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    uint8_t payload[] = { 0x55, 0x66 };
    size_t first_frame_len;
    int failed = 0;

    link.cfg.tx_retry_buffer = bus.retry_frame;
    link.cfg.tx_retry_capacity = sizeof(bus.retry_frame);
    link.cfg.tx_retry_timeout_ms = 50u;
    link.cfg.tx_retry_max_retries = 1u;

    failed |= expect(uartbin_send_request(&link, 0x30, 0, payload, sizeof(payload)) == UARTBIN_OK,
                     "reliable request should send");
    first_frame_len = bus.tx_len;
    failed |= expect(link.retry_active != 0u, "reliable request should become pending");

    bus.tx_len = 0u;
    uartbin_poll(&link, 100u);
    failed |= expect(bus.tx_len == 0u, "first poll should arm retry timer only");

    uartbin_poll(&link, 149u);
    failed |= expect(bus.tx_len == 0u, "poll before timeout should not retry");

    uartbin_poll(&link, 150u);
    failed |= expect(bus.tx_len == first_frame_len, "timeout should retransmit pending frame");
    failed |= expect(link.retry_count == 1u, "retry count should increment");

    bus.tx_len = 0u;
    uartbin_poll(&link, 200u);
    failed |= expect(bus.tx_len == 0u, "exhausted retry should not transmit again");
    failed |= expect(bus.error_count == 1u, "retry exhaustion should report one error");
    failed |= expect(bus.last_error == UARTBIN_ERROR_RETRY_EXHAUSTED, "error should be retry exhausted");
    failed |= expect(link.retry_active == 0u, "retry state should clear after exhaustion");

    return failed;
}

static int test_response_clears_reliable_request(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    int failed = 0;

    link.cfg.tx_retry_buffer = bus.retry_frame;
    link.cfg.tx_retry_capacity = sizeof(bus.retry_frame);
    link.cfg.tx_retry_timeout_ms = 50u;
    link.cfg.tx_retry_max_retries = 3u;

    failed |= expect(uartbin_send_request(&link, 0x40, 0, NULL, 0) == UARTBIN_OK,
                     "reliable request should send");
    failed |= expect(link.retry_active != 0u, "request should be pending");

    bus.tx_len = 0u;
    failed |= expect(uartbin_send(&link, 0x41, UARTBIN_FLAG_RESPONSE, 1u, NULL, 0) == UARTBIN_OK,
                     "response frame should send");
    uartbin_feed(&link, bus.tx, bus.tx_len);

    failed |= expect(link.retry_active == 0u, "matching response should clear retry state");
    failed |= expect(bus.packet_count == 1u, "response should still be delivered");

    return failed;
}

static int test_reliable_request_busy_until_response(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    int failed = 0;

    link.cfg.tx_retry_buffer = bus.retry_frame;
    link.cfg.tx_retry_capacity = sizeof(bus.retry_frame);
    link.cfg.tx_retry_timeout_ms = 50u;
    link.cfg.tx_retry_max_retries = 3u;

    failed |= expect(uartbin_send_request(&link, 0x50, 0, NULL, 0) == UARTBIN_OK,
                     "first reliable request should send");
    failed |= expect(uartbin_send_request(&link, 0x51, 0, NULL, 0) == UARTBIN_EBUSY,
                     "second reliable request should be rejected while pending");

    return failed;
}

static int test_retry_config_error_does_not_advance_sequence(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    int failed = 0;

    link.cfg.tx_retry_timeout_ms = 50u;
    link.cfg.tx_retry_max_retries = 3u;

    failed |= expect(uartbin_send_request(&link, 0x60, 0, NULL, 0) == UARTBIN_ENO_BUFFER,
                     "retry without buffer should be rejected");
    failed |= expect(link.tx_seq == 0u, "failed retry config should not advance sequence");
    failed |= expect(bus.tx_len == 0u, "failed retry config should not transmit");

    link.cfg.tx_retry_buffer = bus.retry_frame;
    link.cfg.tx_retry_capacity = 4u;

    failed |= expect(uartbin_send_event(&link, 0x61, 0, NULL, 0) == UARTBIN_EPAYLOAD_TOO_LONG,
                     "retry buffer too small should be rejected");
    failed |= expect(link.tx_seq == 0u, "small retry buffer should not advance sequence");
    failed |= expect(bus.tx_len == 0u, "small retry buffer should not transmit");

    return failed;
}

static int test_rx_feed_does_not_drive_tx_retry(void)
{
    test_bus_t bus;
    uartbin_t link = make_link(&bus);
    size_t first_frame_len;
    int failed = 0;

    link.cfg.tx_retry_buffer = bus.retry_frame;
    link.cfg.tx_retry_capacity = sizeof(bus.retry_frame);
    link.cfg.tx_retry_timeout_ms = 50u;
    link.cfg.tx_retry_max_retries = 3u;

    failed |= expect(uartbin_send_request(&link, 0x70, 0, NULL, 0) == UARTBIN_OK,
                     "reliable request should send before rx feed retry test");
    first_frame_len = bus.tx_len;
    bus.tx_len = 0u;

    uartbin_feed_byte_at(&link, 0x00, 100u);
    uartbin_feed_byte_at(&link, 0x00, 200u);

    failed |= expect(bus.tx_len == 0u, "rx feed should not retransmit pending reliable frame");
    uartbin_poll(&link, 200u);
    failed |= expect(bus.tx_len == 0u, "first explicit poll should arm retry timer");
    uartbin_poll(&link, 250u);
    failed |= expect(bus.tx_len == first_frame_len, "explicit poll should drive tx retry");

    return failed;
}

int main(void)
{
    int failed = 0;

    failed |= test_roundtrip();
    failed |= test_bad_crc();
    failed |= test_noise_resync();
    failed |= test_too_large_payload_is_rejected();
    failed |= test_timeout();
    failed |= test_auto_request_and_event_sequences();
    failed |= test_auto_response_echoes_sequence();
    failed |= test_reliable_request_retries_and_exhausts();
    failed |= test_response_clears_reliable_request();
    failed |= test_reliable_request_busy_until_response();
    failed |= test_retry_config_error_does_not_advance_sequence();
    failed |= test_rx_feed_does_not_drive_tx_retry();

    if (failed != 0) {
        return 1;
    }

    printf("uartbin tests passed\n");
    return 0;
}
