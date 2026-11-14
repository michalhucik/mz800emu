/* 
 * File:   cmtext.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. května 2018, 13:21
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
 * @file cmtext.h
 * @brief Rozhraní rozšíření Virtual CMT (formát pásky) a registr rozšíření.
 */

#ifndef CMTEXT_H
#define CMTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "cmtext_container.h"
#include "cmtext_block.h"


#define CMTEXT_TYPE_UNKNOWN     0x00
#define CMTEXT_TYPE_PLAYABLE    0x01
#define CMTEXT_TYPE_RECORDABLE  0x02


    typedef struct st_CMTEXT_INFO {
        char *name;
        char **fileext; // podporovane pripony, zakonceno NULL
        char *description;
        int type;
    } st_CMTEXT_INFO;


    typedef void ( *cmtext_cb_init ) (void);
    typedef void ( *cmtext_cb_exit ) (void);
    typedef int ( *cmtext_cb_open ) (char *filename );
    typedef void ( *cmtext_cb_stop ) (void);
    typedef void ( *cmtext_cb_eject ) (void);
    typedef void ( *cmtext_cb_write_data ) ( uint64_t play_ticks, int value );


    typedef struct st_CMTEXT {
        st_CMTEXT_INFO *info;
        st_CMTEXT_CONTAINER *container;
        st_CMTEXT_BLOCK *block; // prave prehravany datovy blok
        cmtext_cb_init cb_init;
        cmtext_cb_exit cb_exit;
        cmtext_cb_open cb_open;
        cmtext_cb_stop cb_stop;
        cmtext_cb_eject cb_eject;
        cmtext_cb_write_data cb_write;
    } st_CMTEXT;


    extern void cmtext_init ( void );
    extern void cmtext_exit ( void );

    extern st_CMTEXT* cmtext_get_extension ( const char *filename );
    extern const char* cmtext_get_description ( st_CMTEXT *ext );
    extern const char* cmtext_get_name ( st_CMTEXT *ext );
    extern st_CMTEXT_BLOCK* cmtext_get_block ( st_CMTEXT *ext );
    extern st_CMTEXT_CONTAINER* cmtext_get_container ( st_CMTEXT *ext );
    extern int cmtext_get_type ( st_CMTEXT *ext );

    extern int cmtext_container_open ( const char *filename );

    extern const char* cmtext_get_filename_extension ( const char *filename );

    /**
     * @brief Zjistí, zda má soubor příponu přehrávatelné pásky.
     *
     * Na rozdíl od cmtext_get_extension() nezávisí na cmtext_init(): prochází
     * statickou tabulku g_cmtext[] až po koncový NULL, takže ji lze volat
     * i při kontrole CLI voleb před startem emulátoru. Přípona se porovnává
     * bez ohledu na velikost písmen (tmz, tzx, mzf, m12, mzt, tap, wav, wave).
     *
     * @param filename Jméno nebo cesta souboru (UTF-8); NULL = false.
     * @return true, pokud přípona patří některé přehrávatelné (PLAYABLE) příponě.
     * @note Bez vedlejších efektů, bezpečné z libovolného vlákna.
     */
    extern bool cmtext_is_playable_filename ( const char *filename );

    /**
     * @brief Zapíše seznam přípon přehrávatelných pásek, např. ".wav, .wave, .mzf".
     *
     * Stejně jako cmtext_is_playable_filename() nezávisí na cmtext_init().
     * Slouží pro chybové zprávy (anglicky, bez lokalizace).
     *
     * @param buf Cílový buffer; výsledek je vždy ukončený nulou.
     * @param size Velikost bufferu v bajtech (> 0); delší seznam se zkrátí.
     */
    extern void cmtext_get_playable_extensions_list ( char *buf, size_t size );

    /**
     * @brief Zjistí, zda lze pásku daného rozšíření přehrávat.
     *
     * Pozor na konvenci návratové hodnoty: nejde o bool, ale o EXIT_* kód.
     * Volající musí porovnávat s EXIT_SUCCESS
     * (`EXIT_SUCCESS == cmtext_is_playable ( ext )`), ne testovat pravdivost.
     *
     * @param ext Rozšíření (může být NULL).
     * @return EXIT_SUCCESS (0), pokud rozšíření má CMTEXT_TYPE_PLAYABLE;
     *         EXIT_FAILURE pro NULL, chybějící info nebo nepřehratelný typ.
     */
    extern int cmtext_is_playable ( st_CMTEXT *ext );

    /**
     * @brief Zjistí, zda lze do pásky daného rozšíření nahrávat.
     *
     * Stejná konvence jako cmtext_is_playable(): EXIT_SUCCESS = ano.
     *
     * @param ext Rozšíření (může být NULL).
     * @return EXIT_SUCCESS (0), pokud rozšíření má CMTEXT_TYPE_RECORDABLE;
     *         EXIT_FAILURE pro NULL, chybějící info nebo typ bez nahrávání.
     *         Dnes žádné rozšíření typ RECORDABLE nemá (vrací vždy
     *         EXIT_FAILURE): záznam se připojuje k pásce cmt_tmz
     *         (cmt_record()), příznak recordable v MCP výpisu pásky proto
     *         zůstává false.
     */
    extern int cmtext_is_recordable ( st_CMTEXT *ext );


#ifdef __cplusplus
}
#endif

#endif /* CMTEXT_H */

