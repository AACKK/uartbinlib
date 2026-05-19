\page guide_ring_buffer Halka Buffer TX Portu

# Halka Buffer TX Portu

Halka buffer, `write` hook'unu non-blocking yapmak icin yaygin yoldur. Hook,
byte'lari statik TX halkasi icine kopyalar ve UART TX-empty interrupt veya DMA TX
yolunu etkinlestirir. Basariyi yalnizca her byte kabul edildiyse dondurur.

## Minimal Halka Buffer Yapisi

```c
#define TX_RING_SIZE 512u

typedef struct {
    uint8_t data[TX_RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} tx_ring_t;

static tx_ring_t tx_ring;
```

Full ve empty durumlarini ayirmak icin bir slot bos birak:

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

Hook, TX interrupt'a gore atomic olmalidir. Byte kopyalarken ilgili UART TX
interrupt'unu kapat veya kisa bir critical section kullan.

Kopyalamadan once bos alani kontrol et; hook all-or-nothing davranmalidir.
Cercevenin yalnizca bir kismini kuyruga alip hata dondurmek hatta bozuk partial
cerceve cikmasina neden olabilir.

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

Halka buffer yeterli bos alana sahip degilse non-zero dondur. Send API `UARTBIN_EWRITE`
dondurur; retry sirasindaki yazma hatasinda ise
`UARTBIN_ERROR_RETRY_WRITE` bildirilir.

## TX Interrupt Tarafi

UART TX-empty interrupt icinde:

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

## Pratik Notlar

- Ring boyutunu bekledigin en buyuk burst'e gore sec.
- `UARTBIN_MAX_FRAME_OVERHEAD + max_payload`, tek cercevenin en buyuk boyudur.
- Retry aciksa retry frame buffer, TX halkasindan ayridir.
- `head`/`tail` guncellemelerini interrupt race durumlarina karsi koru.
- uartbin'in gecici lokal buffer pointer'larini saklama; byte'lari her zaman
  kopyala.
