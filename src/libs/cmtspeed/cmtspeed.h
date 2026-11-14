/**
 * @file   cmtspeed.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.0
 * @brief  Definice rychlostních poměrů CMT pásky a inline funkce pro výpočet baudových rychlostí.
 *
 * Hlavičkový soubor knihovny cmtspeed poskytuje enum rychlostních režimů,
 * deklarace externích datových polí (divisory, textové poměry) a sadu
 * inline funkcí pro validaci, výpočet a formátování rychlostních parametrů
 * kazetové pásky počítačů Sharp MZ.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-11-09: Přidána hodnota CMTSPEED_CUSTOM (vlastní délky pulzů, UniCMT) a cmtspeed_is_custom (2.1.0).
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


#ifndef CMTSPEED_H
#define CMTSPEED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <math.h>


    /**
     * @brief Výčet rychlostních poměrů CMT pásky.
     *
     * Poměr je nominální násobek rychlosti 1:1 ("1200 Bd"). Skutečné
     * zapisovače se od nominálu liší: ROM má ~1350 Bd, Intercopy 10.2
     * "2400/2800/3200 Bd" je skutečně ~1,96x/2,39x/2,75x, TurboCopy škáluje
     * pulzy afinně (2:1 je ~1,8x).
     */
    typedef enum en_CMTSPEED {
        CMTSPEED_NONE = 0,  /**< Žádná rychlost (neplatná hodnota) */
        CMTSPEED_1_1,       /**< Poměr 1:1 - rychlost ROM (nominálně 1200 Bd, skutečně ~1350 Bd) */
        CMTSPEED_2_1,       /**< Poměr 2:1 - dvojnásobná rychlost */
        CMTSPEED_2_1_CPM,   /**< Poměr 2:1 - CP/M CMT.COM (SOKODI), 2400 Bd s vlastní pulzní sadou */
        CMTSPEED_3_1,       /**< Poměr 3:1 - trojnásobná rychlost */
        CMTSPEED_3_2,       /**< Poměr 3:2 */
        CMTSPEED_7_3,       /**< Poměr 7:3 - nominálně Intercopy 10.2 "2800 Bd" (skutečně ~2,39x) */
        CMTSPEED_8_3,       /**< Poměr 8:3 - nominálně Intercopy 10.2 "3200 Bd" (skutečně ~2,75x); CP/M cmt.com používá 2:1 */
        CMTSPEED_9_7,       /**< Poměr 9:7 */
        CMTSPEED_25_14,     /**< Poměr 25:14 */
        CMTSPEED_CUSTOM,    /**< Vlastní délky pulzů (např. hlavička CMTSPEED zařízení UniCMT).
                                 Nejde o poměr: délky nese volající zvlášť, divisor je 0
                                 a cmtspeed_is_valid() vrací 0. */
        CMTSPEED_COUNT      /**< Počet hodnot enumu (sentinel - nepoužívat jako rychlost) */
    } en_CMTSPEED;

    /** @brief Pole násobitelů rychlosti indexované hodnotami en_CMTSPEED. */
    extern const double g_cmtspeed_divisor[];

    /** @brief Pole textových popisů poměrů rychlosti indexované hodnotami en_CMTSPEED. */
    extern const char *g_cmtspeed_ratio[];


    /**
     * @brief Ověří, zda je zadaná rychlost platný poměr.
     *
     * Platná rychlost je v rozsahu CMTSPEED_1_1 .. CMTSPEED_25_14 (včetně).
     * CMTSPEED_NONE, CMTSPEED_CUSTOM (vlastní délky pulzů, nejde o poměr)
     * a CMTSPEED_COUNT nejsou platné.
     *
     * @param cmtspeed Rychlostní poměr k ověření.
     * @return 1 pokud je rychlost platný poměr, 0 jinak.
     */
    static inline int cmtspeed_is_valid ( en_CMTSPEED cmtspeed ) {
        return ( cmtspeed > CMTSPEED_NONE && cmtspeed < CMTSPEED_CUSTOM );
    }


    /**
     * @brief Zjistí, zda hodnota znamená vlastní délky pulzů.
     *
     * @param cmtspeed Hodnota rychlosti.
     * @return 1 pro CMTSPEED_CUSTOM, 0 jinak.
     */
    static inline int cmtspeed_is_custom ( en_CMTSPEED cmtspeed ) {
        return ( cmtspeed == CMTSPEED_CUSTOM );
    }


    /**
     * @brief Vrátí násobitel rychlosti pro zadaný rychlostní poměr.
     *
     * Např. pro CMTSPEED_2_1 vrací 2.0, pro CMTSPEED_7_3 vrací 2.333...
     *
     * @param cmtspeed Rychlostní poměr.
     * @return Násobitel jako double. Pro neplatné hodnoty a CMTSPEED_CUSTOM
     *         vrací 0.0.
     */
    static inline double cmtspeed_get_divisor ( en_CMTSPEED cmtspeed ) {
        if ( cmtspeed < 0 || cmtspeed >= CMTSPEED_COUNT ) return 0;
        return g_cmtspeed_divisor[cmtspeed];
    }


    /**
     * @brief Vypočítá baudovou rychlost pro zadaný poměr a základní rychlost.
     *
     * Výpočet: round(divisor * base_bdspeed). Typická base: 1200 Bd (MZ-800 standard).
     *
     * @param cmtspeed Rychlostní poměr.
     * @param base_bdspeed Základní baudová rychlost (typicky 1200).
     * @return Vypočtená baudová rychlost. Pro neplatné hodnoty a
     *         CMTSPEED_CUSTOM vrací 0.
     */
    static inline uint16_t cmtspeed_get_bdspeed ( en_CMTSPEED cmtspeed, uint16_t base_bdspeed ) {
        if ( cmtspeed < 0 || cmtspeed >= CMTSPEED_COUNT ) return 0;
        return (uint16_t) round ( g_cmtspeed_divisor[cmtspeed] * base_bdspeed );
    }


    /**
     * @brief Vrátí textový řetězec s poměrem rychlosti.
     *
     * Např. "7:3", "2:1 (cp/m)", pro CMTSPEED_CUSTOM "custom". Pro neplatné
     * hodnoty vrací "?:?".
     *
     * @param cmtspeed Rychlostní poměr.
     * @return Ukazatel na statický řetězec s poměrem.
     */
    static inline const char* cmtspeed_get_ratiotxt ( en_CMTSPEED cmtspeed ) {
        if ( cmtspeed < 0 || cmtspeed >= CMTSPEED_COUNT ) return "?:?";
        return g_cmtspeed_ratio[cmtspeed];
    }


    /**
     * @brief Formátuje řetězec s baudovou rychlostí.
     *
     * Výstup: "2400 Bd" nebo "2400 Bd (cp/m)" pro CP/M variantu.
     * Pro neplatné hodnoty a CMTSPEED_CUSTOM (Bd závisí na délkách pulzů,
     * které funkce nezná) "? Bd".
     *
     * @param dsttxt Cílový buffer pro výstupní řetězec.
     * @param size Velikost cílového bufferu v bajtech.
     * @param cmtspeed Rychlostní poměr.
     * @param base_bdspeed Základní baudová rychlost (typicky 1200).
     */
    static inline void cmtspeed_get_speedtxt ( char *dsttxt, int size, en_CMTSPEED cmtspeed, uint16_t base_bdspeed ) {
        if ( cmtspeed < 0 || cmtspeed >= CMTSPEED_COUNT || cmtspeed == CMTSPEED_CUSTOM ) {
            snprintf ( dsttxt, size, "? Bd" );
            return;
        }
        if ( cmtspeed == CMTSPEED_2_1_CPM ) {
            snprintf ( dsttxt, size, "%d Bd (cp/m)", cmtspeed_get_bdspeed ( cmtspeed, base_bdspeed ) );
        } else {
            snprintf ( dsttxt, size, "%d Bd", cmtspeed_get_bdspeed ( cmtspeed, base_bdspeed ) );
        };
    }


    /**
     * @brief Formátuje kombinovaný řetězec "poměr - rychlost Bd".
     *
     * Výstup: např. "7:3 - 2800 Bd". Pro neplatné hodnoty "?:? - ? Bd",
     * pro CMTSPEED_CUSTOM "custom - ? Bd".
     *
     * @param dsttxt Cílový buffer pro výstupní řetězec.
     * @param size Velikost cílového bufferu v bajtech.
     * @param cmtspeed Rychlostní poměr.
     * @param base_bdspeed Základní baudová rychlost (typicky 1200).
     */
    static inline void cmtspeed_get_ratiospeedtxt ( char *dsttxt, int size, en_CMTSPEED cmtspeed, uint16_t base_bdspeed ) {
        if ( cmtspeed < 0 || cmtspeed >= CMTSPEED_COUNT ) { snprintf ( dsttxt, size, "?:? - ? Bd" ); return; }
        if ( cmtspeed == CMTSPEED_CUSTOM ) {
            snprintf ( dsttxt, size, "%s - ? Bd", cmtspeed_get_ratiotxt ( cmtspeed ) );
            return;
        }
        snprintf ( dsttxt, size, "%s - %d Bd", cmtspeed_get_ratiotxt ( cmtspeed ), cmtspeed_get_bdspeed ( cmtspeed, base_bdspeed ) );
    }

    /**
     * @brief Najde nejbližší en_CMTSPEED pro zadanou baudovou rychlost.
     *
     * Porovná zadanou rychlost se všemi platnými hodnotami
     * CMTSPEED_1_1 .. CMTSPEED_25_14 (CMTSPEED_CUSTOM nemá Bd, proto se
     * neprochází); Bd kandidáta počítá cmtspeed_get_bdspeed() z base_bdspeed.
     * Vrátí hodnotu s nejmenší absolutní odchylkou. Při shodné odchylce
     * vyhrává hodnota, která je v enumu dřív (např. CMTSPEED_2_1 před
     * CMTSPEED_2_1_CPM, obě mají při základu 1200 Bd shodně 2400 Bd).
     *
     * @param bdspeed Baudová rychlost (např. 2400, 2800, 3200).
     * @param base_bdspeed Základní baudová rychlost (typicky 1200).
     * @return Nejbližší platná en_CMTSPEED hodnota; CMTSPEED_NONE, pokud
     *         bdspeed nebo base_bdspeed je 0.
     */
    static inline en_CMTSPEED cmtspeed_from_bdspeed ( uint16_t bdspeed, uint16_t base_bdspeed ) {
        if ( bdspeed == 0 || base_bdspeed == 0 ) return CMTSPEED_NONE;

        en_CMTSPEED best = CMTSPEED_1_1;
        double best_diff = 1e9;

        for ( int s = CMTSPEED_1_1; s < CMTSPEED_CUSTOM; s++ ) {
            uint16_t candidate = cmtspeed_get_bdspeed ( ( en_CMTSPEED ) s, base_bdspeed );
            double diff = fabs ( ( double ) bdspeed - ( double ) candidate );
            if ( diff < best_diff ) {
                best_diff = diff;
                best = ( en_CMTSPEED ) s;
            }
        }

        return best;
    }


    /** @brief Verze knihovny cmtspeed. */
#define CMTSPEED_VERSION "2.1.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny cmtspeed.
     * @return Statický řetězec s verzí knihovny (hodnota CMTSPEED_VERSION).
     */
    extern const char* cmtspeed_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* CMTSPEED_H */

