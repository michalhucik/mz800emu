/**
 * @file   videorec_sidecar.h
 * @brief  Sidecar model video záznamu: segmenty (střihy), markery a AVI party, zápis do JSON.
 *
 * Soubor `<záznam>.cuts.json` popisuje, jak z nahraných AVI partů sestavit výsledné
 * video (exportní skript čte segmenty a přechody). Čísla snímků jsou globální index
 * výstupního snímku napříč party. Pauza nahrávání nespotřebovává snímky, takže segmenty
 * na sebe v číslování navazují a hranice segmentu je místo střihu.
 *
 * Modul nepoužívá json-glib (build s MZ_NO_MCP ho nemá); JSON se skládá přes GString
 * s escapováním. Formát (verze 4, viz níže; bez videorec_sidecar_set_platform()
 * verze 3 - chybí jen řádky s platformou, framebufferem a canvasem):
 * @code
 * { "version": 4, "platform": "mz800", "tv_system": "pal",
 *   "width": 928, "height": 576, "line_doubled": true,
 *   "framebuffer_width": 928, "framebuffer_height": 288,
 *   "canvas": { "x": 154, "y": 46, "width": 640, "height": 200 },
 *   "fps_num": 50, "fps_den": 1,
 *   "audio_rate": 48000, "default_transition": "fade", "transition_ms": 500,
 *   "parts": [ { "file": "x.avi", "first_frame": 0 } ],
 *   "segments": [ { "start": 0, "end": 1500, "transition_in": "none" } ],
 *   "markers": [ { "frame": 250, "label": "Level 2" } ],
 *   "events": [ { "frame": 0, "kind": "timebase", "value": "realtime" },
 *               { "frame": 700, "kind": "pause_start", "value": "user" } ] }
 * @endcode
 *
 * `width` a `height` jsou rozměry snímku v AVI. `line_doubled` = true znamená, že
 * každý řádek framebufferu emulátoru je v AVI zapsán dvakrát (AVI má pak poměr
 * stran jako okno emulátoru; framebuffer má `height / 2` řádků). Verze 1 (starší
 * nahrávky) pole `line_doubled` nemá a její snímek je nativní framebuffer
 * (928x288) - exportní skript čte obě verze.
 *
 * Verze 3 přidává pole `events` - značky stavu emulátoru pro export (např.
 * vypálení indikátorů pauzy a rychlosti do videa). Každá událost je
 * `{frame, kind, value}`: `frame` je globální index snímku, od kterého stav
 * platí, `kind` jeden z VIDEOREC_SC_EVENT_* a `value` vždy řetězec (význam
 * podle druhu, viz makra). Pole je vždy přítomné (může být prázdné, např.
 * při nastavení `state_marks = none`). Verze 1 a 2 pole nemají; jinak je
 * formát shodný, takže čtenář verze 3 přečte i starší sidecary.
 *
 * Verze 4 přidává popis platformy, na které se nahrávalo (nahrávat jde na
 * MZ-700 PAL/NTSC, MZ-800 i MZ-1500, které se liší rozměry i snímkovou
 * frekvencí): `platform` ("mz700", "mz800", "mz1500"), `tv_system` ("pal",
 * "ntsc"), nativní rozměry framebufferu emulátoru `framebuffer_width`
 * a `framebuffer_height` (bez zdvojení řádků) a `canvas` - obdélník
 * kreslicí plochy mezi bordery v nativních souřadnicích framebufferu.
 * `fps_num` / `fps_den` jsou skutečná snímková frekvence platformy (50 nebo
 * 60). Ostatní pole jsou shodná s verzí 3, takže čtenář verze 4 přečte
 * i verze 1-3 (u nich platí MZ-800: framebuffer 928x288, canvas
 * 154,46 640x200, 50 snímků/s).
 *
 * @par Vlákna
 * Instance není thread-safe; volající serializuje přístup vnějším zámkem.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_SIDECAR_H
#define VIDEOREC_SIDECAR_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Typ přechodu na začátku segmentu. */
typedef enum {
    VIDEOREC_TRANS_NONE = 0,   /**< Žádný přechod (jen první segment). */
    VIDEOREC_TRANS_CUT,        /**< Tvrdý střih. */
    VIDEOREC_TRANS_FADE,       /**< Prolnutí přes černou. */
    VIDEOREC_TRANS_CROSSFADE,  /**< Křížové prolnutí. */
    VIDEOREC_TRANS_CARD        /**< Titulní karta. */
} en_VIDEOREC_TRANSITION;

/**
 * @brief Textový název přechodu ("none", "cut", "fade", "crossfade", "card").
 * @param t Přechod.
 * @return Statický řetězec; pro neplatnou hodnotu "none".
 */
const char *videorec_transition_name(en_VIDEOREC_TRANSITION t);

/**
 * @brief Převede název na přechod (inverzní k videorec_transition_name()).
 * @param s   Název (nesmí být NULL).
 * @param out Výstup; při neúspěchu nezměněn.
 * @return true při nalezení, false pro neznámý název.
 */
bool videorec_transition_from_name(const char *s, en_VIDEOREC_TRANSITION *out);

/**
 * @name Druhy událostí stavu (pole `kind` v `events`, verze 3)
 * @{
 */
/** Časová základna od tohoto snímku; value `"emulated"` nebo `"realtime"`. */
#define VIDEOREC_SC_EVENT_TIMEBASE "timebase"
/** Začátek pauzy emulace; value `"user"` (ruční pauza UI / MCP pause), `"breakpoint"` nebo `"frames"` (doběhnutí N snímků, MCP emu_run). */
#define VIDEOREC_SC_EVENT_PAUSE_START "pause_start"
/** Konec pauzy emulace; value `""`. */
#define VIDEOREC_SC_EVENT_PAUSE_END "pause_end"
/** Rychlost emulace od tohoto snímku; value procenta (`"100"`, `"400"`) nebo `"max"` (MAX SPEED). */
#define VIDEOREC_SC_EVENT_SPEED "speed"
/** Nahrání snapshotu; value `"retake"` (zahozeno a přetočeno) nebo `"seam"` (šev). */
#define VIDEOREC_SC_EVENT_SNAPSHOT "snapshot"
/** Reset emulovaného počítače; value `""`. */
#define VIDEOREC_SC_EVENT_RESET "reset"
/** @} */

/** @brief Neprůhledný sidecar model. Vlastní segmenty, markery a party (hluboké kopie řetězců). */
typedef struct st_VIDEOREC_SIDECAR st_VIDEOREC_SIDECAR;

/**
 * @brief Vytvoří prázdný model.
 * @param width              Šířka snímku v AVI [px].
 * @param height             Výška snímku v AVI [px] (u zdvojených řádků už zdvojená).
 * @param fps_num            Čitatel snímkové frekvence.
 * @param fps_den            Jmenovatel snímkové frekvence.
 * @param audio_rate         Vzorkovací frekvence audia [Hz].
 * @param default_transition Výchozí přechod pro export.
 * @param transition_ms      Délka přechodu [ms].
 * @return Nová instance (uvolnit videorec_sidecar_free()); `line_doubled` je false
 *         (viz videorec_sidecar_set_line_doubled()).
 */
st_VIDEOREC_SIDECAR *videorec_sidecar_new(unsigned width, unsigned height, unsigned fps_num, unsigned fps_den,
                                          unsigned audio_rate, en_VIDEOREC_TRANSITION default_transition,
                                          unsigned transition_ms);

/**
 * @brief Nastaví příznak zdvojených řádků (pole `line_doubled` v JSON).
 * @param s            Model.
 * @param line_doubled true = AVI obsahuje každý řádek framebufferu dvakrát
 *                     (`height` modelu je pak dvojnásobek výšky framebufferu).
 * @note Mění jen zápis do JSON; rozměry modelu nepřepočítává.
 */
void videorec_sidecar_set_line_doubled(st_VIDEOREC_SIDECAR *s, bool line_doubled);

/**
 * @brief Nastaví popis platformy; JSON pak má verzi 4 (pole `platform`, `tv_system`,
 *        `framebuffer_width`, `framebuffer_height`, `canvas`).
 *
 * @param s         Model.
 * @param platform  Platforma ("mz700", "mz800", "mz1500"); zkopíruje se, NULL = "".
 * @param tv_system TV norma ("pal", "ntsc"); zkopíruje se, NULL = "".
 * @param fb_width  Šířka nativního framebufferu [px].
 * @param fb_height Výška nativního framebufferu [px] (bez zdvojení řádků).
 * @param canvas_x  Levý okraj canvasu ve framebufferu [px].
 * @param canvas_y  Horní okraj canvasu ve framebufferu [px].
 * @param canvas_w  Šířka canvasu [px].
 * @param canvas_h  Výška canvasu [px].
 * @post Opakované volání přepíše předchozí hodnoty (staré řetězce uvolní).
 * @note Rozměry AVI (`width`, `height`) se tím nemění.
 */
void videorec_sidecar_set_platform(st_VIDEOREC_SIDECAR *s, const char *platform, const char *tv_system,
                                   unsigned fb_width, unsigned fb_height, unsigned canvas_x, unsigned canvas_y,
                                   unsigned canvas_w, unsigned canvas_h);

/** @brief Uvolní model včetně všech řetězců. @param s Instance nebo NULL (pak no-op). */
void videorec_sidecar_free(st_VIDEOREC_SIDECAR *s);

/**
 * @brief Přidá AVI part.
 * @param s           Model.
 * @param filename    Jméno souboru bez cesty (zkopíruje se).
 * @param first_frame Globální index prvního snímku partu; party se přidávají vzestupně.
 */
void videorec_sidecar_add_part(st_VIDEOREC_SIDECAR *s, const char *filename, uint64_t first_frame);

/**
 * @brief Zahájí nový segment.
 *
 * Je-li otevřený segment, nejdřív se uzavře na `frame` (prázdný segment se zahodí).
 * První segment v modelu má vždy přechod NONE bez ohledu na parametr (platí i když
 * byl předchozí první segment zahozen jako prázdný).
 *
 * @param s             Model.
 * @param frame         Globální index prvního snímku segmentu.
 * @param transition_in Přechod na začátku segmentu.
 * @post segment_open == true.
 */
void videorec_sidecar_segment_begin(st_VIDEOREC_SIDECAR *s, uint64_t frame, en_VIDEOREC_TRANSITION transition_in);

/**
 * @brief Uzavře otevřený segment na `frame` (exkluzivní konec). Prázdný segment (start == end) se odstraní.
 *        Bez otevřeného segmentu no-op. `frame` menší než začátek segmentu se bere jako
 *        začátek (segment je pak prázdný a odstraní se).
 * @param s     Model.
 * @param frame Aktuální počet snímků.
 */
void videorec_sidecar_segment_end(st_VIDEOREC_SIDECAR *s, uint64_t frame);

/** @brief Je poslední segment otevřený? @param s Model. @return true pokud ano. */
bool videorec_sidecar_segment_open(const st_VIDEOREC_SIDECAR *s);

/**
 * @brief Počet segmentů v modelu (včetně otevřeného).
 *
 * Prázdný segment zahozený při uzavření se nepočítá; videorec_sidecar_truncate()
 * počet sníží o odstraněné segmenty. Při otevřeném segmentu je to zároveň
 * pořadové číslo aktuálního segmentu (od 1).
 *
 * @param s Model.
 * @return Počet segmentů (0 = žádný).
 */
unsigned videorec_sidecar_segment_count(const st_VIDEOREC_SIDECAR *s);

/**
 * @brief Přidá marker.
 *
 * Markery se drží seřazené podle snímku; marker se zařadí za poslední
 * marker se stejným nebo menším snímkem (stabilně - stejné snímky v pořadí
 * přidání). Přidání mimo pořadí může nastat v režimu podle reality, kde se
 * značky vztahují k pozici obrazu za zpožďovací frontou.
 *
 * @param s     Model.
 * @param frame Globální index snímku.
 * @param label Popisek (zkopíruje se; NULL se bere jako prázdný řetězec).
 */
void videorec_sidecar_add_marker(st_VIDEOREC_SIDECAR *s, uint64_t frame, const char *label);

/**
 * @brief Přidá událost stavu (pole `events`, verze 3).
 *
 * Řazení stejně jako u videorec_sidecar_add_marker() (podle snímku, stabilně).
 *
 * @param s     Model.
 * @param frame Globální index snímku, od kterého stav platí.
 * @param kind  Druh (VIDEOREC_SC_EVENT_*; zkopíruje se; NULL = "").
 * @param value Hodnota (zkopíruje se; NULL = "").
 */
void videorec_sidecar_add_event(st_VIDEOREC_SIDECAR *s, uint64_t frame, const char *kind, const char *value);

/**
 * @brief Počet událostí stavu v modelu.
 * @param s Model.
 * @return Počet událostí.
 */
unsigned videorec_sidecar_event_count(const st_VIDEOREC_SIDECAR *s);

/**
 * @brief Ořízne model na snímky `< frame` (pro návrat v čase / přetočení záznamu).
 *
 * Zahodí markery a události s `frame >= frame`, segmenty začínající na `>= frame` (kromě prvního,
 * ten se zkrátí) a party s `first_frame >= frame` (kromě prvního). Poslední zbylý
 * segment dostane `end = frame` a je znovu otevřený.
 *
 * @param s     Model.
 * @param frame Hranice oříznutí.
 */
void videorec_sidecar_truncate(st_VIDEOREC_SIDECAR *s, uint64_t frame);

/**
 * @brief Serializuje model do JSON (verze 4 s popisem platformy, bez něj verze 3; vždy včetně polí
 *        `line_doubled` a `events`). Otevřený segment se zapíše s aktuálním `end`
 *        (posledně předaným do segment_end/truncate; u čerstvě otevřeného je end == start).
 * @param s Model.
 * @return Nový řetězec (uvolnit g_free()).
 */
char *videorec_sidecar_to_json(const st_VIDEOREC_SIDECAR *s);

/**
 * @brief Atomicky uloží JSON do souboru (g_file_set_contents).
 * @param s    Model.
 * @param path Cílová cesta.
 * @param err  Chyba GLib (může být NULL).
 * @return true při úspěchu.
 */
bool videorec_sidecar_save(const st_VIDEOREC_SIDECAR *s, const char *path, GError **err);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOREC_SIDECAR_H */
