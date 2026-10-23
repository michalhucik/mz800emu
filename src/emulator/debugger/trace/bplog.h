/**
 * @file   bplog.h
 * @brief  Záznam výstupu akcí breakpointů (`log`, hláška `stop`) do ringu
 *         v paměti, čtený přes MCP (kanál `bplog` v trace_read).
 *
 * Akce `log` a `stop` s hláškou vypisují text na stdout (`[BP-LOG]`,
 * `[BP-STOP]`). Tentýž text se zároveň uloží sem, aby ho agent přečetl
 * přes `emu_trace_read(channel="bplog")` i tam, kde stdout nevidí
 * (`--mcp-pipe`, TCP).
 *
 * @par Ring
 * Pevná kapacita @ref BPLOG_CAPACITY záznamů, nejstarší se přepisují
 * (first_seq roste). Ring se alokuje až při prvním záznamu; do té doby
 * modul nezabírá paměť a kanál je prázdný (next_seq 0). Mimo zásah
 * breakpointu modul nic nestojí (volá ho jen provedení akce).
 *
 * @par Segment
 * Kanál běží vždy, segment proto není nikdy 0 (od startu 1). Kurzor
 * vzatý z prázdného kanálu platí i po prvním záznamu. Segment se zvýší
 * jen při bplog_clear() nad alokovaným ringem a při bplog_free() (data
 * zanikla). Reset stroje ring nemaže.
 *
 * @par Vlákna
 * Záznam (akce BP), vyčištění i čtení (drain dbgapi) běží v emu vlákně.
 * Modul nemá zámky; čtení z jiného vlákna jde přes kopii v drainu dbgapi
 * (DBGAPI_CMD_TRACE_SNAPSHOT, kanál DBGAPI_TRACE_CHANNEL_BPLOG).
 *
 * @par Licence: GPLv3
 */

#ifndef BPLOG_H
#define BPLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Kapacita ringu [záznamy]. */
#define BPLOG_CAPACITY 4096u
/** @brief Velikost pole textu v záznamu (text nejvýš BPLOG_TEXT_SIZE - 1 znaků + NUL). */
#define BPLOG_TEXT_SIZE 240u
/** @brief Velikost pole jména BP v záznamu (jméno nejvýš BPLOG_NAME_SIZE - 1 znaků + NUL). */
#define BPLOG_NAME_SIZE 32u

/** @brief Druh záznamu (která akce text vytvořila). */
typedef enum en_BPLOG_KIND {
    BPLOG_KIND_LOG = 0,    /**< Akce `log`. */
    BPLOG_KIND_STOP,       /**< Hláška akce `stop "..."`. */
    BPLOG_KIND_COUNT       /**< Počet druhů (není platná hodnota). */
} en_BPLOG_KIND;

/** @brief Příznak záznamu: text byl delší než BPLOG_TEXT_SIZE - 1 a je oříznutý. */
#define BPLOG_FLAG_TEXT_TRUNCATED 0x01u
/** @brief Příznak záznamu: jméno BP bylo delší než BPLOG_NAME_SIZE - 1 a je oříznuté. */
#define BPLOG_FLAG_NAME_TRUNCATED 0x02u

/**
 * @brief Záznam v ringu (296 B; rozložení čte i trace_reader, kanál `bplog`).
 *
 * Paměť hostitele (little endian na podporovaných hostitelích); stejné bajty
 * vrací čtení `raw_b64`.
 *
 * @invariant `name` a `text` jsou vždy ukončené NUL, zbytek pole je vynulovaný.
 * @invariant `text_len` = strlen(text).
 */
typedef struct st_BPLOG_REC {
    uint64_t t;           /**< Čas zásahu [takty GDG od startu emulace] (= `Cycle` ve výrazu BP). */
    uint32_t frame;       /**< Snímek (= `Frame` ve výrazu BP). */
    uint16_t insn_pc;     /**< Adresa právě vykonávané instrukce (= `InsnPC`). */
    uint16_t pc;          /**< Živé PC CPU v okamžiku akce (u MEM_R/MEM_W/IORQ už za instrukcí). */
    int32_t bp_id;        /**< Id breakpointu, -1 = akce mimo breakpoint. */
    uint8_t kind;         /**< en_BPLOG_KIND. */
    uint8_t flags;        /**< BPLOG_FLAG_*. */
    uint16_t text_len;    /**< Délka textu bez NUL. */
    char name[BPLOG_NAME_SIZE]; /**< Jméno breakpointu ("" = bez jména). */
    char text[BPLOG_TEXT_SIZE]; /**< Vyrenderovaný text akce. */
} st_BPLOG_REC;

/** @brief Stav ringu. */
typedef struct st_BPLOG_STATUS {
    bool allocated;       /**< Ring existuje (od prvního záznamu do bplog_free()). */
    unsigned segment;     /**< Pořadí segmentu, vždy >= 1. */
    uint32_t capacity;    /**< Kapacita ringu (BPLOG_CAPACITY). */
    uint64_t first_seq;   /**< Nejstarší záznam v ringu (= počet přepsaných). */
    uint64_t next_seq;    /**< Příští seq (= všechny záznamy segmentu). */
} st_BPLOG_STATUS;

/**
 * @brief Zaznamenat výstup akce breakpointu.
 *
 * Při prvním volání alokuje ring (BPLOG_CAPACITY záznamů); segment se
 * nemění. Text a jméno delší než pole se oříznou a záznam dostane příznak.
 *
 * @param kind     Druh (en_BPLOG_KIND).
 * @param bp_id    Id breakpointu (-1 = mimo breakpoint).
 * @param name     Jméno breakpointu (NULL = "").
 * @param t        Čas [takty GDG].
 * @param frame    Snímek.
 * @param insn_pc  Adresa vykonávané instrukce.
 * @param pc       Živé PC.
 * @param text     Text (NULL = "").
 *
 * @post Při selhání alokace se záznam tiše zahodí (ring zůstane
 *       nealokovaný, další volání alokaci zkusí znovu).
 * @note Jen emu vlákno.
 */
void bplog_record(en_BPLOG_KIND kind, int32_t bp_id, const char *name, uint64_t t,
                  uint32_t frame, uint16_t insn_pc, uint16_t pc, const char *text);

/**
 * @brief Vyprázdnit ring a začít nový segment.
 *
 * Nad nealokovaným ringem nic nedělá (není co mazat, segment i kurzory
 * platí dál). Alokace zůstává.
 *
 * @note Jen emu vlákno.
 */
void bplog_clear(void);

/**
 * @brief Uvolnit ring (ukončení emulátoru, testy).
 *
 * @post bplog_get_status() hlásí allocated = false a next_seq 0; pokud ring
 *       existoval, i nový segment (starý kurzor neplatí). Další záznam
 *       alokuje ring znovu.
 * @note Jen emu vlákno.
 */
void bplog_free(void);

/**
 * @brief Stav ringu.
 * @param out Výstup (nesmí být NULL).
 * @note Jen emu vlákno.
 */
void bplog_get_status(st_BPLOG_STATUS *out);

/**
 * @brief Zkopírovat souvislý rozsah záznamů z ringu.
 *
 * Začátek se posune na first_seq, pokud je @p from_seq starší.
 *
 * @param from_seq  První požadovaný záznam.
 * @param count     Nejvýš záznamů.
 * @param dst       Cíl (aspoň @p count položek).
 * @param out_seq   Výstup: seq prvního zkopírovaného záznamu.
 * @return Počet zkopírovaných záznamů.
 * @note Jen emu vlákno.
 */
uint64_t bplog_copy(uint64_t from_seq, uint64_t count, st_BPLOG_REC *dst, uint64_t *out_seq);

/**
 * @brief Jméno druhu ("log", "stop").
 * @return Statický řetězec, "?" mimo rozsah.
 */
const char *bplog_kind_name(unsigned kind);

/**
 * @brief Druh podle jména.
 * @return en_BPLOG_KIND, nebo -1 pro neznámé jméno (i NULL).
 */
int bplog_kind_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* BPLOG_H */
