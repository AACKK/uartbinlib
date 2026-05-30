\page guide_porting Port Etme Rehberi

# Port Etme Rehberi

uartbinlib platformdan bagimsizdir. Bir porta yalnizca dort sey gerekir:

- Statik bir `uartbin_t` veya ust katman kullaniliyorsa `uartbin_app_t` context.
- Statik RX payload alani.
- Verilen tum byte'lari kabul eden bir write hook.
- RX byte/block besleme ve uygun poll fonksiyonunu cagirma yolu.

Port islemi iki seviyede yapilabilir:

- Cekirdek port: `uartbin.h` ile frame, CRC, seq, request/response/event
  API'lerini dogrudan kullanirsin.
- App katmani portu: `uartbin_app.h` ile confirmed mesaj, otomatik ACK ve retry
  davranisini kullanirsin.

Iki seviye ayni transport hook'larini kullanir; fark, uygulama callback'lerinin
ve poll/feed fonksiyonlarinin hangi context'e yonlendirildigidir.

## Port Etme Kontrol Listesi

### Cekirdek `uartbin_t` icin

1. Her UART/protokol linki icin bir context ayir.
2. Kabul edecegin en buyuk uygulama payload'u icin yeterli RX buffer ayir.
3. Opsiyonel: otomatik retry istiyorsan TX retry frame buffer ayir.
4. `uartbin_write_fn` fonksiyonunu uygula.
5. RX byte'larini `uartbin_feed_byte_at()` ile, bloklari `uartbin_feed_at()`
   ile besle.
6. `uartbin_poll()` fonksiyonunu periyodik cagir.
7. UART framing/noise/overrun hatalarinda parser'i `uartbin_reset()` ile
   resetle ve platform RX yolunu yeniden baslat.

### App katmani `uartbin_app_t` icin

1. Her UART/protokol linki icin bir `uartbin_app_t` context ayir.
2. RX payload buffer ayir.
3. Confirmed mesaj kullanacaksan TX retry frame buffer ayir.
4. Ayni `uartbin_write_fn` sozlesmesine uyan write hook'u uygula.
5. `uartbin_app_config_t` icinde `on_message`, `on_delivery`, `on_error` ve
   buffer alanlarini bagla.
6. RX byte'larini `uartbin_app_feed_byte_at()` ile, bloklari
   `uartbin_app_feed_at()` ile besle.
7. Main loop veya task icinden yalnizca `uartbin_app_poll()` cagir.
8. Ayni link icin ayrica `uartbin_poll(&app.link, now_ms)` cagirma; app poll
   bunu kendi icinde yapar.

## Write Hook Sozlesmesi

Write hook, donmeden once tum byte'lari gondermeli veya uygulama tarafindan
sahip olunan TX kuyruguna kopyalamalidir. Byte'lar guvende degilken basari
dondurmek cerceveleri bozabilir; cunku kutuphane tek paket icin hook'u birden
fazla kez cagirabilir.

Bloklayan TX basittir:

```c
static int uart_write(const uint8_t *data, size_t len, void *user)
{
    return HAL_UART_Transmit((UART_HandleTypeDef *)user,
                             (uint8_t *)data,
                             (uint16_t)len,
                             100) == HAL_OK ? 0 : -1;
}
```

Interrupt/RTOS sistemlerinde queued TX daha iyidir, fakat queue push islemi
donmeden once tum byte araligini kopyalamalidir.

Bu sozlesme app katmani icin de aynidir. `uartbin_app_config_t.write` hook'u,
alt katmandaki `uartbin_send()` tarafindan kullanilir. Bu nedenle hook, `data`
pointer'ini saklamamali; byte'lari donmeden once gondermeli veya kendi TX
kuyruguna kopyalamalidir.

## Cekirdek Port Iskeleti

Asagidaki iskelet `uartbin.h` API'sini dogrudan kullanan bir port icindir.

```c
#include "uartbin.h"

typedef struct my_uartbin_port {
    platform_uart_t *uart;
    uartbin_t link;
    uint8_t rx_payload[256];
    uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 256u];
    uint32_t protocol_errors;
} my_uartbin_port_t;

static int my_uart_write(const uint8_t *data, size_t len, void *user)
{
    my_uartbin_port_t *port = (my_uartbin_port_t *)user;

    return platform_uart_write_all(port->uart, data, len) == 0 ? 0 : -1;
}

static void my_on_packet(const uartbin_packet_t *packet, void *user)
{
    my_uartbin_port_t *port = (my_uartbin_port_t *)user;

    if ((packet->flags & UARTBIN_FLAG_REQUEST) != 0u) {
        (void)uartbin_send_response(&port->link, packet, MSG_RESULT, 0, NULL, 0);
    }
}

static void my_on_error(uartbin_error_t error, void *user)
{
    my_uartbin_port_t *port = (my_uartbin_port_t *)user;
    (void)error;

    port->protocol_errors++;
}

void my_port_init(my_uartbin_port_t *port, platform_uart_t *uart)
{
    uartbin_config_t cfg;

    memset(port, 0, sizeof(*port));
    port->uart = uart;

    memset(&cfg, 0, sizeof(cfg));
    cfg.write = my_uart_write;
    cfg.on_packet = my_on_packet;
    cfg.on_error = my_on_error;
    cfg.user = port;
    cfg.rx_payload_buffer = port->rx_payload;
    cfg.rx_payload_capacity = sizeof(port->rx_payload);
    cfg.rx_timeout_ms = 50u;
    cfg.tx_retry_buffer = port->tx_retry_frame;
    cfg.tx_retry_capacity = sizeof(port->tx_retry_frame);
    cfg.tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS;
    cfg.tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES;

    uartbin_init(&port->link, &cfg);
}

void my_port_rx_bytes(my_uartbin_port_t *port, const uint8_t *data, size_t len)
{
    uartbin_feed_at(&port->link, data, len, platform_millis());
}

void my_port_poll(my_uartbin_port_t *port)
{
    uartbin_poll(&port->link, platform_millis());
}
```

## App Katmani Port Iskeleti

Bu iskelet `uartbin_app.h` API'sini kullanir. Uygulama confirmed mesaj
gonderirken ACK/seq/retry ayrintilarini elle yonetmez.

```c
#include "uartbin_app.h"

typedef struct my_uartbin_app_port {
    platform_uart_t *uart;
    uartbin_app_t app;
    uint8_t rx_payload[256];
    uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 256u];
    uint32_t protocol_errors;
    uint32_t delivered_count;
} my_uartbin_app_port_t;

static int my_app_uart_write(const uint8_t *data, size_t len, void *user)
{
    my_uartbin_app_port_t *port = (my_uartbin_app_port_t *)user;

    return platform_uart_write_all(port->uart, data, len) == 0 ? 0 : -1;
}

static void my_app_on_message(const uartbin_app_message_t *message, void *user)
{
    my_uartbin_app_port_t *port = (my_uartbin_app_port_t *)user;

    switch (message->type) {
    case MSG_COMMAND:
        /* Transport ACK otomatik doner; bu ayrica uygulama seviyeli cevaptir. */
        (void)uartbin_app_send_confirmed(&port->app, MSG_RESULT, 0, NULL, 0);
        break;

    default:
        break;
    }
}

static void my_app_on_delivery(uint16_t seq, void *user)
{
    my_uartbin_app_port_t *port = (my_uartbin_app_port_t *)user;
    (void)seq;

    port->delivered_count++;
}

static void my_app_on_error(uartbin_error_t error, void *user)
{
    my_uartbin_app_port_t *port = (my_uartbin_app_port_t *)user;
    (void)error;

    port->protocol_errors++;
}

void my_app_port_init(my_uartbin_app_port_t *port, platform_uart_t *uart)
{
    uartbin_app_config_t cfg;

    memset(port, 0, sizeof(*port));
    port->uart = uart;

    memset(&cfg, 0, sizeof(cfg));
    cfg.write = my_app_uart_write;
    cfg.on_message = my_app_on_message;
    cfg.on_delivery = my_app_on_delivery;
    cfg.on_error = my_app_on_error;
    cfg.user = port;
    cfg.rx_payload_buffer = port->rx_payload;
    cfg.rx_payload_capacity = sizeof(port->rx_payload);
    cfg.rx_timeout_ms = 50u;
    cfg.tx_retry_buffer = port->tx_retry_frame;
    cfg.tx_retry_capacity = sizeof(port->tx_retry_frame);
    cfg.tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS;
    cfg.tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES;

    uartbin_app_init(&port->app, &cfg);
}

void my_app_port_rx_bytes(my_uartbin_app_port_t *port, const uint8_t *data, size_t len)
{
    uartbin_app_feed_at(&port->app, data, len, platform_millis());
}

void my_app_port_poll(my_uartbin_app_port_t *port)
{
    uartbin_app_poll(&port->app, platform_millis());
}
```

App katmani portunda `uartbin_app_send_confirmed()` ilk gonderimin baslayip
baslamadigini return eder. ACK sonradan gelirse `on_delivery()` cagrilir. ACK
hic gelmez ve retry limiti dolarsa `on_error(UARTBIN_ERROR_RETRY_EXHAUSTED, user)`
cagrilir.

## Zamanlama

Tum `_at` feed cagrilari ve `uartbin_poll()` icin monotonic milisaniye tick
kullan. Hedefte unsigned subtraction dogru calisiyorsa tick wrap edebilir.

```c
uartbin_feed_at(&link, data, len, platform_millis());
uartbin_poll(&link, platform_millis());
```

App katmaninda feed ve poll fonksiyonlari app context'e yonlendirilir:

```c
uartbin_app_feed_at(&app, data, len, platform_millis());
uartbin_app_poll(&app, platform_millis());
```

Her fiziksel link icin tek poll yolu sec:

- Cekirdek API kullaniyorsan `uartbin_poll(&link, now_ms)`.
- App API kullaniyorsan `uartbin_app_poll(&app, now_ms)`.

App poll, alt `uartbin_poll()` cagrimini kendi icinde yaptigi icin ikisini ayni
link icin birlikte cagirma.

## RX Baglama Kaliplari

### Tek byte interrupt

Her RX complete callback'inde byte'i besle ve hemen sonraki RX'i arm et:

```c
void UART_RxCompleteCallback(void)
{
    uartbin_feed_byte_at(&link, rx_byte, platform_millis());
    platform_uart_receive_it(&rx_byte, 1u);
}
```

App katmani icin ayni kalip:

```c
void UART_RxCompleteCallback(void)
{
    uartbin_app_feed_byte_at(&app, rx_byte, platform_millis());
    platform_uart_receive_it(&rx_byte, 1u);
}
```

### DMA veya blok RX

DMA idle-line, POSIX `read()` veya RTOS stream buffer gibi blok kaynaklarinda
alinan byte araligini oldugu gibi feed et:

```c
void on_rx_block(const uint8_t *data, size_t len)
{
    uartbin_feed_at(&link, data, len, platform_millis());
}
```

App katmani icin:

```c
void on_rx_block(const uint8_t *data, size_t len)
{
    uartbin_app_feed_at(&app, data, len, platform_millis());
}
```

Feed buffer'inin tam bir frame icermesi gerekmez. Parser SOF, header, payload ve
CRC durumunu context icinde tutar.

## Hata Yonetimi

Parser ve reliable-TX hatalari `on_error` ile gelir:

- `UARTBIN_ERROR_BAD_VERSION`: desteklenmeyen protokol surumu.
- `UARTBIN_ERROR_BAD_LENGTH`: payload RX kapasitesinden buyuk.
- `UARTBIN_ERROR_CRC`: cerceve bozulmasi.
- `UARTBIN_ERROR_RX_OVERFLOW`: non-empty payload icin RX payload buffer yok.
- `UARTBIN_ERROR_TIMEOUT`: yarim RX cercevesi timeout oldu.
- `UARTBIN_ERROR_RETRY_EXHAUSTED`: reliable mesaj zamaninda cevaplanmadi.
- `UARTBIN_ERROR_RETRY_WRITE`: retry write hook basarisiz oldu.

Protokol hatalarini platform UART hatalarindan ayri tut. Platform hatalari UART
peripheral'i yeniden baslatmali ve `uartbin_reset()` cagirmalidir.

App katmani kullaniliyorsa platform UART hatasinda alt parser'i resetlemek icin
`uartbin_reset(&app.link)` kullanilabilir. Bu, app context'in konfigurasyonunu
ve pending retry bilgisini silmez; yalnizca yarim RX frame'i atar. Bilincli
olarak pending confirmed mesaji terk etmek istiyorsan `uartbin_app_cancel()`
kullan.

## Buffer Boyutlari

RX payload buffer'i, kabul edecegin en buyuk payload kadar olmalidir. Gelen
frame daha buyuk payload ilan ederse parser `UARTBIN_ERROR_BAD_LENGTH` bildirir
ve frame'i atar.

TX retry frame buffer'i payload'dan daha buyuktur; cunku SOF, header ve CRC de
saklanir:

```c
static uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + MAX_PAYLOAD];
```

Retry kapaliysa cekirdek request/event veya app confirmed mesajlari retry
etmez. Confirmed mesaj semantigi istiyorsan `tx_retry_timeout_ms`,
`tx_retry_max_retries` ve yeterli `tx_retry_buffer` ver.

## Coklu UART Sistemleri

Her UART linki icin bir `uartbin_t`, bir RX payload buffer ve opsiyonel bir TX
retry buffer kullan. Otomatik sira sayaci ve retry durumu `uartbin_t` icinde
yasadigi icin linkler birbirinin durumunu paylasmaz.

App katmani icin her UART linki kendi `uartbin_app_t` context'ine, kendi RX
payload buffer'ina ve kendi TX retry frame buffer'ina sahip olmalidir.
`uartbin_app_t` init edildikten sonra kopyalanmamalidir.

## Port Dogrulama Kontrol Listesi

Portu ilk calistirirken su senaryolari test et:

- Tek frame gonder/al: type, flags, seq ve payload beklenen gibi mi?
- CRC boz: `UARTBIN_ERROR_CRC` geliyor mu?
- RX payload kapasitesinden buyuk frame: `UARTBIN_ERROR_BAD_LENGTH` geliyor mu?
- Yarim frame ve timeout: `UARTBIN_ERROR_TIMEOUT` geliyor mu?
- Retry acik confirmed/request: ACK/response yokken tekrar gonderiliyor mu?
- Retry limiti dolunca `UARTBIN_ERROR_RETRY_EXHAUSTED` geliyor mu?
- App katmaninda duplicate confirmed frame uygulamaya ikinci kez teslim edilmiyor mu?
- UART hata callback'i veya `POLLHUP/POLLERR` sonrasi parser resetlenip RX yeniden
  basliyor mu?
