/*
 * File:   mz800_vramwait.h
 * Author: Michal Hucik <hucik@ordoz.com>
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
 * @file mz800_vramwait.h
 * @brief Model WAIT (nWTGD) u přístupu CPU do VRAM přes GDG - čisté funkce.
 *
 * GDG dělí čas DRAM na sloty po 8 CLK0. Ve video slotech čte data pro obraz,
 * v CPU slotech obslouží CPU (a když CPU nic nechce, čte naprázdno). Model
 * pracuje jen s rozvrhem CPU slotů a s okamžikem, kdy se uvolní WRITE FF:
 *
 * - **Čtení** (READ FF od T1f): GDG data přečte v prvním CPU slotu, který
 *   začne nejdřív 5 CLK0 po T1r a až po uvolnění WRITE FF předchozího zápisu.
 *   READ FF se uvolní 9 CLK0 po začátku slotu. Každé čtení tedy čeká.
 * - **Zápis** jde přes jednobajtový buffer: sám čeká jen na uvolnění WRITE FF
 *   předchozího zápisu. Do DRAM se zapíše v prvním CPU slotu, který začne
 *   nejdřív 13 CLK0 po T1r (po případném WAIT), v 320x200 ještě v dalším CPU
 *   slotu (sloupec s+40h). WRITE FF se uvolní 9 CLK0 po začátku posledního
 *   zápisového slotu.
 * - **WAIT FF** vzorkuje na náběžných hranách T2r, TW1r, ...; Z80 je vidí
 *   na následující sestupné hraně. Počet TW = počet náběžných hran od T2r
 *   (T1r + 5 CLK0) ostře před uvolněním FF.
 *
 * Rozvrh CPU slotů (CLK0 od sestupné hrany nHSYN, stejný na všech řádcích
 * včetně svislého zatemnění):
 *
 * | Režim | CPU sloty |
 * |-------|-----------|
 * | MZ-800 (DMD 00h-07h) | 0-320 po 8, 336-944 po 16 (střídání s video sloty), 960-1128 po 8 |
 * | MZ-700 (DMD 08h-0Fh) | 0-312 po 8, 960-1128 po 8 (viditelná část bez CPU slotů) |
 *
 * Zdroj: simulace netlistu GDG (C model `c-whid-0` z projektu gdg-mz800)
 * s časováním sběrnice Z80 podle Zilog UM0080 (/RD na T1f). Model se shoduje
 * se simulací ve všech 129 504 měřeních (DMD 00h, 04h, 08h; začátek přístupu
 * na každé pozici řádku; samostatné čtení a zápis, dvojice W+R, W+W, R+R,
 * R+W s mezerou 0-8 T). Simulace má krok 1/2 CLK0 a sloty ve viditelné části
 * začínají na půl CLK0; model je zaokrouhluje na celé CLK0 tak, že výsledné
 * počty TW sedí.
 *
 * Měření na MZ-800 (analyzátor, 18 000 přístupů: čtení, zápis, W+R, W+W, R+R
 * ve třech režimech a všech fázích řádku; emu-experiments gdg-meas, třetí
 * měření) model potvrdilo. Jediná odchylka od simulace: uvolnění WRITE FF
 * je na stroji o 1 CLK0 později (VRAMWAIT_WRITE_RELEASE_CLK0 = 9), což se
 * projeví u dvou zápisů těsně za sebou. S touto hodnotou sedí 18 000
 * z 18 003 změřených přístupů.
 *
 * Časová osa: všechny časy jsou absolutní (CLK0). Pozice v řádku se počítá
 * od začátku řádku modelu (sestupná hrana nHSYN), který předává volající
 * spolu s délkou řádku (st_VRAMWAIT_LINE). Řádek má VRAMWAIT_LINE_CLK0, při
 * CKSW = 1 o 16 CLK0 méně; rozvrh slotů od hrany nHSYN je v obou případech
 * stejný, krátký řádek jen dřív končí (simulace netlistu s CKSW = 1,
 * emu-experiments superimpose/gdgsim). Funkce bez přípony _ln počítají
 * s řádky pevné délky, kde `t % VRAMWAIT_LINE_CLK0` je pozice od nHSYN.
 * Převod z času emulátoru dělá volající (gdg_get_vramwait_ticks()
 * a gdg_get_vramwait_line() v mz800_gdg.h).
 */

#ifndef MZ800_VRAMWAIT_H
#define MZ800_VRAMWAIT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

/** @brief Délka řádku PAL v CLK0 (bez zkrácení bitem CKSW). */
#define VRAMWAIT_LINE_CLK0 1136u

/** @brief Rozteč CPU slotů v zatemnění (CLK0). */
#define VRAMWAIT_BLANK_SLOT_CLK0 8u

/** @brief Délka T-stavu Z80 v CLK0. */
#define VRAMWAIT_TSTATE_CLK0 5u

/** @brief Nejmenší odstup začátku CPU slotu, který obslouží čtení, od T1r. */
#define VRAMWAIT_READ_LEAD_CLK0 5u

/** @brief Uvolnění READ FF za začátkem CPU slotu, který četl. */
#define VRAMWAIT_READ_RELEASE_CLK0 9u

/** @brief Nejmenší odstup začátku prvního zápisového CPU slotu od T1r zápisu. */
#define VRAMWAIT_WRITE_LEAD_CLK0 13u

/**
 * @brief Uvolnění WRITE FF za začátkem posledního zápisového CPU slotu.
 *
 * Simulace netlistu dávala 8 CLK0. Měření na MZ-800 (emu-experiments gdg-meas,
 * třetí měření, 3600 dvojic zápisů `LD (nn),HL` ve třech režimech) sedí
 * s hodnotou 9 ve všech případech, s hodnotou 8 ve 2911 a s hodnotou 10 ve 2962.
 */
#define VRAMWAIT_WRITE_RELEASE_CLK0 9u

    /**
     * @brief Rozvrh CPU slotů v řádku.
     */
    typedef enum en_VRAMWAIT_GRID
    {
        VRAMWAIT_GRID_MZ800 = 0, /**< grafické režimy MZ-800 (DMD bit 3 = 0) */
        VRAMWAIT_GRID_MZ700 = 1, /**< režim MZ-700 (DMD bit 3 = 1) */
    } en_VRAMWAIT_GRID;

    /**
     * @brief Řádek modelu: začátek (sestupná hrana nHSYN) a délka.
     *
     * Invarianty: len je násobek VRAMWAIT_BLANK_SLOT_CLK0 a je větší než 960
     * (konec viditelné části). Časy před start nejsou přípustné. Řádky za
     * tímto řádkem model bere jako stejně dlouhé.
     */
    typedef struct st_VRAMWAIT_LINE
    {
        uint64_t start; /**< absolutní čas sestupné hrany nHSYN řádku (CLK0) */
        unsigned len;   /**< délka řádku v CLK0 (1136, s CKSW = 1 1120) */
    } st_VRAMWAIT_LINE;

    /**
     * @brief Začátek prvního CPU slotu, který začne v čase @p t nebo později.
     *
     * @param t Absolutní čas v CLK0.
     * @param grid Rozvrh slotů podle režimu.
     * @param ln Řádek, od jehož začátku se počítá pozice.
     * @return Absolutní čas začátku slotu (>= @p t), případně slot 0
     *         následujícího řádku.
     * @pre @p ln != NULL, @p t >= ln->start.
     */
    static inline uint64_t vramwait_next_cpu_slot_ln(uint64_t t, en_VRAMWAIT_GRID grid, const st_VRAMWAIT_LINE *ln)
    {
        uint64_t base = ln->start;
        uint64_t rel = t - base;
        while (rel >= ln->len)
        {
            rel -= ln->len;
            base += ln->len;
        };
        unsigned pos = (unsigned) rel;
        unsigned slot;
        unsigned blank_end = (grid == VRAMWAIT_GRID_MZ800) ? 320u : 312u;

        if (pos <= blank_end)
        {
            slot = (pos + 7u) & ~7u;
        }
        else if (pos <= 960u)
        {
            if (grid == VRAMWAIT_GRID_MZ700)
            {
                slot = 960u;
            }
            else if (pos <= 336u)
            {
                slot = 336u;
            }
            else
            {
                slot = 336u + ((pos - 336u + 15u) & ~15u);
            };
        }
        else if (pos <= ln->len - VRAMWAIT_BLANK_SLOT_CLK0)
        {
            slot = (pos + 7u) & ~7u;
        }
        else
        {
            slot = ln->len;
        };
        return base + slot;
    }

    /**
     * @brief Řádek pevné délky VRAMWAIT_LINE_CLK0, ve kterém leží čas @p t.
     *
     * @param t Absolutní čas v CLK0 v doméně, kde `t % VRAMWAIT_LINE_CLK0`
     *        je pozice od nHSYN.
     * @return Řádek se začátkem <= @p t.
     */
    static inline st_VRAMWAIT_LINE vramwait_fixed_line(uint64_t t)
    {
        st_VRAMWAIT_LINE ln;
        ln.start = t - (t % VRAMWAIT_LINE_CLK0);
        ln.len = VRAMWAIT_LINE_CLK0;
        return ln;
    }

    /**
     * @brief Jako vramwait_next_cpu_slot_ln() pro řádky pevné délky.
     *
     * @param t Absolutní čas v CLK0 (doména nHSYN, viz popis souboru).
     * @param grid Rozvrh slotů podle režimu.
     * @return Absolutní čas začátku slotu (>= @p t).
     */
    static inline uint64_t vramwait_next_cpu_slot(uint64_t t, en_VRAMWAIT_GRID grid)
    {
        st_VRAMWAIT_LINE ln = vramwait_fixed_line(t);
        return vramwait_next_cpu_slot_ln(t, grid, &ln);
    }

    /**
     * @brief Počet WAIT stavů, než se uvolní FF v čase @p release.
     *
     * WAIT FF vzorkuje na náběžných hranách T1r + 5 + 5n (n >= 0).
     *
     * @param t1r Náběžná hrana T1 přístupu (CLK0).
     * @param release Okamžik uvolnění READ/WRITE FF (CLK0).
     * @return Počet náběžných hran ostře před @p release (0, pokud je
     *         @p release nejpozději v T2r).
     */
    static inline unsigned vramwait_tw_until(uint64_t t1r, uint64_t release)
    {
        uint64_t first = t1r + VRAMWAIT_TSTATE_CLK0;
        if (release <= first)
        {
            return 0;
        };
        return (unsigned) ((release - first + VRAMWAIT_TSTATE_CLK0 - 1u) / VRAMWAIT_TSTATE_CLK0);
    }

    /**
     * @brief Čtení CPU z VRAM: vrátí počet WAIT stavů.
     *
     * @param wrff_release Okamžik uvolnění WRITE FF posledního zápisu (CLK0,
     *        0 = žádný zápis); čtení ho nemění.
     * @param t1r Náběžná hrana T1 čtení (CLK0).
     * @param grid Rozvrh slotů podle aktuálního režimu.
     * @param ln Řádek, ve kterém (nebo za jehož začátkem) leží @p t1r.
     * @return Počet TW (každé čtení alespoň 2).
     * @pre @p wrff_release a @p t1r jsou ve stejné časové doméně;
     *      @p ln != NULL, @p t1r >= ln->start.
     */
    static inline unsigned vramwait_read_ln(uint64_t wrff_release, uint64_t t1r, en_VRAMWAIT_GRID grid, const st_VRAMWAIT_LINE *ln)
    {
        uint64_t from = t1r + VRAMWAIT_READ_LEAD_CLK0;
        if (wrff_release + 1u > from)
        {
            from = wrff_release + 1u;
        };
        uint64_t slot = vramwait_next_cpu_slot_ln(from, grid, ln);
        return vramwait_tw_until(t1r, slot + VRAMWAIT_READ_RELEASE_CLK0);
    }

    /**
     * @brief Jako vramwait_read_ln() pro řádky pevné délky VRAMWAIT_LINE_CLK0.
     *
     * @param wrff_release Okamžik uvolnění WRITE FF posledního zápisu (CLK0,
     *        0 = žádný zápis).
     * @param t1r Náběžná hrana T1 čtení (CLK0, doména nHSYN).
     * @param grid Rozvrh slotů podle aktuálního režimu.
     * @return Počet TW (každé čtení alespoň 2).
     */
    static inline unsigned vramwait_read(uint64_t wrff_release, uint64_t t1r, en_VRAMWAIT_GRID grid)
    {
        st_VRAMWAIT_LINE ln = vramwait_fixed_line(t1r);
        return vramwait_read_ln(wrff_release, t1r, grid, &ln);
    }

    /**
     * @brief Zápis CPU do VRAM: vrátí počet WAIT stavů a posune stav WRITE FF.
     *
     * @param wrff_release [in,out] Okamžik uvolnění WRITE FF: na vstupu
     *        z předchozího zápisu (0 = žádný), na výstupu z tohoto zápisu.
     * @param t1r Náběžná hrana T1 zápisu (CLK0).
     * @param grid Rozvrh slotů podle aktuálního režimu.
     * @param two_slots 1 = zápis do DRAM ve dvou CPU slotech (320x200),
     *        0 = v jednom (640x200, MZ-700).
     * @param ln Řádek, ve kterém (nebo za jehož začátkem) leží @p t1r.
     * @return Počet TW (0, pokud WRITE FF předchozího zápisu už je uvolněný).
     * @pre @p wrff_release != NULL; časy ve stejné doméně; @p ln != NULL,
     *      @p t1r >= ln->start.
     * @post *wrff_release > @p t1r.
     */
    static inline unsigned vramwait_write_ln(uint64_t *wrff_release, uint64_t t1r, en_VRAMWAIT_GRID grid, int two_slots, const st_VRAMWAIT_LINE *ln)
    {
        unsigned tw = vramwait_tw_until(t1r, *wrff_release);
        uint64_t slot = vramwait_next_cpu_slot_ln(t1r + (uint64_t) tw * VRAMWAIT_TSTATE_CLK0 + VRAMWAIT_WRITE_LEAD_CLK0, grid, ln);
        if (two_slots)
        {
            slot = vramwait_next_cpu_slot_ln(slot + 1u, grid, ln);
        };
        *wrff_release = slot + VRAMWAIT_WRITE_RELEASE_CLK0;
        return tw;
    }

    /**
     * @brief Jako vramwait_write_ln() pro řádky pevné délky VRAMWAIT_LINE_CLK0.
     *
     * @param wrff_release [in,out] Okamžik uvolnění WRITE FF.
     * @param t1r Náběžná hrana T1 zápisu (CLK0, doména nHSYN).
     * @param grid Rozvrh slotů podle aktuálního režimu.
     * @param two_slots 1 = zápis do DRAM ve dvou CPU slotech, 0 = v jednom.
     * @return Počet TW.
     * @pre @p wrff_release != NULL.
     */
    static inline unsigned vramwait_write(uint64_t *wrff_release, uint64_t t1r, en_VRAMWAIT_GRID grid, int two_slots)
    {
        st_VRAMWAIT_LINE ln = vramwait_fixed_line(t1r);
        return vramwait_write_ln(wrff_release, t1r, grid, two_slots, &ln);
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ800_VRAMWAIT_H */
