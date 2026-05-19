\page guide_api_usage API Kullanim Rehberi

# API Kullanim Rehberi

uartbinlib iki katmandan olusur:

- Cerceve katmani: `uartbin_send()`, `uartbin_feed_at()`, `uartbin_poll()`.
- Guvenilir mesaj katmani: `uartbin_send_request()`,
  `uartbin_send_response()`, `uartbin_send_event()`.

Cogu uygulama guvenilir yardimcilari kullanmali, `uartbin_send()` fonksiyonunu
yalnizca acik sira numarasi gereken ozel durumlar icin saklamalidir.

## Temel Kurulum

Her UART linki icin bir `uartbin_t` ayir. Her link kendi RX parser durumunu,
otomatik TX sira sayacini ve opsiyonel retry durumunu tasir.

```c
static uartbin_t link;
static uint8_t rx_payload[256];
static uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 256];

static int uart_write(const uint8_t *data, size_t len, void *user)
{
    /* Tum byte'lari simdi gonder veya bir TX kuyruguna kopyala. */
    return platform_uart_write(data, len, user) == 0 ? 0 : -1;
}

static void on_packet(const uartbin_packet_t *packet, void *user)
{
    (void)user;

    switch (packet->type) {
    case MSG_DALI_COMMAND:
        /* Dogrula/isle ve ayni sira numarasi ile cevapla. */
        uartbin_send_response(&link, packet, MSG_DALI_RESULT, 0, NULL, 0);
        break;

    case MSG_DALI_EVENT:
        /* Peer tarafindan event geldi; ACK ile cevapla. */
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
    /* Say, logla, supervisor'a bildir veya ust seviye session'i resetle. */
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

## Mesaj Gonderme

Bu cihaz peer'dan bir is istiyorsa request kullan:

```c
uartbin_send_request(&link, MSG_DALI_SET_LEVEL, 0, payload, payload_len);
```

Alinan paketi cevaplarken response kullan:

```c
uartbin_send_response(&link, packet, MSG_DALI_RESULT, 0, result, result_len);
```

Modulun DALI bus event'i raporlamasi gibi kendiliginden olusan veri icin event
kullan:

```c
uartbin_send_event(&link, MSG_DALI_BUS_EVENT, 0, event, event_len);
```

## Byte Alma

Alinan her byte'i veya blogu monotonic milisaniye timestamp ile parser'a besle:

```c
uartbin_feed_at(&link, rx_data, rx_len, platform_millis());
```

`uartbin_poll()` fonksiyonunu main loop, scheduler tick veya RTOS task icinden
periyodik cagir. Bu fonksiyon RX parser timeout ve TX retry zamanlamasini
yonetir:

```c
uartbin_poll(&link, platform_millis());
```

Feed fonksiyonlari byte kabul etmeden once yalnizca RX parser timeout kontrolu
yapar. Pending reliable mesajlari yeniden gondermez. TX retry yazmalarinin UART
RX interrupt veya DMA callback icinde beklenmedik sekilde calismamasi icin
retry yolunu acik `uartbin_poll()` cagrinda tut.

## Guvenilir Mesaj Kurallari

Retry acikken:

- `uartbin_send_request()` ve `uartbin_send_event()` kodlanmis cerceveyi saklar.
- Ayni `seq` degerine sahip `UARTBIN_FLAG_RESPONSE` gelmezse cerceve
  `uartbin_poll()` tarafindan yeniden gonderilir.
- Retry sayisi biterse `on_error`, `UARTBIN_ERROR_RETRY_EXHAUSTED` alir.
- Her `uartbin_t` icin ayni anda yalnizca bir reliable request/event pending
  olabilir; baska bir gonderim `UARTBIN_EBUSY` dondurur.

Peer, reliable request/event paketlerine `uartbin_send_response()` ile cevap
vermelidir. Bu fonksiyon gelen sira numarasini otomatik olarak geri yazar.

## Packet Omru

`packet->payload`, ayarlanan RX payload buffer'ini isaret eder. Ayni context
uzerinde bir sonraki feed, poll, reset veya parser islemine kadar gecerlidir.
Daha uzun sure saklanacaksa payload'u `on_packet()` icinde kopyala.
