/**
 * @file vram_sim.h
 * @brief Stateless simulátor MZ-800 VRAM přístupů (CPU R/W + pixel render).
 *
 * Modulární knihovna která zduplikuje chování emu modulů `mz800_vramctrl`
 * + `mz800_hwscroll` bez závislosti na globálním emu state. Umožňuje:
 *
 *  - Memory Browser CPU R/W view (Live, banking-off, override) - simulace
 *    "co by CPU dostalo / co by zapsalo" do VRAM bez nutnosti reálně
 *    spustit MEM operaci na emu CPU.
 *  - VRAM Viewer (budoucí mutant) - pixel extraction z plane dat pro canvas
 *    rendering nezávisle na běžícím emu.
 *
 * Lib je arch-independent v rámci public API - žádné `g_vramctrl`, `g_gdg`,
 * `g_hwscroll`. Vše bere přes `st_VRAM_SIM_STATE` snapshot. Adapter funkce
 * `vram_sim_capture_from_emu()` je v `vram_sim.c` a kompiluje se pouze
 * pro MZARCH=800 build.
 *
 * Hot path discipline: lib NESMÍ být volána z emu CPU/render hot path.
 * Je to off-cycle pomocná lib pro debuggery/browsery.
 *
 * Reference port:
 *  - `src/emulator/mzarch/mz800/gdg/mz800_vramctrl.c` (read/write per-mode)
 *  - `src/emulator/mzarch/mz800/gdg/mz800_hwscroll.h` (shift)
 *  - `src/emulator/mzarch/mz800/gdg/mz800_gdg.h` (DMD flag konstanty)
 *
 * Bit-for-bit identita: `vram_sim_cpu_read(state, addr)` MUSÍ vrátit
 * identický bajt jako `vramctrl_mz800_memop_read_byte(addr)` pokud `state`
 * odpovídá současnému emu state. Stejně tak `vram_sim_cpu_write()` musí
 * modifikovat plane data identicky jako `vramctrl_mz800_memop_write_byte()`.
 */

#ifndef VRAM_SIM_H
#define VRAM_SIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Konstanty
 * ------------------------------------------------------------------------- */

/** Velikost jednoho VRAM plane v bajtech (8 KB). */
#define VRAM_SIM_PLANE_SIZE 0x2000

/** Počet plane (I, II, III, IV). */
#define VRAM_SIM_PLANE_COUNT 4

/** Maximální počet plane lokací které může jeden pixel vrátit (320@16). */
#define VRAM_SIM_MAX_PIXEL_LOCATIONS 4

/* ---------------------------------------------------------------------------
 * Enumerace
 * ------------------------------------------------------------------------- */

/**
 * @brief Write Format Mode - jak se interpretuje WF_MODE z portu 0xCC.
 *
 * Hodnoty kopírují `WFR_MODE` enum v `mz800_vramctrl.h`. Sim používá vlastní
 * enum aby public API nezáviselo na emu hlavičkách (kromě adapteru).
 *
 * Mapování z RAW WF portu (vrchní 3 bity):
 *  - 000 (0) = SINGLE
 *  - 001 (1) = EXOR
 *  - 010 (2) = OR
 *  - 011 (3) = RESET
 *  - 10x (4) = REPLACE (bit 0 maskován při WM2=1)
 *  - 11x (6) = PSET    (bit 0 maskován při WM2=1)
 */
typedef enum {
    VRAM_SIM_WF_SINGLE  = 0, /**< přímý zápis hodnoty do vybrané plane */
    VRAM_SIM_WF_EXOR    = 1, /**< XOR hodnoty se současným obsahem */
    VRAM_SIM_WF_OR      = 2, /**< OR hodnoty se současným obsahem */
    VRAM_SIM_WF_RESET   = 3, /**< (~value) AND current = vynulování bitů */
    VRAM_SIM_WF_REPLACE = 4, /**< value do vybrané plane, 0 do ostatních dostupných */
    VRAM_SIM_WF_PSET    = 6  /**< value OR current ve vybrané, RESET v ostatních */
} en_VRAM_SIM_WF_MODE;

/**
 * @brief Dekódovaný video mód z DMD registru.
 *
 * Devět hodnot odpovídá DMD bitům 0-3 v 800 modu plus speciální MZ-700 mód
 * (bit 3 = 1). Hodnoty se používají jako index do dispatch tabulek.
 */
typedef enum {
    VRAM_SIM_MODE_320_4_A   = 0, /**< 320x200@4 banka A - plane I, II */
    VRAM_SIM_MODE_320_4_B   = 1, /**< 320x200@4 banka B - plane III, IV */
    VRAM_SIM_MODE_320_16    = 2, /**< 320x200@16 - plane I, II, III, IV */
    VRAM_SIM_MODE_320_UNDOC = 3, /**< DMD 0x03 - nedokumentovaný 320 mód */
    VRAM_SIM_MODE_640_2_A   = 4, /**< 640x200@2 banka A - plane I (sudé) + II (liché) */
    VRAM_SIM_MODE_640_2_B   = 5, /**< 640x200@2 banka B - plane III (sudé) + IV (liché) */
    VRAM_SIM_MODE_640_4     = 6, /**< 640x200@4 - I+III (sudé) + II+IV (liché) */
    VRAM_SIM_MODE_640_UNDOC = 7, /**< DMD 0x07 - nedokumentovaný 640 mód */
    VRAM_SIM_MODE_MZ700     = 8, /**< DMD bit 3 = 1 - MZ-700 charakterový mód */
    VRAM_SIM_MODE_COUNT          /**< počet hodnot (pro dispatch table size) */
} en_VRAM_SIM_MODE;

/* ---------------------------------------------------------------------------
 * Datové struktury
 * ------------------------------------------------------------------------- */

/**
 * @brief Snapshot stavu MZ-800 GDG/vramctrl/hwscroll pro stateless sim.
 *
 * Obsahuje vše potřebné pro simulaci CPU R/W přístupu do VRAM bez závislosti
 * na běžícím emu. Konzument (Memory Browser / VRAM Viewer) si state připraví:
 *  - kopií z aktuálního emu state přes `vram_sim_capture_from_emu()`
 *  - custom konstrukcí (override mode, snapshot z .mzs souboru, atd.)
 *
 * Invariant: `plane_data[i]` musí ukazovat na min `VRAM_SIM_PLANE_SIZE` bajtů
 * dat (nebo být NULL pro plane III/IV pokud `has_exvram == 0`). Pointer
 * lifetime je odpovědnost volajícího - musí být platný po celou dobu volání
 * sim funkcí.
 *
 * Pro write API musí být `plane_data_writable[i]` non-NULL pro každý plane
 * který má být zápisem ovlivněn. NULL znamená read-only - sim tichý skip.
 *
 * VBANK pozn.: WF a RF sdílejí latch bit 4 (`wfrf_vbank`). Pokud konzument
 * státu mění WF nebo RF, musí aktualizovat i `wfrf_vbank` aby reflektoval
 * poslední zápis (viz `vramctrl_mz800_set_wf_rf_reg` v emu kódu).
 */
typedef struct st_VRAM_SIM_STATE {
    /* --- GDG DMD register (port 0xCE) --- */
    uint8_t dmd;                /**< DMD bity 0-3: VBANK, HICOLOR, SCRW640, MZ700 */

    /* --- RF register (port 0xCD) dekódovaný --- */
    uint8_t rf_plane;           /**< bity 0-3: plane I-IV select */
    uint8_t rf_search;          /**< 0 = READ, 1 = SEARCH */

    /* --- WF register (port 0xCC) dekódovaný --- */
    uint8_t wf_plane;           /**< bity 0-3: plane I-IV select */
    uint8_t wf_mode;            /**< en_VRAM_SIM_WF_MODE hodnota */

    /* --- Sdílený VBANK latch (RF/WF bit 4) - CPU R/W banka --- */
    uint8_t wfrf_vbank;         /**< 0 = banka A (I/II), 1 = banka B (III/IV) */

    /* --- HW scroll (rozkódované hodnoty, jak je v emu st_HWSCROLL) --- */
    uint16_t hwscroll_sof;      /**< Scroll Offset - 10-bit << 3, max 0x1F40 */
    uint16_t hwscroll_sw;       /**< Scroll Width - 7-bit << 6 */
    uint16_t hwscroll_ssa;      /**< Scroll Start Addr - 7-bit << 6 */
    uint16_t hwscroll_sea;      /**< Scroll End Addr - 7-bit << 6 */
    uint8_t  hwscroll_enabled;  /**< 1 = scroll aktivní (precomputed dle podmínek) */

    /* --- Plane data pointery (4× 8 KB) --- */
    const uint8_t *plane_data[VRAM_SIM_PLANE_COUNT];   /**< [0]=I, [1]=II, [2]=III, [3]=IV */
    uint8_t       *plane_data_writable[VRAM_SIM_PLANE_COUNT]; /**< NULL = read-only */

    /* --- HW konfigurace --- */
    uint8_t has_exvram;         /**< 0 = plane III/IV nepřítomny (read = 0xFF, write ignored) */
} st_VRAM_SIM_STATE;

/**
 * @brief Inverze pixel → VRAM lokace.
 *
 * Vrací informaci kterou plane + offset + bit obsahuje zdroj pixelu. Pro
 * multi-plane módy (320@16, 320@4, 640@4) vrací více lokací.
 */
typedef struct st_VRAM_PIXEL_LOC {
    int plane_index;       /**< 0..3 = I..IV */
    uint16_t byte_offset;  /**< 0..0x1FFF (offset uvnitř plane) */
    uint8_t bit_in_byte;   /**< 0..7, MSB-first (bit 7 = nejlevější pixel) */
} st_VRAM_PIXEL_LOC;

/* ---------------------------------------------------------------------------
 * Snapshot helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Vyplní state z aktuálního emu g_vramctrl/g_gdg/g_hwscroll/VRAM.
 *
 * Adapter funkce - kompiluje se pouze pro MZARCH=800 build (na 700/1500
 * není přítomna). Side-effect free, čte jen emu globals.
 *
 * @param[out] state Snapshot (vše naplněno).
 */
extern void vram_sim_capture_from_emu(st_VRAM_SIM_STATE *state);

/**
 * @brief Připojí pouze plane data pointery z emu (ne registry).
 *
 * Pro override view: konzument si zachová své vlastní DMD/RF/WF/scroll, ale
 * chce číst plane data z aktuálního emu (live data, ne snapshot).
 *
 * @param[in,out] state Existující state (jen plane_data[]/plane_data_writable[] se přepíše).
 */
extern void vram_sim_attach_emu_planes(st_VRAM_SIM_STATE *state);

/**
 * @brief Přepočítá `hwscroll_enabled` flag dle podmínek.
 *
 * Replika podmínek z `hwscroll_regs_changed()`:
 *  - SOF > 0
 *  - SSA <= 0x1E00
 *  - SEA >= 0x0140
 *  - SW > SOF
 *  - SEA > SSA
 *
 * Rovnost SW == SEA - SSA podmínkou není: na MZ-800 její porušení scroll
 * nevypne (mz800-knowledge hw/10c-hw-scroll.md, OPRAVA 2026-11-22).
 *
 * Volat po manuální změně SOF/SW/SSA/SEA v state.
 *
 * @param[in,out] state
 */
extern void vram_sim_recompute_hwscroll_enabled(st_VRAM_SIM_STATE *state);

/* ---------------------------------------------------------------------------
 * CPU R/W simulace
 * ------------------------------------------------------------------------- */

/**
 * @brief Simulace CPU read z VRAM - replika `vramctrl_mz800_memop_read_byte()`.
 *
 * Pro logické Z80 adresy 0x8000-0x9FFF (320 mode) nebo 0x8000-0xBFFF
 * (640 mode). Vrací bajt jako by CPU dostala podle aktuálního RF/DMD
 * dispatch + HW scroll. Side-effect free (nemodifikuje state).
 *
 * V MZ-700 modu (DMD bit 3 = 1) vrací 0xFF - sim 800-only.
 *
 * @param[in] state  Snapshot
 * @param[in] addr   Z80 adresa (nižších 14 bitů relevantních)
 * @return Bajt jak by CPU dostala
 */
extern uint8_t vram_sim_cpu_read(const st_VRAM_SIM_STATE *state, uint16_t addr);

/**
 * @brief Simulace CPU write do VRAM - replika `vramctrl_mz800_memop_write_byte()`.
 *
 * Modifikuje plane data podle WF_MODE (může zapsat do víc plane najednou
 * v REPLACE/PSET módech). Vyžaduje `state->plane_data_writable[]` non-NULL
 * pro každý plane který má být zapsán - jinak tichý skip toho plane.
 *
 * V MZ-700 modu (DMD bit 3 = 1) tichý no-op - sim 800-only.
 *
 * @param[in,out] state  Snapshot (plane data se modifikují)
 * @param[in]     addr   Z80 adresa
 * @param[in]     value  Bajt k zápisu (interpretace dle wf_mode)
 */
extern void vram_sim_cpu_write(st_VRAM_SIM_STATE *state, uint16_t addr, uint8_t value);

/* ---------------------------------------------------------------------------
 * Pixel rendering API (basic)
 * ------------------------------------------------------------------------- */

/**
 * @brief Vrátí palette index pro pixel na canvas pozici (x, y).
 *
 * Vstup: state s plane data + DMD mode. Výstup: 0..15 dle počtu bitů per
 * pixel v aktuálním režimu:
 *  - 320@4 A/B, 640@4: 2-bit (0..3)
 *  - 320@16: 4-bit (0..15)
 *  - 640@2 A/B: 1-bit (0..1)
 *  - undoc 0x03/0x07, MZ-700: vrací -1 (neimplementováno v V-1d)
 *
 * Hot path safe: žádné malloc, side-effect free.
 *
 * @param[in] state  Snapshot
 * @param[in] x      0..319 nebo 0..639 dle DMD SCRW640
 * @param[in] y      0..199
 * @return Palette index 0..15, nebo -1 pokud (x,y) out of range / neimplementovaný mód
 */
extern int vram_sim_get_pixel_palette_index(const st_VRAM_SIM_STATE *state,
                                            int x, int y);

/**
 * @brief Inverze: canvas pozice (x, y) → seznam plane lokací.
 *
 * Pro "klikni na pixel v canvas → najdi bajt v Memory Browser hex view".
 * Vrací 1-4 lokace dle DMD modu (320@4: 2, 320@16: 4, 640@2: 1, 640@4: 2).
 *
 * @param[in]  state           Snapshot
 * @param[in]  x, y            Canvas pozice
 * @param[out] out_locations   Pole min `VRAM_SIM_MAX_PIXEL_LOCATIONS` položek
 * @return Počet vyplněných položek (1-4), nebo 0 pokud out of range / nepodporovaný mód
 */
extern int vram_sim_locate_pixel(const st_VRAM_SIM_STATE *state,
                                 int x, int y,
                                 st_VRAM_PIXEL_LOC *out_locations);

/* ---------------------------------------------------------------------------
 * Metadata helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Dekódování DMD bajtu na enum video mód.
 *
 * Použije nižší 4 bity DMD. Bit 3 (MZ700) má přednost.
 *
 * @param[in] dmd Hodnota DMD registru (0x00..0xFF, jen low 4 bity relevantní)
 * @return en_VRAM_SIM_MODE hodnota
 */
extern en_VRAM_SIM_MODE vram_sim_decode_mode(uint8_t dmd);

/**
 * @brief Vrací jméno video režimu pro UI zobrazení.
 *
 * Texty anglicky pro `_()` lokalizaci ve volající UI vrstvě.
 *
 * @param[in] mode
 * @return Const string s názvem, nebo "Unknown" pro out-of-range
 */
extern const char *vram_sim_mode_name(en_VRAM_SIM_MODE mode);

/**
 * @brief Vrací počet bitů per pixel v daném režimu.
 *
 * @param[in] mode
 * @return 1, 2, 4, nebo 0 pro neimplementované módy (undoc, MZ-700)
 */
extern int vram_sim_bits_per_pixel(en_VRAM_SIM_MODE mode);

/**
 * @brief Vrací canvas dimenze pro daný režim.
 *
 * @param[in]  mode
 * @param[out] out_width  320, 640 nebo 0 pro nepodporovaný mód
 * @param[out] out_height 200 nebo 0 pro nepodporovaný mód
 */
extern void vram_sim_canvas_dimensions(en_VRAM_SIM_MODE mode,
                                       int *out_width, int *out_height);

/**
 * @brief Bitmaska plane aktivních pro display render v aktuálním režimu.
 *
 * Bity 0-3 = plane I-IV. Liší se od `cpu_planes()` - display je řízen DMD,
 * CPU avlb je řízen DMD + WFRF_VBANK + SCRW640.
 *
 * @param[in] state
 * @return Bitmaska (např. 0x03 = plane I+II v 320@4 A)
 */
extern uint8_t vram_sim_display_planes(const st_VRAM_SIM_STATE *state);

/**
 * @brief Bitmaska plane dostupných pro CPU R/W (`avlb_plane` v emu kódu).
 *
 * Pravidla:
 *  - HICOLOR: všechny plane jsou dostupné (jinak žádné z bitu 1 efektu)
 *  - VBANK: doplní plane I+II (A) nebo III+IV (B)
 *  - SCRW640: výsledek AND s (I+III) - jen "lichá strana" pro sudý bajt
 *
 * @param[in] state
 * @return Bitmaska plane bits 0-3
 */
extern uint8_t vram_sim_cpu_planes(const st_VRAM_SIM_STATE *state);

/* ---------------------------------------------------------------------------
 * HW scroll API
 * ------------------------------------------------------------------------- */

/**
 * @brief Aplikuje HW scroll posun na fyzickou plane adresu.
 *
 * Replika `hwscroll_shift_addr()` bez globálního state. Pokud
 * `hwscroll_enabled == 0`, vrací `phy_offset` beze změny.
 *
 * Pro adresy mimo `[SSA, SEA)` vrací `phy_offset` beze změny.
 * Pro adresy uvnitř scroll area:
 *  - addr >= SEA - SOF: posun dolů (addr + SOF - SW)
 *  - jinak: posun nahoru (addr + SOF)
 *
 * Posunutá adresa přeteče v 13 bitech (výsledek `& 0x1FFF`) stejně jako
 * v `hwscroll_shift_addr()` emulátoru (HWSCRL_ADDR_MASK), takže je vždy
 * platný index do `plane_data[]`.
 *
 * @param[in] state
 * @param[in] phy_offset  Fyzický offset uvnitř plane (typicky 0..0x1FFF)
 * @return Posunutý offset
 */
extern uint16_t vram_sim_hwscroll_shift(const st_VRAM_SIM_STATE *state,
                                        uint16_t phy_offset);

/**
 * @brief Zjistí, zda se na adresu CPU přístupu (čtení i zápis) uplatní HW scroll.
 *
 * Replika `vramctrl_mz800_cpu_scroll_applies()`: GDG přičítá SOF k adrese CPU
 * přístupu jen tehdy, když B/A (`wfrf_vbank`) ukazuje na zobrazenou frame,
 * tj. `wfrf_vbank == (dmd & 0x01)`. V režimech s HICOLOR (DMD bit 1 = 1,
 * 320x200@16 a 640x200@4) se posun uplatní vždy. Přístup do nezobrazené frame
 * jde na neposunutou adresu. Zobrazení (pixel render) posouvá vždy, tato
 * podmínka se ho netýká.
 *
 * Funkce neřeší, zda je scroll vůbec aktivní (`hwscroll_enabled`) - to dělá
 * `vram_sim_hwscroll_shift()`.
 *
 * @param[in] state Stav simulátoru; nesmí být NULL.
 * @return 1 = adresu přepočítat přes `vram_sim_hwscroll_shift()`, 0 = beze změny.
 */
extern int vram_sim_cpu_hwscroll_applies(const st_VRAM_SIM_STATE *state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VRAM_SIM_H */
