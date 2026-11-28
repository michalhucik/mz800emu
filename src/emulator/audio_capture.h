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
 * | `reference.wav`  | zvuk z rendereru videa (videorec_audio, přesné převzorkování v emulovaném čase, takty na vzorky podle krystalu) |
 * | `sdl.wav`        | zvuk tak, jak ho vyrobila skutečná SDL cesta (iface_audio_wait_for_data()) |
 * | `capture.json`   | metadata (platforma, takt, kanály, počty, důvod konce); zapisuje se při konci |
 *
 * Časy v CSV jsou v taktech GDG od začátku zachycení (`ticks`) a v µs
 * (`time_us`, skutečný čas podle krystalu, GDGCLK_REAL_TICKS_PER_SEC); `frame`
 * je pořadí zvukového snímku (19,985 ms PAL / 16,688 ms NTSC) od začátku
 * zachycení.
 *
 * Začátek: audio_capture_start() zachycení jen naplánuje, skutečně začne na
 * konci zvukového snímku (audio_capture_on_frame()), kdy jsou dopočítané
 * všechny kanály PSG - stejně jako začátek video záznamu. Se sdl.wav až po
 * rozjezdu SDL cesty (6 snímků, bloky se zahazují), bez něj na konci
 * nejbližšího snímku. Úrovně kanálů na začátku se berou z živého audio logu;
 * renderer reference.wav se zahřeje 100 až 120 ms (celý počet vzorků) se stejnými úrovněmi a stav
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
 * | `frames`        | st_AUDIO_CAPTURE_FRAME_REC (208 B), souhrn zvukového snímku | konec snímku |
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
 * Snímek zachycení je jedna synchronizační událost emulace (konec
 * zvukového logu): při 100 % jeden snímek obrazu, při vlastní rychlosti
 * úsek emulace o délce rychlost krát snímek. Reference za něj vyrenderuje
 * vzorky jeho emulovaného času (při 50 % asi polovinu), SDL cesta dodá
 * jeden blok (881 nebo 882, NTSC 735 nebo 736 vzorků) bez ohledu na
 * rychlost. ref_first a sdl_first jsou indexy prvního vzorku snímku
 * v proudech `pcm_reference` a `pcm_sdl` (= `seq` těchto proudů), takže
 * `[x_first, x_first + x_samples)` sousedních snímků na sebe přesně
 * navazují (audio_capture_pcm po snímcích).
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
    uint64_t ref_first;   /**< Index prvního vzorku snímku v proudu reference (vzorky vyrenderované před ním). */
    uint64_t sdl_first;   /**< Index prvního vzorku snímku v proudu SDL cesty (vzorky bloků přijaté před ním). */
} st_AUDIO_CAPTURE_FRAME_REC;

G_STATIC_ASSERT(sizeof(st_AUDIO_CAPTURE_LEVEL_REC) == 16);
G_STATIC_ASSERT(sizeof(st_AUDIO_CAPTURE_PSG_REC) == 24);
G_STATIC_ASSERT(sizeof(st_AUDIO_CAPTURE_FRAME_REC) == 208);

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
    uint64_t memory_bytes;       /**< Alokovaná paměť ringů a indexu hranic snímků [B] (index roste podle potřeby, viz audio_capture_frame_range()). */
    st_AUDIO_CAPTURE_STREAM_INFO streams[AUDIO_CAPTURE_STREAM_COUNT]; /**< Proudy paměti. */
} st_AUDIO_CAPTURE_STATUS;

/**
 * @brief Kopie úseku proudu paměti zachycení (audio_capture_stream_copy(),
 *        audio_capture_stream_copy_window()).
 *
 * @par Invarianty
 * `first_seq <= next_seq`; zkopírované záznamy jsou
 * `[data_seq, data_seq + data_count)` a leží v `[first_seq, next_seq)`.
 * U audio_capture_stream_copy_window() navíc `data_seq == held_seq`,
 * `data_count <= held_count` a `[held_seq, held_seq + held_count)` leží
 * v požadovaném okně.
 *
 * @par Vlastnictví
 * `data` alokuje kopírovací funkce (g_malloc), uvolní
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
    uint64_t data_seq;    /**< seq prvního zkopírovaného záznamu (u okna i bez dat: viz `held_seq`). */
    uint64_t data_count;  /**< Počet zkopírovaných záznamů. */
    uint64_t held_seq;    /**< Jen audio_capture_stream_copy_window(): začátek průniku okna s drženým úsekem (u prázdného průniku audio_capture_clip_window()). */
    uint64_t held_count;  /**< Jen audio_capture_stream_copy_window(): délka průniku před omezením `max_bytes` (`held_count > data_count` = kopie zkrácená). */
} st_AUDIO_CAPTURE_STREAM_COPY;

/**
 * @brief Průnik požadovaného okna záznamů `[from, to)` s drženým úsekem `[first, next)`.
 *
 * Čistá funkce bez zámku a bez stavu (volá ji audio_capture_stream_copy_window()
 * pod zámkem modulu; jednotkové testy přímo). Nic nepřičítá, takže nepřeteče
 * ani pro indexy u UINT64_MAX.
 *
 * @param from   Začátek okna.
 * @param to     Konec okna (bez); `to < from` se bere jako prázdné okno `[from, from)`.
 * @param first  Nejstarší držený záznam proudu.
 * @param next   Příští záznam proudu (`first <= next`; `first == next` = prázdný proud).
 * @param lo_out Výstup: začátek průniku. U prázdného průniku místo v okně,
 *               kde by data začínala: konec okna `to`, leží-li celé okno před
 *               `first` (data ztracena), jinak `max(from, first)` omezené na `to`
 *               (okno za `next` nebo prázdné okno).
 * @return Délka průniku (0 = žádný záznam okna se nedrží).
 * @post `from <= *lo_out <= max(from, to)` a `[*lo_out, *lo_out + návrat)`
 *       leží v `[from, to)` i v `[first, next)`.
 */
static inline uint64_t audio_capture_clip_window(uint64_t from, uint64_t to, uint64_t first, uint64_t next,
                                                 uint64_t *lo_out)
{
    if (to < from) to = from;
    const uint64_t lo = (from > first) ? from : first;
    const uint64_t hi = (to < next) ? to : next;
    if (lo >= hi) {
        *lo_out = (lo < to) ? lo : to;
        return 0;
    }
    *lo_out = lo;
    return hi - lo;
}

/**
 * @brief Společné dvojice dvou úseků proudu při pevném posunu druhého.
 *
 * Dvojice je (a[i], b[i + lag]) pro index i v souřadnicích prvního proudu.
 * Úseky jsou `[a_seq, a_seq + a_count)` a `[b_seq, b_seq + b_count)`;
 * vrátí, pro kolik po sobě jdoucích i od `*from_out` existují oba prvky.
 * Bez přetečení (odečítá jen tam, kde nepodteče, součty nasytí).
 *
 * @param a_seq    Začátek prvního úseku (souřadnice reference).
 * @param a_count  Délka prvního úseku.
 * @param b_seq    Začátek druhého úseku (souřadnice druhého proudu).
 * @param b_count  Délka druhého úseku.
 * @param lag      Posun druhého proudu za prvním [záznamy], >= 0.
 * @param from_out Výstup: první společné i (u nulového výsledku `a_seq`).
 * @return Počet společných dvojic (0 = žádné).
 */
static inline uint64_t audio_capture_lag_overlap(uint64_t a_seq, uint64_t a_count, uint64_t b_seq,
                                                 uint64_t b_count, uint64_t lag, uint64_t *from_out)
{
    *from_out = a_seq;
    const uint64_t a_end = (a_count > UINT64_MAX - a_seq) ? UINT64_MAX : a_seq + a_count;
    const uint64_t b_end = (b_count > UINT64_MAX - b_seq) ? UINT64_MAX : b_seq + b_count;
    if (b_end <= lag) return 0;                         /* celý druhý úsek před posunem */
    const uint64_t b_lo = (b_seq > lag) ? b_seq - lag : 0u;   /* první i s b[i + lag] */
    const uint64_t b_hi = b_end - lag;
    const uint64_t lo = (a_seq > b_lo) ? a_seq : b_lo;
    const uint64_t hi = (a_end < b_hi) ? a_end : b_hi;
    if (lo >= hi) return 0;
    *from_out = lo;
    return hi - lo;
}

/** @brief Bit st_AUDIO_CAPTURE_FRAME_RANGE::flags: zatím žádný uzavřený snímek (prázdný výřez). */
#define AUDIO_CAPTURE_RANGE_NO_FRAMES       0x01u
/** @brief Bit st_AUDIO_CAPTURE_FRAME_RANGE::flags: začátek za posledním uzavřeným snímkem (prázdný výřez). */
#define AUDIO_CAPTURE_RANGE_START_NOT_CLOSED 0x02u
/** @brief Bit st_AUDIO_CAPTURE_FRAME_RANGE::flags: konec za posledním uzavřeným snímkem, oříznuto na jeho konec. */
#define AUDIO_CAPTURE_RANGE_END_NOT_CLOSED  0x04u
/** @brief Bit st_AUDIO_CAPTURE_FRAME_RANGE::flags: hranice začátku se už nedrží, začátek posunut na nejstarší drženou (při `samples == 0` celý rozsah). */
#define AUDIO_CAPTURE_RANGE_START_NOT_HELD  0x08u

/**
 * @brief Výřez proudu PCM podle snímků zachycení (audio_capture_frame_range()).
 *
 * Pokrývá jen uzavřené snímky, jejichž hranice index drží; vše ostatní
 * hlásí bity `flags`. Indexy vzorků jsou přesné (ne odhad podle krystalu).
 */
typedef struct st_AUDIO_CAPTURE_FRAME_RANGE {
    uint64_t sample_from;    /**< První vzorek výřezu v proudu (u prázdného výřezu hranice, u které skončil). */
    uint64_t samples;        /**< Počet vzorků (0 = prázdný výřez). */
    uint64_t frame_from;     /**< První pokrytý snímek. */
    uint64_t frame_to;       /**< Konec pokrytých snímků (bez); `frame_to == frame_from` u prázdného výřezu. */
    uint64_t closed_frames;  /**< Počet uzavřených snímků zachycení (= číslo dosud otevřeného snímku). */
    uint64_t held_from;      /**< Nejstarší snímek, jehož hranici index drží. */
    unsigned flags;          /**< AUDIO_CAPTURE_RANGE_* (0 = celý požadovaný rozsah). */
    unsigned segment;        /**< Pořadí zachycení, ke kterému hranice patří (k ověření, že kopie vzorků je z téhož). */
} st_AUDIO_CAPTURE_FRAME_RANGE;

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
 * Kurzorové čtení: když ring začátek už přepsal, kopie začne nejstarším
 * drženým záznamem a odměří od něj `count` záznamů (může tedy sahat za
 * `from_seq + count`). Pro pevné okno (výřez podle snímku, srovnání úseků)
 * slouží audio_capture_stream_copy_window().
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
 * @brief Zkopíruje z proudu jen záznamy pevného okna `[from_seq, to_seq)`, které ring ještě drží.
 *
 * Pod jedním zámkem modulu spočte průnik okna s drženým úsekem
 * `[first_seq, next_seq)` (audio_capture_clip_window()) a zkopíruje jeho
 * začátek, nejvýš `max_bytes` bajtů. Průnik i kopie jsou tak atomické vůči
 * zápisu (živé čtení): mezi nimi ring nemůže poskočit. Okno se nikdy
 * neposouvá: přepsaný začátek se vynechá, nic za `to_seq` se nepřidá.
 * Zcela ztracené okno dá `data_count == 0`, `held_count == 0`
 * a `held_seq == to_seq`.
 *
 * @param stream    Proud.
 * @param from_seq  Začátek okna.
 * @param to_seq    Konec okna (bez; `to_seq <= from_seq` = prázdné okno).
 * @param max_bytes Nejvýš bajtů kopie (omezení se uplatní až na průnik).
 * @param out       Výstup (uvolnit audio_capture_stream_copy_clear()); kromě
 *                  metadat `held_seq`, `held_count` a `data_seq == held_seq`.
 * @return false = nedostatek paměti pro kopii (out bez dat, metadata a průnik platí).
 * @post `[data_seq, data_seq + data_count)` leží v `[from_seq, to_seq)`
 *       i v drženém úseku v okamžiku kopie.
 * @note Libovolné vlákno.
 */
bool audio_capture_stream_copy_window(en_AUDIO_CAPTURE_STREAM stream, uint64_t from_seq,
                                      uint64_t to_seq, size_t max_bytes,
                                      st_AUDIO_CAPTURE_STREAM_COPY *out);

/**
 * @brief Rozsah vzorků proudu PCM pro snímky zachycení `[first, first + frames)`.
 *
 * Hranice snímků bere z indexu hranic, který modul drží po celou dobu,
 * co jsou v PCM ringu data snímku (nezávisle na ringu souhrnů snímků,
 * který mohl souhrn už přepsat): začátek je index prvního vzorku snímku
 * `first` v proudu, konec index prvního vzorku snímku `first + frames`.
 * Platí stejně při jakékoli rychlosti emulace (reference má na snímek
 * vzorky jeho emulovaného času, SDL cesta bloky, které přišly).
 *
 * Kontrakt: výřez pokrývá jen uzavřené snímky. Konec za posledním
 * uzavřeným snímkem se ořízne na jeho konec (AUDIO_CAPTURE_RANGE_END_NOT_CLOSED),
 * začátek za ním dá prázdný výřez (AUDIO_CAPTURE_RANGE_START_NOT_CLOSED,
 * `sample_from` = konec uzavřených snímků; další PCM, např. blok SDL
 * přijatý po zastavení, zůstává čitelné přes index vzorku). Snímky, jejichž
 * hranici index už nedrží, se vynechají (AUDIO_CAPTURE_RANGE_START_NOT_HELD).
 * Pro dva sousední dotazy i jeden společný tak vyjdou stejné indexy.
 *
 * @param stream AUDIO_CAPTURE_STREAM_PCM_REFERENCE nebo AUDIO_CAPTURE_STREAM_PCM_SDL.
 * @param first  První snímek.
 * @param frames Počet snímků, >= 1 (součet se nasytí).
 * @param out    Výstup (vynuluje se).
 * @return false = jiný proud, proud se nedrží nebo zachycení nemá index hranic (out nulový).
 * @note Libovolné vlákno (pod zámkem modulu, konzistentní stav).
 */
bool audio_capture_frame_range(en_AUDIO_CAPTURE_STREAM stream, uint64_t first, uint64_t frames,
                               st_AUDIO_CAPTURE_FRAME_RANGE *out);

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
