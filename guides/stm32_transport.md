\page guide_stm32_transport STM32 Interrupt ve DMA Kullanimi

# STM32 Interrupt ve DMA Kullanimi

Portable kutuphane STM32 HAL'e bagimli degildir, fakat repo iki adapter ornegi
icerir:

- `examples/stm32_hal_interrupt.c`
- `examples/stm32_hal_dma_idle.c`
- `examples/stm32_app_confirmed.c`

Bunlari zorunlu kutuphane kodu gibi degil, kalip olarak kullan.

## Interrupt RX

Tek byte interrupt RX basit ve guvenilirdir:

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uartbin_feed_byte_at(&link, rx_byte, HAL_GetTick());
    HAL_UART_Receive_IT(huart, &rx_byte, 1);
}
```

Onerilen davranis:

- Gecerli byte'i besledikten hemen sonra sonraki byte RX'i arm et.
- HAL noise/framing/parity/overrun hatalarindan sonra `uartbin_reset()` cagir.
- Abort/error recovery sonrasinda RX'i yeniden baslat.
- `uartbin_poll()` fonksiyonunu main loop veya scheduler icinden cagir.
- Retry'yi `uartbin_poll()` sursun; RX callback'leri yalnizca gelen byte'lari
  beslesin.

Bu model debug etmesi kolaydir ve orta baud rate'lerde iyi calisir.

## DMA Idle-Line RX

DMA idle-line RX blok alir ve uartbinlib'e besler:

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    uartbin_feed_at(&link, dma_rx_buffer, size, HAL_GetTick());
    HAL_UARTEx_ReceiveToIdle_DMA(huart, dma_rx_buffer, sizeof(dma_rx_buffer));
}
```

Onerilen davranis:

- DMA buffer'in tam bir uartbin cercevesi tasimasi gerekmez.
- Yalnizca yeni alinmis gecerli byte'lari besle.
- Her idle event sonrasinda `HAL_UARTEx_ReceiveToIdle_DMA()` yeniden baslat.
- Adapter offset takip etmiyorsa DMA half-transfer interrupt'larini kapat.
- HAL UART hatalarinda DMA'i abort/restart et ve `uartbin_reset()` cagir.

Bu model yuksek baud rate veya burst tarzi trafik icin daha uygundur.

## TX Secenekleri

STM32 ornekleri blocking `HAL_UART_Transmit()` kullanir; cunku bu, write hook
sozlesmesini en az kodla saglar. Production projelerde bunu genelde sunlardan
biriyle degistirirsin:

- TX-empty interrupt ile surulen UART TX ring buffer.
- DMA TX queue.
- UART task tarafindan bosaltilan RTOS stream buffer.

Bu ucu de gecerlidir; tek sart write hook'un basari dondurmeden once tum
byte'lari kopyalamasi veya kabul etmesidir.

## STM32 ile Retry

Otomatik retry icin `uartbin_poll()` duzenli cagrilmalidir:

```c
void main_loop(void)
{
    uartbin_poll(&link, HAL_GetTick());
}
```

RTOS sistemlerinde 1 ms ile 10 ms arasi periyodik task genelde yeterlidir.
`tx_retry_timeout_ms` degerini peer'in normal round-trip suresinden ve
uygulama seviyesindeki beklenen islem gecikmesinden buyuk sec.

Retry zamanlamasi icin RX callback'lere guvenme. Feed callback'leri yalnizca RX
parser timeout kontrolu yapar; TX retry acik poll yolunda calisir.

## STM32 App Katmani

Uygulama kodunun ACK/seq/retry ayrintilariyla ilgilenmesini istemiyorsan
`uartbin_app.h` katmanini kullan. STM32 tarafinda RX callback'leri
`uartbin_app_feed_byte_at()` veya `uartbin_app_feed_at()` cagirir; main loop ise
`uartbin_app_poll()` ile timeout ve retry servis eder.

`examples/stm32_app_confirmed.c`, interrupt RX ile bu kalibi gosterir:

```c
void main_loop(void)
{
    stm32_uartbin_app_it_poll(&g_link);
}

void send_status(void)
{
    (void)stm32_uartbin_app_it_send_status(&g_link, payload, payload_len);
}
```

Gelen confirmed mesajlar icin ACK otomatik doner. Uygulama seviyesinde ayrica
feedback vermek istersen `on_message()` icinden yeni bir
`uartbin_app_send_confirmed()` cagrisi yapabilirsin.

Ayni link icin hem `uartbin_app_poll(&app, now_ms)` hem de
`uartbin_poll(&app.link, now_ms)` cagirma. App poll fonksiyonu alt
`uartbin_poll()` cagrimini kendi icinde yapar.
