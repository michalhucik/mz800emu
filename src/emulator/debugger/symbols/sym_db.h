/*
 * File:   sym_db.h
 *
 * Symbol DB pro debugger (D.8). Načítá symboly z buildchain output
 * (SDCC NoICE / sdldz80 map / sjasmplus .sym) + user write-back
 * annotace v `.lbl` formátu. Disassembler poté zobrazuje jména
 * místo hex adres.
 *
 * Datový model:
 *   - dynamic array (linear scan při lookup, OK pro <1000 symbolů)
 *   - každý záznam má source priority (LBL > MAP > NOI > SYM)
 *   - lookup_by_addr() vrací symbol s nejvyšší prioritou pro daný addr
 *   - při násobném load se nepřepisuje existující entry, pokud
 *     novější má nižší prioritu
 *
 * Banking awareness V1:
 *   - bank_id 0 = "CPU view" (= globální jméno bez ohledu na banking)
 *   - bank_id != 0 = explicit bank scope (jen pro .lbl uživatel
 *     může nastavit; importéry zatím vždy dávají 0)
 *   - pełná banking-aware resolva = V1.5
 *
 * Threading:
 *   - load/save/iterace = jen UI vlákno
 *   - lookup z disassembleru = jen UI vlákno (čte)
 *   - žádný přístup z hot path EMU vlákna - sym_db NENÍ čteno
 *     v cpu_step ani BP enforce
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef SYM_DB_H
#define SYM_DB_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../dbgapi_cmdrq.h"


/**
 * @brief Zdroj symbolu (= odkud byl naimportován).
 *
 * Hodnoty jsou stabilní (= persistence v .lbl výstupu pro round-trip
 * není nutná - .lbl ukládá vždy LBL prioritní záznamy).
 *
 * Priorita pro lookup_by_addr je SYM_SOURCE_LBL > MAP > NOI > SYM.
 * Vyšší enum hodnota == vyšší priorita (= jednoduché srovnání).
 */
typedef enum en_SYM_SOURCE {
    SYM_SOURCE_SJASMPLUS = 0,    /**< sjasmplus .sym (LABEL EQU value) */
    SYM_SOURCE_NOI       = 1,    /**< NoICE .noi (DEF name 0xADDR) - sdldz80 */
    SYM_SOURCE_MAP       = 2,    /**< sdldz80 .map linker map (incl. module info) */
    SYM_SOURCE_LBL       = 3     /**< user .lbl write-back (highest priority) */
} en_SYM_SOURCE;


/**
 * @brief Záznam jednoho symbolu.
 *
 * Vlastnictví:
 *   - name, comment = heap (g_strdup), uvolňuje sym_db_clear/destroy
 *   - lifetime záznamu: do clear/destroy nebo remove
 *
 * @invariant name je non-NULL, neprázdný (po vytvoření).
 *            comment může být NULL.
 *            addr je 16-bit CPU adresa nebo (V1.5) physical address.
 *            bank_id 0 = CPU view, jinak explicit bank scope.
 */
typedef struct st_SYMBOL {
    char         *name;        /**< Identifier (heap-allocated) */
    uint32_t      addr;        /**< CPU 16-bit nebo physical (V1.5) */
    uint8_t       bank_id;     /**< 0 = CPU view, jinak bank scope */
    en_SYM_SOURCE source;      /**< Odkud byl symbol naimportován */
    char         *comment;     /**< Volitelný komentář (jen .lbl), nebo NULL */
    char         *module;      /**< Volitelně module/source file (jen .map), nebo NULL */
    /**
     * @brief V1.C.3 - kdo symbol vytvořil (audit + cooperative UX).
     *
     * Default DBGAPI_CMD_ORIGIN_USER (= 0). Pole je metadata - žádný
     * runtime efekt na lookup ani render disassembleru. Setter
     * sym_db_set_cmd_origin() umožní dispatcheru přepsat na MCP origin.
     */
    en_DBGAPI_CMD_ORIGIN cmd_origin;
} st_SYMBOL;


/**
 * @brief Inicializuje DB. Idempotentní.
 *
 * Po volání je DB prázdná, připravená na load.
 */
void sym_db_init ( void );


/**
 * @brief Uvolní storage. Po volání je nutno znovu volat init().
 */
void sym_db_destroy ( void );


/**
 * @brief Smaže všechny záznamy, ponechá DB ve fungujícím stavu.
 */
void sym_db_clear ( void );


/**
 * @brief Načte SDCC NoICE format soubor (.noi).
 *
 * Format: per řádek "DEF <name> 0x<hex>"
 *
 * @param path  cesta k souboru
 * @return počet načtených symbolů, nebo -1 při chybě
 */
int sym_db_load_noi ( const char *path );


/**
 * @brief Načte sdldz80 linker map (.map).
 *
 * Parser čte sekce "Global Defined In Module"; navíc extrahuje
 * module name jako optional comment hint.
 *
 * @return počet načtených symbolů, nebo -1 při chybě
 */
int sym_db_load_map ( const char *path );


/**
 * @brief Načte .sym soubor (sjasmplus i pasmo formát).
 *
 * Format: per řádek "<NAME> EQU <value>". Hex notace přes prefix
 * ($NNNN / 0xNNNN - sjasmplus styl) nebo suffix (NNNNH - pasmo styl).
 * Decimal hodnoty bez prefixu/suffixu.
 *
 * @return počet načtených symbolů, nebo -1 při chybě
 */
int sym_db_load_sym ( const char *path );


/**
 * @brief Načte user write-back .lbl soubor.
 *
 * Format line-based:
 *   # comment-only line (skip)
 *   <addr_hex>  <name>  <bank>  ; per-symbol comment after ;
 *
 * @return počet načtených symbolů, nebo -1 při chybě
 */
int sym_db_load_lbl ( const char *path );


/**
 * @brief Serializuje SYM_SOURCE_LBL záznamy do .lbl souboru.
 *
 * Záznamy z jiných zdrojů (NOI/MAP/SYM) se neserializují - .lbl
 * je zamýšlen jen jako user-driven persistence komentářů a
 * vlastních labelů.
 *
 * @return 0 při úspěchu, -1 při chybě
 */
int sym_db_save_lbl ( const char *path );


/**
 * @brief Auto-detekce formátu dle suffixu souboru.
 *
 * Mapování: ".noi" -> NOI, ".map" -> MAP, ".sym" -> SYM, ".lbl" -> LBL.
 *
 * @return počet načtených symbolů, nebo -1 při chybě / neznámý formát
 */
int sym_db_load_auto ( const char *path );


/* ===================================================================== */
/*  Default .lbl persistence (V1.7+ 3.2, debugger-fixes-5)                */
/* ===================================================================== */

/**
 * @brief Inicializuje cfg sekci [SYMBOLS] a případně auto-loadne .lbl.
 *
 * Registruje 3 cfg klíče:
 *   - lbl_file (TEXT, per-arch default: "mz800.lbl" / "mz1500.lbl" /
 *     "mz700.lbl") - cesta relativní k cfg_dir nebo absolutní
 *   - auto_load (BOOL, default 1) - načíst .lbl při startu
 *   - auto_save (BOOL, default 1) - uložit .lbl při exit
 *
 * Pokud je auto_load nastavený, pokusí se načíst default LBL soubor
 * (mlčky tolerantní k chybějícímu souboru = při prvním startu).
 *
 * Volat PO sym_db_init(). Idempotentní - opakované volání nepřinese
 * další registrace.
 */
void sym_db_cfg_init ( void );


/**
 * @brief Auto-save default .lbl pokud je auto_save zapnutý.
 *
 * Volat PŘED sym_db_destroy() (typicky v debugger_at_exit). Neudělá nic
 * pokud auto_save = 0 nebo není inicializovaný default_file.
 */
void sym_db_cfg_auto_save ( void );


/**
 * @brief Vrátí aktuální default LBL cestu (= cfg klíč lbl_file).
 *
 * @return pointer do cfg storage (lifetime = do exit), nikdy NULL po
 *         úspěšném sym_db_cfg_init.
 */
const char *sym_db_get_default_lbl_file ( void );


/**
 * @brief Nastaví novou default LBL cestu (= aktualizuje cfg klíč).
 *
 * Pouze in-memory - persist proběhne při typickém cfgmain save.
 *
 * @param path  nová cesta (relativní k cfg_dir nebo absolutní),
 *              NULL nebo prázdný string = no-op
 */
void sym_db_set_default_lbl_file ( const char *path );


/**
 * @brief Vrátí stav auto_load flagu (0/1).
 */
int sym_db_get_auto_load_lbl ( void );


/**
 * @brief Nastaví auto_load flag (0/1).
 */
void sym_db_set_auto_load_lbl ( int enable );


/**
 * @brief Vrátí stav auto_save flagu (0/1).
 */
int sym_db_get_auto_save_lbl ( void );


/**
 * @brief Nastaví auto_save flag (0/1).
 */
void sym_db_set_auto_save_lbl ( int enable );


/**
 * @brief Načte default .lbl (resolved přes sdlapp_paths) do aktuální DB.
 *
 * Helper pro UI - akce "Load default .lbl now".
 *
 * @return počet načtených symbolů (>=0) nebo -1 při chybě / nenastavená
 *         default_file
 */
int sym_db_load_default_lbl ( void );


/**
 * @brief Uloží aktuální LBL symboly do default .lbl (resolved cesta).
 *
 * Helper pro UI - akce "Save default .lbl now".
 *
 * @return 0 při úspěchu, -1 při chybě / nenastavená default_file
 */
int sym_db_save_default_lbl ( void );


/**
 * @brief Vyhledá symbol podle jména (case-sensitive).
 *
 * @return pointer na záznam nebo NULL. Lifetime = do clear/destroy/remove.
 */
const st_SYMBOL *sym_db_lookup_by_name ( const char *name );


/**
 * @brief Vyhledá symbol s nejvyšší prioritou pro daný addr v daném banku.
 *
 * Pokud existuje LBL záznam, vrátí ten; jinak MAP, jinak NOI, jinak SYM.
 *
 * Filtrace bank_id:
 *   - bank_id 0 ve volání = "CPU view" pohled, vrátí jen záznamy
 *     s bank_id == 0 (= globální symbols)
 *   - bank_id != 0 ve volání = vrátí záznam s bank_id == volané bank,
 *     fallback na bank_id == 0 pokud žádný explicitní není
 *
 * @return pointer na záznam s nejvyšší prioritou, nebo NULL
 */
const st_SYMBOL *sym_db_lookup_by_addr ( uint32_t addr, uint8_t bank_id );


/**
 * @brief Počet záznamů v DB (= velikost iterace).
 */
size_t sym_db_count ( void );


/**
 * @brief Verze obsahu DB (= monotonní čítač změn).
 *
 * Inkrementuje se při každé úspěšné modifikaci DB:
 *   - sym_db_clear() (= velký reset, počítá se vždy 1x)
 *   - úspěšný load_noi/map/sym/lbl/auto (= 1x na soubor)
 *   - sym_db_set_comment / add_user_label / remove_user_label (= 1x každé)
 *
 * Init/destroy verzi nemění (= žádné semantické změny obsahu).
 *
 * Slouží pro cache invalidaci v UI vrstvě (např. mostový build
 * z80_symtab_t v dbg_disassembled.cpp - rebuild jen když verze
 * neodpovídá). Hodnota 0 = "ještě nikdy nemodifikováno" (= prázdná
 * DB). Wrap-around po 2^32 změnách je teoreticky možný, ale prakticky
 * neexistuje (debugger session nikdy 4 mld změn).
 *
 * @return aktuální verze (monotonně rostoucí)
 *
 * @note Threading: stejná pravidla jako u count/lookup - jen UI vlákno.
 */
unsigned sym_db_get_version ( void );


/**
 * @brief Iterace - vrátí záznam na indexu (0..count-1).
 *
 * Pořadí záznamů je insertion-order (= ne tříděné). Pro tříděnou
 * iteraci si UI vlákno udělá vlastní index array.
 *
 * @return pointer na záznam, nebo NULL při idx >= count
 */
const st_SYMBOL *sym_db_get_by_index ( size_t idx );


/**
 * @brief Opaque iterator state pro lineární scan DB.
 *
 * Wrapper nad existujícím insertion-order storage; zapouzdřuje index
 * pro idiomatickou iteraci (init -> next-loop). Pattern záměrně mimicry
 * @c GHashTableIter z GLib (= konzistentní look-and-feel s ostatními
 * GLib-backed strukturami v projektu).
 *
 * Lifetime: iterator je platný dokud se DB nemodifikuje (= clear/add/
 * remove iterátor invaliduje). Stejná thread-safety pravidla jako pro
 * @ref sym_db_count / @ref sym_db_get_by_index - jen UI vlákno.
 *
 * @field next_idx  Příští index k vrácení (interní, neměnit ručně).
 */
typedef struct {
    size_t next_idx;
} st_SYM_DB_ITER;


/**
 * @brief Inicializuje iterátor na začátek DB.
 *
 * Po volání další @ref sym_db_iter_next() vrátí první záznam (idx 0)
 * pokud DB není prázdná.
 *
 * @param iter  Pointer na uživatelem vlastněnou strukturu iterátoru.
 *              NULL = no-op.
 */
void sym_db_iter_init ( st_SYM_DB_ITER *iter );


/**
 * @brief Vrátí další záznam v insertion-order a posune iterátor.
 *
 * Volat opakovaně, dokud nevrátí @c NULL (= konec iterace).
 *
 * Iterátor MUSÍ být inicializován přes @ref sym_db_iter_init() PŘED
 * prvním voláním. Volání nad neinicializovaným iterátorem nebo nad
 * mezitím modifikovanou DB je nedefinované chování (= invalidates).
 *
 * @param iter  Iterátor z @ref sym_db_iter_init() (NULL = vrátí NULL).
 * @return Pointer na záznam (lifetime do nejbližší modifikace DB),
 *         nebo @c NULL po posledním záznamu.
 */
const st_SYMBOL *sym_db_iter_next ( st_SYM_DB_ITER *iter );


/**
 * @brief Nastaví/aktualizuje komentář existujícího symbolu.
 *
 * Pokud symbol nebyl SYM_SOURCE_LBL, automaticky se promotuje na
 * LBL (= write-back persistence). Pokud nový komentář je prázdný
 * řetězec nebo NULL, comment se vyčistí (nastaví na NULL).
 *
 * @return 0 při úspěchu, -1 pokud symbol neexistuje
 */
int sym_db_set_comment ( const char *name, const char *new_comment );


/**
 * @brief Přidá user-defined label (LBL source).
 *
 * Pokud symbol stejného jména existuje, je přepsán (= overwrite name
 * mapping; addr+comment+source se aktualizuje na LBL).
 *
 * @param addr     CPU 16-bit nebo physical
 * @param name     identifier (non-NULL, neprázdný)
 * @param comment  volitelný komentář (může být NULL nebo "")
 * @return 0 při úspěchu, -1 při chybě (NULL name apod.)
 */
int sym_db_add_user_label ( uint32_t addr, const char *name,
                            const char *comment );


/**
 * @brief Odstraní symbol podle jména.
 *
 * @return 0 při úspěchu, -1 pokud neexistoval
 */
int sym_db_remove_user_label ( const char *name );


/**
 * @brief V1.C.3 - nastaví owner attribution pro symbol s daným jménem.
 *
 * Volá ho dispatcher v dbgapi.c po úspěšném sym_db_add_user_label(),
 * aby přiřadil symbolu origin volajícího klienta. Pole je metadata -
 * žádný runtime efekt na lookup ani render.
 *
 * @param name    Jméno symbolu (NULL/prázdné = no-op, vrátí -1).
 * @param origin  Hodnota en_DBGAPI_CMD_ORIGIN.
 * @return 0 při úspěchu, -1 pokud symbol s daným jménem neexistuje.
 */
int sym_db_set_cmd_origin ( const char *name, en_DBGAPI_CMD_ORIGIN origin );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* SYM_DB_H */
