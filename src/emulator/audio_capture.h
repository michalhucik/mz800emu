/**
 * @file   audio_capture.h
 * @brief  Zachycení zvuku do složky pro hledání chyb zvuku (MCP audio_capture_*).
 *
 * Po startu zapisuje do zvolené složky v emulovaném čase:
 *
 * | Soubor           | Obsah |
 * |------------------|-------|
 * | `events.csv`     | každá změna výstupní úrovně zvukového kanálu (CTC0 = reproduktor, kanály PSG) |
 * | `psg_writes.csv` | každý zápis do PSG s dekódováním (kanál, registr, hodnota po zápisu) |
 * | `reference.wav`  | zvuk z rendereru videa (videorec_audio, přesné převzorkování v emulovaném čase) |
 * | `sdl.wav`        | zvuk tak, jak ho vyrobila skutečná SDL cesta (iface_audio_wait_for_data()) |
 * | `capture.json`   | metadata (platforma, takt, kanály, počty, důvod konce); zapisuje se při konci |
 *
 * Časy v CSV jsou v taktech GDG od začátku zachycení (`ticks`) a v µs
 * (`time_us`); `frame` je pořadí zvukového snímku (20 ms PAL / 16,7 ms NTSC)
 * od začátku zachycení.
 *
 * Začátek: audio_capture_start() zachycení jen naplánuje, skutečně začne na
 * konci zvukového snímku (audio_capture_on_frame()), kdy jsou dopočítané
 * všechny kanály PSG - stejně jako začátek video záznamu. Se sdl.wav až po
 * rozjezdu SDL cesty (6 snímků, bloky se zahazují), bez něj na konci
 * nejbližšího snímku. Úrovně kanálů na začátku se berou z živého audio logu;
 * renderer reference.wav se zahřeje 100 ms se stejnými úrovněmi a stav
 * střídavých vazeb (CTC0, celý výstup) převezme od emulátoru, takže začíná
 * stejně jako SDL cesta.
 *
 * `reference.wav` a `sdl.wav` mají stejný formát (44100 Hz, 16 bit, stereo)
 * a lze je přímo porovnat. SDL cesta je ale jiný zdroj: zpracovává hotové
 * zvukové snímky se zpožděním (bez zařízení přesně o jeden snímek, se
 * zařízením podle jeho bufferu) a při MAX SPEED může snímky vynechat
 * (status: `frames` vs `sdl_blocks`).
 * V režimu bez zvukového zařízení (headless) si zachycení SDL bloky vyrábí
 * samo na každý snímek (iface_audio_20ms_sync()).
 *
 * Paměť (parametr `memory`, výchozí jen při startu přes MCP): vedle souborů
 * (nebo místo nich) drží zachycení ringy v paměti, které jde číst za běhu
 * i po konci až do dalšího startu (audio_capture_stream_copy()):
 *
 * | Proud (en_AUDIO_CAPTURE_STREAM) | Záznam | Plní se |
 * |-------------------------------|--------|---------|
 * | `psg_writes`    | st_AUDIO_CAPTURE_PSG_REC (24 B), jako řádek psg_writes.csv | zápis do PSG |
 * | `levels`        | st_AUDIO_CAPTURE_LEVEL_REC (16 B), jako řádek events.csv | změna úrovně kanálu |
 * | `frames`        | st_AUDIO_CAPTURE_FRAME_REC (192 B), souhrn zvukového snímku | konec snímku |
 * | `pcm_reference` | stereo pár int16 (4 B), stejná data jako reference.wav | konec snímku |
 * | `pcm_sdl`       | stereo pár int16 (4 B), stejná data jako sdl.wav | blok SDL cesty |
 *
 * Každý proud má pořadová čísla záznamů `seq` od 0 (začátek zachycení).
 * Ring po naplnění přepisuje nejstarší záznamy (first_seq roste). Číslo
 * `segment` se zvýší při každém startu; ringy předchozího zachycení start
 * uvolní. S pamětí běží renderer reference i bez reference.wav (souhrn
 * snímku z něj bere RMS a špičku) a s `pcm_sdl` i SDL cesta bez sdl.wav.
 * Zachycení jen do paměti (žádný soubor) nevytváří složku ani capture.json.
 *
 * Konec: audio_capture_stop(), dosažení `frames`, pojistka
 * AUDIO_CAPTURE_MAX_FRAMES_SAFETY, skok času zpět (reset emulátoru, načtení
 * snapshotu), chyba zápisu, nebo ukončení emulátoru (audio_capture_exit()).
 *
 * Ovládání: MCP (dbgapi DBGAPI_CMD_AUDIO_CAPTURE), GUI (Tools -> Audio
 * Capture, Alt+Q) a příkazová řádka (--audio-capture, --audio-capture-frames;
 * spouští mzarch po bootu). GUI a příkazová řádka berou výběr souborů
 * z g_audio_capture_settings (INI `[AUDIO_CAPTURE]`); bez zadané složky se
 * zachycení zakládá ve výstupní složce zachycení (výchozí `audio_captures`
 * vedle emulátoru).
 *
 * @par Vlákna
 * Všechny funkce kromě audio_capture_config_init() a
 * audio_capture_params_from_settings() lze volat z libovolného vlákna; stav
 * chrání jeden zámek modulu. Háčky audio_capture_on_*() volá emu vlákno,
 * audio_capture_sdl_output() vlákno SDL zvuku (GUI) nebo emu vlákno
 * (headless). g_audio_capture_settings patří UI vláknu (jako nastavení
 * screenshotu).
 *
 * @par Licence: GPLv3
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximální délka cesty ke složce. */
#define AUDIO_CAPTURE_PATH_MAX 1024
/** @brief Velikost bufferů s textem (chyba, důvod konce). */
#define AUDIO_CAPTURE_TEXT_MAX 256
/** @brief Vzorkovací frekvence obou WAV [Hz] (stejná jako výstup SDL cesty). */
#define AUDIO_CAPTURE_WAV_RATE 44100u
/** @brief Pojistka délky zachycení bez `frames` (1 hodina při 60 snímcích/s). */
#define AUDIO_CAPTURE_MAX_FRAMES_SAFETY 216000u
/** @brief Nejvýš zvukových kanálů v souhrnu snímku (CTC0 + 2x 4 kanály PSG). */
#define AUDIO_CAPTURE_MAX_CHANNELS 9u
/** @brief Výchozí kapacita ringu změn úrovní [záznamy] (16 MiB). */
#define AUDIO_CAPTURE_RING_LEVELS_DEFAULT (1024u * 1024u)
/** @brief Výchozí kapacita ringu zápisů do PSG [záznamy] (6 MiB). */
#define AUDIO_CAPTURE_RING_PSG_DEFAULT (256u * 1024u)
/** @brief Výchozí délka ringu souhrnů snímků [minuty]. */
#define AUDIO_CAPTURE_MEMORY_MINUTES_DEFAULT 10u
/** @brief Výchozí délka PCM ringů [s] (10,6 MB na zdroj). */
#define AUDIO_CAPTURE_PCM_SECONDS_DEFAULT 60u

/** @brief Proud paměti zachycení (pořadí = indexy v st_AUDIO_CAPTURE_STATUS::streams). */
typedef enum en_AUDIO_CAPTURE_STREAM {
    AUDIO_CAPTURE_STREAM_PSG_WRITES = 0, /**< Zápisy do PSG (st_AUDIO_CAPTURE_PSG_REC). */
    AUDIO_CAPTURE_STREAM_LEVELS,         /**< Změny úrovní kanálů (st_AUDIO_CAPTURE_LEVEL_REC). */
    AUDIO_CAPTURE_STREAM_FRAMES,         /**< Souhrny zvukových snímků (st_AUDIO_CAPTURE_FRAME_REC). */
    AUDIO_CAPTURE_STREAM_PCM_REFERENCE,  /**< PCM reference (stereo pár int16). */
    AUDIO_CAPTURE_STREAM_PCM_SDL,        /**< PCM SDL cesty (stereo pár int16). */
    AUDIO_CAPTURE_STREAM_COUNT           /**< Počet proudů (není platná hodnota). */
} en_AUDIO_CAPTURE_STREAM;

/**
 * @brief Záznam změny úrovně kanálu (proud `levels`, jako řádek events.csv).
 *
 * Paměť hostitele (little endian na podporovaných hostitelích); stejné
 * bajty vrací čtení `raw_b64`.
 */
typedef struct st_AUDIO_CAPTURE_LEVEL_REC {
    uint64_t t;           /**< Absolutní čas změny [takty GDG] (origin_ticks = čas 0 v CSV). */
    uint32_t frame;       /**< Zvukový snímek od začátku zachycení. */
    uint8_t src;          /**< Kanál 0 = ctc0, 1..4 = psg0_0..3, 5..8 = psg1_0..3. */
    uint8_t level;        /**< Hodnota zvukového logu: 0..15 plná výchylka stupně hlasitosti, 16..30 výchylka 0,49 stupně `level - 15` (tón PSG s děličem 001h), 31 výchylka 0,64 stupně 15 (CTC0 v režimu 3 s děličem 2). */
    uint8_t reserved[2];  /**< 0. */
} st_AUDIO_CAPTURE_LEVEL_REC;

/** @brief Registr zapsaný do PSG (dekódováno ze stavu PSG po zápisu, jako psg_writes.csv). */
typedef enum en_AUDIO_CAPTURE_PSG_REG {
    AUDIO_CAPTURE_PSG_REG_TONE_LO = 0, /**< Latch bajt tónu: value = dolní 4 bity děliče (dělič se změní až datovým bajtem). */
    AUDIO_CAPTURE_PSG_REG_TONE,        /**< Dělič tónu po zápisu (10 bitů). */
    AUDIO_CAPTURE_PSG_REG_VOLUME,      /**< Útlum 0..15 (15 = vypnuto). */
    AUDIO_CAPTURE_PSG_REG_NOISE        /**< Šum: (typ << 2) | rychlost. */
} en_AUDIO_CAPTURE_PSG_REG;

/** @brief Záznam zápisu do PSG (proud `psg_writes`). */
typedef struct st_AUDIO_CAPTURE_PSG_REC {
    uint64_t t;           /**< Absolutní čas zápisu [takty GDG]. */
    uint32_t frame;       /**< Zvukový snímek od začátku zachycení. */
    uint16_t value;       /**< Hodnota registru po zápisu (viz en_AUDIO_CAPTURE_PSG_REG). */
    uint8_t psg;          /**< Cílové PSG: bit 0 = PSG0, bit 1 = PSG1. */
    uint8_t byte;         /**< Zapsaný bajt. */
    uint8_t channel;      /**< Kanál 0..3 (3 = šum). */
    uint8_t reg;          /**< en_AUDIO_CAPTURE_PSG_REG. */
    uint8_t reserved[6];  /**< 0. */
} st_AUDIO_CAPTURE_PSG_REC;

/** @brief Souhrn jednoho kanálu za zvukový snímek. */
typedef struct st_AUDIO_CAPTURE_FRAME_CH {
    uint32_t edges;       /**< Změny úrovně ve snímku (jako řádky events.csv s tímto `frame`). */
    uint32_t on_ticks;    /**< Takty GDG s úrovní > 0. */
    uint32_t energy;      /**< Součet úroveň^2 x takty (RMS úrovně = sqrt(energy / len_ticks)); úroveň = stupeň hlasitosti, u poloviční výchylky (tón PSG s děličem 001h) stupeň^2 x 6 / 25, u hodnoty 31 (CTC0 v režimu 3 s děličem 2) 15^2 x 41 / 100. */
    uint8_t max_level;    /**< Největší stupeň hlasitosti 0..15 ve snímku (u poloviční výchylky stupeň bez snížení, u hodnoty 31 stupeň 15). */
    uint8_t end_level;    /**< Stupeň hlasitosti 0..15 na konci snímku (u poloviční výchylky stupeň bez snížení, u hodnoty 31 stupeň 15). */
    uint8_t reserved[2];  /**< 0. */
} st_AUDIO_CAPTURE_FRAME_CH;

/** @brief Bit st_AUDIO_CAPTURE_FRAME_REC::flags: ref_rms / ref_peak platí. */
#define AUDIO_CAPTURE_FRAME_FLAG_REF 0x01u
/** @brief Bit st_AUDIO_CAPTURE_FRAME_REC::flags: SDL cesta se zachytávala (sdl_* platí). */
#define AUDIO_CAPTURE_FRAME_FLAG_SDL 0x02u

/**
 * @brief Souhrn zvukového snímku (proud `frames`).
 *
 * Kanály a změny se přiřazují snímku, během kterého je zvuková cesta
 * doručila (stejně jako sloupec `frame` v CSV); změny CTC0 doručené po
 * horizontu snímku (nejvýš jeden krok PSG) se tak počítají do snímku před
 * ním. Součty on_ticks a energy přes všechny snímky jsou přesné.
 * Úrovně SDL cesty jsou z bloků, které přišly od konce předchozího snímku
 * (SDL cesta je o snímek a víc zpožděná).
 *
 * @invariant ch[i] pro i >= počet kanálů platformy je nulový.
 */
typedef struct st_AUDIO_CAPTURE_FRAME_REC {
    uint64_t t;           /**< Absolutní čas začátku snímku [takty GDG] (horizont předchozího). */
    uint32_t frame;       /**< Snímek od začátku zachycení (= seq). */
    uint32_t len_ticks;   /**< Délka snímku [takty GDG]. */
    st_AUDIO_CAPTURE_FRAME_CH ch[AUDIO_CAPTURE_MAX_CHANNELS]; /**< Kanály v pořadí audio logu. */
    uint16_t ref_rms[2];  /**< RMS reference L, R (0..32767). */
    uint16_t ref_peak[2]; /**< Špička reference L, R (0..32768). */
    uint16_t sdl_rms[2];  /**< RMS bloků SDL cesty L, R. */
    uint16_t sdl_peak[2]; /**< Špička bloků SDL cesty L, R. */
    uint32_t ref_samples; /**< Vzorky reference vyrenderované za snímek. */
    uint32_t sdl_samples; /**< Vzorky bloků SDL cesty od předchozího snímku. */
    uint16_t sdl_blocks;  /**< Bloky SDL cesty od předchozího snímku. */
    uint8_t flags;        /**< AUDIO_CAPTURE_FRAME_FLAG_*. */
    uint8_t reserved[5];  /**< 0. */
} st_AUDIO_CAPTURE_FRAME_REC;

G_STATIC_ASSERT(sizeof(st_AUDIO_CAPTURE_LEVEL_REC) == 16);
G_STATIC_ASSERT(sizeof(st_AUDIO_CAPTURE_PSG_REC) == 24);
G_STATIC_ASSERT(sizeof(st_AUDIO_CAPTURE_FRAME_REC) == 192);

/** @brief Parametry zachycení. */
typedef struct st_AUDIO_CAPTURE_PARAMS {
    char dir[AUDIO_CAPTURE_PATH_MAX]; /**< Složka; "" = nová podsložka `<platforma>_audio_RRRRMMDD_HHMMSS` výstupní složky zachycení (audio_capture_resolve_output_dir()). */
    uint64_t frames;                  /**< Po tolika zvukových snímcích automatický stop (0 = do stop, nejvýš pojistka). */
    bool events;                      /**< Zapisovat events.csv. */
    bool psg_writes;                  /**< Zapisovat psg_writes.csv. */
    bool reference_wav;               /**< Zapisovat reference.wav. */
    bool sdl_wav;                     /**< Zapisovat sdl.wav. */
    bool quit_after_stop;             /**< Po dosažení `frames` ukončit emulátor (CLI v režimu --headless). */
    bool memory;                      /**< Držet ringy v paměti (proudy psg_writes, levels, frames). */
    bool pcm_reference;               /**< S `memory`: PCM ring reference. */
    bool pcm_sdl;                     /**< S `memory`: PCM ring SDL cesty (zapne SDL cestu i bez sdl.wav). */
    uint32_t ring_levels;             /**< Kapacita ringu levels [záznamy]; 0 = AUDIO_CAPTURE_RING_LEVELS_DEFAULT. */
    uint32_t ring_psg_writes;         /**< Kapacita ringu psg_writes [záznamy]; 0 = AUDIO_CAPTURE_RING_PSG_DEFAULT. */
    uint32_t ring_frames;             /**< Kapacita ringu frames [snímky]; 0 = AUDIO_CAPTURE_MEMORY_MINUTES_DEFAULT minut. */
    uint32_t ring_pcm;                /**< Kapacita PCM ringů [stereo páry]; 0 = AUDIO_CAPTURE_PCM_SECONDS_DEFAULT s. */
} st_AUDIO_CAPTURE_PARAMS;

/**
 * @brief Výběr souborů pro GUI a příkazovou řádku (INI `[AUDIO_CAPTURE]`).
 *
 * MCP si soubory volí parametry a nastavení nepoužívá.
 */
typedef struct st_AUDIO_CAPTURE_SETTINGS {
    char output_dir[AUDIO_CAPTURE_PATH_MAX]; /**< INI `output_dir`: výstupní složka; "" = `audio_captures` vedle emulátoru. */
    bool events;         /**< INI `events`: zapisovat events.csv (výchozí 1). */
    bool psg_writes;     /**< INI `psg_writes`: zapisovat psg_writes.csv (výchozí 1). */
    bool reference_wav;  /**< INI `reference_wav`: zapisovat reference.wav (výchozí 1). */
    bool sdl_wav;        /**< INI `sdl_wav`: zapisovat sdl.wav (výchozí 1). */
} st_AUDIO_CAPTURE_SETTINGS;

/**
 * @brief Aktuální nastavení (UI vlákno; ukládá se do INI při ukončení).
 *
 * Bez zámku: output_dir čte i start z MCP v emu vlákně (audio_capture_resolve_output_dir()).
 * Zápis dělá jen dialog nastavení v UI vlákně; souběh se startem z MCP ve
 * stejném okamžiku je prakticky nedosažitelný (stejně jako u g_videorec_settings).
 */
extern st_AUDIO_CAPTURE_SETTINGS g_audio_capture_settings;

/** @brief Stav jednoho proudu paměti zachycení. */
typedef struct st_AUDIO_CAPTURE_STREAM_INFO {
    bool present;         /**< Proud se drží (memory, u PCM navíc pcm_reference / pcm_sdl). */
    size_t record_size;   /**< Velikost záznamu [B]. */
    uint64_t capacity;    /**< Kapacita ringu [záznamy]. */
    uint64_t first_seq;   /**< Nejstarší držený záznam (= přepsané záznamy). */
    uint64_t next_seq;    /**< Pořadové číslo příštího záznamu (= všechny záznamy od začátku). */
} st_AUDIO_CAPTURE_STREAM_INFO;

/** @brief Stav zachycení (běžícího nebo posledního). */
typedef struct st_AUDIO_CAPTURE_STATUS {
    bool pending;                /**< Start naplánovaný, čeká na konec zvukového snímku. */
    bool active;                 /**< Zachytává se. */
    uint64_t frames;             /**< Zachycené zvukové snímky. */
    uint64_t events;             /**< Řádky events.csv. */
    uint64_t psg_writes;         /**< Řádky psg_writes.csv. */
    uint64_t reference_samples;  /**< Vzorky (stereo páry) v reference.wav. */
    uint64_t sdl_blocks;         /**< Bloky SDL cesty zapsané do sdl.wav (ideálně = frames). */
    uint64_t sdl_samples;        /**< Vzorky (stereo páry) v sdl.wav. */
    char dir[AUDIO_CAPTURE_PATH_MAX];          /**< Složka ("" = dosud žádná). */
    char stop_reason[AUDIO_CAPTURE_TEXT_MAX];  /**< Anglicky: proč zachycení skončilo ("" = běží / nikdy). */
    char last_error[AUDIO_CAPTURE_TEXT_MAX];   /**< Anglicky: poslední chyba ("" = žádná). */
    unsigned segment;            /**< Pořadí zachycení od spuštění emulátoru (0 = dosud žádné). */
    bool began;                  /**< Zachycení opravdu začalo (origin_ticks platí). */
    uint64_t origin_ticks;       /**< Absolutní čas začátku [takty GDG] (čas 0 v CSV). */
    unsigned channels;           /**< Počet zvukových kanálů platformy (AUDIO_SRC_CHANNELS_COUNT). */
    bool stereo;                 /**< Druhé PSG je zapnuté (MZ-800 / MZ-1500). */
    bool memory;                 /**< Paměť zachycení se drží. */
    uint64_t memory_bytes;       /**< Alokovaná paměť ringů [B]. */
    st_AUDIO_CAPTURE_STREAM_INFO streams[AUDIO_CAPTURE_STREAM_COUNT]; /**< Proudy paměti. */
} st_AUDIO_CAPTURE_STATUS;

/**
 * @brief Kopie úseku proudu paměti zachycení (audio_capture_stream_copy()).
 *
 * @par Vlastnictví
 * `data` alokuje audio_capture_stream_copy() (g_malloc), uvolní
 * audio_capture_stream_copy_clear().
 */
typedef struct st_AUDIO_CAPTURE_STREAM_COPY {
    unsigned segment;     /**< Pořadí zachycení (0 = dosud žádné). */
    bool live;            /**< Zachycení je naplánované nebo běží. */
    bool present;         /**< Proud se v tomto zachycení drží. */
    size_t record_size;   /**< Velikost záznamu [B] (0 = proud se nedrží). */
    uint64_t first_seq;   /**< Nejstarší držený záznam. */
    uint64_t next_seq;    /**< Příští záznam. */
    uint64_t origin_ticks; /**< Absolutní čas začátku zachycení [takty GDG] (0 = nezačalo). */
    uint8_t *data;        /**< Zkopírované záznamy (NULL = žádné). */
    uint64_t data_seq;    /**< seq prvního zkopírovaného záznamu. */
    uint64_t data_count;  /**< Počet zkopírovaných záznamů. */
} st_AUDIO_CAPTURE_STREAM_COPY;

/** @brief Stav kanálu PSG na začátku zachycení (pro detektor not). */
typedef struct st_AUDIO_CAPTURE_PSG_INITIAL {
    bool valid;                 /**< Zachycení začalo na platformě s PSG. */
    uint16_t divider[2][4];     /**< Dělič tónu [PSG][kanál]. */
    uint8_t attn[2][4];         /**< Útlum 0..15 [PSG][kanál]. */
    bool noise[2][4];           /**< Kanál je šumový [PSG][kanál]. */
} st_AUDIO_CAPTURE_PSG_INITIAL;

/**
 * @brief Příznak pro rychlé testy v háčcích (0 = nic se nezachytává ani nečeká).
 *
 * Bit 0 = start naplánovaný nebo zachycení běží (háčky emu vlákna),
 * bit 1 = zapisuje se sdl.wav (háček SDL cesty). Čte se atomicky.
 */
extern gint g_audio_capture_flags;

/** @brief Bit g_audio_capture_flags: zachycení naplánované nebo běží. */
#define AUDIO_CAPTURE_FLAG_ON  0x01
/** @brief Bit g_audio_capture_flags: zapisuje se sdl.wav. */
#define AUDIO_CAPTURE_FLAG_SDL 0x02

/** @brief Výchozí parametry: všechny soubory, bez paměti a limitu snímků, výchozí složka. */
static inline void audio_capture_params_init(st_AUDIO_CAPTURE_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    p->events = true;
    p->psg_writes = true;
    p->reference_wav = true;
    p->sdl_wav = true;
}

/** @brief Je potřeba volat háčky emu vlákna (start naplánovaný nebo běží)? */
static inline bool audio_capture_wanted(void)
{
    return (g_atomic_int_get(&g_audio_capture_flags) & AUDIO_CAPTURE_FLAG_ON) != 0;
}

/** @brief Zapisuje se sdl.wav (rozhoduje i o výrobě SDL bloků bez zařízení)? */
static inline bool audio_capture_wants_sdl(void)
{
    return (g_atomic_int_get(&g_audio_capture_flags) & AUDIO_CAPTURE_FLAG_SDL) != 0;
}

/**
 * @brief Naplánuje zachycení: vytvoří složku a soubory, začne na konci zvukového snímku.
 *
 * @param p        Parametry.
 * @param err      Buffer na anglický text chyby, nebo NULL.
 * @param err_size Velikost `err`.
 * Ringy předchozího zachycení uvolní a s `memory` alokuje nové; segment
 * se zvýší i bez paměti.
 *
 * @return false = zachycení už běží, žádný výstup (soubor ani paměť) není
 *         zvolený, nejde vytvořit složka či soubor, nebo nejde alokovat
 *         paměť ringů (text v `err`); nic se nespustí.
 * @post Při úspěchu status.pending == true; status.dir je složka, nebo ""
 *       u zachycení jen do paměti.
 */
bool audio_capture_start(const st_AUDIO_CAPTURE_PARAMS *p, char *err, size_t err_size);

/**
 * @brief Ukončí zachycení (i naplánované), dopíše a zavře soubory a capture.json.
 * @return false = nic neběželo ani nečekalo.
 */
bool audio_capture_stop(void);

/**
 * @brief Stav běžícího nebo posledního zachycení.
 * @param out Výstup.
 */
void audio_capture_get_status(st_AUDIO_CAPTURE_STATUS *out);

/**
 * @brief Háček: změna výstupní úrovně zvukového kanálu (volá audio_changed()).
 * @param src_id Index kanálu (0 = CTC0, 1..4 = PSG0, 5..8 = PSG1).
 * @param value  Nová hodnota zvukového logu 0..31 (16..30 = výchylka 0,49 stupně value - 15,
 *               31 = výchylka 0,64 stupně 15 u CTC0 v režimu 3 s děličem 2).
 * @param ticks  Absolutní čas změny v taktech GDG.
 * @pre Emu vlákno, audio_capture_wanted().
 */
void audio_capture_on_change(unsigned src_id, uint8_t value, uint64_t ticks);

/**
 * @brief Háček: zápis do PSG (volá psg_write_byte() po zápisu do registrů).
 * @param ticks Absolutní čas zápisu v taktech GDG.
 * @param mask  Cílové PSG: bit 0 = PSG0 (mono), bit 1 = PSG1.
 * @param value Zapsaný bajt.
 * @pre Emu vlákno, audio_capture_wanted().
 */
void audio_capture_on_psg_write(uint64_t ticks, unsigned mask, uint8_t value);

/**
 * @brief Háček: konec zvukového snímku (volá audiolog_finish_20ms_frame()).
 *
 * Naplánované zachycení zde začne; běžící vyrenderuje reference.wav do
 * `horizon_ticks` a zkontroluje konec (frames, pojistka, skok času).
 * Konec po `frames` s quit_after_stop ukončí emulátor (emulator_quit(),
 * nevrátí se), stejně jako video záznam s --record-frames.
 *
 * @param horizon_ticks Čas, do kterého jsou doručené všechny změny kanálů.
 * @pre Emu vlákno, audio_capture_wanted(); g_audio.log je log končícího snímku.
 */
void audio_capture_on_frame(uint64_t horizon_ticks);

/**
 * @brief Háček: blok výstupu SDL cesty (volá iface_audio_wait_for_data()).
 * @param stereo Prokládané L,R vzorky v rozsahu -1..1.
 * @param frames Počet stereo párů.
 * @note Libovolné vlákno; bez běžícího zachycení se nic nestane.
 */
void audio_capture_sdl_output(const float *stereo, size_t frames);

/**
 * @brief Ukončí zachycení při ukončení emulátoru (soubory se dopíší) a uvolní ringy.
 * @note Libovolné vlákno (bere zámek modulu).
 */
void audio_capture_exit(void);

/**
 * @brief Zkopíruje metadata a úsek proudu paměti zachycení.
 *
 * Pod zámkem modulu (ten drží i emu vlákno a vlákno SDL zvuku) zkopíruje
 * záznamy [max(from_seq, first_seq), ...) nejvýš `count` záznamů a nejvýš
 * `max_bytes` bajtů; volající proto drží `max_bytes` malé (zámek blokuje
 * zvukovou cestu po dobu kopie).
 *
 * @param stream    Proud.
 * @param from_seq  První požadovaný záznam.
 * @param count     Nejvýš záznamů (0 = jen metadata).
 * @param max_bytes Nejvýš bajtů kopie.
 * @param out       Výstup (uvolnit audio_capture_stream_copy_clear()).
 * @return false = nedostatek paměti pro kopii (out bez dat, metadata platí).
 * @note Libovolné vlákno.
 */
bool audio_capture_stream_copy(en_AUDIO_CAPTURE_STREAM stream, uint64_t from_seq,
                               uint64_t count, size_t max_bytes,
                               st_AUDIO_CAPTURE_STREAM_COPY *out);

/**
 * @brief Uvolní data kopie a vynuluje ji.
 * @param c Kopie (smí být už prázdná).
 */
void audio_capture_stream_copy_clear(st_AUDIO_CAPTURE_STREAM_COPY *c);

/**
 * @brief Stav kanálů PSG na začátku posledního zachycení.
 * @param out Výstup (valid = false, pokud nezačalo nebo platforma nemá PSG).
 * @note Libovolné vlákno.
 */
void audio_capture_get_psg_initial(st_AUDIO_CAPTURE_PSG_INITIAL *out);

/**
 * @brief Zaregistruje sekci INI `[AUDIO_CAPTURE]` a načte g_audio_capture_settings.
 * @pre Voláno jednou z cfgmain_init() (vedle screenshot_config_init()).
 */
void audio_capture_config_init(void);

/**
 * @brief Výstupní složka zachycení (kořen, do kterého se zakládají podsložky jednotlivých zachycení).
 *
 * Nastavená `output_dir`, jinak `audio_captures` v domovském adresáři
 * emulátoru (vedle `screenshots`), jinak ".". Složku vytvoří, pokud chybí.
 *
 * @param out  Výstup.
 * @param size Velikost `out`.
 * @return false = nastavenou output_dir nelze vytvořit (out ji přesto obsahuje).
 */
bool audio_capture_resolve_output_dir(char *out, size_t size);

/**
 * @brief Výchozí parametry s výběrem souborů z g_audio_capture_settings.
 * @param p Výstup (dir "", frames 0, quit_after_stop false).
 */
void audio_capture_params_from_settings(st_AUDIO_CAPTURE_PARAMS *p);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_H */
