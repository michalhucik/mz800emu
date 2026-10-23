/**
 * @file rom_request.h
 * @brief Zásahy GUI do ROM přes emu vlákno (device_request.h).
 *
 * Menu ROM a okno User Defined ROM Settings dřív volaly rom_reinstall()
 * přímo z UI vlákna: přepis g_memory.ROM (včetně CG-ROM) a patche CMT
 * hacku souběžně s během programu, přepis cest k souborům ROM
 * (realloc), které čte rom_install(). Tyto funkce zásah jen zařadí;
 * provede ho emu vlákno v device_request_poll() (při aktivním replay ho
 * odmítne, záznam by změnu nezopakoval).
 *
 * Výměna ROM proběhne mezi instrukcemi bez resetu, stejně jako dřív
 * z menu.
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef ROM_REQUEST_H
#define ROM_REQUEST_H

#include <stdint.h>

#include "rom.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Nastavení uživatelské ROM z okna User Defined ROM Settings.
 *
 * Odpovídá položkám `user_defined_*` v st_ROM. Cesty jsou řetězce
 * volajícího; rom_request_set_user_defined() si je zkopíruje.
 * NULL u cesty znamená prázdnou cestu.
 */
typedef struct st_ROM_USER_DEFINED_SETTINGS {
    en_ROM_BOOL    allinone;                /**< Hlavní ROM jedním souborem. */
    const char    *allinone_fp;             /**< Soubor celé ROM. */
    const char    *mz700_fp;                /**< Soubor ROM 0000h. */
    const char    *cgrom_fp;                /**< Soubor CG-ROM. */
    const char    *mz800_fp;                /**< Soubor ROM 0E000h. */
    en_ROM_CMTHACK cmthack_type;            /**< Varianta patche CMT hacku. */
    en_ROM_BOOL    cmthack_allinone;        /**< ROM pro CMT hack jedním souborem. */
    const char    *cmthack_allinone_fp;     /**< Soubor celé ROM pro CMT hack. */
    const char    *cmthack_mz700_fp;        /**< Soubor ROM 0000h pro CMT hack. */
    const char    *cmthack_cgrom_fp;        /**< Soubor CG-ROM pro CMT hack. */
    const char    *cmthack_mz800_fp;        /**< Soubor ROM 0E000h pro CMT hack. */
} st_ROM_USER_DEFINED_SETTINGS;

/**
 * @brief Zařadí výběr ROM z menu (rom_reinstall() s řádkem konfigurace).
 * @param row Řádek g_rom_config->roms typu ROM_CONFIG_TYPE_ROM (jiný nebo
 *        NULL = nezařadí se). Předává se jeho index v poli.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t rom_request_select(const st_ROM_CONFIG_ROW *row);

/**
 * @brief Zařadí použití nastavení uživatelské ROM.
 *
 * Emu vlákno přepíše položky `user_defined_*` v g_rom, vynutí nové
 * načtení souborů a je-li právě vybraná uživatelská ROM, přeinstaluje ji.
 * Nastavení se zkopíruje do čekající kopie modulu; když ji emu vlákno
 * ještě nepřevzalo, nové volání ji nahradí (platí poslední).
 *
 * @param s Nastavení (nesmí být NULL); řetězce se zkopírují.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (`s` je NULL).
 */
uint32_t rom_request_set_user_defined(const st_ROM_USER_DEFINED_SETTINGS *s);

#ifdef __cplusplus
}
#endif

#endif /* ROM_REQUEST_H */
