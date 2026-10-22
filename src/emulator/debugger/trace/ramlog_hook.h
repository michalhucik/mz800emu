/**
 * @file   ramlog_hook.h
 * @brief  Napojení záznamu zápisů do paměti (ramlog.h) na emulátor.
 *
 * Obecná část (`ramlog_hook.c`) obaluje zápis: zjistí cíle zápisu, přečte
 * původní hodnoty fyzických buněk, provede zápis a zaznamená nové hodnoty.
 * Platformní část (`mzarch/mz<N>/memory/mz<N>_ramlog.c`) zná mapování
 * paměti: ramlog_platform_targets() odpovídá makrům zápisu
 * `memory_internal_write_*` dané platformy.
 *
 * Napojení na zápis CPU bez nové podmínky v horké cestě:
 * - **háček A** (ramlog bez jiné diagnostiky): write callback
 *   ramlog_hook_cpu_write_cb() místo memory_write_cb(); read callback
 *   zůstává čistý, RAM fast-path MZ-800 vyřadí jen sledované stránky
 *   (mz800_ram_fastpath_rebuild()),
 * - **háček B** (souběžně s logovací cestou debuggeru): na konci
 *   memory_write_with_logging_cb() ramlog_hook_logging_write() místo
 *   memory_write_cb(), pokud g_ramlog_active.
 *
 * Zápisy mimo CPU: memory_write_byte_no_bp() (debugger, akce `poke`,
 * region Logical), dbgapi_regions_write() (fyzické regiony),
 * memory_load_block() (souhrnný záznam). Zdroj nastavuje volající přes
 * ramlog_source_push() (freeze, poke, CMT hack, přijetí přerušení).
 *
 * @par Vlákna
 * Jen emu vlákno.
 *
 * @par Licence: GPLv3
 */

#ifndef RAMLOG_HOOK_H
#define RAMLOG_HOOK_H

#include <stdbool.h>
#include <stdint.h>

#include "ramlog.h"
#include "libs/cpu-z80/z80.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Nejvýš cílů jednoho zápisu (4 plány grafické VRAM MZ-800). */
#define RAMLOG_TARGETS_MAX 4u

/** @brief Cíl zápisu: fyzická buňka. */
typedef struct st_RAMLOG_TARGET {
    uint8_t space;        /**< en_RAMLOG_SPACE. */
    uint8_t plane;        /**< Plán grafické VRAM MZ-800 (jen has_plane). */
    bool has_plane;       /**< Cíl je plán grafické VRAM. */
    uint32_t offset;      /**< Offset v prostoru. */
    uint8_t *cell;        /**< Buňka v paměti emulátoru, NULL = zápis se zahodí (pod ROM). */
} st_RAMLOG_TARGET;

/**
 * @brief Cíle zápisu na CPU adrese v aktuálním mapování (platforma).
 *
 * Bez vedlejších efektů. Musí odpovídat makrům zápisu platformy.
 *
 * @param addr  CPU adresa.
 * @param out   Výstup, nejvýš RAMLOG_TARGETS_MAX.
 * @param mmio  Výstup: adresa je mapovaný port (E000-E008), ne paměť.
 * @return Počet cílů; 0 a *mmio == false = zápis do grafické VRAM, který
 *         žádný plán nezmění.
 */
unsigned ramlog_platform_targets(uint16_t addr, st_RAMLOG_TARGET out[RAMLOG_TARGETS_MAX], bool *mmio);

/**
 * @brief Cíle souvislého načtení bloku (memory_load_block) na CPU adrese (platforma).
 *
 * Jako ramlog_platform_targets(), ale podle maker memory_load_block
 * (např. E000-E008 s ROM se při načtení nezapíše, ne jako port).
 *
 * @param addr  CPU adresa.
 * @param ramonly Načtení MEMORY_LOAD_RAMONLY (vždy RAM podle memram_write).
 * @param out   Výstup (jeden cíl; grafická VRAM MZ-800 = space VRAM bez buňky).
 * @return false = na adrese se nic nezapíše.
 */
bool ramlog_platform_load_target(uint16_t addr, bool ramonly, st_RAMLOG_TARGET *out);

/**
 * @brief Buňky pro dotaz "kdo zapsal" na CPU adrese v aktuálním mapování (platforma).
 *
 * Jako ramlog_platform_targets(), ale grafická VRAM MZ-800 vrátí všechny
 * 4 plány na fyzické adrese bez ohledu na aktuální režim zápisu (WF).
 *
 * @param addr  CPU adresa.
 * @param out   Výstup, nejvýš RAMLOG_TARGETS_MAX.
 * @param mmio  Výstup: adresa je mapovaný port.
 * @return Počet buněk.
 */
unsigned ramlog_platform_query_cells(uint16_t addr, st_RAMLOG_TARGET out[RAMLOG_TARGETS_MAX], bool *mmio);

/**
 * @brief Velikosti prostorů platformy [buňky] pro ramlog_start().
 * @param out Výstup (RAMLOG_SPACE_COUNT položek, chybějící prostor 0).
 */
void ramlog_platform_space_sizes(uint32_t out[RAMLOG_SPACE_COUNT]);

/**
 * @brief Prostor a offset buňky RAM podle ukazatele (hlavní RAM nebo MemExt).
 *
 * @param ptr        Ukazatel do memram_write[] stránky (+ offset).
 * @param out_space  Výstup.
 * @param out_offset Výstup.
 * @return false = ukazatel nepatří RAM ani MemExt.
 */
bool ramlog_platform_ram_cell(const uint8_t *ptr, uint8_t *out_space, uint32_t *out_offset);

/**
 * @brief Klíč stavu mapování paměti (platforma).
 *
 * Zahrnuje vstupy ramlog_platform_targets() kromě adresy, ukazatelů
 * `g_memory.memram_read[]` / `memram_write[]` a zápisových registrů
 * grafické VRAM MZ-800 (mapa ROM, VRAM, CG-RAM a PCG, u MZ-800 i režim
 * DMD). Při stejném klíči a stejných ukazatelích memram dá
 * ramlog_platform_targets() pro tutéž adresu tytéž prostory a offsety,
 * jen počet plánů grafické VRAM MZ-800 se může lišit (pokrytí ji bere
 * celou jako sběrnici). Podle toho ověřuje platnost cache buňky pokrytí
 * kódu (coverage_hook.c). Kdo přidá do ramlog_platform_targets() nový
 * vstup, musí ho přidat i sem.
 *
 * Bez vedlejších efektů, emu vlákno.
 *
 * @return Klíč (hodnota nemá jiný význam než porovnání na rovnost).
 */
uint32_t ramlog_platform_map_key(void);

/** @brief Provede zápis (callback platformy nebo zápis bez synchronizace). */
typedef void (*ramlog_write_fn)(uint16_t addr, uint8_t value, void *ctx);

/**
 * @brief Obalí jeden zápis na CPU adrese záznamem (cíle, old, zápis, new).
 *
 * @param addr        CPU adresa.
 * @param value       Zapisovaná hodnota.
 * @param default_src Zdroj, když g_ramlog_src je RAMLOG_SRC_INSN a zápis
 *                    nejde z instrukce (zápisy debuggerem: RAMLOG_SRC_DEBUGGER;
 *                    zápis CPU: RAMLOG_SRC_INSN).
 * @param fn          Vlastní zápis.
 * @param ctx         Kontext pro fn.
 * @pre g_ramlog_active, emu vlákno.
 */
void ramlog_hook_write(uint16_t addr, uint8_t value, en_RAMLOG_SRC default_src,
                       ramlog_write_fn fn, void *ctx);

/**
 * @brief Write callback CPU pro háček A (memory_write_cb() se záznamem).
 * @param cpu       CPU.
 * @param addr      Adresa.
 * @param value     Hodnota.
 * @param user_data Nepoužito.
 */
void ramlog_hook_cpu_write_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data);

/**
 * @brief Háček B: zápis CPU se záznamem z logovacího callbacku.
 *
 * Volá ho memory_write_with_logging_cb() místo memory_write_cb(), když
 * g_ramlog_active.
 */
void ramlog_hook_logging_write(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data);

/**
 * @brief Souhrnné záznamy načtení bloku (jeden na souvislý úsek se stejným cílem).
 *
 * Volá memory_load_block() před vlastním zápisem. Zdroj: g_ramlog_src,
 * když není RAMLOG_SRC_INSN (CMT hack), jinak RAMLOG_SRC_LOAD.
 *
 * @param addr    CPU adresa začátku.
 * @param size    Počet bajtů.
 * @param ramonly MEMORY_LOAD_RAMONLY.
 * @pre g_ramlog_active.
 */
void ramlog_hook_load_block(uint16_t addr, uint16_t size, bool ramonly);

/**
 * @brief Záznam zápisu do fyzické buňky mimo CPU (fyzické regiony MCP region_write).
 *
 * @param space   Prostor.
 * @param offset  Offset.
 * @param old_v   Hodnota před zápisem.
 * @param new_v   Hodnota po zápisu.
 * @pre g_ramlog_active.
 */
void ramlog_hook_cell_write(en_RAMLOG_SPACE space, uint32_t offset, uint8_t old_v, uint8_t new_v);

/**
 * @brief Spustí záznam s velikostmi prostorů platformy a přepne callbacky.
 *
 * @param cfg Parametry.
 * @return false = nedostatek paměti.
 * @post Callbacky CPU a maska RAM fast-path odpovídají novému stavu.
 */
bool ramlog_hook_start(const st_RAMLOG_CONFIG *cfg);

/**
 * @brief Zastaví záznam a vrátí callbacky (data zůstanou čitelná).
 */
void ramlog_hook_stop(void);

/**
 * @brief Režim spuštění záznamu z konfigurace (INI `[RAMLOG] mode`, CLI
 *        `--ramlog-mode`). Hodnoty shodné s režimy Trace Suite.
 */
typedef enum en_RAMLOG_CFG_MODE {
    RAMLOG_CFG_MODE_OFF = 0,      /**< Konfigurace záznam nespouští (jen MCP, DSL, GUI). */
    RAMLOG_CFG_MODE_WITH_WINDOW,  /**< Záznam běží, dokud je otevřené okno debuggeru. */
    RAMLOG_CFG_MODE_ALWAYS        /**< Záznam běží od startu emulátoru. */
} en_RAMLOG_CFG_MODE;

/**
 * @brief Konfigurace spouštění záznamu (INI sekce `[RAMLOG]` + CLI).
 *
 * Invarianty: `capacity` v RAMLOG_MIN_CAPACITY..RAMLOG_MAX_CAPACITY,
 * `range_lo` a `range_hi` jsou CPU adresy; `range_lo` 0 a `range_hi` FFFFh
 * = celá paměť (bez filtru, i buňky mimo aktuální mapování).
 */
typedef struct st_RAMLOG_INI {
    unsigned mode;      /**< en_RAMLOG_CFG_MODE. */
    unsigned ring_mode; /**< en_RAMLOG_MODE (RING / FILL). */
    unsigned capacity;  /**< Kapacita ringu v záznamech. */
    unsigned range_lo;  /**< První CPU adresa filtru. */
    unsigned range_hi;  /**< Poslední CPU adresa filtru. */
} st_RAMLOG_INI;

/** @brief Konfigurace spouštění záznamu (platí po ramlog_cfg_init a ramlog_apply_cli_options). */
extern st_RAMLOG_INI g_ramlog_ini;

/**
 * @brief Zaregistruje INI sekci `[RAMLOG]` a načte ji do @ref g_ramlog_ini.
 *
 * @pre g_cfgmain existuje. Volá se jednou z debugger_init.
 */
void ramlog_cfg_init(void);

/**
 * @brief Přepíše @ref g_ramlog_ini hodnotami z CLI (`--ramlog-mode`,
 *        `--ramlog-ring`, `--ramlog-capacity`, `--ramlog-lo`, `--ramlog-hi`).
 *
 * Neplatná hodnota se ohlásí na stderr a ponechá se hodnota z INI;
 * kapacita se ořízne do povoleného rozsahu, prohozené meze se seřadí.
 */
void ramlog_apply_cli_options(void);

/**
 * @brief Spustí nebo zastaví záznam podle `g_ramlog_ini.mode`.
 *
 * Volá se z mzarch_platform_fn_debugger_state_changed před výběrem
 * callbacků CPU, proto přímo ramlog_start / ramlog_stop (ne ramlog_hook_*,
 * které by state_changed volaly znovu). Spustí jen tehdy, když žádný záznam
 * neběží; záznam zastavený přes MCP / DSL / GUI znovu nespustí. Zastaví
 * běžící záznam, jen když ho předtím spustila sama (i když ho mezitím MCP
 * přestartovalo s jinými parametry).
 *
 * @param debugger_active Stav okna debuggeru.
 */
void ramlog_recompute_active(bool debugger_active);

#ifdef __cplusplus
}
#endif

#endif /* RAMLOG_HOOK_H */
