/**
 * @file membrowser_char_inserter.h
 * @brief Char Inserter okno - paleta znaků pro vkládání do Memory Browseru.
 *
 * Singleton okno otevřené z context menu hexview ("Insert character...")
 * nebo z menu Debugger. Drží referenci na zdrojovou MB instanci (per
 * @c source_instance_idx) - klik na cell v gridu zapíše příslušný byte
 * na aktuální cursor pos zdrojové MB + posune cursor + push undo + mark
 * edited (identicky s ASCII typing dispatchem).
 *
 * 3 taby:
 *   - SharpMZ ASCII EU (encoding @c MB_CHARSET_SHARPMZ_EU_UTF8)
 *   - SharpMZ ASCII JP (encoding @c MB_CHARSET_SHARPMZ_JP_UTF8)
 *   - KOI8-CS         (encoding @c MB_CHARSET_KOI8CS)
 *
 * Cell layout: 16x16 grid (256 bytů 0x00..0xFF), per-cell glyph z
 * @c membrowser_encoding_byte_to_utf8 v monospace fontu. Tooltip "0xNN -
 * <utf8 glyph>". LMB = write + advance + okno zůstane otevřené.
 *
 * Backend cesta: ne direct, ale přes @c membrowser_window_write_byte_at_cursor
 * v membrowser_window.cpp (zapouzdřuje backend lifecycle + undo + edited).
 *
 * Persistence: jen ImGui pos/size přes imgui.ini; show flag se neukládá
 * (podle PCG editor pattern; uživatel otvírá explicit per session).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_CHAR_INSERTER_H
#define MEMBROWSER_CHAR_INSERTER_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Otevři Char Inserter okno pro zdrojovou MB instanci.
 *
 * Nastaví @c g_gui->showMembrowserCharInserter = true a uloží zdrojovou
 * instanci. Pokud je @p source_instance_idx mimo rozsah, nastaví no-target
 * stav (cells disabled, status "No target").
 *
 * @param source_instance_idx 0..MB_INSTANCE_COUNT-1 (-1 = no target).
 */
void membrowser_char_inserter_open ( int source_instance_idx );

/**
 * @brief Render Char Inserter okna do current ImGui frame.
 *
 * Volá se per frame z main_window.cpp. No-op pokud
 * @c g_gui->showMembrowserCharInserter je false.
 *
 * @pre Volat pouze z UI vlákna mezi NewFrame a EndFrame.
 */
void membrowser_char_inserter_render ( void );

/**
 * @brief Toggle viditelnosti okna (pro menu Debugger + Alt+Shift+I).
 *
 * Otevře / zavře okno. Pokud otevírá a poprvé v session, target zůstane
 * defaultní 0 (main MB instance).
 */
void membrowser_char_inserter_toggle ( void );


/**
 * @brief Callback typ pro emisi bytu z Char Inserter do volajícího
 *        v "pattern build" módu.
 *
 * Voláno z UI vlákna v render frame Char Inserter okna při kliku na cell
 * v gridu. Caller je odpovědný za thread-safe handling (storage update,
 * eventuální re-render volajícího okna v dalším frame).
 *
 * @param byte     Byte odpovídající kliknutému cell v gridu (0x00..0xFF).
 * @param userdata Opaque pointer předaný při @ref membrowser_char_inserter_open_for_callback.
 */
typedef void ( *membrowser_char_emit_cb_t ) ( uint8_t byte, void *userdata );


/**
 * @brief Otevři Char Inserter v "pattern build" módu - emise bytů jde do
 *        callback volajícího místo do Memory Browser instance.
 *
 * Místo zápisu na cursor pos MB instance (= výchozí chování po @ref
 * membrowser_char_inserter_open), v tomto módu click na cell zavolá
 * @p cb(byte, userdata). Volající typicky appende byte do svého pattern
 * bufferu (např. search pattern v search panelu).
 *
 * Status řádek okna ukazuje @p label místo info o MB instanci (= jasná
 * indikace že target = nepaměť ale např. "Search pattern").
 *
 * Mode přetrvá dokud volající znovu nezavolá @ref membrowser_char_inserter_open
 * (= návrat na MB cursor mode) nebo @ref membrowser_char_inserter_open_for_callback
 * s jiným callbackem.
 *
 * @param cb        Callback volaný při click na cell (NESmí být NULL).
 * @param userdata  Opaque kontext pro callback (NULL OK).
 * @param label     UTF-8 popis target pro status řádek (NULL = default
 *                  "Pattern buffer"). Caller drží lifetime; Char Inserter
 *                  si interně kopíruje do statického bufferu.
 */
void membrowser_char_inserter_open_for_callback (
    membrowser_char_emit_cb_t cb,
    void *userdata,
    const char *label );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* MEMBROWSER_CHAR_INSERTER_H */
