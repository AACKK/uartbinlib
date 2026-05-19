\page guide_porting Port Etme Rehberi

# Port Etme Rehberi

uartbinlib platformdan bagimsizdir. Bir porta yalnizca dort sey gerekir:

- Statik bir `uartbin_t` context.
- Statik RX payload alani.
- Verilen tum byte'lari kabul eden bir write hook.
- RX byte/block besleme ve `uartbin_poll()` cagirma yolu.

## Port Etme Kontrol Listesi

1. Her UART/protokol linki icin bir context ayir.
2. Kabul edecegin en buyuk uygulama payload'u icin yeterli RX buffer ayir.
3. Opsiyonel: otomatik retry istiyorsan TX retry frame buffer ayir.
4. `uartbin_write_fn` fonksiyonunu uygula.
5. RX byte'larini `uartbin_feed_byte_at()` ile, bloklari `uartbin_feed_at()`
   ile besle.
6. `uartbin_poll()` fonksiyonunu periyodik cagir.
7. UART framing/noise/overrun hatalarinda parser'i `uartbin_reset()` ile
   resetle ve platform RX yolunu yeniden baslat.

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

## Zamanlama

Tum `_at` feed cagrilari ve `uartbin_poll()` icin monotonic milisaniye tick
kullan. Hedefte unsigned subtraction dogru calisiyorsa tick wrap edebilir.

```c
uartbin_feed_at(&link, data, len, platform_millis());
uartbin_poll(&link, platform_millis());
```

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

## Coklu UART Sistemleri

Her UART linki icin bir `uartbin_t`, bir RX payload buffer ve opsiyonel bir TX
retry buffer kullan. Otomatik sira sayaci ve retry durumu `uartbin_t` icinde
yasadigi icin linkler birbirinin durumunu paylasmaz.
