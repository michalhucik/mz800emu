/**
 * @file   coverage.h
 * @brief  Pokrytí kódu: které instrukce se vykonaly, v jaké verzi a které větve se prošly.
 *
 * Jádro pokrytí bez vazby na platformu. Háček po instrukci (coverage_hook.c)
 * mu po každém kroku CPU předá st_COVERAGE_STEP: adresu, PC po kroku,
 * F a B po kroku, fyzickou buňku prvního bajtu a čtení bajtů kroku. Jádro
 * krok dekóduje (coverage_decode.h), odvodí výsledek podmínky a zapíše ho.
 *
 * @par Dvě úrovně
 * - **branch**: klíčem je logické PC. Pět bitmap po 8 KiB (vykonáno,
 *   podmíněná instrukce, podmínka splněna, nesplněna, vstup do obsluhy
 *   přerušení) a epocha prvního vykonání na adresu. Bajty kroku se čtou
 *   z paměti až po kroku (instrukce, která přepíše sama sebe nebo přepne
 *   banking pod sebou, se dekóduje podle nového obsahu).
 * - **detail**: navíc tabulka jednotek (st_COVERAGE_UNIT). Jednotka je
 *   n-tice (fyzická buňka prvního bajtu, logické PC, klíč verze kódu).
 *   Klíčem jsou bajty opkódu (prefixy, CBh/EDh a opkód; u DD/FD CB d op
 *   i poslední bajt), operand se eviduje zvlášť (poslední hodnota
 *   a příznak změny). Volba COVERAGE_KEY_FULL_BYTES dá do klíče první
 *   4 bajty kroku včetně operandu. Bajty bere háček ze záznamu fetch
 *   (jak je CPU načetlo), proto úroveň detail vynucuje logovací callbacky.
 * Úroveň detail plní i bitmapy úrovně branch.
 *
 * @par Úplnost
 * Tabulka má pevnou kapacitu. Jednotka, která se nevejde, se zahodí
 * a započte v `dropped` (existující jednotky se počítají dál). Čítače
 * jednotky saturují na 0xFFFFFFFF s příznakem COVERAGE_F_SATURATED.
 * Stav (coverage_get_status()) hlásí `complete` = nic se neztratilo,
 * bajty byly ze záznamu fetch a žádný čítač nesaturoval.
 *
 * @par Nečinný HALT
 * Jádro Z80 během HALT drží PC na adrese HALT a háček po instrukci se volá
 * každé 4 T. Krok, kdy byl CPU v HALT už před ním i po něm na téže adrese,
 * se nezapisuje (jen čítač `halt_idle`), stejně jako v profileru podle PC.
 *
 * @par Nespojitosti a epochy
 * Reset stroje, načtení snapshotu a skok v přehrávání záznamu data nemažou,
 * jen zvýší `discontinuities` (s volbou `reset_on_discontinuity` data
 * smažou). coverage_mark() zvýší epochu; jednotky a adresy si pamatují
 * epochu, ve které poprvé vznikly ("co přibylo od značky").
 *
 * @par Vlákna
 * Všechny funkce volá emu vlákno (háček po instrukci, drain dbgapi), nebo
 * když emu vlákno neběží. Modul nemá zámky; čtení z jiného vlákna jde přes
 * kopii v drainu dbgapi.
 *
 * @par Licence: GPLv3
 */

#ifndef COVERAGE_H
#define COVERAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "coverage_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Velikost bitmapy podle logického PC (1 bit na adresu, LSB = adresa & 7). */
#define COVERAGE_PC_BITMAP_BYTES 8192

/** @brief Výchozí kapacita tabulky jednotek úrovně detail. */
#define COVERAGE_DEFAULT_CAPACITY 65536u

/** @brief Nejvyšší povolená kapacita tabulky jednotek (2 Mi slotů = 64 MiB). */
#define COVERAGE_MAX_CAPACITY ( 1u << 20 )

/** @brief Nejvyšší epocha; coverage_mark() nad ní už epochu nezvyšuje. */
#define COVERAGE_EPOCH_MAX 0xFFFFu


/** @brief Úroveň pokrytí. */
typedef enum en_COVERAGE_LEVEL {
    COVERAGE_LEVEL_BRANCH = 0, /**< Bitmapy podle logického PC. */
    COVERAGE_LEVEL_DETAIL = 1, /**< Bitmapy + tabulka jednotek (buňka, PC, verze kódu). */
} en_COVERAGE_LEVEL;


/** @brief Klíč verze kódu v úrovni detail. */
typedef enum en_COVERAGE_KEY {
    COVERAGE_KEY_OPCODE = 0,     /**< Bajty opkódu; operand zvlášť (poslední hodnota, příznak změny). */
    COVERAGE_KEY_FULL_BYTES = 1, /**< První 4 bajty kroku včetně operandu (ladění samomodifikace). */
} en_COVERAGE_KEY;


/**
 * @name Příznaky jednotky a kroku (st_COVERAGE_UNIT.flags)
 * @{
 */
#define COVERAGE_F_DECISION       0x0001u /**< Podmíněná instrukce (má větve taken / not taken). */
#define COVERAGE_F_OPERAND_VARIED 0x0002u /**< Operand se mezi vykonáními měnil (klíč opcode). */
#define COVERAGE_F_INT_ENTRY      0x0004u /**< První instrukce obsluhy přijatého přerušení (INT, NMI). */
#define COVERAGE_F_CMT_TRAP       0x0008u /**< Během kroku zasáhl CMT hack (zápis paměti a AF hostitelem). */
#define COVERAGE_F_FETCH_FALLBACK 0x0010u /**< Některý bajt kroku nebyl v záznamu fetch, čten z paměti po kroku. */
#define COVERAGE_F_SATURATED      0x0020u /**< Některý čítač jednotky dosáhl 0xFFFFFFFF. */
#define COVERAGE_F_HALT           0x0040u /**< Instrukce HALT. */
#define COVERAGE_F_CELL_BUS       0x0080u /**< Buňka není fyzická (grafická VRAM, mapované porty): offset = CPU adresa. */
#define COVERAGE_F_PREFIX_SPLIT   0x0100u /**< Krok jen z 64 prefixů DD/FD (jádro řetěz rozdělilo). */
#define COVERAGE_F_KEY_TRUNCATED  0x0200u /**< Opkód nebo krok delší než 4 bajty, klíč obsahuje jen první 4. */
/** @} */

/** @brief Příznaky, které smí háček předat v st_COVERAGE_STEP.flags (původ kroku). */
#define COVERAGE_F_STEP_MASK ( COVERAGE_F_INT_ENTRY | COVERAGE_F_CMT_TRAP | COVERAGE_F_CELL_BUS )


/**
 * @brief Fyzická buňka: prostor (horních 8 bitů) a offset v prostoru (dolních 24 bitů).
 *
 * Prostor má význam en_RAMLOG_SPACE (RAM, VRAM, PCG, MemExt, ROM); jádro
 * pokrytí ho nevykládá.
 */
#define COVERAGE_CELL( space, offset ) ( ( (uint32_t) ( space ) << 24 ) | ( (uint32_t) ( offset ) & 0x00FFFFFFu ) )
/** @brief Prostor z hodnoty COVERAGE_CELL. */
#define COVERAGE_CELL_SPACE( cell ) ( (uint8_t) ( ( cell ) >> 24 ) )
/** @brief Offset z hodnoty COVERAGE_CELL. */
#define COVERAGE_CELL_OFFSET( cell ) ( ( cell ) & 0x00FFFFFFu )

/** @brief Prostor buňky bez fyzického cíle (grafická VRAM MZ-800, mapované porty): offset = CPU adresa. */
#define COVERAGE_SPACE_BUS 0xFFu


/**
 * @brief Jednotka úrovně detail: jedna verze kódu na jedné buňce a adrese (32 B).
 *
 * @invariant `insn_len` >= 1 u obsazené jednotky; 0 = volný slot.
 * @invariant `taken` + `not_taken` <= `exec` (bez saturace); u jednotky bez
 *            COVERAGE_F_DECISION jsou oba nulové.
 * @invariant 1 <= `key_len` <= 4.
 */
typedef struct st_COVERAGE_UNIT {
    uint32_t cell;          /**< Fyzická buňka prvního bajtu (COVERAGE_CELL). */
    uint16_t pc;            /**< Logická adresa začátku kroku. */
    uint8_t key_len;        /**< Počet platných bajtů v `key`. */
    uint8_t insn_len;       /**< Délka kroku v bajtech (prefixy + instrukce). */
    uint8_t key[ 4 ];       /**< Klíč verze kódu (bajty opkódu, nebo první bajty kroku). */
    uint16_t operand_last;  /**< Poslední operand (první dva bajty operandu, little-endian; 0 bez operandu). */
    uint16_t flags;         /**< Příznaky COVERAGE_F_*. */
    uint32_t exec;          /**< Počet vykonání (bez nečinného HALT). */
    uint32_t taken;         /**< Kolikrát byla podmínka splněna. */
    uint32_t not_taken;     /**< Kolikrát nebyla podmínka splněna. */
    uint16_t next_pc_last;  /**< PC po posledním vykonání (u RET, JP (HL), RST dává cíl). */
    uint16_t first_epoch;   /**< Epocha, ve které jednotka vznikla. */
} st_COVERAGE_UNIT;


/** @brief Nastavení sběru. */
typedef struct st_COVERAGE_CONFIG {
    en_COVERAGE_LEVEL level;     /**< Úroveň. */
    en_COVERAGE_KEY key;         /**< Klíč verze kódu (jen detail). */
    uint32_t capacity;           /**< Kapacita tabulky jednotek (jen detail), 1..COVERAGE_MAX_CAPACITY. */
    bool reset_on_discontinuity; /**< Reset, snapshot, rewind data smažou (jinak jen čítač). */
} st_COVERAGE_CONFIG;


/**
 * @brief Jeden krok CPU, jak ho předává háček po instrukci.
 *
 * @note `read` musí vracet bajty kroku od jeho začátku; úroveň detail je
 *       bere ze záznamu fetch, branch z paměti po kroku. Když háček pro
 *       některý bajt sáhne do paměti místo záznamu fetch, nastaví
 *       `*fallback` = true (může až během čtení, jádro ho čte až po
 *       dekódování).
 */
typedef struct st_COVERAGE_STEP {
    uint16_t pc;                  /**< Adresa začátku kroku. */
    uint16_t next_pc;             /**< PC po kroku (před přijetím přerušení). */
    uint8_t f;                    /**< Registr F po kroku. */
    uint8_t b;                    /**< Registr B po kroku. */
    bool halted;                  /**< cpu->halted po kroku. */
    uint32_t cell;                /**< Fyzická buňka prvního bajtu (COVERAGE_CELL), jen detail. */
    uint16_t flags;               /**< Původ kroku, podmnožina COVERAGE_F_STEP_MASK. */
    coverage_decode_read_fn read; /**< Čtení bajtů kroku. */
    void *read_ctx;               /**< Kontext pro `read`. */
    bool *fallback;               /**< Výstup čtení: bajt mimo záznam fetch (smí být NULL). */
} st_COVERAGE_STEP;


/** @brief Stav sběru a úplnost dat. */
typedef struct st_COVERAGE_STATUS {
    bool active;                  /**< Sběr běží. */
    bool has_data;                /**< Data existují (od startu, i po stopu). */
    en_COVERAGE_LEVEL level;      /**< Úroveň dat. */
    en_COVERAGE_KEY key;          /**< Klíč verze kódu (detail). */
    bool reset_on_discontinuity;  /**< Volba z konfigurace. */
    bool complete;                /**< dropped == 0 && fetch_fallback == 0 && !saturated. */
    uint32_t units;               /**< Detail: obsazené jednotky; branch: počet vykonaných adres. */
    uint32_t capacity;            /**< Kapacita tabulky jednotek (branch: 0). */
    uint64_t dropped;             /**< Kroky, jejichž nová jednotka se nevešla. */
    uint64_t fetch_fallback;      /**< Kroky s bajtem mimo záznam fetch (detail). */
    bool saturated;               /**< Některý čítač saturoval. */
    uint32_t discontinuities;     /**< Reset, načtení snapshotu, skok přehrávání. */
    uint32_t host_state_edits;    /**< Zásahy hostitele do PC/F/paměti kódu mezi instrukcemi. */
    uint32_t epoch;               /**< Aktuální epocha (coverage_mark()). */
    uint32_t generation;          /**< Generace dat: zvýší ji každé smazání nebo nové založení dat (kurzor čtení). */
    uint64_t instructions;        /**< Zapsané kroky (bez nečinného HALT). */
    uint64_t halt_idle;           /**< Kroky nečinného HALT. */
    uint32_t decisions;           /**< Adresy s podmíněnou instrukcí (bitmapa). */
    uint32_t branches_both;       /**< Z nich prošly obě větve. */
    uint32_t branches_taken_only; /**< Jen podmínka splněna. */
    uint32_t branches_not_taken_only; /**< Jen podmínka nesplněna. */
} st_COVERAGE_STATUS;


/**
 * @brief Kopie dat pokrytí z jednoho okamžiku (coverage_snapshot_take()).
 *
 * Asi 170 KiB; pole `units` alokuje coverage_snapshot_take() přes malloc
 * a uvolní ho coverage_snapshot_clear().
 */
typedef struct st_COVERAGE_SNAPSHOT {
    st_COVERAGE_STATUS status;                     /**< Stav v okamžiku kopie. */
    uint8_t exec[ COVERAGE_PC_BITMAP_BYTES ];      /**< Vykonané adresy. */
    uint8_t decision[ COVERAGE_PC_BITMAP_BYTES ];  /**< Adresy s podmíněnou instrukcí. */
    uint8_t taken[ COVERAGE_PC_BITMAP_BYTES ];     /**< Podmínka splněna. */
    uint8_t not_taken[ COVERAGE_PC_BITMAP_BYTES ]; /**< Podmínka nesplněna. */
    uint8_t int_entry[ COVERAGE_PC_BITMAP_BYTES ]; /**< První instrukce obsluhy přerušení. */
    uint16_t first_epoch[ 0x10000 ];               /**< Epocha prvního vykonání adresy. */
    st_COVERAGE_UNIT *units;                       /**< Obsazené jednotky (v pořadí slotů), NULL bez detail. */
    uint32_t n_units;                              /**< Počet položek v `units`. */
} st_COVERAGE_SNAPSHOT;


/** @brief Nenulové = sběr běží (bit MZARCH_POST_INSN_COVERAGE masky háčků). */
extern int g_coverage_active;


/**
 * @brief Spustí sběr.
 *
 * Když data neexistují, nebo se liší úroveň, klíč či kapacita, nebo je
 * @p reset true, data se založí znovu (prázdná, epocha 0). Jinak sběr
 * pokračuje do stávajících dat.
 *
 * @param cfg   Nastavení; kapacita se ořízne do 1..COVERAGE_MAX_CAPACITY.
 * @param reset true = začít s prázdnými daty.
 * @return true = sběr běží; false = nepodařila se alokace (sběr neběží,
 *         předchozí data zůstala).
 * @post Při úspěchu g_coverage_active = 1. Masku háčků ani callbacky CPU
 *       nemění (to dělá coverage_hook_start()).
 */
bool coverage_start ( const st_COVERAGE_CONFIG *cfg, bool reset );

/** @brief Zastaví sběr; data zůstanou. @post g_coverage_active = 0. */
void coverage_stop ( void );

/** @brief Smaže data (bitmapy, tabulku, čítače, epochu); nastavení a stav běhu zůstanou. */
void coverage_reset ( void );

/** @brief Zastaví sběr a uvolní data (konec emulátoru). */
void coverage_free ( void );

/**
 * @brief Zvýší epochu (značka "od teď").
 * @return Nová epocha (nejvýš COVERAGE_EPOCH_MAX).
 */
uint32_t coverage_mark ( void );

/**
 * @brief Nespojitost běhu (reset stroje, načtení snapshotu, skok přehrávání).
 *
 * Zvýší `discontinuities`, s `reset_on_discontinuity` data smaže. Bez dat
 * nedělá nic.
 */
void coverage_note_discontinuity ( void );

/** @brief Zásah hostitele mezi instrukcemi (změna PC nebo F debuggerem); jen čítač. */
void coverage_note_host_state_edit ( void );

/**
 * @brief Zapíše jeden krok CPU.
 *
 * @param s Krok; `read` se volá jen pro bajty, které dekódování a klíč
 *          potřebují.
 * @pre g_coverage_active (volá ho háček po instrukci).
 */
void coverage_on_step ( const st_COVERAGE_STEP *s );

/**
 * @brief Vyplní stav sběru (počty větví spočítá z bitmap).
 * @param out Výstup.
 */
void coverage_get_status ( st_COVERAGE_STATUS *out );

/**
 * @brief Zkopíruje bitmapy úrovně branch.
 *
 * Každý ukazatel smí být NULL (bitmapa se nekopíruje); jinak buffer
 * COVERAGE_PC_BITMAP_BYTES bajtů. Bez dat se vyplní nulami.
 *
 * @param exec      Vykonané adresy.
 * @param decision  Adresy s podmíněnou instrukcí.
 * @param taken     Adresy, kde byla podmínka splněna.
 * @param not_taken Adresy, kde nebyla podmínka splněna.
 * @param int_entry Adresy první instrukce obsluhy přerušení.
 */
void coverage_get_bitmaps ( uint8_t *exec, uint8_t *decision, uint8_t *taken,
                            uint8_t *not_taken, uint8_t *int_entry );

/**
 * @brief Epocha prvního vykonání adresy (úroveň branch i detail).
 * @param pc Logická adresa.
 * @return Epocha; 0 i u nevykonané adresy (rozliší bitmapa exec).
 */
uint16_t coverage_pc_first_epoch ( uint16_t pc );

/**
 * @brief Zkopíruje epochy prvního vykonání všech adres.
 * @param out Výstup, 65536 položek (index = logická adresa).
 */
void coverage_get_first_epochs ( uint16_t *out );

/**
 * @brief Zkopíruje obsazené jednotky úrovně detail (v pořadí slotů).
 *
 * @param out Výstup, nejvýš @p max jednotek; NULL = jen spočítat.
 * @param max Kapacita @p out.
 * @return Počet obsazených jednotek (může být > @p max, zkopíruje se jen @p max).
 */
uint32_t coverage_copy_units ( st_COVERAGE_UNIT *out, uint32_t max );

/**
 * @brief Zkopíruje stav, bitmapy, epochy a volitelně jednotky do snímku.
 *
 * @param out        Snímek; předchozí obsah se přepíše (pole `units`
 *                   předchozí kopie musí být uvolněné,
 *                   coverage_snapshot_clear()).
 * @param with_units false = bez jednotek úrovně detail (out->units NULL,
 *                   n_units 0); okna GUI tak obnovují značky bez kopie
 *                   tabulky, která může mít až 32 MiB.
 * @return false = nepodařila se alokace pole jednotek (out->units NULL,
 *         n_units 0, ostatní data vyplněná).
 */
bool coverage_snapshot_take ( st_COVERAGE_SNAPSHOT *out, bool with_units );

/**
 * @brief Uvolní pole jednotek snímku (strukturu samotnou ne).
 * @param s Snímek (smí být NULL).
 */
void coverage_snapshot_clear ( st_COVERAGE_SNAPSHOT *s );

/**
 * @brief Stav větve podmíněné instrukce jako text.
 *
 * @param decision Instrukce je podmíněná.
 * @param taken    Počet (nebo příznak) splnění.
 * @param not_taken Počet (nebo příznak) nesplnění.
 * @return "none" (nepodmíněná), "unobserved" (podmíněná, žádný výsledek),
 *         "taken_only", "not_taken_only" nebo "both"; statický řetězec.
 */
const char *coverage_branch_state_name ( bool decision, uint32_t taken, uint32_t not_taken );

/**
 * @brief Jméno prostoru buňky.
 * @param space Prostor: hodnota en_RAMLOG_SPACE (0 RAM, 1 VRAM, 2 PCG,
 *              3 MemExt, 4 ROM; pořadí hlídá coverage_hook.c) nebo
 *              COVERAGE_SPACE_BUS.
 * @return "ram", "vram", "pcg", "memext", "rom", "bus" nebo "unknown"; statický řetězec.
 */
const char *coverage_space_name ( uint8_t space );

/**
 * @brief Prostor podle jména (coverage_space_name()).
 * @param name Jméno.
 * @param out  Výstup.
 * @return false = neznámé jméno.
 */
bool coverage_space_parse ( const char *name, uint8_t *out );

#ifdef __cplusplus
}
#endif

#endif /* COVERAGE_H */
