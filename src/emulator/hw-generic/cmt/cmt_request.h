/**
 * @file cmt_request.h
 * @brief Zásahy GUI do kazety přes emu vlákno (device_request.h).
 *
 * Menu CMT, okno Virtual CMT, seznam bloků pásky a zadní DIP přepínač dřív
 * volaly přímo z UI vlákna funkce, které otevírají pásku (uvolní předchozí
 * obraz a jeho bloky), přegenerují stream bloku (rychlost, vlastní pulzy),
 * přepisují ROM patchem CMT hacku nebo mění polaritu streamu, zatímco
 * emu vlákno pásku čte. Tyto funkce zásah jen zařadí; provede ho emu
 * vlákno v device_request_poll() (při aktivním replay ho odmítne, záznam
 * by změnu nezopakoval; GUI tyto prvky při záznamu i tak zamyká).
 *
 * Transport (play, stop, pauza, výběr bloku, nahrávání, vysunutí) jde dál
 * přes replay_host_cmt(), který zásah zapíše do záznamu vstupů.
 * Volby hostitele CPU Boost a Enable MZF Size Check zůstávají přímé:
 * mění jen jeden příznak a stav kazety nepřestavují.
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef CMT_REQUEST_H
#define CMT_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#include "libs/mztape/mztape.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí otevření pásky ze souboru (cmt_open_file_by_extension()).
 * @param path Cesta k souboru (nesmí být NULL ani prázdná; delší než
 *        DEVICE_REQUEST_STR_MAX - 1 bajtů = nezařadí se).
 * @param play true = po úspěšném otevření spustit přehrávání (cmt_play()).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 * @note Návrat zásahu (st_DEVICE_REQ_RESULT.rc): 0 = otevřeno, -1 =
 *       nepodařilo se (chybu hlásí kazeta sama).
 */
uint32_t cmt_request_open(const char *path, bool play);

/**
 * @brief Zařadí změnu výchozí rychlosti Virtual CMT (cmt_change_speed()).
 * @param cmtspeed Poměr en_CMTSPEED (ne CMTSPEED_CUSTOM, neplatná hodnota =
 *        nezařadí se); vlastní pulzy nastaví cmt_request_set_custom_pulses().
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t cmt_request_set_speed(int cmtspeed);

/**
 * @brief Zařadí výchozí vlastní pulzy Virtual CMT (cmt_change_custom_pulses()).
 * @param pulses Délky pulzů (nesmí být NULL); předají se přesně (textem
 *        v šestnáctkovém zápisu double).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (`pulses` je NULL).
 */
uint32_t cmt_request_set_custom_pulses(const st_MZTAPE_PULSES_LENGTH *pulses);

/**
 * @brief Zařadí změnu rychlosti jednoho bloku pásky (cmt_tape_change_block_speed()).
 * @param block_id Index bloku v kontejneru (záporný = nezařadí se).
 * @param cmtspeed CMTSPEED_NONE = výchozí rychlost, poměr, nebo
 *        CMTSPEED_CUSTOM (pak je @p pulses povinné).
 * @param is_mz true = blok MZ formátu (rychlost DEFAULT/SET), false = jiný
 *        formát (jen rychlost).
 * @param pulses Vlastní pulzy pro CMTSPEED_CUSTOM, jinak NULL.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t cmt_request_set_block_speed(int block_id, int cmtspeed, bool is_mz,
                                     const st_MZTAPE_PULSES_LENGTH *pulses);

/**
 * @brief Zařadí zapnutí nebo vypnutí CMT hacku (cmthack_load_rom_patch()).
 * @param enabled true = patch ROM nainstalovat.
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t cmt_request_set_hack(bool enabled);

/**
 * @brief Zařadí volbu Fix FNAME Terminator CMT hacku.
 * @param enabled true = opravovat zakončení jména souboru.
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t cmt_request_set_fix_fname_terminator(bool enabled);

/**
 * @brief Zařadí polaritu kazety ze zadního DIP přepínače
 *        (cmt_rear_dip_switch_cmt_inverted_polarity()).
 * @param inverted true = invertovaná polarita.
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t cmt_request_set_polarity(bool inverted);

#ifdef __cplusplus
}
#endif

#endif /* CMT_REQUEST_H */
