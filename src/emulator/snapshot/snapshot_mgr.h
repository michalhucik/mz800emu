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
