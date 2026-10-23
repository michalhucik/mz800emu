/**
 * @file   tlog_common.h
 * @brief  Společný framework pro 4 trace-suite subsystémy.
 *
 * Sdílená infrastruktura pro `cputrack`, `iorqlog`, `intlog` a `hwlog`:
 *  - chunk writer (RAM buffer + sync flush na disk)
 *  - manifest <name>.json builder (per-recording, aktualizovaný po každém flush)
 *  - per-subsystém limity (chunk-mb, max-total-mb)
 *  - sjednocený přístup k pxCLK / cpuclk / screens (přes GDG counter)
 *  - konzolové notifikace (chunk swap, max reached)
 *
 * Architektura: každý subsystém vlastní jednu instanci @ref tlog_writer_t
 * (RAM buffer + soubor handle + meta state). Subsystém volá
 * @ref tlog_writer_append() po každé události. Při naplnění RAM bufferu se
 * provede synchronní blocking flush a vypíše se notifikace na konzoli.
 *
 * @note Sync writer = žádné async thready. Emulace se může zakuckávat
 *       během flush - akceptovatelné, je to ladící mód.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#ifndef TLOG_COMMON_H
#define TLOG_COMMON_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <glib.h>

/**
 * @brief Režim aktivace trace-suite subsystému.
 *
 * Paralelní k @c en_DEBUGGER_MHMAP_MODE (CDL pattern). Všechny 4 subsystémy
 * trace-suite (cputrack/iorqlog/intlog/hwlog) sdílí tento enum přes alias.
 */
typedef enum en_TLOG_MODE
{
    TLOG_MODE_OFF = 0,           /**< default - subsystém vypnutý, žádná režie */
    TLOG_MODE_WITH_WINDOW,       /**< aktivní jen pokud je otevřené debug okno */
    TLOG_MODE_ALWAYS,            /**< aktivní trvale */
} en_TLOG_MODE;

/**
 * @brief Default chunk velikost v MB (per subsystém).
 *
 * Po dosažení této velikosti se RAM buffer flushne na disk jako další
 * `<name>.NNN.bin` soubor. Vyšší hodnota = méně častý disk I/O ale
 * vyšší peak RAM usage.
 */
#define TLOG_DEFAULT_CHUNK_MB     64u

/**
 * @brief Default max-total-mb (per kanál). 0 = unlimited.
 *
 * Hard limit na celkovou velikost recordingu (sum všech chunků). Po
 * dosažení se subsystém zastaví, do manifestu <name>.json se zapíše
 * `"truncated": true, "reason": "max_total_mb"`.
 *
 * Default 2048 MB (2 GB) = pojistka proti zaplnění disku: cputrack chrlí
 * ~40 MB/s a bez stropu (0) by dlouhý záznam zaplnil disk. 2 GB je pro
 * reálné ladicí trasy dost a zároveň ohraničuje runaway. Per-kanál, takže
 * limit prakticky dopadá jen na cputrack/iorqlog (intlog/hwlog/marklog jsou
 * malé). Lze přenastavit přes INI (max_total_mb, 0 = bez omezení). Pozn.:
 * default platí jen pro NOVÉ configy; existující .ini s explicitním klíčem
 * (vč. max_total_mb=0) si drží svou hodnotu.
 */
#define TLOG_DEFAULT_MAX_TOTAL_MB 2048u

/**
 * @brief Per-chunk metadata zapisovaná do manifestu <name>.json.
 *
 * Pro každý flushnutý chunk si pamatujeme jeho start timestamp (= seek
 * anchor) a velikost. To umožňuje O(1) seek na chunk + lineární scan
 * uvnitř (rekonstrukce timestampu sumací insn_T_states + wait_clk).
 */
typedef struct st_TLOG_CHUNK_META
{
    unsigned index;              /**< 0-based chunk index */
    size_t bytes;                /**< Velikost souboru v bytech */
    uint64_t start_pxclk;        /**< pxCLK total na začátku chunku */
    uint64_t start_cpuclk;       /**< CPU clk total na začátku chunku */
    uint32_t start_screens;      /**< screens total na začátku chunku */
} st_TLOG_CHUNK_META;

/**
 * @brief Popis posledního uzavřeného segmentu writeru (čtení po stopu).
 *
 * @ref tlog_writer_close() sem přesune adresář, jméno a seznam chunků
 * místo jejich uvolnění, aby šel segment číst i po zastavení záznamu.
 * Uvolní ho až další @ref tlog_writer_open() téhož writeru.
 *
 * @invariant `segment == 0` <=> popis je prázdný (`dir`, `name`, `chunks`
 *            jsou NULL).
 * @ownership `dir`, `name` (g_strdup) a `chunks` vlastní writer.
 */
typedef struct st_TLOG_CLOSED_SEGMENT
{
    unsigned segment;            /**< Identita segmentu (0 = žádný). */
    size_t record_size;          /**< Velikost záznamu v bajtech (0 = neznámá). */
    uint64_t next_seq;           /**< Počet záznamů segmentu (vše na disku). */
    int truncated;               /**< Segment skončil na max_total_bytes. */
    char *dir;                   /**< Adresář chunků. */
    char *name;                  /**< Basename chunků. */
    GArray *chunks;              /**< Pole st_TLOG_CHUNK_META (zapsané chunky). */
} st_TLOG_CLOSED_SEGMENT;

/**
 * @brief Stav writeru jednoho trace-suite subsystému.
 *
 * Nezbytné invariants:
 *  - `buffer != NULL` po @ref tlog_writer_open(), `NULL` po @ref tlog_writer_close()
 *  - `buffer_used <= buffer_size`
 *  - `total_bytes_written` = sum všech doposud flushnutých bytů +
 *    `buffer_used` (= "live" total)
 *  - `chunk_index` = počet již zapsaných chunků (= příští flush bude
 *    `<dir>/<name>.<chunk_index:03d>.bin`)
 *  - `truncated == 1` => writer odmítá další @ref tlog_writer_append()
 *
 * @ownership Buffer alokuje @ref tlog_writer_open(), uvolňuje
 *            @ref tlog_writer_close().
 * @ownership `dir`, `name`, `subsys_name` - pointers vlastněné callerem,
 *            writer je jen referencuje (lifetime musí přesahovat writer).
 */
typedef struct st_TLOG_WRITER
{
    /* Konfigurace (neměnné po open) */
    const char *subsys_name;     /**< "cputrack" / "iorqlog" / ... (pro msg/log) */
    char *dir;                   /**< Cílový adresář (g_strdup, vlastní writer) */
    char *name;                  /**< Basename recordingu (g_strdup, vlastní writer) */
    size_t chunk_size_bytes;     /**< chunk-mb * 1024 * 1024 */
    size_t max_total_bytes;      /**< 0 = unlimited */

    /* Runtime stav */
    uint8_t *buffer;             /**< RAM buffer velikosti chunk_size_bytes */
    size_t buffer_size;          /**< == chunk_size_bytes (cached) */
    size_t buffer_used;          /**< Počet platných bajtů v buffer[0..buffer_used-1] */
    unsigned chunk_index;        /**< Index příštího chunku (= počet hotových chunků) */
    uint64_t total_bytes_written;/**< Sum všech doposud flushnutých bajtů + buffer_used */
    int truncated;               /**< 1 = max_total_bytes dosaženo, append blokován */

    /* Per-chunk anchors pro manifest <name>.json (timestamp na startu chunku) */
    uint64_t current_chunk_start_pxclk;   /**< pxCLK total na začátku aktuálního bufferu */
    uint64_t current_chunk_start_cpuclk;  /**< CPU clk total na začátku aktuálního bufferu */
    uint32_t current_chunk_start_screens; /**< screens total na začátku aktuálního bufferu */

    /* Manifest - akumulace záznamů o chunkách (rebuild celého manifestu <name>.json
     * po každém flush). Implementováno jako pole st_TLOG_CHUNK_META. */
    GArray *chunks_meta;         /**< Pole st_TLOG_CHUNK_META */

    /* Subsys-specific header fragment (JSON), předaný při start.
     * Drží se pro automatické re-zápisy manifestu <name>.json při flush_chunk a close,
     * aby header neztratil obsah po prvním auto flush. NULL = žádný header.
     * Vlastní writer (g_strdup), uvolní se v close. */
    char *subsys_header_cache;

    /* Čtení proudu záznamů (devdoc/trace/stream-read.md) */
    size_t record_size;          /**< Velikost záznamu (tlog_writer_set_record_size, 0 = neznámá). */
    /**
     * Identita segmentu: nastaví ji tlog_writer_open() z globálního
     * čítače (> 0), close ji ponechá (popis segmentu přejde do `closed`).
     * Zapisuje jen vlákno, které writer otevírá (emu vlákno), čte se
     * atomicky (@ref tlog_writer_segment) z jiných vláken.
     */
    gint segment;
    st_TLOG_CLOSED_SEGMENT closed; /**< Poslední uzavřený segment (segment == closed.segment). */
} st_TLOG_WRITER;



/**
 * @brief Otevřít writer pro daný subsystém.
 *
 * Zalokuje RAM buffer (`chunk_mb` MB), inicializuje counter, vytvoří
 * (resp. zajistí existenci) cílový adresář. Pokud adresář nelze
 * vytvořit, vrací -1.
 *
 * @param w               Writer struktura (caller-allocated, vyplní se).
 *                        Před prvním open musí být vynulovaná (statická
 *                        proměnná nebo `= { 0 }`): open uvolní popis
 *                        předchozího uzavřeného segmentu.
 * @param subsys_name     Identifikátor subsystému (např. "cputrack");
 *                        používá se v console notifikacích a v manifestu <name>.json.
 *                        Lifetime musí přesahovat writer (typ. string literal).
 * @param dir             Cílový adresář (zkopíruje se přes g_strdup).
 * @param name            Basename (zkopíruje se přes g_strdup).
 * @param chunk_mb        Velikost chunku v MB (0 = použít default).
 * @param max_total_mb    Max total v MB (0 = unlimited).
 * @param init_pxclk      pxCLK total na startu recordingu.
 * @param init_cpuclk     CPU clk total na startu recordingu.
 * @param init_screens    screens total na startu recordingu.
 *
 * @return 0 OK, -1 error (alloc fail, mkdir fail).
 *
 * @post Při úspěchu w->buffer != NULL, w->buffer_used == 0, segment
 *       je nová identita (> 0, vyšší než všechny dřívější), popis
 *       předchozího uzavřeného segmentu je uvolněný. Při chybě je writer
 *       prázdný (segment 0).
 */
int tlog_writer_open ( st_TLOG_WRITER *w,
                       const char *subsys_name,
                       const char *dir, const char *name,
                       unsigned chunk_mb, unsigned max_total_mb,
                       uint64_t init_pxclk, uint64_t init_cpuclk,
                       uint32_t init_screens );

/**
 * @brief Připojit `n` bajtů event payloadu do bufferu.
 *
 * Pokud by append způsobil přetečení buffer_size, nejdřív se volá
 * @ref tlog_writer_flush_chunk() (synchronní disk write). Pokud je
 * writer truncated (max_total dosaženo), volání se ignoruje (tichý
 * no-op - pollerem se zjistí přes @ref tlog_writer_is_truncated()).
 *
 * @note Funkce je volaná z hot path (každý CPU instr u cputrack), proto
 *       je inline-friendly a bez bounds checks na user side.
 *
 * @param w        Writer (musí být open).
 * @param data     Payload buffer (n bytes).
 * @param n        Velikost payloadu v bytech (>= 1).
 *
 * @return 0 OK, -1 disk write error nebo truncated stop.
 */
int tlog_writer_append ( st_TLOG_WRITER *w, const void *data, size_t n );

/**
 * @brief Vynutit flush aktuálního bufferu na disk (i nezaplněného).
 *
 * Použito při zavírání recordingu, nebo při explicitním "Export now"
 * z UI. Pokud je buffer_used == 0, no-op. Po flush se buffer_used
 * resetuje na 0 a `chunk_index++`.
 *
 * Po flush se přepíše manifest <name>.json (sériový rewrite celého manifestu).
 *
 * @param w           Writer.
 * @param now_pxclk   Aktuální pxCLK total (pro aktualizaci anchoru
 *                    příštího chunku).
 * @param now_cpuclk  Aktuální CPU clk total.
 * @param now_screens Aktuální screens total.
 *
 * @return 0 OK, -1 disk write error.
 */
int tlog_writer_flush_chunk ( st_TLOG_WRITER *w,
                              uint64_t now_pxclk, uint64_t now_cpuclk,
                              uint32_t now_screens );

/**
 * @brief Začít nový chunk s danou časovou kotvou (nespojitost času).
 *
 * Neprázdný buffer zapíše jako chunk (stejně jako
 * @ref tlog_writer_flush_chunk) a nový chunk dostane předané kotvy.
 * Prázdný buffer na disk nepíše a jen přepíše kotvy aktuálního chunku
 * (žádný záznam je ještě nepoužil). Použití: subsystém, jehož čtenář
 * počítá čas záznamů z kotvy chunku (cputrack), po skoku emulovaného
 * času (seek, rewind, načtení snapshotu) nebo před přetečením chunku,
 * když kotvu zná přesněji než aktuální čas.
 *
 * @param w           Writer (otevřený).
 * @param pxclk       Kotva pxCLK total pro příští záznam.
 * @param cpuclk      Kotva CPU clk total.
 * @param screens     Kotva screens total.
 *
 * @return 0 OK, -1 writer zavřený nebo chyba zápisu chunku.
 * @post Při úspěchu buffer_used == 0 a current_chunk_start_* = předané
 *       hodnoty; manifest je přepsaný, pokud vznikl chunk.
 */
int tlog_writer_rechunk ( st_TLOG_WRITER *w,
                          uint64_t pxclk, uint64_t cpuclk, uint32_t screens );

/**
 * @brief Aktualizovat / přepsat manifest <name>.json soubor.
 *
 * Používá GLib JSON serializaci. Soubor: `<dir>/<name>.json`.
 * Volá se po každém @ref tlog_writer_flush_chunk(). Lze volat i ručně.
 *
 * @param w               Writer.
 * @param header_extra    Volitelný JSON fragment (string) přidaný do
 *                        objektu pod klíč "subsys_header" (per-subsystém
 *                        specifická pole). NULL = bez extra.
 *
 * @return 0 OK, -1 write error.
 */
int tlog_writer_update_meta ( st_TLOG_WRITER *w, const char *header_extra );

/**
 * @brief Zavřít writer, flushnout zbytek bufferu, finalizovat manifest <name>.json.
 *
 * Po close je writer nepoužitelný pro zápis (buffer dealokován).
 * Idempotentní - druhé volání = no-op. Adresář, jméno a seznam chunků
 * přejdou do popisu uzavřeného segmentu (`w->closed`), takže segment jde
 * dál číst (@ref tlog_writer_read_snapshot) až do dalšího open.
 *
 * @param w           Writer.
 * @param now_pxclk   Aktuální pxCLK total.
 * @param now_cpuclk  Aktuální CPU clk total.
 * @param now_screens Aktuální screens total.
 */
void tlog_writer_close ( st_TLOG_WRITER *w,
                         uint64_t now_pxclk, uint64_t now_cpuclk,
                         uint32_t now_screens );

/**
 * @brief Test, zda byl writer zastaven kvůli max_total limitu.
 *
 * @return 1 = truncated, 0 = stále recording.
 */
static inline int tlog_writer_is_truncated ( const st_TLOG_WRITER *w )
{
    return w->truncated;
}

/* ===========================================================================
 *  Čtení proudu záznamů (devdoc/trace/stream-read.md)
 * =========================================================================== */

/** @brief Velikost záznamu cputrack v bajtech (docs CPU-track_format.md). */
#define TLOG_RECORD_SIZE_CPUTRACK 12u

/** @brief Velikost záznamu iorqlog, intlog, hwlog a marklog v bajtech. */
#define TLOG_RECORD_SIZE_EVENT24 24u

/**
 * @brief Nastavit velikost záznamu kanálu (pevná, pro výpočet seq).
 *
 * Volá subsystém hned po úspěšném @ref tlog_writer_open(). Bez ní
 * (record_size == 0) writer čtení přes @ref tlog_writer_read_snapshot()
 * odmítne.
 *
 * @param w            Otevřený writer.
 * @param record_size  Velikost jednoho záznamu v bajtech (> 0).
 */
void tlog_writer_set_record_size ( st_TLOG_WRITER *w, size_t record_size );

/**
 * @brief Aktuální identita segmentu writeru, čtená atomicky.
 *
 * Bezpečné z libovolného vlákna (dispatch MCP ověřuje, že se mu během
 * čtení chunků z disku segment nezměnil).
 *
 * @return Segment (> 0), nebo 0 když writer nikdy nebyl otevřen.
 */
unsigned tlog_writer_segment ( const st_TLOG_WRITER *w );

/**
 * @brief Snímek segmentu writeru pro čtení (metadata + kopie z RAM).
 *
 * @invariant `ram_first_seq <= next_seq`; záznamy `[0, ram_first_seq)`
 *            leží v chuncích na disku, `[ram_first_seq, next_seq)`
 *            v RAM bufferu (u uzavřeného segmentu `ram_first_seq ==
 *            next_seq`).
 * @invariant `ram_copy` obsahuje `ram_copy_count` záznamů od
 *            `ram_copy_seq` (souvislý výřez RAM bufferu).
 * @ownership Vše vlastní snímek, uvolní @ref tlog_read_snapshot_clear().
 */
typedef struct st_TLOG_READ_SNAPSHOT
{
    unsigned segment;            /**< 0 = writer nikdy nebyl otevřen (ostatní pole prázdná). */
    int live;                    /**< 1 = writer otevřený, data přibývají. */
    int truncated;               /**< Záznam skončil na max_total_bytes. */
    size_t record_size;          /**< Velikost záznamu (0 = neznámá, čtení nejde). */
    uint64_t first_seq;          /**< Nejstarší čitelný záznam (Trace Suite 0; ring Event Vieweru next_seq - count). */
    uint64_t next_seq;           /**< Počet záznamů segmentu. */
    uint64_t ram_first_seq;      /**< První záznam v RAM bufferu. */
    uint64_t ram_start_pxclk;    /**< Kotva RAM chunku: pxCLK total na jeho začátku (jen live). */
    uint64_t ram_start_cpuclk;   /**< Kotva RAM chunku: CPU clk total na jeho začátku (jen live). */
    uint32_t ram_start_screens;  /**< Kotva RAM chunku: screens total na jeho začátku (jen live). */
    char *dir;                   /**< Adresář chunků (g_strdup). */
    char *name;                  /**< Basename chunků (g_strdup). */
    GArray *chunks;              /**< Kopie pole st_TLOG_CHUNK_META (zapsané chunky). */
    uint8_t *ram_copy;           /**< Kopie záznamů z RAM bufferu (g_malloc) nebo NULL. */
    uint64_t ram_copy_seq;       /**< seq prvního záznamu v ram_copy. */
    uint64_t ram_copy_count;     /**< Počet záznamů v ram_copy. */
} st_TLOG_READ_SNAPSHOT;

/**
 * @brief Pořídit snímek segmentu writeru a zkopírovat záznamy z RAM.
 *
 * Funguje pro otevřený writer i pro poslední uzavřený segment (popis
 * z @ref tlog_writer_close()). Kopie z RAM bufferu pokryje průnik
 * `[from_seq, from_seq + count)` s RAM částí segmentu, nejvýš
 * `max_copy_bytes` bajtů (celé záznamy).
 *
 * @param w               Writer.
 * @param from_seq        První požadovaný záznam.
 * @param count           Počet požadovaných záznamů.
 * @param max_copy_bytes  Strop kopie z RAM (0 = žádná kopie, jen metadata).
 * @param out             Výstup (přepíše se; volající ho po použití
 *                        uvolní @ref tlog_read_snapshot_clear()).
 *
 * @return 0 OK (i pro writer bez segmentu: out->segment == 0),
 *         -1 při selhání alokace.
 * @pre Volat z vlákna, které writer zapisuje (emu vlákno: drain dbgapi),
 *      mezi dvěma append.
 */
int tlog_writer_read_snapshot ( const st_TLOG_WRITER *w,
                                uint64_t from_seq, uint64_t count,
                                size_t max_copy_bytes,
                                st_TLOG_READ_SNAPSHOT *out );

/**
 * @brief Uvolnit obsah snímku (idempotentní, struct vynuluje).
 *
 * Inline, aby ji mohl použít i kód bez tlog_common.c (dispatch MCP
 * a jeho samostatný test).
 */
static inline void tlog_read_snapshot_clear ( st_TLOG_READ_SNAPSHOT *snap )
{
    g_free ( snap->dir );
    g_free ( snap->name );
    if ( snap->chunks ) g_array_free ( snap->chunks, TRUE );
    g_free ( snap->ram_copy );
    memset ( snap, 0, sizeof ( *snap ) );
}

/**
 * @brief Pomocná - vytvoří cílový adresář (s parents) pokud neexistuje.
 *
 * @param dir     Cílový adresář.
 * @return 0 OK, -1 error.
 */
int tlog_common_ensure_dir ( const char *dir );

/**
 * @brief Přičte @p n do globálního kumulativního čítače disk-bajtů trace-suite.
 *
 * Volá se z každého reálného zápisu na disk v rámci trace-suite (chunk soubor,
 * manifest <name>.json, per-subsystém initial-state dumpy), aby existoval jeden centrální
 * součet CELÉHO disk footprintu, který trace-suite zapsala. Slouží 0019
 * vrstvě 3 (byte backstop): trace_save BP akce countuje deltu tohoto čítače,
 * takže do auto-pauzy se započítá i objem chunk segmentů a initial-state
 * dumpů (ne jen hlavní index soubor - to byl undercount vektor, viz V19).
 *
 * @param n  Počet bajtů úspěšně zapsaných na disk (0 = no-op).
 *
 * Side effects: inkrement process-globálního čítače (monotónní).
 * Threading: čítač modifikován z emu vlákna (trace zápisy běží na něm); prostý
 * uint64 read/modify/write bez locku - případný race jen posune okamžik
 * accountingu, neohrozí korektnost.
 */
void tlog_common_disk_bytes_add ( uint64_t n );

/**
 * @brief Vrátí kumulativní počet disk-bajtů zapsaných trace-suite od startu
 *        procesu (resp. od posledního @ref tlog_common_disk_bytes_reset).
 *
 * Monotónně neklesající (kromě explicitního resetu). Konzument (0019 byte
 * backstop v bp_action.c) si pamatuje předchozí hodnotu a accountuje deltu.
 *
 * @return Kumulativní součet disk-bajtů trace-suite.
 */
uint64_t tlog_common_disk_bytes_total ( void );

/**
 * @brief Vynuluje kumulativní čítač disk-bajtů trace-suite.
 *
 * Určeno pro reset emulátoru a pro deterministickou izolaci testů. V produkci
 * běžně volat netřeba (konzument pracuje s deltou, ne s absolutní hodnotou).
 */
void tlog_common_disk_bytes_reset ( void );

/**
 * @brief Typ hooku volaného po každém inkrementu disk-byte čítače (0019 v3 flush-side).
 *
 * Notifikační callback, který trace vrstva (tlog_common) volá pokaždé, když
 * inkrementuje kumulativní disk-byte čítač přes @ref tlog_common_disk_bytes_add
 * (chunk swap, manifest <name>.json, initial-state dump). Slouží flush-side byte backstop
 * guardu: trace flooduje disk INKREMENTÁLNĚ po chunkách i mezi dvěma trace_save
 * BP fire, takže per-fire accounting (bp_action) by tu rostoucí stopu chytil až
 * při dalším fire. Hook umožní vyhodnotit backstop průběžně, na flush cestě.
 *
 * LAYERING: tlog_common je čistá trace vrstva a NESMÍ znát breakpoints ani
 * emulator (závislostní inverze). Proto jen vystaví tento hook; konkrétní
 * vyhodnocení prahu + auto-pauzu provádí vrstva, která breakpoints/emulator
 * už zná (bp_action.c), a hook si registruje. tlog_common hooku jen předá
 * právě přičtený počet bajtů.
 *
 * @param added  Počet bajtů, o který právě narostl kumulativní čítač.
 *
 * Threading: volán z emu vlákna (trace zápisy běží na něm), synchronně uvnitř
 * @ref tlog_common_disk_bytes_add. Hook nesmí re-entrantně volat
 * tlog_common_disk_bytes_add.
 */
typedef void ( *tlog_disk_flush_hook_fn ) ( uint64_t added );

/**
 * @brief Zaregistruje hook volaný po každém inkrementu disk-byte čítače (0019 v3).
 *
 * @param fn  Hook funkce, nebo @c NULL pro odregistraci (default = žádný hook,
 *            chování trace-suite beze změny).
 *
 * Side effects: mutuje process-globální ukazatel na hook.
 * Threading: typicky voláno při inicializaci (breakpoints_init) nebo z testu;
 * není synchronizováno - nevolat souběžně s běžící emulací používající čítač.
 */
void tlog_common_set_disk_flush_hook ( tlog_disk_flush_hook_fn fn );

/**
 * @brief Získat aktuální pxCLK total (= přes všechny screens).
 *
 * @return uint64 počet pxCLK od startu emulace.
 */
uint64_t tlog_common_get_pxclk_total ( void );

/**
 * @brief Získat aktuální screens count.
 */
uint32_t tlog_common_get_screens_total ( void );

/**
 * @brief Získat aktuální pxCLK uvnitř aktuálního screenu.
 */
uint32_t tlog_common_get_pxclk_in_screen ( void );

/**
 * @brief Získat aktuální CPU clk total.
 *
 * Odvozeno jako pxCLK total / cpu_divider (5 pro MZ-800/700/1500).
 */
uint64_t tlog_common_get_cpuclk_total ( void );

/**
 * @brief Získat platform string ("MZ-800", "MZ-700", "MZ-1500").
 *
 * Vrací konstantní string podle MZARCH compile-time konstanty.
 */
const char *tlog_common_get_platform_name ( void );

/**
 * @brief Získat pxCLK frekvenci (Hz) - typ. 17734475 pro standard config.
 */
uint32_t tlog_common_get_pxclk_freq ( void );

/**
 * @brief Získat CPU divider (= pxCLK / divider = CPU CLK).
 *
 * Pro MZ-800/700/1500 = 5 (3.546895 MHz).
 */
uint32_t tlog_common_get_cpu_divider ( void );

/**
 * @brief Získat počet pxCLK per screen (typ. 97344 pro MZ-800).
 */
uint32_t tlog_common_get_pxclk_per_screen ( void );

#ifdef __cplusplus
}
#endif

#endif /* TLOG_COMMON_H */
