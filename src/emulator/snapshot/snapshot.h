/**
 * @file snapshot.h
 * @brief Hlavní veřejné API snapshot systému pro MZ-800/700/1500 emulátor
 *
 * Snapshot systém umožňuje uložit a načíst kompletní stav emulátoru
 * do/ze ZIP archivu s příponou .mzs.
 */

#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Verze formátu snapshotu */
#define SNAPSHOT_FORMAT_VERSION  1

/* Maximální velikost archivu (100 MB) */
#define SNAPSHOT_MAX_ARCHIVE_SIZE  (100 * 1024 * 1024)

/* Maximální počet entries v archivu */
#define SNAPSHOT_MAX_ENTRIES  100


/**
 * Chybové kódy snapshot systému
 */
typedef enum en_SNAPSHOT_RESULT {
    SNAPSHOT_OK = 0,
    SNAPSHOT_ERR_IO,              /* Chyba čtení/zápisu souboru */
    SNAPSHOT_ERR_ZIP,             /* Chyba ZIP archivu */
    SNAPSHOT_ERR_XML_PARSE,       /* Chyba parsování XML */
    SNAPSHOT_ERR_XML_WRITE,       /* Chyba zápisu XML */
    SNAPSHOT_ERR_VERSION,         /* Nepodporovaná verze snapshotu */
    SNAPSHOT_ERR_ARCHITECTURE,    /* Nekompatibilní architektura */
    SNAPSHOT_ERR_MISSING_FILE,    /* V archivu chybí povinný soubor */
    SNAPSHOT_ERR_CORRUPTED,       /* Data jsou poškozená (checksum nesedí) */
    SNAPSHOT_ERR_COMPONENT,       /* Chyba při save/load komponenty */
    SNAPSHOT_ERR_ALLOC,           /* Chyba alokace paměti */
    SNAPSHOT_ERR_EXTERNAL_REF,    /* Chybí referenční externí soubor */
    SNAPSHOT_ERR_NOT_PAUSED,      /* Emulátor není v pauze */
    SNAPSHOT_ERR_REPLAY_ACTIVE,   /* Načtení odmítnuto: běží záznam nebo přehrávání vstupů (replay) */
} en_SNAPSHOT_RESULT;


/**
 * Nastavení pro snapshot
 */
/**
 * Mód obnovení emulace po load snapshotu
 * 0 = vždy run, 1 = vždy pause, 2 = předchozí stav (default)
 */
#define SNAPSHOT_RESUME_ALWAYS_RUN   0
#define SNAPSHOT_RESUME_ALWAYS_PAUSE 1
#define SNAPSHOT_RESUME_PREVIOUS     2

typedef struct st_SNAPSHOT_SETTINGS {
    bool include_ramdisk;         /* Uložit obsah volatile ramdisku (default: false) */
    bool include_memext;          /* Uložit obsah MEMEXT RAM/FLASH (default: true) */
    int compression_level;        /* ZIP kompresní úroveň 0-9 (default: 6) */
    char default_directory[1024]; /* Implicitní adresář pro snapshoty; prázdný = pracovní adresář (--work-dir), relativní vůči němu (Quick Save, Quick Load) */
    char quicksave_filename[256]; /* Název souboru pro quicksave (default: "quicksave") */
    int quicksave_mode;           /* 0=Basic, 1=Incremental, 2=Rotational */
    int quicksave_max_slots;      /* Max. počet slotů pro rotational mód (default: 5) */
    int load_resume_mode;         /* Chování po Load: 0=run, 1=pause, 2=previous (default: 2) */
    int quickload_resume_mode;    /* Chování po Quick Load: 0=run, 1=pause, 2=previous (default: 2) */
} st_SNAPSHOT_SETTINGS;

extern st_SNAPSHOT_SETTINGS g_snapshot_settings;


/**
 * Metadata snapshotu (pro zobrazení v UI)
 */
typedef struct st_SNAPSHOT_METADATA {
    char description[256];        /* Popis zadaný uživatelem */
    char emulator_version[64];    /* Verze emulátoru */
    char created[32];             /* Datum a čas vytvoření */
    int architecture;             /* MZARCH hodnota: 700, 800, 1500 */
    uint32_t format_version;      /* Verze formátu snapshotu */
    char checksum[65];            /* SHA-256 hash (64 hex znaků + null) */
} st_SNAPSHOT_METADATA;


/**
 * Inicializace snapshot systému — volat jednou při startu emulátoru
 */
void snapshot_init(void);

/**
 * Ukončení snapshot systému — volat při ukončení emulátoru
 */
void snapshot_exit(void);

/**
 * Uložení snapshotu do souboru
 *
 * @param filepath Cesta k výstupnímu souboru (.mzs)
 * @param description Popis snapshotu (může být NULL)
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_save(const char *filepath,
                                const char *description);

/**
 * Načtení snapshotu ze souboru
 *
 * @param filepath Cesta k souboru (.mzs)
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_load(const char *filepath);

/**
 * Uložení snapshotu do paměťového bufferu
 *
 * Funkčně ekvivalentní @ref snapshot_save, ale výstupem je nově alokovaný
 * buffer s .mzs daty místo zápisu na disk. Použití: MCP server (inline
 * payload pro AI klienta), back-stepping snapshot ring v paměti,
 * headless CI bez disk I/O.
 *
 * @param description Popis snapshotu (může být NULL)
 * @param out_data Výstupní ukazatel na alokovaný buffer s .mzs daty.
 *                 Volající uvolní přes g_free(). Při chybě bude
 *                 *out_data = NULL.
 * @param out_size Výstupní velikost dat v bajtech. Při chybě bude
 *                 *out_size = 0.
 * @return SNAPSHOT_OK při úspěchu; jinak některý z @ref en_SNAPSHOT_RESULT
 *         (mimo jiné SNAPSHOT_ERR_NOT_PAUSED pokud emulátor nestojí v pauze
 *         ani v snapshot safe-pointu).
 *
 * @note Vyžaduje pauzu nebo `g_emulator.snapshot_safepoint` (emu vlákno mezi
 *       instrukcemi, stejně jako @ref snapshot_save) - checkpointy záznamu
 *       vstupů ho nastaví na dobu volání v bodě hranice snímku.
 * @note Vyprodukovaný buffer je bit-identický s .mzs souborem, který by
 *       vznikl voláním @ref snapshot_save - lze jej následně načíst přes
 *       @ref snapshot_load (po zapsání na disk) i přes
 *       @ref snapshot_load_from_buffer.
 */
en_SNAPSHOT_RESULT snapshot_save_to_buffer(const char *description,
                                           uint8_t **out_data,
                                           size_t *out_size);

/**
 * @brief SHA-256 stavu vybraných komponent snapshotu (bez zápisu archivu).
 *
 * Zavolá save handlery komponent ze seznamu @p names (v pořadí registru
 * podle priority) nad handlem v hashovacím režimu
 * (@ref snapshot_io_open_hash) s `for_hash` v kontextu: handlery vynechají
 * pole, která nejsou stavem stroje (plánovaná událost hlavní smyčky,
 * průběh vykreslení obrazu). Komponenta, která v registru není (na
 * platformě chybí), se přeskočí. Stejný stav komponent dá vždy stejný hash,
 * nezávisle na čase, kompresi a tom, zda emulace právě stojí v pauze.
 *
 * @param names    Jména komponent (`"ctc8253"`, `"pio8255"`, ...).
 * @param count    Počet jmen.
 * @param out_hex  Výstup: 64 hex znaků a nula.
 * @param out_size Velikost @p out_hex (aspoň 65).
 * @return SNAPSHOT_OK; chyba handleru nebo SNAPSHOT_ERR_IO (pak je
 *         @p out_hex prázdný).
 *
 * @pre Volá emu vlákno (nebo emulace stojí): handlery čtou stav bez zámku.
 * @note Na rozdíl od @ref snapshot_save nekontroluje pauzu; volá ji replay
 *       v bodě hranice snímku.
 */
en_SNAPSHOT_RESULT snapshot_hash_components(const char *const *names, size_t count,
                                            char *out_hex, size_t out_size);

/**
 * Načtení snapshotu z paměťového bufferu
 *
 * Funkčně ekvivalentní @ref snapshot_load, ale vstupem je in-memory ZIP
 * archiv místo souboru.
 *
 * @param data Ukazatel na .mzs data v paměti (musí zůstat platné po
 *             celou dobu volání - po návratu lze uvolnit)
 * @param size Velikost dat v bajtech
 * @return SNAPSHOT_OK při úspěchu; jinak některý z @ref en_SNAPSHOT_RESULT.
 *
 * @note Vyžaduje, aby byl emulátor v pauze.
 * @note Akceptuje data vyprodukovaná jak @ref snapshot_save_to_buffer,
 *       tak @ref snapshot_save (= identický .mzs formát).
 */
en_SNAPSHOT_RESULT snapshot_load_from_buffer(const uint8_t *data,
                                             size_t size);

/**
 * Načtení metadat snapshotu (pro náhled v selektoru, bez načtení stavu)
 *
 * @param filepath Cesta k souboru (.mzs)
 * @param metadata Výstupní struktura pro metadata
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_read_metadata(const char *filepath,
                                         st_SNAPSHOT_METADATA *metadata);

/**
 * Extrakce screenshotu ze snapshotu (pro náhled v selektoru)
 *
 * @param filepath Cesta k souboru (.mzs)
 * @param png_data Výstupní ukazatel na PNG data (volající uvolní přes g_free)
 * @param png_size Výstupní velikost PNG dat
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_extract_screenshot(const char *filepath,
                                              uint8_t **png_data,
                                              size_t *png_size);

/**
 * Textový popis chybového kódu
 */
const char *snapshot_result_to_string(en_SNAPSHOT_RESULT result);

/**
 * Přepočítá SHA-256 checksum snapshotu (pro ruční editace)
 *
 * Otevře existující .mzs, přepočítá checksum a aktualizuje manifest.xml.
 */
en_SNAPSHOT_RESULT snapshot_recalculate_checksum(const char *filepath);


/**
 * Konfigurace — načtení/uložení nastavení z/do INI souboru
 */
void snapshot_config_load(void);
void snapshot_config_save(void);


#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_H */
