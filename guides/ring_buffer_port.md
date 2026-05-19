\page guide_ring_buffer Ring Buffer TX Port

# Ring Buffer TX Port

A ring buffer is the usual way to make the `write` hook non-blocking. The hook
copies bytes into a static TX ring and enables the UART TX-empty interrupt or
DMA transmitter. It returns success only if every byte was accepted.

## Minimal Ring Buffer Shape

```c
#define TX_RING_SIZE 512u

typedef struct {
    uint8_t data[TX_RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} tx_ring_t;

static tx_ring_t tx_ring;
```

Keep one slot empty so full and empty are distinct:

```c
static uint16_t ring_next(uint16_t index)
{
    return (uint16_t)((index + 1u) % TX_RING_SIZE);
}

static int ring_push_byte(tx_ring_t *ring, uint8_t byte)
{
    uint16_t next = ring_next(ring->head);

    if (next == ring->tail) {
        return -1;
    }

    ring->data[ring->head] = byte;
    ring->head = next;
    return 0;
}
```

## uartbin Write Hook

The hook must be atomic with respect to the TX interrupt. Disable the relevant
UART TX interrupt or enter a short critical section while pushing bytes.

Check free space before copying so the hook is all-or-nothing. Returning an
error after only part of a frame was queued can put a corrupted partial frame on
the wire.

```c
static uint16_t ring_used(const tx_ring_t *ring)
{
    if (ring->head >= ring->tail) {
        return (uint16_t)(ring->head - ring->tail);
    }

    return (uint16_t)(TX_RING_SIZE - ring->tail + ring->head);
}

static uint16_t ring_free(const tx_ring_t *ring)
{
    return (uint16_t)(TX_RING_SIZE - 1u - ring_used(ring));
}
```

```c
static int uartbin_ring_write(const uint8_t *data, size_t len, void *user)
{
    tx_ring_t *ring = (tx_ring_t *)user;

    platform_tx_lock();

    if (len > ring_free(ring)) {
        platform_tx_unlock();
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        (void)ring_push_byte(ring, data[i]);
    }

    platform_uart_enable_tx_empty_irq();
    platform_tx_unlock();
    return 0;
}
```

If the ring does not have enough space, return non-zero. The send API will
return `UARTBIN_EWRITE`, or retry logic will report
`UARTBIN_ERROR_RETRY_WRITE` during retransmission.

## TX Interrupt Side

In the UART TX-empty interrupt:

```c
void UART_TX_IRQHandler(void)
{
    if (tx_ring.tail == tx_ring.head) {
        platform_uart_disable_tx_empty_irq();
        return;
    }

    platform_uart_write_data_register(tx_ring.data[tx_ring.tail]);
    tx_ring.tail = ring_next(tx_ring.tail);
}
```

## Practical Notes

- Size the ring for the largest burst you expect.
- `UARTBIN_MAX_FRAME_OVERHEAD + max_payload` is the largest single frame size.
- If retry is enabled, the retry frame buffer is separate from the TX ring.
- Protect `head`/`tail` updates from interrupt races.
- Do not store pointers to uartbin's temporary local buffers; always copy bytes.
