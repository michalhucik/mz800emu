/**
 * @file version_info.h
 * @brief Výpis informací o verzi a buildu (CLI volba --version).
 *
 * Modul sestaví textový přehled o binárce: program a platforma (MZ-800,
 * MZ-700 PAL/NTSC, MZ-1500), verze, revize, datum buildu, původ zdrojů
 * (repozitář NAS1 / GitHub / neznámý) s větví, commitem a příznakem
 * neuložených změn, překladač, MSYS2 toolchain, zakompilované volitelné
 * části, verze SDL a GLib a hostitelský OS.
 *
 * Výpis nevyžaduje inicializaci SDL videa/audia, GLib ani emulátoru, takže
 * ho lze volat hned na začátku main() a skončit. Texty jsou anglicky (jako
 * výpis --help), nelokalizují se - i18n se inicializuje až později.
 *
 * Modul se překládá zvlášť pro každou binárku (používá per-target defines
 * MZARCH a MZTVSYS), proto leží mimo sdílené knihovny.
 */

#ifndef VERSION_INFO_H
#define VERSION_INFO_H

#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Zajistí použitelný stdout/stderr pro textový výpis na Windows.
     *
     * Binárka s GUI subsystémem (-mwindows, výchozí build) spuštěná z cmd.exe
     * nebo PowerShellu nemá platný stdout - výpis by se ztratil. Pokud stdout
     * není platný handle, funkce se připojí ke konzoli rodičovského procesu
     * (AttachConsole(ATTACH_PARENT_PROCESS)) a přesměruje na ni stdout
     * a stderr. Pokud stdout platný je (přesměrování do souboru nebo roury,
     * MSYS2 terminál, build s konzolovým subsystémem), nedělá nic.
     *
     * Vedlejší efekt: po připojení ke konzoli rodiče se výstup objeví v jeho
     * okně; cmd.exe na GUI program nečeká, takže prompt může být vypsán
     * dřív než výstup.
     *
     * Na ostatních platformách nedělá nic.
     *
     * @pre Volat před prvním zápisem do stdout/stderr.
     */
    void version_info_prepare_console(void);

    /**
     * @brief Vypíše kompletní informace o verzi a buildu.
     *
     * Formát: řádky "Klíč: hodnota" (anglicky). Neznámé hodnoty se vypíšou
     * jako "unknown". Funkce nevolá SDL_Init ani nic neinicializuje; verzi
     * linkované SDL zjišťuje přes SDL_GetVersion(), které inicializaci
     * nepotřebuje.
     *
     * @param out cílový proud (typicky stdout); NULL = stdout.
     * @post Proud je po výpisu vyprázdněn (fflush).
     */
    void version_info_print(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* VERSION_INFO_H */
