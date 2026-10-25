/**
 * @file snapshot_quicksave.h
 * @brief Quick Save/Load logika — 3 módy (Basic, Incremental, Rotational)
 */

#ifndef SNAPSHOT_QUICKSAVE_H
#define SNAPSHOT_QUICKSAVE_H

#include "snapshot.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Provede Quick Save dle aktuálního nastaveného módu
 *
 * Adresář: `[SNAPSHOT] default_directory`, prázdný = pracovní adresář
 * (--work-dir), relativní vůči němu; chybějící adresář se vytvoří.
 *
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_quicksave(void);

/**
 * Provede Quick Load — načte nejnovější quicksave snapshot
 *
 * Hledá ve stejném adresáři jako snapshot_quicksave().
 *
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_quickload(void);

/**
 * Najde další pořadové číslo pro incremental/rotational mód
 *
 * @param dir Adresář
 * @param basename Základ jména souboru
 * @return Další číslo (1-based)
 */
int snapshot_quicksave_find_next_number(const char *dir, const char *basename);

/**
 * Najde cestu k nejnovějšímu quicksave souboru
 *
 * @param out_path Výstupní buffer pro cestu
 * @param out_size Velikost bufferu
 * @param dir Adresář
 * @param basename Základ jména
 * @param mode Quick save mód (0=Basic, 1=Incremental, 2=Rotational)
 * @return true pokud soubor nalezen
 */
bool snapshot_quicksave_find_latest(char *out_path, size_t out_size,
                                     const char *dir, const char *basename,
                                     int mode);

/**
 * Smaže nejstarší soubory pokud jich je víc než max_slots (pro rotational mód)
 *
 * @param dir Adresář
 * @param basename Základ jména
 * @param max_slots Maximální počet slotů
 */
void snapshot_quicksave_cleanup_oldest(const char *dir, const char *basename,
                                        int max_slots);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_QUICKSAVE_H */
