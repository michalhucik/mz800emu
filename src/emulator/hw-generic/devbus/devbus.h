/**
 * @file   devbus.h
 * @brief  Sběrnice volitelných zařízení (devbus) - arch-neutrální jádro.
 *
 * Registr volitelných zařízení (karet) s deskriptorem, volbami v INI,
 * životním cyklem, dispečerskými tabulkami (porty IORQ, MMIO snoop zápisu),
 * video háčky, snapshotem a sekundárními video výstupy.
 *
 * Jádro nezná CPU ani paměť konkrétní platformy: zná jen adaptér
 * (st_DEVBUS_ARCH_OPS), který mu při devbus_init() předá platforma.
 * V D1 ho inicializuje jen adaptér MZ-700 (mzarch/mz700/devbus/); na MZ-800
 * a MZ-1500 se jádro přeloží, ale nikdo ho nezavolá (devbus_is_initialized()
 * vrací false, žádná sekce INI, komponenta snapshotu ani menu).
 *
 * Pravidla vypnutého stavu (bez připojeného zařízení): jádro nic nemění na
 * horké cestě. Adaptér se o potřebě obálek dozví přes
 * st_DEVBUS_ARCH_OPS::hooks_changed() a vymění callbacky CPU a pointer
 * kreslení řádku; v instrukční smyčce ani v přístupu do paměti nepřibude
 * žádná podmínka.
 *
 * Vlákna: registr (deskriptory) se plní jen v emu vlákně do devbus_start()
 * a pak je neměnný, takže ho UI, MCP a katalog I/O smějí číst bez zámku.
 * Dynamický stav (připojení, volby, tabulky) mění jen emu vlákno (zásahy
 * device_request, dbgapi pod zámkem stavu zařízení) nebo load snapshotu při
 * zastavené emulaci; UI ho čte pod device_state_lock().
 *
 * Postup přidání zařízení: devdoc/devbus.md.
 *
 * @par Licence: GPLv3
 */

#ifndef DEVBUS_H
#define DEVBUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "snapshot/snapshot.h"
#include "debugger/io_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/*                               Konstanty                                   */
/* ------------------------------------------------------------------------- */

/** @brief Strop statického registru zařízení. */
#define DEVBUS_MAX_DEVICES 8

/** @brief Nejvyšší počet voleb jednoho zařízení (bez rezervovaného "connected"). */
#define DEVBUS_MAX_OPTIONS 16

/** @brief Nejvyšší počet rozsahů portů jednoho zařízení. */
#define DEVBUS_MAX_PORT_RANGES 4

/** @brief Nejvyšší počet MMIO rozsahů jednoho zařízení. */
#define DEVBUS_MAX_MMIO_RANGES 4

/** @brief Nejvyšší počet regionů paměti jednoho zařízení (sub_id = slot * 16 + index). */
#define DEVBUS_MAX_REGIONS 4

/** @brief Nejvyšší počet sekundárních video výstupů jednoho zařízení. */
#define DEVBUS_MAX_VOUTS 2

/** @brief Nejvyšší délka id zařízení a klíče volby (bez nuly). */
#define DEVBUS_ID_MAX 15

/** @brief Nejvyšší délka názvu sekce INI zařízení (bez nuly). */
#define DEVBUS_INI_SECTION_MAX 31

/** @brief Jádro MZ-700 (exe PAL i NTSC). */
#define DEVBUS_PLATFORM_MZ700  (1u << 0)
/** @brief Rezerva, D1 neinicializuje. */
#define DEVBUS_PLATFORM_MZ800  (1u << 1)
/** @brief Rezerva, D1 neinicializuje. */
#define DEVBUS_PLATFORM_MZ1500 (1u << 2)

/**
 * @brief Výsledek operací devbus (záporné = chyba).
 */
typedef enum en_DEVBUS_RESULT {
    DEVBUS_OK = 0,               /**< Úspěch (devbus_register() vrací místo něj číslo slotu >= 0). */
    DEVBUS_ERR_INVALID = -1,     /**< Chybný argument nebo deskriptor. */
    DEVBUS_ERR_FULL = -2,        /**< Registr je plný (DEVBUS_MAX_DEVICES). */
    DEVBUS_ERR_EXISTS = -3,      /**< Id už je registrované. */
    DEVBUS_ERR_CONFLICT = -4,    /**< Kolize portu, MMIO rozsahu nebo video háčku. */
    DEVBUS_ERR_UNSUPPORTED = -5, /**< Platforma nebo druh háčku není podporován (i devbus bez inicializace). */
    DEVBUS_ERR_NOT_FOUND = -6,   /**< Neznámé id nebo slot. */
    DEVBUS_ERR_DEVICE = -7,      /**< connect() nebo init() zařízení selhal; zůstává odpojené. */
} en_DEVBUS_RESULT;

/* ------------------------------------------------------------------------- */
/*                                 Volby                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief Typ volby zařízení.
 */
typedef enum en_DEVBUS_OPT_TYPE {
    DEVBUS_OPT_BOOL = 0,   /**< 0 / 1, v INI BOOL. */
    DEVBUS_OPT_KEYWORD,    /**< Index do pole klíčových slov, v INI klíčové slovo. */
    DEVBUS_OPT_UNSIGNED,   /**< Číslo v rozsahu min..max, v INI UNSIGNED. */
} en_DEVBUS_OPT_TYPE;

/**
 * @brief Kdy se změna volby projeví.
 */
typedef enum en_DEVBUS_OPT_APPLY {
    DEVBUS_APPLY_LIVE = 0,  /**< Ihned: u připojeného zařízení option_changed() za běhu. */
    DEVBUS_APPLY_CONNECT,   /**< Při příštím připojení (např. obsah RAM po zapnutí). */
} en_DEVBUS_OPT_APPLY;

/**
 * @brief Volba zařízení = prvek INI v sekci zařízení + položka v GUI a MCP.
 *
 * @invariant key odpovídá [a-z][a-z0-9_]{0,14}, není "connected" a je
 *            v rámci zařízení unikátní; def je platná hodnota podle typu
 *            (BOOL 0/1, KEYWORD index < počet slov, UNSIGNED min..max);
 *            keyword_labels je NULL, nebo (jen u KEYWORD) má nenulový
 *            popisek pro každé klíčové slovo.
 * @note Vlastnictví: statická data zařízení (žijí do konce procesu).
 */
typedef struct st_DEVBUS_OPTION {
    const char *key;              /**< INI klíč; zároveň klíč v MCP a snapshotu. */
    const char *label;            /**< Anglický msgid (UI ho překládá, soubor UI zařízení ho značí N_()). */
    en_DEVBUS_OPT_TYPE type;      /**< Typ hodnoty. */
    unsigned def;                 /**< Výchozí hodnota: 0/1, index klíčového slova, číslo. */
    unsigned min;                 /**< Jen UNSIGNED: nejmenší hodnota. */
    unsigned max;                 /**< Jen UNSIGNED: největší hodnota. */
    const char *const *keywords;  /**< Jen KEYWORD: slova pro INI zakončená NULL ([a-z0-9_]). */
    en_DEVBUS_OPT_APPLY apply;    /**< Kdy se změna projeví. */
    /**
     * Jen KEYWORD, nepovinné: anglické msgid popisků hodnot pro menu
     * (stejný počet a pořadí jako keywords, soubor UI zařízení je značí
     * N_()). NULL = menu ukazuje klíčová slova z INI. INI, MCP a snapshot
     * pracují vždy s klíčovými slovy.
     */
    const char *const *keyword_labels;
} st_DEVBUS_OPTION;

/* ------------------------------------------------------------------------- */
/*                               I/O porty                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Rozsah portů zařízení (dekód podle dolního bajtu adresy, jako dnes).
 *
 * Handlery volá emu vlákno z obálky portového callbacku CPU po
 * mzarch_main_insideop_iorq() (čas je srovnaný s paprskem), takže smějí
 * volat devbus_cpu_wait() a devbus_time_ticks().
 *
 * @invariant first <= last; žádný port rozsahu neobsluhuje statický switch
 *            platformy (st_DEVBUS_ARCH_OPS::port_is_static).
 */
typedef struct st_DEVBUS_PORT_RANGE {
    uint8_t first;                                          /**< První port (včetně). */
    uint8_t last;                                           /**< Poslední port (včetně). */
    uint8_t (*read)(void *ctx, uint16_t port);              /**< IN; NULL = čtení vrací latch sběrnice. */
    uint8_t (*peek)(void *ctx, uint16_t port);              /**< Čtení bez vedlejšího efektu (debugger); NULL = latch. */
    void    (*write)(void *ctx, uint16_t port, uint8_t v);  /**< OUT; NULL = zápis se ignoruje. */
} st_DEVBUS_PORT_RANGE;

/**
 * @brief Slot záložní tabulky portů.
 * @invariant range == NULL právě tehdy, když port nepatří žádnému připojenému zařízení.
 */
typedef struct st_DEVBUS_PORT_SLOT {
    const st_DEVBUS_PORT_RANGE *range; /**< Rozsah připojeného zařízení, nebo NULL. */
    void *ctx;                         /**< Kontext zařízení pro handlery. */
} st_DEVBUS_PORT_SLOT;

/**
 * @brief Záložní tabulka 256 portů (index = dolní bajt adresy).
 *
 * Zapisuje ji jen devbus_connect() / devbus_disconnect() (emu vlákno);
 * čte ji obálka portového callbacku adaptéru.
 */
extern st_DEVBUS_PORT_SLOT g_devbus_ports[256];

/* ------------------------------------------------------------------------- */
/*                          MMIO (snoop zápisu)                              */
/* ------------------------------------------------------------------------- */

/** @brief Stav mapy: horní oblast mapovaná (MZ-700: OUT 0E3h / 0E4h, reset). */
#define DEVBUS_MMIO_MAPPED    (1u << 0)
/** @brief Stav mapy: na MMIO okně je DRAM (MZ-700: po OUT 0E1h). */
#define DEVBUS_MMIO_DRAM      (1u << 1)
/** @brief Maska stavů mapy. */
#define DEVBUS_MMIO_WHEN_MASK (DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM)
/** @brief Zápis instrukcí CPU (handler smí volat devbus_cpu_wait()). */
#define DEVBUS_ACC_CPU        (1u << 8)
/** @brief Zápis debuggeru / MCP (bez synchronizace s paprskem). */
#define DEVBUS_ACC_DEBUGGER   (1u << 9)
/** @brief V okamžiku zápisu platí Prohibited (MZ-700: OUT 0E5h). */
#define DEVBUS_ACC_PROHIBITED (1u << 10)

/**
 * @brief MMIO rozsah; zařízení zápis jen pozoruje, chování desky se nemění.
 *
 * Handler dostane v @c flags stav mapy (DEVBUS_MMIO_MAPPED nebo
 * DEVBUS_MMIO_DRAM) a druh přístupu (DEVBUS_ACC_*). V CPU cestě volá adaptér
 * před prvním handlerem synchronizaci času (st_DEVBUS_ARCH_OPS::mmio_sync).
 *
 * @invariant first <= last; pro každý stav v when_default leží rozsah v okně
 *            platformy pro ten stav (MZ-700: MAPPED 0E009h-0EFFFh, DRAM
 *            0E000h-0EFFFh); write != NULL.
 */
typedef struct st_DEVBUS_MMIO_RANGE {
    uint16_t first;         /**< První adresa (včetně). */
    uint16_t last;          /**< Poslední adresa (včetně). */
    unsigned when_default;  /**< Výchozí maska DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM (0 = neaktivní). */
    void (*write)(void *ctx, uint16_t addr, uint8_t value, unsigned flags); /**< Pozorovaný zápis. */
} st_DEVBUS_MMIO_RANGE;

/* ------------------------------------------------------------------------- */
/*                                 Video                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief Vrátí aktuální 4 kB generátor znaků ve formátu CG-ROM.
 *
 * Formát MZ-700: 2 sady x 256 znaků x 8 B, levý bod = bit 0. Volá se jednou
 * za vykreslovaný řádek (emu vlákno, uvnitř zpracování události GDG).
 *
 * @param ctx Kontext zařízení.
 * @return Ukazatel na 4096 B platný aspoň do dalšího volání; nikdy NULL.
 */
typedef const uint8_t *(*devbus_glyph_fn)(void *ctx);

/**
 * @brief Vykreslí jeden řádek plátna MZ-700.
 *
 * @param ctx      Kontext zařízení.
 * @param canvas_y 0 .. VIDEO_CANVAS_HEIGHT - 1.
 * @param cg       Platný generátor znaků (zdroj glyfů, nebo g_rom.cgrom).
 * @param dst      Začátek řádku plátna ve framebufferu (VIDEO_CANVAS_WIDTH
 *                 bajtů, hodnoty IGRB jako devbus_mz700_colormap[]).
 * @pre Emu vlákno, uvnitř zpracování události GDG. Nesmí měnit stav stroje.
 */
typedef void (*devbus_row_fn)(void *ctx, unsigned canvas_y, const uint8_t *cg, uint8_t *dst);

/**
 * @brief Video háčky zařízení.
 * @invariant Aspoň jeden z ukazatelů je nenulový; připojený smí být nejvýš
 *            jeden glyph_source a nejvýš jeden render_row.
 */
typedef struct st_DEVBUS_VIDEO_OPS {
    devbus_glyph_fn glyph_source;  /**< NULL = nemění glyfy (SUC). */
    devbus_row_fn   render_row;    /**< NULL = nemění rozložení (PCG-700). */
} st_DEVBUS_VIDEO_OPS;

/**
 * @brief Barvy MZ-700: index 0-7 -> hodnota IGRB v pixbuf.
 *
 * Shodné s c_MZ700_COLORMAP (mzarch/mz700/gdg/mz700_framebuffer.c).
 */
extern const uint8_t devbus_mz700_colormap[8];

/* ------------------------------------------------------------------------- */
/*                                Regiony                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Paměť zařízení pro Memory Browser a MCP region_read / region_write.
 * @invariant name je ASCII anglicky; size > 0; data != NULL.
 */
typedef struct st_DEVBUS_REGION {
    const char *name;                 /**< Např. "PCG-700 RAM (2 KB)". */
    uint32_t size;                    /**< Velikost v bajtech. */
    int writable;                     /**< Nenulové = debugger smí zapisovat. */
    uint8_t *(*data)(void *ctx);      /**< Přímý ukazatel na paměť zařízení (emu vlákno). */
    void (*on_write)(void *ctx, uint32_t off, uint32_t len); /**< Po zápisu debuggerem; NULL = nic. */
} st_DEVBUS_REGION;

/* ------------------------------------------------------------------------- */
/*                                Snapshot                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Kontext snapshotu předávaný zařízení (soubory pod "devices/devbus/<id>/").
 *
 * Neprůhledný typ; platí jen během volání save() / load().
 */
typedef struct st_DEVBUS_SNAP st_DEVBUS_SNAP;

/**
 * @brief Stav zařízení pro snapshot.
 * @invariant version >= 1, save a load nenulové.
 */
typedef struct st_DEVBUS_SNAP_OPS {
    uint32_t version;  /**< Verze stavu zařízení (od 1); zvýšit při změně formátu. */
    /** @brief Uloží stav (jen připojené zařízení). Výstup hostitele (vout) neukládat. */
    en_SNAPSHOT_RESULT (*save)(void *ctx, st_DEVBUS_SNAP *s);
    /** @brief Načte stav verze @p version (<= version); chybějící pole = výchozí hodnota. */
    en_SNAPSHOT_RESULT (*load)(void *ctx, st_DEVBUS_SNAP *s, uint32_t version);
} st_DEVBUS_SNAP_OPS;

/* ------------------------------------------------------------------------- */
/*                            Životní cyklus                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Callbacky životního cyklu.
 *
 * Volá je emu vlákno, nebo vlákno nahrávající snapshot při zastavené
 * emulaci. NULL = nic (connect() NULL = úspěch).
 */
typedef struct st_DEVBUS_LIFECYCLE {
    int  (*init)(void *ctx);        /**< Jednou při registraci: alokace, výchozí stav. 0 = OK. Slot už vrací devbus_find(). */
    int  (*connect)(void *ctx);     /**< Zapnutí napájení zařízení (obsah RAM podle voleb). 0 = OK. */
    void (*reset)(void *ctx);       /**< Signál RESET stroje (mzarch_main_reset). */
    void (*disconnect)(void *ctx);  /**< Odpojení; háčky už jsou odpojené. */
    void (*destroy)(void *ctx);     /**< Konec procesu (devbus_exit()). */
    void (*option_changed)(void *ctx, unsigned opt, unsigned value); /**< Jen APPLY_LIVE u připojeného zařízení. */
    void (*frame_done)(void *ctx);  /**< Konec snímku (jen připojené zařízení); nesmí volat insideop (I/O, VRAM). */
    void (*after_load)(void *ctx);  /**< Po načtení celého snapshotu: odvozený stav, překreslení. */
    void (*rom_changed)(void *ctx); /**< Změna ROM / CG-ROM (rom_reinstall), jen připojené zařízení. */
} st_DEVBUS_LIFECYCLE;

/* ------------------------------------------------------------------------- */
/*                               Deskriptor                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Neměnný popis zařízení (statická data zařízení).
 *
 * @invariant id odpovídá [a-z][a-z0-9_]{0,14}, je unikátní a stabilní napříč
 *            verzemi (klíč snapshotu i MCP kind); ini_section odpovídá
 *            [A-Z0-9_]{1,31} a nekoliduje s existující sekcí INI.
 * @note Vlastnictví: deskriptor i pole v něm patří zařízení a musí žít do
 *       konce procesu, devbus ukládá jen ukazatele.
 */
typedef struct st_DEVBUS_DEVICE_DESC {
    const char *id;                       /**< Např. "pcg700". */
    const char *name;                     /**< Anglický název pro UI (msgid) a MCP. */
    const char *ini_section;              /**< Např. "PCG700". */
    unsigned platforms;                   /**< Maska DEVBUS_PLATFORM_*. */
    const st_DEVBUS_OPTION *options;      /**< Volby (smí být NULL při option_count 0). */
    unsigned option_count;                /**< Počet voleb (<= DEVBUS_MAX_OPTIONS). */
    const st_DEVBUS_PORT_RANGE *ports;    /**< Rozsahy portů. */
    unsigned port_count;                  /**< Počet rozsahů (<= DEVBUS_MAX_PORT_RANGES). */
    const st_DEVBUS_MMIO_RANGE *mmio;     /**< MMIO rozsahy. */
    unsigned mmio_count;                  /**< Počet rozsahů (<= DEVBUS_MAX_MMIO_RANGES). */
    const st_DEVBUS_VIDEO_OPS *video;     /**< NULL = obraz nemění. */
    const st_DEVBUS_REGION *regions;      /**< Regiony paměti pro debugger. */
    unsigned region_count;                /**< Počet regionů (<= DEVBUS_MAX_REGIONS). */
    const st_IO_PORT_DESC *io_catalog;    /**< Popisy portů / MMIO pro okno I/O. */
    unsigned io_catalog_count;            /**< Počet popisů. */
    st_DEVBUS_SNAP_OPS snap;              /**< Stav pro snapshot (povinné). */
    const st_DEVBUS_LIFECYCLE *ops;       /**< Životní cyklus (povinné). */
} st_DEVBUS_DEVICE_DESC;

/* ------------------------------------------------------------------------- */
/*                                Adaptér                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Adaptér platformy; jádro nezná CPU ani paměť, volá jen tyto funkce.
 *
 * @invariant Struktura žije do devbus_exit(); hooks_changed nenulové.
 */
typedef struct st_DEVBUS_ARCH_OPS {
    unsigned platform;                        /**< DEVBUS_PLATFORM_*. */
    bool (*port_is_static)(uint8_t port_lsb); /**< Port obsluhuje statický switch (NULL = žádný). */
    void (*hooks_changed)(void);              /**< Přepočítat CPU callbacky a pointer řádku. */
    void (*cpu_wait)(unsigned tstates);       /**< Implementace devbus_cpu_wait() (NULL = nic). */
    uint64_t (*time_ticks)(void);             /**< Implementace devbus_time_ticks() (NULL = 0). */
    void (*video_invalidate)(void);           /**< Implementace devbus_video_invalidate() (NULL = nic). */
    void (*mmio_sync)(void);                  /**< Srovnání času před MMIO handlerem v CPU cestě (NULL = nic). */
    uint16_t mmio_mapped_first;               /**< Okno MMIO ve stavu DEVBUS_MMIO_MAPPED (first > last = nepodporováno). */
    uint16_t mmio_mapped_last;                /**< Konec okna MAPPED (včetně). */
    uint16_t mmio_dram_first;                 /**< Okno MMIO ve stavu DEVBUS_MMIO_DRAM (first > last = nepodporováno). */
    uint16_t mmio_dram_last;                  /**< Konec okna DRAM (včetně). */
} st_DEVBUS_ARCH_OPS;

/**
 * @brief Souhrn háčků připojených zařízení pro adaptér.
 *
 * Přepočítává ho jádro při každé změně připojení a devbus_mmio_set_when()
 * před voláním hooks_changed(). Čte ho adaptér (emu vlákno).
 *
 * @invariant ports == true, právě když g_devbus_ports má aspoň jeden obsazený
 *            slot; mmio == true, právě když existuje připojený MMIO rozsah
 *            s nenulovou maskou stavů.
 */
typedef struct st_DEVBUS_HOOKS {
    bool ports;               /**< Je potřeba obálka portových callbacků. */
    bool mmio;                /**< Je potřeba obálka zápisu do paměti. */
    devbus_glyph_fn glyph;    /**< Zdroj glyfů připojeného zařízení, nebo NULL. */
    void *glyph_ctx;          /**< Kontext zdroje glyfů. */
    devbus_row_fn row;        /**< Náhrada kreslení řádku, nebo NULL. */
    void *row_ctx;            /**< Kontext náhrady kreslení. */
} st_DEVBUS_HOOKS;

/** @brief Aktuální souhrn háčků (viz st_DEVBUS_HOOKS). */
extern st_DEVBUS_HOOKS g_devbus_hooks;

/**
 * @brief Inicializuje prázdný registr a uloží adaptér.
 * @param ops Adaptér platformy (statický, nesmí být NULL).
 * @pre Emu vlákno v mzarch_platform_fn_init() (jen adaptér MZ-700), nebo test.
 * @post devbus_is_initialized() == true; registr je prázdný a otevřený.
 */
void devbus_init(const st_DEVBUS_ARCH_OPS *ops);

/**
 * @brief Konec registrace: připojí zařízení, která mají v INI connected = 1.
 * @pre devbus_init(); emu vlákno na konci mzarch_platform_fn_init().
 * @post Registr je neměnný; selhání připojení vypíše varování a zařízení
 *       zůstane odpojené.
 */
void devbus_start(void);

/**
 * @brief Ukončí devbus: odpojí a zničí zařízení, vyprázdní registr.
 * @post devbus_is_initialized() == false; g_devbus_ports a g_devbus_hooks
 *       jsou prázdné. Sekce INI zůstávají v g_cfgmain, ale jejich elementy
 *       ukazují na vynulované úložiště: INI je nutné uložit před
 *       devbus_exit() (emulátor ukládá v cfgmain_exit(), které běží před
 *       mzarch_platform_fn_exit()). Bez inicializace nic nedělá.
 * @note Volá mzarch_platform_fn_exit() a testy (opakovaná inicializace).
 */
void devbus_exit(void);

/** @brief false na MZ-800 a MZ-1500 (devbus nikdo neinicializoval). */
bool devbus_is_initialized(void);

/**
 * @brief Zaregistruje zařízení, založí a načte jeho sekci INI, zavolá init().
 *
 * @param desc Deskriptor (statický).
 * @param ctx  Kontext předávaný všem callbackům zařízení.
 * @return Číslo slotu (>= 0) při úspěchu; DEVBUS_ERR_FULL, DEVBUS_ERR_EXISTS,
 *         DEVBUS_ERR_INVALID (chybný deskriptor, porty kolidující se
 *         statickými porty platformy, MMIO mimo okno, chybné volby,
 *         kolize sekce INI, registrace po devbus_start()),
 *         DEVBUS_ERR_UNSUPPORTED (devbus není inicializovaný, platforma
 *         není v masce, MMIO na platformě bez okna), DEVBUS_ERR_DEVICE
 *         (init() selhal; slot se uvolní).
 * @pre Emu vlákno v mzarch_platform_fn_init() po devbus_init(), před devbus_start().
 * @post Zařízení je odpojené; volby mají hodnoty z INI (jinak výchozí).
 */
int devbus_register(const st_DEVBUS_DEVICE_DESC *desc, void *ctx);

/**
 * @brief Připojí zařízení: kolize -> connect() -> tabulky -> hooks_changed().
 * @param slot Slot z devbus_register().
 * @return DEVBUS_OK (i když už připojené - pak beze změny a bez
 *         hooks_changed()), DEVBUS_ERR_NOT_FOUND, DEVBUS_ERR_CONFLICT
 *         (kolize s jiným připojeným zařízením: porty, MMIO, render_row,
 *         glyph_source), DEVBUS_ERR_DEVICE (connect() selhal; tabulky
 *         beze změny).
 * @pre Emu vlákno mezi instrukcemi, nebo zastavená emulace (load snapshotu).
 * @note STATE_REV zvedá volající cesta (device_request, dbgapi, snapshot).
 */
int devbus_connect(int slot);

/**
 * @brief Odpojí zařízení: tabulky pryč -> hooks_changed() -> disconnect().
 * @param slot Slot z devbus_register().
 * @return DEVBUS_OK (i když už odpojené - pak bez hooks_changed()),
 *         DEVBUS_ERR_NOT_FOUND.
 * @pre Jako devbus_connect().
 */
int devbus_disconnect(int slot);

/**
 * @brief Najde slot podle id.
 * @param id Id zařízení (smí být NULL).
 * @return Slot (>= 0), nebo DEVBUS_ERR_NOT_FOUND.
 * @note Libovolné vlákno po devbus_start(); během registrace jen emu vlákno.
 */
int devbus_find(const char *id);

/** @brief Počet registrovaných zařízení (sloty 0 .. count - 1). */
unsigned devbus_count(void);

/**
 * @brief Deskriptor zařízení ve slotu.
 * @return Deskriptor, nebo NULL při neplatném slotu.
 */
const st_DEVBUS_DEVICE_DESC *devbus_desc(int slot);

/**
 * @brief Kontext zařízení ve slotu (z devbus_register()).
 * @return Kontext, nebo NULL při neplatném slotu.
 */
void *devbus_ctx(int slot);

/**
 * @brief Je zařízení připojené?
 * @return false i při neplatném slotu.
 * @note Atomické čtení: smí volat i UI bez zámku (např. katalog I/O).
 */
bool devbus_is_connected(int slot);

/** @brief Počet připojených zařízení (atomické čtení). */
unsigned devbus_connected_count(void);

/** @brief reset() všech připojených zařízení (mzarch_main_reset). */
void devbus_reset_all(void);

/**
 * @brief frame_done() připojených zařízení, která ho mají.
 * Bez takového zařízení jedno čtení čítače.
 */
void devbus_frame_done(void);

/** @brief rom_changed() připojených zařízení; bez inicializace okamžitý návrat. */
void devbus_rom_changed(void);

/**
 * @brief after_load() připojených zařízení po načtení celého snapshotu.
 * Bez inicializace okamžitý návrat. Volá snapshot_mgr před plným
 * překreslením obrazu.
 */
void devbus_after_load(void);

/* ------------------------------------------------------------------------- */
/*                              Volby (API)                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Aktuální hodnota volby.
 * @return Hodnota, nebo 0 při neplatném slotu / indexu.
 * @note Emu vlákno, nebo UI pod device_state_lock().
 */
unsigned devbus_option_get(int slot, unsigned opt);

/**
 * @brief Nastaví volbu.
 *
 * Ověří hodnotu podle typu, uloží ji (INI ji uloží při ukončení). U volby
 * DEVBUS_APPLY_LIVE připojeného zařízení zavolá option_changed() (jen při
 * skutečné změně hodnoty).
 *
 * @return DEVBUS_OK, DEVBUS_ERR_NOT_FOUND (slot, index), DEVBUS_ERR_INVALID (hodnota).
 * @pre Emu vlákno (zásah device_request, dbgapi).
 */
int devbus_option_set(int slot, unsigned opt, unsigned value);

/**
 * @brief Najde volbu podle klíče.
 * @return Index volby, nebo DEVBUS_ERR_NOT_FOUND.
 */
int devbus_option_find(int slot, const char *key);

/**
 * @brief Převede text na hodnotu volby ("0"/"1"/"true"/"false", klíčové slovo, číslo).
 * @param slot Slot.
 * @param opt  Index volby.
 * @param text Text (nesmí být NULL).
 * @param out  Výstup hodnoty.
 * @return true = text je platná hodnota (*out zapsán).
 */
bool devbus_option_parse(int slot, unsigned opt, const char *text, unsigned *out);

/**
 * @brief Textová podoba hodnoty volby (klíčové slovo jménem, BOOL "0"/"1", číslo).
 * @param buf  Výstup (nejméně 16 B).
 * @param size Velikost @p buf.
 * @return @p buf, nebo "" při neplatném slotu / indexu.
 */
const char *devbus_option_format(int slot, unsigned opt, unsigned value, char *buf, size_t size);

/**
 * @brief Najde volbu zařízení podle sekce a klíče INI.
 * @param module  Název sekce INI (porovnání bez ohledu na velikost písmen, jako cfgfile).
 * @param element Klíč (porovnání bez ohledu na velikost písmen).
 * @param opt_out Výstup indexu volby (smí být NULL); pro "connected" se
 *                nezapíše.
 * @return Slot (>= 0), když jde o volbu zařízení; DEVBUS_ERR_INVALID, když
 *         jde o rezervovaný klíč "connected" (mění ho jen periph_*);
 *         DEVBUS_ERR_NOT_FOUND jinak.
 * @note Čte jen neměnný registr; libovolné vlákno po devbus_start().
 */
int devbus_option_key(const char *module, const char *element, unsigned *opt_out);

/* ------------------------------------------------------------------------- */
/*                                  MMIO                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief Změní stavy mapy, ve kterých MMIO rozsah reaguje (volba za běhu).
 * @param slot      Slot.
 * @param range_idx Index rozsahu v deskriptoru.
 * @param when      Maska DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM (0 = vypnuto).
 * @return DEVBUS_OK, DEVBUS_ERR_NOT_FOUND, DEVBUS_ERR_INVALID (maska nebo
 *         rozsah mimo okno stavu).
 * @post Volá hooks_changed(), když se tím mění potřeba obálky zápisu.
 * @pre Emu vlákno.
 */
int devbus_mmio_set_when(int slot, unsigned range_idx, unsigned when);

/**
 * @brief Aktuální maska stavů MMIO rozsahu.
 * @return Maska, nebo 0 při neplatném slotu / indexu.
 */
unsigned devbus_mmio_get_when(int slot, unsigned range_idx);

/**
 * @brief Rozešle pozorovaný zápis aktivním MMIO rozsahům.
 *
 * Volá ho obálka zápisu adaptéru po zápisu do desky. Rozsah reaguje, když
 * adresa leží v rozsahu a stav mapy z @p flags je v jeho masce. V CPU cestě
 * (DEVBUS_ACC_CPU) zavolá před prvním handlerem st_DEVBUS_ARCH_OPS::mmio_sync.
 *
 * @param addr  Adresa zápisu.
 * @param value Zapsaný bajt.
 * @param flags Stav mapy | DEVBUS_ACC_*.
 * @pre Emu vlákno.
 */
void devbus_mmio_dispatch(uint16_t addr, uint8_t value, unsigned flags);

/**
 * @brief Patří adresa MMIO rozsahu některého připojeného zařízení (bez ohledu na masku stavů)?
 * @note Pro sledování v debuggeru (okno I/O, Event Viewer); lineární průchod.
 */
bool devbus_mmio_claims(uint16_t addr);

/* ------------------------------------------------------------------------- */
/*                                  Porty                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Čtení portu zařízení bez vedlejšího efektu (debugger, katalog).
 * @param port Adresa portu (dekód podle dolního bajtu).
 * @param out  Výstup hodnoty.
 * @return true = port patří připojenému zařízení s peek (*out zapsán);
 *         false = volající použije latch sběrnice.
 */
bool devbus_port_peek(uint16_t port, uint8_t *out);

/* ------------------------------------------------------------------------- */
/*                             WAIT, čas, obraz                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Prodlouží aktuální cyklus CPU o @p tstates.
 *
 * Jen z portového handleru nebo MMIO handleru s DEVBUS_ACC_CPU. Adaptér
 * MZ-700: z80_add_wait_states() + saturovaná
 * g_mzarch_main.instruction_wait_tstates.
 */
void devbus_cpu_wait(unsigned tstates);

/** @brief Čas emulace v taktech GDG od startu (snímky * takty snímku + takty snímku). */
uint64_t devbus_time_ticks(void);

/** @brief Vyžádá překreslení obrazu (MZ-700: screen_changes = SCRSTS_THIS_IS_CHANGED). Emu vlákno. */
void devbus_video_invalidate(void);

/* ------------------------------------------------------------------------- */
/*                         Regiony a katalog I/O                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief Region paměti zařízení.
 * @param slot Slot.
 * @param idx  Index regionu v deskriptoru.
 * @return Popis regionu, nebo NULL.
 */
const st_DEVBUS_REGION *devbus_region(int slot, unsigned idx);

/**
 * @brief Celkový počet položek katalogu I/O všech registrovaných zařízení.
 * @note Registr je po devbus_start() neměnný, počet se nemění.
 */
size_t devbus_io_catalog_total(void);

/**
 * @brief Položka katalogu I/O zařízení podle souhrnného indexu.
 * @param i        0 .. devbus_io_catalog_total() - 1.
 * @param slot_out Výstup slotu vlastníka (smí být NULL).
 * @return Položka, nebo NULL při indexu mimo rozsah.
 */
const st_IO_PORT_DESC *devbus_io_catalog_at(size_t i, int *slot_out);

/* ------------------------------------------------------------------------- */
/*                   Sekundární video výstup (druhý monitor)                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Popis sekundárního výstupu; pixely INDEX8 do vlastní palety.
 * @invariant id odpovídá [a-z][a-z0-9_]{0,14}; width, height, palette_size > 0;
 *            palette má palette_size položek.
 * @note Vlastnictví: statická data zařízení.
 */
typedef struct st_DEVBUS_VOUT_DESC {
    const char *id;           /**< Např. "kp80"; klíč pro UI a MCP. */
    const char *title;        /**< Anglický msgid titulku okna. */
    uint16_t width;           /**< Šířka v bodech. */
    uint16_t height;          /**< Výška v bodech. */
    float pixel_aspect;       /**< Poměr stran bodu pro zobrazení (šířka / výška). */
    const uint32_t *palette;  /**< Barvy RGBA (0xRRGGBBAA), palette_size položek. */
    uint16_t palette_size;    /**< Počet barev. */
} st_DEVBUS_VOUT_DESC;

/** @brief Sekundární výstup (neprůhledný; vlastní ho devbus, žije do devbus_exit()). */
typedef struct st_DEVBUS_VOUT st_DEVBUS_VOUT;

/**
 * @brief Založí sekundární výstup zařízení.
 * @param slot Slot zařízení (v init() ho vrací devbus_find(desc->id)).
 * @param d    Popis (statický).
 * @return Výstup, nebo NULL (neplatný popis, slot, plno).
 * @pre Emu vlákno v init() zařízení (před devbus_start()).
 * @post Všechny tři buffery jsou vynulované (index 0).
 */
st_DEVBUS_VOUT *devbus_vout_create(int slot, const st_DEVBUS_VOUT_DESC *d);

/**
 * @brief Buffer, do kterého emu vlákno kreslí další snímek (width * height B).
 * @note Obsah = poslední předaný snímek (devbus_vout_present() ho zkopíruje),
 *       zařízení smí kreslit jen změny.
 */
uint8_t *devbus_vout_draw_buffer(st_DEVBUS_VOUT *o);

/**
 * @brief Předá hotový snímek čtenářům (emu vlákno).
 *
 * Trojitý buffer s atomickou výměnou: emu vlákno na čtenáře nikdy nečeká.
 */
void devbus_vout_present(st_DEVBUS_VOUT *o);

/**
 * @brief Zkopíruje poslední předaný snímek, je-li jiný než *io_seen.
 * @param o        Výstup.
 * @param dst      Cíl (nejméně width * height B).
 * @param dst_size Velikost @p dst.
 * @param io_seen  Vstup: pořadí naposledy viděného snímku; výstup: pořadí
 *                 zkopírovaného snímku (0 = zatím žádný).
 * @return true = zkopírován novější snímek.
 * @note Jeden čtenář na výstup (UI vlákno, nebo MCP v emu vlákně pod zámkem
 *       stavu zařízení); vlastní buffer čtenáře emu vlákno nepřepisuje.
 */
bool devbus_vout_fetch(st_DEVBUS_VOUT *o, uint8_t *dst, size_t dst_size, uint32_t *io_seen);

/** @brief Počet sekundárních výstupů všech registrovaných zařízení. */
unsigned devbus_vout_count(void);

/** @brief Výstup podle indexu 0 .. devbus_vout_count() - 1, jinak NULL. */
st_DEVBUS_VOUT *devbus_vout_at(unsigned i);

/** @brief Popis výstupu. */
const st_DEVBUS_VOUT_DESC *devbus_vout_desc(const st_DEVBUS_VOUT *o);

/** @brief Slot zařízení, kterému výstup patří. */
int devbus_vout_slot(const st_DEVBUS_VOUT *o);

/* ------------------------------------------------------------------------- */
/*                    Snapshot - pomocníci pro zařízení                      */
/* ------------------------------------------------------------------------- */

/** @brief Zapíše XML soubor zařízení @p name (relativně k "devices/devbus/<id>/"). */
en_SNAPSHOT_RESULT devbus_snap_write_xml(st_DEVBUS_SNAP *s, const char *name, const char *xml);
/** @brief Zapíše binární soubor zařízení @p name. */
en_SNAPSHOT_RESULT devbus_snap_write_bin(st_DEVBUS_SNAP *s, const char *name, const void *d, size_t n);
/** @brief Přečte XML soubor zařízení; *out_xml uvolní volající přes g_free(). */
en_SNAPSHOT_RESULT devbus_snap_read_xml(st_DEVBUS_SNAP *s, const char *name, char **out_xml);
/** @brief Přečte binární soubor zařízení přesné velikosti @p n do @p d. */
en_SNAPSHOT_RESULT devbus_snap_read_bin_into(st_DEVBUS_SNAP *s, const char *name, void *d, size_t n);
/** @brief Existuje soubor zařízení @p name? */
bool devbus_snap_exists(st_DEVBUS_SNAP *s, const char *name);
/** @brief true = ukládá se jen pro hash stavu: vynechat pole, která nejsou stavem stroje. */
bool devbus_snap_for_hash(const st_DEVBUS_SNAP *s);

/**
 * @brief Zaregistruje komponentu snapshotu "devbus" (jen inicializovaný devbus).
 *
 * Volá snapshot_init(); bez inicializace nic nedělá (MZ-800, MZ-1500).
 * Komponenta je volitelná, priorita SNAPSHOT_PRIORITY_DEVICE.
 */
void devbus_snap_register(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVBUS_H */
