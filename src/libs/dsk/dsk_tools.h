/**
 * @file   dsk_tools.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 3.0.0
 * @brief  Vyšší nástroje pro práci s Extended CPC DSK obrazy.
 *
 * Poskytuje API pro vytváření, modifikace, validaci, diagnostiku,
 * inspekci, editaci, identifikaci formátu a iteraci přes stopy/sektory
 * DSK diskových obrazů.
 *
 * @par Changelog:
 * - 2026-04-15: Diagnostické API (diagnose/repair), inspekce a editace hlaviček.
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


#ifndef DSK_TOOLS_H
#define DSK_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "dsk.h"


    /* ─── Logování ─── */

    /**
     * @brief Callback pro logování.
     *
     * Knihovna je ve výchozím stavu tichá — loguje pouze pokud je nastaven
     * callback přes dsk_tools_set_log_cb().
     *
     * @param level Úroveň logování: 0 = info, 1 = warning, 2 = error.
     * @param msg Formátovaná zpráva.
     * @param user_data Uživatelská data předaná při registraci callbacku.
     */
    typedef void (*dsk_tools_log_cb_t)( int level, const char *msg, void *user_data );

    /** @brief Úroveň logování: informační zpráva. */
#define DSK_LOG_INFO    0
    /** @brief Úroveň logování: varování. */
#define DSK_LOG_WARNING 1
    /** @brief Úroveň logování: chyba. */
#define DSK_LOG_ERROR   2

    /**
     * @brief Nastaví logovací callback.
     *
     * NULL = žádné logování (výchozí). user_data se předá callbacku při každém volání.
     *
     * @param cb Logovací callback, nebo NULL pro vypnutí logování.
     * @param user_data Uživatelská data předávaná callbacku.
     */
    extern void dsk_tools_set_log_cb ( dsk_tools_log_cb_t cb, void *user_data );


    /* ─── Popis geometrie pro vytváření obrazů ─── */

    /** @brief Typ řazení sektorů na stopě. */
    typedef enum en_DSK_SECTOR_ORDER_TYPE {
        DSK_SEC_ORDER_CUSTOM = 0,           /**< Uživatelská mapa (vyžaduje přiložený sector_map) */
        DSK_SEC_ORDER_NORMAL = 1,           /**< Sekvenční řazení: 1, 2, 3, ... */
        DSK_SEC_ORDER_INTERLACED_LEC = 2,   /**< 1x prokládané řazení */
        DSK_SEC_ORDER_INTERLACED_LEC_HD = 3 /**< 2x prokládané řazení */
    } en_DSK_SECTOR_ORDER_TYPE;


    /** @brief Jedno pravidlo popisující geometrii stop v rozsahu. */
    typedef struct st_DSK_DESCRIPTION_RULE {
        uint8_t absolute_track;             /**< Absolutní stopa od které pravidlo platí */
        uint8_t sectors;                    /**< Počet sektorů na stopě */
        en_DSK_SECTOR_SIZE ssize;           /**< Kódovaná velikost sektoru */
        en_DSK_SECTOR_ORDER_TYPE sector_order; /**< Typ řazení sektorů */
        uint8_t *sector_map;                /**< Mapa sektorů (jen pro CUSTOM, jinak NULL) */
        uint8_t filler;                     /**< Filler byte pro nové sektory */
    } st_DSK_DESCRIPTION_RULE;


    /**
     * @brief Popis geometrie disku pro vytvoření obrazu.
     *
     * Alokuje se přes malloc(dsk_tools_compute_description_size(n)),
     * pravidla jsou za strukturou jako C99 flexible array member.
     */
    typedef struct st_DSK_DESCRIPTION {
        uint16_t count_rules;               /**< Počet pravidel */
        uint8_t tracks;                     /**< Počet stop (per strana) */
        uint8_t sides;                      /**< Počet stran (1 nebo 2) */
#ifdef __cplusplus
        st_DSK_DESCRIPTION_RULE rules[1];  /**< Pravidla geometrie (C++ compat: [1] místo []) */
#else
        st_DSK_DESCRIPTION_RULE rules[];   /**< Pravidla geometrie (C99 flexible array member) */
#endif
    } st_DSK_DESCRIPTION;


    /**
     * @brief Výpočet velikosti alokace pro st_DSK_DESCRIPTION s daným počtem pravidel.
     * @param rules Počet pravidel.
     * @return Velikost v bajtech pro malloc().
     */
    static inline size_t dsk_tools_compute_description_size ( uint8_t rules ) {
        return ( sizeof ( st_DSK_DESCRIPTION ) + sizeof ( st_DSK_DESCRIPTION_RULE ) * rules );
    }


    /* ─── Vytváření a modifikace obrazů ─── */

    /**
     * @brief Přiřadí jedno pravidlo do popisu geometrie disku.
     *
     * Pravidla musí být přiřazována vzestupně podle abs_track.
     *
     * @param dskdesc Odkaz na existující strukturu popisu. Může být NULL (NOP).
     * @param rule Pořadové číslo záznamu (index do rules[]).
     * @param abs_track Absolutní stopa od které pravidlo platí.
     * @param sectors Počet sektorů na stopě.
     * @param ssize Kódovaná velikost sektoru.
     * @param sector_order Typ řazení sektorů.
     * @param sector_map Mapa sektorů (jen pro CUSTOM, jinak NULL).
     * @param default_value Filler byte pro nové sektory.
     */
    extern void dsk_tools_assign_description ( st_DSK_DESCRIPTION *dskdesc, uint8_t rule, uint8_t abs_track, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, en_DSK_SECTOR_ORDER_TYPE sector_order, uint8_t *sector_map, uint8_t default_value );

    /**
     * @brief Vygeneruje mapu ID sektorů podle typu řazení.
     *
     * CUSTOM se automaticky převede na NORMAL.
     *
     * @param sectors Počet sektorů.
     * @param sector_order Typ řazení sektorů.
     * @param sector_map Výstupní pole o velikosti sectors.
     */
    extern void dsk_tools_make_sector_map ( uint8_t sectors, en_DSK_SECTOR_ORDER_TYPE sector_order, uint8_t *sector_map );

    /**
     * @brief Vytvoří kompletní DSK obraz podle popisu geometrie.
     * @param h Handler.
     * @param desc Popis geometrie disku.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_create_image ( st_HANDLER *h, st_DSK_DESCRIPTION *desc );

    /**
     * @brief Vytvoří DSK hlavičku podle popisu geometrie.
     * @param h Handler.
     * @param desc Popis geometrie disku.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_create_image_header ( st_HANDLER *h, st_DSK_DESCRIPTION *desc );

    /**
     * @brief Vytvoří postupně všechny stopy podle popisu geometrie.
     * @param h Handler.
     * @param desc Popis geometrie.
     * @param first_abs_track První absolutní stopa.
     * @param dsk_offset Offset v souboru (0 = sizeof(st_DSK_HEADER)).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_create_image_tracks ( st_HANDLER *h, st_DSK_DESCRIPTION *desc, uint8_t first_abs_track, uint32_t dsk_offset );

    /**
     * @brief Vytvoří jednu kompletní DSK stopu (hlavičku + sektory).
     * @param h Handler.
     * @param dsk_offset Offset v souboru.
     * @param track Číslo stopy.
     * @param side Strana (0/1).
     * @param sectors Počet sektorů.
     * @param ssize Kódovaná velikost sektoru.
     * @param sector_map Seznam ID sektorů.
     * @param default_value Filler byte.
     * @param track_total_bytes Výstup: celková velikost zapsané stopy.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_create_track ( st_HANDLER *h, uint32_t dsk_offset, uint8_t track, uint8_t side, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t *sector_map, uint8_t default_value, uint32_t *track_total_bytes );

    /**
     * @brief Vytvoří hlavičku pro jednu stopu.
     *
     * Pole `filler` a `gap` jsou metadata DSK formátu, která popisují jak byla
     * stopa fyzicky zformátována. Neovlivňují reálný obsah sektorů (ten se
     * zapisuje přes dsk_tools_create_track_sectors), ale některé nástroje
     * (např. mzdsk-dsk tracks, externí reformátovací utility) je čtou.
     *
     * Každému sektoru se do Sector Info zapíše i skutečná délka dat
     * (dsk_decode_sector_size ( ssize )), kterou formát Extended CPC DSK
     * vyžaduje. Verze < 3.4.0 ji nechávaly nulovou (bug 2026-11-10-003).
     *
     * @post Pro i < sectors platí
     *       dsk_sector_info_get_data_length ( &sinfo[i] ) == dsk_decode_sector_size ( ssize ).
     *
     * @param h Handler.
     * @param dsk_offset Offset v souboru.
     * @param track Číslo stopy.
     * @param side Strana (0/1).
     * @param sectors Počet sektorů.
     * @param ssize Kódovaná velikost sektoru.
     * @param sector_map Seznam ID jednotlivých sektorů.
     * @param filler Filler byte (hodnota, kterou jsou sektory formátovány).
     * @param gap Délka GAP#3 (typicky DSK_DEFAULT_GAP = 0x4E).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_create_track_header ( st_HANDLER *h, uint32_t dsk_offset, uint8_t track, uint8_t side, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t *sector_map, uint8_t filler, uint8_t gap );

    /**
     * @brief Vyplní všechny sektory na stopě výchozí hodnotou.
     * @param h Handler.
     * @param dsk_offset Offset za hlavičkou stopy.
     * @param sectors Počet sektorů.
     * @param ssize Kódovaná velikost sektoru.
     * @param default_value Filler byte.
     * @param sectors_total_bytes Výstup: celková velikost zapsaných dat.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_create_track_sectors ( st_HANDLER *h, uint32_t dsk_offset, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t default_value, uint16_t *sectors_total_bytes );

    /**
     * @brief Změní geometrii a obsah jedné stopy v existujícím obrazu.
     *
     * Pokud se změní velikost stopy, přesune data následujících stop.
     *
     * @param h Handler.
     * @param short_image_info Informace o obrazu (NULL = načte se automaticky).
     * @param abstrack Absolutní stopa.
     * @param sectors Nový počet sektorů.
     * @param ssize Nová kódovaná velikost sektoru.
     * @param sector_map Nová mapa sektorů.
     * @param default_value Filler byte.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_change_track ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, uint8_t abstrack, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t *sector_map, uint8_t default_value );

    /**
     * @brief Přidá stopy na konec existujícího DSK obrazu.
     * @param h Handler.
     * @param desc Popis geometrie s novými stopami.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_add_tracks ( st_HANDLER *h, st_DSK_DESCRIPTION *desc );

    /**
     * @brief Zmenší obraz odstraněním stop od konce.
     * @param h Handler.
     * @param short_image_info Informace o obrazu (NULL = načte se automaticky).
     * @param total_tracks Nový celkový počet absolutních stop.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_shrink_image ( st_HANDLER *h, st_DSK_SHORT_IMAGE_INFO *short_image_info, uint8_t total_tracks );


    /* ─── Validace a inspekce ─── */

    /**
     * @brief Přečte pole file_info z hlavičky DSK.
     * @param h Handler.
     * @param dsk_fileinfo_buffer Výstupní buffer (min DSK_FILEINFO_FIELD_LENGTH bajtů).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_get_dsk_fileinfo ( st_HANDLER *h, uint8_t *dsk_fileinfo_buffer );

    /**
     * @brief Ověří, že pole file_info v hlavičce odpovídá Extended CPC DSK formátu.
     * @param h Handler.
     * @return EXIT_SUCCESS pokud je platný, EXIT_FAILURE pokud ne.
     */
    extern int dsk_tools_check_dsk_fileinfo ( st_HANDLER *h );

    /**
     * @brief Přečte pole creator z hlavičky DSK.
     * @param h Handler.
     * @param dsk_creator_buffer Výstupní buffer (min DSK_CREATOR_FIELD_LENGTH bajtů).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_get_dsk_creator ( st_HANDLER *h, uint8_t *dsk_creator_buffer );


    /** @brief Výsledek kontroly track info identifikátoru. */
    typedef enum en_DSK_TOOLS_CHCKTRKINFO {
        DSK_TOOLS_CHCKTRKINFO_SUCCESS = 0,      /**< Track info je platný */
        DSK_TOOLS_CHCKTRKINFO_READ_ERROR,        /**< Chyba čtení */
        DSK_TOOLS_CHCKTRKINFO_FAILURE,           /**< Track info není platný */
    } en_DSK_TOOLS_CHCKTRKINFO;

    /**
     * @brief Ověří identifikátor track info na zadaném offsetu.
     * @param h Handler.
     * @param offset Offset v souboru kde se očekává track info.
     * @return Výsledek kontroly (en_DSK_TOOLS_CHCKTRKINFO).
     */
    extern en_DSK_TOOLS_CHCKTRKINFO dsk_tools_check_dsk_trackinfo_on_offset ( st_HANDLER *h, uint32_t offset );

    /**
     * @brief Kompletní validace DSK obrazu s volitelným automatickým opravením.
     *
     * Zpětně kompatibilní wrapper nad novým diagnostickým API.
     * Interně volá dsk_tools_diagnose() a případně dsk_tools_repair().
     *
     * @param h Handler.
     * @param print_info Nenulová = logovat detaily přes logovací callback.
     * @param dsk_autofix Nenulová = opravit nalezené chyby v hlavičce.
     * @return EXIT_SUCCESS pokud je obraz v pořádku (nebo opraven), EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_check_dsk ( st_HANDLER *h, int print_info, int dsk_autofix );


    /* ─── Diagnostické API ─── */

    /**
     * @brief Diagnostické flagy per-track (bitová maska).
     *
     * Každá stopa může mít nula nebo více flagů indikujících nalezené problémy.
     */
    typedef enum en_DSK_DIAG_TRACK_FLAG {
        DSK_DIAG_TRACK_OK               = 0x0000, /**< Stopa je v pořádku */
        DSK_DIAG_TRACK_NO_TRACKINFO     = 0x0001, /**< Chybí/neplatný Track-Info identifikátor */
        DSK_DIAG_TRACK_READ_ERROR       = 0x0002, /**< Chyba čtení dat stopy */
        DSK_DIAG_TRACK_BAD_TRACK_NUM    = 0x0004, /**< Špatné číslo stopy v track headeru */
        DSK_DIAG_TRACK_BAD_SIDE_NUM     = 0x0008, /**< Špatné číslo strany v track headeru */
        DSK_DIAG_TRACK_BAD_SECTORS      = 0x0010, /**< Neplatný počet sektorů */
        DSK_DIAG_TRACK_BAD_SSIZE        = 0x0020, /**< Neplatná kódovaná velikost sektoru */
        DSK_DIAG_TRACK_BAD_TSIZE        = 0x0040, /**< tsize v DSK hlavičce nesedí se skutečností */
        DSK_DIAG_TRACK_DATA_UNREADABLE  = 0x0080, /**< Sektorová data nelze přečíst */
        DSK_DIAG_TRACK_ZERO_DATALEN     = 0x0100, /**< >= 1 sektor má v Sector Info nulovou délku dat (opravitelné, není fatální) */
    } en_DSK_DIAG_TRACK_FLAG;


    /**
     * @brief Per-track diagnostický záznam.
     *
     * Obsahuje informace o jedné stopě - skutečné hodnoty z track headeru,
     * očekávané hodnoty a flagy indikující nalezené problémy.
     */
    typedef struct st_DSK_DIAG_TRACK {
        uint8_t  abstrack;          /**< Absolutní číslo stopy */
        uint32_t offset;            /**< Offset stopy v souboru */
        uint16_t flags;             /**< Bitová maska en_DSK_DIAG_TRACK_FLAG */
        uint8_t  hdr_track;         /**< Číslo stopy z track headeru */
        uint8_t  hdr_side;          /**< Číslo strany z track headeru */
        uint8_t  expected_track;    /**< Očekávané číslo stopy */
        uint8_t  expected_side;     /**< Očekávaná strana */
        uint8_t  sectors;           /**< Počet sektorů na stopě */
        uint8_t  ssize;             /**< Kódovaná velikost sektoru */
        uint8_t  computed_tsize;    /**< Skutečná tsize (spočtená z dat) */
        uint8_t  header_tsize;      /**< tsize z DSK hlavičky */
    } st_DSK_DIAG_TRACK;


    /**
     * @brief Diagnostické flagy celého obrazu (bitová maska).
     *
     * Sumarizují stav celého DSK obrazu.
     */
    typedef enum en_DSK_DIAG_IMAGE_FLAG {
        DSK_DIAG_IMAGE_OK                    = 0x0000, /**< Obraz je v pořádku */
        DSK_DIAG_IMAGE_BAD_FILEINFO          = 0x0001, /**< Neplatný file_info identifikátor */
        DSK_DIAG_IMAGE_BAD_TRACKCOUNT        = 0x0002, /**< Počet stop v hlavičce != skutečnost */
        DSK_DIAG_IMAGE_ODD_DOUBLE            = 0x0004, /**< 2-sided s lichým počtem stop */
        DSK_DIAG_IMAGE_BAD_TSIZE             = 0x0008, /**< >= 1 stopa má špatnou tsize */
        DSK_DIAG_IMAGE_TRAILING_DATA         = 0x0010, /**< Data za poslední stopou */
        DSK_DIAG_IMAGE_TRACK_ERRORS          = 0x0020, /**< >= 1 stopa má per-track chybu jinou než BAD_TSIZE a ZERO_DATALEN */
        DSK_DIAG_IMAGE_TRACKCOUNT_EXCEEDED   = 0x0040, /**< Hlavička deklaruje tracks*sides > DSK_MAX_TOTAL_TRACKS (audit H-11) */
        DSK_DIAG_IMAGE_ZERO_DATALEN          = 0x0080, /**< >= 1 stopa má sektor s nulovou délkou dat v Sector Info */
    } en_DSK_DIAG_IMAGE_FLAG;


    /**
     * @brief Celkový diagnostický výsledek.
     *
     * Alokuje se dynamicky přes dsk_tools_diagnose(), uvolňuje přes
     * dsk_tools_destroy_diag_result(). Obsahuje sumarizaci na úrovni
     * celého obrazu i per-track diagnostiku.
     *
     * @invariant Pokud count_tracks > 0, pak tracks != NULL.
     * @invariant Pokud count_tracks == 0, pak tracks == NULL.
     */
    typedef struct st_DSK_DIAG_RESULT {
        uint16_t image_flags;                              /**< Bitová maska en_DSK_DIAG_IMAGE_FLAG */
        uint8_t  header_tracks;                            /**< tracks*sides z hlavičky (po oříznutí na DSK_MAX_TOTAL_TRACKS) */
        uint8_t  actual_tracks;                            /**< Skutečný počet platných stop */
        uint8_t  raw_header_tracks;                        /**< Raw `tracks` z hlavičky před ořezem (audit H-11) */
        uint8_t  sides;                                    /**< Počet stran */
        uint8_t  creator[DSK_CREATOR_FIELD_LENGTH + 1];    /**< Creator (nulou ukončený) */
        uint8_t  tsize_differences;                        /**< Počet stop se špatnou tsize */
        uint8_t  zero_datalen_tracks;                      /**< Počet stop s flagem DSK_DIAG_TRACK_ZERO_DATALEN */
        uint32_t expected_image_size;                      /**< Hlavička + všechny stopy */
        uint32_t actual_file_size;                         /**< Skutečná velikost souboru */
        uint8_t  count_tracks;                             /**< Počet záznamů v tracks[] */
        st_DSK_DIAG_TRACK *tracks;                         /**< Dynamické pole per-track záznamů (nebo NULL) */
    } st_DSK_DIAG_RESULT;


    /**
     * @brief Provede kompletní diagnostiku DSK obrazu bez opravy.
     *
     * Analyzuje hlavičku, všechny stopy, porovná tsize, detekuje trailing data.
     * Výsledek alokuje dynamicky - volající ho musí uvolnit přes
     * dsk_tools_destroy_diag_result().
     *
     * @param h Handler (musí být otevřený a platný).
     * @return Dynamicky alokovaný diagnostický výsledek, nebo NULL při fatální chybě alokace.
     *
     * @post Vrácená struktura vlastní svou paměť (tracks[]).
     */
    extern st_DSK_DIAG_RESULT* dsk_tools_diagnose ( st_HANDLER *h );

    /**
     * @brief Uvolní diagnostický výsledek alokovaný přes dsk_tools_diagnose().
     *
     * Bezpečné volat s NULL (NOP).
     *
     * @param result Ukazatel na výsledek k uvolnění, nebo NULL.
     */
    extern void dsk_tools_destroy_diag_result ( st_DSK_DIAG_RESULT *result );

    /**
     * @brief Zjistí, zda diagnostický výsledek obsahuje opravitelné chyby.
     *
     * Opravitelné chyby: BAD_TRACKCOUNT, BAD_TSIZE, TRAILING_DATA,
     * TRACKCOUNT_EXCEEDED, ZERO_DATALEN.
     *
     * @param result Diagnostický výsledek (může být NULL - vrátí 0).
     * @return Nenulová pokud existují opravitelné chyby, 0 pokud ne.
     */
    extern int dsk_tools_diag_has_repairable_errors ( const st_DSK_DIAG_RESULT *result );

    /**
     * @brief Zjistí, zda diagnostický výsledek obsahuje fatální chyby.
     *
     * Fatální chyby: BAD_FILEINFO, per-track BAD_TRACK_NUM/BAD_SIDE_NUM/
     * BAD_SECTORS/BAD_SSIZE/DATA_UNREADABLE.
     *
     * @param result Diagnostický výsledek (může být NULL - vrátí 0).
     * @return Nenulová pokud existují fatální chyby, 0 pokud ne.
     */
    extern int dsk_tools_diag_has_fatal_errors ( const st_DSK_DIAG_RESULT *result );

    /**
     * @brief Opraví opravitelné chyby na základě diagnostiky.
     *
     * Opravuje: počet stop v hlavičce, tsize pole, trailing data za
     * poslední stopou (zkrácením obrazu) a nulové délky dat sektorů
     * v Sector Info (ZERO_DATALEN).
     * Vyžaduje handler otevřený pro zápis.
     *
     * Při ZERO_DATALEN se nulová délka nahradí velikostí, se kterou
     * sektor počítá dsk_compute_sector_offset(): dekódované pole ssize
     * sektoru, a je-li nulové, dekódované ssize stopy. Nenulové délky,
     * sektorová data a ostatní pole Sector Info se nemění.
     *
     * @param h Handler (musí být otevřený pro zápis).
     * @param diag Diagnostický výsledek z dsk_tools_diagnose().
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre diag != NULL, h otevřený pro zápis.
     */
    extern int dsk_tools_repair ( st_HANDLER *h, const st_DSK_DIAG_RESULT *diag );


    /* ─── Inspekce hlaviček ─── */

    /**
     * @brief Raw informace z DSK hlavičky.
     *
     * Přímý přepis polí z hlavičky DSK souboru bez jakékoliv interpretace.
     * Řetězcová pole (file_info, creator) jsou nulou ukončená.
     */
    typedef struct st_DSK_HEADER_INFO {
        uint8_t file_info[DSK_FILEINFO_FIELD_LENGTH + 1];  /**< Identifikační řetězec (nulou ukončený) */
        uint8_t creator[DSK_CREATOR_FIELD_LENGTH + 1];     /**< Creator string (nulou ukončený) */
        uint8_t tracks;                                    /**< Počet stop per strana */
        uint8_t sides;                                     /**< Počet stran */
        uint8_t tsize[DSK_MAX_TOTAL_TRACKS];               /**< Pole tsize pro každou stopu */
    } st_DSK_HEADER_INFO;


    /**
     * @brief Raw informace z hlavičky jedné stopy.
     *
     * Přímý přepis polí z track headeru včetně kompletních sector info.
     */
    typedef struct st_DSK_TRACK_HEADER_INFO {
        uint8_t track;                              /**< Číslo stopy */
        uint8_t side;                               /**< Strana (0/1) */
        uint8_t ssize;                              /**< Kódovaná velikost sektoru */
        uint8_t sectors;                            /**< Počet sektorů */
        uint8_t gap;                                /**< Délka GAP#3 */
        uint8_t filler;                             /**< Filler byte */
        st_DSK_SECTOR_INFO sinfo[DSK_MAX_SECTORS];  /**< Kompletní sector info včetně FDC statusu */
    } st_DSK_TRACK_HEADER_INFO;


    /**
     * @brief Přečte raw informace z DSK hlavičky.
     *
     * @param h Handler.
     * @param info Výstupní struktura.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_read_header_info ( st_HANDLER *h, st_DSK_HEADER_INFO *info );

    /**
     * @brief Přečte raw informace z hlavičky jedné stopy.
     *
     * @param h Handler.
     * @param abstrack Absolutní číslo stopy.
     * @param info Výstupní struktura.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre abstrack musí být platný index stopy v obrazu.
     */
    extern int dsk_tools_read_track_header_info ( st_HANDLER *h, uint8_t abstrack, st_DSK_TRACK_HEADER_INFO *info );

    /**
     * @brief Detekuje trailing data za poslední stopou.
     *
     * Porovná očekávanou velikost (hlavička + součet tsize) se skutečnou
     * velikostí souboru/paměti.
     *
     * @param h Handler.
     * @param trailing_offset Výstup: offset kde trailing data začínají (0 pokud nejsou).
     * @param trailing_size Výstup: velikost trailing dat v bajtech (0 pokud nejsou).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_detect_trailing_data ( st_HANDLER *h, uint32_t *trailing_offset, uint32_t *trailing_size );


    /* ─── Editace hlaviček ─── */

    /**
     * @brief Nastaví pole creator v DSK hlavičce.
     *
     * Zkopíruje max DSK_CREATOR_FIELD_LENGTH znaků, zbytek doplní nulami.
     *
     * @param h Handler (musí být otevřený pro zápis).
     * @param creator Nový creator string (max DSK_CREATOR_FIELD_LENGTH znaků).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_set_creator ( st_HANDLER *h, const char *creator );

    /**
     * @brief Upraví vybraná pole v hlavičce stopy.
     *
     * Načte stávající track header, změní požadovaná pole a zapíše zpět.
     * Hodnota -1 znamená "neměnit", 0-255 je nová hodnota daného pole.
     * Díky rozsahu int16_t lze bezpečně nastavit i hodnotu 0xFF (běžný
     * filler u 5.25" disket) - hodnota 0xFF byla dříve sentinelem a
     * nešla tak uložit (BUG 4 final test report 2026-04-19).
     *
     * @param h Handler (musí být otevřený pro zápis).
     * @param abstrack Absolutní číslo stopy.
     * @param track_num Nové číslo stopy (-1 = neměnit, 0-255 = hodnota).
     * @param side Nová strana (-1 = neměnit, 0 nebo 1).
     * @param gap Nová hodnota GAP#3 (-1 = neměnit, 0-255 = hodnota).
     * @param filler Nový filler byte (-1 = neměnit, 0-255 = hodnota).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_set_track_header ( st_HANDLER *h, uint8_t abstrack,
        int16_t track_num, int16_t side, int16_t gap, int16_t filler );

    /**
     * @brief Nastaví FDC stavové registry pro konkrétní sektor.
     *
     * @param h Handler (musí být otevřený pro zápis).
     * @param abstrack Absolutní číslo stopy.
     * @param sector_idx Index sektoru v rámci stopy (0-based).
     * @param fdc_sts1 Nová hodnota FDC stavového registru 1.
     * @param fdc_sts2 Nová hodnota FDC stavového registru 2.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre sector_idx musí být menší než počet sektorů na stopě.
     */
    extern int dsk_tools_set_sector_fdc_status ( st_HANDLER *h, uint8_t abstrack,
        uint8_t sector_idx, uint8_t fdc_sts1, uint8_t fdc_sts2 );

    /**
     * @brief Nastaví ID jednoho sektoru v hlavičce stopy.
     *
     * Modifikuje pole sinfo[sector_idx].sector v track headeru.
     * Data sektoru a ostatní pole sinfo (track, side, ssize, fdc_sts)
     * zůstávají beze změny.
     *
     * @param h Handler (musí být otevřený pro zápis).
     * @param abstrack Absolutní číslo stopy.
     * @param sector_idx Index sektoru v rámci stopy (0-based).
     * @param new_sector_id Nová hodnota ID sektoru (0-255).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre h != NULL, abstrack platný, sector_idx < počet sektorů na stopě.
     * @post sinfo[sector_idx].sector == new_sector_id v track headeru.
     */
    extern int dsk_tools_set_sector_id ( st_HANDLER *h, uint8_t abstrack,
        uint8_t sector_idx, uint8_t new_sector_id );

    /**
     * @brief Nastaví ID všech sektorů na stopě najednou.
     *
     * Přepíše sinfo[0..count-1].sector v track headeru.
     * Ostatní pole sinfo (track, side, ssize, fdc_sts) a data sektorů
     * zůstávají beze změny.
     *
     * @param h Handler (musí být otevřený pro zápis).
     * @param abstrack Absolutní číslo stopy.
     * @param sector_ids Pole nových ID sektorů (min count prvků).
     * @param count Počet prvků v poli (musí odpovídat počtu sektorů na stopě).
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     *
     * @pre h != NULL, sector_ids != NULL, count == počet sektorů na stopě.
     * @post sinfo[i].sector == sector_ids[i] pro i ∈ <0, count).
     */
    extern int dsk_tools_set_sector_ids ( st_HANDLER *h, uint8_t abstrack,
        const uint8_t *sector_ids, uint8_t count );


    /* ─── Verze knihovny ─── */

    /** @brief Verze knihovny dsk_tools. */
#define DSK_TOOLS_VERSION "3.4.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny dsk_tools.
     * @return Statický řetězec s verzí.
     */
    extern const char* dsk_tools_version ( void );


    /* ─── Analýza geometrie (pravidla stop) ─── */

    /** @brief Kompaktní pravidlo popisující rozsah stop se stejnou geometrií. */
    typedef struct st_DSK_TOOLS_TRACK_RULE_INFO {
        uint8_t from_track;         /**< Absolutní stopa od které pravidlo platí */
        uint8_t count_tracks;       /**< Počet stop pokrytých tímto pravidlem */
        uint8_t sectors;            /**< Počet sektorů na stopě */
        en_DSK_SECTOR_SIZE ssize;   /**< Kódovaná velikost sektoru */
    } st_DSK_TOOLS_TRACK_RULE_INFO;


    /** @brief Výsledek analýzy geometrie disku — sada pravidel stop. */
    typedef struct st_DSK_TOOLS_TRACKS_RULES_INFO {
        uint8_t total_tracks;                   /**< Celkový počet absolutních stop */
        uint8_t sides;                          /**< Počet stran */
        uint8_t count_rules;                    /**< Počet pravidel */
        uint8_t mzboot_track;                   /**< 1 pokud stopa 1 je MZ boot (16x256B) */
        st_DSK_TOOLS_TRACK_RULE_INFO *rule;     /**< Pole pravidel (dynamicky alokované) */
    } st_DSK_TOOLS_TRACKS_RULES_INFO;


    /**
     * @brief Uvolní paměť alokovanou pro pravidla stop.
     * @param tracks_rules Struktura k uvolnění (může být NULL).
     */
    extern void dsk_tools_destroy_track_rules ( st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules );

    /**
     * @brief Analyzuje geometrii DSK obrazu a extrahuje pravidla stop.
     * @param h Handler.
     * @return Alokovaná struktura (uvolnit přes dsk_tools_destroy_track_rules()), nebo NULL při chybě.
     */
    extern st_DSK_TOOLS_TRACKS_RULES_INFO* dsk_tools_get_tracks_rules ( st_HANDLER *h );


    /* ─── Identifikace formátu ─── */

    /** @brief Rozpoznané formáty MZ disket. */
    typedef enum en_DSK_TOOLS_IDENTFORMAT {
        DSK_TOOLS_IDENTFORMAT_UNKNOWN = 0,  /**< Neznámý formát */
        DSK_TOOLS_IDENTFORMAT_MZBASIC,      /**< Sharp MZ-BASIC disketa (16 x 256 B na všech stopách) */
        DSK_TOOLS_IDENTFORMAT_MZCPM,        /**< Sharp LEC CP/M SD (9 x 512 B + boot 16 x 256 B) */
        DSK_TOOLS_IDENTFORMAT_MZCPMHD,      /**< Sharp LEC CP/M HD (18 x 512 B + boot 16 x 256 B) */
        DSK_TOOLS_IDENTFORMAT_MZBOOT,       /**< Bootovatelný disk (1. stopa je MZ-BASIC, zbývající formát neznámý) */
    } en_DSK_TOOLS_IDENTFORMAT;


    /**
     * @brief Identifikuje formát z existujících pravidel stop.
     * @param tracks_rules Pravidla stop (může být NULL -> UNKNOWN).
     * @return Identifikovaný formát.
     */
    extern en_DSK_TOOLS_IDENTFORMAT dsk_tools_identformat_from_tracks_rules ( const st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules );

    /**
     * @brief Identifikuje formát DSK obrazu.
     * @param h Handler.
     * @param result Výstup: identifikovaný formát.
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
     */
    extern int dsk_tools_identformat ( st_HANDLER *h, en_DSK_TOOLS_IDENTFORMAT *result );

    /**
     * @brief Vrátí pravidlo platné pro zadanou stopu.
     * @param tracks_rules Pravidla stop (může být NULL).
     * @param track Absolutní číslo stopy.
     * @return Ukazatel na pravidlo, nebo NULL pokud neexistuje.
     */
    extern st_DSK_TOOLS_TRACK_RULE_INFO* dsk_tools_get_rule_for_track ( const st_DSK_TOOLS_TRACKS_RULES_INFO *tracks_rules, uint8_t track );


    /* ─── Iterace přes stopy a sektory ─── */

    /**
     * @brief Callback pro iteraci přes stopy.
     *
     * @param h Handler.
     * @param abstrack Absolutní číslo stopy.
     * @param tinfo Informace o stopě.
     * @param user_data Uživatelská data z dsk_for_each_track.
     * @return 0 = pokračovat, nenulová = zastavit iteraci (vrátí se volajícímu).
     */
    typedef int (*dsk_track_callback_t)( st_HANDLER *h, uint8_t abstrack, const st_DSK_SHORT_TRACK_INFO *tinfo, void *user_data );

    /**
     * @brief Callback pro iteraci přes sektory na stopě.
     *
     * @param h Handler.
     * @param abstrack Absolutní číslo stopy.
     * @param sector_idx Index sektoru v rámci stopy (0-based).
     * @param sector_id ID sektoru (z hlavičky).
     * @param sector_offset Absolutní offset dat sektoru v souboru.
     * @param sector_size Velikost sektoru v bajtech.
     * @param user_data Uživatelská data z dsk_for_each_sector.
     * @return 0 = pokračovat, nenulová = zastavit iteraci.
     */
    typedef int (*dsk_sector_callback_t)( st_HANDLER *h, uint8_t abstrack, uint8_t sector_idx, uint8_t sector_id, uint32_t sector_offset, uint16_t sector_size, void *user_data );

    /**
     * @brief Iteruje přes všechny stopy v DSK obrazu a volá callback pro každou.
     * @param h Handler.
     * @param cb Callback volaný pro každou stopu.
     * @param user_data Uživatelská data předávaná callbacku.
     * @return EXIT_SUCCESS, EXIT_FAILURE (chyba I/O), nebo nenulová návratová hodnota z callbacku.
     */
    extern int dsk_for_each_track ( st_HANDLER *h, dsk_track_callback_t cb, void *user_data );

    /**
     * @brief Iteruje přes všechny sektory na zadané stopě a volá callback pro každý.
     * @param h Handler.
     * @param abstrack Absolutní číslo stopy.
     * @param cb Callback volaný pro každý sektor.
     * @param user_data Uživatelská data předávaná callbacku.
     * @return EXIT_SUCCESS, EXIT_FAILURE, nebo nenulová návratová hodnota z callbacku.
     */
    extern int dsk_for_each_sector ( st_HANDLER *h, uint8_t abstrack, dsk_sector_callback_t cb, void *user_data );

#ifdef __cplusplus
}
#endif

#endif /* DSK_TOOLS_H */
