\page guide_linux_usage Linux POSIX Serial Kullanimi

# Linux POSIX Serial Kullanimi

uartbinlib Linux uzerinde de ayni cekirdek API ile kullanilir. Linux tarafinda
STM32 HAL yerine POSIX file descriptor, `termios`, `read`, `write` ve `poll`
kullanilir.

Repo icindeki ornek:

- `examples/linux_posix_serial.c`

Bu ornek zorunlu adapter degil, baslangic kalibidir. Kendi uygulamanda event
loop, thread, epoll veya async yapi kullanabilirsin; onemli olan RX byte'larini
uartbin'e beslemek ve `uartbin_poll()` fonksiyonunu duzenli cagirmaktir.

## Serial Port Acma

Linux'ta seri port genellikle `/dev/ttyUSB0`, `/dev/ttyACM0` veya benzeri bir
path ile acilir:

```c
int fd = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY | O_NONBLOCK);
```

`O_NOCTTY`, portun controlling terminal olmasini engeller. `O_NONBLOCK`, read ve
write yollarinda event loop ile calismayi kolaylastirir.

## termios Ayari

Raw 8N1 mod tipik baslangictir:

```c
struct termios tio;

tcgetattr(fd, &tio);
cfmakeraw(&tio);
tio.c_cflag |= (CLOCAL | CREAD);
tio.c_cflag &= (tcflag_t)~CSTOPB;
tio.c_cflag &= (tcflag_t)~PARENB;
tio.c_cflag &= (tcflag_t)~CSIZE;
tio.c_cflag |= CS8;
tio.c_cc[VMIN] = 0;
tio.c_cc[VTIME] = 0;
cfsetispeed(&tio, B115200);
cfsetospeed(&tio, B115200);
tcsetattr(fd, TCSANOW, &tio);
```

Urun protokolun farkli baud rate veya parity istiyorsa bu ayarlari degistir.

## Write Hook

Linux write hook'u, basari dondurmeden once tum byte'lari fd'ye kabul
ettirmelidir. Non-blocking fd `EAGAIN` dondururse `poll(POLLOUT)` ile kisa
bekleme yapilabilir:

```c
static int linux_uartbin_write(const uint8_t *data, size_t len, void *user)
{
    linux_uartbin_t *port = (linux_uartbin_t *)user;
    size_t sent = 0u;

    while (sent < len) {
        ssize_t n = write(port->fd, &data[sent], len - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { port->fd, POLLOUT, 0 };
            if (poll(&pfd, 1u, 100) > 0 && (pfd.revents & POLLOUT) != 0) {
                continue;
            }
        }

        return -1;
    }

    return 0;
}
```

Bu sozlesme onemlidir; `uartbin_send()` tek cerceve icin write hook'u birden
fazla kez cagirabilir. Hook basari dondurdugu anda verilen byte araligi artik
guvende olmalidir.

## RX ve poll Dongusu

Linux'ta RX icin `poll(POLLIN)` beklenir, gelen blok `uartbin_feed_at()` ile
beslenir. Retry ve RX timeout icin `uartbin_poll()` periyodik cagrilir:

```c
for (;;) {
    struct pollfd pfd = { fd, POLLIN | POLLERR | POLLHUP, 0 };
    int rc = poll(&pfd, 1u, 10);

    if (rc > 0 && (pfd.revents & POLLIN) != 0) {
        uint8_t rx[256];
        ssize_t n = read(fd, rx, sizeof(rx));
        if (n > 0) {
            uartbin_feed_at(&link, rx, (size_t)n, linux_millis());
        }
    }

    if (rc > 0 && (pfd.revents & (POLLERR | POLLHUP)) != 0) {
        uartbin_reset(&link);
    }

    uartbin_poll(&link, linux_millis());
}
```

`poll()` timeout degeri, retry cozunurlugunu belirler. 1 ms ile 10 ms arasi
genelde yeterlidir; CPU yukune ve gecikme ihtiyacina gore sec.

## Otomatik Retry

Linux tarafinda retry, STM32 ile ayni calisir:

- `uartbin_send_request()` veya `uartbin_send_event()` frame'i retry buffer'a
  kaydeder.
- `uartbin_poll()` timeout doldugunda ayni frame'i tekrar yazar.
- Eslesen `UARTBIN_FLAG_RESPONSE` ve ayni `seq` gelirse retry state kapanir.
- Limit dolarsa `UARTBIN_ERROR_RETRY_EXHAUSTED` gelir.

Linux'ta fd write hatalari daha olasi oldugu icin `UARTBIN_ERROR_RETRY_WRITE`
hatasini da loglamak faydalidir.

## Derleme

Tek dosya ornegi su sekilde derlenebilir:

```sh
cc -Iinclude examples/linux_posix_serial.c src/uartbin.c -o linux_uartbin
```

Calistirma:

```sh
./linux_uartbin /dev/ttyUSB0
```

Port yetkisi yoksa kullaniciyi ilgili gruba eklemek gerekebilir. Debian/Ubuntu
tarzi sistemlerde bu grup genellikle `dialout` olur.

## Mimari Notlar

- Linux uygulamasinda her serial fd icin bir `uartbin_t` kullan.
- Her link icin ayri RX payload buffer ve ayri TX retry buffer ayir.
- `packet->payload`, callback sonrasi uzun sure saklanacaksa kopyalanmalidir.
- Event loop icinde `uartbin_poll()` unutulursa retry ve RX timeout calismaz.
- `POLLERR` veya `POLLHUP` gorulurse parser'i resetle ve gerekirse portu
  yeniden ac.
