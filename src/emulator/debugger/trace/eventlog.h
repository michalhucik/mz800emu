/**
 * @file   eventlog.h
 * @brief  Event Log - in-memory ring buffer významných HW/CPU událostí.
 *
 * Event Viewer (Vlna 1 mutantu event-viewer). Samostatný subsystém vedle
 * trace-suite (cputrack/iorqlog/intlog/hwlog/marklog). Záznam byl
 * původně 24 B bit-identický s tlog chunky; od Vlny 4 Commit 24 nabobtnal
 * na 26 B kvůli ambient state poli (= eventlog ring NENÍ nadále shodný s
 * tlog chunky, eventuální budoucí export potřebuje konverzi).
 *
 *  - Cíl = real-time UI tab "Events" (ne post-mortem souborová analýza).
 *  - Storage = single in-memory ring, default 50 000 záznamů (~1.3 MB).
 *  - Hot-path gate = @ref TEST_TRACE_EVENTLOG_ACTIVE + per-kategorie
 *    bitmask (@ref g_eventlog_active_mask, 64 bitů).
 *  - Lifecycle = OFF / WITH_WINDOW (default) / ALWAYS (paralelně s tlog).
 *
 * Fan-out emit pointy v hwlog/intlog/iorqlog/marklog/bp_event přidají
 * v Commit 2-3 paralelní volání @ref eventlog_record() vedle existujícího
 * @c tlog_writer_record() (= obě cesty mohou běžet zároveň, nezávislé).
 *
 * Per-event záznam (32 B, alignment 8 B):
 *
 * | Offset | Velikost | Pole              |
 * |--------|----------|-------------------|
 * | 0      | 8 B      | pxclk_total       |
 * | 8      | 4 B      | screens_total     |
 * | 12     | 4 B      | pxclk_in_screen   |
 * | 16     | 1 B      | category          |
 * | 17     | 1 B      | subtype           |
 * | 18     | 2 B      | pc                |
 * | 20     | 4 B      | payload           |
 * | 24     | 2 B      | ambient           |
 * | 26     | 6 B      | reserved (= 0)    |
 *
 * Poznámka k velikosti: struktura má 8B alignment (kvůli @c uint64_t
 * na začátku), takže přidání 2B @c ambient pole způsobí 6B trailing
 * padding (= sizeof 32 B). Tento prostor je explicitně vyhrazen v
 * @c reserved poli pro budoucí ambient rozšíření (Vlna 4+5 follow-up).
 * Drift z původních 24 B na 32 B = +8 B per event (~400 kB navíc při
 * default 50 000 ringu).
 *
 * Pro UI dekódování (scanline + px) se nepoužívá cache - dekóduje se
 * per-render z @c pxclk_in_screen pomocí @c VIDEO_GET_SCREEN_ROW/COL
 * maker (= shodné s tlog chunk replay).
 *
 * Threading: hot-path zapisuje v emu vlákně, UI čte v UI vlákně bez
 * locku. Pole 24 B nejsou jednou atomickou operací, ale per-field
 * zápisy jsou aligned a krátký data race při čtení znamená nejvýše
 * zobrazený "ulomek" eventu (= acceptable, viz io_history pattern).
 *
 * @author Michal Hucik <hucik@ordoz.com>
 *
 * Licence: GPLv3
 */

#ifndef EVENTLOG_H
#define EVENTLOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <assert.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ===========================================================================
 *  Kategorie eventů (= bit index v g_eventlog_active_mask, 0..63)
 * =========================================================================== */

/**
 * @brief Kategorie eventu pro Event Viewer.
 *
 * Mapování přímo na trace-suite subsystémy + drobné rozšíření o
 * @c BP_FIRE a @c CPU_CTRL (vlastní fan-out emit pointy). Hodnota slouží
 * současně jako bit index v @ref g_eventlog_active_mask, proto MUSÍ
 * zůstat v rozsahu @c [0..63].
 *
 * @note Stabilní součást Event Viewer API - nové kategorie přidávat na
 *       konec před @c EVENTLOG_CAT_COUNT, nepřemapovávat existující
 *       (= persistované filter masky v cfg).
 */
typedef enum en_EVENTLOG_CATEGORY
{
    EVENTLOG_CAT_CPU_INT       =  0, /**< CPU interrupt state (IM/IFF/RETI/EI). */
    EVENTLOG_CAT_CPU_PIN_EDGE  =  1, /**< INT/NMI pin edge (zdroj signálu). */
    EVENTLOG_CAT_IRQ_ACK_IM2   =  2, /**< IM2 IRQ ack (vector + ISR addr). */
    EVENTLOG_CAT_IORQ_IN       =  3, /**< CPU IN port read. */
    EVENTLOG_CAT_IORQ_OUT      =  4, /**< CPU OUT port write. */
    EVENTLOG_CAT_MMIO_R        =  5, /**< MMIO read (0xE000-0xE008 region). */
    EVENTLOG_CAT_MMIO_W        =  6, /**< MMIO write. */
    EVENTLOG_CAT_GDG_MODE      =  7, /**< GDG DMD (display mode descriptor). */
    EVENTLOG_CAT_GDG_BANKING   =  8, /**< GDG banking porty (0xE0-0xE6). */
    EVENTLOG_CAT_GDG_HWSCROLL  =  9, /**< GDG SOF/WID/scroll. */
    EVENTLOG_CAT_GDG_COLORS    = 10, /**< BORDER/PAL/PCG paleta. */
    EVENTLOG_CAT_GDG_VIDEO     = 11, /**< VBLN/VS/HBLN/HS edges. */
    EVENTLOG_CAT_PIO8255       = 12, /**< 8255 PPI port A/B/C/CW. */
    EVENTLOG_CAT_CTC8253       = 13, /**< 8253 CTC counter/control. */
    EVENTLOG_CAT_PIOZ80        = 14, /**< Z80 PIO mode/vector/mask. */
    EVENTLOG_CAT_PSG           = 15, /**< SN76489 register write. */
    EVENTLOG_CAT_FDC           = 16, /**< WD279x register write. */
    EVENTLOG_CAT_MEMEXT        = 17, /**< Memory extension banking. */
    EVENTLOG_CAT_BP_FIRE       = 18, /**< Breakpoint fire (halt/mark/continue/ignore). */
    EVENTLOG_CAT_USER_MARK     = 19, /**< BP mark "name" action z marklog. */
    EVENTLOG_CAT_CPU_CTRL      = 20, /**< HALT enter/exit, RST 00..38. */
    EVENTLOG_CAT_GDG_WFRF      = 21, /**< GDG Write Format / Read Format register. */
    EVENTLOG_CAT_QD            = 22, /**< Quick Disk register write. */
    EVENTLOG_CAT_RD            = 23, /**< Ramdisk (Pezik / STD) write. */
    EVENTLOG_CAT_SYS           = 24, /**< System lifecycle: reset, snapshot, MZF inject. */

    EVENTLOG_CAT_COUNT               /**< Počet definovaných kategorií. */
} en_EVENTLOG_CATEGORY;

/**
 * @brief Compile-time pojistka: kategorie se musí vejít do 64-bit masky.
 */
#ifdef __cplusplus
static_assert ( (int) EVENTLOG_CAT_COUNT <= 64,
                "en_EVENTLOG_CATEGORY musi mit max 64 hodnot kvuli uint64 active_mask" );
#elif defined(__GNUC__) || defined(__clang__)
_Static_assert ( (int) EVENTLOG_CAT_COUNT <= 64,
                 "en_EVENTLOG_CATEGORY musi mit max 64 hodnot kvuli uint64 active_mask" );
#endif

/* ===========================================================================
 *  Subtype enum - IORQ kategorie
 * =========================================================================== */

/**
 * @brief Subtype pro kategorie @c EVENTLOG_CAT_IORQ_IN a
 *        @c EVENTLOG_CAT_IORQ_OUT.
 *
 * Rozlišuje "normální" IORQ (= port je mapovaný na nějakou periferii,
 * která vrátila / přijala data) od "ghost" cyklu, kdy port není mapovaný
 * a sběrnice plove (= NORMAL vs UNCONNECTED).
 *
 * Zdroj rozlišení v hot-path = globální flag @c g_tracelog_iorq_unconnected,
 * který se v per-arch @c port_*_with_logging_cb nastavuje před voláním
 * @c g_pio8255_iorq_unconnected_cb a vyhodnocuje se PO callback returnu,
 * v eventlog fan-out bloku.
 *
 * @note Hodnoty stabilní součást Event Viewer API (cfg / UI filtry je
 *       perzistuje). Platí symetricky pro @c IORQ_IN i @c IORQ_OUT - i
 *       teoretický nemapovaný OUT cyklus by se vyhodnotil jako
 *       @c UNCONNECTED, ačkoliv v praxi se ghost write detekuje stejně
 *       jako ghost read (= port_*_with_logging_cb -> unconnected branch).
 */
typedef enum en_EVENTLOG_IORQ_SUB
{
    EVENTLOG_IORQ_SUB_NORMAL      = 0, /**< Port mapován, periferie vrátila / přijala data. */
    EVENTLOG_IORQ_SUB_UNCONNECTED = 1, /**< Ghost cyklus - port nemapován, sběrnice plovoucí. */
} en_EVENTLOG_IORQ_SUB;

/* ===========================================================================
 *  Subtype enum - BP_FIRE kategorie
 * =========================================================================== */

/**
 * @brief Subtype pro kategorii @c EVENTLOG_CAT_BP_FIRE.
 *
 * Každé fíření breakpointu (= podmínka @c bp_expr_eval splněna a action
 * skript byl klasifikován) generuje právě jeden event této kategorie, bez
 * ohledu na typ akce. Subtype popisuje, "co BP dělá při fire", podle
 * dominantního statementu DSL skriptu (viz @c bp_action_classify_subtype()).
 *
 * Mapping je 1:1 sjednocený s @c en_BP_ACTION_SUB v @c bp_action.h, aby
 * caller v @c breakpoints_enforce() nemusel hodnoty překládat (= jen
 * casted uint8 do eventlog payloadu).
 *
 * Payload (32 b) pro tuto kategorii:
 *
 * | Bity   | Význam                                                      |
 * |--------|-------------------------------------------------------------|
 * |  0..15 | @c bpt->id (BP identifikátor, max 65535)                    |
 * | 16..23 | @c g_bp_fire_reason (ambient @c en_BP_REASON v okamžiku fire) |
 * | 24..31 | rezervováno (= 0)                                           |
 *
 * Field @c pc v @ref st_EVENTLOG_EVENT obsahuje @c cpu->pc v okamžiku
 * fire (= adresa instrukce která způsobila trigger).
 *
 * @note Hodnoty stabilní součást Event Viewer API (cfg / UI filtry je
 *       perzistuje). @c IGNORE je vyhrazená pro budoucí DSL rozšíření -
 *       aktuálně se nemůže reálně objevit (= classifier ji nikdy nevrátí).
 */
typedef enum en_EVENTLOG_BP_FIRE_SUB
{
    EVENTLOG_BP_FIRE_SUB_HALT     = 0, /**< Default akce - emu halt (= action prázdná). */
    EVENTLOG_BP_FIRE_SUB_MARK     = 1, /**< Action @c mark "name" - paralelně @c USER_MARK z marklog. */
    EVENTLOG_BP_FIRE_SUB_CONTINUE = 2, /**< Action log / poke / set / var / continue. */
    EVENTLOG_BP_FIRE_SUB_IGNORE   = 3, /**< Rezervováno (DSL aktuálně nemá "ignore" stmt). */
    EVENTLOG_BP_FIRE_SUB_ENABLE   = 4, /**< Action @c enable @c <bp>. */
    EVENTLOG_BP_FIRE_SUB_DISABLE  = 5, /**< Action @c disable / @c disable_self. */
} en_EVENTLOG_BP_FIRE_SUB;

/* ===========================================================================
 *  Subtype enum - CPU_CTRL kategorie
 * =========================================================================== */

/**
 * @brief Subtype pro kategorii @c EVENTLOG_CAT_CPU_CTRL.
 *
 * Pokrývá HALT entry/exit (= přechody @c cpu->halted 0->1 / 1->0) a osm
 * RST opcodů (@c RST @c 00h ... @c RST @c 38h). Zdrojem eventů je z80 lib
 * callback @c z80_cpu_ctrl_event_cb (viz @c libs/cpu-z80/z80.h), který se
 * v mzarch konzumentu (@c mzarch_cpu_ctrl_event_cb v
 * @c emulator/mzarch/interrupt.c) přemapuje do této eventlog kategorie.
 *
 * Hodnoty MUSÍ být 1:1 s @c z80_cpu_ctrl_event_t z @c libs/cpu-z80/z80.h
 * (= konzument nemapuje hodnoty, jen cast @c uint8_t -> subtype). Pokud
 * z80 lib přidá nové control eventy, musí se synchronně rozšířit i tento
 * enum a aktualizovat regression test
 * @c test_eventlog_cpu_ctrl_subtype_enum_values.
 *
 * Payload (32 b) pro tuto kategorii: aktuálně vždy @c 0 (= veškerá
 * informace je v subtype + @c pc). RST adresa je dohledatelná z subtype
 * (@c RST @c 00h => 0x0000, @c RST @c 08h => 0x0008, ...).
 *
 * Field @c pc v @ref st_EVENTLOG_EVENT obsahuje PC z callbacku (viz
 * dokumentace @c z80_cpu_ctrl_event_cb pro přesný mapping per event type).
 *
 * @note Hodnoty stabilní součást Event Viewer API (cfg / UI filtry je
 *       perzistuje). Test @c test_eventlog_cpu_ctrl_subtype_enum_values
 *       chrání 1:1 mapping s @c z80_cpu_ctrl_event_t.
 */
typedef enum en_EVENTLOG_CPU_CTRL_SUB
{
    EVENTLOG_CPU_CTRL_SUB_HALT_ENTER = 0, /**< Instrukce HALT vykonána, @c cpu->halted 0->1. */
    EVENTLOG_CPU_CTRL_SUB_HALT_EXIT  = 1, /**< IRQ/NMI probudilo z HALT, @c cpu->halted 1->0. */
    EVENTLOG_CPU_CTRL_SUB_RST_00     = 2, /**< Opcode 0xC7 (RST 00h) dispatch. */
    EVENTLOG_CPU_CTRL_SUB_RST_08     = 3, /**< Opcode 0xCF (RST 08h) dispatch. */
    EVENTLOG_CPU_CTRL_SUB_RST_10     = 4, /**< Opcode 0xD7 (RST 10h) dispatch. */
    EVENTLOG_CPU_CTRL_SUB_RST_18     = 5, /**< Opcode 0xDF (RST 18h) dispatch. */
    EVENTLOG_CPU_CTRL_SUB_RST_20     = 6, /**< Opcode 0xE7 (RST 20h) dispatch. */
    EVENTLOG_CPU_CTRL_SUB_RST_28     = 7, /**< Opcode 0xEF (RST 28h) dispatch. */
    EVENTLOG_CPU_CTRL_SUB_RST_30     = 8, /**< Opcode 0xF7 (RST 30h) dispatch. */
    EVENTLOG_CPU_CTRL_SUB_RST_38     = 9, /**< Opcode 0xFF (RST 38h) dispatch. */
} en_EVENTLOG_CPU_CTRL_SUB;

/* ===========================================================================
 *  Subtype enum - SYS kategorie (Vlna 5 Commit 31)
 * =========================================================================== */

/**
 * @brief Subtype pro kategorii @c EVENTLOG_CAT_SYS.
 *
 * Pokrývá lifecycle eventy emulátoru, které nepatří do žádného HW subsystému:
 * reset (cold / warm), snapshot save/load, MZF inject přes tape/cmthack a
 * volitelně start/stop emu vlákna.
 *
 * Payload (32 b) per subtype:
 *
 * | Subtype             | Payload                                              |
 * |---------------------|------------------------------------------------------|
 * | COLD_RESET          | 0                                                    |
 * | WARM_RESET          | 0                                                    |
 * | SNAPSHOT_SAVE       | @ref eventlog_filename_hash() basename                |
 * | SNAPSHOT_LOAD       | @ref eventlog_filename_hash() basename                |
 * | MZF_INJECT          | @ref eventlog_filename_hash() basename                |
 * | EMU_STARTED         | 0                                                    |
 * | EMU_STOPPED         | 0                                                    |
 *
 * Field @c pc v @ref st_EVENTLOG_EVENT je @c 0 (= SYS eventy nejsou bound na
 * konkrétní CPU instrukci - emit se děje z driveru, ne z hot-path Z80 loopu).
 *
 * @note Hodnoty stabilní součást Event Viewer API (cfg / UI filtry je
 *       perzistuje). Nové subtypy přidávat na konec.
 */
typedef enum en_EVENTLOG_SYS_SUB
{
    EVENTLOG_SYS_COLD_RESET    = 0, /**< Power-on / cold reset trigger. */
    EVENTLOG_SYS_WARM_RESET    = 1, /**< Warm reset (= ne power-cycle). */
    EVENTLOG_SYS_SNAPSHOT_SAVE = 2, /**< Snapshot saved to disk. */
    EVENTLOG_SYS_SNAPSHOT_LOAD = 3, /**< Snapshot loaded from disk. */
    EVENTLOG_SYS_MZF_INJECT    = 4, /**< MZF tape file injected via cmthack / tape driver. */
    EVENTLOG_SYS_EMU_STARTED   = 5, /**< Emu main loop started (= thread spuštěn). */
    EVENTLOG_SYS_EMU_STOPPED   = 6, /**< Emu main loop stopped (= shutdown). */
} en_EVENTLOG_SYS_SUB;

/* ===========================================================================
 *  Per-event záznam (24 B)
 * =========================================================================== */

/**
 * @brief Jeden záznam v Event Viewer ringu.
 *
 * Layout sjednocen s tlog chunky (= bit-identický pro budoucí
 * export ring -> chunk). 24 B, alignment 8 B (@c uint64_t na začátku).
 *
 * @field pxclk_total      T-state counter pixel clock domény z
 *                         @c tlog_common_get_pxclk_total().
 * @field screens_total    Počet dokončených snímků (= frame number).
 * @field pxclk_in_screen  Pixel clock pozice v aktuálním snímku
 *                         (= scanline * VIDEO_SCREEN_WIDTH + col).
 * @field category         Kategorie eventu (@ref en_EVENTLOG_CATEGORY).
 * @field subtype          Per-kategorie subtype (význam dle category).
 * @field pc               CPU PC v okamžiku eventu (16-bit).
 * @field payload          Dodatečná data: port/addr/value/decoded info.
 * @field ambient          Bitově skládaný snapshot HW state v okamžiku
 *                         emit (= IFF1, IM, fire reason, banking summary).
 *                         Layout viz @ref EVENTLOG_AMBIENT_IFF1 a sousedy.
 *                         Vyplňuje se v @ref eventlog_record() přes interní
 *                         capture helper. Nulový obsah znamená "neznámý
 *                         stav" (= callers mimo standardní emit cestu).
 */
typedef struct st_EVENTLOG_EVENT
{
    uint64_t pxclk_total;
    uint32_t screens_total;
    uint32_t pxclk_in_screen;
    uint8_t  category;
    uint8_t  subtype;
    uint16_t pc;
    uint32_t payload;
    uint16_t ambient;
    uint16_t reserved16; /**< Padding pro alignment + rezerva pro budoucí ambient pole. MUSÍ být 0. */
    uint32_t reserved32; /**< Padding pro alignment + rezerva pro budoucí pole. MUSÍ být 0. */
} st_EVENTLOG_EVENT;

/**
 * @brief Compile-time pojistka: layout MUSÍ zůstat 32 B.
 *
 * Vlna 4 Commit 24 - struct rozšířen z 24 B na 32 B (= ambient pole +
 * trailing padding kvůli 8B alignment). Breaking změna oproti původnímu
 * tlog chunk layoutu (= eventlog ring již není binárně shodný s tlog
 * chunky).
 */
#ifdef __cplusplus
static_assert ( sizeof ( st_EVENTLOG_EVENT ) == 32,
                "st_EVENTLOG_EVENT MUSI mit presne 32 B (Vlna 4 ambient + padding)" );
#elif defined(__GNUC__) || defined(__clang__)
_Static_assert ( sizeof ( st_EVENTLOG_EVENT ) == 32,
                 "st_EVENTLOG_EVENT MUSI mit presne 32 B (Vlna 4 ambient + padding)" );
#endif

/* ===========================================================================
 *  Ambient state bity (Vlna 4 Commit 24)
 * =========================================================================== */

/**
 * @brief IFF1 stav (1 bit) v poli @c ambient.
 *
 * Hodnota @c g_mzarch_main.cpu->iff1 v okamžiku emit eventu. Pozor:
 * Z80 lib drží IFF1 jako @c uint8_t (= 0 / 1 / případně cokoliv jiného
 * u některých forks), v ambient bitu je @c 1 pokud byl nenulový.
 */
#define EVENTLOG_AMBIENT_IFF1            (1u << 0)

/**
 * @brief IM mode (2 bity) shift v poli @c ambient.
 *
 * Pozice bitů 1..2. Hodnoty 0 / 1 / 2 odpovídají Z80 IM 0/1/2;
 * hodnota 3 je rezervovaná (= IM mode mimo 0..2 nevyplývá z Z80 ISA).
 */
#define EVENTLOG_AMBIENT_IM_SHIFT        1

/**
 * @brief IM mode (2 bity) maska v poli @c ambient.
 */
#define EVENTLOG_AMBIENT_IM_MASK         (0x03u << EVENTLOG_AMBIENT_IM_SHIFT)

/**
 * @brief Fire reason (3 bity) shift v poli @c ambient.
 *
 * Pozice bitů 3..5. Hodnoty @ref en_EVENTLOG_AMBIENT_REASON. Mapování
 * z @c en_BP_FIRE_REASON (bp_event.h) - viz @ref eventlog_capture_ambient.
 */
#define EVENTLOG_AMBIENT_REASON_SHIFT    3

/**
 * @brief Fire reason (3 bity) maska v poli @c ambient.
 */
#define EVENTLOG_AMBIENT_REASON_MASK     (0x07u << EVENTLOG_AMBIENT_REASON_SHIFT)

/**
 * @brief Banking summary (3 bity) shift v poli @c ambient.
 *
 * Pozice bitů 6..8. Hodnoty @ref en_EVENTLOG_AMBIENT_BANKING per-arch
 * coded (= MZ-800 / MZ-700 / MZ-1500 každá vlastní mapping).
 */
#define EVENTLOG_AMBIENT_BANKING_SHIFT   6

/**
 * @brief Banking summary (3 bity) maska v poli @c ambient.
 */
#define EVENTLOG_AMBIENT_BANKING_MASK    (0x07u << EVENTLOG_AMBIENT_BANKING_SHIFT)

/**
 * @brief Bity 9..15 jsou rezervovány pro budoucí ambient pole (= Vlna 4+5
 *        follow-up). Při neznámém čtení MUSÍ být 0.
 */
#define EVENTLOG_AMBIENT_RESERVED_MASK   (0xFE00u)

/**
 * @brief Fire reason kódy (3 bity) v ambient poli.
 *
 * Mapování z @c en_BP_FIRE_REASON (bp_event.h): hodnoty 0..6 jsou 1:1,
 * hodnota @c 7 (= @c EVENTLOG_AMBIENT_REASON_NONE) reprezentuje
 * @c BP_REASON_NONE (0xFF v originálu, který se do 3 bitů nevejde).
 *
 * @note Default ambient = 0 znamená @c IFF_RESET reason. Pro rozlišení
 *       "nedefinovaný" stav se používá @c REASON_NONE = 7.
 */
typedef enum en_EVENTLOG_AMBIENT_REASON
{
    EVENTLOG_AMBIENT_REASON_IFF_RESET   = 0, /**< IFF cleared - CPU reset. */
    EVENTLOG_AMBIENT_REASON_IFF_EI      = 1, /**< IFF set - EI instrukce. */
    EVENTLOG_AMBIENT_REASON_IFF_DI      = 2, /**< IFF cleared - DI instrukce. */
    EVENTLOG_AMBIENT_REASON_IFF_INT_ACK = 3, /**< IFF cleared - INT acknowledgement. */
    EVENTLOG_AMBIENT_REASON_IFF_NMI_ACK = 4, /**< IFF1 cleared - NMI acknowledgement. */
    EVENTLOG_AMBIENT_REASON_IFF_RETI    = 5, /**< RETI signal (no-op pro IFF). */
    EVENTLOG_AMBIENT_REASON_IFF_RETN    = 6, /**< IFF1 obnoveno z IFF2 - RETN. */
    EVENTLOG_AMBIENT_REASON_NONE        = 7, /**< Žádný relevantní reason (default). */
} en_EVENTLOG_AMBIENT_REASON;

/**
 * @brief Banking summary kódy (3 bity) v ambient poli, per-arch coded.
 *
 * Hodnoty mají per-arch sémantiku - decoder MUSÍ znát aktuální MZARCH,
 * aby je správně interpretoval (viz @ref eventlog_decode_ambient).
 * Mapování per-arch:
 *
 * | Kód | MZ-800 native          | MZ-700 / 800 v MZ-700 modu | MZ-1500       |
 * |-----|------------------------|----------------------------|---------------|
 * |  0  | DEFAULT (ROM+VRAM)     | DEFAULT (ROM+RAM+TEXT)     | DEFAULT       |
 * |  1  | ALL_RAM                | ALL_RAM                    | ALL_RAM       |
 * |  2  | ROM_LOW_OFF (VRAM low) | (nepoužito)                | (nepoužito)   |
 * |  3  | ROM_HIGH_OFF           | (nepoužito)                | (nepoužito)   |
 * |  4  | CGROM_VISIBLE          | (nepoužito)                | CGROM (SPEC=1)|
 * |  5  | VRAM_640 (SCRW)        | (nepoužito)                | PCG_1 (SPEC=2)|
 * |  6  | (nepoužito)            | (nepoužito)                | PCG_2/3       |
 * |  7  | OTHER                  | OTHER                      | OTHER         |
 *
 * Implementace per-arch v @c eventlog.c - jednoduchý decode z
 * @c memmap_query(0) / @c memmap_query(8) / @c memmap_query(14).
 */
typedef enum en_EVENTLOG_AMBIENT_BANKING
{
    EVENTLOG_AMBIENT_BANKING_DEFAULT      = 0, /**< Výchozí mapping per-arch (= ROM_LOW + ROM_HIGH + arch-specific zbytek). */
    EVENTLOG_AMBIENT_BANKING_ALL_RAM      = 1, /**< Plný RAM mapping (= ROM_LOW i ROM_HIGH OFF). */
    EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF  = 2, /**< ROM low OFF, ostatní default (MZ-800). */
    EVENTLOG_AMBIENT_BANKING_ROM_HIGH_OFF = 3, /**< ROM high OFF, ostatní default (MZ-800). */
    EVENTLOG_AMBIENT_BANKING_CGROM        = 4, /**< CGROM viditelný v page region (MZ-800 native / MZ-1500 SPEC=1). */
    EVENTLOG_AMBIENT_BANKING_VRAM_640     = 5, /**< MZ-800 SCRW640 (= VRAM blok II viditelný) / MZ-1500 PCG_1. */
    EVENTLOG_AMBIENT_BANKING_PCG_HIGH     = 6, /**< MZ-1500 PCG_2 nebo PCG_3 (SPEC=3/4). */
    EVENTLOG_AMBIENT_BANKING_OTHER        = 7, /**< Neznámá / nezatříditelná konfigurace. */
} en_EVENTLOG_AMBIENT_BANKING;

/* ===========================================================================
 *  Capacity limits
 * =========================================================================== */

/**
 * @brief Default capacity ringu (= 50 000 eventů, ~1.2 MB).
 */
#define EVENTLOG_DEFAULT_CAPACITY  50000u

/**
 * @brief Minimum capacity (UI Settings slider).
 */
#define EVENTLOG_MIN_CAPACITY      10000u

/**
 * @brief Maximum capacity (UI Settings slider).
 */
#define EVENTLOG_MAX_CAPACITY     200000u

/* ===========================================================================
 *  Konfigurace + globální stav
 * =========================================================================== */

/**
 * @brief Mode lifecycle Event Log subsystému.
 *
 * Paralelní k @c en_TLOG_MODE, ale rozhodnutí "kdy je active" závisí na
 * stavu Events okna (= UI), ne na obecném "debugger_active" stavu (tlog
 * pattern). UI v Commit 7 přepíná @c g_event_viewer_window_open.
 */
typedef enum en_EVENTLOG_MODE
{
    EVENTLOG_MODE_OFF              = 0, /**< Subsystém vypnutý, žádná režie. */
    EVENTLOG_MODE_WHEN_WINDOW_OPEN = 1, /**< Aktivní jen pokud Events okno otevřené. */
    EVENTLOG_MODE_ALWAYS           = 2, /**< Aktivní trvale (= advanced). */
} en_EVENTLOG_MODE;

/**
 * @brief Konfigurace načítaná z cfg sekce @c [EVENT_LOG].
 *
 * @field mode             OFF / WHEN_WINDOW_OPEN / ALWAYS (default OFF).
 * @field capacity         Velikost ringu, clamped do
 *                         @c [EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY].
 * @field categories_mask  Bitmask povolených kategorií
 *                         (bit i = @ref en_EVENTLOG_CATEGORY hodnota i).
 *                         Default = @c 0xFFFFFFFFFFFFFFFF (= vše on).
 */
typedef struct st_EVENTLOG_CONFIG
{
    en_EVENTLOG_MODE mode;
    unsigned         capacity;
    uint64_t         categories_mask;
} st_EVENTLOG_CONFIG;

extern st_EVENTLOG_CONFIG g_eventlog_config;

/**
 * @brief Globální flag = subsystém právě zapisuje do ringu.
 *
 * Aktualizováno v @ref eventlog_recompute_active(). Hot-path emit
 * pointy v hwlog/intlog/iorqlog/marklog testují přes
 * @ref TEST_TRACE_EVENTLOG_ACTIVE.
 */
extern int g_eventlog_active;

/**
 * @brief Bitmask povolených kategorií pro hot-path filter.
 *
 * Bit @c (1ULL << category) = 1 znamená "tuto kategorii zapisuj".
 * UI checkbox v "Events" okně tento bit přepíná. Hot-path test:
 *
 * @code
 * if ( TEST_TRACE_EVENTLOG_ACTIVE
 *      && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_FOO ) ) ) {
 *     eventlog_record ( EVENTLOG_CAT_FOO, subtype, pc, payload );
 * }
 * @endcode
 *
 * @note Default po @ref eventlog_init() = @c categories_mask z cfg
 *       (= typicky vše 1).
 */
extern uint64_t g_eventlog_active_mask;

/**
 * @brief Inline-friendly check pro hot path.
 *
 * Caller je zodpovědný ještě dotestovat per-kategorie bit (viz code
 * snippet u @ref g_eventlog_active_mask). Pattern shodný s
 * @c TEST_TRACE_HWLOG_ACTIVE z hwlog.h.
 */
#define TEST_TRACE_EVENTLOG_ACTIVE (g_eventlog_active)

/* ===========================================================================
 *  Ring buffer
 * =========================================================================== */

/**
 * @brief In-memory ring buffer eventlogu.
 *
 * @field events    Heap-alokované pole (@c capacity * sizeof ( st_EVENTLOG_EVENT )).
 * @field capacity  Maximální počet eventů v ringu (clamped v init).
 * @field head      Index dalšího zápisu (0..capacity-1, wrap-around).
 * @field count     Počet validních eventů (capped at capacity).
 * @field overflow  @c true pokud došlo k wrap-around (= ring přepisuje
 *                  nejstarší eventy).
 *
 * @invariant @c head < capacity (kromě stavu @c capacity == 0).
 * @invariant @c count <= capacity.
 * @invariant @c overflow implikuje @c count == capacity.
 */
typedef struct st_EVENTLOG_RING
{
    st_EVENTLOG_EVENT *events;
    size_t             capacity;
    size_t             head;
    size_t             count;
    bool               overflow;
} st_EVENTLOG_RING;

/**
 * @brief Globální instance ringu.
 */
extern st_EVENTLOG_RING g_eventlog;

/* ===========================================================================
 *  Lifecycle API
 * =========================================================================== */

/**
 * @brief Inicializace eventlog subsystému.
 *
 * Registruje cfg sekci @c [EVENT_LOG] (klíče @c mode, @c capacity,
 * @c categories_mask), naplní @ref g_eventlog_config defaulty, alokuje
 * ring podle @c capacity (po cfg propagate). Volat z @c debugger_init()
 * po @c marklog_init().
 *
 * @param capacity  Požadovaná velikost ringu. @c 0 = použít default
 *                  @ref EVENTLOG_DEFAULT_CAPACITY. Hodnoty jsou clamped
 *                  do @c [EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY].
 *
 * @post @ref g_eventlog naalokovaný, ring prázdný, recording NEběží
 *       (= @ref g_eventlog_active == 0 do prvního recompute).
 */
void eventlog_init ( size_t capacity );

/**
 * @brief Cleanup + uvolnění ringu.
 *
 * Volat z @c debugger_exit() před @c marklog_finalize(). Po návratu je
 * @c g_eventlog_active = 0 a @ref g_eventlog.events == NULL.
 */
void eventlog_destroy ( void );

/**
 * @brief Spustit recording (= povolit zápis do ringu).
 *
 * Idempotentní - druhé volání bez @ref eventlog_stop() je no-op. Po
 * úspěchu @ref g_eventlog_active == 1, @ref TEST_TRACE_EVENTLOG_ACTIVE
 * platí.
 *
 * @return @c 0 OK, @c -1 chyba (ring není inicializovaný).
 */
int eventlog_start ( void );

/**
 * @brief Zastavit recording (= zakázat zápis do ringu).
 *
 * Idempotentní. Data v ringu jsou zachována (= UI je může číst dál).
 */
void eventlog_stop ( void );

/**
 * @brief Update active stav podle cfg mode + debugger/window stavu.
 *
 * Spouští/zastavuje recording dle finálního active stavu. Paralelní
 * k @c hwlog_recompute_active(). Logika:
 *
 *  - @c MODE_OFF              -> active = 0
 *  - @c MODE_WHEN_WINDOW_OPEN -> active = (Events okno otevřené?)
 *  - @c MODE_ALWAYS           -> active = 1
 *
 * Stav Events okna je interní static (zatím v Commit 1 vždy 0).
 * UI v Commit 7 přidá setter @c eventlog_set_window_open() (= TODO).
 *
 * Volající: handler @c DBGAPI_CMD_EVENTLOG_SET_MODE (emu vlákno, změna
 * režimu z okna Events) a @ref eventlog_notify_window_open() (UI vlákno,
 * hrana otevření / zavření okna). Funkce mění jen @c int příznak
 * @ref g_eventlog_active (a vypisuje do stderr), nic nealokuje; oba
 * volající spouští UI vlákno a změna režimu je synchronní příkaz, takže
 * se spolu nepřekrývají.
 *
 * @param debugger_active  Reserved pro budoucí use (= sjednocení API
 *                         s tlog @c *_recompute_active(int)). V Commit 1
 *                         se nepoužívá.
 */
void eventlog_recompute_active ( int debugger_active );

/**
 * @brief Vyprázdnit ring (= count = 0, head = 0, overflow = false).
 *
 * Bezpečné volat během recording. Data v @c events[] zůstanou v paměti
 * nedefinovaná, ale @c count = 0 zaručuje, že je nikdo nepřečte.
 */
void eventlog_clear ( void );

/**
 * @brief Změnit capacity ringu za běhu (UI slider).
 *
 * Realokuje @c events[] a ZAHODÍ předchozí data. UI by měla zobrazit
 * potvrzovací dialog.
 *
 * @param new_capacity  Nová velikost (clamped do
 *                      @c [EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY]).
 */
void eventlog_set_capacity ( size_t new_capacity );

/* ===========================================================================
 *  Hot-path emit + Reader API
 * =========================================================================== */

/**
 * @brief Hot-path zápis 24B recordu do ringu.
 *
 * Caller je zodpovědný za gate test:
 *
 * @code
 * if ( TEST_TRACE_EVENTLOG_ACTIVE
 *      && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_XXX ) ) ) {
 *     eventlog_record ( EVENTLOG_CAT_XXX, subtype, pc, payload );
 * }
 * @endcode
 *
 * Funkce sama provádí jen zápis - žádná další gate logika. Pokud je
 * ring NULL (= před initem nebo po destroy), funkce no-op. Timestamp
 * fields (@c pxclk_total / @c screens_total / @c pxclk_in_screen) se
 * dotahují z @c tlog_common_get_*().
 *
 * Side effects:
 *  - Inkrementuje @c head, případně překlopí @c overflow = true.
 *
 * Thread safety: Volat jen z EMU vlákna (hot path). UI vlákno smí jen
 * číst přes @ref eventlog_get_event().
 *
 * @param category  Kategorie eventu (@ref en_EVENTLOG_CATEGORY).
 * @param subtype   Per-kategorie subtype.
 * @param pc        CPU PC v okamžiku eventu.
 * @param payload   Dodatečná data (port/addr/value/decoded info).
 */
void eventlog_record ( uint8_t category, uint8_t subtype,
                       uint16_t pc, uint32_t payload );

/* ===========================================================================
 *  SYS lifecycle emit API (Vlna 5 Commit 31)
 * =========================================================================== */

/**
 * @brief Emit SYS lifecycle event.
 *
 * Volá se z reset path / snapshot driveru / cmt-tape driveru / main loop
 * pro zaznamenání nehot-path lifecycle eventů emulátoru (reset, snapshot
 * save+load, MZF inject, emu start/stop).
 *
 * Funkce sama provádí gate test (= TEST_TRACE_EVENTLOG_ACTIVE + per-kategorie
 * bit v @ref g_eventlog_active_mask), caller nemusí gate testovat. Vzhledem
 * k tomu, že SYS eventy jsou rare (= jednotky per session), je overhead
 * zanedbatelný.
 *
 * Field @c pc v zapsaném eventu = @c 0 (= SYS eventy nejsou bound na
 * konkrétní CPU instrukci).
 *
 * Threading: volat z UI / driver vlákna (= NE z hot-path Z80 loopu).
 * Krátký race vůči eventlog_record() je acceptable identicky s ostatními
 * emit cestami.
 *
 * @param sub      Subtype (@ref en_EVENTLOG_SYS_SUB).
 * @param payload  Event-specific data (filename hash, 0 pro reset/start/stop).
 */
void eventlog_sys_event ( uint8_t sub, uint32_t payload );

/**
 * @brief djb2 string hash z basename filename (= 32-bit ID).
 *
 * Použité pro encoding cesty do 4 B payloadu SYS eventu (SNAPSHOT_SAVE,
 * SNAPSHOT_LOAD, MZF_INJECT). Hash počítá pouze z basename části cesty
 * (= za posledním '/' nebo '\\'), full cesty by daly zbytečně různé hashe
 * pro stejný soubor z jiného adresáře.
 *
 * Algoritmus = klasický djb2 (Daniel J. Bernstein):
 * @code
 * hash = 5381
 * pro každý byte basename:
 *     hash = hash * 33 + byte
 * @endcode
 *
 * Funkce je bezpečná pro NULL / prázdný řetězec (vrací @c 0).
 *
 * @param filename  Vstupní cesta (absolutní nebo relativní). Smí být NULL.
 *
 * @return 32-bit hash basename části. Pro NULL / prázdný vstup @c 0.
 */
uint32_t eventlog_filename_hash ( const char *filename );

/* ===========================================================================
 *  Pause-on-match callback hook (Commit 19)
 * =========================================================================== */

/**
 * @brief Typ callback funkce pro pause-on-match trigger.
 *
 * Volaná z @ref eventlog_record() bezprostředně po zápisu nového eventu
 * do ringu, pokud je @ref g_eventlog_pause_trigger_active != 0 a
 * @ref g_eventlog_pause_callback != NULL. Callback dostane pointer na
 * právě zapsaný event a může (podle vlastní filter logiky) zavolat
 * @c emulator_pause(true) pro async halt.
 *
 * Thread safety: callback je volán z vlákna, které volá
 * @ref eventlog_record() (běžně EMU vlákno, hot path). Implementace
 * v @c eventlog_trigger.c čte jen stav, který mění výhradně emu vlákno
 * (výměna filtru přes @c eventlog_trigger_set() v drainu fronty dbgapi),
 * takže výměna nikdy neproběhne uprostřed vyhodnocení.
 *
 * @param e  Pointer na právě zapsaný event v ringu (read-only, lifetime
 *           do dalšího wrap-around).
 */
typedef void ( *evlog_pause_cb_t ) ( const st_EVENTLOG_EVENT *e );

/**
 * @brief Hot-path gate pro pause callback.
 *
 * @c 0 = callback se nikdy nevolá (= default, žádná režie kromě jedné
 * branchu v @ref eventlog_record()). @c jiné než 0 = callback se volá,
 * pokud @ref g_eventlog_pause_callback != NULL.
 *
 * Zapisuje @c eventlog_trigger_set() (@c 1 = trigger má filtr, @c 0 =
 * vypnutý), @c eventlog_trigger_shutdown() a @ref eventlog_destroy().
 *
 * Thread safety: zapisuje jen emu vlákno (okno Events posílá změny
 * příkazem @c DBGAPI_CMD_EVENTLOG_TRIGGER_SET); UI smí číst jen pro
 * zobrazení (@c eventlog_trigger_get_status()).
 */
extern int g_eventlog_pause_trigger_active;

/**
 * @brief Callback pointer volaný z @ref eventlog_record() při aktivním
 *        gate.
 *
 * Default @c NULL (= callback se nevolá ani při aktivním gate).
 * @c eventlog_trigger_set() ho na emu vlákně nastaví na interní funkci
 * modulu @c eventlog_trigger.c, která vyhodnotí filtr a při shodě volá
 * @c emulator_pause(true). Testy ho smí nastavit přímo.
 *
 * Volání NULL pointer je no-op (= obě podmínky se testují v hot path).
 */
extern evlog_pause_cb_t g_eventlog_pause_callback;

/* ===========================================================================
 *  Auto-mark on match callback hook (Commit 20)
 * =========================================================================== */

/**
 * @brief Typ callback funkce pro auto-mark on match trigger.
 *
 * Volaná z @ref eventlog_record() bezprostředně po zápisu nového eventu
 * do ringu, pokud je @ref g_eventlog_automark_trigger_active != 0 a
 * @ref g_eventlog_automark_callback != NULL. Callback dostane pointer
 * na právě zapsaný event a může (podle vlastní filter logiky) zavolat
 * @c marklog_record() pro synthetic @c USER_MARK event.
 *
 * Pozor na re-entry: @c marklog_record() volá zpět @c eventlog_record()
 * s kategorií @c EVENTLOG_CAT_USER_MARK. Callback proto MUSÍ defenzivně
 * skipnout události této kategorie (jinak nekonečná rekurze - gate se
 * uvnitř callbacku nevypne). Stejný kontrakt platí pro testy.
 *
 * Thread safety: identicky s @ref evlog_pause_cb_t (volání z EMU vlákna).
 *
 * @param e  Pointer na právě zapsaný event v ringu (read-only, lifetime
 *           do dalšího wrap-around).
 */
typedef void ( *evlog_automark_cb_t ) ( const st_EVENTLOG_EVENT *e );

/**
 * @brief Hot-path gate pro auto-mark callback.
 *
 * @c 0 = callback se nikdy nevolá (= default, jeden load + branch v
 * @ref eventlog_record()). @c jiné než 0 = callback se volá, pokud
 * @ref g_eventlog_automark_callback != NULL.
 *
 * Zapisuje @c eventlog_trigger_set() (@c 1 = trigger má filtr a jméno
 * markeru, @c 0 = vypnutý), @c eventlog_trigger_shutdown() a
 * @ref eventlog_destroy().
 *
 * Thread safety: identicky s @ref g_eventlog_pause_trigger_active.
 */
extern int g_eventlog_automark_trigger_active;

/**
 * @brief Callback pointer volaný z @ref eventlog_record() při aktivním
 *        @ref g_eventlog_automark_trigger_active.
 *
 * Default @c NULL. @c eventlog_trigger_set() ho na emu vlákně nastaví na
 * interní funkci modulu @c eventlog_trigger.c, která vyhodnotí filtr
 * a při shodě volá @c marklog_register() (první shoda pod daným jménem)
 * + @c marklog_record(). Testy ho smí nastavit přímo.
 *
 * Volání NULL pointer je no-op (= obě podmínky se testují v hot path).
 *
 * Pause + automark callbacky jsou nezávislé - oba se volají, pokud jsou
 * oba gate aktivní. Pořadí: pause callback první, automark druhý
 * (= definováno @ref eventlog_record()).
 */
extern evlog_automark_cb_t g_eventlog_automark_callback;

/**
 * @brief UI -> emu notifikace: Events okno bylo otevřeno / zavřeno.
 *
 * Přepíná interní flag @c s_event_viewer_window_open uvnitř
 * @c eventlog.c a okamžitě volá @ref eventlog_recompute_active(), takže
 * v módu @c MODE_WHEN_WINDOW_OPEN se zápis do ringu spustí / zastaví
 * synchronně s prvním renderem / zavřením UI okna.
 *
 * Volání mimo @c MODE_WHEN_WINDOW_OPEN je no-op pro recording stav, ale
 * vždy nastaví interní flag (= robustnost při pozdějším přepnutí módu
 * uživatelem).
 *
 * Thread safety: volat výhradně z UI vlákna při render hraně okna
 * (open / close transition). @c int store je atomický na x86, krátký
 * race vůči hot-path emit gate je acceptable.
 *
 * @param is_open  @c 0 = okno zavřeno, @c jiné než 0 = okno otevřeno.
 */
void eventlog_notify_window_open ( int is_open );

/**
 * @brief Reader API: počet validních eventů v ringu.
 *
 * @return @c count v rozsahu @c [0..capacity].
 */
size_t eventlog_get_count ( void );

/**
 * @brief Reader API: vrať event na logickém indexu (0 = nejstarší).
 *
 * Při @c overflow = false: idx == fyzický index. Při @c overflow = true:
 * idx 0 odpovídá fyzickému @c head (= nejstarší přeživší event).
 *
 * @param idx  Logical index, @c 0 = oldest.
 * @return Pointer na event (vlastněný ringem), nebo @c NULL pokud
 *         @c idx >= count.
 */
const st_EVENTLOG_EVENT *eventlog_get_event ( size_t idx );

/* ===========================================================================
 *  Export / Import (Vlna 4 Commit 29)
 * =========================================================================== */

/**
 * @brief Magic identifikátor binárního export formátu.
 *
 * 8 B literál v hlavičce souboru (= prvních 8 B před version + record_size).
 * Hodnota není NUL-terminated string, jen sekvence znaků.
 */
#define EVENTLOG_EXPORT_MAGIC      "MZEVTLOG"

/**
 * @brief Délka magic identifikátoru v bytech.
 */
#define EVENTLOG_EXPORT_MAGIC_LEN  8u

/**
 * @brief Aktuální verze formátu (= 1).
 *
 * Validátor odmítne soubor s jinou verzí. Budoucí rozšíření layoutu může
 * zvednout verzi na 2+ a podporu V1 si import zachová přes switch.
 */
#define EVENTLOG_EXPORT_VERSION    1u

/**
 * @brief Hlavička exportovaného binárního souboru.
 *
 * Layout (= little-endian na x86 / ARM Win/Linux, viz Endianness poznámka
 * níže):
 *
 * | Offset | Velikost | Pole         | Hodnota                          |
 * |--------|----------|--------------|----------------------------------|
 * | 0      | 8 B      | magic        | "MZEVTLOG" (literal, ne string)  |
 * | 8      | 4 B      | version      | @ref EVENTLOG_EXPORT_VERSION     |
 * | 12     | 4 B      | record_size  | @c sizeof(st_EVENTLOG_EVENT) = 32 |
 * | 16     | 8 B      | record_count | Počet následujících records      |
 * | 24     | 8 B      | timestamp    | Unix epoch sekundy při exportu   |
 *
 * Celkem 32 B. Za hlavičkou následuje @c record_count * 32 B records
 * v chronologickém pořadí (= oldest first, viz @ref eventlog_get_event).
 *
 * @note Endianness: native zápis (= little-endian na podporovaných
 *       platformách Win MSYS2 / Linux x86_64 / ARM64). Soubory NEJSOU
 *       přenositelné mezi big-endian a little-endian systémy. Pro budoucí
 *       cross-platform potřebu by se musela přidat explicit byte order
 *       konverze (= V2 formát).
 */
typedef struct st_EVENTLOG_EXPORT_HEADER
{
    char     magic[ EVENTLOG_EXPORT_MAGIC_LEN ]; /**< Literal "MZEVTLOG" bez NUL. */
    uint32_t version;                            /**< Verze formátu, V1 = 1. */
    uint32_t record_size;                        /**< @c sizeof(st_EVENTLOG_EVENT). */
    uint64_t record_count;                       /**< Počet records v souboru. */
    uint64_t timestamp;                          /**< Unix epoch sekundy. */
} st_EVENTLOG_EXPORT_HEADER;

/**
 * @brief Compile-time pojistka: hlavička MUSÍ být přesně 32 B.
 *
 * Změna velikosti je breaking - musí jít ruku v ruce se zvednutím
 * @ref EVENTLOG_EXPORT_VERSION a switch v importu.
 */
#ifdef __cplusplus
static_assert ( sizeof ( st_EVENTLOG_EXPORT_HEADER ) == 32,
                "st_EVENTLOG_EXPORT_HEADER MUSI mit 32 B (Vlna 4 Commit 29)" );
#elif defined(__GNUC__) || defined(__clang__)
_Static_assert ( sizeof ( st_EVENTLOG_EXPORT_HEADER ) == 32,
                 "st_EVENTLOG_EXPORT_HEADER MUSI mit 32 B (Vlna 4 Commit 29)" );
#endif

/**
 * @brief Export aktuálního ringu do binárního souboru.
 *
 * Iteruje events v chronologickém pořadí (= oldest first přes
 * @ref eventlog_get_event). Zapíše @ref st_EVENTLOG_EXPORT_HEADER s
 * platnou @c record_count a aktuálním unix timestamp, pak sekvenci
 * @c record_count * 32 B records.
 *
 * Pokud je ring prázdný (@c eventlog_get_count == 0), výstupní soubor
 * obsahuje jen 32 B hlavičky s @c record_count = 0.
 *
 * Side effects:
 *  - Otevírá soubor v binárním write módu (= @c "wb"), případně přepíše
 *    existující obsah.
 *  - Zapisuje sekvenčně přes @c fwrite, na chybě uzavírá soubor a vrací -1.
 *
 * Threading: volat z UI vlákna (= ne hot path). Při běžícím recording
 * (@c g_eventlog_active != 0) může dojít ke krátkému race vůči
 * @ref eventlog_record, který znamená nejvýše ztracený / extra poslední
 * event v exportu. Pro deterministický export se doporučuje pauznout emu
 * nebo vypnout recording před voláním.
 *
 * @param path  Cesta k cílovému souboru. Bude přepsán, pokud existuje.
 *              MUSÍ být non-NULL.
 *
 * @return @c 0 OK, @c -1 chyba (= @c fopen / @c fwrite selhání, @c path
 *         NULL nebo ring není inicializován).
 */
int eventlog_export_to_file ( const char *path );

/**
 * @brief Import souboru zpět do ringu (= replay).
 *
 * Validuje hlavičku (magic, version, record_size). Volá
 * @ref eventlog_clear, případně @ref eventlog_set_capacity pokud
 * @c record_count > aktuální capacity (= ring se zvětší až do limitu
 * @ref EVENTLOG_MAX_CAPACITY; přebývající records jsou ignorovány).
 *
 * Po úspěchu obsahuje ring přesně @c record_count events (clamped do
 * capacity). @c overflow je @c false, @c head ukazuje za poslední record.
 *
 * Side effects:
 *  - Otevírá soubor v binárním read módu (= @c "rb").
 *  - Volá @ref eventlog_clear před zápisem (= stávající data se ztratí).
 *  - Může volat @ref eventlog_set_capacity pokud @c record_count > capacity.
 *
 * Threading: volat jen z emu vlákna (UI přes
 * @c DBGAPI_CMD_EVENTLOG_IMPORT_FILE, helper @c dbg_ui_eventlog_import_file)
 * nebo při initu, kdy emu smyčka ještě neběží (replay v @c debugger.c).
 * Funkce může realokovat ring (@ref eventlog_set_capacity = free + calloc)
 * a čte záznamy přímo do @c events[]; souběh s @ref eventlog_record() by
 * zapisoval do uvolněné paměti. Čtenáři ringu z UI vlákna (Log, Strip)
 * musí po návratu příkazu počítat se změnou @c count / @c capacity.
 *
 * @param path  Cesta k input souboru. MUSÍ být non-NULL.
 *
 * @return @c 0 OK, @c -1 chyba (= @c fopen selhání, invalid magic /
 *         version / record_size, @c fread selhání nebo @c path NULL).
 */
int eventlog_import_from_file ( const char *path );

#ifdef __cplusplus
}
#endif

#endif /* EVENTLOG_H */
