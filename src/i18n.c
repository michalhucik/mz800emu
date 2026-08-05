/**
 * @file   i18n.c
 * @brief  Implementace i18n_init() — bind gettext domain proti home_dir/locale.
 *
 * Hledá locale/ relativně k @c g_sdlapp->paths->home_dir. V dist/ je
 * @c locale/ vedle binárky, ve vývoji v repo není a fallback na
 * @c src/locale/ kde @c make i18n-mo-auto generuje .mo soubory.
 *
 * Kompiluje se jako samostatný .c (na rozdíl od inline v i18n.h),
 * protože potřebuje glib a sdlapp_paths headers.
 */

#include <stdio.h>

#include "i18n.h"
#include <glib.h>
#include <sys/stat.h>
#include "libs/sdlapp/sdlapp.h"
#include "emulator/mzarch/mzhal.h"

extern SdlApp *g_sdlapp;

void i18n_init(void)
{
    setlocale(LC_ALL, "");

    /* Doména runtime z g_mzhal ("mz800" -> "mz800emu"); dřívější
     * compile-time I18N_TEXTDOMAIN z MZARCH_NAME měl tichý fallback na
     * "mz800emu", který by po sjednocení kompilace potichu rozbil
     * doménu mz700/mz1500 EXE (mzhal krok 7). Pozn.: oba mz700 targety
     * sdílí arch_name "mz700" = jednu doménu (katalogy jsou 3). */
    static char domain[32];
    snprintf(domain, sizeof(domain), "%semu", g_mzhal.arch_name);

    char *localedir = sdlapp_paths_resolve_home(g_sdlapp->paths, "locale");
    struct stat st;
    if (stat(localedir, &st) != 0) {
        /* Vývoj v repo: HOME_DIR je repo root, .mo jsou v src/locale/ */
        char *fallback = sdlapp_paths_resolve_home(g_sdlapp->paths, "src/locale");
        if (stat(fallback, &st) == 0) {
            g_free(localedir);
            localedir = fallback;
        } else {
            g_free(fallback);
        }
    }
    bindtextdomain(domain, localedir);
    g_free(localedir);

    /* ImGui vyžaduje UTF-8; na Windows by gettext jinak vracel cp1250 */
    bind_textdomain_codeset(domain, "UTF-8");
    textdomain(domain);
}
