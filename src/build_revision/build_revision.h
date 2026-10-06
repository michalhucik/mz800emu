/* 
 * File:   build_revision.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 15. září 2015, 12:19
 * 
 * 
 * ----------------------------- License -------------------------------------
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * ---------------------------------------------------------------------------
 */

/**
 * @file build_revision.h
 * @brief Informace o buildu odvozené z gitu (revize, původ zdrojů, commit).
 *
 * Implementaci (build_revision.c) generuje při každém buildu skript
 * tools/generate_build_revision_git.sh (target mz_build_revision, viz
 * cmake/BuildRevision.cmake). Všechny funkce vracejí hodnoty zapečené
 * v okamžiku buildu; za běhu se git nikdy nevolá.
 *
 * Když git informace chybí (zdrojový archiv bez .git, git není nainstalovaný,
 * repozitář bez remotů nebo commitů, selhání git příkazu), generátor zapíše
 * hodnoty "unknown" (resp. -1 / BUILD_REVISION_ORIGIN_UNKNOWN) a build
 * pokračuje.
 *
 * Všechny vrácené řetězce jsou statické literály: volající je nesmí
 * uvolňovat ani měnit, platí po celou dobu běhu programu. Funkce jsou
 * bezstavové a lze je volat z libovolného vlákna i před inicializací
 * čehokoliv dalšího (např. pro --version).
 */

#ifndef BUILD_REVISION_H
#define	BUILD_REVISION_H

#ifdef	__cplusplus
extern "C" {
#endif

    /**
     * @brief Původ zdrojů, ze kterých byl build sestaven.
     *
     * Určuje se z URL git remotů zdrojového stromu. Remote, který je lokální
     * cestou k jinému repozitáři (klon klonu), se sleduje nejvýše 4 úrovně
     * hluboko (s ochranou proti cyklům) - týká se jen detekce NAS1.
     */
    typedef enum en_BUILD_REVISION_ORIGIN {
        BUILD_REVISION_ORIGIN_UNKNOWN = 0, /**< neznámý původ (fork, žádné remoty, bez gitu) */
        BUILD_REVISION_ORIGIN_GITHUB = 1, /**< oficiální upstream github.com/michalhucik/mz800emu (přímý remote) */
        BUILD_REVISION_ORIGIN_NAS1 = 2, /**< repozitář na hostiteli "nas1" (přímo nebo přes řetěz lokálních klonů) */
    } en_BUILD_REVISION_ORIGIN;

    /**
     * @brief Textová podoba revize pro výpis.
     * @return "Revision: <číslo>" pro oficiální upstream build,
     *         jinak "Revision: ???". Statický řetězec.
     */
    extern const char* build_revision_get_const_char ( void );

    /**
     * @brief Číslo revize.
     *
     * Počítá se jen pro oficiální upstream klon (BUILD_REVISION_ORIGIN_GITHUB)
     * jako 250 + počet commitů na HEAD (offset zachovává kontinuitu s
     * historickými SVN revizemi ze SourceForge). U shallow klonu je počet
     * commitů neúplný.
     *
     * @return číslo revize, nebo -1 pro jakýkoliv jiný build.
     */
    extern int build_revision_get_int ( void );

    /**
     * @brief Původ zdrojů buildu.
     *
     * Přednost má oficiální upstream (přímý remote na GitHub); NAS1 se
     * hledá jen tehdy, když upstream remote chybí.
     *
     * @return hodnota en_BUILD_REVISION_ORIGIN.
     */
    extern en_BUILD_REVISION_ORIGIN build_revision_get_repo_origin ( void );

    /**
     * @brief Anglické jméno původu zdrojů.
     * @return "GitHub", "NAS1" nebo "unknown". Statický řetězec.
     */
    extern const char* build_revision_get_repo_origin_name ( void );

    /**
     * @brief Jméno větve, ze které byl build sestaven.
     * @return krátké jméno větve; "detached" při detached HEAD (větev
     *         stejného jména odliší build_revision_is_detached()); "unknown"
     *         bez git informací. Statický řetězec (zvláštní znaky jména
     *         větve escapuje generátor v C literálu).
     */
    extern const char* build_revision_get_branch ( void );

    /**
     * @brief Příznak detached HEAD.
     * @return 1 = build ze stavu detached HEAD (build_revision_get_branch()
     *         vrací "detached"), 0 = build z větve nebo neznámo.
     */
    extern int build_revision_is_detached ( void );

    /**
     * @brief Plný hash commitu HEAD.
     * @return hex hash (40 znaků, u SHA-256 repozitáře 64), nebo "unknown".
     *         Statický řetězec.
     */
    extern const char* build_revision_get_commit ( void );

    /**
     * @brief Zkrácený hash commitu HEAD (git rev-parse --short).
     * @return zkrácený hex hash (obvykle 7-8 znaků), nebo "unknown".
     *         Statický řetězec.
     */
    extern const char* build_revision_get_commit_short ( void );

    /**
     * @brief Příznak neuložených změn v pracovním stromu při buildu.
     *
     * Počítají se jen změny sledovaných souborů (neverzované soubory ne),
     * stejně jako u git describe --dirty.
     *
     * @return 1 = pracovní strom měl neuložené změny, 0 = čistý strom,
     *         -1 = neznámo (bez git informací).
     */
    extern int build_revision_is_dirty ( void );

    /**
     * @brief Hodnota proměnné prostředí MSYSTEM v okamžiku buildu.
     * @return např. "UCRT64" nebo "MINGW64"; prázdný řetězec, pokud
     *         proměnná nebyla nastavená (build mimo MSYS2). Statický řetězec.
     */
    extern const char* build_revision_get_build_msystem ( void );

#ifdef	__cplusplus
}
#endif

#endif	/* BUILD_REVISION_H */

