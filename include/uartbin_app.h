/**
 * @file uartbin_app.h
 * @brief uartbin uzerinde otomatik ACK/retry yapan app seviyesi mesaj katmani.
 *
 * Bu header, cekirdek ::uartbin_t frame katmaninin ustunde daha yuksek seviye
 * bir mesaj API'si sunar. Uygulama, confirmed mesaj gonderirken ACK response,
 * seq eslestirme ve retry ayrintilarini elle yonetmez.
 *
 * Tipik kullanim:
 *
 * @code
 * static uartbin_app_t app;
 * static uint8_t rx_payload[256];
 * static uint8_t tx_retry_frame[UARTBIN_MAX_FRAME_OVERHEAD + 256];
 *
 * uartbin_app_config_t cfg = {
 *     .write = uart_write,
 *     .on_message = on_message,
 *     .on_delivery = on_delivery,
 *     .on_error = on_error,
 *     .rx_payload_buffer = rx_payload,
 *     .rx_payload_capacity = sizeof(rx_payload),
 *     .tx_retry_buffer = tx_retry_frame,
 *     .tx_retry_capacity = sizeof(tx_retry_frame),
 *     .tx_retry_timeout_ms = UARTBIN_DEFAULT_RETRY_TIMEOUT_MS,
 *     .tx_retry_max_retries = UARTBIN_DEFAULT_RETRY_MAX_RETRIES
 * };
 *
 * uartbin_app_init(&app, &cfg);
 * uartbin_app_send_confirmed(&app, MSG_DATA, 0, payload, payload_len);
 * @endcode
 */
#ifndef UARTBIN_APP_H
#define UARTBIN_APP_H

#include "uartbin.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Ust seviye app katmaninda otomatik ACK bekleyen mesaj flag'i. */
#define UARTBIN_APP_FLAG_CONFIRMED 0x08u

/** @brief Ust seviye app katmaninda otomatik ACK response flag'i. */
#define UARTBIN_APP_FLAG_ACK 0x10u

/**
 * @brief Ust seviye app katmaninin uygulamaya teslim ettigi mesaj.
 *
 * Bu katmanda ACK/seq/retry ayrintilari otomatik yonetilir. Uygulama yalnizca
 * gercek mesajlari gorur; otomatik ACK response paketleri bu callback'e
 * dusmez.
 */
typedef struct uartbin_app_message {
    /** Uygulama tarafindan tanimlanan mesaj tipi. */
    uint8_t type;

    /** Uygulama flag'leri; internal app flag'leri maskelenmis olarak gelir. */
    uint8_t flags;

    /** Mesajin confirmed olarak gelip gelmedigi. */
    uint8_t confirmed;

    /** Mesajin sira numarasi; cogu uygulama bunu kullanmak zorunda degildir. */
    uint16_t seq;

    /** CRC dogrulamasi gecmis payload byte'larina pointer. */
    const uint8_t *payload;

    /** Payload pointer'indaki gecerli byte sayisi. */
    uint16_t payload_len;
} uartbin_app_message_t;

/**
 * @brief App katmaninda gercek mesaj alindiginda cagrilir.
 */
typedef void (*uartbin_app_message_fn)(const uartbin_app_message_t *message, void *user);

/**
 * @brief App katmaninda confirmed mesaj ACK aldiginda cagrilir.
 *
 * Bu callback yalnizca beklenen pending confirmed mesaja ait ACK geldiginde
 * cagrilir. ACK gelmez ve retry limiti dolarsa sonuc ::uartbin_error_fn
 * uzerinden ::UARTBIN_ERROR_RETRY_EXHAUSTED olarak bildirilir.
 */
typedef void (*uartbin_app_delivery_fn)(uint16_t seq, void *user);

/**
 * @brief Ust seviye otomatik ACK/retry mesajlasma katmani konfigurasyonu.
 *
 * Bu konfigurasyon, alt ::uartbin_config alanlarinin app katmanina uyarlanmis
 * halidir. `write` hook'u uygulamanin verdigi `user` pointer'i ile cagrilir.
 * `on_message` yalnizca gercek uygulama mesajlarini gorur; otomatik ACK
 * response paketleri burada teslim edilmez.
 */
typedef struct uartbin_app_config {
    /** Alt uartbin_send() tarafindan kullanilan TX hook. */
    uartbin_write_fn write;

    /** ACK disindaki gercek mesajlar icin uygulama callback'i. NULL olabilir. */
    uartbin_app_message_fn on_message;

    /** Gonderilen confirmed mesaj ACK aldiginda cagrilan callback. NULL olabilir. */
    uartbin_app_delivery_fn on_delivery;

    /** Parser, timeout ve retry hatalari icin cagrilan callback. NULL olabilir. */
    uartbin_error_fn on_error;

    /** Hook ve app callback'lerine iletilen opaque uygulama pointer'i. */
    void *user;

    /** Uygulamanin sahip oldugu statik payload buffer. */
    uint8_t *rx_payload_buffer;

    /** rx_payload_buffer boyutu. Maksimum RX payload degeridir. */
    uint16_t rx_payload_capacity;

    /** Milisaniye cinsinden RX timeout. 0 timeout'u kapatir. */
    uint32_t rx_timeout_ms;

    /** Confirmed mesaj retry icin kullanilan statik frame buffer. */
    uint8_t *tx_retry_buffer;

    /** tx_retry_buffer boyutu, byte cinsinden. */
    uint16_t tx_retry_capacity;

    /** Confirmed mesaj retry timeout'u. 0 retry'yi kapatir. */
    uint32_t tx_retry_timeout_ms;

    /** UARTBIN_ERROR_RETRY_EXHAUSTED oncesi yeniden gonderim sayisi. */
    uint8_t tx_retry_max_retries;
} uartbin_app_config_t;

/**
 * @brief Ust seviye otomatik ACK/retry mesajlasma context'i.
 *
 * Her UART/protokol linki icin bir context ayir. Context icinde alt
 * ::uartbin_t bulundugu ve internal callback'lerde kendi adresi kullanildigi
 * icin init sonrasinda baska bir adrese kopyalanmamalidir.
 */
typedef struct uartbin_app {
    /** Alt frame/retry katmani context'i. */
    uartbin_t link;

    /** Kullanici konfigurasyonunun kopyasi. */
    uartbin_app_config_t cfg;

    /** Duplicate confirmed mesajlari filtrelemek icin son ACK'lenen seq gecerli mi. */
    uint8_t last_rx_confirmed_valid;

    /** Son ACK'lenen confirmed mesaj tipi. */
    uint8_t last_rx_confirmed_type;

    /** Son ACK'lenen confirmed mesaj sira numarasi. */
    uint16_t last_rx_confirmed_seq;

    /** Gonderilen confirmed mesaj icin ACK bekleniyor mu. */
    uint8_t tx_confirmed_pending;

    /** ACK beklenen confirmed mesaj sira numarasi. */
    uint16_t tx_confirmed_seq;
} uartbin_app_t;

/**
 * @brief Ust seviye app context'ini baslat.
 *
 * Bu API, alt uartbin context'ini internal callback'lerle kurar. Uygulama
 * ACK/response gondermek zorunda kalmadan confirmed mesaj alip gonderebilir.
 * Baslatilan ::uartbin_app_t context kopyalanmamalidir; alt katman internal
 * callback user pointer'i olarak bu adresi kullanir.
 *
 * @param app Baslatilacak app context'i.
 * @param config @p app icine kopyalanacak konfigurasyon.
 */
void uartbin_app_init(uartbin_app_t *app, const uartbin_app_config_t *config);

/**
 * @brief ACK beklemeyen normal app mesaji gonder.
 *
 * Bu fonksiyon otomatik ACK beklemez ve retry state'i baslatmaz. Peer tarafinda
 * `on_message` callback'i cagrilir; fakat ACK response uretilmez.
 *
 * @param app Baslatilmis app context'i.
 * @param type Uygulama mesaj tipi.
 * @param flags Uygulama flag'leri. App katmaninin internal flag bitleri
 * maskelenir.
 * @param payload Payload byte'lari; @p payload_len 0 ise NULL olabilir.
 * @param payload_len Payload byte sayisi.
 * @return Ilk gonderim sonucu.
 */
uartbin_status_t uartbin_app_send(uartbin_app_t *app,
                                  uint8_t type,
                                  uint8_t flags,
                                  const uint8_t *payload,
                                  uint16_t payload_len);

/**
 * @brief Otomatik ACK/retry ile confirmed app mesaji gonder.
 *
 * Peer tarafinda uartbin_app katmani kullaniliyorsa ACK otomatik doner.
 * ACK gelmezse retry ayarlari dogrultusunda ayni mesaj tekrar gonderilir.
 *
 * @param app Baslatilmis app context'i.
 * @param type Uygulama mesaj tipi.
 * @param flags Uygulama flag'leri. App katmaninin internal flag bitleri
 * maskelenir.
 * @param payload Payload byte'lari; @p payload_len 0 ise NULL olabilir.
 * @param payload_len Payload byte sayisi.
 * @return Ilk gonderim baslatma sonucu. Sonradan ACK alinmasi
 * ::uartbin_app_delivery_fn ile, retry limitinin dolmasi ::uartbin_error_fn ile
 * bildirilir.
 */
uartbin_status_t uartbin_app_send_confirmed(uartbin_app_t *app,
                                            uint8_t type,
                                            uint8_t flags,
                                            const uint8_t *payload,
                                            uint16_t payload_len);

/**
 * @brief Pending confirmed mesaj retry durumunu iptal et.
 *
 * RX parser resetlenmez ve yeni frame gonderilmez. Uygulama bekleyen confirmed
 * mesaji bilincli olarak terk etmek istediginde kullanilir.
 */
void uartbin_app_cancel(uartbin_app_t *app);

/** @brief Alinan byte'lari app katmanina besle. */
void uartbin_app_feed(uartbin_app_t *app, const uint8_t *data, size_t len);

/** @brief Alinan byte'lari timestamp ile app katmanina besle. */
void uartbin_app_feed_at(uartbin_app_t *app, const uint8_t *data, size_t len, uint32_t now_ms);

/** @brief Tek alinan byte'i app katmanina besle. */
void uartbin_app_feed_byte(uartbin_app_t *app, uint8_t byte);

/** @brief Tek alinan byte'i timestamp ile app katmanina besle. */
void uartbin_app_feed_byte_at(uartbin_app_t *app, uint8_t byte, uint32_t now_ms);

/**
 * @brief RX timeout ve confirmed mesaj retry zamanlamasini servis et.
 *
 * `tx_retry_timeout_ms` sifir disi ise retry mekanizmasi bu fonksiyonla surulur.
 * Fonksiyonu main loop, scheduler tick veya RTOS task icinden periyodik cagir.
 */
void uartbin_app_poll(uartbin_app_t *app, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
