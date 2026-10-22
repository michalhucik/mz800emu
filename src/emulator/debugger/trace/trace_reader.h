/**
 * @file   trace_reader.h
 * @brief  Stránkované čtení záznamů Trace Suite (trace_read přes MCP).
 *
 * Čistá logika čtenáře proudu záznamů podle kontraktu
 * `devdoc/trace/stream-read.md`: kurzor, čtení chunků z disku a kopie
 * z RAM bufferu (snímek st_TLOG_READ_SNAPSHOT pořízený v emu vlákně),
 * čas u cputrack, filtry, dekódování záznamů na řádky a sestavení stránky
 * s rozpočtem znaků. Modul nečte stav emulátoru; symboly adres a jména
 * značek dodává volající přes callback (@ref trace_resolve_fn), protože je
 * smí číst jen emu vlákno.
 *
 * @section threads Vlákna
 *
 * Funkce běží v dispatch vlákně MCP (i ve více instancích současně).
 * Jediný sdílený stav je cache času cputrack, chráněná vlastním zámkem.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#ifndef TRACE_READER_H
#define TRACE_READER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <json-glib/json-glib.h>

#include "tlog_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Druh kanálu z pohledu čtenáře (určuje dekodér a sloupce).
 */
typedef enum en_TRACE_KIND
{
    TRACE_KIND_CPUTRACK = 0, /**< Záznam instrukce: 12 B (v1, v2) nebo 32 B s registry (v3). */
    TRACE_KIND_IORQLOG,      /**< 24B I/O událost. */
    TRACE_KIND_INTLOG,       /**< 24B událost přerušení. */
    TRACE_KIND_HWLOG,        /**< 24B HW událost čipu. */
    TRACE_KIND_MARKLOG,      /**< 24B značka z akce BP. */
    TRACE_KIND_EVENTLOG,     /**< 32B událost ringu Event Vieweru (st_EVENTLOG_EVENT). */
    TRACE_KIND_AUDIO_PSG_WRITES, /**< 24B zápis do PSG z paměti zachycení zvuku (st_AUDIO_CAPTURE_PSG_REC). */
    TRACE_KIND_AUDIO_LEVELS,     /**< 16B změna úrovně kanálu (st_AUDIO_CAPTURE_LEVEL_REC). */
    TRACE_KIND_AUDIO_FRAMES,     /**< 192B souhrn zvukového snímku (st_AUDIO_CAPTURE_FRAME_REC). */
    TRACE_KIND_RAMLOG,           /**< 32B zápis do paměti (st_RAMLOG_REC, ring ramlog). */
    TRACE_KIND_COUNT         /**< Počet druhů (není platná hodnota). */
} en_TRACE_KIND;

/**
 * @brief Jméno kanálu druhu (např. "cputrack"), NULL pro neplatný druh.
 */
const char *trace_kind_name ( en_TRACE_KIND kind );

/* ===========================================================================
 *  Kurzor
 * =========================================================================== */

/**
 * @brief Zakódovat kurzor `t1.` + base64url(`<channel>:<segment>:<seq>`).
 *
 * @return Nový řetězec (g_free), nikdy NULL.
 */
char *trace_cursor_encode ( const char *channel, unsigned segment, uint64_t seq );

/**
 * @brief Dekódovat kurzor a ověřit, že patří kanálu @p channel.
 *
 * @param cursor   Kurzor od klienta.
 * @param channel  Očekávaný kanál.
 * @param segment  Výstup: segment z kurzoru.
 * @param seq      Výstup: seq z kurzoru.
 * @return true = platný kurzor tohoto kanálu, false = neplatný nebo cizí.
 */
bool trace_cursor_decode ( const char *cursor, const char *channel,
                           unsigned *segment, uint64_t *seq );

/* ===========================================================================
 *  Čtení záznamů
 * =========================================================================== */

/**
 * @brief Přečíst souvislý rozsah záznamů z chunků na disku a z kopie RAM.
 *
 * Záznamy `[from_seq, from_seq + count)` se čtou z chunků na disku
 * (část `< ram_first_seq`) a z `snap->ram_copy` (část v RAM). Čtení
 * skončí dřív, když požadovaná část RAM v kopii není, nebo na
 * `next_seq`. Soubor chunku drží otevřený jen po dobu čtení.
 *
 * @param snap       Snímek segmentu.
 * @param from_seq   První záznam.
 * @param count      Nejvýš záznamů.
 * @param dst        Cíl (aspoň `count * record_size` bajtů).
 * @param out_count  Výstup: přečtené záznamy (souvislé od from_seq).
 * @return 0 OK, -1 chyba čtení chunku (chybí soubor, krátký soubor).
 */
int trace_reader_fetch ( const st_TLOG_READ_SNAPSHOT *snap,
                         uint64_t from_seq, uint64_t count,
                         uint8_t *dst, uint64_t *out_count );

/* ===========================================================================
 *  Stránka (trace_read)
 * =========================================================================== */

/**
 * @brief Filtr řádků trace_read (devdoc/trace/stream-read.md, Filtry).
 *
 * Pole `has_*` určují, které podmínky platí (všechny najednou, AND).
 */
typedef struct st_TRACE_FILTER
{
    bool     has_pc;     /**< cputrack: pc v [pc_from, pc_to]. */
    uint16_t pc_from;
    uint16_t pc_to;
    bool     has_port;   /**< iorqlog: port (<= FFh porovná dolní bajt, jinak 16 bitů). */
    uint16_t port;
    int      dir;        /**< iorqlog: -1 = nefiltrovat, 0 = in, 1 = out. */
    int      type;       /**< iorqlog: -1 = nefiltrovat, jinak en_IORQLOG_EVENT_TYPE. */
    int      chip;       /**< hwlog: -1 = nefiltrovat, jinak chip_id. */
    int      sub;        /**< hwlog: -1 = nefiltrovat, jinak sub_event_type. */
    int      marker;     /**< marklog: -1 = nefiltrovat, jinak marker_id. */
    uint32_t channels;   /**< audio_levels / audio_psg_writes: bity zvukových kanálů (0 = ctc0, 1..4 psg0, 5..8 psg1), 0 = nefiltrovat. */
    int      reg;        /**< audio_psg_writes: -1 = nefiltrovat, jinak en_AUDIO_CAPTURE_PSG_REG. */
    bool     has_addr;   /**< ramlog: CPU adresa zápisu v [addr_from, addr_to]. */
    uint16_t addr_from;
    uint16_t addr_to;
    int      space;      /**< ramlog: -1 = nefiltrovat, jinak en_RAMLOG_SPACE. */
    int      src;        /**< ramlog: -1 = nefiltrovat, jinak en_RAMLOG_SRC. */
    bool     all;        /**< ramlog: i záznamy nezměněné a potlačené (výchozí skryté). */
    bool     has_t;      /**< t v [t_from, t_to] (jen řádky s časem). */
    uint64_t t_from;
    uint64_t t_to;
    unsigned n_regs;     /**< cputrack v3: počet podmínek na registry v regs[] (0 = žádná). */
    struct {
        uint8_t  reg;    /**< Registr (TRACE_CPUTRACK_REG_*). */
        uint16_t lo;     /**< Dolní mez hodnoty (včetně). */
        uint16_t hi;     /**< Horní mez hodnoty (včetně). */
    } regs[ 16 ];        /**< Podmínky na stav registrů (po/před podle st_TRACE_PAGE_REQ.regs_before). */
} st_TRACE_FILTER;

/** @brief Inicializovat filtr na "bez podmínek". */
void trace_filter_init ( st_TRACE_FILTER *f );

/**
 * @name Registry cputrack v3 (sloupce a filtr trace_read)
 *
 * Pořadí odpovídá sloupcům za `kind` v trace_columns() (sloupec =
 * TRACE_COL_CPUTRACK_REG0 + registr).
 * @{
 */
#define TRACE_CPUTRACK_REG_AF     0u  /**< AF */
#define TRACE_CPUTRACK_REG_BC     1u  /**< BC */
#define TRACE_CPUTRACK_REG_DE     2u  /**< DE */
#define TRACE_CPUTRACK_REG_HL     3u  /**< HL */
#define TRACE_CPUTRACK_REG_IX     4u  /**< IX */
#define TRACE_CPUTRACK_REG_IY     5u  /**< IY */
#define TRACE_CPUTRACK_REG_SP     6u  /**< SP */
#define TRACE_CPUTRACK_REG_WZ     7u  /**< WZ (MEMPTR) */
#define TRACE_CPUTRACK_REG_I      8u  /**< I */
#define TRACE_CPUTRACK_REG_R      9u  /**< R */
#define TRACE_CPUTRACK_REG_IFF1   10u /**< IFF1 (0/1) */
#define TRACE_CPUTRACK_REG_IFF2   11u /**< IFF2 (0/1) */
#define TRACE_CPUTRACK_REG_IM     12u /**< Režim přerušení (0-2) */
#define TRACE_CPUTRACK_REG_HALTED 13u /**< CPU v HALT (0/1) */
#define TRACE_CPUTRACK_REG_COUNT  14u /**< Počet registrů. */
/** @brief Index sloupce prvního registru (af) v sloupcích cputrack. */
#define TRACE_COL_CPUTRACK_REG0   9u
/** @brief Maska sloupců registrů cputrack (jen segment v3). */
#define TRACE_CPUTRACK_REG_COLS_MASK ( ( ( 1u << TRACE_CPUTRACK_REG_COUNT ) - 1u ) << TRACE_COL_CPUTRACK_REG0 )
/** @} */

/**
 * @brief Index registru cputrack v3 podle jména sloupce ("hl", "iff1", ...).
 *
 * @return TRACE_CPUTRACK_REG_*, nebo -1 pro neznámé jméno.
 */
int trace_cputrack_reg_index ( const char *name );

/**
 * @brief Jméno zvukového kanálu podle indexu audio logu.
 *
 * @param src Index 0 = ctc0, 1..4 = psg0_0..3, 5..8 = psg1_0..3.
 * @return Statický řetězec ("?" mimo rozsah).
 */
const char *trace_audio_channel_name ( unsigned src );

/** @brief Velikost jména (symbol, značka) v st_TRACE_RESOLVE_REQ. */
#define TRACE_RESOLVE_NAME_SIZE 64u
/** @brief Velikost textu události v st_TRACE_RESOLVE_REQ. */
#define TRACE_RESOLVE_TEXT_SIZE 128u

/**
 * @brief Požadavek na doplnění údajů, které smí číst jen emu vlákno.
 *
 * Výstupní pole alokuje trace_reader_page (vynulovaná), callback je
 * vyplní. Kterákoli část může být prázdná (n_* = 0).
 *
 * @field addrs        Unikátní adresy stránky (cputrack, sloupec sym).
 * @field n_addrs      Počet adres.
 * @field syms         Výstup n_addrs jmen symbolů, každé TRACE_RESOLVE_NAME_SIZE B ("" = žádný).
 * @field sym_offs     Výstup n_addrs offsetů od symbolu.
 * @field markers      Unikátní id značek stránky (marklog, sloupec name).
 * @field n_markers    Počet id.
 * @field marker_names Výstup n_markers jmen, každé TRACE_RESOLVE_NAME_SIZE B ("" = neznámé).
 * @field events       Záznamy událostí stránky (eventlog, sloupec text), 32 B každý.
 * @field n_events     Počet událostí.
 * @field event_texts  Výstup n_events textů, každý TRACE_RESOLVE_TEXT_SIZE B.
 */
typedef struct st_TRACE_RESOLVE_REQ
{
    const uint16_t *addrs;
    size_t          n_addrs;
    char           *syms;
    uint32_t       *sym_offs;
    const uint16_t *markers;
    size_t          n_markers;
    char           *marker_names;
    const uint8_t  *events;
    size_t          n_events;
    char           *event_texts;
} st_TRACE_RESOLVE_REQ;

/**
 * @brief Callback pro symboly adres, jména značek a texty událostí.
 *
 * @param user  Uživatelská data volajícího.
 * @param req   Požadavek (vstup + výstupní pole).
 * @return true OK, false = nelze (stránka pak tyto údaje nemá).
 */
typedef bool ( *trace_resolve_fn ) ( void *user, st_TRACE_RESOLVE_REQ *req );

/** @brief Výchozí rozpočet znaků stránky `rows`. */
#define TRACE_READ_DEFAULT_MAX_CHARS 20000u
/** @brief Strop rozpočtu znaků stránky `rows`. */
#define TRACE_READ_MAX_MAX_CHARS     45000u
/** @brief Výchozí počet řádků stránky. */
#define TRACE_READ_DEFAULT_LIMIT     200u
/** @brief Strop počtu řádků stránky. */
#define TRACE_READ_MAX_LIMIT         5000u
/** @brief Výchozí strop prohledaných záznamů na dotaz. */
#define TRACE_READ_DEFAULT_MAX_SCAN  2000000u
/** @brief Strop dat stránky `raw_b64` v bajtech. */
#define TRACE_READ_MAX_RAW_BYTES     ( 1u << 20 )
/** @brief Strop kopie z RAM bufferu na jeden dotaz (v emu vlákně). */
#define TRACE_READ_MAX_RAM_COPY      ( 4u << 20 )

/**
 * @brief Parametry jedné stránky trace_read.
 */
typedef struct st_TRACE_PAGE_REQ
{
    en_TRACE_KIND   kind;          /**< Druh kanálu. */
    uint64_t        from_seq;      /**< První záznam stránky (už vyřešený kurzor / from). */
    unsigned        limit;         /**< Nejvýš řádků (1..TRACE_READ_MAX_LIMIT). */
    bool            raw;           /**< true = format raw_b64. */
    uint32_t        col_mask;      /**< Bitová maska sloupců (bit = index v trace_columns()). */
    st_TRACE_FILTER filter;        /**< Filtr řádků (u raw se neuplatní). */
    size_t          max_chars;     /**< Rozpočet znaků stránky rows. */
    uint64_t        max_scan;      /**< Strop prohledaných záznamů. */
    /* Čas cputrack */
    bool            cputrack_time; /**< Kanál má časový sloupec (cputrack bez filtru PC). */
    bool            regs_before;   /**< cputrack v3: sloupce a filtr registrů ukazují stav před
                                        záznamem (= stav v předchozím záznamu), jinak po něm. */
    uint64_t        ram_prefix_t;  /**< T-stavy RAM chunku před snap->ram_copy_seq (z emu vlákna). */
    unsigned        cpu_divider;   /**< Takty GDG na T-stav (pro t v taktech GDG; u audio_psg_writes pro Hz). */
    uint64_t        audio_origin;  /**< audio_*: absolutní čas začátku zachycení [takty GDG] (pro time_us). */
    uint64_t        gdg_hz;        /**< audio_*: takt GDG [Hz] (time_us, Hz tónu PSG). */
    unsigned        audio_channels; /**< audio_frames: počet zvukových kanálů platformy (1..9). */
} st_TRACE_PAGE_REQ;

/**
 * @brief Jména sloupců druhu v kanonickém pořadí.
 *
 * @param kind  Druh kanálu.
 * @param n     Výstup: počet sloupců.
 * @return Pole jmen (statické), NULL pro neplatný druh.
 */
const char *const *trace_columns ( en_TRACE_KIND kind, size_t *n );

/**
 * @brief Maska sloupců ze seznamu jmen (NULL / prázdný = výchozí).
 *
 * Výchozí jsou všechny sloupce druhu, u cputrack jen sloupce formátu v2
 * (bez `kind` a registrů, o ty si klient řekne jménem).
 *
 * @param kind    Druh kanálu.
 * @param names   Jména sloupců.
 * @param n       Počet jmen.
 * @param bad     Výstup: první neznámé jméno (NULL = vše platné).
 * @return Maska (bit i = sloupec i z trace_columns()).
 */
uint32_t trace_columns_mask ( en_TRACE_KIND kind, const char *const *names, size_t n,
                              const char **bad );

/**
 * @brief Sestavit stránku trace_read do JSON objektu odpovědi.
 *
 * Doplní pole `scanned`, `matched`, `scan_exhausted`, `page_full`,
 * `end_of_data`, `next_seq_read` (interní, seq za posledním prošlým
 * záznamem) a u `rows` `columns` + `rows`, u `raw_b64` `record_size`,
 * `from_seq`, `count`, `data_b64`. Stavová pole kanálu (segment, next_seq,
 * kurzor) doplňuje volající.
 *
 * @param req       Parametry stránky.
 * @param snap      Snímek segmentu (record_size > 0).
 * @param resolve   Callback symbolů a jmen značek (NULL = bez nich).
 * @param user      Data pro @p resolve.
 * @param out       Objekt odpovědi (doplní se).
 * @param out_next  Výstup: seq, od kterého pokračuje další stránka.
 * @return 0 OK, -1 chyba čtení chunku.
 */
int trace_reader_page ( const st_TRACE_PAGE_REQ *req,
                        const st_TLOG_READ_SNAPSHOT *snap,
                        trace_resolve_fn resolve, void *user,
                        JsonObject *out, uint64_t *out_next );

/**
 * @brief Vyprázdnit cache času cputrack (pro testy).
 */
void trace_reader_time_cache_reset ( void );

#ifdef __cplusplus
}
#endif

#endif /* TRACE_READER_H */
