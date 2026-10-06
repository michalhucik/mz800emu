/*
 * dbgapi_helpers.h - vrstva tenkých UI helperů nad dbgapi CMDRQ kanálem
 *
 * UI vlákno (= ImGui handlery, SDL klávesové callbacky) by mělo komunikovat
 * s emulátorem pouze přes dbgapi frontu CMDRQ. Tyto helpery zaobalují
 * dbgapi_ui_submit_cmd_sync() pro nejčastější příkazy a skrývají alokaci
 * parametrických struktur i timeout konstanty.
 *
 * Vlastnictví paměti:
 * - Helpery alokují parametrické struktury na stacku (lokální proměnné).
 *   submit_cmd_sync() je synchronní, takže po návratu jsou data zase
 *   volně použitelná - žádný heap, žádný caller-side cleanup.
 * - Pro CMD_MEM_WRITE caller předává buffer; helper si ho jen mapuje
 *   do st_DBGAPI_MEM_PARAM (nezalokovává kopii).
 *
 * Thread safety:
 * - Volat výhradně z UI vlákna (= SDL event loop, ImGui render). Helpery
 *   blokují UI vlákno do doručení odpovědi z EMU vlákna (default timeout
 *   DBG_UI_DEFAULT_TIMEOUT_MS).
 *
 * Návratová hodnota:
 * - true  = příkaz byl úspěšně doručen i zpracován
 * - false = timeout / fronta plná / dbgapi se ukončuje / EMU vrátil chybu
 *
 * ----------------------------- License -------------------------------------
 *
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
 *
 * ---------------------------------------------------------------------------
 */
#ifndef UI_DBGAPI_HELPERS_H
#define UI_DBGAPI_HELPERS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "emulator/debugger/dbgapi_cmdrq.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Default timeout pro synchronní CMDRQ submit z UI vlákna.
 *
 * 200 ms odpovídá ~10 frame periodám 50 Hz emulace. Pokrývá běžné
 * případy (= drain queue per-frame screen_done event), avšak je
 * dost krátký aby UI nezaseklo při skutečném emu deadlocku.
 *
 * [neověřeno měřením] - empirická hodnota, A.3.5 acceptance.
 */
#define DBG_UI_DEFAULT_TIMEOUT_MS 200

/* ============================================================================
 * Řízení emulace
 * ============================================================================ */

/**
 * @brief Pozastaví emulaci přes CMD_PAUSE.
 *
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_pause(void);

/**
 * @brief Spustí emulaci přes CMD_RUN.
 *
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_run(void);

/**
 * @brief Toggle pauza emulace - pomocný shortcut pro Alt+P / menu items.
 *
 * Přečte aktuální stav přes CMD_IS_RUNNING, pošle PAUSE nebo RUN podle
 * potřeby. Dvě CMDRQ za jednu operaci.
 *
 * @return true při úspěchu obou operací, false jinak.
 */
bool dbg_ui_pause_toggle(void);

/**
 * @brief Reset emulátoru přes CMD_RESET.
 *
 * Asynchronní - reset se provede v příští iteraci mzarch_main. Helper
 * jen čeká na potvrzení vložení do fronty.
 *
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_reset(void);

/**
 * @brief Přepočet debugger callbacků + active flagů na EMU vlákně.
 *
 * Deleguje mzarch_platform_fn_debugger_state_changed na emu vlákno přes
 * DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE (per-frame safe-point), místo přímého
 * volání z UI vlákna. POVINNÉ pro UI změny trace-suite mode (Off/Window/
 * Always) i save flagů: stop kanálu uvolní writer buffer souběžně s
 * emu-thread tlog_writer_append = use-after-free race. Volající si nejdřív
 * nastaví příslušné g_*_config.mode / flag a teprve pak zavolá tuto funkci.
 *
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_debugger_state_recompute(void);

/* ============================================================================
 * Krokování
 * ============================================================================ */

/**
 * @brief Step Into přes CMD_STEP_INTO - jeden krok přes aktuální instrukci.
 *
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_step_into(void);

/**
 * @brief Step Over přes CMD_STEP_OVER - přeskočí CALL/RST/blokové instrukce.
 *
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_step_over(void);

/**
 * @brief Run To Address přes CMD_RUN_TO - běh do dané adresy.
 *
 * @param addr Cílová adresa (16-bit).
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_run_to(uint16_t addr);

/* ============================================================================
 * Z80 registry
 * ============================================================================ */

/**
 * @brief Zápis do Z80 registru přes CMD_SET_REG.
 *
 * @param reg_id Identifikátor registru (z80_reg_t casted na uint8_t).
 * @param value  Nová 16-bit hodnota.
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_set_reg(uint8_t reg_id, uint16_t value);

/* ============================================================================
 * Paměť
 * ============================================================================ */

/**
 * @brief Zápis bloku paměti přes CMD_MEM_WRITE.
 *
 * Caller poskytuje vstupní buffer. Helper sestaví st_DBGAPI_MEM_PARAM
 * a předá ho dispatcheru. Buffer musí žít po celou dobu volání (= je
 * synchronní, takže do návratu helperu).
 *
 * @param addr Cílová adresa.
 * @param buf  Vstupní data (vlastní caller).
 * @param len  Počet bajtů k zápisu.
 * @return true při úspěchu, false při chybě / timeoutu / NULL parametry.
 */
bool dbg_ui_mem_write(uint16_t addr, const uint8_t *buf, uint16_t len);

/* ============================================================================
 * Breakpointy
 * ============================================================================ */

/**
 * @brief Přidá execution BP přes CMD_BP_ADD.
 *
 * @param addr   Adresa BP.
 * @param out_id Pokud != NULL, sem se zapíše přidělené ID nového BP.
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_bp_add(uint16_t addr, int *out_id);

/**
 * @brief Odebere BP podle ID přes CMD_BP_REMOVE.
 *
 * @param id ID breakpointu.
 * @return true při úspěchu, false při chybě / timeoutu / neexistujícím ID.
 */
bool dbg_ui_bp_remove(int id);

/**
 * @brief Selektivní update polí existujícího BP přes CMD_BP_UPDATE.
 *
 * Plochý snapshot polí + bitmask update_mask řídí které aplikovat. Volá
 * EMU thread, který iteruje mask + volá existující `breakpoints_set_*()`.
 * Caller drží alokaci `p` + jeho stringů do návratu (sync cmd).
 *
 * @param p Vstupní payload (id existujícího BP, update_mask, fieldy).
 * @return true při úspěchu, false pokud BP neexistuje / timeout.
 */
bool dbg_ui_bp_update(const st_DBGAPI_BP_UPDATE_PARAM *p);

/**
 * @brief Quick toggle enabled flag přes CMD_BP_SET_ENABLED.
 *
 * Forwarder, žádný side-effect na ostatní pole. Použití: BP list checkbox,
 * disasm right-click toggle, drag-drop tree.
 *
 * @param id      ID existujícího BP.
 * @param enabled Nový stav.
 * @return true při úspěchu, false pokud BP neexistuje / timeout.
 */
bool dbg_ui_bp_set_enabled(int id, bool enabled);

/**
 * @brief Quick reparent BP do skupiny přes CMD_BP_SET_PARENT.
 *
 * Forwarder, žádný side-effect. Použití: drag-drop přesun mezi skupinami,
 * "Clear parent" v context menu.
 *
 * @param id        ID existujícího BP.
 * @param parent_id -1 = root (bez skupiny), jinak ID existující skupiny.
 * @return true při úspěchu, false pokud BP neexistuje / timeout.
 */
bool dbg_ui_bp_set_parent(int id, int parent_id);

/**
 * @brief Atomický create + init polí přes CMD_BP_CREATE_WITH_INIT.
 *
 * Volá `breakpoints_add_auto(addr, name, parent)` na EMU vlákně. Po
 * úspěchu naplní `p->id` přiděleným ID + aplikuje update_mask přes
 * stejnou helper cestu co BP_UPDATE. Caller MUSÍ nastavit `p->id = -1`
 * na vstupu (= invariant CREATE).
 *
 * Pokud `out_id != NULL`, sem se zapíše přidělené ID (= zkratka pro
 * čtení z `p->id` po návratu).
 *
 * @param p      Vstupní payload (id=-1, update_mask + fieldy).
 *               Po návratu p->id obsahuje přidělené ID nebo zůstává -1.
 * @param out_id Volitelný výstup nového ID.
 * @return true při úspěchu, false pokud create selhal / timeout.
 */
bool dbg_ui_bp_create_with_init(st_DBGAPI_BP_UPDATE_PARAM *p, int *out_id);

/**
 * @brief Přidá novou skupinu BP přes CMD_BPGRP_ADD.
 *
 * @param name   Jméno nové skupiny.
 * @param parent -1 = root, jinak ID existující rodičovské skupiny.
 * @param out_id Pokud != NULL, sem se zapíše přidělené ID nové skupiny.
 * @return true při úspěchu, false při chybě / timeoutu.
 */
bool dbg_ui_bpgrp_add(const char *name, int parent, int *out_id);

/**
 * @brief Odebere skupinu BP podle ID přes CMD_BPGRP_REMOVE.
 *
 * @param id ID existující skupiny.
 * @return true při úspěchu, false pokud skupina neexistuje / timeout.
 */
bool dbg_ui_bpgrp_remove(int id);

/**
 * @brief Selektivní update polí existující skupiny přes CMD_BPGRP_UPDATE.
 *
 * Plochý snapshot enabled/name/colors/parent + bitmask `update_mask` řídí
 * které aplikovat. Caller drží alokaci `p` + jeho `name` do návratu.
 *
 * @param p Vstupní payload (id existující skupiny, update_mask, fieldy).
 * @return true při úspěchu, false pokud skupina neexistuje / timeout.
 */
bool dbg_ui_bpgrp_update(const st_DBGAPI_BPGRP_UPDATE_PARAM *p);

/* ============================================================================
 * Historie a aktivita I/O (okno I/O Ports)
 *
 * Ring io_history a tabulku g_io_activity plní emu vlákno. Mutace proto
 * vykoná emu vlákno v drainu fronty; UI vlákno na výsledek synchronně čeká.
 * Při timeoutu vyzvednutí se příkaz zruší a NEprovede (žádná mutace).
 * ============================================================================ */

/**
 * @brief Limit vyzvednutí příkazů okna I/O Ports emu vláknem (ms).
 *
 * Za běhu emu vlákno vyzvedá frontu na konci snímku (~20 ms při 100 %),
 * v pauze hned (dbgapi_emu_wait_for_cmd). Hodnota 1000 ms je převzatá
 * z Event Vieweru a Memory Browseru (úvaha pro silně zpomalenou emulaci,
 * [neověřeno] měřením).
 */
#define DBG_UI_IO_CMD_TIMEOUT_MS 1000

/**
 * @brief Změní kapacitu ringu historie I/O přes DBGAPI_CMD_IO_HISTORY_SET_CAPACITY.
 *
 * Emu vlákno ring realokuje a zahodí dosavadní události. Hodnota se
 * clampuje do [IO_HISTORY_MIN_CAPACITY..IO_HISTORY_MAX_CAPACITY].
 *
 * @param capacity            Požadovaná kapacita (počet událostí).
 * @param out_capacity_after  Pokud != NULL a příkaz uspěl, sem se zapíše
 *                            skutečně nastavená kapacita. Při neúspěchu
 *                            se nemění.
 * @return true pokud emu vlákno ring realokovalo, false při timeoutu
 *         (příkaz neproveden), plné frontě, ukončování nebo selhání alokace.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_io_history_set_capacity(uint32_t capacity, uint32_t *out_capacity_after);

/**
 * @brief Vyprázdní ring historie I/O přes DBGAPI_CMD_IO_HISTORY_CLEAR.
 *
 * @return true pokud emu vlákno ring vyprázdnilo, false při timeoutu
 *         (příkaz neproveden) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_io_history_clear(void);

/**
 * @brief Vynuluje čítače aktivity všech portů přes DBGAPI_CMD_IO_ACTIVITY_RESET.
 *
 * @return true pokud emu vlákno čítače vynulovalo, false při timeoutu
 *         (příkaz neproveden) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_io_activity_reset_all(void);

/**
 * @brief Vynuluje čítače aktivity jednoho portu přes DBGAPI_CMD_IO_ACTIVITY_RESET_PORT.
 *
 * Reset vykoná emu vlákno (io_activity_reset_port_8bit() resp.
 * io_activity_reset_port()), protože tabulku g_io_activity souběžně
 * plní io_activity_record_hit() a io_activity_advance_frame().
 * Timeout DBG_UI_IO_CMD_TIMEOUT_MS.
 *
 * @param port     Při @p is_8bit low byte portu (horní bajt se ignoruje),
 *                 jinak plná 16-bit klíčová adresa slotu (bus adresa IORQ
 *                 nebo MMIO adresa 0E000h..0E008h).
 * @param is_8bit  true = vynulovat všech 256 high-byte slotů daného low
 *                 byte (8-bit port katalogu), false = jen slot @p port.
 * @return true pokud emu vlákno čítače vynulovalo, false při timeoutu
 *         (příkaz neproveden, čítače beze změny) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_io_activity_reset_port(uint16_t port, bool is_8bit);

/* ============================================================================
 * Hromadné operace s breakpointy (okno Breakpoints)
 *
 * Smazání všech BP a načtení ze souboru přestaví pole BP, skupiny, $vars
 * a bptmap, které emu vlákno čte při vyhodnocení BP. Operaci proto vykoná
 * emu vlákno v drainu fronty; UI vlákno na výsledek synchronně čeká.
 * Souborový dialog zůstává na UI vlákně, emu vlákno dostane jen cestu.
 * Při timeoutu vyzvednutí se příkaz zruší a NEprovede (BP beze změny).
 * ============================================================================ */

/**
 * @brief Limit vyzvednutí hromadných BP příkazů emu vláknem (ms).
 *
 * Stejná úvaha jako u DBG_UI_IO_CMD_TIMEOUT_MS: za běhu emu vlákno
 * vyzvedá frontu na konci snímku, v pauze hned. Delší než
 * DBG_UI_DEFAULT_TIMEOUT_MS, protože jde o jednorázovou akci uživatele,
 * u které je tiché neprovedení při zpomalené emulaci horší než delší
 * čekání. Hodnota 1000 ms [neověřeno] měřením.
 */
#define DBG_UI_BP_BULK_CMD_TIMEOUT_MS 1000

/**
 * @brief Smaže všechny breakpointy, skupiny a $vars přes DBGAPI_CMD_BP_CLEAR_ALL.
 *
 * @return true pokud emu vlákno data smazalo, false při timeoutu (příkaz
 *         neproveden), plné frontě nebo ukončování emulátoru.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 * @post Při true jsou pole BP i skupin prázdná a bptmap vyčištěná
 *       (včetně dočasného BP Run To / Step Over - sémantika
 *       breakpoints_clear_all). ID dříve vybraných položek už neplatí.
 */
bool dbg_ui_bp_clear_all(void);

/**
 * @brief Nahradí breakpointy obsahem souboru přes DBGAPI_CMD_BP_LOAD_FILE.
 *
 * @param filepath Cesta k souboru; NULL nebo "" = výchozí soubor
 *                 (g_breakpoints.default_file). Řetězec musí zůstat platný
 *                 do návratu z funkce (synchronní submit).
 * @return true pokud emu vlákno načtení provedlo, false při timeoutu
 *         (příkaz neproveden, BP beze změny), plné frontě nebo ukončování
 *         emulátoru. Úspěch neznamená, že soubor existoval: loader stávající
 *         data smaže vždy a neexistující soubor zanechá prázdný stav.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu (včetně čtení
 *      a parsování souboru na emu vlákně).
 * @post Při true ID dříve vybraných položek už nemusí odpovídat stejnému BP.
 */
bool dbg_ui_bp_load_from_file(const char *filepath);

/**
 * @brief Vynuluje počítadlo zásahů BP přes DBGAPI_CMD_BP_RESET_HITS.
 *
 * bpt->hits inkrementuje emu vlákno při zásahu BP a porovnává s hit_count;
 * reset proto vykoná emu vlákno (breakpoints_reset_hits). Timeout
 * DBG_UI_BP_BULK_CMD_TIMEOUT_MS.
 *
 * @param id ID breakpointu.
 * @return true pokud emu vlákno počítadlo vynulovalo; false pro neexistující
 *         ID, při timeoutu (příkaz neproveden, počítadlo beze změny) nebo
 *         jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_bp_reset_hits(int id);

/* ============================================================================
 * Memory Heatmap / CDL (okno Memory Heatmap, menu Debugger -> Settings -> CDL)
 *
 * Countery g_mhmap inkrementuje emu vlákno v logging callbaccích. Mazání
 * a Add / Sub vykoná emu vlákno v drainu fronty; UI vlákno na výsledek
 * synchronně čeká. Při timeoutu vyzvednutí se příkaz zruší a NEprovede.
 * ============================================================================ */

/** @brief Neúplný typ mapy counterů (definice v emulator/debugger/mhmap.h). */
struct st_MHMAP;

/**
 * @brief Limit vyzvednutí příkazů Memory Heatmap emu vláknem (ms).
 *
 * Stejná úvaha jako u DBG_UI_IO_CMD_TIMEOUT_MS: za běhu emu vlákno vyzvedá
 * frontu na konci snímku, v pauze hned; jde o jednorázovou akci uživatele.
 * Hodnota 1000 ms [neověřeno] měřením.
 */
#define DBG_UI_MHMAP_CMD_TIMEOUT_MS 1000

/**
 * @brief Přepne režim záznamu Memory Heatmap (Off / With Window / Always).
 *
 * Zapíše g_debugger.mhmap_mode a přepočet callbacků CPU (rychlá / logging
 * cesta) deleguje na emu vlákno přes dbg_ui_debugger_state_recompute()
 * (DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE) - stejný vzor jako režim CPU
 * Instruction History. Do provedení přepočtu emu vlákno zůstává na
 * původní sadě callbacků: logging cesta sama testuje
 * TEST_DEBUGGER_MHMAP_ACTIVE, takže po vypnutí už nezaznamenává; po
 * zapnutí se začne zaznamenávat až po přepočtu.
 *
 * @param mode Hodnota en_DEBUGGER_MHMAP_MODE (OFF / WITH_WINDOW / ALWAYS).
 * @return true pokud emu vlákno přepočet provedlo, false při timeoutu
 *         (DBG_UI_DEFAULT_TIMEOUT_MS) nebo jiné chybě submitu. Režim je
 *         zapsán i při false; callbacky se pak přepnou až při příštím
 *         přepočtu (jakýkoli RECOMPUTE, otevření / zavření debuggeru).
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení přepočtu.
 */
bool dbg_ui_mhmap_set_mode(int mode);

/**
 * @brief Vynuluje všechny countery Memory Heatmap přes DBGAPI_CMD_CDL_RESET.
 *
 * Stejný příkaz jako MCP cdl_reset (mhmap_reset na emu vlákně); režim
 * záznamu se nemění.
 *
 * @return true pokud emu vlákno countery vynulovalo, false při timeoutu
 *         (příkaz neproveden) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_mhmap_reset(void);

/**
 * @brief Vynuluje countery jednoho regionu přes DBGAPI_CMD_MHMAP_RESET_REGION.
 *
 * @param region_index Index regionu v tabulce mhmap_get_export_regions.
 * @return true pokud emu vlákno region vynulovalo, false při indexu mimo
 *         rozsah, timeoutu (příkaz neproveden) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_mhmap_reset_region(uint32_t region_index);

/**
 * @brief Přičte / odečte mapu k živým counterům přes DBGAPI_CMD_MHMAP_MERGE.
 *
 * @param src Zdrojová mapa (typicky importovaná data okna). Jen se čte;
 *            vlastní ji volající, musí žít do návratu (synchronní submit).
 * @param op  Hodnota en_MHMAP_MERGE_OP (MHMAP_MERGE_ADD / MHMAP_MERGE_SUB).
 * @return true pokud emu vlákno operaci provedlo, false pro NULL @p src,
 *         neznámé @p op, timeout (příkaz neproveden) nebo jinou chybu submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_mhmap_merge(const struct st_MHMAP *src, unsigned op);

/* ============================================================================
 * Memory Map - banking a DMD (okno Memory Map)
 *
 * g_memory.map a g_gdg.regDMD čte emu vlákno při každém přístupu CPU do
 * paměti a samo je mění (OUT / IN E0-E6, OUT CEh). Změnu i následné
 * memory_reconnect_ram() (RAM ukazatele, MZ-800 fast-path) a refresh obrazu
 * vykoná emu vlákno v drainu fronty (DBGAPI_CMD_MEMMAP_SET); UI vlákno na
 * výsledek synchronně čeká. Při timeoutu vyzvednutí se příkaz zruší
 * a NEprovede.
 * ============================================================================ */

/**
 * @brief Limit vyzvednutí příkazů Memory Map emu vláknem (ms).
 *
 * Stejná úvaha jako u DBG_UI_IO_CMD_TIMEOUT_MS: za běhu emu vlákno vyzvedá
 * frontu na konci snímku, v pauze hned; jde o jednorázovou akci uživatele.
 * Hodnota 1000 ms [neověřeno] měřením.
 */
#define DBG_UI_MEMMAP_CMD_TIMEOUT_MS 1000

/**
 * @brief Změní banking bity g_memory.map přes DBGAPI_CMD_MEMMAP_SET.
 *
 * Emu vlákno provede g_memory.map = (map & ~clear_mask) | set_mask,
 * memory_reconnect_ram() a refresh obrazu při "Auto refresh on edit".
 * Bity mimo obě masky zůstanou v hodnotě, kterou mají v okamžiku provedení
 * (případná souběžná změna z OUT / IN E0-E6 se neztratí).
 *
 * @param clear_mask     Bity k vynulování (0xFF = nahradit celou hodnotu).
 * @param set_mask       Bity k nastavení (po clear masce).
 * @param[out] map_after g_memory.map po provedení; NULL = nezajímá. Při
 *                       neúspěchu se nemění.
 * @return true pokud emu vlákno změnu provedlo, false při timeoutu (příkaz
 *         neproveden, mapa beze změny) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_memmap_change_map(uint8_t clear_mask, uint8_t set_mask,
                              uint8_t *map_after);

/**
 * @brief Nastaví GDG registr DMD (jen MZ-800) přes DBGAPI_CMD_MEMMAP_SET.
 *
 * Emu vlákno nastaví DMD přes gdg_debug_set_regDMD() - stejné vedlejší
 * efekty jako OUT CEh (CTC0 GATE0, latch MZ-700, fast-path, framebuffer),
 * jen bez záznamu hwlog a HW event breakpointu; viz
 * st_DBGAPI_MEMMAP_SET_PARAM. Následuje memory_reconnect_ram() (DMD bit 3 /
 * bit 2 mění mapování 8000h-DFFFh) a refresh obrazu při "Auto refresh
 * on edit". Banking bity se nemění.
 *
 * @param dmd Nová hodnota DMD (okno používá 00h-07h a 08h).
 * @return true pokud emu vlákno zápis provedlo, false na jiné platformě než
 *         MZ-800, při timeoutu (příkaz neproveden) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_memmap_set_dmd(uint8_t dmd);

/* ============================================================================
 * Event Viewer - režim záznamu, kategorie a import (okno Events)
 *
 * Ring g_eventlog plní emu vlákno v eventlog_record(); příznak záznamu
 * g_eventlog_active a masku kategorií g_eventlog_active_mask čte v gate
 * každého zápisu. Změnu režimu, masky i import souboru proto vykoná emu
 * vlákno v drainu fronty; UI vlákno na výsledek synchronně čeká. Při
 * timeoutu vyzvednutí se příkaz zruší a NEprovede.
 * ============================================================================ */

/**
 * @brief Limit vyzvednutí příkazů okna Events emu vláknem (ms).
 *
 * Stejná hodnota jako EVW_CMD_TIMEOUT_MS v event_viewer_window.cpp
 * (Apply kapacity, Clear) a stejná úvaha jako u DBG_UI_IO_CMD_TIMEOUT_MS:
 * za běhu emu vlákno vyzvedá frontu na konci snímku, v pauze hned.
 * Hodnota 1000 ms [neověřeno] měřením.
 */
#define DBG_UI_EVENTLOG_CMD_TIMEOUT_MS 1000

/**
 * @brief Nastaví režim záznamu Event Vieweru přes DBGAPI_CMD_EVENTLOG_SET_MODE.
 *
 * Emu vlákno zapíše g_eventlog_config.mode a zavolá
 * eventlog_recompute_active() (spuštění / zastavení záznamu podle režimu
 * a stavu okna Events).
 *
 * @param mode            Hodnota en_EVENTLOG_MODE (0 = OFF,
 *                        1 = WHEN_WINDOW_OPEN, 2 = ALWAYS).
 * @param[out] out_active Při úspěchu g_eventlog_active po přepočtu; NULL =
 *                        nezajímá. Při neúspěchu se nemění.
 * @return true pokud emu vlákno režim nastavilo, false pro neplatný
 *         @p mode, při timeoutu (příkaz neproveden, režim beze změny) nebo
 *         jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_eventlog_set_mode(uint32_t mode, bool *out_active);

/**
 * @brief Nastaví masku zaznamenávaných kategorií přes DBGAPI_CMD_EVENTLOG_SET_MASK.
 *
 * Existující příkaz (MCP eventlog_set_mask): emu vlákno zapíše
 * g_eventlog_active_mask i g_eventlog_config.categories_mask. Volající
 * předává celou novou masku (typicky aktuální maska s přepnutým bitem).
 *
 * @param mask Nová maska (bit i = kategorie i z en_EVENTLOG_CATEGORY).
 * @return true pokud emu vlákno masku zapsalo, false při timeoutu (příkaz
 *         neproveden, maska beze změny) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_eventlog_set_mask(uint64_t mask);

/**
 * @brief Nahradí ring Event Vieweru obsahem souboru přes DBGAPI_CMD_EVENTLOG_IMPORT_FILE.
 *
 * Emu vlákno zavolá eventlog_import_from_file(): validace hlavičky,
 * případné zvětšení ringu, fread záznamů do ringu. Soubor se čte na emu
 * vlákně (nejvýš EVENTLOG_MAX_CAPACITY záznamů).
 *
 * @param path                 Cesta k souboru; řetězec musí zůstat platný
 *                             do návratu z funkce (synchronní submit).
 * @param[out] out_rc          Pokud != NULL a emu vlákno příkaz provedlo,
 *                             návratová hodnota importu (0 OK, -1 chyba).
 *                             Při timeoutu / chybě submitu se nemění.
 * @param[out] out_count_after Pokud != NULL a emu vlákno příkaz provedlo,
 *                             počet událostí v ringu po importu (i po
 *                             částečném načtení). Jinak se nemění.
 * @return true jen pokud import uspěl (rc == 0). false pro NULL / prázdnou
 *         @p path, chybu importu (rozliší @p out_rc), timeout (příkaz
 *         neproveden, ring beze změny) nebo jinou chybu submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu (včetně čtení
 *      souboru na emu vlákně).
 */
bool dbg_ui_eventlog_import_file(const char *path, int *out_rc,
                                 uint32_t *out_count_after);

/* ============================================================================
 * Event Viewer - triggery Pause on match / Auto-mark on match
 *
 * Filtr triggeru vyhodnocuje callback v eventlog_record() na emu vlákně;
 * stav triggerů vlastní eventlog_trigger.c a mění ho jen emu vlákno. Okno
 * Events naparsuje nový filtr do nového objektu a předá ho těmito helpery;
 * emu vlákno vymění filtr, jméno markeru a gate a vrátí starý filtr, který
 * helper uvolní až po návratu synchronního submitu.
 * ============================================================================ */

/* Opaque handle filtru (eventlog_filter.h). */
struct st_EVENTLOG_FILTER;

/**
 * @brief Nastaví nebo vypne trigger okna Events přes DBGAPI_CMD_EVENTLOG_TRIGGER_SET.
 *
 * Helper VŽDY převezme vlastnictví @p filter: při úspěchu ho předá modulu
 * eventlog_trigger (emu vlákno) a uvolní předchozí filtr, který emu vlákno
 * vrátilo; při jakémkoli neúspěchu (odmítnutí handlerem, timeout -
 * příkaz zrušen a neproveden, plná fronta, ukončování) uvolní @p filter
 * sám a stav triggeru zůstane beze změny.
 *
 * @param kind   en_EVENTLOG_TRIGGER_KIND (0 = PAUSE, 1 = AUTOMARK).
 * @param filter Nový filtr z eventlog_filter_parse(), který splňuje
 *               eventlog_trigger_filter_is_armable() a nevznikl z prázdného
 *               výrazu; NULL = vypnout trigger.
 * @param name   AUTOMARK: neprázdné jméno markeru (při @p filter != NULL
 *               povinné), řetězec musí platit do návratu (synchronní
 *               submit, emu vlákno si ho kopíruje). PAUSE: ignoruje se.
 * @return true pokud emu vlákno trigger nastavilo / vypnulo, jinak false.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 * @post @p filter už volající nesmí použít ani uvolnit.
 */
bool dbg_ui_eventlog_trigger_set(uint32_t kind,
                                 struct st_EVENTLOG_FILTER *filter,
                                 const char *name);

/**
 * @brief Vymaže počítadla shod triggeru přes DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES.
 *
 * PAUSE: indikátor "Last match" zmizí; AUTOMARK: počet markerů = 0.
 * Počítadla zapisuje callback triggeru na emu vlákně, proto je maže také
 * emu vlákno.
 *
 * @param kind en_EVENTLOG_TRIGGER_KIND.
 * @return true pokud emu vlákno počítadla vymazalo, false pro neplatný
 *         @p kind, při timeoutu (neprovedeno) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_eventlog_trigger_clear_matches(uint32_t kind);

/* ============================================================================
 * Freeze Bytes - zafrození bajtu z kontextového menu Memory Browseru
 *
 * Tabulku zafrozených bajtů čte emu vlákno jednou za snímek ve
 * freeze_apply_all(). Přidání i odebrání záznamu proto vykoná emu vlákno
 * v drainu fronty; UI vlákno na výsledek synchronně čeká. Při timeoutu
 * vyzvednutí se příkaz zruší a NEprovede. Čtení tabulky z UI
 * (freeze_is_frozen pro text menu) zůstává přímé - jen zobrazení.
 * ============================================================================ */

/**
 * @brief Limit vyzvednutí příkazů Freeze Bytes emu vláknem (ms).
 *
 * Stejná úvaha jako u DBG_UI_IO_CMD_TIMEOUT_MS: za běhu emu vlákno
 * vyzvedá frontu na konci snímku, v pauze hned. Hodnota 1000 ms
 * [neověřeno] měřením.
 */
#define DBG_UI_FREEZE_CMD_TIMEOUT_MS 1000

/**
 * @brief Zafrozí bajt (nebo aktualizuje hodnotu) přes DBGAPI_CMD_FREEZE_ADD.
 *
 * @param region_kind en_REGION_KIND regionu (dbgapi_regions.h).
 * @param sub_id      Disambiguator banku / plane.
 * @param offset      Offset v rámci regionu.
 * @param value       Hodnota, kterou bude freeze_apply_all() zapisovat.
 * @return true pokud emu vlákno záznam přidalo / aktualizovalo; false pokud
 *         je tabulka plná (FREEZE_MAX_ENTRIES), při timeoutu (příkaz
 *         neproveden, tabulka beze změny) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_freeze_add(int region_kind, int sub_id, uint32_t offset,
                       uint8_t value);

/**
 * @brief Uvolní zafrozený bajt přes DBGAPI_CMD_FREEZE_REMOVE.
 *
 * @param region_kind en_REGION_KIND regionu.
 * @param sub_id      Disambiguator banku / plane.
 * @param offset      Offset v rámci regionu.
 * @return true pokud emu vlákno záznam odebralo; false pokud záznam
 *         neexistoval, při timeoutu (příkaz neproveden) nebo jiné chybě
 *         submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_freeze_remove(int region_kind, int sub_id, uint32_t offset);

/* ============================================================================
 * Callstack - zapnutí / vypnutí a vyprázdnění (panel Callstack)
 *
 * Shadow stack a statistiky mění emu vlákno v Z80 CALL/RET hoocích.
 * Zapnutí / vypnutí (registrace hooků) i vyprázdnění proto vykoná emu
 * vlákno v drainu fronty; UI vlákno na výsledek synchronně čeká. Při
 * timeoutu vyzvednutí se příkaz zruší a NEprovede.
 * ============================================================================ */

/**
 * @brief Limit vyzvednutí příkazů panelu Callstack emu vláknem (ms).
 *
 * Stejná úvaha jako u DBG_UI_IO_CMD_TIMEOUT_MS. Hodnota 1000 ms
 * [neověřeno] měřením.
 */
#define DBG_UI_CALLSTACK_CMD_TIMEOUT_MS 1000

/**
 * @brief Zapne / vypne callstack přes DBGAPI_CMD_CALLSTACK_SET_ACTIVE.
 *
 * Emu vlákno zavolá callstack_set_active(): zapnutí zaregistruje Z80
 * CALL/RET hooky a vyprázdní shadow stack, vypnutí hooky odregistruje.
 *
 * @param active          true = zapnout, false = vypnout.
 * @param[out] out_active Při úspěchu g_callstack_active po provedení; NULL =
 *                        nezajímá. Při neúspěchu se nemění.
 * @return true pokud emu vlákno příkaz provedlo, false při timeoutu (příkaz
 *         neproveden, stav beze změny) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_callstack_set_active(bool active, bool *out_active);

/**
 * @brief Vyprázdní shadow stack a statistiky přes DBGAPI_CMD_CALLSTACK_RESET.
 *
 * @return true pokud emu vlákno callstack_reset() provedlo, false při
 *         timeoutu (příkaz neproveden) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_callstack_reset(void);

/* ============================================================================
 * Vynucený refresh obrazovky (Ctrl+R v okně debuggeru, menu Emulation)
 * ============================================================================ */

/**
 * @brief Limit vyzvednutí příkazu refresh obrazovky emu vláknem (ms).
 *
 * Stejná úvaha jako u DBG_UI_IO_CMD_TIMEOUT_MS: za běhu emu vlákno
 * vyzvedá frontu na konci snímku, v pauze hned. Hodnota 1000 ms
 * [neověřeno] měřením.
 */
#define DBG_UI_SCREEN_REFRESH_TIMEOUT_MS 1000

/**
 * @brief Vynutí plný refresh obrazovky přes DBGAPI_CMD_SCREEN_REFRESH.
 *
 * Emu vlákno zavolá mzarch_forced_full_screen_refresh(): přegeneruje
 * framebuffer z VRAM (border i screen) a dokončí snímek, takže se obraz
 * překreslí i v pauze (typicky při krokování).
 *
 * @return true pokud emu vlákno refresh provedlo, false při timeoutu
 *         (příkaz neproveden) nebo jiné chybě submitu.
 *
 * @pre Volat z UI vlákna; blokuje až do dokončení příkazu.
 */
bool dbg_ui_screen_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_DBGAPI_HELPERS_H */
