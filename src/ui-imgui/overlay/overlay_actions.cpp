/**
 * @file   overlay_actions.cpp
 * @brief  Tabulka akcí overlay menu: funkce, dostupnost, zkratky a odložené spuštění.
 *
 * Popis kontraktů je v overlay_actions.h. Každá funkce akce volá tutéž
 * funkci jako klasické menu nebo globální zkratka; odkaz na ně je
 * v komentáři funkce.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>

#include "libs/imgui/imgui.h"

#include "i18n.h"
#include "emulator/emulator.h"
#include "emulator/replay.h"
#include "emulator/customspeed.h"
#include "emulator/display.h"
#include "emulator/recent_media/recent_media.h"
#include "audio.h"
#include "iface/iface_audio.h"
#include "iface/iface_joy.h"
extern "C"
{
#include "baseui/baseui.h"
}
#include "hw-generic/joy/joy.h"
#include "hw-generic/joy/joy_request.h"
#include "device_request.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/memory/rom_request.h"
#include "mzarch/mzarch_config.h"
#include "mzarch/mzarch_request.h"
#include "iface/iface_video.h"
#include "hw-generic/cmt/cmt_request.h"

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_request.h"
#endif
#if CFG_HWEXT_HAVE_QDISK
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/qdisk/qdisk_request.h"
#endif

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/cmt/cmt_view.h"
#include "ui-imgui/screenshot/screenshot_ui.h"
#include "ui-imgui/videorec/videorec_menu.h"
#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/overlay/overlay.h"
#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

extern "C"
{
    /* požadavky snapshotů (menu_snapshot.cpp), stejné jako volá imgui_global_shortcuts() */
    void snapshot_ui_request_quickload(void);
    void snapshot_ui_request_quicksave(void);
    void snapshot_ui_request_load(void);
    void snapshot_ui_request_save(void);
}

/* ================================================================
 * Stav
 * ================================================================ */

/** @brief Akce vyžádaná v tomto snímku (OVL_ACTION_NONE = žádná). */
static en_OVL_ACTION s_pending = OVL_ACTION_NONE;
/** @brief Hodnota vyžádané akce s hodnotou (overlay_action_request_value()); platí při čekající akci s arg == OVL_ACTION_ARG_VALUE. */
static int s_pending_value = 0;
/** @brief Naposledy provedená akce pro testy (overlay_actions_test_take_last()). */
static en_OVL_ACTION s_test_last = OVL_ACTION_NONE;
/** @brief Hodnota naposledy provedené akce s hodnotou pro testy (overlay_actions_test_last_value()). */
static int s_test_last_value = 0;

/* ================================================================
 * Dostupnost
 * ================================================================ */

/**
 * @brief Důvod zákazu všech akcí médií (aktivní záznam nebo přehrávání vstupů).
 * @param v Kopie stavu.
 * @return NULL = média nejsou zakázaná; jinak důvod z kopie.
 */
static const char *reason_media(const st_OVERLAY_VIEW *v)
{
    return v->media_disabled_reason[0] ? v->media_disabled_reason : NULL;
}

bool overlay_fdd_wp_on(const st_OVERLAY_VIEW_FDD *d)
{
    return d->user_readonly || (d->mounted && (d->fs_readonly || d->shared_readonly));
}

/**
 * @brief Důvod, proč nejde pracovat s mechanikou (replay, build bez řadiče, odpojený řadič).
 * @param v     Kopie stavu.
 * @param drive Číslo mechaniky 0 až 3.
 * @return NULL = mechanika je k dispozici; jinak přeložený důvod.
 */
static const char *reason_fdd(const st_OVERLAY_VIEW *v, int drive)
{
    const char *r = reason_media(v);
    if (r) return r;
    if (drive < 0 || drive >= OVERLAY_VIEW_FDD_COUNT || !v->fdd[drive].present) return "";
    /* overlay obsluhuje jen řadič FDC0 (jako globální zkratky Alt+1 až Alt+4); FDC1 je v klasickém menu */
    if (!v->fdd[drive].connected) return _("FD controller FDC0 is not connected.");
    return NULL;
}

/** @brief Dostupnost vložení nebo výměny diskety (overlay_action_reason_fn; arg = mechanika). */
static const char *reason_fdd_insert(const st_OVERLAY_VIEW *v, int arg)
{
    return reason_fdd(v, arg);
}

/** @brief Dostupnost vysunutí diskety (overlay_action_reason_fn; arg = mechanika). */
static const char *reason_fdd_eject(const st_OVERLAY_VIEW *v, int arg)
{
    const char *r = reason_fdd(v, arg);
    if (r) return r;
    return v->fdd[arg].mounted ? NULL : _("The drive is empty.");
}

/** @brief Dostupnost uložení změn diskety (overlay_action_reason_fn; arg = mechanika). */
static const char *reason_fdd_save(const st_OVERLAY_VIEW *v, int arg)
{
    const char *r = reason_fdd(v, arg);
    if (r) return r;
    return v->fdd[arg].unsaved ? NULL : _("There are no unsaved changes.");
}

/**
 * @brief Dostupnost přepnutí volby "Read-only" mechaniky (overlay_action_reason_fn; arg = mechanika).
 *
 * Jako klasické menu: u obrazu, jehož soubor nejde zapisovat, je ochrana
 * vynucená a volbu nejde změnit; prázdné mechanice volbu nastavit jde.
 * Zapnutou ochranu nejde sundat ani tehdy, když tentýž soubor drží jiná
 * mechanika se zápisem (do souboru smí zapisovat jen jedna; fdc.h).
 *
 * @return NULL = přepnout jde; jinak přeložený důvod. Důvod se sdílením je
 *         ve statickém bufferu platném do dalšího volání (jen UI vlákno).
 */
static const char *reason_fdd_wp(const st_OVERLAY_VIEW *v, int arg)
{
    const char *r = reason_fdd(v, arg);
    if (r) return r;
    const st_OVERLAY_VIEW_FDD *d = &v->fdd[arg];
    if (d->mounted && d->fs_readonly) return _("The file is write-protected on the disk.");
    if (d->mounted && overlay_fdd_wp_on(d) && d->wp_blocker >= 0) {
        static char buf[192];
        snprintf(buf, sizeof(buf), _("The image is also inserted in FDD %d, which may write to it."), d->wp_blocker);
        return buf;
    }
    return NULL;
}

/**
 * @brief Mechanika, do které vkládá akce "Browse files...": první prázdná, jinak FDD 0.
 * @param v Kopie stavu.
 * @return Číslo mechaniky 0 až 3.
 */
static int browse_target(const st_OVERLAY_VIEW *v)
{
    for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++)
        if (v->fdd[i].present && !v->fdd[i].mounted) return i;
    return 0;
}

/**
 * @brief Dostupnost akce "Browse files..." (overlay_action_reason_fn; arg se nepoužívá).
 *
 * Akce otevírá dialog obrazu diskety, proto ji zakazuje odpojený řadič
 * FDC0, i když kazeta nebo Quick Disk dostupné jsou. Důvod to říká celý:
 * z tlačítka v pruhu nedávných médií není poznat, že patří disketě
 * (posudek N08).
 *
 * @param v   Kopie stavu.
 * @param arg Nepoužívá se.
 * @return NULL = akce je dostupná; jinak přeložený důvod ("" = mechanika v buildu není).
 */
static const char *reason_browse(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const int drive = browse_target(v);
    const char *r = reason_fdd(v, drive);
    if (r && *r && !reason_media(v) && !v->fdd[drive].connected) return _("Browse files opens the disk image dialog, but FD controller FDC0 is not connected.");
    return r;
}

/**
 * @brief Důvod, proč nejde pracovat s Quick Diskem (replay, build bez něj, odpojený, řízený Unicardem).
 * @param v Kopie stavu.
 * @return NULL = Quick Disk je k dispozici; jinak přeložený důvod.
 */
static const char *reason_qd(const st_OVERLAY_VIEW *v)
{
    const char *r = reason_media(v);
    if (r) return r;
    if (!v->qd.present) return "";
    if (!v->qd.connected) return _("Quick Disk is not connected.");
    /* klasické menu mechaniku řízenou Unicardem nenabízí (menu_qdisk.cpp) */
    if (v->qd.unicard) return _("Quick Disk is controlled by Unicard.");
    return NULL;
}

/** @brief Dostupnost vložení Quick Disku: jen do prázdné mechaniky, jako klasické menu (overlay_action_reason_fn). */
static const char *reason_qd_insert(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_qd(v);
    if (r) return r;
    return v->qd.mounted ? _("Eject the disk first.") : NULL;
}

/** @brief Dostupnost vysunutí Quick Disku (overlay_action_reason_fn). */
static const char *reason_qd_eject(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_qd(v);
    if (r) return r;
    return v->qd.mounted ? NULL : _("The drive is empty.");
}

/** @brief Dostupnost uložení změn Quick Disku (overlay_action_reason_fn). */
static const char *reason_qd_save(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_qd(v);
    if (r) return r;
    return v->qd.unsaved ? NULL : _("There are no unsaved changes.");
}

/** @brief Dostupnost přepnutí volby "Write Protected" Quick Disku (overlay_action_reason_fn); jako klasické menu: připojený a neřízený Unicardem. */
static const char *reason_qd_wp(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return reason_qd(v);
}

/** @brief Probíhá nahrávání na pásku? */
static bool cmt_recording(const st_OVERLAY_VIEW *v)
{
    return v->cmt.has_tape && v->cmt.state == OVERLAY_CMT_RECORD;
}

/** @brief Dostupnost Rewind (overlay_action_reason_fn); pravidla jako cw_actions() okna Virtual CMT. */
static const char *reason_cmt_rewind(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_media(v);
    if (r) return r;
    if (!v->cmt.has_tape) return _("No tape inserted.");
    if (v->cmt.block_count == 0) return _("The tape is empty.");
    if (cmt_recording(v)) return _("Not available while recording.");
    if (v->cmt.first_playable < 0) return _("The tape has no playable block.");
    return NULL;
}

/**
 * @brief Dostupnost Play (overlay_action_reason_fn).
 *
 * Okno Virtual CMT má jedno tlačítko Play / Pause; overlay má podle
 * konceptu dvě. Play platí tam, kde okno ukazuje "Play": páska stojí,
 * je v pauze uživatele, nebo stojí nahrávání.
 */
static const char *reason_cmt_play(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_media(v);
    if (r) return r;
    const bool rec = cmt_recording(v);
    if (v->cmt.mode == OVERLAY_CMT_MODE_DIRECT && !rec) return _("Not used in Direct mode");
    if (!v->cmt.has_tape) return _("No tape inserted.");
    if (rec) return v->cmt.paused ? NULL : _("Not available while recording.");
    if (v->cmt.block_count == 0) return _("The tape is empty.");
    if (v->cmt.state == OVERLAY_CMT_PLAY && !v->cmt.user_paused) return _("The tape is already playing.");
    return NULL;
}

/** @brief Dostupnost Pause (overlay_action_reason_fn): jen při přehrávání bez pauzy uživatele. */
static const char *reason_cmt_pause(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_media(v);
    if (r) return r;
    if (v->cmt.mode == OVERLAY_CMT_MODE_DIRECT) return _("Not used in Direct mode");
    if (!v->cmt.has_tape) return _("No tape inserted.");
    if (v->cmt.state != OVERLAY_CMT_PLAY || v->cmt.user_paused) return _("The tape is not playing.");
    return NULL;
}

/** @brief Dostupnost Stop (overlay_action_reason_fn). */
static const char *reason_cmt_stop(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_media(v);
    if (r) return r;
    if (!v->cmt.has_tape) return _("No tape inserted.");
    if (v->cmt.state == OVERLAY_CMT_STOP) return (v->cmt.mode == OVERLAY_CMT_MODE_DIRECT) ? _("Not used in Direct mode") : _("The tape is already stopped.");
    return NULL;
}

/** @brief Dostupnost Record / Stop recording (overlay_action_reason_fn). */
static const char *reason_cmt_record(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_media(v);
    if (r) return r;
    if (cmt_recording(v)) return NULL;
    return (v->cmt.state == OVERLAY_CMT_PLAY) ? _("Stop the tape first.") : NULL;
}

/**
 * @brief Dostupnost přepnutí režimu načítání (overlay_action_reason_fn; arg = en_OVERLAY_CMT_MODE).
 *
 * Změna do režimu Direct nebo z něj jde jen se stojící páskou; Direct
 * navíc vyžaduje ROM s přímým načtením a blok s MZF. Právě zvolený režim
 * je vždy "dostupný" (jeho zvolení nic nedělá).
 */
static const char *reason_cmt_mode(const st_OVERLAY_VIEW *v, int arg)
{
    const char *r = reason_media(v);
    if (r) return r;
    if ((int)v->cmt.mode == arg) return NULL;
    const char *r_hack = (v->cmt.state == OVERLAY_CMT_RECORD) ? _("Stop the recording first.") : ((v->cmt.state != OVERLAY_CMT_STOP) ? _("Stop the tape first.") : NULL);
    if (v->cmt.mode == OVERLAY_CMT_MODE_DIRECT) return r_hack;
    if (arg != OVERLAY_CMT_MODE_DIRECT) return NULL;
    if (!v->cmt.direct_supported) return cmt_view_direct_unsupported_reason();
    if (r_hack) return r_hack;
    return v->cmt.current_has_mzf ? NULL : _("The current block does not contain an MZF file.");
}

/** @brief Dostupnost otevření jiné pásky (overlay_action_reason_fn); jako cw_reason_open() okna Virtual CMT. */
static const char *reason_cmt_change(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_media(v);
    if (r) return r;
    return cmt_recording(v) ? _("Stop the recording first.") : NULL;
}

/** @brief Dostupnost vysunutí pásky (overlay_action_reason_fn); na nahrávání a neuložené změny se ptá automat akcí s páskou. */
static const char *reason_cmt_eject(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    const char *r = reason_media(v);
    if (r) return r;
    return v->cmt.has_tape ? NULL : _("No tape inserted.");
}

/** @brief Dostupnost Quick Load (overlay_action_reason_fn): ne při záznamu ani přehrávání vstupů, jako klasické menu. */
static const char *reason_quick_load(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return v->replay_active ? _("Not available while a replay is active.") : NULL;
}

/** @brief Dostupnost akcí zakázaných při záznamu nebo přehrávání vstupů jako klasické menu: načtení snapshotu, výběr ROM (overlay_action_reason_fn). */
static const char *reason_replay(const st_OVERLAY_VIEW *v, int arg)
{
    return reason_quick_load(v, arg);
}

/**
 * @brief Dostupnost volby vlastní rychlosti (overlay_action_reason_fn).
 *
 * Volba je dostupná, je-li vlastní rychlost nastavená, nebo má-li ji
 * overlay odkud vrátit (overlay_action_custom_speed_restore_value()).
 * Důvod zákazu tedy platí jen tehdy, když není zapamatovaná žádná vlastní
 * rychlost (ani v jádře, ani v overlayi).
 */
static const char *reason_speed_custom(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return v->machine.custom_speed_available ? NULL : _("Set a custom speed with the slider first.");
}

/** @brief Dostupnost intenzity scanlines (overlay_action_reason_fn): jen při zapnutých scanlines, jako klasické menu. */
static const char *reason_scanlines_intensity(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return v->display.scanlines ? NULL : _("Enable scanlines first.");
}

/** @brief Dostupnost vlastní snímkové frekvence (overlay_action_reason_fn): jen v režimu Custom FPS, jako klasické menu. */
static const char *reason_custom_fps(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return (v->display.framerate_mode == (int)DISPLAY_FRAMERATE_MODE_CUSTOM) ? NULL : _("Select Custom FPS first.");
}

/* ================================================================
 * Funkce akcí
 * ================================================================ */

/** @brief Resume: nic nevolá; zavření a rozběhnutí dělá overlay_actions_run_pending() podle OVL_ACTION_CLOSE_RESUME. */
static void run_resume(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
}

/** @brief Speed: přepne maximální rychlost jako Alt+M (global_shortcuts.cpp) a položka "Max CPU Speed". */
static void run_speed(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    emulator_max_speed(!EMULATOR_TEST_MAX_SPEED);
}

/** @brief Quick Save jako Alt+F8 a položka menu Snapshot. */
static void run_quick_save(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    snapshot_ui_request_quicksave();
}

/** @brief Quick Load jako Alt+F9 a položka menu Snapshot (dotaz na pásku a pauzu si řídí sám). */
static void run_quick_load(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    snapshot_ui_request_quickload();
}

/** @brief Screenshot jako Alt+F a položka "Save Screenshot". */
static void run_screenshot(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    screenshot_ui_save_quick();
}

/** @brief Fullscreen jako Alt+Ctrl+Enter (global_shortcuts.cpp). */
static void run_fullscreen(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_iface_video_callbacks && g_iface_video_callbacks->switch_fullscreen) g_iface_video_callbacks->switch_fullscreen();
}

/** @brief Reset jako F12 a položka "Reset" (stav pauzy reset nemění). */
static void run_reset(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    mzarch_request_user_reset();
}

/**
 * @brief Vložení nebo výměna diskety jako Alt+1 až Alt+4 a položka "Mount..." (arg = mechanika).
 * @pre Volající nedrží device_state_lock() (funkce si ho bere na dobu volání fdc_ui_mount()).
 */
static void run_fdd_insert(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
#if CFG_HWEXT_HAVE_FDC
    /* čte stav mechaniky (připojení, cesta posledního obrazu): pod zámkem jako klasické menu; jen otevře dialog, na emu vlákno nečeká */
    device_state_lock();
    fdc_ui_mount(&g_fdc[FDC0], (unsigned)arg);
    device_state_unlock();
#else
    (void)arg;
#endif
}

/** @brief Vysunutí diskety jako Alt+Shift+1 až 4 a položka "Umount" (arg = mechanika). */
static void run_fdd_eject(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
#if CFG_HWEXT_HAVE_FDC
    fdc_request_umount(FDC0, (unsigned)arg);
#else
    (void)arg;
#endif
}

/** @brief Uložení změn diskety jako položka "Sync now" (arg = mechanika). */
static void run_fdd_save(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
#if CFG_HWEXT_HAVE_FDC
    fdc_request_sync(FDC0, (unsigned)arg);
#else
    (void)arg;
#endif
}

/** @brief Přepnutí volby "Read-only" mechaniky jako položka klasického menu (arg = mechanika); nasazený obraz znovu otevře emu vlákno. */
static void run_fdd_wp(const st_OVERLAY_VIEW *v, int arg)
{
#if CFG_HWEXT_HAVE_FDC
    /* vypnutí ruší volbu uživatele i ochranu kvůli sdílení (fdc_drive_set_write_protect()) */
    fdc_request_set_readonly(FDC0, (unsigned)arg, overlay_fdd_wp_on(&v->fdd[arg]) ? 0 : 1);
#else
    (void)v;
    (void)arg;
#endif
}

/** @brief Browse files...: dialog obrazu diskety pro první prázdnou mechaniku, jinak FDD 0. */
static void run_browse(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    run_fdd_insert(v, browse_target(v));
}

/** @brief Dostupnost akce "Clear recent media" (overlay_action_reason_fn; arg se nepoužívá): jen s neprázdnou historií. */
static const char *reason_recent_clear(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return (v->recent_count > 0) ? NULL : "";
}

/** @brief Clear recent media: vyprázdní historii všech druhů; vložená média se nemění. */
static void run_recent_clear(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    recent_media_clear();
    overlay_view_recent_recheck();
}

/**
 * @brief Vložení Quick Disku jako položky "Mount Image..." / "Mount Directory...".
 * @pre Volající nedrží device_state_lock() (funkce si ho bere na dobu volání qdisk_ui_mount()).
 */
static void run_qd_insert(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
#if CFG_HWEXT_HAVE_QDISK
    /* čte typ a poslední cestu Quick Disku: pod zámkem jako klasické menu; jen otevře dialog, na emu vlákno nečeká */
    device_state_lock();
    qdisk_ui_mount();
    device_state_unlock();
#endif
}

/** @brief Vysunutí Quick Disku jako položka "Unmount". */
static void run_qd_eject(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
#if CFG_HWEXT_HAVE_QDISK
    qdisk_request_umount();
#endif
}

/** @brief Uložení změn Quick Disku jako položka "Save now". */
static void run_qd_save(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
#if CFG_HWEXT_HAVE_QDISK
    qdisk_request_sync();
#endif
}

/** @brief Přepnutí volby "Write Protected" Quick Disku jako položka klasického menu. */
static void run_qd_wp(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
#if CFG_HWEXT_HAVE_QDISK
    qdisk_request_set_write_protected(v->qd.write_protected ? 0 : 1);
#else
    (void)v;
#endif
}

/** @brief Rewind jako okno Virtual CMT (cw_do_rewind()): REPLAY_CMT_SEEK na první přehratelný blok, jen nad páskou kopie. */
static void run_cmt_rewind(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    st_REPLAY_CMT_GUARD g;
    g.flags = REPLAY_CMT_GUARD_TAPE;
    g.tape_serial = v->cmt.tape_serial;
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, v->cmt.first_playable, &g);
}

/**
 * @brief Play jako okno Virtual CMT (cw_do_play_pause()).
 *
 * Ze STOP REPLAY_CMT_PLAY; při nahrávání REPLAY_CMT_PAUSE 0 s podmínkou
 * REPLAY_CMT_GUARD_RECORD; jinak (pauza uživatele) REPLAY_CMT_PAUSE 0.
 */
static void run_cmt_play(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    if (v->cmt.state == OVERLAY_CMT_STOP) {
        replay_host_cmt(REPLAY_CMT_PLAY, 0, NULL);
    } else if (v->cmt.state == OVERLAY_CMT_RECORD) {
        st_REPLAY_CMT_GUARD g;
        g.flags = REPLAY_CMT_GUARD_RECORD;
        g.tape_serial = 0u;
        replay_host_cmt_guarded(REPLAY_CMT_PAUSE, 0, &g);
    } else {
        replay_host_cmt(REPLAY_CMT_PAUSE, 0, NULL);
    }
}

/** @brief Pause jako okno Virtual CMT (cw_do_play_pause()): REPLAY_CMT_PAUSE 1 s podmínkou REPLAY_CMT_GUARD_NOT_STOP. */
static void run_cmt_pause(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    st_REPLAY_CMT_GUARD g;
    g.flags = REPLAY_CMT_GUARD_NOT_STOP;
    g.tape_serial = 0u;
    replay_host_cmt_guarded(REPLAY_CMT_PAUSE, 1, &g);
}

/** @brief Stop jako okno Virtual CMT: REPLAY_CMT_STOP_BUTTON. */
static void run_cmt_stop(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
}

/** @brief Record jako okno Virtual CMT (cw_do_record()): při nahrávání Stop, jinak REPLAY_CMT_RECORD bez cesty. */
static void run_cmt_record(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    if (cmt_recording(v)) replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    else replay_host_cmt(REPLAY_CMT_RECORD, 0, NULL);
}

/** @brief Režim načítání jako přepínač Loading okna Virtual CMT (arg = en_OVERLAY_CMT_MODE). */
static void run_cmt_mode(const st_OVERLAY_VIEW *v, int arg)
{
    if ((int)v->cmt.mode == arg) return;
    cmt_request_set_loading_mode((arg == OVERLAY_CMT_MODE_DIRECT) ? CMT_LOADING_MODE_DIRECT
                                                                   : ((arg == OVERLAY_CMT_MODE_BOOST) ? CMT_LOADING_MODE_BOOST : CMT_LOADING_MODE_TAPE));
}

/** @brief Change tape jako Tape > Open... okna Virtual CMT (dotaz na neuložené změny a nahrávání dělá automat akcí s páskou). */
static void run_cmt_change(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_OPEN);
}

/** @brief Eject jako Tape > Eject okna Virtual CMT (dotaz na nahrávání a neuložené změny dělá automat akcí s páskou). */
static void run_cmt_eject(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT);
}

/** @brief Open tape window: ukáže okno Virtual CMT (příznak jako položka "Show Virtual CMT" a Alt+C). */
static void run_cmt_window(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showVirtualCmtWindow = true;
}

/**
 * @brief Přepnutí pauzy jako Alt+P a položka "Pause Emulation".
 *
 * Stav emulace po zavření overlaye pak určil uživatel: pauza přestává být
 * pauzou overlaye a zavření nic nerozbíhá (emulace rozběhnutá odtud běží
 * pod menu dál).
 */
static void run_pause_toggle(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    (void)dbg_ui_pause_toggle();
    imgui_overlay_note_pause_changed(true);
}

/**
 * @brief Výběr ROM jako položka menu ROM (arg = index řádku g_rom_config->roms).
 *
 * Vybere jen řádek, který je v nabídce kopie stavu (povolená ROM, vývojová
 * jen v Development Mode); jiný index se zahodí.
 */
static void run_rom_select(const st_OVERLAY_VIEW *v, int arg)
{
    bool offered = false;
    for (unsigned i = 0; i < v->machine.rom_count; i++)
        if (!v->machine.rom[i].section && v->machine.rom[i].row == arg) offered = true;
    if (!offered || !g_rom_config || !g_rom_config->roms) return;
    /* tabulka konfigurace ROM je neměnná; index ověřit proti jejímu konci */
    int rows = 0;
    while (g_rom_config->roms[rows].type != ROM_CONFIG_TYPE_NONE) rows++;
    if (arg < 0 || arg >= rows) return;
    rom_request_select(&g_rom_config->roms[arg]);
}

/**
 * @brief Vlastní rychlost požadovaná těsně před poslední volbou "Normal" v overlayi; 0 = žádná.
 *
 * Jádro si při přepnutí na normální rychlost pamatuje rychlost uplatněnou
 * (customspeed_store_speed()), ne požadovanou; hodnota nastavená při stojící
 * emulaci by se volbou "Normal" ztratila. Platí jen spolu s
 * s_custom_speed_memory_prev.
 */
static unsigned s_custom_speed_memory = 0;

/**
 * @brief Hodnota previous_speed_in_percentage jádra hned po volbě "Normal", při které vznikla s_custom_speed_memory.
 *
 * Změní-li ji mezitím někdo jiný (klasické menu, zkratka mimo overlay, MCP),
 * je paměť jádra novější a paměť overlaye se nepoužije.
 */
static unsigned s_custom_speed_memory_prev = 0;

int overlay_action_custom_speed_restore_value(void)
{
    const unsigned prev = g_customspeed.previous_speed_in_percentage;
    if (s_custom_speed_memory != 0 && s_custom_speed_memory_prev == prev) return (int)s_custom_speed_memory;
    return (prev != 100) ? (int)prev : 0;
}

/**
 * @brief Normální rychlost jako Alt+N a položka "Normal CPU Speed".
 *
 * Před přepnutím si zapamatuje požadovanou vlastní rychlost, aby ji volba
 * "Custom" uměla vrátit i při stojící emulaci (s_custom_speed_memory).
 */
static void run_speed_normal(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    const unsigned requested = g_customspeed.speed_in_percentage_requested;
    emulator_switch_to_normal_speed();
    if (requested != 100) {
        s_custom_speed_memory = requested;
        s_custom_speed_memory_prev = g_customspeed.previous_speed_in_percentage;
    }
}

/**
 * @brief Vlastní rychlost jako položka "Custom CPU Speed".
 *
 * Volá emulator_switch_to_custom_speed() jako klasické menu. Ta vlastní
 * rychlost obnoví jen při uplatněných 100 % a jen z rychlosti uplatněné před
 * volbou "Normal"; kde by nic neobnovila nebo obnovila starší hodnotu než
 * tu, kterou uživatel nastavil při stojící emulaci, nastaví overlay
 * zapamatovanou rychlost funkcí customspeed_set_request() (funkce posuvníku
 * klasického menu). Je-li vlastní rychlost už nastavená (návrat z Max),
 * nic nepřepisuje.
 */
static void run_speed_custom(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    const bool was_normal = g_customspeed.speed_in_percentage_requested == 100;
    const int want = overlay_action_custom_speed_restore_value();
    emulator_switch_to_custom_speed();
    if (was_normal && want > 0 && g_customspeed.speed_in_percentage_requested != (unsigned)want) customspeed_set_request(want);
}

/** @brief Nastavení vlastní rychlosti jako posuvník "Custom Speed Options" (arg = procenta; rozsah omezí customspeed_set_request()). */
static void run_speed_set(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    customspeed_set_request(arg);
}

/** @brief Zpomalení jako položka "Decrease - step 10 %" (arg = krok v procentech). */
static void run_speed_down(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    customspeed_step_down_request(arg);
}

/** @brief Zrychlení jako položka "Increase - step 10 %" (arg = krok v procentech). */
static void run_speed_up(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    customspeed_step_up_request(arg);
}

/** @brief Save Snapshot... jako Alt+F6 a položka menu Snapshot (emulace už stojí, po uložení zůstane stát). */
static void run_snapshot_save(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    snapshot_ui_request_save();
}

/** @brief Load Snapshot... jako Alt+F7 a položka menu Snapshot (dotaz na pásku, pauzu a dialog si řídí sám). */
static void run_snapshot_load(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    snapshot_ui_request_load();
}

/** @brief Snapshot Setup...: ukáže okno Snapshot Settings (příznak jako položka klasického menu). */
static void run_snapshot_setup(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showSnapshotSetupWindow = true;
}

/** @brief Open classic menu: požádá hlavní okno o otevření klasického menu; overlay se potom zavře. */
static void run_classic_menu(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    imgui_overlay_request_classic_menu();
}

/** @brief Velikost okna při startu jako položky "Window Size on Startup" (arg = en_DISPLAY_STARTUP_SIZE). */
static void run_startup_size(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    if (arg < 0 || arg >= (int)DISPLAY_STARTUP_SIZES_COUNT) return;
    display_set_window_startup_scale((en_DISPLAY_STARTUP_SIZE)arg);
}

/** @brief Přepnutí "Lock Window Aspect Ratio" jako položka klasického menu. */
static void run_lock_aspect(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    display_set_locked_aspect_ratio(v->display.locked_aspect ? 0 : 1);
}

/** @brief Srovnání poměru stran okna jako Alt+W / Alt+H (arg = 'W' nebo 'H'). */
static void run_fix_aspect(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    if (g_iface_video_callbacks && g_iface_video_callbacks->fix_window_aspect_ratio) g_iface_video_callbacks->fix_window_aspect_ratio((char)arg);
}

/** @brief Barevné schéma jako položky "Color Schema" (arg = en_DISPLAY_COLOR_SCHEMA). */
static void run_colors(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    if (arg < 0 || arg >= (int)DISPLAY_COLORS_COUNT) return;
    display_set_colors((en_DISPLAY_COLOR_SCHEMA)arg);
}

/** @brief Přepnutí "Enable Scanlines" jako položka klasického menu. */
static void run_scanlines(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    display_set_scanlines(v->display.scanlines ? 0 : 1);
}

/** @brief Intenzita scanlines jako posuvník "Intensity" (arg = procenta; rozsah omezí display_set_scanlines_intensity()). */
static void run_scanlines_intensity(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    display_set_scanlines_intensity(arg);
}

/** @brief Řízení snímkové frekvence jako položky "Frame Rate Control" (arg = en_DISPLAY_FRAMERATE_MODE). */
static void run_framerate(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    if (arg < 0 || arg >= (int)DISPLAY_FRAMERATE_MODE_COUNT) return;
    display_set_framerate_mode((en_DISPLAY_FRAMERATE_MODE)arg);
}

/** @brief Vlastní snímková frekvence jako posuvník " Hz" klasického menu (arg = Hz, omezí se na DISPLAY_CUSTOM_FPS_MIN až _MAX). */
static void run_custom_fps(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    if (arg < DISPLAY_CUSTOM_FPS_MIN) arg = DISPLAY_CUSTOM_FPS_MIN;
    if (arg > DISPLAY_CUSTOM_FPS_MAX) arg = DISPLAY_CUSTOM_FPS_MAX;
    g_display.custom_fps = arg;
    display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_CUSTOM);
}

/** @brief Přepnutí "Forced Redraw Every Screen" jako položka klasického menu. */
static void run_forced_redraw(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    display_set_forced_redrawing(v->display.forced_redraw ? 0 : 1);
}

/* ================================================================
 * Tabulka
 * ================================================================ */

/* ================================================================
 * Taby Sound, Input a Capture
 * ================================================================ */

/**
 * @brief Dostupnost přepínače Listen (overlay_action_reason_fn).
 * @return NULL = dostupný; jinak přeložený důvod: emulace stojí a pauza není
 *         overlaye, nebo emulace běží z jiného důvodu než kvůli poslechu.
 */
static const char *reason_listen(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    if (v->sound.listen == (int)OVERLAY_LISTEN_BLOCKED_PAUSED) return _("The emulation was paused before the menu was opened. Resume it to listen.");
    if (v->sound.listen == (int)OVERLAY_LISTEN_BLOCKED_RUNNING) return _("The emulation is running.");
    return NULL;
}

/**
 * @brief Přepnutí poslechu na tabu Sound (přepínač Listen).
 *
 * Jen předá žádost rámci overlaye; emulaci rozběhne nebo zastaví příští
 * imgui_overlay_frame() podle vlastnictví pauzy.
 */
static void run_listen(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    imgui_overlay_set_listen(v->sound.listen != (int)OVERLAY_LISTEN_ON);
}

/** @brief Hlavní hlasitost poslechu (arg = procenta; rozsah omezí iface_audio_set_master_gain()). */
static void run_master_volume(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    iface_audio_set_master_gain((float)arg / 100.0f);
}

/** @brief Přepnutí ztlumení poslechu. */
static void run_mute(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    iface_audio_set_mute(!v->sound.mute);
}

/** @brief Dostupnost volby "Hardware output filter" (overlay_action_reason_fn): jen na stroji, který propust má. */
static const char *reason_output_filter(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return v->sound.has_output_filter ? NULL : "";
}

/** @brief Přepnutí věrné výstupní propusti jako volba "Hardware output filter" okna Audio Mixer. */
static void run_output_filter(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    iface_audio_set_output_filter(!v->sound.output_filter);
}

/**
 * @brief Hlasitost kanálu jako posuvník okna Audio Mixer (arg = OVL_CHANNEL_VOLUME_VALUE()).
 *
 * Okno Audio Mixer zapisuje g_iface_audio.gain[] přímo z UI vlákna
 * (audio_window.cpp); funkce pro to rozhraní nemá. Kanál mimo kanály
 * kopie se ignoruje, procenta se omezí na 0 až 100.
 */
static void run_channel_volume(const st_OVERLAY_VIEW *v, int arg)
{
    if (arg < 0) return;
    const int ch = arg / 1000;
    int pct = arg % 1000;
    if (ch >= (int)v->sound.channel_count || ch >= AUDIO_SRC_CHANNELS_COUNT) return;
    if (pct > 100) pct = 100;
    g_iface_audio.gain[ch] = (float)pct / 100.0f;
}

/** @brief Reset channels: hlasitosti všech kanálů na výchozích 100 % (i kanálů druhého PSG, které okno právě neukazuje). */
static void run_channels_reset(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) g_iface_audio.gain[i] = 1.0f;
}

/** @brief Virtuální klávesnice jako Alt+K: ukáže okno; overlay se potom zavře. */
static void run_vkbd(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showVirtualKeyboardWindow = true;
}

/** @brief Autotype Buffer: ukáže okno; overlay se potom zavře. */
static void run_autotype(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showAutotypeWindow = true;
}

/** @brief Joystick Setup...: ukáže okno; overlay se potom zavře. */
static void run_joy_setup(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showJoystickSetupWindow = true;
}

/** @brief Jen pro testy: počet nepovedených otevření zařízení joysticku od posledního vyzvednutí. */
static int s_test_joy_open_failures = 0;

int overlay_actions_test_take_joy_open_failures(void)
{
    const int n = s_test_joy_open_failures;
    s_test_joy_open_failures = 0;
    return n;
}

/**
 * @brief Dostupnost volby zařízení joysticku (overlay_action_reason_fn).
 *
 * Při záznamu nebo přehrávání vstupů změnu typu emu vlákno odmítne
 * (joy_setup_window.cpp), proto je volba zakázaná celá.
 */
static const char *reason_joy_device(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return v->replay_active ? _("Not available while a replay is active.") : NULL;
}

/**
 * @brief Zařízení prvního portu joysticku jako "Apply" okna Joystick Setup (arg = en_OVERLAY_JOY_TYPE).
 *
 * Typ změní emu vlákno (joy_request_set_type()). Volba "Joystick" otevře
 * zařízení hostitele: to, které port už má, je-li v seznamu dostupných,
 * jinak první dostupné s výchozím mapováním (okno Joystick Setup dává
 * výběr zařízení a mapování). Bez dostupného joysticku se volba ignoruje.
 * Když se zařízení otevřít nepovede, typ portu zůstane "Joystick" bez
 * zařízení a uživatel dostane oznámení (baseui_show_message()).
 * Odchod z volby "Joystick" zařízení zavře.
 *
 * @pre UI vlákno (zařízení SDL otevírá a zavírá UI vlákno pod zámkem iface_joy).
 */
static void run_joy_device(const st_OVERLAY_VIEW *v, int arg)
{
    if (arg < 0 || arg >= (int)OVERLAY_JOY_TYPE_COUNT) return;
    const en_JOY_TYPE new_type = (en_JOY_TYPE)arg;
    const en_JOY_TYPE old_type = (en_JOY_TYPE)v->input.joy_type;
    if (new_type == old_type) return;
    if (new_type == JOY_TYPE_JOYSTICK && g_iface_joy.available_count <= 0) return;
    joy_request_set_type(JOY_DEVID_0, new_type);
    if (new_type == JOY_TYPE_JOYSTICK) {
        int idx = -1;
        for (int j = 0; j < g_iface_joy.available_count; j++) {
            if (g_iface_joy.available[j].instance_id == g_iface_joy.dev[JOY_DEVID_0].sysdevice.instance_id) {
                idx = j;
                break;
            }
        }
        st_IFACE_JOY_PARAMS params = g_iface_joy.dev[JOY_DEVID_0].params;
        if (idx < 0) {
            idx = 0;
            iface_joy_set_default_params(&params);
        }
        if (iface_joy_open_sysdevice(JOY_DEVID_0, &g_iface_joy.available[idx]) != EXIT_SUCCESS) {
            /* port zůstal bez zařízení: oznámit, jinak by volba vypadala zapnutá a joystick nereagoval (review O6 Z6) */
            s_test_joy_open_failures++;
            baseui_show_message(true, (char *)"%s", _("The joystick device could not be opened."));
        }
        iface_joy_set_params(JOY_DEVID_0, &params);
    } else if (old_type == JOY_TYPE_JOYSTICK) {
        iface_joy_close_device(JOY_DEVID_0);
    }
}

/** @brief Save Screenshot As...: dialog souboru jako položka klasického menu. */
static void run_screenshot_as(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    screenshot_ui_save_as();
}

/** @brief Record Changed Frames jako Alt+Shift+F. */
static void run_series(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    screenshot_ui_toggle_series();
}

/** @brief Open Output Folder karty Screenshot. */
static void run_screenshot_folder(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    screenshot_ui_open_output_folder();
}

/** @brief Settings... karty Screenshot: ukáže okno nastavení; overlay se potom zavře. */
static void run_screenshot_setup(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showScreenshotSetupWindow = true;
}

/** @brief Start / Stop Recording jako Alt+O. */
static void run_video_toggle(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    videorec_ui_toggle_recording();
}

/** @brief Dostupnost "Pause Recording" (overlay_action_reason_fn): jen při nahrávání, jako klasické menu. */
static const char *reason_video_pause(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return v->capture.video_pause_enabled ? NULL : _("No video recording is running.");
}

/** @brief Pause Recording jako Alt+Shift+O. */
static void run_video_pause(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    videorec_ui_toggle_pause();
}

/** @brief Record in Real Time jako Alt+U. */
static void run_video_realtime(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    videorec_ui_toggle_timebase();
}

/** @brief Open Output Folder karty Video Recording. */
static void run_video_folder(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    videorec_ui_open_output_folder();
}

/** @brief Settings... karty Video Recording: ukáže okno nastavení; overlay se potom zavře. */
static void run_video_setup(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showVideorecSetupWindow = true;
}

/** @brief Dostupnost "Record Inputs..." a "Play Replay..." (overlay_action_reason_fn): jen při vypnutém replay, jako klasické menu. */
static const char *reason_replay_start(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return (v->capture.replay_mode == (int)OVERLAY_REPLAY_OFF) ? NULL : _("A replay is already active.");
}

/** @brief Dostupnost "Stop Replay" (overlay_action_reason_fn): jen při běžícím záznamu nebo přehrávání, jako klasické menu. */
static const char *reason_replay_stop(const st_OVERLAY_VIEW *v, int arg)
{
    (void)arg;
    return (v->capture.replay_mode != (int)OVERLAY_REPLAY_OFF) ? NULL : _("No replay is active.");
}

/** @brief Record Inputs...: dialog startu záznamu jako položka klasického menu (overlay je už zavřený). */
static void run_replay_record(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    imgui_replay_open_record_dialog();
}

/** @brief Play Replay...: dotaz pásky a dialog přehrání jako položka klasického menu (overlay je už zavřený). */
static void run_replay_play(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    imgui_replay_open_play_dialog();
}

/** @brief Stop Replay jako položka klasického menu: požadavek a ohlášení jeho výsledku. */
static void run_replay_stop(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    imgui_replay_request(replay_request_stop());
}

/** @brief Replay Control... jako Alt+J: ukáže okno; overlay se potom zavře. */
static void run_replay_window(const st_OVERLAY_VIEW *v, int arg)
{
    (void)v;
    (void)arg;
    if (g_gui) g_gui->showReplayWindow = true;
}

/** @brief Zkratka Alt + klávesa. */
#define OVL_ALT(key) ((ImGuiKeyChord)(ImGuiMod_Alt | (key)))
/** @brief Zkratka Alt + Shift + klávesa. */
#define OVL_ALT_SHIFT(key) ((ImGuiKeyChord)(ImGuiMod_Alt | ImGuiMod_Shift | (key)))

/**
 * @brief Tabulka akcí v pořadí en_OVL_ACTION.
 *
 * Zkratky odpovídají obsluze v imgui_global_shortcuts(): mechaniky FDD 0
 * až 3 jsou na klávesách 1 až 4 (Alt = vložit, Alt+Shift = vysunout).
 */
static const st_OVL_ACTION_DEF s_actions[OVL_ACTION_COUNT] = {
    {OVL_ACTION_NONE, NULL, NULL, 0, NULL, NULL, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_RESUME, N_("Resume"), ICON_LC_PLAY, 0, NULL, run_resume, 0, NULL, OVL_ACTION_CLOSE_RESUME},
    {OVL_ACTION_SPEED, N_("Speed"), ICON_LC_GAUGE, OVL_ALT(ImGuiKey_M), "Alt+M", run_speed, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_QUICK_SAVE, N_("Quick Save"), ICON_LC_DOWNLOAD, OVL_ALT(ImGuiKey_F8), "Alt+F8", run_quick_save, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_QUICK_LOAD, N_("Quick Load"), ICON_LC_UPLOAD, OVL_ALT(ImGuiKey_F9), "Alt+F9", run_quick_load, 0, reason_quick_load, OVL_ACTION_CLOSE_BEFORE},
    {OVL_ACTION_SCREENSHOT, N_("Screenshot"), ICON_LC_CAMERA, OVL_ALT(ImGuiKey_F), "Alt+F", run_screenshot, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FULLSCREEN, N_("Fullscreen"), ICON_LC_MAXIMIZE, (ImGuiKeyChord)(ImGuiMod_Alt | ImGuiMod_Ctrl | ImGuiKey_Enter), "Alt+Ctrl+Enter", run_fullscreen, 0, NULL,
     OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_RESET, N_("Reset"), ICON_LC_ROTATE_CW, (ImGuiKeyChord)ImGuiKey_F12, "F12", run_reset, 0, NULL, OVL_ACTION_CLOSE_AFTER},

    {OVL_ACTION_FDD_INSERT_0, N_("Insert disk"), NULL, OVL_ALT(ImGuiKey_1), "Alt+1", run_fdd_insert, 0, reason_fdd_insert, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_INSERT_1, N_("Insert disk"), NULL, OVL_ALT(ImGuiKey_2), "Alt+2", run_fdd_insert, 1, reason_fdd_insert, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_INSERT_2, N_("Insert disk"), NULL, OVL_ALT(ImGuiKey_3), "Alt+3", run_fdd_insert, 2, reason_fdd_insert, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_INSERT_3, N_("Insert disk"), NULL, OVL_ALT(ImGuiKey_4), "Alt+4", run_fdd_insert, 3, reason_fdd_insert, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_EJECT_0, N_("Eject"), OVL_ICON_EJECT, OVL_ALT_SHIFT(ImGuiKey_1), "Alt+Shift+1", run_fdd_eject, 0, reason_fdd_eject, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_EJECT_1, N_("Eject"), OVL_ICON_EJECT, OVL_ALT_SHIFT(ImGuiKey_2), "Alt+Shift+2", run_fdd_eject, 1, reason_fdd_eject, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_EJECT_2, N_("Eject"), OVL_ICON_EJECT, OVL_ALT_SHIFT(ImGuiKey_3), "Alt+Shift+3", run_fdd_eject, 2, reason_fdd_eject, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_EJECT_3, N_("Eject"), OVL_ICON_EJECT, OVL_ALT_SHIFT(ImGuiKey_4), "Alt+Shift+4", run_fdd_eject, 3, reason_fdd_eject, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_SAVE_0, N_("Save changes"), ICON_LC_SAVE, 0, NULL, run_fdd_save, 0, reason_fdd_save, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_SAVE_1, N_("Save changes"), ICON_LC_SAVE, 0, NULL, run_fdd_save, 1, reason_fdd_save, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_SAVE_2, N_("Save changes"), ICON_LC_SAVE, 0, NULL, run_fdd_save, 2, reason_fdd_save, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_SAVE_3, N_("Save changes"), ICON_LC_SAVE, 0, NULL, run_fdd_save, 3, reason_fdd_save, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_WP_0, N_("Write protect"), NULL, 0, NULL, run_fdd_wp, 0, reason_fdd_wp, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_WP_1, N_("Write protect"), NULL, 0, NULL, run_fdd_wp, 1, reason_fdd_wp, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_WP_2, N_("Write protect"), NULL, 0, NULL, run_fdd_wp, 2, reason_fdd_wp, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FDD_WP_3, N_("Write protect"), NULL, 0, NULL, run_fdd_wp, 3, reason_fdd_wp, OVL_ACTION_CLOSE_NO},

    {OVL_ACTION_CMT_REWIND, N_("Rewind"), ICON_LC_REWIND, 0, NULL, run_cmt_rewind, 0, reason_cmt_rewind, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_PLAY, N_("Play"), ICON_LC_PLAY, 0, NULL, run_cmt_play, 0, reason_cmt_play, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_PAUSE, N_("Pause"), ICON_LC_PAUSE, 0, NULL, run_cmt_pause, 0, reason_cmt_pause, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_STOP, N_("Stop"), ICON_LC_SQUARE, 0, NULL, run_cmt_stop, 0, reason_cmt_stop, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_RECORD, N_("Record"), ICON_LC_CIRCLE, 0, NULL, run_cmt_record, 0, reason_cmt_record, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_MODE_TAPE, N_("Tape"), NULL, 0, NULL, run_cmt_mode, OVERLAY_CMT_MODE_TAPE, reason_cmt_mode, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_MODE_BOOST, N_("Boost"), NULL, 0, NULL, run_cmt_mode, OVERLAY_CMT_MODE_BOOST, reason_cmt_mode, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_MODE_DIRECT, N_("Direct"), NULL, 0, NULL, run_cmt_mode, OVERLAY_CMT_MODE_DIRECT, reason_cmt_mode, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_CHANGE, N_("Change tape"), NULL, 0, NULL, run_cmt_change, 0, reason_cmt_change, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_EJECT, N_("Eject"), OVL_ICON_EJECT, 0, NULL, run_cmt_eject, 0, reason_cmt_eject, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CMT_WINDOW, N_("Open tape window"), NULL, OVL_ALT(ImGuiKey_C), "Alt+C", run_cmt_window, 0, NULL, OVL_ACTION_CLOSE_AFTER},

    {OVL_ACTION_QD_INSERT, N_("Insert disk"), NULL, 0, NULL, run_qd_insert, 0, reason_qd_insert, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_QD_EJECT, N_("Eject"), OVL_ICON_EJECT, 0, NULL, run_qd_eject, 0, reason_qd_eject, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_QD_SAVE, N_("Save changes"), ICON_LC_SAVE, 0, NULL, run_qd_save, 0, reason_qd_save, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_QD_WP, N_("Write protect"), NULL, 0, NULL, run_qd_wp, 0, reason_qd_wp, OVL_ACTION_CLOSE_NO},

    {OVL_ACTION_BROWSE_FILES, N_("Browse files..."), ICON_LC_FOLDER_OPEN, 0, NULL, run_browse, 0, reason_browse, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_RECENT_CLEAR, N_("Clear recent media"), NULL, 0, NULL, run_recent_clear, 0, reason_recent_clear, OVL_ACTION_CLOSE_NO},

    {OVL_ACTION_PAUSE_TOGGLE, N_("Pause emulation"), ICON_LC_PAUSE, OVL_ALT(ImGuiKey_P), "Alt+P", run_pause_toggle, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_ROM_SELECT, N_("ROM"), NULL, 0, NULL, run_rom_select, OVL_ACTION_ARG_VALUE, reason_replay, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SPEED_NORMAL, N_("Normal"), NULL, OVL_ALT(ImGuiKey_N), "Alt+N", run_speed_normal, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SPEED_CUSTOM, N_("Custom"), NULL, 0, NULL, run_speed_custom, 0, reason_speed_custom, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SPEED_SET, N_("Custom speed"), NULL, 0, NULL, run_speed_set, OVL_ACTION_ARG_VALUE, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SPEED_DOWN, N_("Decrease by 10 %"), ICON_LC_MINUS, 0, NULL, run_speed_down, 10, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SPEED_UP, N_("Increase by 10 %"), ICON_LC_PLUS, 0, NULL, run_speed_up, 10, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SNAPSHOT_SAVE, N_("Save Snapshot..."), NULL, OVL_ALT(ImGuiKey_F6), "Alt+F6", run_snapshot_save, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SNAPSHOT_LOAD, N_("Load Snapshot..."), NULL, OVL_ALT(ImGuiKey_F7), "Alt+F7", run_snapshot_load, 0, reason_replay, OVL_ACTION_CLOSE_BEFORE},
    {OVL_ACTION_SNAPSHOT_SETUP, N_("Snapshot Setup..."), ICON_LC_SETTINGS, 0, NULL, run_snapshot_setup, 0, NULL, OVL_ACTION_CLOSE_AFTER},
    {OVL_ACTION_CLASSIC_MENU, N_("Open classic menu"), ICON_LC_MENU, 0, NULL, run_classic_menu, 0, NULL, OVL_ACTION_CLOSE_AFTER},

    {OVL_ACTION_STARTUP_SIZE, N_("Window size on startup"), NULL, 0, NULL, run_startup_size, OVL_ACTION_ARG_VALUE, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_LOCK_ASPECT, N_("Lock window aspect ratio"), NULL, 0, NULL, run_lock_aspect, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FIX_ASPECT_W, N_("By width"), NULL, OVL_ALT(ImGuiKey_W), "Alt+W", run_fix_aspect, 'W', NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FIX_ASPECT_H, N_("By height"), NULL, OVL_ALT(ImGuiKey_H), "Alt+H", run_fix_aspect, 'H', NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_COLORS, N_("Color schema"), NULL, 0, NULL, run_colors, OVL_ACTION_ARG_VALUE, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SCANLINES, N_("Enable scanlines"), NULL, 0, NULL, run_scanlines, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SCANLINES_INTENSITY, N_("Intensity"), NULL, 0, NULL, run_scanlines_intensity, OVL_ACTION_ARG_VALUE, reason_scanlines_intensity, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FRAMERATE, N_("Frame rate control"), NULL, 0, NULL, run_framerate, OVL_ACTION_ARG_VALUE, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CUSTOM_FPS, N_("Custom FPS"), NULL, 0, NULL, run_custom_fps, OVL_ACTION_ARG_VALUE, reason_custom_fps, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_FORCED_REDRAW, N_("Forced redraw every screen"), NULL, 0, NULL, run_forced_redraw, 0, NULL, OVL_ACTION_CLOSE_NO},

    {OVL_ACTION_LISTEN, N_("Listen"), ICON_LC_HEADPHONES, 0, NULL, run_listen, 0, reason_listen, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_MASTER_VOLUME, N_("Master volume"), ICON_LC_VOLUME_2, 0, NULL, run_master_volume, OVL_ACTION_ARG_VALUE, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_MUTE, N_("Mute"), ICON_LC_VOLUME_X, 0, NULL, run_mute, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_OUTPUT_FILTER, N_("Hardware output filter"), NULL, 0, NULL, run_output_filter, 0, reason_output_filter, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CHANNEL_VOLUME, N_("Channel volume"), NULL, 0, NULL, run_channel_volume, OVL_ACTION_ARG_VALUE, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_CHANNELS_RESET, N_("Reset channels"), NULL, 0, NULL, run_channels_reset, 0, NULL, OVL_ACTION_CLOSE_NO},

    {OVL_ACTION_VKBD, N_("Virtual keyboard"), ICON_LC_KEYBOARD, OVL_ALT(ImGuiKey_K), "Alt+K", run_vkbd, 0, NULL, OVL_ACTION_CLOSE_AFTER},
    {OVL_ACTION_AUTOTYPE, N_("Autotype buffer"), NULL, 0, NULL, run_autotype, 0, NULL, OVL_ACTION_CLOSE_AFTER},
    {OVL_ACTION_JOY_DEVICE, N_("Device"), NULL, 0, NULL, run_joy_device, OVL_ACTION_ARG_VALUE, reason_joy_device, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_JOY_SETUP, N_("Joystick Setup..."), NULL, 0, NULL, run_joy_setup, 0, NULL, OVL_ACTION_CLOSE_AFTER},

    {OVL_ACTION_SCREENSHOT_AS, N_("Save Screenshot As..."), NULL, 0, NULL, run_screenshot_as, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SERIES, N_("Record Changed Frames"), NULL, OVL_ALT_SHIFT(ImGuiKey_F), "Alt+Shift+F", run_series, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SCREENSHOT_FOLDER, N_("Open Output Folder"), ICON_LC_FOLDER, 0, NULL, run_screenshot_folder, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_SCREENSHOT_SETUP, N_("Settings..."), ICON_LC_SETTINGS, 0, NULL, run_screenshot_setup, 0, NULL, OVL_ACTION_CLOSE_AFTER},
    {OVL_ACTION_VIDEO_TOGGLE, N_("Start Recording"), ICON_LC_CIRCLE, OVL_ALT(ImGuiKey_O), "Alt+O", run_video_toggle, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_VIDEO_PAUSE, N_("Pause Recording"), ICON_LC_PAUSE, OVL_ALT_SHIFT(ImGuiKey_O), "Alt+Shift+O", run_video_pause, 0, reason_video_pause, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_VIDEO_REALTIME, N_("Record in Real Time"), NULL, OVL_ALT(ImGuiKey_U), "Alt+U", run_video_realtime, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_VIDEO_FOLDER, N_("Open Output Folder"), ICON_LC_FOLDER, 0, NULL, run_video_folder, 0, NULL, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_VIDEO_SETUP, N_("Settings..."), ICON_LC_SETTINGS, 0, NULL, run_video_setup, 0, NULL, OVL_ACTION_CLOSE_AFTER},
    {OVL_ACTION_REPLAY_RECORD, N_("Record Inputs..."), NULL, 0, NULL, run_replay_record, 0, reason_replay_start, OVL_ACTION_CLOSE_BEFORE},
    {OVL_ACTION_REPLAY_PLAY, N_("Play Replay..."), NULL, 0, NULL, run_replay_play, 0, reason_replay_start, OVL_ACTION_CLOSE_BEFORE},
    {OVL_ACTION_REPLAY_STOP, N_("Stop Replay"), NULL, 0, NULL, run_replay_stop, 0, reason_replay_stop, OVL_ACTION_CLOSE_NO},
    {OVL_ACTION_REPLAY_WINDOW, N_("Replay Control..."), NULL, OVL_ALT(ImGuiKey_J), "Alt+J", run_replay_window, 0, NULL, OVL_ACTION_CLOSE_AFTER},
};

const st_OVL_ACTION_DEF *overlay_action_def(en_OVL_ACTION id)
{
    if ((int)id <= 0 || (int)id >= (int)OVL_ACTION_COUNT) return &s_actions[OVL_ACTION_NONE];
    return &s_actions[id];
}

const char *overlay_action_reason(en_OVL_ACTION id, const st_OVERLAY_VIEW *v)
{
    const st_OVL_ACTION_DEF *def = overlay_action_def(id);
    if (!v || !def->run) return "";
    return def->reason ? def->reason(v, def->arg) : NULL;
}

const char *overlay_action_keycap(en_OVL_ACTION id, const st_OVERLAY_VIEW *v)
{
    const st_OVL_ACTION_DEF *def = overlay_action_def(id);
    if (!def->keycap || overlay_action_reason(id, v)) return NULL;
    return def->keycap;
}

void overlay_action_request(en_OVL_ACTION id)
{
    if ((int)id <= 0 || (int)id >= (int)OVL_ACTION_COUNT) return;
    /* akce s hodnotou jde vyžádat jen s hodnotou */
    if (s_actions[id].arg == OVL_ACTION_ARG_VALUE) return;
    if (s_pending == OVL_ACTION_NONE) s_pending = id;
}

void overlay_action_request_value(en_OVL_ACTION id, int value)
{
    if ((int)id <= 0 || (int)id >= (int)OVL_ACTION_COUNT) return;
    if (s_actions[id].arg != OVL_ACTION_ARG_VALUE) return;
    if (s_pending != OVL_ACTION_NONE) return;
    s_pending = id;
    s_pending_value = value;
}

bool overlay_action_keycaps_fit(const en_OVL_ACTION *ids, const char *const *texts, const float *widths, int count)
{
    if (!ids || !widths || count <= 0) return false;
    for (int i = 0; i < count; i++) {
        const st_OVL_ACTION_DEF *def = overlay_action_def(ids[i]);
        if (!def->keycap) continue;
        const char *text = (texts && texts[i]) ? texts[i] : (def->label ? _(def->label) : "");
        if (ovl_button_natural_width(text, def->icon, def->keycap) > widths[i] + 0.5f) return false;
    }
    return true;
}

bool overlay_action_button(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, bool icon_only, en_OVL_BUTTON_KIND kind, ImVec2 size, const char *test_name)
{
    return overlay_action_button_ex(id, v, text, icon_only, kind, size, test_name, true);
}

bool overlay_action_button_ex(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, bool icon_only, en_OVL_BUTTON_KIND kind, ImVec2 size, const char *test_name,
                              bool show_keycap)
{
    const st_OVL_ACTION_DEF *def = overlay_action_def(id);
    const char *reason = overlay_action_reason(id, v);
    const char *keycap = overlay_action_keycap(id, v);
    if (!text) text = def->label ? _(def->label) : "";
    char label[384];
    snprintf(label, sizeof(label), "%s###ovl_action_%d", icon_only ? "" : text, (int)id);
    if (icon_only) show_keycap = false;

    if (reason) ImGui::BeginDisabled();
    bool pressed = ovl_button_ex(label, def->icon, show_keycap ? keycap : NULL, kind, size);
    if (reason) ImGui::EndDisabled();
    if (test_name) overlay_test_note_item(test_name);
    if (test_name && show_keycap && keycap) {
        /* štítek, který tlačítko kreslí u pravého okraje (ovl_button_ex() ho kreslí, jen když se vejde) */
        const ImVec2 kc = ovl_keycap_size(keycap);
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        if (ovl_button_natural_width(text, def->icon, keycap) <= mx.x - mn.x + 0.5f) {
            char kc_name[48];
            snprintf(kc_name, sizeof(kc_name), "%.36s_keycap", test_name);
            overlay_test_note_rect(kc_name, ImVec2(mx.x - kc.x, mn.y), mx);
        }
    }

    if (reason) {
        ovl_tooltip_disabled(reason);
        pressed = false;
    } else if (icon_only || (keycap && !show_keycap)) {
        char tip[384];
        if (keycap) snprintf(tip, sizeof(tip), "%s (%s)", text, keycap);
        else snprintf(tip, sizeof(tip), "%s", text);
        ovl_tooltip(tip);
    }
    if (pressed) overlay_action_request(id);
    return pressed;
}

bool overlay_action_is_pending(void)
{
    return s_pending != OVL_ACTION_NONE;
}

void overlay_actions_handle_shortcuts(const st_OVERLAY_VIEW *v)
{
    if (!v) return;
    const ImGuiIO &io = ImGui::GetIO();
    const ImGuiKeyChord mods = (ImGuiKeyChord)(io.KeyMods & ImGuiMod_Mask_);
    for (int i = 1; i < (int)OVL_ACTION_COUNT; i++) {
        const st_OVL_ACTION_DEF *def = &s_actions[i];
        if (!def->chord) continue;
        if ((def->chord & ImGuiMod_Mask_) != mods) continue;
        if (!ImGui::IsKeyPressed((ImGuiKey)(def->chord & ~ImGuiMod_Mask_), false)) continue;
        if (overlay_action_reason(def->id, v)) continue;
        overlay_action_request(def->id);
    }
}

void overlay_actions_run_pending(const st_OVERLAY_VIEW *v)
{
    const en_OVL_ACTION id = s_pending;
    s_pending = OVL_ACTION_NONE;
    if (id == OVL_ACTION_NONE || !v) return;
    if (overlay_action_reason(id, v)) return;
    const st_OVL_ACTION_DEF *def = overlay_action_def(id);

    const bool with_value = def->arg == OVL_ACTION_ARG_VALUE;
    if (def->close == OVL_ACTION_CLOSE_BEFORE) imgui_overlay_close();
    def->run(v, with_value ? s_pending_value : def->arg);
    s_test_last = id;
    if (with_value) s_test_last_value = s_pending_value;
    if (def->close == OVL_ACTION_CLOSE_RESUME) imgui_overlay_note_pause_changed(false);
    if (def->close == OVL_ACTION_CLOSE_AFTER || def->close == OVL_ACTION_CLOSE_RESUME) imgui_overlay_close();
}

en_OVL_ACTION overlay_actions_test_take_last(void)
{
    const en_OVL_ACTION id = s_test_last;
    s_test_last = OVL_ACTION_NONE;
    return id;
}

int overlay_actions_test_last_value(void)
{
    return s_test_last_value;
}
