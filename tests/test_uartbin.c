#include "uartbin.h"

#include <stdio.h>
#include <string.h>

typedef struct test_bus {
    uint8_t tx[512];
    size_t tx_len;
    uint8_t rx_payload[128];
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

int main(void)
{
    int failed = 0;

    failed |= test_roundtrip();
    failed |= test_bad_crc();
    failed |= test_noise_resync();
    failed |= test_too_large_payload_is_rejected();
    failed |= test_timeout();

    if (failed != 0) {
        return 1;
    }

    printf("uartbin tests passed\n");
    return 0;
}
