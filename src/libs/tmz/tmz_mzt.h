/**
 * @file   tmz_mzt.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  MZT (MZF soubory za sebou) a hlavička CMTSPEED UniCMT (spec kap. 10).
 *
 * MZT není součástí TMZ, je to vstupní a výstupní formát jako MZF.
 * Rozšíření UniCMT vkládá mezi soubory 128B hlavičky CMTSPEED (typ 00h,
 * jméno "CMTSPEED" + 0Dh, fsize/fstrt/fexec 0, bajt 20h = 01h, od 30h
 * 4x WORD LE: LONG první, LONG druhá, SHORT první, SHORT druhá půlvlna
 * v µs). Rychlost platí pro všechny další soubory až do další hlavičky.
 *
 * Modul čte MZT na položky, rozpozná a sestaví hlavičku CMTSPEED.
 * Hlavičky MZF bere bajt po bajtu tak, jak jsou v souboru.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
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

#ifndef TMZ_MZT_H
#define TMZ_MZT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#include "../tzx/tzx.h"
#include "tmz_profile.h"


/** @brief Velikost hlavičky MZF i CMTSPEED v bajtech. */
#define TMZ_MZT_HEADER_SIZE 128

/**
 * @brief Pulzy UniCMT 1x v µs (LONG H, LONG L, SHORT H, SHORT L), soubor
 *        1xspeed.mzf; rovné PULSES profilu ROM-800-NOMINAL. Inicializátor pole.
 */
#define TMZ_MZT_CMTSPEED_PULSES_1X { 470, 494, 240, 278 }


    /**
     * @brief Jedna položka MZT: soubor MZF, nebo hlavička CMTSPEED.
     *
     * @invariant is_cmtspeed == 1: body == NULL, body_size == 0
     * @par Vlastnictví: header a body ukazují do dat předaných tmz_mzt_parse();
     *      platí, dokud žijí tato data.
     */
    typedef struct st_TMZ_MZT_ITEM {
        uint32_t offset;            /**< offset hlavičky v MZT */
        int is_cmtspeed;            /**< 1 = hlavička CMTSPEED */
        uint16_t pulses_us[4];      /**< CMTSPEED: hodnoty hlavičky; MZF: platná rychlost (poslední CMTSPEED před ním, nebo 1x).
                                         Pozor: i úvodní CMTSPEED (první položka MZT) se sem promítne,
                                         tmz_mzt_build_unicmt_blocks() ji ale ignoruje (UniCMT ji nepřehraje) */
        const uint8_t *header;      /**< 128 B hlavičky (bajty souboru, LE) */
        const uint8_t *body;        /**< tělo (NULL při délce 0) */
        uint32_t body_size;         /**< délka těla = fsize z hlavičky */
    } st_TMZ_MZT_ITEM;


    /**
     * @brief Načtené MZT.
     *
     * @invariant cmtspeed_count <= count; items == NULL právě když count == 0
     * @par Vlastnictví: pole items vlastní struktura (uvolní tmz_mzt_free()),
     *      položky ukazují do dat volajícího.
     */
    typedef struct st_TMZ_MZT {
        st_TMZ_MZT_ITEM *items;     /**< položky v pořadí souboru */
        uint32_t count;             /**< počet položek */
        uint32_t cmtspeed_count;    /**< z toho hlaviček CMTSPEED */
    } st_TMZ_MZT;


    /**
     * @brief Rozpozná hlavičku CMTSPEED (spec 10.1).
     *
     * Kontroluje typ 00h, jméno "CMTSPEED" + 0Dh a fsize 0. Ostatní bajty
     * (20h, fstrt, fexec) nekontroluje.
     *
     * @param header 128 B hlavičky (bajty souboru).
     * @param[out] pulses_us Při návratu 1 pulzy z 30h (LONG H, LONG L,
     *        SHORT H, SHORT L v µs); smí být NULL.
     * @return 1 = CMTSPEED, 0 = jiná hlavička nebo NULL.
     */
    extern int tmz_mzt_is_cmtspeed ( const uint8_t *header, uint16_t pulses_us[4] );


    /**
     * @brief Sestaví hlavičku CMTSPEED (spec 10.1).
     *
     * Pro pulzy 1x, 2x a 3x je výsledek bajtově shodný se soubory
     * 1xspeed.mzf, 2xspeed.mzf a 3xspeed.mzf UniCMT.
     *
     * @param pulses_us LONG H, LONG L, SHORT H, SHORT L v µs.
     * @param[out] out 128 B výstupu.
     */
    extern void tmz_mzt_build_cmtspeed ( const uint16_t pulses_us[4], uint8_t out[TMZ_MZT_HEADER_SIZE] );


    /**
     * @brief Přečte MZT na položky.
     *
     * Položka MZF dostane platnou rychlost: poslední CMTSPEED před ní,
     * jinak 1x. Data se nekopírují.
     *
     * @param data MZT.
     * @param size Délka.
     * @param[out] mzt Výsledek; při chybě vynulovaný.
     * @return EXIT_SUCCESS; EXIT_FAILURE při NULL, prázdném vstupu, zbytku
     *         kratším než hlavička, těle přes konec dat nebo selhání alokace.
     * @post Při úspěchu volající uvolní mzt přes tmz_mzt_free() a data
     *       nechá žít, dokud položky používá.
     */
    extern int tmz_mzt_parse ( const uint8_t *data, uint32_t size, st_TMZ_MZT *mzt );


    /**
     * @brief Jako tmz_mzt_parse(), ale zbytek kratší než hlavička na konci
     *        dat není chyba.
     *
     * Soubory přenesené přes CP/M bývají doplněné na násobek 128 B; tento
     * zbytek se nepřečte a jeho délka se vrátí v tail. Tělo přes konec dat
     * zůstává chybou.
     *
     * @param data MZT. @param size Délka.
     * @param[out] mzt Výsledek; při chybě vynulovaný.
     * @param[out] tail Délka nepřečteného zbytku (0..127); smí být NULL,
     *        pak se funkce chová jako tmz_mzt_parse().
     * @return EXIT_SUCCESS; EXIT_FAILURE jako tmz_mzt_parse(), nebo když
     *         data neobsahují žádnou celou hlavičku.
     * @post Při úspěchu volající uvolní mzt přes tmz_mzt_free().
     */
    extern int tmz_mzt_parse_tail ( const uint8_t *data, uint32_t size, st_TMZ_MZT *mzt, uint32_t *tail );


    /**
     * @brief Uvolní pole položek (data MZT neuvolňuje).
     * @param mzt MZT (NULL bezpečné); po návratu vynulované.
     */
    extern void tmz_mzt_free ( st_TMZ_MZT *mzt );


    /**
     * @brief Z položek MZT sestaví bloky 0x46 s profilem UNICMT (spec 10.2).
     *
     * Každý soubor MZF dá jeden blok. Pulzy (PROFILE_ARGS cmtspeed) jsou
     * z poslední předchozí hlavičky CMTSPEED, jinak 1x. CMTSPEED na prvním
     * místě MZT se nepoužije (UniCMT ji nepřehraje, spec 10.1; *leading = 1),
     * CMTSPEED bez dalšího souboru také ne (*trailing = 1). Hlavička bloku =
     * bajty položky (převádí se jen pořadí fsize/fstrt/fexec).
     *
     * @param mzt Načtené MZT.
     * @param base Parametry pro všechny bloky (gap, pause, machine; NULL =
     *        výchozí); cmtspeed se přepíše rychlostí položky.
     * @param[out] out Pole nových bloků (uvolnit každý tmz_block_free() a pole free()); NULL při 0 blocích.
     * @param[out] count Počet bloků.
     * @param[out] leading 1 = MZT začíná hlavičkou CMTSPEED (smí být NULL).
     * @param[out] trailing 1 = MZT končí hlavičkou CMTSPEED bez souboru (smí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE při NULL vstupu, chybě registru nebo
     *         alokace (pak se nic nevrací a vše je uvolněno).
     */
    extern int tmz_mzt_build_unicmt_blocks ( const st_TMZ_MZT *mzt, const st_TMZ_PROFILE_ARGS *base,
                                             st_TZX_BLOCK ***out, uint32_t *count, int *leading, int *trailing );


    /**
     * @brief MZF z bloku a jeho rychlost pro export MZT.
     *
     * @invariant body_size == fsize hlavičky
     * @par Vlastnictví: body ukazuje do dat bloku, nebo do owned (kopie dat
     *      bloku v1.0, parser mění endianitu); uvolní tmz_mzt_block_info_free().
     */
    typedef struct st_TMZ_MZT_BLOCK_INFO {
        uint8_t header[TMZ_MZT_HEADER_SIZE];  /**< hlavička MZF v pořadí souboru (bajty bloku) */
        const uint8_t *body;        /**< tělo (NULL při délce 0) */
        uint32_t body_size;         /**< délka těla */
        int rom_structure;          /**< 1 = záznam ROM (hlavička + tělo FM), rychlost platí */
        uint16_t pulses_us[4];      /**< rychlost UniCMT, LONG H, LONG L, SHORT H, SHORT L v µs */
        uint8_t *owned;             /**< interní kopie dat bloku, nebo NULL */
    } st_TMZ_MZT_BLOCK_INFO;


    /**
     * @brief Zjistí MZF a rychlost bloku pro export MZT.
     *
     * Zařazení: 0x40 = 1x; 0x41 NORMAL = 1x při 1:1, jinak floor(1x / dělitel
     * rychlosti) (2:1 a 3:1 dají přesně 2x a 3x UniCMT), neplatná rychlost
     * jako 1:1; 0x46 ROM-800-NOMINAL, ROM-800-EXACT = 1x, UNICMT = cmtspeed
     * z PROFILE_ARGS, TAPEMZ-V1-LEGACY format normal jako 0x41 NORMAL,
     * MZ800EMU-CMT = cmtspeed z PROFILE_ARGS (všechna čtyři pole nenulová),
     * jinak speed jako 0x41 NORMAL (2:1 CPM bez struktury ROM),
     * IC-10.2-NORMAL podle bd, TC-1.21 loader=N podle poměru TurboCopy.
     * Ostatní (0x41 TURBO, FASTIPL, 2:1 CPM a další, 0x46 jiných profilů)
     * mají rom_structure = 0.
     *
     * @param block Blok.
     * @param[out] info Výsledek; při návratu 1 uvolnit tmz_mzt_block_info_free().
     * @return 1 = blok nese MZF, 0 = jiný blok, -1 = poškozený blok, fsize
     *         hlavičky jiné než délka těla, selhání alokace.
     */
    extern int tmz_mzt_block_info ( const st_TZX_BLOCK *block, st_TMZ_MZT_BLOCK_INFO *info );


    /**
     * @brief Uvolní interní kopii v info (NULL bezpečné).
     * @param info Info.
     */
    extern void tmz_mzt_block_info_free ( st_TMZ_MZT_BLOCK_INFO *info );


    /**
     * @brief Varianta výstupního MZT (spec 10.3).
     */
    typedef enum en_TMZ_MZT_MODE {
        TMZ_MZT_CLASSIC = 0,   /**< jen MZF za sebou, časování se ztratí (--mzt) */
        TMZ_MZT_UNICMT = 1,    /**< s hlavičkami CMTSPEED, kde se mění rychlost (--mzt-unicmt) */
    } en_TMZ_MZT_MODE;


    /**
     * @brief Callback varování exportu.
     * @param user Data volajícího.
     * @param block_index Index bloku v souboru.
     * @param text Text varování (anglicky).
     */
    typedef void ( *tmz_mzt_warn_cb ) ( void *user, uint32_t block_index, const char *text );


    /**
     * @brief Zapíše MZT ze všech bloků souboru, které nesou MZF (spec 10.3).
     *
     * CLASSIC: MZF za sebou; varování u bloku, který nemá strukturu ROM nebo
     * rychlost 1x. UNICMT: před souborem s jinou rychlostí, než má předchozí
     * (výchozí 1x), vloží hlavičku CMTSPEED (tmz_mzt_build_cmtspeed); první
     * soubor musí být 1x a všechny bloky musí mít strukturu ROM.
     * Hlavičky MZF jdou do výstupu bajt po bajtu z bloku.
     *
     * @param file Soubor TMZ/TZX.
     * @param mode Varianta.
     * @param[out] out_data Výstup (uvolnit free()); při chybě NULL.
     * @param[out] out_size Délka výstupu.
     * @param warn Varování (smí být NULL); u chyby dostane i důvod chyby.
     * @param user Data pro warn.
     * @return EXIT_SUCCESS; EXIT_FAILURE při NULL vstupu, souboru bez MZF,
     *         poškozeném bloku, porušení pravidel UNICMT nebo selhání alokace.
     */
    extern int tmz_mzt_export ( const st_TZX_FILE *file, en_TMZ_MZT_MODE mode, uint8_t **out_data, uint32_t *out_size,
                                tmz_mzt_warn_cb warn, void *user );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_MZT_H */
