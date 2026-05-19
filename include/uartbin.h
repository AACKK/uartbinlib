/**
 * @file uartbin.h
 * @brief Platformdan bagimsiz binary UART cerceveleme kutuphanesi.
 *
 * uartbinlib, UART gibi byte akislari uzerinden degisken uzunluklu binary
 * paket gonderip almak icin kucuk bir C API sunar. Cekirdek kutuphane HAL,
 * RTOS, interrupt, DMA veya dinamik bellek bagimliligi tasimaz. Tum buffer'lar
 * uygulamaya aittir; kutuphane hedef platforma hook ve callback'lerle baglanir.
 *
 * @section uartbin_h_overview Genel Bakis
 *
 * uartbinlib binary payload'lari su sekilde cerceveler:
 *
 * @code
 * SOF(2) | version(1) | type(1) | flags(1) | reserved(1) |
 * seq(2) | payload_len(2) | payload(N) | crc16(2)
 * @endcode
 *
 * Alici, tam payload'u kullanicinin verdigi statik buffer icinde saklar. Paket
 * callback'i yalnizca tum cerceve CRC dogrulamasindan gectikten sonra cagrilir.
 * Bu tasarim API'yi basit tutar ve gomulu komut, telemetri, konfigurasyon ve
 * firmware parca protokolleri icin uygun hale getirir.
 *
 * Uygulamalar acik sira numarasi ile dusuk seviye uartbin_send() API'sini veya
 * daha ust seviye request/response/event yardimcilarini kullanabilir.
 * Yardimcilar context basina otomatik sira sayaci tutar, response icin sira
 * numarasini geri yazar ve opsiyonel olarak eslesen response gelene kadar
 * request/event cercevelerini retry edebilir.
 *
 * @section uartbin_h_no_alloc Bellek Modeli
 *
 * Kutuphane bellek ayirmaz. Uygulama sunlari verir:
 *
 * - kalici bir ::uartbin_t context,
 * - statik RX payload buffer,
 * - opsiyonel statik TX retry frame buffer,
 * - TX icin write hook,
 * - packet ve error callback'leri.
 *
 * @section uartbin_h_timing Zaman Asimi Modeli
 *
 * ::uartbin_config::rx_timeout_ms degerini sifir disi ayarla ve uartbin_poll()
 * fonksiyonunu periyodik cagir. Timestamp alan feed fonksiyonlari da her
 * byte/block kabul etmeden once RX timeout kontrolu yapar. Ayni uartbin_poll()
 * cagrisi, ::uartbin_config::tx_retry_timeout_ms ayarlandiginda opsiyonel TX
 * retry zamanlamasini surer.
 *
 * @section uartbin_h_stm32 STM32 Entegrasyonu
 *
 * Portable cekirdek STM32 header'lari icermez. Interrupt ve DMA idle-line
 * adapter kaliplari ile HAL hata recovery icin @c examples/ altindaki dosyalara
 * bak.
 */
#ifndef UARTBIN_H
#define UARTBIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Kutuphane semantik versiyon major degeri. */
#define UARTBINLIB_VERSION_MAJOR 1u

/** @brief Kutuphane semantik versiyon minor degeri. */
#define UARTBINLIB_VERSION_MINOR 0u

/** @brief Kutuphane semantik versiyon patch degeri. */
#define UARTBINLIB_VERSION_PATCH 0u

/** @brief Kutuphane semantik versiyon metni. */
#define UARTBINLIB_VERSION_STRING "V1.0.0"

/** @brief Her cerceve header'inda tasinan protokol surumu. */
#define UARTBIN_VERSION 1u

/** @brief Start-of-frame dizisinin ilk byte'i. */
#define UARTBIN_SOF0 0xA5u

/** @brief Start-of-frame dizisinin ikinci byte'i. */
#define UARTBIN_SOF1 0x5Au

/** @brief Iki SOF byte'indan sonraki header boyutu, byte cinsinden. */
#define UARTBIN_HEADER_SIZE 8u

/** @brief CRC alan boyutu, byte cinsinden. */
#define UARTBIN_CRC_SIZE 2u

/** @brief Her payload etrafina eklenen byte'lar: SOF + header + CRC. */
#define UARTBIN_MAX_FRAME_OVERHEAD (2u + UARTBIN_HEADER_SIZE + UARTBIN_CRC_SIZE)

#ifndef UARTBIN_DEFAULT_RETRY_TIMEOUT_MS
/** @brief Uygulamalarin config icinde kullanabilecegi varsayilan retry timeout. */
#define UARTBIN_DEFAULT_RETRY_TIMEOUT_MS 100u
#endif

#ifndef UARTBIN_DEFAULT_RETRY_MAX_RETRIES
/** @brief Uygulamalarin config icinde kullanabilecegi varsayilan retry sayisi. */
#define UARTBIN_DEFAULT_RETRY_MAX_RETRIES 3u
#endif

/** @brief uartbin_send_request() tarafindan set edilen flag biti. */
#define UARTBIN_FLAG_REQUEST 0x01u

/** @brief uartbin_send_response() tarafindan set edilen flag biti. */
#define UARTBIN_FLAG_RESPONSE 0x02u

/** @brief uartbin_send_event() tarafindan set edilen flag biti. */
#define UARTBIN_FLAG_EVENT 0x04u

/**
 * @brief Hemen basarisiz olabilen API cagrilari icin donus durumlari.
 */
typedef enum uartbin_status {
    /** Islem basariyla tamamlandi. */
    UARTBIN_OK = 0,

    /** Gecersiz arguman; NULL context veya eksik payload pointer gibi. */
    UARTBIN_EINVAL = -1,

    /** Ayarlanan write hook TX hatasi bildirdi. */
    UARTBIN_EWRITE = -2,

    /** Daha kati TX payload limiti ekleyen uygulamalar icin ayrildi. */
    UARTBIN_EPAYLOAD_TOO_LONG = -3,

    /** Gerekli buffer'i olmayan konfigurasyonlar icin ayrildi. */
    UARTBIN_ENO_BUFFER = -4,

    /** Guvenilir request/event zaten response bekliyor. */
    UARTBIN_EBUSY = -5
} uartbin_status_t;

/**
 * @brief ::uartbin_error_fn ile iletilen parser/runtime hatalari.
 */
typedef enum uartbin_error {
    /** Cerceve desteklenmeyen protokol surumu kullandi. */
    UARTBIN_ERROR_BAD_VERSION = 1,

    /** Payload uzunlugu ayarlanan RX payload buffer'dan buyuk. */
    UARTBIN_ERROR_BAD_LENGTH,

    /** Cerceve CRC degeri alinan header ve payload ile eslesmedi. */
    UARTBIN_ERROR_CRC,

    /** Payload alindi fakat RX payload buffer ayarlanmamis. */
    UARTBIN_ERROR_RX_OVERFLOW,

    /** RX state machine rx_timeout_ms suresinden uzun sure cerceve icinde kaldi. */
    UARTBIN_ERROR_TIMEOUT,

    /** Response gelmeden guvenilir TX retry limitine ulasildi. */
    UARTBIN_ERROR_RETRY_EXHAUSTED,

    /** Guvenilir TX retry ayarlanan hook ile yazilamadi. */
    UARTBIN_ERROR_RETRY_WRITE
} uartbin_error_t;

/**
 * @brief Uygulamaya iletilen CRC kontrolunden gecmis paket.
 *
 * Payload pointer ::uartbin_config::rx_payload_buffer alanini isaret eder.
 * Ayni context uzerinde bir sonraki uartbin_feed(), uartbin_feed_at(),
 * uartbin_feed_byte(), uartbin_feed_byte_at(), uartbin_poll() veya
 * uartbin_reset() cagrimina kadar gecerlidir.
 */
typedef struct uartbin_packet {
    /** Uygulama tarafindan tanimlanan paket tipi. */
    uint8_t type;

    /** Uygulama tarafindan tanimlanan flag bitleri. */
    uint8_t flags;

    /** Uygulama sira numarasi; request/response akislari icin kullanislidir. */
    uint16_t seq;

    /** CRC dogrulamasi gecmis payload byte'larina pointer. */
    const uint8_t *payload;

    /** Payload pointer'indaki gecerli byte sayisi. */
    uint16_t payload_len;
} uartbin_packet_t;

/**
 * @brief uartbin_send() tarafindan kullanilan platform write hook'u.
 *
 * Hook, donmeden once @p len byte'in tamamini yazmali veya kopyalamalidir.
 * Bu onemlidir; cunku uartbin_send() tek cerceve icin hook'u birden fazla kez
 * cagirabilir. Non-blocking UART TX icin bu hook'u uygulama TX kuyruguna
 * kopyalayacak ve byte'lar kabul edildiyse basari dondurecek sekilde yaz.
 *
 * @param data Gonderilecek byte'lar.
 * @param len Gonderilecek byte sayisi.
 * @param user ::uartbin_config icinden gelen kullanici pointer'i.
 * @return Basarida 0, hatada sifir disi.
 */
typedef int (*uartbin_write_fn)(const uint8_t *data, size_t len, void *user);

/**
 * @brief Tam cerceve CRC dogrulamasindan gecince cagrilir.
 *
 * @param packet CRC kontrolunden gecmis paket metadata ve payload pointer'i.
 * @param user ::uartbin_config icinden gelen kullanici pointer'i.
 */
typedef void (*uartbin_packet_fn)(const uartbin_packet_t *packet, void *user);

/**
 * @brief Parser protokol hatasi veya timeout algiladiginda cagrilir.
 *
 * Hata bildirildikten sonra parser SOF arama durumuna resetlenir ve sonraki
 * byte'larda yeniden senkron olmaya calisir.
 *
 * @param error Hata kodu.
 * @param user ::uartbin_config icinden gelen kullanici pointer'i.
 */
typedef void (*uartbin_error_fn)(uartbin_error_t error, void *user);

/**
 * @brief uartbin context baslatmak icin kullanilan konfigurasyon.
 */
typedef struct uartbin_config {
    /** uartbin_send() tarafindan kullanilan TX hook. Context RX-only ise NULL olabilir. */
    uartbin_write_fn write;

    /** CRC basarili olduktan sonra cagrilan packet callback. NULL olabilir. */
    uartbin_packet_fn on_packet;

    /** Parser hatalari ve timeout icin cagrilan error callback. NULL olabilir. */
    uartbin_error_fn on_error;

    /** Hook ve callback'lere iletilen opaque uygulama pointer'i. */
    void *user;

    /** Uygulamanin sahip oldugu statik payload buffer. */
    uint8_t *rx_payload_buffer;

    /** rx_payload_buffer boyutu. Maksimum RX payload degeridir. */
    uint16_t rx_payload_capacity;

    /**
     * @brief Milisaniye cinsinden RX timeout.
     *
     * Zaman asimi yonetimini kapatmak icin 0 yap. Timestamp kaynagi uygulama
     * tarafindan uartbin_feed_at(), uartbin_feed_byte_at() ve uartbin_poll()
     * cagrilari ile verilir.
     */
    uint32_t rx_timeout_ms;

    /** Otomatik request/event retry icin kullanilan statik frame buffer. Opsiyonel. */
    uint8_t *tx_retry_buffer;

    /** tx_retry_buffer boyutu, byte cinsinden. */
    uint16_t tx_retry_capacity;

    /** Otomatik request/event gonderimleri icin retry timeout. 0 retry kapatir. */
    uint32_t tx_retry_timeout_ms;

    /** UARTBIN_ERROR_RETRY_EXHAUSTED oncesi yeniden gonderim sayisi. */
    uint8_t tx_retry_max_retries;
} uartbin_config_t;

/**
 * @brief Tek uartbin linki icin runtime durum.
 *
 * Her UART/protokol linki icin bir context ayir. Alanlar gizli allocation
 * olmadan stack/statik ayirmaya izin vermek icin public'tir; fakat uygulama
 * kodu bunlari internal detay gibi kabul etmelidir.
 */
typedef struct uartbin {
    /** Kullanici konfigurasyonunun kopyasi. */
    uartbin_config_t cfg;

    /** Internal parser durumu. */
    uint8_t state;

    /** Internal cerceve header buffer'i. */
    uint8_t header[UARTBIN_HEADER_SIZE];

    /** Su ana kadar alinan header byte sayisi. */
    uint8_t header_pos;

    /** Su ana kadar alinan payload byte sayisi. */
    uint16_t payload_pos;

    /** Gecerli cerceve header'indan cozulmus payload uzunlugu. */
    uint16_t payload_len;

    /** Internal alinan CRC byte'lari. */
    uint8_t crc_bytes[UARTBIN_CRC_SIZE];

    /** Su ana kadar alinan CRC byte sayisi. */
    uint8_t crc_pos;

    /** Header ve payload uzerinde akan CRC. */
    uint16_t crc;

    /** Son kabul edilen RX byte/block timestamp degeri. */
    uint32_t last_rx_time_ms;

    /** Otomatik send yardimcilari tarafindan kullanilan sira sayaci. */
    uint16_t tx_seq;

    /** Guvenilir request/event response beklerken sifir disidir. */
    uint8_t retry_active;

    /** Retry zamanlayicisi uartbin_poll() ile arm edildikten sonra sifir disidir. */
    uint8_t retry_timer_started;

    /** Pending cerceve icin denenmis yeniden gonderim sayisi. */
    uint8_t retry_count;

    /** Pending guvenilir cercevenin sira numarasi. */
    uint16_t retry_seq;

    /** cfg.tx_retry_buffer icinde saklanan byte sayisi. */
    uint16_t retry_frame_len;

    /** Pending cercevenin ne zaman tekrar gonderilecegini belirleyen timestamp. */
    uint32_t retry_last_tx_time_ms;
} uartbin_t;

/**
 * @brief uartbin context baslat.
 *
 * @param ctx Baslatilacak context. Link kullanildigi surece gecerli kalmalidir.
 * @param config @p ctx icine kopyalanacak konfigurasyon.
 */
void uartbin_init(uartbin_t *ctx, const uartbin_config_t *config);

/**
 * @brief Yalnizca RX parser durumunu resetle.
 *
 * Platform UART hatalari, manuel abort veya link recovery olaylarindan sonra
 * kullan. Konfigurasyon ve callback'ler korunur.
 *
 * @param ctx Resetlenecek context.
 */
void uartbin_reset(uartbin_t *ctx);

/**
 * @brief Pending otomatik retry durumunu iptal et.
 *
 * RX parsing resetlenmez ve bir sey gonderilmez. Uygulama response gelmeden
 * bir istegi bilincli olarak terk etmek istediginde kullanislidir.
 *
 * @param ctx Baslatilmis context.
 */
void uartbin_cancel_retry(uartbin_t *ctx);

/**
 * @brief Tek cerceveli paketi olustur ve gonder.
 *
 * @param ctx Gecerli write hook'a sahip baslatilmis context.
 * @param type Uygulama tarafindan tanimlanan paket tipi.
 * @param flags Uygulama tarafindan tanimlanan flag'ler.
 * @param seq Uygulama tarafindan tanimlanan sira numarasi.
 * @param payload Payload byte'lari; @p payload_len 0 ise NULL olabilir.
 * @param payload_len Payload byte sayisi.
 * @return Basarida ::UARTBIN_OK, aksi halde hata durumu.
 */
uartbin_status_t uartbin_send(uartbin_t *ctx,
                              uint8_t type,
                              uint8_t flags,
                              uint16_t seq,
                              const uint8_t *payload,
                              uint16_t payload_len);

/**
 * @brief Bu context icin sonraki sifir olmayan TX sira numarasini dondur.
 *
 * Bu daha cok otomatik uretilecek request'i gondermeden once takip etmek
 * isteyen uygulamalar icin kullanislidir. Request/event yardimcilari bunu
 * internal olarak cagirir; cogu uygulamanin dogrudan kullanmasi gerekmez.
 *
 * @param ctx Baslatilmis context.
 * @return Sonraki sira numarasi; @p ctx NULL ise 0.
 */
uint16_t uartbin_next_seq(uartbin_t *ctx);

/**
 * @brief Otomatik uretilen sira numarasi ile yeni request gonder.
 *
 * Yardimci @p flags icine ::UARTBIN_FLAG_REQUEST OR eder ve context'in internal
 * sira sayacini kullanir. Bu paketi cevaplamak icin response yardimcisini kullan.
 *
 * @param ctx Gecerli write hook'a sahip baslatilmis context.
 * @param type Uygulama tarafindan tanimlanan request tipi.
 * @param flags Request flag'i ile birlestirilecek uygulama flag'leri.
 * @param payload Payload byte'lari; @p payload_len 0 ise NULL olabilir.
 * @param payload_len Payload byte sayisi.
 * @return Basarida ::UARTBIN_OK, aksi halde hata durumu.
 */
uartbin_status_t uartbin_send_request(uartbin_t *ctx,
                                      uint8_t type,
                                      uint8_t flags,
                                      const uint8_t *payload,
                                      uint16_t payload_len);

/**
 * @brief Alinan paketteki sira numarasini kullanarak response gonder.
 *
 * Yardimci @p flags icine ::UARTBIN_FLAG_RESPONSE OR eder ve @p request->seq
 * degerini geri yazar. Context sira sayacini ilerletmez.
 *
 * @param ctx Gecerli write hook'a sahip baslatilmis context.
 * @param request Cevaplanan paket.
 * @param type Uygulama tarafindan tanimlanan response tipi.
 * @param flags Response flag'i ile birlestirilecek uygulama flag'leri.
 * @param payload Payload byte'lari; @p payload_len 0 ise NULL olabilir.
 * @param payload_len Payload byte sayisi.
 * @return Basarida ::UARTBIN_OK, aksi halde hata durumu.
 */
uartbin_status_t uartbin_send_response(uartbin_t *ctx,
                                       const uartbin_packet_t *request,
                                       uint8_t type,
                                       uint8_t flags,
                                       const uint8_t *payload,
                                       uint16_t payload_len);

/**
 * @brief Otomatik uretilen sira numarasi ile unsolicited event gonder.
 *
 * Yardimci @p flags icine ::UARTBIN_FLAG_EVENT OR eder ve context'in internal
 * sira sayacini kullanir. Peer event'i ACK'liyorsa response icinde bu paketin
 * sira numarasini geri yazmalidir.
 *
 * @param ctx Gecerli write hook'a sahip baslatilmis context.
 * @param type Uygulama tarafindan tanimlanan event tipi.
 * @param flags Event flag'i ile birlestirilecek uygulama flag'leri.
 * @param payload Payload byte'lari; @p payload_len 0 ise NULL olabilir.
 * @param payload_len Payload byte sayisi.
 * @return Basarida ::UARTBIN_OK, aksi halde hata durumu.
 */
uartbin_status_t uartbin_send_event(uartbin_t *ctx,
                                    uint8_t type,
                                    uint8_t flags,
                                    const uint8_t *payload,
                                    uint16_t payload_len);

/**
 * @brief Alinan byte'lari timeout timestamp guncellemesi olmadan besle.
 *
 * Zaman asimi yonetimi acikken uartbin_feed_at() tercih edilmelidir.
 *
 * @param ctx Baslatilmis context.
 * @param data Alinan byte buffer'i.
 * @param len Alinan byte sayisi.
 */
void uartbin_feed(uartbin_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Tek alinan byte'i timeout timestamp guncellemesi olmadan besle.
 *
 * Zaman asimi yonetimi acikken uartbin_feed_byte_at() tercih edilmelidir.
 *
 * @param ctx Baslatilmis context.
 * @param byte Alinan byte.
 */
void uartbin_feed_byte(uartbin_t *ctx, uint8_t byte);

/**
 * @brief Alinan byte'lari gecerli uygulama timestamp degeri ile besle.
 *
 * @param ctx Baslatilmis context.
 * @param data Alinan byte buffer'i.
 * @param len Alinan byte sayisi.
 * @param now_ms HAL_GetTick() gibi monotonic milisaniye timestamp.
 */
void uartbin_feed_at(uartbin_t *ctx, const uint8_t *data, size_t len, uint32_t now_ms);

/**
 * @brief Tek alinan byte'i gecerli uygulama timestamp degeri ile besle.
 *
 * @param ctx Baslatilmis context.
 * @param byte Alinan byte.
 * @param now_ms HAL_GetTick() gibi monotonic milisaniye timestamp.
 */
void uartbin_feed_byte_at(uartbin_t *ctx, uint8_t byte, uint32_t now_ms);

/**
 * @brief RX timeout ve opsiyonel TX retry zamanlamasini servis et.
 *
 * ::uartbin_config::rx_timeout_ms veya ::uartbin_config::tx_retry_timeout_ms
 * sifir disi oldugunda bunu main loop veya scheduler icinden periyodik cagir.
 * Feed fonksiyonlari yalnizca RX timeout servis eder; otomatik TX retry bu
 * acik poll cagrisi ile surulur.
 *
 * @param ctx Baslatilmis context.
 * @param now_ms Monotonic milisaniye timestamp.
 */
void uartbin_poll(uartbin_t *ctx, uint32_t now_ms);

/**
 * @brief CRC-16/CCITT-FALSE degerini guncelle.
 *
 * Polynomial: 0x1021. Kutuphane cerceveler icin 0xFFFF seed kullanir.
 *
 * @param data Giris byte'lari. Yalnizca @p len 0 iken NULL olabilir.
 * @param len Giris byte sayisi.
 * @param seed Baslangic/akan CRC degeri.
 * @return Guncellenmis CRC degeri.
 */
uint16_t uartbin_crc16_ccitt(const uint8_t *data, size_t len, uint16_t seed);

#ifdef __cplusplus
}
#endif

#endif
