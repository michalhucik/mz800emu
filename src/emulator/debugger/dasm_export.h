/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file dasm_export.h
 * @brief Auto-label scanner pro disassembler V1 + export support.
 *
 * Modul nezávislý na UI vrstvě - implementuje dvouprůchodný algoritmus
 * pro detekci cílů skoků, volání a paměťových referencí v daném
 * rozsahu adres. Pro každý takový cíl vygeneruje auto-label s konvencí
 * S/L/D/W (Subroutine / Label / Data / Warn).
 *
 * Konvence pojmenování (sjednoceno na lowercase hex; mzdisasm
 * konvenci se nepodařilo dohledat v ~/projects/mz800-knowledge/tools/
 * = adresář prázdný):
 *
 *   S<addr>  - subroutine target (CALL nn / CALL cc,nn / RST n)
 *   L<addr>  - jump target (JP nn / JP cc,nn / JR e / JR cc,e / DJNZ e)
 *   D<addr>  - data reference (LD A,(nn) / LD (nn),A / LD HL,(nn) ...)
 *   W<addr>  - warning: cíl padá doprostřed jiné instrukce (= SMC nebo
 *              chybná disassemblace - bývá indikátor problému)
 *
 *   <addr> = 4-místné lowercase hex (např. "Sc020", "Lc004", "Dd000",
 *            "Wc021").
 *
 * Priorita při konfliktu typů na stejné adrese: WARN > JUMP > SUBROUTINE >
 * DATA (= L vyhrává nad S protože re-entry point má vyšší kontextovou
 * důležitost, WARN je vždy override jako indikátor chyby).
 *
 * Per-call gating (klíčové pro V1 architekturu):
 *   - symdb parametr nullable → NULL = "neaktivovat sym_db lookup"
 *   - cdl parametr nullable → NULL = "neaktivovat CDL/mhmap" (V1 F2/F3
 *     vždy NULL, doplní F8)
 *
 * Per-call gating je vědomá designová volba: snapshot z jiného běhu
 * nebo ROM monitor + herní symboly = false hits, takže defaultně
 * "konzultovat všechno automaticky" je špatně.
 *
 * Threading: jen UI vlákno (čte přes user-supplied read callback,
 * nemodifikuje globální stav).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef DASM_EXPORT_H
#define DASM_EXPORT_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include <stdint.h>

#include "libs/dasm-z80/z80_dasm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Typ auto-labelu (= rovnou ASCII písmeno pro snadný prefix
 *        při generování jména).
 *
 * Hodnoty jsou ASCII kódy 'S' / 'L' / 'D' / 'W' - umožňuje přímé
 * embedding do snprintf("%c%04x", type, addr) bez switch().
 *
 * @c DASM_LABEL_NONE je nehodnota (= "žádný label"), používá se jako
 * sentinel v interních map strukturách.
 */
typedef enum {
    DASM_LABEL_NONE       = 0,    /**< Žádný label (sentinel). */
    DASM_LABEL_SUBROUTINE = 'S',  /**< CALL / RST target. */
    DASM_LABEL_JUMP       = 'L',  /**< JP / JR / DJNZ target. */
    DASM_LABEL_DATA       = 'D',  /**< LD (nn) / LD r,(nn) target. */
    DASM_LABEL_WARN       = 'W',  /**< Cíl padá doprostřed jiné instrukce. */
} dasm_label_type_t;

/**
 * @brief Jeden auto-label.
 *
 * Vlastnictví: pole je celé naplněno @ref dasm_export_collect_labels
 * do callerem-vlastněného out bufferu; caller pole nealokuje na heap.
 *
 * @invariant @c name je vždy null-terminated.
 * @invariant @c type != DASM_LABEL_NONE u všech zapsaných záznamů.
 *
 * F8: pole @c is_data_zone + @c data_zone_end přidána na konec struktury
 * (= backward compat s F2-F7 callery které je nečtou). Význam:
 *
 *   - @c is_data_zone == false: běžný code label (S/L/D/W konvence)
 *   - @c is_data_zone == true: label otevírá data zónu (= rozsah bajtů
 *     které mhmap detekoval jako "čteno, neexekutováno" = data, ne kód).
 *     @c data_zone_end pak drží poslední adresu zóny (inclusive).
 *
 * Data-zone labely vytváří Pass 3 v @ref dasm_export_collect_labels
 * jen pokud caller předá ne-NULL @c cdl parametr (= F8 aktivováno).
 * Default jméno je @c "D%04x" (lowercase hex) - stejný prefix jako
 * běžný @c DASM_LABEL_DATA, ale @c is_data_zone == true funkčně
 * odlišuje data zónu od pouhé "referenced data adresy".
 */
typedef struct {
    uint16_t          addr;        /**< Adresa cíle (= adresa labelu). */
    char              name[32];    /**< Pojmenování (lowercase hex prefix
                                        nebo sym_db jméno pokud
                                        from_symdb == true). */
    dasm_label_type_t type;        /**< S/L/D/W klasifikace. */
    bool              from_symdb;  /**< true = jméno pochází ze sym_db,
                                        false = auto-generated. */
    bool              is_data_zone;   /**< F8: true = label otevírá data
                                            zónu (rendering jako DB ...).
                                            Default false (= běžný code
                                            label). */
    uint16_t          data_zone_end;  /**< F8: poslední adresa data zóny
                                            (inclusive). Platné jen pokud
                                            @c is_data_zone == true. */
} dasm_label_t;

/**
 * @brief Snapshot mhmap counter flagů pro per-call CDL pohled.
 *
 * F8: vlastní pohled na CDL flagy, který @ref dasm_export_collect_labels
 * konzumuje pro detekci data zón. Buffer je copy-on-create (= odpojený
 * snapshot, ne reference na živý @c g_mhmap) - dasm_export tedy nemůže
 * narazit na lock contention s emu vláknem.
 *
 * Konvence flagů (klasický CDL bitmap): hodnota 1 = "byl alespoň jeden
 * přístup daného typu zaznamenán", hodnota 0 = "žádný přístup".
 *
 * Vlastnictví: pole @c exec_flag / @c read_flag / @c write_flag jsou
 * malloc-alokované buffery velikosti @c (to - from + 1). Caller je
 * uvolňuje přes @c mhmap_view_destroy() (definováno v mhmap modulu).
 *
 * Pro standalone unit testy: struct se dá vyrobit ručně bez mhmap
 * závislosti - test si alokuje buffery sám, předá pointer na @c
 * dasm_export_collect_labels a po skončení uvolní.
 *
 * @invariant @c from <= @c to.
 * @invariant Velikost každého flag bufferu = @c (to - from + 1) bajtů.
 * @invariant @c exec_flag != NULL, @c read_flag != NULL,
 *            @c write_flag != NULL (= žádný nullable; pokud snapshot
 *            má 0 flagů, buffer je vyplněn nulami).
 */
typedef struct mhmap_view {
    uint16_t  from;          /**< Počáteční adresa snapshotu (inclusive). */
    uint16_t  to;            /**< Koncová adresa snapshotu (inclusive). */
    uint8_t  *exec_flag;     /**< Per-byte flag X (= byl spuštěn). */
    uint8_t  *read_flag;     /**< Per-byte flag R (= byl čten jako data). */
    uint8_t  *write_flag;    /**< Per-byte flag W (= byl zapisován). */
} mhmap_view_t;

/**
 * @brief Bezpečný čtecí helper pro CDL execute flag.
 *
 * Vrací 0 mimo rozsah @c view->from..view->to (= "nemáme info"). Pro
 * caller pohodlí - umožní volat bez bounds checku.
 *
 * @param view mhmap snapshot. Nesmí být NULL.
 * @param addr CPU adresa.
 * @return 1 pokud byl bajt v rámci snapshotu spuštěn, jinak 0.
 */
static inline uint8_t cdl_byte_is_exec(const mhmap_view_t *view, uint16_t addr)
{
    if (addr < view->from || addr > view->to) return 0;
    return view->exec_flag[addr - view->from];
}

/**
 * @brief Bezpečný čtecí helper pro CDL read flag.
 *
 * @param view mhmap snapshot. Nesmí být NULL.
 * @param addr CPU adresa.
 * @return 1 pokud byl bajt v rámci snapshotu čten jako data, jinak 0.
 */
static inline uint8_t cdl_byte_is_read(const mhmap_view_t *view, uint16_t addr)
{
    if (addr < view->from || addr > view->to) return 0;
    return view->read_flag[addr - view->from];
}

/**
 * @brief Bezpečný čtecí helper pro CDL write flag.
 *
 * @param view mhmap snapshot. Nesmí být NULL.
 * @param addr CPU adresa.
 * @return 1 pokud byl bajt v rámci snapshotu zapisován, jinak 0.
 */
static inline uint8_t cdl_byte_is_write(const mhmap_view_t *view, uint16_t addr)
{
    if (addr < view->from || addr > view->to) return 0;
    return view->write_flag[addr - view->from];
}

/**
 * @brief Projde rozsah a vygeneruje auto-labely + případně sym_db hits.
 *
 * Algoritmus (třícestný):
 *
 *   Pass 1: namapuje všechny start adresy instrukcí v rozsahu
 *           (přes opakovaná z80_dasm() volání s posunem o instrukční
 *           délku). Pole inst_start[from..to] = true znamená "tato
 *           adresa je začátek nějaké instrukce".
 *
 *   Pass 2: pro každou instrukci zjistí @c z80_dasm_target_addr().
 *           Pokud target spadá do rozsahu [from..to]:
 *             - klasifikuje typ podle @c z80_flow_type_t
 *             - pokud target není v inst_start = mid-instruction →
 *               type = WARN
 *             - vyřeší konflikt s případným existujícím labelem na
 *               stejné adrese (priorita W > L > S > D)
 *             - pokud @p symdb != NULL, pokusí se najít sym_db jméno;
 *               pokud nalezeno, přepíše name + nastaví from_symdb=true,
 *               ale type zůstává (= WARN se nikdy neztrácí jako
 *               vizuální indikátor)
 *
 *   Pass 3 (F8, jen pokud @p cdl != NULL): scan rozsahu pro detekci
 *           data zón = souvislých rozsahů kde @c cdl_byte_is_exec ==
 *           false && @c cdl_byte_is_read == true. Každá taková zóna
 *           dostane vlastní @c dasm_label_t s @c is_data_zone == true
 *           a @c data_zone_end nastaveným na poslední adresu. Render
 *           ve @ref dasm_export_write pak zónu emituje jako DB direktivy
 *           místo disasmu.
 *
 * Per-call gating: pokud @p symdb == NULL, sym_db lookup se nikdy
 * neaktivuje a všechny labely jsou auto-generated. Stejně pro @p cdl -
 * NULL = "neaktivovat CDL/mhmap konzultaci", celý rozsah jako kód.
 *
 * @param[in]  from       Počáteční adresa rozsahu (inclusive).
 * @param[in]  to         Koncová adresa rozsahu (inclusive).
 * @param[in]  read_fn    Callback pro čtení paměti. Nesmí být NULL.
 * @param[in]  read_ud    User data pro read_fn (může být NULL).
 * @param[in]  symdb      Symtab z bridge nebo NULL = sym_db disabled.
 * @param[in]  cdl        CDL/mhmap snapshot nebo NULL = disabled. F8
 *                        aktivuje data-zone detekci jen pokud != NULL.
 * @param[out] out        Caller-vlastněné pole, naplní se labely.
 *                        Nesmí být NULL.
 * @param[in]  max_labels Kapacita @p out pole. Musí být > 0.
 * @return Počet zapsaných labelů (0 až max_labels), nebo -1 při chybě
 *         (read_fn==NULL nebo out==NULL nebo max_labels<=0).
 *
 * @pre read_fn != NULL
 * @pre out != NULL
 * @pre max_labels > 0
 * @post Pokud @p from > @p to, vrátí 0 (= prázdný rozsah, není chyba).
 * @post Vrácený počet <= max_labels.
 * @post Žádný side effect mimo zápis do @p out.
 *
 * @note Funkce alokuje dočasné pole pro inst_start mapu (velikost
 *       rozsahu). Pro nejhorší případ 64 KB rozsahu = 64 KB heap;
 *       je to OK pro UI vlákno (single-shot per Disassemble click).
 *
 * @note @p out pole musí mít kapacitu pro pesimistický odhad - v
 *       praxi typicky 1-3 % instrukcí má jump/call/data target.
 *       Doporučená velikost: alespoň 2048 záznamů pro běžný kód.
 */
int dasm_export_collect_labels(
    uint16_t from, uint16_t to,
    z80_dasm_read_fn read_fn, void *read_ud,
    const z80_symtab_t *symdb,
    const mhmap_view_t *cdl,
    dasm_label_t *out, int max_labels);


/**
 * @brief Cílový assembler pro export disassembly.
 *
 * Per-dialekt liší se:
 *   - hex zápis (pasmo "0CEh", sjasmplus "$CE", sdcc "0xCE")
 *   - direktivy (ORG / .org, EQU / .equ, DB / .byte)
 *   - terminator labelu (":" / "::") - sdcc "::" pro global
 *   - IX/IY syntax (Zilog "(IX+d)" / Motorola "(d,IX)" pro sdcc)
 *   - default soubor extension (.asm / .s)
 *
 * V1 F5 implementuje @ref DASM_DIALECT_PASMO a @ref DASM_DIALECT_SJASMPLUS.
 * F6 doplní sdcc-asz80. Render loop @ref dasm_export_write je
 * dialect-agnostic - jen čte fieldy z interní @c dialect_spec_t
 * struktury (file-static v dasm_export.c).
 */
typedef enum {
    DASM_DIALECT_PASMO        = 0,  /**< pasmo (Julian Albo). */
    DASM_DIALECT_SJASMPLUS    = 1,  /**< sjasmplus ($ hex prefix). */
    DASM_DIALECT_SDCC_ASZ80   = 2,  /**< sdcc as-z80 (F6 - zatím stub). */
} dasm_dialect_t;


/**
 * @brief Volby formátu exportu.
 *
 * Caller naplní strukturu a předá do @ref dasm_export_write. Všechny
 * fieldy mají jednoznačný význam, žádné magic defaultní hodnoty -
 * caller je zodpovědný za explicitní nastavení.
 *
 * @c generate_equ_for_syms je v V1 F4 ignorován (collect_labels v
 * F2/F3 generuje labely jen pro in-range cíle, EQU sekce by zůstala
 * prázdná). Pole zachováno pro budoucí rozšíření (V1.5+).
 */
typedef struct {
    dasm_dialect_t dialect;                /**< Cílový assembler. */
    bool           include_org;            /**< Emit ORG direktivu na začátku. */
    bool           include_bytes_comment;  /**< Emit komentář s opcode bytes za mnemonikou. */
    bool           generate_equ_for_syms;  /**< V1 F4 ignored; rezervováno V1.5+. */
    bool           uppercase_mnemonics;    /**< true = LD A,B / false = ld a,b. */
} dasm_export_opts_t;


/**
 * @brief Chybové kódy exportu disassembly.
 *
 * F7: rozšíření z původního 0/-1 návratu @ref dasm_export_write na
 * granularní typy chyb. UI vrstva je mapuje na user-friendly hlášky
 * (otevření souboru, write fail, neplatný rozsah, atd.).
 *
 * @c DASM_EXPORT_ERR_OUT_OF_MEMORY je rezervován pro alokační selhání
 * dočasných bufferů (z80_symtab, string sink). V praxi při běžné
 * velikosti rozsahu (max 64 KB) k němu nedochází.
 */
typedef enum {
    DASM_EXPORT_OK = 0,                  /**< Úspěch. */
    DASM_EXPORT_ERR_OPEN_FAIL,           /**< fopen selhal (permission, neexistuje cesta). */
    DASM_EXPORT_ERR_WRITE_FAIL,          /**< fwrite/fprintf selhal (disk full, IO error). */
    DASM_EXPORT_ERR_INVALID_RANGE,       /**< from > to nebo NULL pointer v povinném parametru. */
    DASM_EXPORT_ERR_UNSUPPORTED_DIALECT, /**< @ref pick_dialect_spec vrátil NULL. */
    DASM_EXPORT_ERR_OUT_OF_MEMORY,       /**< Alokace dočasného bufferu selhala. */
} dasm_export_result_t;


/**
 * @brief Zapíše assembler-ready disassembly do souboru.
 *
 * Sekvence (per-dialekt přes interní @c dialect_spec_t):
 *   1) Header komentář (tool, range, dialect name).
 *   2) ORG direktiva (pokud @c opts->include_org).
 *   3) Body: pro každou instrukci v rozsahu
 *      - pokud na adrese existuje label v @p labels, emit "<name><sep>"
 *      - mnemonic přes @c z80_dasm_to_str_sym() s lokálním symtabem
 *        sestaveným z @p labels (= substituce hex cílů jmény)
 *      - volitelně bytes komentář ("; XX YY ZZ")
 *
 * V1 F4 neemitujeme EQU sekci (collect_labels neobsahuje out-of-range
 * cíle).
 *
 * @param[in] path      Cílový soubor (UTF-8). Existující se přepíše.
 * @param[in] from      Adresa rozsahu (inclusive).
 * @param[in] to        Adresa rozsahu (inclusive).
 * @param[in] read_fn   Memory read callback. Nesmí být NULL.
 * @param[in] read_ud   User data pro @p read_fn (může být NULL).
 * @param[in] labels    Pole z @ref dasm_export_collect_labels. Může
 *                      být NULL pokud @p n_labels == 0.
 * @param[in] n_labels  Počet platných záznamů v @p labels.
 * @param[in] opts      Volby formátu. Nesmí být NULL.
 *
 * @return Hodnota @ref dasm_export_result_t (F7 rozšíření). Pro
 *         zachování zpětné kompatibility s F4-F6 callers: 0 = OK,
 *         != 0 = error. UI vrstva F7 inspekt přímo enum hodnotu pro
 *         friendly hlášku.
 *
 * @pre opts != NULL
 * @pre read_fn != NULL
 * @pre from <= to (jinak return @ref DASM_EXPORT_ERR_INVALID_RANGE)
 * @post Soubor je při OK návratu kompletně zapsán a uzavřen.
 * @post Při chybě (!= OK) soubor buď neexistuje (open fail), nebo
 *       existuje s částečným obsahem - caller by ho měl smazat.
 *
 * @note Funkce alokuje dočasný @c z80_symtab_t a interní hex buffery
 *       na zásobníku. Žádná long-running alokace.
 */
int dasm_export_write(
    const char *path,
    uint16_t from, uint16_t to,
    z80_dasm_read_fn read_fn, void *read_ud,
    const dasm_label_t *labels, int n_labels,
    const dasm_export_opts_t *opts);


/**
 * @brief Vyrenderuje assembler-ready disassembly do dynamicky alokovaného
 *        řetězce (clipboard cesta).
 *
 * F7: alternativa @ref dasm_export_write která místo @c FILE* zapisuje
 * do paměti. Použito pro "Copy to clipboard" akci v Disassembler okně
 * (@c ImGui::SetClipboardText přijímá @c const @c char*).
 *
 * Implementace sdílí render core s @ref dasm_export_write přes interní
 * sink abstrakci (FILE* / dynamic buffer). Output formát i obsah jsou
 * identické s file exportem stejných parametrů.
 *
 * @param[in]  from      Adresa rozsahu (inclusive).
 * @param[in]  to        Adresa rozsahu (inclusive).
 * @param[in]  read_fn   Memory read callback. Nesmí být NULL.
 * @param[in]  read_ud   User data pro @p read_fn (může být NULL).
 * @param[in]  labels    Pole z @ref dasm_export_collect_labels. Může
 *                       být NULL pokud @p n_labels == 0.
 * @param[in]  n_labels  Počet platných záznamů v @p labels.
 * @param[in]  opts      Volby formátu. Nesmí být NULL.
 * @param[out] out_str   Pointer na proměnnou kam se uloží malloc-alokovaný
 *                       null-terminated řetězec. Nesmí být NULL.
 *                       Caller vlastní (musí @c free()) - i při OK návratu.
 *                       Při chybě je nastaven na NULL.
 *
 * @return @ref dasm_export_result_t. OK = řetězec alokován v
 *         @p *out_str. ERR_OUT_OF_MEMORY = malloc selhal nebo realloc
 *         během růstu bufferu selhal.
 *
 * @pre out_str != NULL
 * @pre opts != NULL
 * @pre read_fn != NULL
 * @pre from <= to
 *
 * @note Ownership: caller @c free() na @p *out_str. Při ERR_OUT_OF_MEMORY
 *       je @p *out_str == NULL (nic k free není).
 */
int dasm_export_to_string(
    uint16_t from, uint16_t to,
    z80_dasm_read_fn read_fn, void *read_ud,
    const dasm_label_t *labels, int n_labels,
    const dasm_export_opts_t *opts,
    char **out_str);


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DASM_EXPORT_H */
