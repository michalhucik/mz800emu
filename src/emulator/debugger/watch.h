/*
 * File:   watch.h
 *
 * Watch panel - storage + parse + persist pro user-defined paměťové
 * hlídky (Watch V1).
 *
 * Phase A (L0) = MVP: jen typ `WATCH_TYPE_U8`, hex display.
 * Phase B (L1) = více typů + display formátů.
 * Phase C (L2) = výrazové módy + Save/Load (tento soubor).
 *
 * Datový model:
 *   - každý řádek hlídá konkrétní paměťovou adresu (0-0xFFFF) v CPU view,
 *     nebo (Phase C) výraz vyhodnocený přes `bp_expr` engine
 *   - `mode` určuje sémantiku řádku:
 *       * `WATCH_MODE_ADDRESS`     - literal `addr` + `type` (= L0/L1)
 *       * `WATCH_MODE_EXPR_SCALAR` - `expr` se vyhodnotí jako int32, type
 *         určuje display format (žádný memory read kromě toho co dělá výraz
 *         sám interně)
 *       * `WATCH_MODE_EXPR_DEREF`  - `expr` se vyhodnotí jako uint16 adresa,
 *         z té se pak čte podle `type` (analogicky ADDRESS, jen adresa je
 *         dynamická)
 *   - `type` určuje šířku a interpretaci (u8/i8/u16le/be/i16le/be/
 *     u32le/be/i32le/be/bit/ascii/mzascii/bytes)
 *   - `fmt` určuje display sub-format (dec/hex/bin/char), platný jen
 *     pro int typy (`watch_type_has_format()` rozliší)
 *   - `length` = počet bajtů u variabilních typů (ascii/mzascii/bytes),
 *     1-256; pro fixed-size typy ignorováno
 *   - `bit_index` = pozice bitu (0-7) pro typ `WATCH_TYPE_BIT`
 *   - `bank`: -1 = current banking (= aktuální view), >= 0 = explicit bank
 *     (v L0/L1 vždy -1; banking-aware Watch je Phase C+)
 *   - `name` je volitelné (NULL nebo prázdný řetězec = anonymní řádek)
 *   - `expr_text` / `expr_ast`: pro mode == ADDRESS jsou NULL; pro EXPR
 *     módy nese původní text výrazu + cached parsed AST (vlastněno řádkem)
 *
 * Persistence:
 *   - per-arch JSON soubor (`mz800.watch` / `mz700.watch` / `mz1500.watch`),
 *     auto_save při exit + auto_load při start (cfg sekce `[WATCH_PANEL]`)
 *   - schema Phase B rozšířeno o `fmt` / `length` / `bit_index` (volitelné)
 *   - schema Phase C rozšířeno o `mode` / `expr` (volitelné, chybějící =
 *     mode=address, expr=NULL = backward compat s Phase A/B soubory)
 *   - parsovaný AST se neukládá - reparse z `expr` při load
 *   - pattern přesně jako u `bp_vars` (bp_vars.c / bp_vars.h)
 *
 * Threading:
 *   - storage je vlastněn UI vláknem (= CRUD jen z UI), EMU vlákno nečte
 *   - read hodnoty (`watch_read_int`, `watch_read_bytes`) volá
 *     `memory_read_byte_no_se()` z UI vlákna - akceptovaný stale-frame
 *     race s EMU vláknem (= UI může zobrazit hodnotu zachycenou uprostřed
 *     instrukce, pattern jako u io_window). Race-free čtení by vyžadovalo
 *     dotažení dbgapi MEM_READ STUB handleru, což je mimo scope V1.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef WATCH_H
#define WATCH_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "dbgapi_cmdrq.h"


/**
 * @brief Maximální délka uživatelského jména řádku (bez terminátoru).
 *
 * Limit kontroluje `watch_add()` (truncate při překročení).
 */
#define WATCH_NAME_MAX 63


/**
 * @brief Minimální délka pole pro typy s parametrem délky.
 */
#define WATCH_LENGTH_MIN 1


/**
 * @brief Maximální délka pole pro typy ascii / mzascii.
 *
 * Volba: 64 bajtů je rozumný strop pro inline display + tooltip.
 * Pro `WATCH_TYPE_BYTES` lze jít až do `WATCH_LENGTH_MAX_BYTES`.
 */
#define WATCH_LENGTH_MAX_STRING 64


/**
 * @brief Maximální délka pole pro typ `WATCH_TYPE_BYTES`.
 */
#define WATCH_LENGTH_MAX_BYTES 256


/**
 * @brief Typ hodnoty hlídané na adrese.
 *
 * Enum hodnoty + jejich pořadí jsou stabilní (= mapování na persistence
 * řetězce v `watch_type_to_str()` / `watch_type_from_str()`).
 *
 * Default při `watch_add()` = `WATCH_TYPE_U8`.
 */
typedef enum en_WATCH_TYPE {
    WATCH_TYPE_U8 = 0,        /**< Unsigned 8-bit */
    WATCH_TYPE_I8,            /**< Signed 8-bit */
    WATCH_TYPE_U16LE,         /**< Unsigned 16-bit little-endian */
    WATCH_TYPE_U16BE,         /**< Unsigned 16-bit big-endian */
    WATCH_TYPE_I16LE,         /**< Signed 16-bit little-endian */
    WATCH_TYPE_I16BE,         /**< Signed 16-bit big-endian */
    WATCH_TYPE_U32LE,         /**< Unsigned 32-bit little-endian */
    WATCH_TYPE_U32BE,         /**< Unsigned 32-bit big-endian */
    WATCH_TYPE_I32LE,         /**< Signed 32-bit little-endian */
    WATCH_TYPE_I32BE,         /**< Signed 32-bit big-endian */
    WATCH_TYPE_BIT,           /**< Jeden bit (`bit_index` 0-7) */
    WATCH_TYPE_ASCII,         /**< ASCII řetězec, N bajtů (`length`) */
    WATCH_TYPE_MZASCII,       /**< Sharp MZ ASCII řetězec, N bajtů (`length`) */
    WATCH_TYPE_BYTES,         /**< Hex dump N bajtů (`length`) */
} en_WATCH_TYPE;


/**
 * @brief Display sub-format pro integer typy.
 *
 * Platí jen pro `WATCH_TYPE_U8` / `I8` / `U16LE` / `U16BE` / `I16LE` /
 * `I16BE` / `U32LE` / `U32BE` / `I32LE` / `I32BE`. Pro ostatní (`BIT`,
 * `ASCII`, `MZASCII`, `BYTES`) se ignoruje (= `watch_type_has_format()`
 * vrátí false).
 *
 * Default při `watch_add()`:
 *   - unsigned typy (u8/u16/u32): `WATCH_FMT_HEX`
 *   - signed typy (i8/i16/i32): `WATCH_FMT_DEC`
 *   - ostatní: `WATCH_FMT_HEX` (nepoužije se)
 */
typedef enum en_WATCH_FMT {
    WATCH_FMT_DEC = 0,        /**< Decimal (signed pro i*, unsigned pro u*) */
    WATCH_FMT_HEX,            /**< Hex `0xNN` */
    WATCH_FMT_BIN,            /**< Binary `0b10101010` */
    WATCH_FMT_CHAR,           /**< Char `'A'` (jen pro u8; multibyte = hex fallback) */
} en_WATCH_FMT;


/**
 * @brief Mód řádku - jak se získává hodnota.
 *
 * Stabilní mapování na persistence strings (`watch_mode_to_str` /
 * `watch_mode_from_str`).
 *
 * `WATCH_MODE_ADDRESS` = legacy L0/L1 chování (literal addr + type).
 * `WATCH_MODE_EXPR_SCALAR` = výraz se vyhodnotí jako int32, výsledek
 *  se interpretuje podle `type` jen pro DISPLAY (žádný extra memory read).
 *  Vhodné jen pro int typy; pro ascii/mzascii/bytes je výsledek 0.
 * `WATCH_MODE_EXPR_DEREF` = výraz se vyhodnotí jako uint16 adresa, z té
 *  se pak čte podle `type` analogicky jako u ADDRESS módu.
 */
typedef enum en_WATCH_MODE {
    WATCH_MODE_ADDRESS = 0,   /**< Literal `addr` + `type` (L0/L1) */
    WATCH_MODE_EXPR_SCALAR,   /**< Výraz -> scalar, `type` = display fmt */
    WATCH_MODE_EXPR_DEREF,    /**< Výraz -> uint16 addr, read podle `type` */
} en_WATCH_MODE;


/* Opaque forward decl - aby header nemusel includovat bp_expr.h. */
struct st_BP_EXPR;
typedef struct st_BP_EXPR bp_expr_t;


/**
 * @brief Záznam jednoho watch řádku.
 *
 * Vlastnictví:
 *   - `name` a `expr_text` jsou heap-allocated (g_strdup) nebo NULL
 *   - uvolňuje `watch_clear_storage()` / `watch_remove()` / `watch_shutdown()`
 *
 * @invariant `addr` je 16-bit CPU adresa (0..0xFFFF).
 * @invariant `bank` je -1 (= current banking) nebo >= 0 (explicit, ne L0/L1).
 * @invariant `type` je platná hodnota `en_WATCH_TYPE`.
 * @invariant `fmt` je platná hodnota `en_WATCH_FMT` (ignorováno pokud
 *            `watch_type_has_format(type) == false`).
 * @invariant `length` je 1..`WATCH_LENGTH_MAX_BYTES` pokud
 *            `watch_type_has_length(type) == true`, jinak 0.
 * @invariant `bit_index` je 0..7 pokud `type == WATCH_TYPE_BIT`,
 *            jinak 0.
 * @invariant `mode` je platná hodnota `en_WATCH_MODE`.
 * @invariant Pro `mode == WATCH_MODE_ADDRESS` jsou `expr_text` a
 *            `expr_ast` vždy NULL.
 * @invariant Pro `mode == WATCH_MODE_EXPR_*` je `expr_text` non-NULL
 *            (alokovaný heap). `expr_ast` je non-NULL pokud parse
 *            posledně proběhl OK, jinak NULL (+ `expr_error` obsahuje
 *            chybovou hlášku).
 * @invariant `expr_error` je NULL pokud parser neselhal, jinak heap-
 *            alokovaný popis chyby.
 * @invariant `id` je >= 1, monotonní counter, unikátní pro každý řádek
 *            v daném programovém běhu. Nepersistuje se (= po load se
 *            přidělí znovu). Slouží jako stabilní klíč pro UI selection
 *            přes mutace storage (delete/move).
 */
typedef struct st_WATCH_ROW {
    int           id;          /**< Stabilní runtime ID (monotonní counter, nepersistuje se) */
    char         *name;        /**< Volitelné jméno (NULL = anonymní) */
    uint16_t      addr;        /**< CPU 16-bit adresa (jen mode=ADDRESS) */
    int           bank;        /**< -1 = current, >= 0 = explicit bank (Phase C+) */
    en_WATCH_MODE mode;        /**< Mód řádku (Address / Expr-scalar / Expr-deref) */
    en_WATCH_TYPE type;        /**< Typ hodnoty */
    en_WATCH_FMT  fmt;         /**< Display sub-format (jen pro int typy) */
    uint16_t      length;      /**< Délka v B (jen ascii/mzascii/bytes) */
    uint8_t       bit_index;   /**< Pozice bitu 0-7 (jen WATCH_TYPE_BIT) */
    char         *expr_text;   /**< Text výrazu (NULL pro mode=ADDRESS) */
    bp_expr_t    *expr_ast;    /**< Cached parsed AST (NULL = parse error nebo ADDRESS) */
    char         *expr_error;  /**< Heap kopie poslední parse error (NULL = OK) */
    /**
     * @brief V1.C.3 - kdo watch řádek vytvořil. V1.E.6.B - persistuje se
     *        do .watch JSON jako stable string token.
     *
     * Default DBGAPI_CMD_ORIGIN_USER (= 0, dosazené přes calloc / memset).
     * Dispatcher v dbgapi.c po úspěšném watch_add() přepíše hodnotu na
     * rq->cmd_origin. Persistence v .watch JSON přes stable tokeny
     * "user"/"mcp"/"test"/"internal"; tolerant load (chybějící klíč nebo
     * unknown token = USER fallback) zachovává backward kompat.
     */
    en_DBGAPI_CMD_ORIGIN cmd_origin;
} st_WATCH_ROW;


/* ===================================================================== */
/*  Lifecycle                                                            */
/* ===================================================================== */


/**
 * @brief Inicializuje storage. Idempotentní.
 *
 * Volat z `debugger_init()` před prvním load souboru. Po volání je
 * storage prázdný a připravený pro `watch_add()`.
 */
extern void watch_init ( void );


/**
 * @brief Uvolní storage. Po volání je nutno volat `watch_init()` znovu.
 *
 * Volat z `debugger_exit()` po posledním save.
 */
extern void watch_shutdown ( void );


/**
 * @brief Smaže všechny záznamy (= storage je po volání prázdný, ale
 *        zůstává inicializovaný).
 *
 * Použití: load `.watch` souboru = nejprve clear, pak append.
 */
extern void watch_clear_storage ( void );


/* ===================================================================== */
/*  CRUD                                                                 */
/* ===================================================================== */


/**
 * @brief Přidá nový watch řádek na konec storage.
 *
 * Default: `type = WATCH_TYPE_U8`, `fmt = WATCH_FMT_HEX`, `length = 0`,
 * `bit_index = 0`, `expr_text = NULL`. Caller upraví field-by-field přes
 * `watch_set_type()` / `watch_set_fmt()` / atd. po insertu.
 *
 * @param name  Volitelné jméno (NULL nebo prázdný = anonymní řádek).
 *              Storage si dělá vlastní g_strdup kopii. Truncate na
 *              `WATCH_NAME_MAX` znaků.
 * @param addr  CPU 16-bit adresa.
 * @param bank  -1 = current banking (default), >= 0 = explicit bank.
 *              V L0/L1 očekáváno vždy -1.
 * @return Index nově přidaného řádku (0..count-1) nebo -1 při chybě
 *         (= storage neinicializovaný).
 *
 * Side effects: alokace heap pro `name` (pokud non-NULL).
 *               Storage je dynamic array - pointer na předchozí záznamy
 *               se může realokací invalidovat (= nedržet `st_WATCH_ROW *`
 *               přes add/remove/clear).
 */
extern int watch_add ( const char *name, uint16_t addr, int bank );


/**
 * @brief Přidá nový watch řádek ve výrazovém módu.
 *
 * Volá se z UI při Add s nastaveným EXPR_SCALAR nebo EXPR_DEREF módem.
 * Parsuje `expr_text` přes `bp_expr_parse`; pokud parse selže, řádek se
 * stejně vloží (s expr_ast = NULL + expr_error nastaveným) - user pak v UI
 * uvidí červený `!parse: ...` text a může výraz upravit přes
 * `watch_set_expr`.
 *
 * @param name       Volitelné jméno (NULL/prázdný = anonymní).
 * @param expr_text  Text výrazu (NULL nebo prázdný = chyba, vrátí -1).
 * @param mode       `WATCH_MODE_EXPR_SCALAR` nebo `WATCH_MODE_EXPR_DEREF`.
 *                   Pro `WATCH_MODE_ADDRESS` se chová jako `watch_add(name,
 *                   0, -1)` + warning na stderr.
 * @param type       Typ hodnoty. Pro EXPR_SCALAR doporučeno int typ; pro
 *                   string/bytes vrátí value reader 0 (= "N/A").
 * @return Index nově přidaného řádku, nebo -1 při chybě (storage
 *         neinicializovaný / expr_text NULL).
 *
 * Side effects: alokace heap pro `name`, `expr_text`, případně `expr_ast`
 * a `expr_error` (drží storage).
 */
extern int watch_add_expr ( const char *name, const char *expr_text,
                             en_WATCH_MODE mode, en_WATCH_TYPE type );


/**
 * @brief Změní výraz / mód existujícího řádku.
 *
 * Uvolní starý `expr_ast` / `expr_error` / `expr_text`, parsuje nový a
 * uloží. Mode lze přepnout mezi `EXPR_SCALAR` a `EXPR_DEREF` (oba pracují
 * s výrazem).
 *
 * Volání s `mode == WATCH_MODE_ADDRESS` přepne řádek zpět na address mód
 * (= vyčistí expr_text/ast/error a nechá `addr` field, který caller už
 * měl nastavit přes `watch_add` / pomocný setter); `expr_text` argument se
 * v tomto případě ignoruje.
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op (return false).
 * @param expr_text  Text výrazu (NULL nebo prázdný = chyba).
 * @param mode       Nový mód.
 * @return true při úspěchu (= mode update + expr_text alokace + parse
 *         attempt; parse error sám o sobě NENÍ false-return, jen
 *         expr_ast == NULL + expr_error). false pokud index out-of-range
 *         nebo (pro EXPR módy) expr_text == NULL.
 */
extern bool watch_set_expr ( int row_index, const char *expr_text,
                              en_WATCH_MODE mode );


/**
 * @brief Vrátí poslední parse error pro EXPR řádek (nebo NULL pokud
 *        nejde o EXPR řádek nebo je výraz validní).
 *
 * @param row  Řádek (NULL = NULL).
 * @return Konstantní pointer do storage (lifetime = lifetime řádku),
 *         NULL pokud nic k hlášení.
 */
extern const char *watch_get_expr_error ( const st_WATCH_ROW *row );


/**
 * @brief Vyhodnotí výraz řádku jako signed int32.
 *
 * Použití pro EXPR_SCALAR display (= int hodnota se pak formátuje podle
 * `type` + `fmt`).
 *
 * Threading: bezpečné z UI vlákna. Vnitřně sestaví `bp_expr_ctx_t`
 * s aktuálním CPU stavem (`g_emulator.cpu`) a globálními counters; ctx
 * dědí `side_effect = false` chování přes `bp_read_byte_no_se` cestu.
 *
 * @param row  Řádek (mode musí být EXPR_*; ADDRESS / invalid = 0).
 * @return Hodnota výrazu nebo 0 při chybě (= unparsed AST / NULL row).
 */
extern int32_t watch_eval_expr ( const st_WATCH_ROW *row );


/**
 * @brief Vyhodnotí výraz řádku a vrátí uint16 adresu (= dolních 16 bitů).
 *
 * Použití pro EXPR_DEREF mode = vyhodnoť pointer, pak čti `type` z té
 * adresy.
 *
 * @param row  Řádek.
 * @return uint16 adresa.
 */
extern uint16_t watch_eval_expr_as_addr ( const st_WATCH_ROW *row );


/**
 * @brief Odstraní řádek podle indexu.
 *
 * Ostatní řádky se posunou v underlying GArray (= invaliduje předchozí
 * `st_WATCH_ROW *` pointery).
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op.
 *
 * Side effects: g_free pro `name`/`expr_text` field záznamu.
 */
extern void watch_remove ( int row_index );


/**
 * @brief Přejmenuje řádek (nastaví `name`).
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op.
 * @param new_name   Nové jméno (NULL nebo prázdný = clear jméno = anonymní).
 *                   Storage si dělá vlastní kopii. Truncate na
 *                   `WATCH_NAME_MAX` znaků.
 *
 * Side effects: g_free pro stávající `name`, g_strdup pro nový.
 */
extern void watch_rename ( int row_index, const char *new_name );


/**
 * @brief Přesune řádek na nový index (drag-reorder).
 *
 * Pokud `row_index == new_index` nebo některý je out-of-range, no-op.
 *
 * @param row_index  Aktuální index (0..count-1).
 * @param new_index  Cílový index (0..count-1).
 */
extern void watch_move ( int row_index, int new_index );


/**
 * @brief Nastaví typ řádku a aplikuje default fmt podle typu.
 *
 * Pro unsigned int typy default `WATCH_FMT_HEX`, pro signed
 * `WATCH_FMT_DEC`, pro string/bytes/bit fmt se nepoužije.
 *
 * Pro variabilní typy (ascii/mzascii/bytes) nastaví `length` na
 * `WATCH_LENGTH_MIN` pokud je aktuálně 0; jinak nechá.
 *
 * Pro `WATCH_TYPE_BIT` nemění `bit_index` (caller nastaví zvlášť).
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op.
 * @param type       Nový typ.
 */
extern void watch_set_type ( int row_index, en_WATCH_TYPE type );


/**
 * @brief Nastaví display sub-format řádku.
 *
 * Pokud `watch_type_has_format(row->type) == false`, no-op.
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op.
 * @param fmt        Nový formát.
 */
extern void watch_set_fmt ( int row_index, en_WATCH_FMT fmt );


/**
 * @brief Nastaví délku pole pro variabilní typy (ascii/mzascii/bytes).
 *
 * Pokud `watch_type_has_length(row->type) == false`, no-op.
 * Hodnota se clampuje na povolený rozsah (1..MAX dle typu).
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op.
 * @param length     Nová délka v bajtech.
 */
extern void watch_set_length ( int row_index, uint16_t length );


/**
 * @brief Nastaví bit index pro `WATCH_TYPE_BIT`.
 *
 * Pokud `row->type != WATCH_TYPE_BIT`, no-op.
 * Hodnota se clampuje na 0..7.
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op.
 * @param bit_index  Pozice bitu (0-7).
 */
extern void watch_set_bit_index ( int row_index, uint8_t bit_index );


/**
 * @brief V1.C.3 - nastaví owner attribution pro daný řádek.
 *
 * Slouží pro dispatcher v dbgapi.c, aby po úspěšném watch_add() /
 * watch_add_expr() přiřadil řádku origin volajícího klienta (USER /
 * MCP / TEST / INTERNAL). Hodnota je pouze metadata - žádný runtime
 * efekt na evaluaci watch.
 *
 * @param row_index  Index 0..count-1. Out-of-range = no-op.
 * @param origin     Hodnota enum en_DBGAPI_CMD_ORIGIN.
 */
extern void watch_set_cmd_origin ( int row_index, en_DBGAPI_CMD_ORIGIN origin );


/* ===================================================================== */
/*  Enumerace                                                            */
/* ===================================================================== */


/**
 * @brief Počet aktuálně registrovaných řádků.
 *
 * @return Počet (0 pokud storage neinicializovaný nebo prázdný).
 */
extern size_t watch_count ( void );


/**
 * @brief Vrátí read-only ukazatel na řádek podle indexu.
 *
 * @param index  Index 0..count-1.
 * @return Pointer na řádek, nebo NULL pokud index out-of-range.
 *
 * Lifetime: pointer platný do nejbližšího `watch_add()` / `watch_remove()` /
 * `watch_move()` / `watch_clear_storage()` / `watch_shutdown()` (může
 * nastat realokace dynamic array). Pro persistentní použití caller
 * musí zkopírovat data.
 */
extern const st_WATCH_ROW *watch_get ( size_t index );


/* ===================================================================== */
/*  Type metadata                                                        */
/* ===================================================================== */


/**
 * @brief Vrátí velikost čteného bloku v bajtech pro daný typ.
 *
 * Pro fixed-size typy (u8/i8/u16/i16/u32/bit) `length` argument se
 * ignoruje. Pro variabilní typy (ascii/mzascii/bytes) vrací `length`.
 *
 * `WATCH_TYPE_BIT` vrací 1 (= čte se jeden byte, z něj se extrahuje bit).
 *
 * @param t       Typ.
 * @param length  Délka pole (jen pro variabilní typy).
 * @return Velikost v bajtech.
 */
extern size_t watch_type_size ( en_WATCH_TYPE t, uint16_t length );


/**
 * @brief Vrací true pokud typ má parametr `length` (ascii/mzascii/bytes).
 *
 * @param t  Typ.
 * @return true pokud typ vyžaduje Length spinner v UI, false jinak.
 */
extern bool watch_type_has_length ( en_WATCH_TYPE t );


/**
 * @brief Vrací true pokud typ má smysluplný display sub-format
 *        (dec/hex/bin/char).
 *
 * @param t  Typ.
 * @return true pro int typy (u8/i8/u16/i16/u32/i32), false pro
 *         bit/ascii/mzascii/bytes (mají fixed display).
 */
extern bool watch_type_has_format ( en_WATCH_TYPE t );


/**
 * @brief Vrátí default `en_WATCH_FMT` pro typ.
 *
 * Unsigned int -> HEX, signed int -> DEC, ostatní -> HEX (= placeholder).
 *
 * @param t  Typ.
 * @return Default formát.
 */
extern en_WATCH_FMT watch_type_default_fmt ( en_WATCH_TYPE t );


/**
 * @brief Vrátí maximální povolenou délku pro variabilní typ.
 *
 * `WATCH_TYPE_BYTES` = `WATCH_LENGTH_MAX_BYTES` (256),
 * `WATCH_TYPE_ASCII` / `MZASCII` = `WATCH_LENGTH_MAX_STRING` (64),
 * ostatní = 0.
 *
 * @param t  Typ.
 * @return Max délka v bajtech.
 */
extern uint16_t watch_type_max_length ( en_WATCH_TYPE t );


/* ===================================================================== */
/*  Polling read (UI vlákno)                                             */
/* ===================================================================== */


/**
 * @brief Přečte hodnotu integer typu (u8/i8/u16/i16/u32/bit) jako uint64_t.
 *
 * Volání `memory_read_byte_no_se()` na adrese (a addr+1, ...) podle typu.
 *
 * Pro signed typy se výsledek vrací jako bit-cast (zero-extended pokud
 * je užší než 64-bit; caller musí sign-extend přes mask pokud potřebuje
 * skutečnou signed hodnotu pro výpočet - pro display formátování to dělá
 * `watch_format_int()`).
 *
 * Pro `WATCH_TYPE_BIT` vrací 0 nebo 1.
 *
 * Pro non-int typy (ascii/mzascii/bytes) vrací 0 (= caller má použít
 * `watch_read_bytes()`).
 *
 * @param row  Pointer na řádek (NULL = vrátí 0).
 * @return Hodnota.
 *
 * Threading: bezpečné z UI vlákna. Race s EMU vláknem může způsobit
 * zobrazení stale hodnoty (akceptovaný UX kompromis).
 */
extern uint64_t watch_read_int ( const st_WATCH_ROW *row );


/**
 * @brief Přečte sekvenci bajtů pro variabilní typy (ascii/mzascii/bytes).
 *
 * Volá `memory_read_byte_no_se()` opakovaně na `row->addr` ... `row->addr +
 * length - 1` (s wrap modulo 0x10000 = caller si je vědom Z80 16-bit
 * adresního prostoru).
 *
 * @param row       Pointer na řádek (NULL = no-op).
 * @param out_buf   Cílový buffer.
 * @param buf_size  Velikost cíle.
 * @param out_len   Výstup - počet zapsaných bajtů = min(buf_size,
 *                  watch_type_size(row->type, row->length)). NULL OK.
 *
 * Pro int typy (u8/u16/...) vrátí `out_len = 0`.
 */
extern void watch_read_bytes ( const st_WATCH_ROW *row,
                                uint8_t *out_buf, size_t buf_size,
                                size_t *out_len );


/**
 * @brief Legacy wrapper - vrací u8 na adrese row (jen pro Phase A
 *        kompatibilitu).
 *
 * @param row  Řádek (NULL = 0).
 * @return Byte z `memory_read_byte_no_se(row->addr)`.
 */
extern uint8_t watch_read_u8 ( const st_WATCH_ROW *row );


/* ===================================================================== */
/*  Formátování hodnot na řetězec                                        */
/* ===================================================================== */


/**
 * @brief Formátuje integer hodnotu podle `row->type` + `row->fmt` do
 *        výstupního bufferu.
 *
 * Délka výstupu závisí na typu+formátu (např. u8/hex = `"0xNN"`, u32/dec =
 * `"4294967295"`). Pro `WATCH_TYPE_BIT` ignoruje fmt a vrací `"0"` / `"1"`.
 *
 * Pro CHAR formát jen u8: pokud printable -> `"'A'"`, jinak fallback
 * `"0xNN"`.
 *
 * @param row       Řádek (NULL = výstup `""`).
 * @param value     Surová hodnota z `watch_read_int()` (může být wrap-around
 *                  pro signed; tato funkce si dělá sign-extend dle typu).
 * @param out       Výstupní buffer.
 * @param out_size  Velikost (>= 32 doporučeno).
 */
extern void watch_format_int ( const st_WATCH_ROW *row, uint64_t value,
                                char *out, size_t out_size );


/**
 * @brief Formátuje sekvenci bajtů jako ASCII řetězec s escape pro
 *        non-printable znaky.
 *
 * Non-printable bajt (< 0x20 nebo >= 0x7F) -> `\\xNN` escape.
 * Backslash `\\` -> `\\\\`. Uvozovky NEescapovány (= surový text).
 *
 * @param buf               Vstup.
 * @param len               Počet bajtů.
 * @param out               Výstupní buffer.
 * @param out_size          Velikost.
 * @param sharp_translation Pokud true, každý byte projde
 *                          `sharpmz_to_utf8(byte, SHARPMZ_CHARSET_EU)` a
 *                          výsledek se vloží přímo (= mzascii mode).
 *                          Pokud false = čistý ASCII (řadové bajty 0x00-0xFF).
 */
extern void watch_format_ascii ( const uint8_t *buf, size_t len,
                                  char *out, size_t out_size,
                                  bool sharp_translation );


/**
 * @brief Formátuje sekvenci bajtů jako space-separated hex dump.
 *
 * Výstup: `"12 34 56 ..."`. Pokud `len > max_inline`, vypíše prvních
 * `max_inline` bajtů + `" ..."` ellipsis.
 *
 * @param buf         Vstup.
 * @param len         Počet bajtů.
 * @param out         Výstupní buffer.
 * @param out_size    Velikost.
 * @param max_inline  Limit inline bajtů (0 = neomezený, ale stále omezený
 *                    velikostí bufferu).
 * @return true pokud byl výstup zkrácen (= caller může zobrazit tooltip
 *         s plným dumpem).
 */
extern bool watch_format_bytes ( const uint8_t *buf, size_t len,
                                  char *out, size_t out_size,
                                  size_t max_inline );


/* ===================================================================== */
/*  Parse helper                                                         */
/* ===================================================================== */


/**
 * @brief Parsuje řetězec na 16-bit adresu.
 *
 * Akceptované formáty:
 *   - hex C-style: `0xC080`, `0xc080`
 *   - hex IASM:    `0C080h`, `C080h` (suffix `h` / `H`)
 *   - decimal:     `49280`
 *   - symbol:      libovolný identifier resolvený přes
 *                  `sym_db_lookup_by_name()` (case-sensitive)
 *
 * Vstup je whitespace-tolerant (= leading + trailing). Hodnota musí být
 * v rozsahu 0..0xFFFF, jinak se vrací false.
 *
 * @param input    Vstupní řetězec (NULL = false).
 * @param out_addr Výstup 16-bit adresa (jen při úspěchu).
 * @return true při úspěšném parse, false jinak (= unknown symbol /
 *         invalid format / out-of-range).
 */
extern bool watch_parse_address ( const char *input, uint16_t *out_addr );


/* ===================================================================== */
/*  Persistence                                                          */
/* ===================================================================== */


/**
 * @brief Uloží storage do JSON souboru.
 *
 * Schéma: top-level objekt s klíčem `"watch"`, hodnota = pole objektů:
 *   {
 *     "name": "...",       // string nebo missing = NULL
 *     "addr": 49280,       // int (decimal)
 *     "bank": -1,          // int
 *     "type": "u8",        // string (mapping přes watch_type_to_str)
 *     "fmt": "hex",        // string (jen pokud má smysl pro typ)
 *     "length": 8,         // int (jen pro variabilní typy)
 *     "bit_index": 3       // int (jen pro WATCH_TYPE_BIT)
 *   }
 *
 * Volitelné fieldy (fmt/length/bit_index) se emitují jen pokud jsou
 * relevantní pro typ.
 *
 * @param path  Cílová cesta. NULL = default per-arch (`mz<N>.watch`
 *              resolved proti cfg dir).
 * @return true při úspěchu, false při I/O chybě.
 *
 * Side effects: zápis na disk.
 */
extern bool watch_save_to_filepath ( const char *path );


/**
 * @brief Načte storage z JSON souboru (REPLACE mode).
 *
 * Před load se storage wipne. Pokud soubor neexistuje, storage zůstane
 * prázdný (= success).
 *
 * BC tolerance:
 *   - chybějící `fmt` / `length` / `bit_index` = defaulty (BC s Phase A)
 *   - neznámý `type` string = řádek se skipne s warning na stderr
 *     (= forward compat)
 *
 * @param path  Zdrojová cesta. NULL = default per-arch.
 * @return true při úspěchu (i pokud soubor neexistuje), false při
 *         I/O / parse chybě.
 */
extern bool watch_load_from_filepath ( const char *path );


/**
 * @brief Vrátí default `.watch` filepath per-arch (resolved proti cfg dir).
 *
 * Mapping:
 *   - MZ800 build: `mz800.watch`
 *   - MZ700 build: `mz700.watch`
 *   - MZ1500 build: `mz1500.watch`
 *
 * @return Heap-allocated string, caller je zodpovědný za `g_free`.
 *         NULL pokud cfg dir není dostupný.
 */
extern char *watch_default_filepath ( void );


/* ===================================================================== */
/*  Cfg integrace                                                        */
/* ===================================================================== */


/**
 * @brief Inicializuje cfg sekci `[WATCH_PANEL]` a případně auto-loadne.
 *
 * Registruje klíče:
 *   - `watch_file` (TEXT, per-arch default)
 *   - `auto_load` (BOOL, default 1)
 *   - `auto_save` (BOOL, default 1)
 *
 * Po registraci + propagate provede auto_load pokud je `auto_load=1`.
 *
 * Volat z `debugger_init()` PO `watch_init()`.
 */
extern void watch_cfg_init ( void );


/**
 * @brief Auto-save storage do default souboru pokud `auto_save=1` v cfg.
 *
 * Volat z `debugger_exit()` PŘED `watch_shutdown()`.
 */
extern void watch_cfg_auto_save ( void );


/**
 * @brief Vrátí aktuální stav cfg flag `auto_load`.
 *
 * @return true / false dle cfg klíče `auto_load`.
 */
extern bool watch_cfg_get_auto_load ( void );


/**
 * @brief Nastaví cfg flag `auto_load` (in-memory, persist proběhne
 *        při typickém cfgmain save).
 *
 * @param enable  Nová hodnota.
 */
extern void watch_cfg_set_auto_load ( bool enable );


/**
 * @brief Vrátí aktuální stav cfg flag `auto_save`.
 *
 * @return true / false dle cfg klíče `auto_save`.
 */
extern bool watch_cfg_get_auto_save ( void );


/**
 * @brief Nastaví cfg flag `auto_save` (in-memory).
 *
 * @param enable  Nová hodnota.
 */
extern void watch_cfg_set_auto_save ( bool enable );


/**
 * @brief Vrátí aktuální stav cfg flag `highlight_changes` (Phase E.1).
 *
 * Default true. UI volá pro inicializaci sticky-header checkboxu.
 *
 * @return true / false dle cfg.
 */
extern bool watch_cfg_get_highlight_changes ( void );


/**
 * @brief Nastaví cfg flag `highlight_changes` (in-memory). UI volá při
 *        toggle.
 *
 * @param enable  Nová hodnota.
 */
extern void watch_cfg_set_highlight_changes ( bool enable );


/**
 * @brief Vrátí aktuální `highlight_fade_ms` (Phase E.1, default 500).
 *
 * @return Hodnota v ms (>= 1).
 */
extern int watch_cfg_get_highlight_fade_ms ( void );


/**
 * @brief Nastaví `highlight_fade_ms` (Phase E.1, in-memory).
 *
 * @param ms  Hodnota v ms (caller zajistí >= 1).
 */
extern void watch_cfg_set_highlight_fade_ms ( int ms );


/* ===================================================================== */
/*  Type / fmt string mapping (sdílené s persistence + UI)               */
/* ===================================================================== */


/**
 * @brief Převede `en_WATCH_TYPE` na stabilní řetězec pro JSON persist.
 *
 * @param t  Hodnota enum.
 * @return Konstantní řetězec (lifetime = program lifetime), nebo
 *         `"u8"` pro neznámé hodnoty (= defenzivní fallback).
 */
extern const char *watch_type_to_str ( en_WATCH_TYPE t );


/**
 * @brief Převede řetězec na `en_WATCH_TYPE`.
 *
 * @param s    Vstup (`"u8"` / `"i8"` / `"u16le"` / ... viz `watch_type_to_str`).
 * @param out  Výstup enum (jen při úspěchu).
 * @return true při známém řetězci, false jinak (= forward-compat
 *         signál pro persistence loader).
 */
extern bool watch_type_from_str ( const char *s, en_WATCH_TYPE *out );


/**
 * @brief Převede `en_WATCH_FMT` na stabilní řetězec.
 *
 * @param f  Hodnota enum.
 * @return Konstantní řetězec (`"dec"` / `"hex"` / `"bin"` / `"char"`).
 */
extern const char *watch_fmt_to_str ( en_WATCH_FMT f );


/**
 * @brief Převede řetězec na `en_WATCH_FMT`.
 *
 * @param s    Vstup.
 * @param out  Výstup enum (jen při úspěchu).
 * @return true při známém řetězci, false jinak.
 */
extern bool watch_fmt_from_str ( const char *s, en_WATCH_FMT *out );


/**
 * @brief Převede `en_WATCH_MODE` na stabilní řetězec pro JSON persist.
 *
 * Mapping: `WATCH_MODE_ADDRESS` -> `"address"`,
 *          `WATCH_MODE_EXPR_SCALAR` -> `"expr_scalar"`,
 *          `WATCH_MODE_EXPR_DEREF`  -> `"expr_deref"`.
 *
 * @param m  Hodnota enum.
 * @return Konstantní řetězec (lifetime = program lifetime).
 */
extern const char *watch_mode_to_str ( en_WATCH_MODE m );


/**
 * @brief Převede řetězec na `en_WATCH_MODE`.
 *
 * @param s    Vstup.
 * @param out  Výstup enum (jen při úspěchu).
 * @return true při známém řetězci, false jinak.
 */
extern bool watch_mode_from_str ( const char *s, en_WATCH_MODE *out );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* WATCH_H */
