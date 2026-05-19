/**
 * @file linux_posix_serial.c
 * @brief uartbinlib icin Linux POSIX serial port ornegi.
 *
 * @example linux_posix_serial.c
 *
 * Bu ornek, uartbinlib'i Linux uzerinde POSIX serial port ile kullanmanin
 * basit bir yolunu gosterir. Ornek bloklamayan fd, poll() tabanli main loop ve
 * monotonic milisaniye timestamp kullanir.
 *
 * Tipik derleme:
 *
 * @code
 * cc -I../include examples/linux_posix_serial.c src/uartbin.c -o linux_uartbin
 * @endcode
 *
 * Tipik calistirma:
 *
 * @code
 * ./linux_uartbin /dev/ttyUSB0
 * @endcode
 */
#include "uartbin.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Tek Linux serial link icin uygulama tarafi durum.
 */
typedef struct linux_uartbin {
    /** POSIX serial file descriptor. */
    int fd;

    /** Portable uartbin parser/sender context. */
    uartbin_t link;

    /** Blok okumalar icin gecici RX buffer. */
    uint8_t rx_block[256];

    /** Tam paket payload buffer'i. */
    uint8_t rx_payload[4096];

    /** Otomatik retry icin encode edilmis frame buffer'i. */
    uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 4096u];

    /** Protokol seviyeli hata sayaci. */
    unsigned protocol_errors;
} linux_uartbin_t;

/**
 * @brief CLOCK_MONOTONIC ile milisaniye timestamp dondur.
 */
static uint32_t linux_millis(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }

    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/**
 * @brief termios speed_t degeri sec.
 *
 * Ornek yalnizca yaygin baud rate degerlerini icerir. Gerekiyorsa tabloyu
 * genislet.
 */
static speed_t linux_baud_to_speed(unsigned baud)
{
    switch (baud) {
    case 9600u:
        return B9600;
    case 19200u:
        return B19200;
    case 38400u:
        return B38400;
    case 57600u:
        return B57600;
    case 115200u:
        return B115200;
    default:
        return B115200;
    }
}

/**
 * @brief Serial portu 8N1 raw modda ve non-blocking calisacak sekilde ayarla.
 */
static int linux_serial_configure(int fd, unsigned baud)
{
    struct termios tio;
    speed_t speed = linux_baud_to_speed(baud);

    if (tcgetattr(fd, &tio) != 0) {
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= (tcflag_t)~CSTOPB;
    tio.c_cflag &= (tcflag_t)~PARENB;
    tio.c_cflag &= (tcflag_t)~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0) {
        return -1;
    }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief POSIX write kullanan uartbin TX hook.
 *
 * Hook basari dondurmeden once tum byte'lari fd'ye kabul ettirir. Non-blocking
 * fd EAGAIN dondururse kisa poll() beklemesi yapar.
 */
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
        if (n < 0 && (errno == EINTR)) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;

            pfd.fd = port->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1u, 100) > 0 && (pfd.revents & POLLOUT) != 0) {
                continue;
            }
        }

        return -1;
    }

    return 0;
}

/**
 * @brief CRC'den gecmis paketleri isleyen uygulama callback'i.
 */
static void linux_uartbin_on_packet(const uartbin_packet_t *packet, void *user)
{
    linux_uartbin_t *port = (linux_uartbin_t *)user;

    printf("packet type=0x%02X flags=0x%02X seq=%u len=%u\n",
           packet->type,
           packet->flags,
           (unsigned)packet->seq,
           (unsigned)packet->payload_len);

    if ((packet->flags & UARTBIN_FLAG_REQUEST) != 0u) {
        (void)uartbin_send_response(&port->link, packet, 0x80u, 0, NULL, 0);
    } else if ((packet->flags & UARTBIN_FLAG_EVENT) != 0u) {
        (void)uartbin_send_response(&port->link, packet, 0x81u, 0, NULL, 0);
    }
}

/**
 * @brief Protokol hata callback'i.
 */
static void linux_uartbin_on_error(uartbin_error_t error, void *user)
{
    linux_uartbin_t *port = (linux_uartbin_t *)user;

    port->protocol_errors++;
    fprintf(stderr, "uartbin error=%d count=%u\n", (int)error, port->protocol_errors);
}

/**
 * @brief Linux adapter'i baslat.
 */
static int linux_uartbin_init(linux_uartbin_t *port, const char *path, unsigned baud)
{
    uartbin_config_t cfg;

    memset(port, 0, sizeof(*port));
    port->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port->fd < 0) {
        return -1;
    }
    if (linux_serial_configure(port->fd, baud) != 0) {
        close(port->fd);
        port->fd = -1;
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.write = linux_uartbin_write;
    cfg.on_packet = linux_uartbin_on_packet;
    cfg.on_error = linux_uartbin_on_error;
    cfg.user = port;
    cfg.rx_payload_buffer = port->rx_payload;
    cfg.rx_payload_capacity = sizeof(port->rx_payload);
    cfg.rx_timeout_ms = 50u;
    cfg.tx_retry_buffer = port->tx_retry_frame;
    cfg.tx_retry_capacity = sizeof(port->tx_retry_frame);
    cfg.tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS;
    cfg.tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES;

    uartbin_init(&port->link, &cfg);
    return 0;
}

/**
 * @brief poll() tabanli RX ve retry servis dongusu.
 */
static int linux_uartbin_run(linux_uartbin_t *port)
{
    for (;;) {
        struct pollfd pfd;
        int rc;

        pfd.fd = port->fd;
        pfd.events = POLLIN | POLLERR | POLLHUP;
        pfd.revents = 0;

        rc = poll(&pfd, 1u, 10);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc < 0) {
            return -1;
        }

        if (rc > 0 && (pfd.revents & POLLIN) != 0) {
            ssize_t n = read(port->fd, port->rx_block, sizeof(port->rx_block));
            if (n > 0) {
                uartbin_feed_at(&port->link, port->rx_block, (size_t)n, linux_millis());
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                uartbin_reset(&port->link);
            }
        }

        if (rc > 0 && (pfd.revents & (POLLERR | POLLHUP)) != 0) {
            uartbin_reset(&port->link);
        }

        uartbin_poll(&port->link, linux_millis());
    }
}

int main(int argc, char **argv)
{
    linux_uartbin_t port;
    const char *path = argc > 1 ? argv[1] : "/dev/ttyUSB0";

    if (linux_uartbin_init(&port, path, 115200u) != 0) {
        perror("linux_uartbin_init");
        return 1;
    }

    (void)uartbin_send_event(&port.link, 0x01u, 0, NULL, 0);

    if (linux_uartbin_run(&port) != 0) {
        perror("linux_uartbin_run");
        close(port.fd);
        return 1;
    }

    close(port.fd);
    return 0;
}
