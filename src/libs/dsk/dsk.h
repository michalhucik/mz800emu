/**
 * @file   dsk.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Nízkoúrovňové API pro práci s diskovými obrazy ve formátu Extended CPC DSK.
 *
 * Knihovna poskytuje čtení/zápis sektorů, výpočty offsetů, parsování
 * hlaviček a práci s geometrií disku. Veškeré I/O operace abstrahuje
 * přes generic_driver.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef DSK_H
#define DSK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "libs/generic_driver/generic_driver.h"


    /** @brief Kódování velikosti sektoru (pole ssize v hlavičce stopy). */
    typedef enum en_DSK_SECTOR_SIZE {
        DSK_SECTOR_SIZE_128 = 0,        /**< 128 bajtů */
        DSK_SECTOR_SIZE_256,            /**< 256 bajtů */
        DSK_SECTOR_SIZE_512,            /**< 512 bajtů */
        DSK_SECTOR_SIZE_1024,           /**< 1024 bajtů */
        DSK_SECTOR_SIZE_INVALID = 0xff  /**< Neplatná velikost */
    } en_DSK_SECTOR_SIZE;


    /** @brief Maximální počet absolutních stop v DSK obrazu (obě strany dohromady). */
#define DSK_MAX_TOTAL_TRACKS        204

    /** @brief Maximální počet sektorů na jedné stopě. */
#define DSK_MAX_SECTORS             29

    /** @brief Maximální velikost jednoho sektoru v bajtech. */
#define DSK_MAX_SECTOR_SIZE         1024

    /** @brief Délka pole file_info v hlavičce DSK (bez terminátoru). */
#define DSK_FILEINFO_FIELD_LENGTH   34
    /** @brief Výchozí obsah pole file_info (34 znaků, bez 0x00). */
#define DSK_DEFAULT_FILEINFO        "EXTENDED CPC DSK File\r\nDisk-Info\r\n"

    /** @brief Délka pole creator v hlavičce DSK (bez terminátoru). */
#define DSK_CREATOR_FIELD_LENGTH    14
    /** @brief Výchozí obsah pole creator (14 znaků, bez 0x00). */
#define DSK_DEFAULT_CREATOR         "DSKLib v1.1\x0\x0\x0"

    /** @brief Délka pole track_info v hlavičce stopy (bez terminátoru). */
#define DSK_TRACKINFO_FIELD_LENGTH  12
    /** @brief Výchozí obsah pole track_info (12 znaků, bez 0x00). */
#define DSK_DEFAULT_TRACKINFO       "Track-Info\r\n"

    /** @brief Výchozí hodnota GAP#3 v hlavičce stopy. */
#define DSK_DEFAULT_GAP             0x4e
    /** @brief Výchozí filler byte pro nově vytvářené sektory. */
#define DSK_DEFAULT_FILLER          0xe5


    /** @brief Hlavička DSK souboru — 256 bajtů na začátku image. */
    typedef struct st_DSK_HEADER {
        uint8_t file_info [ DSK_FILEINFO_FIELD_LENGTH ];    /**< Identifikační řetězec formátu */
        uint8_t creator [ DSK_CREATOR_FIELD_LENGTH ];       /**< Identifikace tvůrce obrazu */
        uint8_t tracks;                                     /**< Počet stop per strana */
        uint8_t sides;                                      /**< Počet stran (1 nebo 2) */
        uint8_t unused [ 2 ];                               /**< Rezervované bajty */
        uint8_t tsize [ DSK_MAX_TOTAL_TRACKS ];             /**< Velikost každé stopy (×256 B); tsize=0 = stopa neexistuje */
    } st_DSK_HEADER; /* 256 B */


    /**
     * @brief Informace o jednom sektoru v hlavičce stopy - 8 bajtů.
     *
     * Odpovídá položce Sector Information List formátu Extended CPC DSK.
     * Pole data_len je v tomto formátu povinné ("actual data length in
     * bytes"); nevyužité (nulové) je jen ve starém formátu "MV - CPC",
     * který knihovna nepodporuje. Zdroj: CPCWiki,
     * Format:DSK_disk_image_file_format.
     *
     * @invariant U obrazů vytvořených touto knihovnou platí
     *            data_len == dsk_decode_sector_size ( ssize ).
     */
    typedef struct st_DSK_SECTOR_INFO {
        uint8_t track;          /**< Číslo stopy */
        uint8_t side;           /**< Strana (0/1) */
        uint8_t sector;         /**< ID sektoru */
        uint8_t ssize;          /**< Kódovaná velikost sektoru (en_DSK_SECTOR_SIZE) */
        uint8_t fdc_sts1;       /**< FDC stavový registr 1 */
        uint8_t fdc_sts2;       /**< FDC stavový registr 2 */
        uint8_t data_len [ 2 ]; /**< Skutečná délka uložených dat sektoru v bajtech, little-endian
                                     (přístup přes dsk_sector_info_get/set_data_length) */
    } st_DSK_SECTOR_INFO; /* 8 B */


    /**
     * @brief Vrátí délku dat sektoru uloženou v Sector Info.
     *
     * @param sinfo Sector Info (nesmí být NULL).
     * @return Délka dat v bajtech; 0 znamená, že pole není vyplněné.
     */
    static inline uint16_t dsk_sector_info_get_data_length ( const st_DSK_SECTOR_INFO *sinfo ) {
        return (uint16_t) ( sinfo->data_len[0] | ( (uint16_t) sinfo->data_len[1] << 8 ) );
    }


    /**
     * @brief Uloží délku dat sektoru do Sector Info (little-endian).
     *
     * Mění jen strukturu v paměti, do obrazu nezapisuje.
     *
     * @param sinfo Sector Info (nesmí být NULL).
     * @param length Délka dat v bajtech.
     */
    static inline void dsk_sector_info_set_data_length ( st_DSK_SECTOR_INFO *sinfo, uint16_t length ) {
        sinfo->data_len[0] = (uint8_t) ( length & 0xff );
        sinfo->data_len[1] = (uint8_t) ( length >> 8 );
    }


    /** @brief Hlavička stopy — 256 bajtů. */
    typedef struct st_DSK_TRACK_INFO {
        uint8_t track_info [ DSK_TRACKINFO_FIELD_LENGTH ];  /**< Identifikační řetězec "Track-Info\r\n" */
        uint8_t unused1 [ 4 ];                              /**< Rezervované bajty */
        uint8_t track;                                      /**< Číslo stopy */
        uint8_t side;                                       /**< Strana (0/1) */
        uint8_t unused2 [ 2 ];                              /**< Rezervované bajty */
        uint8_t ssize;                                      /**< Kódovaná velikost sektoru (en_DSK_SECTOR_SIZE) */
        uint8_t sectors;                                    /**< Počet sektorů na stopě */
        uint8_t gap;                                        /**< Délka GAP#3 */
        uint8_t filler;                                     /**< Filler byte */
        st_DSK_SECTOR_INFO sinfo [ DSK_MAX_SECTORS ];       /**< Pole informací o sektorech */
    } st_DSK_TRACK_INFO; /* 256 B */


    /**
     * @brief Dekóduje kódovanou velikost sektoru na bajty.
     *
     * Převod: ssize=0 -> 128, ssize=1 -> 256, ssize=2 -> 512, ssize=3 -> 1024.
     *
     * @param ssize Kódovaná velikost sektoru.
     * @return Velikost sektoru v bajtech.
     */
    static inline uint16_t dsk_decode_sector_size ( en_DSK_SECTOR_SIZE ssize ) {
        /* Clamp na platný rozsah 0..3 (audit M-1): vstup pochází
         * z dat na disku, `ssize > 3` by dalo nesprávný výsledek a
         * `ssize >= 24` undefined behavior (posun mimo šířku typu). */
        uint8_t clamped = ( (uint8_t) ssize > 3 ) ? 3 : (uint8_t) ssize;
        uint16_t retval = 0x80;
        return ( retval << clamped );
    }


    /**
     * @brief Dekóduje hodnotu tsize z hlavičky na bajty.
     *
     * Hodnota tsize se ukládá jako počet 256B bloků.
     *
     * @param tsize Kódovaná velikost stopy (násobek 256 B).
     * @return Velikost stopy v bajtech.
     */
    static inline uint16_t dsk_decode_track_size ( uint8_t tsize ) {
        return ( (uint16_t) tsize << 8 );
    }


    /**
     * @brief Zakóduje velikost sektoru v bajtech na en_DSK_SECTOR_SIZE.
     *
     * @param sector_size Velikost sektoru v bajtech (128, 256, 512 nebo 1024).
     * @return Kódovaná velikost, nebo DSK_SECTOR_SIZE_INVALID pro neplatné hodnoty.
     */
    static inline en_DSK_SECTOR_SIZE dsk_encode_sector_size ( uint16_t sector_size ) {
        switch ( sector_size ) {
            case 1024:
                return DSK_SECTOR_SIZE_1024;
            case 512:
                return DSK_SECTOR_SIZE_512;
            case 256:
                return DSK_SECTOR_SIZE_256;
            case 128:
                return DSK_SECTOR_SIZE_128;
            default:
                break;
        };
        return DSK_SECTOR_SIZE_INVALID;
    }


    /**
     * @brief Zakóduje parametry stopy na tsize hodnotu pro hlavičku DSK.
     *
     * Výsledná velikost stopy se zaokrouhluje nahoru na násobek 256 B.
     * To je nezbytné pro případ 128B sektorů s lichým počtem, kde
     * surová velikost (header + N*128) není dělitelná 256. Bez tohoto
     * zaokrouhlení by tsize byl o 1 menší než potřeba, což způsobí
     * posunutí offsetů všech následujících stop a ztrátu dat.
     *
     * @param sectors Počet sektorů na stopě.
     * @param ssize Kódovaná velikost sektoru.
     * @return Hodnota tsize (velikost stopy jako násobek 256 B, zaokrouhleno nahoru).
     */
    static inline uint8_t dsk_encode_track_size ( uint8_t sectors, en_DSK_SECTOR_SIZE ssize ) {
        if ( sectors == 0 ) return 0;
        uint16_t retval = sizeof ( st_DSK_TRACK_INFO ) + ( dsk_decode_sector_size ( ssize ) * sectors );
        /* Zaokrouhlení nahoru na násobek 256 B: (retval + 255) / 256 */
        return (uint8_t) ( ( retval + 0xff ) >> 8 );
    }


    /** @brief Chybové kódy DSK knihovny — rozšiřují HANDLER_ERROR_USER. */
    typedef enum en_DSK_ERROR {
        DSK_ERROR_NONE = HANDLER_ERROR_NONE,                    /**< Žádná chyba */
        DSK_ERROR_NOT_READY = HANDLER_ERROR_NOT_READY,          /**< Handler není připravený */
        DSK_ERROR_WRITE_PROTECTED = HANDLER_ERROR_WRITE_PROTECTED, /**< Obraz je chráněný proti zápisu */
        DSK_ERROR_TRACK_NOT_FOUND = HANDLER_ERROR_USER,         /**< Stopa neexistuje */
        DSK_ERROR_SECTOR_NOT_FOUND,                             /**< Sektor neexistuje */
        DSK_ERROR_DOUBLE_SIDED,                                 /**< Pokus o operaci na oboustranném disku s lichým počtem stop */
        DSK_ERROR_NO_TRACKS,                                    /**< Pokus o vytvoření disku s nulovým počtem stop */
        DSK_ERROR_INVALID_PARAM,                                /**< Neplatný parametr (NULL, mimo rozsah) */
        DSK_ERROR_UNKNOWN                                       /**< Neznámá chyba */
    } en_DSK_ERROR;


    /** @brief Stav handleru DSK. */
    typedef enum en_DSK_STATUS {
        DSK_STATUS_NOT_READY = HANDLER_STATUS_NOT_READY,    /**< Handler není připravený */
        DSK_STATUS_READY = HANDLER_STATUS_READY,            /**< Handler je připravený */
        DSK_STATUS_READ_ONLY = HANDLER_STATUS_READ_ONLY     /**< Handler je pouze pro čtení */
    } en_DSK_STATUS;


    /** @brief Zkrácená informace o DSK obrazu (z hlavičky souboru).
     *
     * Pole `header_tracks` obsahuje **raw** hodnotu `tracks` z hlavičky,
     * `tracks` je případně oříznuto tak, aby `tracks * sides <= DSK_MAX_TOTAL_TRACKS`.
     * Pokud je `header_tracks > tracks`, signalizuje to situaci, kdy hlavička
     * deklaruje více stop než kolik umí driver zpracovat (audit H-11).
     * Vyšší vrstva (dsk_tools_diagnose/repair) tento rozdíl detekuje a opraví
     * přepsáním pole `tracks` v DSK hlavičce.
     */
    typedef struct st_DSK_SHORT_IMAGE_INFO {
        uint8_t tracks;                         /**< Počet stop per strana (případně oříznuto na DSK_MAX_TOTAL_TRACKS/sides) */
        uint8_t sides;                          /**< Počet stran (1 nebo 2) */
        uint8_t header_tracks;                  /**< Raw hodnota `tracks` z DSK hlavičky, bez ořezu (audit H-11) */
        uint8_t tsize [ DSK_MAX_TOTAL_TRACKS ]; /**< Velikost každé stopy (×256 B) */
    } st_DSK_SHORT_IMAGE_INFO;


    /** @brief Zkrácená informace o jedné stopě. */
    typedef struct st_DSK_SHORT_TRACK_INFO {
        uint8_t track;                              /**< Číslo stopy */
        uint8_t side;                               /**< Strana (0/1) */
        uint8_t ssize;                              /**< Kódovaná velikost sektoru společná pro stopu */
        uint8_t sectors;                            /**< Počet sektorů na stopě */
        uint8_t sinfo [ DSK_MAX_SECTORS ];          /**< Pole ID sektorů */
        uint8_t sector_sizes [ DSK_MAX_SECTORS ];   /**< Kódovaná velikost per-sektor (z hlavičky) */
    } st_DSK_SHORT_TRACK_INFO;


    /** @brief Směr operace pro dsk_rw_sector. */
    typedef enum en_DSK_RWOP {
        DSK_RWOP_READ = 0,     /**< Operace čtení */
        DSK_RWOP_WRITE,        /**< Operace zápisu */
    } en_DSK_RWOP;


    /* ─── Chybové zprávy ─── */

    /**
     * @brief Vrátí textový popis poslední chyby.
     * @param h Handler DSK operace.
     * @param d Driver pro I/O operace.
     * @return Ukazatel na statický řetězec s popisem chyby.
     */
    extern const char* dsk_error_message ( st_HANDLER *h, st_DRIVER *d );


    /* ─── Výpočty offsetů ─── */

    /**
     * @brief Vypočítá absolutní offset stopy v DSK souboru.
     * @param abstrack Absolutní číslo stopy (0 .. total_tracks-1).
     * @param tsizes Pole velikostí stop z hlavičky DSK (min DSK_MAX_TOTAL_TRACKS prvků). Může být NULL.
     * @return Offset v bajtech od začátku souboru.
     */
    extern uint32_t dsk_compute_track_offset ( uint8_t abstrack, const uint8_t *tsizes );

    /**
     * @brief Vypočítá offset sektoru v rámci stopy.
     * @param sector ID sektoru.
     * @param tinfo Informace o stopě. Může být NULL (vrátí -1).
     * @return Offset v rámci stopy, nebo -1 pokud sektor nenalezen.
     */
    extern int32_t dsk_compute_sector_offset ( uint8_t sector, const st_DSK_SHORT_TRACK_INFO *tinfo );


    /* ─── Čtení metadat ─── */

    /**
     * @brief Načte základní informace o DSK obrazu (tracks, sides, tsize pole).
     * @param h Handler.
     * @param short_image_info Výstupní struktura pro informace o obrazu.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_read_short_image_info ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info );

    /**
     * @brief Přečte informaci o stopě na zadaném offsetu v souboru.
     * @param h Handler.
     * @param track_offset Offset stopy v souboru.
     * @param short_track_info Výstupní struktura pro informace o stopě.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_read_short_track_info_on_offset ( st_HANDLER *h, uint32_t track_offset, st_DSK_SHORT_TRACK_INFO *short_track_info );

    /**
     * @brief Přečte informaci o stopě podle absolutního čísla.
     * @param h Handler.
     * @param short_image_info Informace o obrazu (NULL = načte se automaticky).
     * @param abstrack Absolutní číslo stopy.
     * @param short_track_info Výstupní struktura pro informace o stopě.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_read_short_track_info ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, uint8_t abstrack, st_DSK_SHORT_TRACK_INFO *short_track_info );

    /**
     * @brief Pro požadovaný sektor na konkrétní stopě získá absolutní offset a velikost.
     * @param h Handler.
     * @param short_image_info Informace o obrazu (NULL = načte se automaticky).
     * @param short_track_info Informace o stopě (NULL = načte se automaticky).
     * @param abstrack Absolutní stopa.
     * @param sector ID sektoru.
     * @param sector_offset Výstup: absolutní pozice sektoru v souboru.
     * @param ssize_bytes Výstup: velikost sektoru v bajtech.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_read_short_sector_info ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, st_DSK_SHORT_TRACK_INFO *short_track_info, uint8_t abstrack, uint8_t sector, uint32_t *sector_offset, uint16_t *ssize_bytes );


    /* ─── Čtení/zápis dat ─── */

    /**
     * @brief Univerzální čtení/zápis sektoru s volitelným cachovaným info.
     * @param h Handler.
     * @param rwop Typ operace (DSK_RWOP_READ / DSK_RWOP_WRITE).
     * @param short_image_info Informace o obrazu (NULL = načte se automaticky).
     * @param short_track_info Informace o stopě (NULL = načte se automaticky).
     * @param abstrack Absolutní stopa.
     * @param sector ID sektoru.
     * @param buffer Datový buffer (musí být dostatečně velký pro velikost sektoru).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_rw_sector ( st_HANDLER *h, en_DSK_RWOP rwop, st_DSK_SHORT_IMAGE_INFO *short_image_info, st_DSK_SHORT_TRACK_INFO *short_track_info, uint8_t abstrack, uint8_t sector, void *buffer );

    /**
     * @brief Přečte data z libovolného offsetu v DSK souboru.
     * @param h Handler.
     * @param offset Offset v souboru.
     * @param buffer Cílový buffer.
     * @param buffer_size Počet bajtů ke čtení.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_read_on_offset ( st_HANDLER *h, uint32_t offset, void *buffer, uint16_t buffer_size );

    /**
     * @brief Zapíše data na libovolný offset v DSK souboru.
     * @param h Handler.
     * @param offset Offset v souboru.
     * @param buffer Zdrojový buffer.
     * @param buffer_size Počet bajtů k zápisu.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_write_on_offset ( st_HANDLER *h, uint32_t offset, const void *buffer, uint16_t buffer_size );

    /**
     * @brief Přečte celý sektor z DSK obrazu.
     * @param h Handler.
     * @param abstrack Absolutní stopa.
     * @param sector ID sektoru.
     * @param buffer Cílový buffer (musí být dostatečně velký pro velikost sektoru).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_read_sector ( st_HANDLER *h, uint8_t abstrack, uint8_t sector, void *buffer );

    /**
     * @brief Zapíše celý sektor do DSK obrazu.
     * @param h Handler.
     * @param abstrack Absolutní stopa.
     * @param sector ID sektoru.
     * @param buffer Zdrojový buffer s daty sektoru.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_write_sector ( st_HANDLER *h, uint8_t abstrack, uint8_t sector, void *buffer );


    /* ─── Geometrie disku ─── */

    /** @brief Souhrnná geometrie DSK obrazu. */
    typedef struct st_DSK_GEOMETRY {
        uint8_t tracks;             /**< Počet stop (per strana) */
        uint8_t sides;              /**< Počet stran (1 nebo 2) */
        uint8_t total_tracks;       /**< Celkový počet absolutních stop */
        uint32_t total_data_bytes;  /**< Celková velikost dat (bez hlaviček) */
        uint32_t image_size;        /**< Celková velikost image v bajtech */
    } st_DSK_GEOMETRY;

    /**
     * @brief Získá souhrnnou geometrii DSK obrazu.
     * @param h Handler.
     * @param geom Výstupní struktura pro geometrii.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_get_geometry ( st_HANDLER *h, st_DSK_GEOMETRY *geom );


    /* ─── Verze knihovny ─── */

    /** @brief Verze knihovny dsk. */
#define DSK_VERSION "2.1.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny dsk.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* dsk_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* DSK_H */
