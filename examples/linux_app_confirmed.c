/**
 * @file linux_app_confirmed.c
 * @brief Linux POSIX serial ile uartbin_app confirmed mesaj ornegi.
 *
 * @example linux_app_confirmed.c
 *
 * Bu ornek, ust seviye `uartbin_app.h` API'sini Linux uzerinde POSIX serial
 * port ile kullanir. Program acilista bir confirmed mesaj gonderir, gelen
 * confirmed mesajlara otomatik ACK doner ve ACK/retry sonuclari callback'lerle
 * loglanir.
 *
 * Tipik derleme:
 *
 * @code
 * cc -I../include examples/linux_app_confirmed.c src/uartbin.c src/uartbin_app.c -o linux_uartbin_app
 * @endcode
 *
 * Tipik calistirma:
 *
 * @code
 * ./linux_uartbin_app /dev/ttyUSB0
 * @endcode
 */
#include "uartbin_app.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
    MSG_APP_HELLO = 0x20u,
    MSG_APP_TEXT = 0x21u
};

typedef struct linux_uartbin_app {
    /** POSIX serial file descriptor. */
    int fd;

    /** Ust seviye otomatik ACK/retry context'i. */
    uartbin_app_t app;

    /** Blok okumalar icin gecici RX buffer. */
    uint8_t rx_block[256];

    /** Tam uygulama payload buffer'i. */
    uint8_t rx_payload[1024];

    /** Confirmed mesaj retry icin frame buffer. */
    uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 1024u];

    /** Protokol seviyeli hata sayaci. */
    unsigned protocol_errors;

    /** ACK alinan confirmed mesaj sayaci. */
    unsigned delivered_count;
} linux_uartbin_app_t;

static uint32_t linux_millis(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }

    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

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

static int linux_uartbin_app_write(const uint8_t *data, size_t len, void *user)
{
    linux_uartbin_app_t *port = (linux_uartbin_app_t *)user;
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

static void linux_uartbin_app_on_message(const uartbin_app_message_t *message, void *user)
{
    linux_uartbin_app_t *port = (linux_uartbin_app_t *)user;

    printf("message type=0x%02X confirmed=%u seq=%u len=%u\n",
           message->type,
           (unsigned)message->confirmed,
           (unsigned)message->seq,
           (unsigned)message->payload_len);

    if (message->type == MSG_APP_HELLO) {
        const uint8_t reply[] = "hello-ack-from-linux";

        /*
         * Bu reply otomatik ACK degildir; uygulama seviyesinde yeni bir
         * confirmed mesajdir. Peer bunun icin de otomatik ACK donecektir.
         */
        (void)uartbin_app_send_confirmed(&port->app, MSG_APP_TEXT, 0, reply, (uint16_t)(sizeof(reply) - 1u));
    }
}

static void linux_uartbin_app_on_delivery(uint16_t seq, void *user)
{
    linux_uartbin_app_t *port = (linux_uartbin_app_t *)user;

    port->delivered_count++;
    printf("delivery ack seq=%u count=%u\n", (unsigned)seq, port->delivered_count);
}

static void linux_uartbin_app_on_error(uartbin_error_t error, void *user)
{
    linux_uartbin_app_t *port = (linux_uartbin_app_t *)user;

    port->protocol_errors++;
    fprintf(stderr, "uartbin_app error=%d count=%u\n", (int)error, port->protocol_errors);
}

static int linux_uartbin_app_init(linux_uartbin_app_t *port, const char *path, unsigned baud)
{
    uartbin_app_config_t cfg;

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
    cfg.write = linux_uartbin_app_write;
    cfg.on_message = linux_uartbin_app_on_message;
    cfg.on_delivery = linux_uartbin_app_on_delivery;
    cfg.on_error = linux_uartbin_app_on_error;
    cfg.user = port;
    cfg.rx_payload_buffer = port->rx_payload;
    cfg.rx_payload_capacity = sizeof(port->rx_payload);
    cfg.rx_timeout_ms = 50u;
    cfg.tx_retry_buffer = port->tx_retry_frame;
    cfg.tx_retry_capacity = sizeof(port->tx_retry_frame);
    cfg.tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS;
    cfg.tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES;

    uartbin_app_init(&port->app, &cfg);
    return 0;
}

static int linux_uartbin_app_run(linux_uartbin_app_t *port)
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
                uartbin_app_feed_at(&port->app, port->rx_block, (size_t)n, linux_millis());
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                uartbin_reset(&port->app.link);
            }
        }

        if (rc > 0 && (pfd.revents & (POLLERR | POLLHUP)) != 0) {
            uartbin_reset(&port->app.link);
        }

        uartbin_app_poll(&port->app, linux_millis());
    }
}

int main(int argc, char **argv)
{
    linux_uartbin_app_t port;
    const char *path = argc > 1 ? argv[1] : "/dev/ttyUSB0";
    const uint8_t hello[] = "hello-from-linux";

    if (linux_uartbin_app_init(&port, path, 115200u) != 0) {
        perror("linux_uartbin_app_init");
        return 1;
    }

    if (uartbin_app_send_confirmed(&port.app, MSG_APP_HELLO, 0, hello, (uint16_t)(sizeof(hello) - 1u)) != UARTBIN_OK) {
        fprintf(stderr, "initial confirmed message could not be queued\n");
    }

    if (linux_uartbin_app_run(&port) != 0) {
        perror("linux_uartbin_app_run");
        close(port.fd);
        return 1;
    }

    close(port.fd);
    return 0;
}
