/* 
 * File:   pio8255.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 21. června 2015, 22:56
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

#ifndef PIO8255_H
#define PIO8255_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "hw-generic/gdg/gdgclk.h"

#if MZARCH != 700
#define CTC_AUDIO_MASK  ( g_pio8255.signal_pc00 )
#else
#define CTC_AUDIO_MASK  1
#endif


    /** @brief Stav "probe" pro ověření dosednutí virtuální klávesy.
     *
     * Umožňuje MCP/HID vrstvě zjistit, zda guest během držení vstříknuté
     * klávesy opravdu naskenoval cílový sloupec klávesnicové matice
     * (= dostal vstříknutý bit přes Port B). Slouží jako reálný signál
     * `landing_verified` (fix 0016 / cesta A) místo dřívějšího natvrdo
     * false.
     *
     * Invarianty:
     *  - Pokud `active == false`, probe je neaktivní a hot path Port B
     *    read jej vůbec nevyhodnocuje (= žádný overhead, viz hook
     *    v pio8255_read PORTB).
     *  - `col` je v rozsahu 0..9 (cílový sloupec, kde leží vstříknutá
     *    klávesa). `bit` se neporovnává (stačí, že guest skenuje sloupec,
     *    fyzicky tak vidí všechny jeho bity včetně našeho).
     *  - `seen` se nastaví na true jakmile guest přečte Port B se
     *    selektovaným sloupcem == col; jednou nastavené už se nemaže
     *    (sticky až do disarm/arm).
     *
     * Lifetime: arm -> (emu běží, guest čte) -> check -> disarm. Probe
     * je čistě pozorovací, nemění návratovou hodnotu Port B ani matice.
     */
    typedef struct st_PIO8255_VKBD_PROBE {
        bool active;    /**< true = probe sleduje čtení Port B */
        uint8_t col;    /**< cílový sloupec klávesnice (0..9) */
        uint8_t bit;    /**< cílový bit ve sloupci (jen informativně) */
        bool seen;      /**< true = guest naskenoval sloupec col */
    } st_PIO8255_VKBD_PROBE;

    typedef struct st_PIO8255 {
        uint8_t keyboard_matrix [ 10 ];
        uint8_t vkbd_matrix [ 10 ];
        st_PIO8255_VKBD_PROBE vkbd_probe; /* readback dosednutí vkbd klávesy (fix 0016) */
        char *vkbd_autotype;
        int vkbd_autotype_col;
        uint8_t vkbd_autotype_ret;
        bool vkbd_autotype_ret_shift;
        int vkbd_autotype_keydown;
        double vkbd_autotype_kd_ms; // key down ms
        uint64_t vkbd_autotype_kd_ticks; // key down ticks
        double vkbd_autotype_ku_ms; // key up ms
        uint64_t vkbd_autotype_ku_ticks; // key up ticks
        uint64_t vkbd_autotype_start_ticks;
        unsigned signal_PA; /* Vystupni port A */
        unsigned signal_PA_keybord_column; /* vzorkovani klavesnice: 0. - 3. bit (0 - 9), 10 = zadny sloupec (PA0-3 = 10 - 15) */
        unsigned signal_PA_joy1_enabled; /* vzorkování JOY1 (IN 0F0h): 1 = PA4 v L; čte jen IORQ MZ-800 */
        unsigned signal_PA_joy2_enabled; /* vzorkování JOY2 (IN 0F1h): 1 = PA5 v L; čte jen IORQ MZ-800 */
        unsigned signal_PC; /* Port C: 0 - 3 je vystup, 4 - 7 je vstup */
        unsigned signal_pc00; /* OUT: blokovani zvuku z CTC0 - jen u MZ-800 a MZ-1500 */
        unsigned signal_pc01; /* OUT: data do CMT */
        unsigned signal_pc02; /* OUT: blokovani preruseni z CTC2, 0 = zakazano rusit */
        unsigned signal_pc03; /* OUT: rizeni motoru CMT - zmenu provede nabezna hrana */
        unsigned signal_pc04; /* IN: stav motoru  */
        //unsigned signal_pc05; /* IN: data z CMT */
        //unsigned signal_pc06; /* IN: blikani kurzoru NE556@OUT */
        //unsigned signal_pc07; /* IN: VBLN */
        uint8_t last_cw_byte; /* debug/UI mirror: posledni byte zapsany na Control port
                               * (DEF_PIO8255_MASTER, IORQ 0xD3 / MMIO 0xE003).
                               * 8255 CW je sequencer, ne readable register - hardware
                               * to neumi precist. Tracking pouze pro debug zobrazeni
                               * v I/O Ports Overview. Bit 7 = 1 znamena Mode Set
                               * (default 0x8A v init sekvenci), bit 7 = 0 znamena
                               * Bit Set/Reset (bit 3-1 vybira PC bit, bit 0 hodnota).
                               * Default 0x00 dokud HW init nebyl proveden. */
    } st_PIO8255;

    extern st_PIO8255 g_pio8255;


    extern void pio8255_init ( void );
    extern void pio8255_keyboard_matrix_reset ( void );

    /** @brief Reset virtuální klávesnice (vkbd_matrix) na klidový stav.
     *
     * Vyplní `g_pio8255.vkbd_matrix` hodnotami 0xff (= žádná klávesa
     * stisknutá; bit aktivní = 0). Použití: light input-reset pro
     * vyčištění přiklepeného vkbd bitu. Definice v pio8255.c. */
    extern void pio8255_vkbd_matrix_reset ( void );
    /**
     * @brief Nastaví dobu stisku klávesy autotype.
     * @param ms Doba v ms.
     * @pre Emu vlákno (GUI ji mění přes replay_host_autotype_delays()) nebo
     *      inicializace.
     */
    extern void pio8255_autotype_set_key_down_ms ( double ms );

    /**
     * @brief Nastaví dobu uvolnění klávesy autotype.
     * @param ms Doba v ms.
     * @pre Emu vlákno (GUI ji mění přes replay_host_autotype_delays()) nebo
     *      inicializace.
     */
    extern void pio8255_autotype_set_key_up_ms ( double ms );

    /**
     * @brief Spustí autotype textu, nebo ho zastaví.
     *
     * Spuštění zvýší generaci autotype a vynuluje počet napsaných bajtů,
     * zastavení i přirozený konec textu ji označí jako dokončenou (viz
     * pio8255_autotype_progress()).
     *
     * @param txt Text (ukončený nulou) nebo NULL = zastavit. Text musí žít,
     *        dokud autotype neskončí nebo ho nenahradí další volání; vlastní
     *        ho volající (replay.c).
     * @pre Emu vlákno (GUI autotype spouští přes replay_host_autotype()).
     */
    extern void pio8255_set_autotype ( char *txt );

    /**
     * @brief Průběh autotype pro GUI (atomická čtení, libovolné vlákno).
     *
     * @param[out] gen Generace posledního spuštění (roste s každým
     *        spuštěním, smí být NULL).
     * @param[out] done_gen Generace posledního dokončeného nebo zastaveného
     *        autotype (== gen: nic neběží; smí být NULL).
     * @param[out] typed Počet zpracovaných bajtů textu poslední generace
     *        (po přirozeném konci délka textu; smí být NULL).
     * @note Tři hodnoty se čtou každá zvlášť; mezi nimi se smí změnit.
     */
    extern void pio8255_autotype_progress ( int *gen, int *done_gen, int *typed );
    extern int pio8255_autotype_get_matrix ( char c, uint8_t *ret, bool *ret_shift );
    extern uint8_t pio8255_read ( int addr );

    /**
     * @brief Čtení portu 8255 bez vedlejšího efektu (debugger, mapované 0E000h-0E003h).
     *
     * Vrací totéž co pio8255_read() pro stejný stav, ale stav nemění:
     *  - Port B: autotype se neposune (hodnota jako při čtení CPU teď),
     *    nezpracují se čekající události klávesnice hostitele
     *    (iface_keyboard_pool_keyboard_events) a vkbd probe se neoznačí.
     *    Bez autotype vrací matici z posledního zpracování událostí.
     *  - Port C: data z pásky (bit 5) z poslední synchronizace pásky,
     *    bez cmt_update_output (ten může přejít na další blok nebo do STOP).
     *  - Port A a nepodporovaná adresa jako pio8255_read().
     *
     * @param addr DEF_PIO8255_PORTA / PORTB / PORTC (spodní 2 bity adresy).
     * @return Hodnota portu.
     * @note Hodnota se od čtení CPU v tomtéž okamžiku může lišit jen tam,
     *       kde read nejdřív dosynchronizuje vstup (klávesnice hostitele,
     *       páska); jinak je shodná.
     * @pre Emu vlákno, nebo pauza (čte sdílený stav bez zámku jako IO mirror).
     */
    extern uint8_t pio8255_peek ( int addr );
    extern void pio8255_write ( int addr, uint8_t value );

    /** @brief Aktivuje probe pro ověření dosednutí vkbd klávesy.
     *
     * Nastaví `g_pio8255.vkbd_probe` na active s cílovým sloupcem/bitem
     * a vynuluje `seen`. Od tohoto okamžiku každé čtení Port B se
     * selektovaným sloupcem == col nastaví `seen` na true.
     *
     * @param[in] col cílový sloupec klávesnice (0..9). Mimo rozsah =
     *                no-op (probe zůstane neaktivní).
     * @param[in] bit cílový bit (jen informativně, neporovnává se).
     *
     * @note Volat z host/MCP vlákna před vstříknutím klávesy. Probe je
     *       pozorovací, nemodifikuje matice ani Port B.
     */
    extern void pio8255_vkbd_probe_arm ( int col, int bit );

    /** @brief Vrátí, zda guest naskenoval sledovaný sloupec.
     *
     * @return true pokud byl od posledního arm Port B přečten se
     *         selektovaným sloupcem == probe.col (= klávesa dosedla);
     *         false jinak (vč. neaktivní probe).
     */
    extern bool pio8255_vkbd_probe_check ( void );

    /** @brief Deaktivuje probe (active = false).
     *
     * Po návratu hot path Port B read probe opět ignoruje. `seen`
     * hodnota se nemaže (lze ji ještě přečíst přes check do dalšího arm).
     */
    extern void pio8255_vkbd_probe_disarm ( void );

    extern void pio8255_pc2_set ( int value );
    extern int pio8255_pc1_get ( void );
    extern int pio8255_pc2_get ( void );
    extern int pio8255_pc4_get ( void );
    extern int pio8255_pa4_get ( void );
    extern int pio8255_pa5_get ( void );
    extern int pio8255_pa0_3_get ( void );

    /* priprava klavesove matrix */
    extern void interface_keyboard_init ( void );

    /* vrati *keyboard_matrix */
    extern uint8_t* interface_keyboard_scan ( void );

#define PIO8255_KEYBOARD_MATRIX_RESET()  pio8255_keyboard_matrix_reset()
#define PIO8255_VKBD_MATRIX_RESET()  pio8255_vkbd_matrix_reset()

#define PIO8255_MZKEYBIT_RESET( column, bit ) g_pio8255.keyboard_matrix [ column ] &= ~ ( 1 << bit );
#define PIO8255_VKBDBIT_RESET( column, bit ) g_pio8255.vkbd_matrix [ column ] &= ~ ( 1 << bit );
#define PIO8255_VKBDBIT_SET( column, bit ) g_pio8255.vkbd_matrix [ column ] |= ( 1 << bit );

// Pokud je aktivni, tak je to 0
#define PIO8255_MZKEY_BIT_GET( column, bit ) ( g_pio8255.keyboard_matrix [ column ] & ( 1 << bit ) )
#define PIO8255_VKBD_BIT_GET( column, bit ) ( g_pio8255.vkbd_matrix [ column ] & ( 1 << bit ) )
#define PIO8255_KBDALL_BIT_GET( column, bit ) ( PIO8255_MZKEY_BIT_GET( column, bit ) & PIO8255_VKBD_BIT_GET( column, bit ) )


#define PIO8255_VKBDAUTO_LIBRA  (char) ( 0xc0 | 0x00 )
#define PIO8255_VKBDAUTO_PI     (char) ( 0xc0 | 0x01 )

#ifdef __cplusplus
}
#endif

#endif /* PIO8255_H */

