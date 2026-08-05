/*
 * mztest.c — implementace testovacího wrapperu
 *
 * Inicializuje globální stav emulátoru v headless režimu
 * (bez SDL oken, bez ImGui, bez emulátorového vlákna).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "main.h"
#include "mzarch/mzhal.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <signal.h>

/* MSVC CRT: přesměrování assert dialogů na stderr */
__declspec(dllimport) int _set_error_mode(int mode);
#define _OUT_TO_STDERR 1

/* SIGABRT handler — zachytí abort() po assert() a ukončí bez dialogu */
static void mztest_sigabrt_handler(int sig)
{
    (void)sig;
    fprintf(stderr, "[mztest] SIGABRT zachycen (pravděpodobně assert failure)\n");
    _exit(1);
}
#endif

/* emulátorové hlavičky */
#include "emulator/emulator.h"
#include "emulator/display.h"
#include "emulator/cfgmain.h"
#include "emulator/audio.h"
#include "emulator/snapshot/snapshot.h"
#include "iface/iface_video.h"
#include "iface/iface_audio.h"
#include "mzarch/mzarch_platform_functions.h"
#include "generic_driver/file_driver.h"
#include "generic_driver/memory_driver.h"
#include "libs/sdlapp/sdlapp_options.h"

/* globální proměnné mztestu */
int mztest_current_level = MZTEST_LEVEL_UNIT;
int mztest_verbose = 0;

/* g_sdlapp je vyžadován emulátorovým jádrem (cfgmain.c, version_check, ...).
 * V headless testu vytváříme minimální stub se sdlapp_paths (odkazujícími
 * na CWD), aby kód resolvující paths fungoval. Plný SDL kontext nepotřebujeme. */
static SdlApp s_sdlapp_test_stub;
SdlApp *g_sdlapp = &s_sdlapp_test_stub;

/* příznak, zda je mztest inicializován */
static bool mztest_initialized = false;

/* stub iface callbacky — deklarovány v mztest_stubs.c */
extern void mztest_install_video_stubs(void);

/*
 * Inicializace emulátoru bez SDL oken a GUI.
 *
 * Sekvence odpovídá emulator_thread() z emulator.c,
 * ale přeskakuje SDL/ImGui závislosti.
 */
void mztest_init(void)
{
    if (mztest_initialized) {
        MZTEST_LOG("mztest_init(): už inicializováno, přeskakuji");
        return;
    }

    /* Konzistenční kontrola HW layer specifikace (g_mzhal) - fail-fast
     * dřív, než se cokoliv inicializuje (stejně jako v main()). */
    mzhal_validate();

    MZTEST_LOG("mztest_init(): začátek inicializace");

    /* 0. Potlačit Windows chybové dialogy (crash/assert) — jdou na stderr */
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);       /* CRT assert → stderr místo dialogu */
    signal(SIGABRT, mztest_sigabrt_handler); /* zachytit abort() po assert() */
#endif

    /* 1. Nainstalovat stub video callbacky PŘED iface_video_init() */
    mztest_install_video_stubs();
    MZTEST_LOG("  video stubs nainstalovány");

    /* 1b. SdlAppPaths stub — cfgmain.c volá g_sdlapp->paths->cfg_dir.
     *     Pro test prostředí používáme CWD jako home/cfg/work_dir
     *     (nemáme detekci binárky a nepotřebujeme ji). */
    g_sdlapp->paths = sdlapp_paths_new();
    MZTEST_LOG("  sdlapp_paths inicializovány");

    /* 1c. Inject --no-save-ini do sdlapp_options aby cfgmain_exit
     *     nezapisoval ini soubor do build-tests/. Test wrapper nemá
     *     persistovat config — sdlapp_options drží jen ukazatele,
     *     proto pole musí žít po dobu programu (static). */
    static const char *s_test_argv[] = { "mztest", "--no-save-ini", NULL };
    sdlapp_options_init(2, (char **)s_test_argv);
    MZTEST_LOG("  sdlapp_options nainjektovány (--no-save-ini)");

    /* 2. Konfigurace — musí být po nastavení g_mzarch_platform_name
     *    (to je nastaveno staticky v mzarch_platform.c dle MZARCH makra) */
    cfgmain_init();
    MZTEST_LOG("  cfgmain inicializován");

    /* 3. Video interface — vytvoří g_iface_video se semafory,
     *    zavolá stub init callback (nastaví is_initialized=true) */
    iface_video_init();
    MZTEST_LOG("  iface_video inicializován (stub)");

    /* 4. Audio interface — iface_audio_lowlevel_init() je stub */
    iface_audio_init();
    MZTEST_LOG("  iface_audio inicializován (stub)");

    /* 5. Inicializace emulátorové struktury */
    memset(&g_emulator, 0, sizeof(st_EMULATOR));
    MZTEST_LOG("  g_emulator vynulován");

    /* 6. Display (barvy, schéma) */
    display_init();
    MZTEST_LOG("  display inicializován");

    /* 7. Generické drivery */
    file_driver_init();
    memory_driver_init();
    MZTEST_LOG("  file/memory drivery inicializovány");

    /* 8. HW platforma — jádro emulátoru:
     *    vytvoří Z80 CPU, inicializuje paměť, periferie, audio */
    mzarch_platform_fn_init();
    MZTEST_LOG("  mzarch_platform_fn inicializován (CPU, paměť, periferie)");

    /* 9. Snapshot systém */
    snapshot_init();
    MZTEST_LOG("  snapshot systém inicializován");

    mztest_initialized = true;
    MZTEST_LOG("mztest_init(): inicializace dokončena");
}

/*
 * Úklid — uvolnění prostředků
 */
void mztest_teardown(void)
{
    if (!mztest_initialized) {
        return;
    }

    MZTEST_LOG("mztest_teardown()");

    /* Úklid video interface */
    if (g_iface_video) {
        iface_video_exit();
    }

    /* Úklid konfigurace */
    cfgmain_exit();

    /* Úklid sdlapp_paths stub */
    if (g_sdlapp && g_sdlapp->paths) {
        sdlapp_paths_destroy(g_sdlapp->paths);
        g_sdlapp->paths = NULL;
    }

    mztest_initialized = false;
}

/*
 * Zpracování CLI argumentů
 *
 * Podporované argumenty:
 *   --level smoke|unit|full   nastavení testovací úrovně
 *   --verbose                 zapnout verbose výstup
 */
void mztest_parse_args(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            mztest_verbose = 1;
        }
        else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "smoke") == 0) {
                mztest_current_level = MZTEST_LEVEL_SMOKE;
            } else if (strcmp(argv[i], "unit") == 0) {
                mztest_current_level = MZTEST_LEVEL_UNIT;
            } else if (strcmp(argv[i], "full") == 0) {
                mztest_current_level = MZTEST_LEVEL_FULL;
            } else {
                fprintf(stderr, "Neznámá úroveň: %s (použij smoke, unit, full)\n", argv[i]);
            }
        }
    }
}
