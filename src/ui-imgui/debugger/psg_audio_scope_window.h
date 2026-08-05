/**
 * @file psg_audio_scope_window.h
 * @brief Debugger: PSG Audio Scope - dynamická analýza zvukového výstupu (F2).
 *
 * Samostatné debug okno pro **slyšitelný výstup** PSG SN76489. Liší se od
 * `psg_window.h` (PSG State), který ukazuje statický snapshot chip stavu.
 *
 * Audio Scope drží per-(chip, channel) ring buffer minulých vzorků
 * `{t, attn, divider, type}` (kapacita 600 vzorků; t je pxCLK gdg timebase)
 * a v UI thread každý ImGui frame:
 *   1) volá `psg_audio_scope_tick()` k uložení aktuálního snapshotu PSG
 *      mirror getterů (side-effect free vůči emulátoru),
 *   2) renderuje responsive oscilloscope per kanál (= šířka řádku
 *      odpovídá `ImGui::GetContentRegionAvail().x`).
 *
 * F2 přidává per-kanál envelope renderer (attn historie z ring bufferu
 * vykreslená jako filled-rect timeline pod responsive scope řádkem).
 *
 * F3 přidává note event detector - state machine per (chip, channel)
 * která detekuje přechody silent ↔ playing v `psg_audio_scope_tick()`
 * a zaznamenává note on/off události s MIDI pitch + cents detune + velocity
 * do per-kanál events ring bufferu (kapacita 1024 not).
 *
 * F4 přidává piano roll renderer - vodorovné timeline pruhy
 * `(t_on..t_off, pitch)` per (chip, channel) v collapsible sekci pod
 * per-kanál scope + envelope řádky. Time range selector (10s / 30s / All),
 * auto-fit pitch range na základě obsahu events bufferu + active note,
 * per-channel coloring, hover tooltip s detailem noty.
 *
 * F5 přidává export aktuálního obsahu events bufferů do CSV nebo MIDI
 * souboru. IGFD save dialog (separátní pro CSV / MIDI), default timestamp
 * filename, locale-safe float formatting (`g_ascii_formatd`) pro CSV.
 * MIDI = Standard MIDI File type 1, 480 PPQN, jeden track per (chip,
 * channel), tempo configurable (40-300 BPM, default 120), NOISE eventy
 * mapované na MIDI drum channel (= "kanál 10" v 1-based, pitch 38 =
 * Acoustic Snare). Tlačítka v toolbaru, disabled stav pokud nejsou žádné
 * uzavřené ani aktivní noty.
 *
 * Plánované rozšíření ve dalších F-fázích (viz mutant README):
 *   F6 = docs + tests.
 *
 * Visibilitu drží `g_gui->showPsgAudioScopeWindow`.
 *
 * MZ-700 PSG nemá - volající gatují okno i tick runtime přes
 * `g_mzhal.psg_count >= 1`.
 *
 * License: GPLv3.
 */

#ifndef PSG_AUDIO_SCOPE_WINDOW_H
#define PSG_AUDIO_SCOPE_WINDOW_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Kapacita ring bufferu attn change pointů per nota.
 *
 * Drží absolutní pxCLK timestamp + attn hodnotu vždy, když se attn během
 * běžící noty změní (= mimo note_on a note_off, ty jsou implicitní).
 * Při překročení se nejstarší pointy přepisují (ring overwrite) a flag
 * `attn_history_overflowed` je nastaven na true.
 *
 * Volba 32: pokrývá běžné případy (vibrato 4-6 cyklů, fade-out ramp 8-16
 * kroků). Pro 1-bit PCM modulaci (Picture Show, ~4-8 kHz) tato kapacita
 * pokrývá posledních ~4 ms aktivity, zbytek se ztrácí (overflowed flag).
 */
#define PSG_SCOPE_MAX_ATTN_POINTS  32u

/**
 * @brief Jeden bod volume envelope (attn change během noty).
 *
 * Snapshot okamžiku, kdy state machine detekovala změnu attn na běžícím
 * kanále. Zachycen v `psg_audio_scope_tick()` mezi note_on a note_off.
 *
 * Časový základ shodný s `st_PSG_SCOPE_NOTE_EVENT::t_on/t_off`
 * (pxCLK total ticks, gdg timebase).
 */
typedef struct st_PSG_SCOPE_ATTN_POINT
{
    uint64_t t_ticks;     /**< Absolutní pxCLK tick okamžiku změny. */
    uint8_t  attn;        /**< Nová hodnota attn (0..15). */
    uint8_t  reserved[3]; /**< Padding na 16 B, future use. */
} st_PSG_SCOPE_ATTN_POINT;

/**
 * @brief Záznam jedné detekované noty (note on/off pár) z PSG kanálu.
 *
 * Vyplňuje state machine v `psg_audio_scope_tick()` při přechodu z hraní
 * do ticha (= note_off uzavře záznam zahájený předchozím note_on).
 *
 * Časové údaje `t_on` / `t_off` jsou hodnoty `gdg_get_total_ticks()` v
 * okamžiku přechodu (= pxCLK monotonní timebase, 17.7345 MHz pro MZ-800).
 * Pro MIDI/CSV export se konvertují na sekundy přes dělení `GDGCLK_BASE`,
 * což je nezávislé na frekvenci UI render loopu (60/75/144 Hz monitor).
 *
 * Invarianty:
 *  - `t_off >= t_on` (note_off vždy nastane po note_on).
 *  - `midi_pitch` v rozsahu 0..127 pro TONE, -1 pro NOISE.
 *  - `cents_detune` v rozsahu -50..+50 (zaokrouhleno k nejbližší
 *    rovnoměrně temperované notě).
 *  - `velocity` v rozsahu 0..127 (MIDI standard) odvozená z `attn`
 *    na okamžiku note_on jako `round(127 * (1 - attn/15))`.
 *  - `channel` 0..3, `chip` 0..1 (0 = mono/levý, 1 = pravý).
 */
typedef struct st_PSG_SCOPE_NOTE_EVENT
{
    uint64_t t_on;          /**< pxCLK total ticks v okamžiku note_on (gdg timebase). */
    uint64_t t_off;         /**< pxCLK total ticks v okamžiku note_off (gdg timebase). */
    int      midi_pitch;    /**< 0..127 MIDI, -1 = NOISE kanál. */
    int      cents_detune;  /**< -50..+50 cents od nejbližší noty (TONE), 0 pro NOISE. */
    uint8_t  velocity;      /**< 0..127 MIDI velocity z attn na note_on. */
    uint8_t  channel;       /**< Index kanálu 0..3. */
    uint8_t  chip;          /**< Index chipu 0..1 (0 = mono/levý). */
    uint8_t  reserved;      /**< Padding, future use. */

    /* Volume envelope tracking (CC 7 export + piano roll vizualizace). */

    /**
     * @brief Ring buffer attn change pointů během běžící noty.
     *
     * Pole se plní pouze pokud se attn během běhu noty mění (= mimo
     * note_on/note_off, ty jsou implicitní). Pořadí v poli odpovídá
     * pořadí příchodu (ring s wrap-around přes `attn_history_head`).
     */
    st_PSG_SCOPE_ATTN_POINT attn_history[PSG_SCOPE_MAX_ATTN_POINTS];

    uint8_t  attn_history_count;     /**< Aktuální plnost ringu (0..PSG_SCOPE_MAX_ATTN_POINTS). */
    uint8_t  attn_history_head;      /**< Index dalšího zápisu (po overflow se točí). */
    bool     attn_history_overflowed;/**< true = od note_on jsme přepsali aspoň jeden starý point. */
    uint8_t  attn_last_seen;         /**< Poslední pozorovaný attn pro detekci změny (0..15). */
} st_PSG_SCOPE_NOTE_EVENT;

/**
 * @brief Vrátí celkový počet uzavřených note eventů přes všechny (chip, ch).
 *
 * Pomocná funkce pro status display ("Notes recorded: N"). Suma per-kanál
 * `count` ring bufferů. Pro detail per kanál je interní API .cpp (F4
 * piano roll bude číst přímo přes interní API).
 *
 * @return Suma uzavřených notu přes všechny kanály (0 pokud nic).
 */
unsigned psg_audio_scope_total_notes(void);

/**
 * @brief Vykreslí ImGui okno PSG Audio Scope.
 *
 * V F4 fázi obsahuje per-kanálový responsive oscilloscope, envelope
 * timeline (attn historie přes ring buffer), info hlavičku a piano roll
 * sekci (timeline pruhů per kanál); export se přidá v F5.
 *
 * Na platformě bez PSG (MZ-700) funkci nevolat - volající gatuje
 * runtime přes `g_mzhal.psg_count >= 1`.
 *
 * @param p_open  Pointer na visibility flag (typicky
 *                `&g_gui->showPsgAudioScopeWindow`). Nesmí být NULL.
 */
void imgui_psg_audio_scope_window(bool *p_open);

/**
 * @brief Snapshot tick - uloží aktuální PSG mirror hodnoty do ring bufferu.
 *
 * Volá se z UI render loopu (`main_window.cpp`) jednou per ImGui frame.
 * Side-effect free vůči emulátoru - čte pouze přes `psg_mirror_*` gettery.
 *
 * Volá se z UI loopu **bezpodmínečně** (= i když je okno zavřené), aby
 * ring buffer průběžně obsahoval posledních ~10 s historie. Vnitřně
 * funkce dělá no-op pokud je emulátor v pauze (`EMULATOR_TEST_PAUSED`)
 * - jinak by se push do ring bufferu vyrobil phantom samples (stejný
 * attn/divider opakovaně, ale frame counter běží), což by po unpause
 * vyrobilo nepravdivé note durations. Po unpause sampling pokračuje od
 * kde skončil (bez time gap).
 *
 * Okno samotné jen renderuje aktuální stav ringu. Overhead 4-8 vzorků
 * per frame je zanedbatelný.
 *
 * Na platformě bez PSG (MZ-700) funkci nevolat - volající gatuje
 * runtime přes `g_mzhal.psg_count >= 1`.
 */
void psg_audio_scope_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* PSG_AUDIO_SCOPE_WINDOW_H */
