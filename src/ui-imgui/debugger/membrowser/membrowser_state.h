/**
 * @file membrowser_state.h
 * @brief Per-window state Memory Browser instance (V0 + V3 multi-view).
 *
 * State struct sdílená napříč membrowser_window.cpp / _hexview.cpp /
 * _navigate.cpp / _search.cpp.
 *
 * V0: 1 instance "main", persist v cfgmain sekci [MEMBROWSER_WINDOW].
 * V3 multi-view: 5 instancí (main + #2..#5), persist v sekcích
 * [MEMBROWSER_WINDOW_MAIN] / [MEMBROWSER_WINDOW_2] / .._3 / .._4 / .._5.
 * Legacy sekce [MEMBROWSER_WINDOW] (V0/V1/V2 INI) je akceptována jako
 * fallback - viz @ref membrowser_window_register_persistence.
 *
 * Per-instance state je oddělen pomocí @c instance_idx v @ref
 * st_MEMBROWSER_STATE; @ref membrowser_state_save_to_persisted z něj
 * určí cíl pole @c s_persist_*[idx]. Žádný cross-talk mezi instancemi
 * kromě globálně sdíleného freeze subsystému (viz V1 dokumentace).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_STATE_H
#define MEMBROWSER_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Počet Memory Browser instancí (main + 4 sekundární).
 *
 * Index 0 = "main" (= legacy okno), index 1..4 = "#2".."#5".
 * Po per-instance refaktoru persist (V3) drží každá instance vlastní
 * pole @c s_persist_*[MB_INSTANCE_COUNT].
 */
#define MB_INSTANCE_COUNT 5

/**
 * @brief Index hlavní (legacy) Memory Browser instance.
 */
#define MB_INSTANCE_MAIN  0

/**
 * @brief Encoding identifikátor pro ASCII column rendering.
 *
 * V0-polish-3: schéma per mzdisk referenční implementace
 * (panel_hexdump_imgui.cpp). Devět "primárních" charsetů + KOI8-CS
 * jako extra na konci.
 *
 * Stabilní číselné hodnoty - persistovány v cfg. Při budoucí změně
 * NIKDY nepřečíslovat existující, jen přidávat na konec před _COUNT.
 *
 * Pozn.: Schéma se NESHODUJE s předchozím (do polish-2). Při načtení
 * starého INI proběhne range check + fallback na RAW. Sémantika
 * jednotlivých hodnot se mezi starým a novým schématem liší (např.
 * staré DISP_EU=3 = nové SHARPMZ_EU_UTF8 vizuálně rozdílné), proto
 * uživatel může vidět "jiný" výchozí encoding po upgrade - to je
 * akceptovatelná cena za přechod na konzistentní schéma s mzdisk.
 */
typedef enum en_MEMBROWSER_ENCODING
{
    MB_CHARSET_RAW = 0,                  /**< Printable ASCII 0x20-0x7E, jinak '.'. */
    MB_CHARSET_SHARPMZ_EU_ASCII = 1,     /**< Sharp MZ ASCII (EU) -> ASCII (jednobajtové). */
    MB_CHARSET_SHARPMZ_JP_ASCII = 2,     /**< Sharp MZ ASCII (JP) -> ASCII. */
    MB_CHARSET_SHARPMZ_EU_UTF8 = 3,      /**< Sharp MZ ASCII (EU) -> UTF-8 (vícebajtové). */
    MB_CHARSET_SHARPMZ_JP_UTF8 = 4,      /**< Sharp MZ ASCII (JP) -> UTF-8. */
    MB_CHARSET_SHARPMZ_EU_CG1 = 5,       /**< MZ ASCII -> video kód -> mzglyphs EU1 (= live emu display). */
    MB_CHARSET_SHARPMZ_EU_CG2 = 6,       /**< MZ ASCII -> video kód -> mzglyphs EU2. */
    MB_CHARSET_SHARPMZ_JP_CG1 = 7,       /**< MZ ASCII -> video kód -> mzglyphs JP1. */
    MB_CHARSET_SHARPMZ_JP_CG2 = 8,       /**< MZ ASCII -> video kód -> mzglyphs JP2. */
    MB_CHARSET_KOI8CS = 9,               /**< KOI8-CS (Czech/Slovak diakritika v CP/M ekosystému). */
    MEMBROWSER_ENC__COUNT                /**< Sentinel - počet encodings (10). */
} en_MEMBROWSER_ENCODING;

/**
 * @brief Search type identifikátor (V4 search engine).
 *
 * V0: jen BYTE_SEQ + ASCII. V4: rozšířeno o word LE/BE, masked, regex,
 * expression. Pořadí stabilní - persistuje se v cfgmain (search_type
 * unsigned 0..MEMBROWSER_SEARCH__COUNT-1), nepřečíslovávat.
 */
typedef enum en_MEMBROWSER_SEARCH_TYPE
{
    MEMBROWSER_SEARCH_BYTE_SEQ = 0,    /**< [LEGACY V0/V4] Byte sequence "AA BB CC" hex, single-field. Pro nový UX použij @ref MEMBROWSER_SEARCH_BYTES. */
    MEMBROWSER_SEARCH_ASCII = 1,       /**< [LEGACY V0/V4] ASCII string s encoding-aware compile (current_encoding lookup). Měl bugy s CG variants (return false) a Raw silent-skip non-ASCII. Pro nový UX použij @ref MEMBROWSER_SEARCH_BYTES. */
    MEMBROWSER_SEARCH_WORD_LE = 2,     /**< 16-bit word little-endian (decimal/hex literal). */
    MEMBROWSER_SEARCH_WORD_BE = 3,     /**< 16-bit word big-endian. */
    MEMBROWSER_SEARCH_MASKED = 4,      /**< Masked hex sekvence "AA ?? CC" (?? = wildcard byte). */
    MEMBROWSER_SEARCH_REGEX = 5,       /**< POSIX ERE regex přes std::regex::extended (proti raw bytes). */
    MEMBROWSER_SEARCH_EXPRESSION = 6,  /**< bp_expr eval per byte (Value = byte, Address = offset). */
    MEMBROWSER_SEARCH_BYTES = 7,       /**< V4.1+ Pattern Builder - dual HEX+ASCII editable fields se sync logikou + Char Inserter integrace. Sjednocuje funkčnost BYTE_SEQ + ASCII; canonical storage v search_pattern[] jako hex string "AA BB CC", ASCII field se rendruje per-frame z bytes přes current_encoding. */
    MEMBROWSER_SEARCH__COUNT           /**< Sentinel. */
} en_MEMBROWSER_SEARCH_TYPE;

/**
 * @brief Scope hledání - kde se má prohledávat.
 */
typedef enum en_MEMBROWSER_SEARCH_SCOPE
{
    MEMBROWSER_SCOPE_CURRENT = 0,      /**< Jen aktuálně vybraný region (default). */
    MEMBROWSER_SCOPE_ALL_REGIONS = 1,  /**< Všechny regiony aktuální architektury (sekvenčně). */
    MEMBROWSER_SCOPE__COUNT
} en_MEMBROWSER_SEARCH_SCOPE;

/**
 * @brief Stav async / chunked search operace.
 *
 * Search běží UI-side v chunks (typicky 256 KB per frame), aby velký
 * region (16 MB Ramdisk) nezablokoval UI. Engine drží scan_pos a každý
 * frame posune o budget; Cancel button nastaví active=false.
 */
typedef enum en_MEMBROWSER_SEARCH_STATE
{
    MEMBROWSER_SEARCH_STATE_IDLE = 0,      /**< Žádné hledání neběží. */
    MEMBROWSER_SEARCH_STATE_RUNNING = 1,   /**< Hledání probíhá (chunked per frame). */
    MEMBROWSER_SEARCH_STATE_DONE = 2,      /**< Dokončeno (nálezy jsou v result listu). */
    MEMBROWSER_SEARCH_STATE_CANCELED = 3,  /**< Uživatel zrušil. */
    MEMBROWSER_SEARCH_STATE_ERROR = 4      /**< Parser/compile chyba (zpráva v st->search_error). */
} en_MEMBROWSER_SEARCH_STATE;

/**
 * @brief Mód jednoho hledání - jak se mají výsledky prezentovat.
 */
typedef enum en_MEMBROWSER_SEARCH_MODE
{
    MEMBROWSER_SEARCH_MODE_NEXT = 0,   /**< Najdi další (od cursor+1), zastav na prvním. */
    MEMBROWSER_SEARCH_MODE_PREV = 1,   /**< Najdi předchozí (od cursor-1 zpět). */
    MEMBROWSER_SEARCH_MODE_ALL = 2     /**< Najdi všechny (do MB_SEARCH_MAX_RESULTS). */
} en_MEMBROWSER_SEARCH_MODE;

/**
 * @brief Maximální počet zobrazených výsledků pro mode ALL.
 *
 * Po dosažení limitu se hledání ukončí (status "max results reached").
 * 256 je rozumný kompromis mezi paměťovou stopou a využitelností.
 */
#define MB_SEARCH_MAX_RESULTS 256

/**
 * @brief Počet posledních patterns držených v history (per instance).
 *
 * Dropdown nad search input umožňuje recall posledních N patternů.
 * Persist v cfgmain klíčích search_history_NN (NN = 00..09).
 */
#define MB_SEARCH_HISTORY_SIZE 10

/**
 * @brief Délka jednoho history pattern bufferu (musí být = délka search_pattern).
 */
#define MB_SEARCH_PATTERN_SIZE 256

/**
 * @brief Jeden záznam ve výsledku hledání.
 *
 * Drží absolutní adresu v rámci regionu + region_id + krátký preview
 * (16 bajtů, hex display). Region label se získává on-render
 * z aktuálního snapshotu (nepersistuje se v hit).
 */
typedef struct st_MB_SEARCH_HIT
{
    int region_id;          /**< Region kde k matchi došlo (resolved v době hledání). */
    int region_kind;        /**< en_REGION_KIND - pro re-resolve po banking změně. */
    int region_sub_id;      /**< Disambiguator pro re-resolve. */
    uint64_t addr;          /**< Offset v rámci regionu. */
    uint8_t preview[16];    /**< Náhled 16 bajtů od addr (clamped k size). */
    int preview_len;        /**< Skutečná délka preview (1..16). */
} st_MB_SEARCH_HIT;

/**
 * @brief Edit mode (HEX nibble vs ASCII char).
 */
typedef enum en_MEMBROWSER_EDIT_MODE
{
    MEMBROWSER_EDIT_HEX = 0,        /**< Edit v HEX sloupci (po nibble). */
    MEMBROWSER_EDIT_ASCII = 1       /**< Edit v ASCII sloupci (UTF-8 vstup -> per-encoding kódování). */
} en_MEMBROWSER_EDIT_MODE;

/**
 * @brief Stabilní (kind, sub_id) identifikátor regionu napříč session.
 *
 * Raw region_id z dbgapi_regions_enumerate() je session-stable jen v rámci
 * jednoho enumerate volání - po HW reconfigure (Memext connect/disconnect)
 * se IDs mohou změnit. Pro persist a UI state držíme (kind, sub_id)
 * a každý frame resolvujeme na aktuální region_id přes lookup.
 */
typedef struct st_MEMBROWSER_REGION_KEY
{
    int kind;     /**< en_REGION_KIND. */
    int sub_id;   /**< Disambiguator (bank/plane index). */
} st_MEMBROWSER_REGION_KEY;

/**
 * @brief Logické grupy regionů pro toolbar Region dropdown.
 *
 * Toolbar dropdown listuje grupy (kategorie), ne per-bank items. To
 * dramaticky zkracuje seznam (typicky 14 položek místo 50-200+ při
 * připojeném Luftner Memextu + 16 MB Ramdisku). Per-bank výběr se pro
 * Memext/Ramdisk grupy řeší dynamicky zobrazenými sub-controls (Type
 * combo + Bank combo) vedle hlavního dropdown.
 *
 * Pevné pořadí - nesmí se měnit (i když enum hodnoty samotné nejsou
 * persistovány, slouží jako klíč pro iteraci v dropdown). Persist se
 * dělá přes (kind, sub_id) páry v @ref st_MEMBROWSER_REGION_KEY, grupa
 * se dopočítá při renderu.
 *
 * Sub-controls semantika:
 *   - MB_RG_MEMEXT - Type RAM/FLASH + bank index (FLASH jen u Luftner).
 *   - MB_RG_RAMDISK_STD - bank index (0..N podle velikosti, max 0xFF).
 *   - MB_RG_RAMDISK_PEZIK_68 / _E8 - bank index 0..7 per instance.
 *   - Ostatní grupy = 1 region, žádný sub-control.
 *
 * Sidebar Regions tree (membrowser_regions.cpp) tuto grupu NEPOUŽÍVÁ -
 * tam zůstává per-bank zobrazení beze změny.
 */
typedef enum en_MEMBROWSER_REGION_GROUP
{
    MB_RG_LOGICAL = 0,             /**< REGION_KIND_LOGICAL. */
    MB_RG_RAM,                     /**< REGION_KIND_RAM. */
    MB_RG_ROM_LOWER,               /**< REGION_KIND_ROM_LOWER. */
    MB_RG_ROM_UPPER,               /**< REGION_KIND_ROM_UPPER. */
    MB_RG_CGROM,                   /**< REGION_KIND_CGROM. */
    MB_RG_CGRAM_700,               /**< REGION_KIND_CGRAM_700. */
    MB_RG_VRAM_PLANE_I,            /**< REGION_KIND_VRAM_PHYS_PLANE sub_id=0. */
    MB_RG_VRAM_PLANE_II,           /**< sub_id=1. */
    MB_RG_VRAM_PLANE_III,          /**< sub_id=2. */
    MB_RG_VRAM_PLANE_IV,           /**< sub_id=3. */
    MB_RG_VRAM_700_CHAR,           /**< REGION_KIND_VRAM_700_CHAR. */
    MB_RG_VRAM_700_ATTR,           /**< REGION_KIND_VRAM_700_ATTR. */
    MB_RG_PCG_1,                   /**< REGION_KIND_PCG_1500 sub_id=0. */
    MB_RG_PCG_2,                   /**< sub_id=1. */
    MB_RG_PCG_3,                   /**< sub_id=2. */
    MB_RG_MEMEXT,                  /**< REGION_KIND_MEMEXT_RAM/FLASH (sub-controls). */
    MB_RG_RAMDISK_STD,             /**< REGION_KIND_RAMDISK_STD (sub-control). */
    MB_RG_RAMDISK_PEZIK_68,        /**< REGION_KIND_RAMDISK_PEZIK sub_id 0..7. */
    MB_RG_RAMDISK_PEZIK_E8,        /**< REGION_KIND_RAMDISK_PEZIK sub_id 8..15. */
    MB_RG_PROHIBITED_SHADOW,       /**< REGION_KIND_PROHIBITED_SHADOW. */
    MB_RG_DEVICE,                  /**< REGION_KIND_DEVICE (paměť zařízení devbus, sub-control výběr regionu). */
    MB_RG__COUNT                   /**< Sentinel. */
} en_MEMBROWSER_REGION_GROUP;

/**
 * @brief Mapuje (region_kind, sub_id) na logickou grupu pro dropdown.
 *
 * Inverzní operace k @ref membrowser_group_to_region. Pro neznámý kind
 * vrací MB_RG_LOGICAL (bezpečný fallback - Logical Z80 je vždy dostupný).
 *
 * @param region_kind  en_REGION_KIND z dbgapi_regions.
 * @param sub_id       Disambiguator (plane/bank index, PEZIK instance*8+bank).
 * @return             Grupa pro dropdown.
 */
en_MEMBROWSER_REGION_GROUP membrowser_region_kind_to_group ( int region_kind,
                                                              int sub_id );

/**
 * @brief Mapuje (grupa, type_idx, bank_idx) zpět na (region_kind, sub_id).
 *
 * Pro grupy bez sub-controls (Logical, ROM, VRAM, PCG bank N, ...) jsou
 * type_idx i bank_idx ignorovány a vrací se kanonický (kind, sub_id) páry
 * dané grupy.
 *
 * Pro MB_RG_MEMEXT:
 *   - type_idx 0 = MEMEXT_RAM, bank_idx = sub_id (0..0x7F)
 *   - type_idx 1 = MEMEXT_FLASH, bank_idx = sub_id (0x80..0xFF)
 *
 * Pro MB_RG_RAMDISK_STD: type_idx ignorován, bank_idx = sub_id (0..0xFF).
 * Pro MB_RG_RAMDISK_PEZIK_68: bank_idx = sub_id (0..7).
 * Pro MB_RG_RAMDISK_PEZIK_E8: bank_idx = sub_id - 8 (0..7).
 *
 * @param[in]  group     Grupa.
 * @param[in]  type_idx  0/1 pro Memext, jinak ignorováno.
 * @param[in]  bank_idx  Bank index (smysl viz výše per grupa).
 * @param[out] out_kind  Výsledný en_REGION_KIND.
 * @param[out] out_sub   Výsledný sub_id.
 */
void membrowser_group_to_region ( en_MEMBROWSER_REGION_GROUP group,
                                   int type_idx, int bank_idx,
                                   int *out_kind, int *out_sub );

/**
 * @brief Lokalizovaný label grupy pro dropdown (klíč pro _L()).
 *
 * Vrací anglický zdroj (ne přeloženou hodnotu) - caller obalí _L()
 * pro stabilní ImGui ID. Pro grupy s ###StableID součástí ale použít
 * raw stringy bez ###suffix; ID je definováno samotným ImGui::Selectable
 * v render loop.
 *
 * @param group  Grupa.
 * @return       NUL-terminovaný anglický string ("Logical Z80", "Memext", ...).
 */
const char *membrowser_group_label_en ( en_MEMBROWSER_REGION_GROUP group );

/**
 * @brief True pokud grupa má dynamické sub-controls (Memext/Ramdisk).
 *
 * @param group  Grupa.
 * @return       true = render Type/Bank combo, false = single region.
 */
bool membrowser_group_has_subcontrols ( en_MEMBROWSER_REGION_GROUP group );

/**
 * @brief Kompletní stav jedné Memory Browser instance.
 *
 * Veřejně viditelný headers pro snapshot/serialize - členy jsou interní.
 * Pro V0 single instance držena jako file-static singleton v
 * membrowser_window.cpp.
 *
 * Invarianty:
 *   - current_encoding < MEMBROWSER_ENC__COUNT
 *   - bytes_per_row in {8, 16, 32}
 *   - edit_nibble in {0, 1}
 *   - cursor_addr < current backend total_size (po každém region switch)
 */
typedef struct st_MEMBROWSER_STATE
{
    /* V3 multi-view: index instance (0=main, 1..4 = #2..#5). Slouží jako
     * klíč pro per-instance persist pole @c s_persist_*[idx] v
     * membrowser_state.cpp. Nastavuje se v @ref membrowser_state_default
     * a NESMÍ být měněn po vytvoření instance. */
    int instance_idx;                     /**< 0..MB_INSTANCE_COUNT-1. */

    /* Region selection. */
    st_MEMBROWSER_REGION_KEY current_key; /**< Persisted (kind, sub_id). */
    int current_region_id;                /**< Resolved per-frame z REGION_LIST. */
    int current_encoding;                 /**< en_MEMBROWSER_ENCODING. */

    /* Cursor / view. */
    uint32_t cursor_addr;                 /**< Aktuální pozice v rámci regionu. */
    uint32_t scroll_top_addr;             /**< Top of visible window. */
    int bytes_per_row;                    /**< 8 / 16 / 32 (default 32 - per stará GTK reference). */
    bool ascii_column_visible;            /**< Zobrazit ASCII sloupec. */
    bool show_pc_marker;                  /**< Zvýraznit řádek s PC (jen Logical). */
    bool show_sp_marker;                  /**< Zvýraznit řádek s SP (jen Logical). */

    /* Edit state.
     *
     * V1-polish-1: zjednodušený single-gate model. Dříve byly dva flagy
     * (edit_enabled jako master safety + edit_active "edit zóna aktivní"
     * togglovaný F2). Druhý gate způsoboval, že po Edit: ON editace
     * nereagovala dokud uživatel nestiskl F2 (a F2 navíc nefungovalo,
     * protože dispatch byl gatovaný edit_enabled). Sloučeno do jednoho
     * flagu - edit_enabled samotný stačí jako safety i jako aktivační
     * stav. F2 zachováno jako klávesová zkratka pro toggle Edit (= klik
     * na tlačítko Edit: OFF/ON ekvivalent). */
    bool edit_enabled;                    /**< Editace povolena (default OFF - safety). F2 toggluje stejně jako tlačítko Edit. */
    bool size_applied;                    /**< Runtime flag (not persisted): true po prvním aplikování default size 600x480 v session. Brání opakovanému force po user resize. */
    int edit_mode;                        /**< en_MEMBROWSER_EDIT_MODE. */
    int edit_nibble;                      /**< 0 = high, 1 = low nibble (HEX mode). */
    char edit_input[8];                   /**< Buffer pro InputText (HEX 2-3 znaky, ASCII UTF-8 ~4). */

    /* Search panel (V0 basic + V4 engine).
     *
     * V0 ponechané fieldy (search_panel_open, search_type, search_pattern,
     * last_match_addr, last_match_valid) zůstávají kompatibilní s persist
     * formátem. V4 přidává scope/case/state engine + result list (transient,
     * NEpersistuje) + history (persistuje).
     *
     * Threading: vše UI-thread only. Engine běží v chunked režimu - každý
     * frame posune scan_pos o search_chunk_budget bajtů. */
    bool search_panel_open;
    int search_type;                      /**< en_MEMBROWSER_SEARCH_TYPE. */
    char search_pattern[MB_SEARCH_PATTERN_SIZE];
    uint32_t last_match_addr;             /**< Poslední nalezená pozice (-1 = nic). */
    bool last_match_valid;

    /* V4: scope + case sensitive (ASCII only). */
    int search_scope;                     /**< en_MEMBROWSER_SEARCH_SCOPE (default CURRENT). */
    bool search_case_sensitive;           /**< ASCII case sensitivity (default true). */

    /* V4: engine state machine pro chunked / multi-frame scan. */
    int search_state;                     /**< en_MEMBROWSER_SEARCH_STATE. */
    int search_mode;                      /**< en_MEMBROWSER_SEARCH_MODE - co se právě dělá. */
    int search_engine_region_id;          /**< Region kde engine aktuálně skenuje (přepíná se v SCOPE_ALL_REGIONS). */
    int search_engine_region_kind;        /**< Kind aktuálního skenovaného regionu. */
    int search_engine_region_sub_id;      /**< Sub_id aktuálního skenovaného regionu. */
    int search_engine_scope_idx;          /**< Index v regions snapshotu (SCOPE_ALL_REGIONS iteration). */
    uint64_t search_scan_pos;             /**< Aktuální pozice scanneru (od kterého offsetu pokračovat). */
    uint64_t search_scan_end;             /**< Konec aktuálního regionu (= region size). */
    uint64_t search_total_scanned;        /**< Celkový počet skenovaných bajtů (pro progress bar). */
    uint64_t search_total_bytes;          /**< Plánovaný celkový rozsah (pro progress bar). */
    char search_error[128];               /**< Chybová hláška kompilace patternu (regex/expr parser). */

    /* V4: výsledky hledání (transient, nepersistuje). */
    int search_results_count;             /**< Počet validních hitů v search_results[]. */
    int search_results_visible;           /**< Result panel viditelný (zobrazí se po DONE / ALL mode). */
    st_MB_SEARCH_HIT search_results[MB_SEARCH_MAX_RESULTS];

    /* V4: history posledních N patternů (most-recent first). */
    int search_history_count;             /**< Validní položky v search_history[0..count-1]. */
    char search_history[MB_SEARCH_HISTORY_SIZE][MB_SEARCH_PATTERN_SIZE];
    int search_history_types[MB_SEARCH_HISTORY_SIZE]; /**< Typ patternu pro každý history slot. */

    /* Goto input buffer. */
    char goto_input[32];

    /* V1 Layers panel - color overlay nad hex view.
     *
     * Default OFF pro všechny - "čistý" view, opt-in per layer. Persist
     * v cfgmain klíčích layer_* (sekce MEMBROWSER_WINDOW).
     *
     * Per-byte rendering pravidlo: pokud více layers indikuje shoda, pořadí
     * je Frozen > Snapshot Δ > CDL X > CDL R > CDL W > Heatmap (= Frozen
     * "vyhraje" pokud byte je zafrozený). */
    bool layers_panel_open;        /**< Pravý kolaps panel viditelný. */
    bool layer_cdl_x;              /**< CDL Code (X) - světle modré bg. */
    bool layer_cdl_r;              /**< CDL Data Read - světle zelené bg. */
    bool layer_cdl_w;              /**< CDL Data Write - světle červené bg. */
    bool layer_heatmap;            /**< Heatmap access count - gradient. */
    bool layer_snapshot_delta;     /**< Δ since snapshot - magenta bg. */
    bool layer_frozen;             /**< Frozen bytes - tmavě fialové bg. */
    bool layer_symbols;            /**< Symbol overlay v ASCII column. */

    /* V1-polish-1: šířka Layers panelu (px).
     *
     * 0 = "auto-compute" = při prvním renderu se spočítá optimal width
     * z labelů (delší texty kvůli lokalizaci se vejdou). User může pak
     * resize přes splitter handle mezi hex view a layers panelem. Hodnota
     * se persistuje v cfgmain jako layers_panel_width. Clamp 200..600 px. */
    int layers_panel_width;        /**< Px (0 = auto-compute při příštím renderu). */

    /* V2: per-row physical origin label v Logical Z80 view.
     *
     * Když ON A current region = REGION_KIND_LOGICAL, na konci každého
     * hex řádku se vykreslí tlumený textový label ukazující, odkud fyzicky
     * pochází 4 KB stránka kde řádek leží ("ROM lower", "Plane I", "RAM",
     * "Memext", atd.). Per-row label používá existující memmap_query() z
     * emulátoru - žádný emu touch v hot path, jen UI thread read.
     *
     * Default OFF (= klasický view bez extra textu). Persist v cfgmain
     * pod show_origin_labels. Pro non-Logical regiony se label nezobrazí
     * (= toggle bez efektu, ale ponechán na true aby se po návratu do
     * Logical regionu znovu objevil). */
    bool show_origin_labels;       /**< Per-row origin label v Logical view (default OFF). */

    /* V2: levý Regions sidebar - hierarchický strom regionů per arch.
     *
     * Default ON (= primary navigation; uživatel chce vidět dostupné regiony
     * hned po startu). Šířka panelu se chová stejně jako layers_panel_width
     * (0 = auto-compute z labelů, jinak px clamp 180..420). Persist v cfgmain
     * pod klíči regions_panel_open + regions_panel_width. */
    bool regions_panel_open;       /**< Levý kolaps panel viditelný (default ON). */
    int regions_panel_width;       /**< Px (0 = auto-compute při příštím renderu). */

    /* Edit semantics: detekce region switche pro recently-edited clear.
     *
     * Drží (kind, sub_id) z předchozího framu - pokud se v aktuálním framu
     * liší od @c current_key, instance volá @ref membrowser_edited_clear_for_instance.
     * Sentinel hodnoty @c last_region_key_kind = -1 = "ještě jsme nic
     * neviděli" (= první render po create), v takovém případě se neclearuje.
     * Nepersistuje se v INI - je to per-session UI stav.
     *
     * Důvod: per Michalova schválená semantika "recently-edited list clear
     * automaticky na region/bank switch", aby uživatel po přepnutí regionu
     * neviděl staré badge z předchozího (možná nesouvisejícího) regionu. */
    int last_region_kind;          /**< Předchozí kind (-1 = uninited). */
    int last_region_sub_id;        /**< Předchozí sub_id. */
} st_MEMBROWSER_STATE;

/**
 * @brief Vrátí default-initialized state pro danou instanci.
 *
 * Volá se v @ref membrowser_view_create. @p instance_idx zaznamenává
 * která instance vlastní state (0=main, 1..4=#2..#5); pole je pak
 * čteno persist funkcemi pro výběr cíle v @c s_persist_*[idx].
 *
 * @param instance_idx 0..MB_INSTANCE_COUNT-1. Mimo rozsah = clamp na 0.
 * @return             Vyplněná struktura s rozumnými výchozími hodnotami.
 */
st_MEMBROWSER_STATE membrowser_state_default ( int instance_idx );

/**
 * @brief Registrace persist klíčů v cfg modulu pro danou instanci.
 *
 * Volá se z debugger_init() po cfgroot_register_new_module pro každou
 * instanci samostatně (sekce [MEMBROWSER_WINDOW_MAIN], [MEMBROWSER_WINDOW_2]
 * .._5). Zapíše handlery do interního static pole @c s_persist_*[idx],
 * cfgmain propagate naplní default hodnotami nebo načtenými z INI.
 * @ref membrowser_state_apply_persisted pak hodnoty zkopíruje do reálné
 * state struct při create instance.
 *
 * @param cmod          CFGMOD pointer (= MEMBROWSER_WINDOW_<X> modul).
 * @param instance_idx  0..MB_INSTANCE_COUNT-1. Mimo rozsah = no-op.
 */
void membrowser_state_register_persistence ( void *cmod, int instance_idx );

/**
 * @brief Aplikuje persisted hodnoty na state struct.
 *
 * Volá se po @ref membrowser_state_default v create cestě. Cílový slot
 * pole @c s_persist_*[] je určen z @c st->instance_idx (= musí být platný).
 * Hodnoty mimo platný rozsah se ignorují (zachová se default).
 *
 * @param st Cílový state (nesmí být NULL).
 */
void membrowser_state_apply_persisted ( st_MEMBROWSER_STATE *st );

/**
 * @brief Uloží aktuální state do persist úložiště pro instanci @c st->instance_idx.
 *
 * Volá se před save cfgmain (v @ref membrowser_view_destroy nebo per frame
 * při změně). Cílový slot pole @c s_persist_*[] je určen z @c st->instance_idx.
 *
 * @param st Zdrojový state (nesmí být NULL).
 */
void membrowser_state_save_to_persisted ( const st_MEMBROWSER_STATE *st );

/**
 * @brief Legacy fallback: zkopíruje persisted hodnoty MAIN slotu (=0) ze
 *        zdrojového slotu (legacy sekce @c [MEMBROWSER_WINDOW]).
 *
 * Workflow: na startu cfgmain načte legacy sekci @c [MEMBROWSER_WINDOW]
 * (pokud existuje v INI) přes @ref membrowser_state_register_persistence
 * volaný s @c instance_idx == MB_INSTANCE_LEGACY (vnitřní index nad rámec
 * pole - zde jen jako "scratch" slot). Pokud zároveň sekce
 * @c [MEMBROWSER_WINDOW_MAIN] chybí (= upgrade z V2 INI bez V3 sekce),
 * apply_persisted_legacy_to_main překopíruje scratch -> MAIN.
 *
 * Tato funkce slouží pouze pro upgrade migration; pro nové INI po V3 je
 * legacy sekce ignorována (pokud existují jak legacy tak MAIN, MAIN má
 * přednost - legacy se přečte ale nepřepíše MAIN).
 *
 * @param have_main_section_in_ini true = MAIN sekce byla v INI (přeskoč fallback).
 */
void membrowser_state_apply_legacy_fallback_if_needed ( bool have_main_section_in_ini );

#ifdef __cplusplus
}
#endif

#endif /* MEMBROWSER_STATE_H */
