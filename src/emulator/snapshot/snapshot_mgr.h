/**
 * @file snapshot_mgr.h
 * @brief Snapshot Manager — registrace komponent, orchestrace save/load
 *
 * Každá HW komponenta registruje své save/load callbacky do centrálního
 * registru. Při save/load se callbacky volají v pořadí dle priority.
 */

#ifndef SNAPSHOT_MGR_H
#define SNAPSHOT_MGR_H

#include "snapshot.h"
#include "snapshot_io.h"
#include "mzarch/mzarch_config.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Kontext pro save/load operaci
 *
 * Snapshot Manager vytvoří tento kontext a předá ho handleru.
 * Handler přes něj přistupuje k ZIP archivu.
 */
typedef struct st_SNAPSHOT_CONTEXT {
    snapshot_io_t *io;            /* I/O handle pro čtení/zápis */
    int format_version;           /* Verze formátu */
    int architecture;             /* MZARCH hodnota */
    bool saving;                  /* true = save, false = load */
    bool for_hash;                /* true = save jen pro hash stavu (snapshot_hash_components):
                                   * handler vynechá pole, která nejsou stavem stroje, ale
                                   * řízení emulátoru nebo průběhu vykreslení pro hostitele
                                   * (mění se např. při pauze, při stejném emulovaném stavu) */
} st_SNAPSHOT_CONTEXT;


/**
 * Callback prototypy pro HW komponenty
 */
typedef en_SNAPSHOT_RESULT (*snapshot_save_cb_t)(st_SNAPSHOT_CONTEXT *ctx);
typedef en_SNAPSHOT_RESULT (*snapshot_load_cb_t)(st_SNAPSHOT_CONTEXT *ctx);


/**
 * Priorita registrace — určuje pořadí save/load operací
 *
 * Při LOAD je důležité načítat komponenty ve správném pořadí:
 * 1. Nejdřív paměti (RAM, VRAM)
 * 2. Pak CPU (registry)
 * 3. Pak HW jádro (GDG, mzarch)
 * 4. Framebuffer
 * 5. I/O periferie (CTC, PIO, PSG)
 * 6. Zařízení (CMT, FDC, QD, RD, IDE, Unicard)
 * 7. Konfigurace
 */
typedef enum en_SNAPSHOT_PRIORITY {
    SNAPSHOT_PRIORITY_MEMORY = 0,        /* Paměti (RAM, VRAM, MEMEXT RAM/FLASH) */
    SNAPSHOT_PRIORITY_CPU = 10,          /* CPU (Z80) */
    SNAPSHOT_PRIORITY_HW_CORE = 20,      /* Jádrový HW (GDG, mzarch, VRAM Control) */
    SNAPSHOT_PRIORITY_FRAMEBUFFER = 25,   /* Framebuffer (závisí na GDG stavu) */
    SNAPSHOT_PRIORITY_HW_IO = 30,        /* I/O HW (CTC, PIO, PSG) */
    SNAPSHOT_PRIORITY_DEVICE = 40,       /* Zařízení (CMT, FDC, QD, RD, IDE, Unicard) */
    SNAPSHOT_PRIORITY_CONFIG = 50,       /* HW konfigurace relevantní pro emulaci */
} en_SNAPSHOT_PRIORITY;


/**
 * Registrace HW komponenty do snapshot systému
 *
 * @param name Identifikátor komponenty (pro logování)
 * @param priority Pořadí zpracování
 * @param save_cb Callback pro uložení stavu
 * @param load_cb Callback pro načtení stavu
 * @param is_optional true pokud je komponenta volitelná (může chybět v snapshotu)
 * @return 0 při úspěchu, -1 při chybě (plný registr)
 */
int snapshot_register_component(const char *name,
                                en_SNAPSHOT_PRIORITY priority,
                                snapshot_save_cb_t save_cb,
                                snapshot_load_cb_t load_cb,
                                bool is_optional);

/**
 * Vrátí počet zaregistrovaných komponent (pro testy)
 */
int snapshot_mgr_get_component_count(void);

/**
 * @brief Vrátí jméno komponenty na pozici @p index v pořadí zpracování (pro testy).
 *
 * Pořadí odpovídá pořadí volání save/load callbacků: vzestupně podle
 * priority, v rámci stejné priority podle pořadí registrace.
 *
 * @param index Pozice 0 .. snapshot_mgr_get_component_count() - 1.
 * @return Jméno předané do snapshot_register_component() (vlastní ho
 *         registrující handler, typicky řetězcový literál), nebo NULL při
 *         indexu mimo rozsah.
 */
const char *snapshot_mgr_get_component_name(int index);


/* ========================================================================= */
/*               Deklarace registračních funkcí handlerů                     */
/* ========================================================================= */

/* Fáze 3: Handlery jádra */
extern void snap_z80_register(void);
extern void snap_memory_register(void);
extern void snap_gdg_register(void);
extern void snap_mzarch_register(void);
extern void snap_framebuffer_register(void);
extern void snap_vramctrl_register(void);

/* Fáze 4: Handlery I/O */
extern void snap_ctc8253_register(void);
extern void snap_pio8255_register(void);
extern void snap_psg_register(void);
extern void snap_audio_register(void);

/**
 * @brief Registruje komponentu "videorec" (informace o video záznamu pro retake).
 *
 * Volá snapshot_init() hned za snap_audio_register(): stejná priorita
 * SNAPSHOT_PRIORITY_DEVICE, pořadí v rámci priority je pořadí registrace.
 * Komponenta je volitelná (is_optional = true).
 */
extern void snap_videorec_register(void);

/**
 * @brief Oznámí lepidlu video záznamu dokončený úspěšný load snapshotu.
 *
 * Volá snapshot_load_through_io() po načtení všech komponent, těsně před
 * návratem SNAPSHOT_OK. Předá informace přečtené load callbackem komponenty
 * "videorec" (nebo NULL, když je snapshot neobsahoval) do
 * videorec_on_snapshot_loaded() a zapomene je.
 *
 * @par Vlákna Vlákno, které snapshot nahrává: UI vlákno při pozastavené
 *      emulaci, nebo emu vlákno při zpracování dbgapi/MCP příkazu (v paused
 *      smyčce i za běhu emulace mezi snímky - emulace pak v pauze není);
 *      bere zámek lepidla videorec, požadavek se zpracuje na nejbližším
 *      konci snímku (viz videorec_on_snapshot_loaded()).
 */
extern void snap_videorec_after_load(void);

#if HAVE_PIOZ80
extern void snap_pioz80_register(void);
#endif

/* Fáze 5: Handlery zařízení */
extern void snap_cmt_register(void);

/**
 * @brief Režim načtení pásky ze snapshotu pro záznam vstupů.
 *
 * S keep = true load komponenty "cmt" použije vloženou pásku (každý
 * formát, rozšíření cmt_tmz) bez ohledu na cestu uloženou ve snapshotu
 * a soubor z té cesty nikdy neotevírá; bez vložené pásky skončí transport
 * ve STOP jako u snapshotu bez pásky. Obnoví se rychlosti bloků, blok,
 * stav playeru a pozice. Záznam vstupů ho zapíná jen na dobu
 * snapshot_load_from_buffer() svých snapshotů (páska je kopie ze záznamu
 * v dočasném souboru, soubor uživatele se číst nesmí).
 *
 * @param keep true = použít vloženou pásku; false = výchozí chování
 *        (otevřít soubor z cesty ve snapshotu, není-li vložený).
 * @pre Vlákno emulátoru (stejné jako snapshot_load_from_buffer()).
 * @post Platí do dalšího volání; volající ho po načtení vrátí na false.
 */
extern void snap_cmt_set_keep_inserted_tape(bool keep);

/**
 * @brief Vkládá save komponenty "cmt" obraz neuložené pásky do snapshotu?
 *
 * Výchozí true: páska Modified nebo bez cesty se do snapshotu vloží
 * (element tape/image). Záznam vstupů ho na dobu svých snapshotů (start,
 * checkpointy) vypíná: pásku nasazuje sám z kopie v záznamu a obraz by
 * jen zvětšoval každý checkpoint.
 *
 * @param embed true = vkládat (výchozí), false = jen cesta.
 * @pre Vlákno emulátoru. @post Platí do dalšího volání.
 */
extern void snap_cmt_set_embed_tape(bool embed);

struct st_CMT_RECORD_CKPT; /* hw-generic/cmt/cmt.h */

/**
 * @brief Kopie nahrávání, kterou load komponenty "cmt" obnoví (checkpoint záznamu vstupů).
 *
 * Load snapshotu pořízeného během nahrávání jinak nahrávání zruší (STOP,
 * varování). S nastavenou kopií transport zůstane v RECORD a vrátí se
 * buffer hran (cmt_record_ckpt_install()). Záznam vstupů kopii nastaví
 * jen na dobu načtení checkpointu a pak ji vrátí na NULL.
 *
 * @param rc Kopie (zůstává volajícímu, musí platit do konce načtení), nebo NULL.
 * @pre Vlákno emulátoru.
 */
extern void snap_cmt_set_record_restore(const struct st_CMT_RECORD_CKPT *rc);

#ifdef MZTEST_HEADLESS
/**
 * @brief Jen pro testy: mez vloženého obrazu pásky v bajtech (0 = výchozí 16 MB).
 * @param limit Mez.
 */
extern void snap_cmt_test_set_embed_limit(size_t limit);

/**
 * @brief Jen pro testy: další save vloží poškozený obraz pásky.
 * @param mode 0 = normálně, 1 = znak mimo abecedu base64, 2 = platný base64, který není obraz pásky,
 *        3 = platný obraz, ale element modified = false (snapshot z cizího zdroje),
 *        4 = celý devices/cmt.xml bez elementu cmt_state (load ho odmítne jako chybu XML),
 *        5 = devices/cmt.xml se nezapíše vůbec (snapshot bez komponenty CMT).
 */
extern void snap_cmt_test_corrupt_image(int mode);

/**
 * @brief Jen pro testy: varování komponenty "cmt" od posledního snap_cmt_test_clear_warnings().
 * @return Texty varování (bez předpony "Snapshot [cmt]: WARN: ") oddělené "\n"; "" = žádné.
 */
extern const char *snap_cmt_test_warnings(void);

/** @brief Jen pro testy: vyprázdní záznam varování (snap_cmt_test_warnings()). */
extern void snap_cmt_test_clear_warnings(void);

/**
 * @brief Jen pro testy: save element <tape> vynechá (snapshot jako z verze bez něj).
 * @param omit true = vynechat, false = normálně.
 */
extern void snap_cmt_test_omit_tape(bool omit);

/**
 * @brief Jen pro testy: save vynechá příznaky neuloženo bloků (<block_N>/<unsaved>), snapshot jako ze staršího emulátoru.
 * @param omit true = vynechat, false = normálně.
 */
extern void snap_cmt_test_omit_unsaved(bool omit);

#endif

#if CFG_HWEXT_HAVE_FDC
extern void snap_fdc_register(void);
#endif

#if CFG_HWEXT_HAVE_QDISK
extern void snap_qdisk_register(void);
#endif

#if CFG_HWEXT_HAVE_RAMDISK
extern void snap_ramdisk_register(void);
#endif

#if CFG_HWEXT_HAVE_IDE8
extern void snap_ide8_register(void);
#endif

#if 1 /* MEMEXT je vždy k dispozici */
extern void snap_memext_register(void);
#endif

extern void snap_unicard_register(void);

extern void snap_hwconfig_register(void);


#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_MGR_H */
