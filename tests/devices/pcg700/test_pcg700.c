/**
 * @file   test_pcg700.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Testy čistého jádra modelu PCG-700 (pcg700_core).
 *
 * Testuje:
 *   - převod adresy PCG RAM na index CG-ROM a bitové převrácení,
 *   - sekvence zápisu z manuálu HAL (`pcg700_05`): BASIC (uvolnění zápisem 0),
 *     assembler PRG1CH (uvolnění se zachovanou adresou), COPY z CG-ROM,
 *     sada 2 (+400h),
 *   - všechny tři režimy strobe (nástupná hrana, úroveň, sestupná hrana),
 *   - přepínače HSW, SSW, GSW a výklad SSW = OFF,
 *   - RESET vs. zapnutí napájení (obě volby obsahu RAM),
 *   - shodu plného a inkrementálního efektivního generátoru,
 *   - round-trip binárního stavu a odmítnutí poškozených dat.
 *
 * Standalone test - jádro nezávisí na emulátoru, linkuje se jen
 * pcg700_core.c + Unity. CG-ROM je syntetická (každý bajt jiný vzor), aby
 * test odhalil záměnu sady, kódu i řádku.
 *
 * @par Licence:
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
 */

#include "unity.h"

#include <string.h>
#include <stdint.h>

#include "mzarch/mz700/devices/pcg700/pcg700_core.h"


/** @brief Syntetická CG-ROM 4 kB (naplní setUp()). */
static uint8_t s_cgrom[PCG700_CGROM_SIZE];

/** @brief Testovaná instance (setUp() ji zapne s výchozí konfigurací). */
static st_PCG700 s_pcg;

/** @brief Vzor znaku z BASIC příkladu manuálu `pcg700_05` (formát PCG-700, bit 7 = levý bod). */
static const uint8_t c_MANUAL_GLYPH[8] = { 16, 126, 20, 62, 85, 73, 50, 0 };


/**
 * @brief Naplní CG-ROM vzorem, kde se liší sousední bajty, sady i kódy.
 */
static void fill_cgrom ( void ) {
    for ( unsigned i = 0; i < PCG700_CGROM_SIZE; i++ ) {
        s_cgrom[i] = (uint8_t) ( ( i * 7u ) ^ ( i >> 8 ) ^ 0x5Au );
    };
}


/** @brief Před každým testem: nová CG-ROM a zapnutí s výchozí konfigurací. */
void setUp ( void ) {
    fill_cgrom ( );
    memset ( &s_pcg, 0xA5, sizeof ( s_pcg ) );
    TEST_ASSERT_EQUAL_INT ( PCG700_OK, pcg700_core_power_on ( &s_pcg, NULL, s_cgrom ) );
}


/** @brief Úklid po testu (nic). */
void tearDown ( void ) {
}


/**
 * @brief Zapíše jeden bajt sekvencí BASIC příkladu manuálu (uvolnění zápisem 0).
 *
 * POKE 0E010h,A : POKE 0E011h,AD AND 255 : POKE 0E012h,INT(AD/256)+16 :
 * POKE 0E012h,0.
 *
 * @param addr Adresa PCG RAM 0 - 7FFh.
 * @param data Bajt ve formátu PCG-700.
 * @return OR událostí všech čtyř zápisů.
 */
static unsigned basic_write ( unsigned addr, uint8_t data ) {
    unsigned evt = 0;
    evt |= pcg700_core_write ( &s_pcg, PCG700_REG_DATA, data, s_cgrom );
    evt |= pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, (uint8_t) ( addr & 0xFF ), s_cgrom );
    evt |= pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, (uint8_t) ( ( addr >> 8 ) + 16 ), s_cgrom );
    evt |= pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0, s_cgrom );
    return evt;
}


/**
 * @brief Zkopíruje jeden bajt z CG-ROM sekvencí BASIC příkladu (+16 +32, uvolnění 0).
 * @param addr Adresa PCG RAM 0 - 7FFh (bit 10 = sada).
 */
static void basic_copy ( unsigned addr ) {
    pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, (uint8_t) ( addr & 0xFF ), s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, (uint8_t) ( ( addr >> 8 ) + 16 + 32 ), s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0, s_cgrom );
}


/**
 * @brief Ověří, že efektivní generátor odpovídá CG-ROM s RAM v horních polovinách.
 */
static void assert_effective_consistent ( void ) {
    for ( unsigned i = 0; i < PCG700_CGROM_SIZE; i++ ) {
        if ( i & 0x400 ) {
            unsigned a = ( ( i & 0x800 ) >> 1 ) | ( i & 0x3FF );
            TEST_ASSERT_EQUAL_HEX8_MESSAGE ( s_pcg.ram[a], s_pcg.effective[i], "effective != ram" );
        } else {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE ( s_cgrom[i], s_pcg.effective[i], "effective != cgrom" );
        };
    };
}


/** @brief Konfigurace instance se zadaným režimem strobe (ostatní výchozí). */
static void set_strobe ( en_PCG700_STROBE mode ) {
    st_PCG700_CONFIG cfg = s_pcg.cfg;
    cfg.strobe = mode;
    pcg700_core_set_config ( &s_pcg, &cfg );
}


/* ========================================================================
 * Pomocné převody
 * ======================================================================== */

/** @brief Bitové převrácení: známé hodnoty a involuce na všech 256 bajtech. */
void test_bitrev ( void ) {
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, pcg700_core_bitrev ( 0x80 ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x08, pcg700_core_bitrev ( 0x10 ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x7C, pcg700_core_bitrev ( 0x3E ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x4C, pcg700_core_bitrev ( 0x32 ) );
    for ( unsigned v = 0; v < 256; v++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( v, pcg700_core_bitrev ( pcg700_core_bitrev ( (uint8_t) v ) ) );
    };
}


/** @brief Adresa PCG RAM -> index CG-ROM (sada 1 od 400h, sada 2 od 0C00h). */
void test_effective_index ( void ) {
    TEST_ASSERT_EQUAL_HEX ( 0x400, pcg700_core_effective_index ( 0x000 ) );
    TEST_ASSERT_EQUAL_HEX ( 0x7FF, pcg700_core_effective_index ( 0x3FF ) );
    TEST_ASSERT_EQUAL_HEX ( 0xC00, pcg700_core_effective_index ( 0x400 ) );
    TEST_ASSERT_EQUAL_HEX ( 0xFFF, pcg700_core_effective_index ( 0x7FF ) );
    /* kód 239 (0EFh) sady 1, řádek 0: (239 - 128) * 8 = 888 */
    TEST_ASSERT_EQUAL_HEX ( 239 * 8, pcg700_core_effective_index ( 888 ) );
    /* kód 0A3h sady 2, řádek 5 */
    TEST_ASSERT_EQUAL_HEX ( 0x800 + 0xA3 * 8 + 5, pcg700_core_effective_index ( 0x400 + ( 0xA3 - 0x80 ) * 8 + 5 ) );
    /* adresa se ořízne na 11 bitů */
    TEST_ASSERT_EQUAL_HEX ( 0x400, pcg700_core_effective_index ( 0x800 ) );
}


/* ========================================================================
 * Zapnutí, reset
 * ======================================================================== */

/** @brief Po zapnutí: latche výchozí, PCG vypnuto bitem 3, RAM nulová, generátor = CG-ROM + nuly. */
void test_power_on_defaults ( void ) {
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.data );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.addr_lo );
    TEST_ASSERT_EQUAL_HEX8 ( PCG700_CTRL_RESET_VALUE, s_pcg.ctrl );
    TEST_ASSERT_EQUAL_INT ( PCG700_STROBE_RISING, s_pcg.cfg.strobe );
    TEST_ASSERT_EQUAL_INT ( PCG700_HSW_PCG, s_pcg.cfg.hsw );
    TEST_ASSERT_EQUAL_INT ( PCG700_SSW_ON, s_pcg.cfg.ssw );
    TEST_ASSERT_EQUAL_INT ( PCG700_GSW_ON, s_pcg.cfg.gsw );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );
    for ( unsigned a = 0; a < PCG700_RAM_SIZE; a++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( 0, s_pcg.ram[a] );
    };
    assert_effective_consistent ( );
    TEST_ASSERT_EQUAL_PTR ( s_cgrom, pcg700_core_glyphs ( &s_pcg, s_cgrom ) );
}


/** @brief Zapnutí s volbou kopie CG-ROM: RAM = glyfy 80h - 0FFh, generátor = CG-ROM. */
void test_power_on_ram_cgrom ( void ) {
    st_PCG700_CONFIG cfg;
    pcg700_core_config_default ( &cfg );
    cfg.poweron_ram = PCG700_POWERON_RAM_CGROM;
    TEST_ASSERT_EQUAL_INT ( PCG700_OK, pcg700_core_power_on ( &s_pcg, &cfg, s_cgrom ) );
    for ( unsigned a = 0; a < PCG700_RAM_SIZE; a++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( s_cgrom[pcg700_core_effective_index ( a )], s_pcg.ram[a] );
    };
    TEST_ASSERT_EQUAL_MEMORY ( s_cgrom, s_pcg.effective, PCG700_CGROM_SIZE );
}


/** @brief Neplatná konfigurace a NULL instance se při zapnutí odmítnou. */
void test_power_on_invalid ( void ) {
    st_PCG700_CONFIG cfg;
    pcg700_core_config_default ( &cfg );
    cfg.strobe = PCG700_STROBE_COUNT;
    TEST_ASSERT_FALSE ( pcg700_core_config_valid ( &cfg ) );
    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_INVALID, pcg700_core_power_on ( &s_pcg, &cfg, s_cgrom ) );
    TEST_ASSERT_EQUAL_INT ( PCG700_STROBE_RISING, s_pcg.cfg.strobe );
    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_INVALID, pcg700_core_power_on ( NULL, NULL, s_cgrom ) );
    TEST_ASSERT_FALSE ( pcg700_core_config_valid ( NULL ) );
}


/** @brief RESET: latche na výchozí, PCG vypnuto, RAM a glyfy zůstávají. */
void test_reset_keeps_ram ( void ) {
    basic_write ( 888, 0x80 );
    pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0x77, s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, 0x55, s_cgrom );
    TEST_ASSERT_TRUE ( pcg700_core_display_enabled ( &s_pcg ) );

    unsigned evt = pcg700_core_reset ( &s_pcg );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, evt );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.data );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.addr_lo );
    TEST_ASSERT_EQUAL_HEX8 ( PCG700_CTRL_RESET_VALUE, s_pcg.ctrl );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, s_pcg.ram[888] );
    assert_effective_consistent ( );

    /* druhý reset už zobrazení nemění */
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_reset ( &s_pcg ) );

    /* zapnutí napájení RAM vynuluje */
    pcg700_core_power_on ( &s_pcg, &s_pcg.cfg, s_cgrom );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.ram[888] );
    assert_effective_consistent ( );
}


/** @brief RESET při volbě sestupné hrany a nastaveném strobe nic nezapíše. */
void test_reset_falling_no_write ( void ) {
    set_strobe ( PCG700_STROBE_FALLING );
    pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0xFF, s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x10, s_cgrom );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, pcg700_core_reset ( &s_pcg ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.ram[0] );
}


/* ========================================================================
 * Sekvence z manuálu, nástupná hrana (výchozí)
 * ======================================================================== */

/** @brief BASIC příklad `pcg700_05`: znak 239 do sady 1, bity převrácené, PCG se zapne. */
void test_manual_basic_program ( void ) {
    unsigned first = pcg700_core_write ( &s_pcg, PCG700_REG_DATA, c_MANUAL_GLYPH[0], s_cgrom );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, first );

    unsigned all = first;
    for ( unsigned i = 0; i < 8; i++ ) {
        unsigned ad = ( 239 - 128 ) * 8 + i;
        all |= basic_write ( ad, c_MANUAL_GLYPH[i] );
    };
    TEST_ASSERT_TRUE ( all & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_TRUE ( all & PCG700_EVT_GLYPH_CHANGED );
    TEST_ASSERT_TRUE ( all & PCG700_EVT_DISPLAY_CHANGED );

    for ( unsigned i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( pcg700_core_bitrev ( c_MANUAL_GLYPH[i] ), s_pcg.ram[888 + i] );
        TEST_ASSERT_EQUAL_HEX8 ( pcg700_core_bitrev ( c_MANUAL_GLYPH[i] ), s_pcg.effective[239 * 8 + i] );
    };
    /* řádek 6 = 32h = 00110010b: levý bod bit 7 -> bit 0 formátu CG-ROM = 4Ch */
    TEST_ASSERT_EQUAL_HEX8 ( 0x4C, s_pcg.ram[888 + 6] );
    /* poslední POKE 0E012h,0 nechal bit 3 = 0: PCG zapnuto */
    TEST_ASSERT_TRUE ( pcg700_core_display_enabled ( &s_pcg ) );
    TEST_ASSERT_EQUAL_PTR ( s_pcg.effective, pcg700_core_glyphs ( &s_pcg, s_cgrom ) );
    assert_effective_consistent ( );
    /* nikam jinam se nezapisovalo */
    for ( unsigned a = 0; a < PCG700_RAM_SIZE; a++ ) {
        if ( a < 888 || a > 895 ) {
            TEST_ASSERT_EQUAL_HEX8 ( 0, s_pcg.ram[a] );
        };
    };
}


/** @brief Assembler PRG1CH (`pcg700_05`): uvolnění strobe se zachovanou adresou. */
void test_manual_asm_prg1ch ( void ) {
    /* A = 0EFh: ADD A,A přeteče (1DEh -> 0DEh), pak HL * 4 = (0EFh - 80h) * 8 */
    unsigned de = ( (unsigned) ( (uint8_t) ( 0xEF * 2 ) ) ) * 4;
    TEST_ASSERT_EQUAL_HEX ( 888, de );
    for ( unsigned i = 0; i < 8; i++, de++ ) {
        uint8_t d = (uint8_t) ( de >> 8 );
        pcg700_core_write ( &s_pcg, PCG700_REG_DATA, c_MANUAL_GLYPH[i], s_cgrom );
        pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, (uint8_t) de, s_cgrom );
        pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, (uint8_t) ( d + 16 ), s_cgrom );
        pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, d, s_cgrom );
    };
    for ( unsigned i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( pcg700_core_bitrev ( c_MANUAL_GLYPH[i] ), s_pcg.ram[888 + i] );
    };
    assert_effective_consistent ( );
}


/** @brief COPY (+30h): znak 163 z CG-ROM sady 1, bez převracení, data latch se nepoužije. */
void test_manual_copy ( void ) {
    pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0xFF, s_cgrom );
    for ( unsigned i = 0; i < 8; i++ ) {
        basic_copy ( ( 163 - 128 ) * 8 + i );
    };
    for ( unsigned i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( s_cgrom[163 * 8 + i], s_pcg.ram[( 163 - 128 ) * 8 + i] );
    };
    assert_effective_consistent ( );
}


/** @brief COPY do sady 2 (ADD 52 místo ADD 48): zdroj je sada 2 CG-ROM. */
void test_manual_copy_set2 ( void ) {
    for ( unsigned i = 0; i < 8; i++ ) {
        unsigned de = ( 163 - 128 ) * 8 + i;
        pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, (uint8_t) de, s_cgrom );
        pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, (uint8_t) ( ( de >> 8 ) + 52 ), s_cgrom );
        pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, (uint8_t) ( de >> 8 ), s_cgrom );
    };
    for ( unsigned i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( s_cgrom[0x800 + 163 * 8 + i], s_pcg.ram[0x400 + ( 163 - 128 ) * 8 + i] );
        TEST_ASSERT_EQUAL_HEX8 ( 0, s_pcg.ram[( 163 - 128 ) * 8 + i] );
    };
    assert_effective_consistent ( );
}


/** @brief Třetí BASIC příklad: AD = J * 8 + I pro sadu 2 (adresa 400h - 7FFh). */
void test_manual_set2_write ( void ) {
    static const unsigned codes[] = { 0x80, 0xC5, 0xFF };
    for ( unsigned k = 0; k < 3; k++ ) {
        for ( unsigned i = 0; i < 8; i++ ) {
            basic_write ( codes[k] * 8 + i, (uint8_t) ( 0x80 >> i ) );
        };
    };
    for ( unsigned k = 0; k < 3; k++ ) {
        for ( unsigned i = 0; i < 8; i++ ) {
            TEST_ASSERT_EQUAL_HEX8 ( 1u << i, s_pcg.effective[0x800 + codes[k] * 8 + i] );
            TEST_ASSERT_EQUAL_HEX8 ( 0, s_pcg.ram[( codes[k] - 0x80 ) * 8 + i] );
        };
    };
    assert_effective_consistent ( );
}


/** @brief Nástupná hrana: držený strobe ani opakovaný zápis s bitem 4 nezapisuje. */
void test_rising_hold_no_write ( void ) {
    pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0x80, s_cgrom );
    TEST_ASSERT_TRUE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x10, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, s_pcg.ram[0] );

    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0x40, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, 0x01, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x10, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, s_pcg.ram[0] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.ram[1] );

    /* nová hrana zapíše s aktuálními latchi */
    TEST_ASSERT_TRUE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x10, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_EQUAL_HEX8 ( 0x02, s_pcg.ram[1] );
}


/** @brief Zápis stejné hodnoty hlásí RAM_WRITTEN, ale ne GLYPH_CHANGED. */
void test_same_value_no_glyph_change ( void ) {
    unsigned evt = basic_write ( 5, 0x00 );
    TEST_ASSERT_TRUE ( evt & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_FALSE ( evt & PCG700_EVT_GLYPH_CHANGED );
    evt = basic_write ( 5, 0x01 );
    TEST_ASSERT_TRUE ( evt & PCG700_EVT_GLYPH_CHANGED );
}


/* ========================================================================
 * Úrovňový režim
 * ======================================================================== */

/** @brief Úroveň: BASIC sekvence funguje, každý zápis při bitu 4 = 1 zapíše. */
void test_level_mode ( void ) {
    set_strobe ( PCG700_STROBE_LEVEL );
    for ( unsigned i = 0; i < 8; i++ ) {
        basic_write ( 888 + i, c_MANUAL_GLYPH[i] );
    };
    for ( unsigned i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( pcg700_core_bitrev ( c_MANUAL_GLYPH[i] ), s_pcg.ram[888 + i] );
    };

    /* strobe trvale nastaven: data i adresa zapisují hned */
    TEST_ASSERT_TRUE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x10, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_TRUE ( pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0x80, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, s_pcg.ram[( 888 + 7 ) & 0xFF] );
    TEST_ASSERT_TRUE ( pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, 0x10, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, s_pcg.ram[0x10] );
    /* opakovaný zápis 0E012h s bitem 4 také zapisuje */
    TEST_ASSERT_TRUE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x11, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, s_pcg.ram[0x110] );
    /* uvolnění nezapisuje */
    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0xFF, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    assert_effective_consistent ( );
}


/* ========================================================================
 * Sestupná hrana
 * ======================================================================== */

/** @brief Sestupná hrana: zápis až při uvolnění, adresa a COPY ze stavu před uvolněním. */
void test_falling_mode_basic_program ( void ) {
    set_strobe ( PCG700_STROBE_FALLING );
    for ( unsigned i = 0; i < 8; i++ ) {
        unsigned ad = 888 + i;
        pcg700_core_write ( &s_pcg, PCG700_REG_DATA, c_MANUAL_GLYPH[i], s_cgrom );
        pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, (uint8_t) ad, s_cgrom );
        unsigned evt = pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, (uint8_t) ( ( ad >> 8 ) + 16 ), s_cgrom );
        TEST_ASSERT_FALSE ( evt & PCG700_EVT_RAM_WRITTEN );
        evt = pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0, s_cgrom );
        TEST_ASSERT_TRUE ( evt & PCG700_EVT_RAM_WRITTEN );
    };
    /* uvolnění zápisem 0 (bity 2 - 0 = 0) zapsalo na adresu 3xxh, ne 0xxh */
    for ( unsigned i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( pcg700_core_bitrev ( c_MANUAL_GLYPH[i] ), s_pcg.ram[888 + i] );
        TEST_ASSERT_EQUAL_HEX8 ( 0, s_pcg.ram[( 888 + i ) & 0xFF] );
    };
    assert_effective_consistent ( );
}


/** @brief Sestupná hrana: COPY a sada se berou z hodnoty 0E012h před uvolněním. */
void test_falling_mode_copy ( void ) {
    set_strobe ( PCG700_STROBE_FALLING );
    pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0xFF, s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, 0x18, s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x10 | 0x20 | 0x04 | 0x01, s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom );
    TEST_ASSERT_EQUAL_HEX8 ( s_cgrom[pcg700_core_effective_index ( 0x518 )], s_pcg.ram[0x518] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.ram[0x018] );

    /* nástupná hrana ani držení v tomto režimu nezapisuje */
    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x10, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_FALSE ( pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0x01, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    /* uvolnění zápisem s jinými bity adresy použije adresu před uvolněním (bity 2 - 0 = 0) */
    TEST_ASSERT_TRUE ( pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x07, s_cgrom ) & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_EQUAL_HEX8 ( 0x80, s_pcg.ram[0x018] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.ram[0x718] );
    assert_effective_consistent ( );
}


/* ========================================================================
 * Přepínače
 * ======================================================================== */

/** @brief SSW = ON: bit 3 zapíná a vypíná, hlásí změnu zobrazení jen při přepnutí. */
void test_ssw_on_bit3 ( void ) {
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom ) );
    TEST_ASSERT_TRUE ( pcg700_core_display_enabled ( &s_pcg ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x08, s_cgrom ) );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );
    /* bity 6 - 7 se latchují, ale nic neřídí */
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0xC8, s_cgrom ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0xC8, s_pcg.ctrl );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );
}


/** @brief HSW = NOM: nikdy nekreslí z PCG, zápisy do RAM ale fungují (GSW = ON). */
void test_hsw_nom ( void ) {
    st_PCG700_CONFIG cfg = s_pcg.cfg;
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom );
    TEST_ASSERT_TRUE ( pcg700_core_display_enabled ( &s_pcg ) );

    cfg.hsw = PCG700_HSW_NOM;
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, pcg700_core_set_config ( &s_pcg, &cfg ) );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );
    TEST_ASSERT_EQUAL_PTR ( s_cgrom, pcg700_core_glyphs ( &s_pcg, s_cgrom ) );

    unsigned evt = basic_write ( 0, 0x80 );
    TEST_ASSERT_TRUE ( evt & PCG700_EVT_RAM_WRITTEN );
    TEST_ASSERT_FALSE ( evt & PCG700_EVT_DISPLAY_CHANGED );
    TEST_ASSERT_FALSE ( pcg700_core_event_needs_redraw ( &s_pcg, evt ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, s_pcg.ram[0] );

    cfg.hsw = PCG700_HSW_PCG;
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, pcg700_core_set_config ( &s_pcg, &cfg ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_set_config ( &s_pcg, &cfg ) );
}


/** @brief SSW = OFF: výchozí výklad (rozhoduje HSW) i varianta "software nezapne". */
void test_ssw_off ( void ) {
    st_PCG700_CONFIG cfg = s_pcg.cfg;
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) ); /* bit 3 = 1 */

    cfg.ssw = PCG700_SSW_OFF;
    TEST_ASSERT_EQUAL_INT ( PCG700_SSWOFF_HSW_DECIDES, cfg.sswoff );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, pcg700_core_set_config ( &s_pcg, &cfg ) );
    TEST_ASSERT_TRUE ( pcg700_core_display_enabled ( &s_pcg ) );
    /* bit 3 se ignoruje */
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x08, s_cgrom ) );
    TEST_ASSERT_TRUE ( pcg700_core_display_enabled ( &s_pcg ) );

    cfg.sswoff = PCG700_SSWOFF_FORCE_OFF;
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_DISPLAY_CHANGED, pcg700_core_set_config ( &s_pcg, &cfg ) );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom ) );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );

    /* HSW = NOM vypíná i ve výkladu HSW_DECIDES */
    cfg.sswoff = PCG700_SSWOFF_HSW_DECIDES;
    cfg.hsw = PCG700_HSW_NOM;
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_set_config ( &s_pcg, &cfg ) );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );
}


/** @brief GSW = OFF: všechny zápisy se ignorují včetně latchů a bitu 3. */
void test_gsw_off ( void ) {
    st_PCG700_CONFIG cfg = s_pcg.cfg;
    cfg.gsw = PCG700_GSW_OFF;
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_set_config ( &s_pcg, &cfg ) );

    st_PCG700 before;
    memcpy ( &before, &s_pcg, sizeof ( before ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, basic_write ( 0, 0xFF ) );
    TEST_ASSERT_EQUAL_MEMORY ( &before, &s_pcg, sizeof ( s_pcg ) );
    TEST_ASSERT_FALSE ( pcg700_core_display_enabled ( &s_pcg ) );

    /* po zapojení GSW zápisy fungují */
    cfg.gsw = PCG700_GSW_ON;
    pcg700_core_set_config ( &s_pcg, &cfg );
    basic_write ( 0, 0xFF );
    TEST_ASSERT_EQUAL_HEX8 ( 0xFF, s_pcg.ram[0] );
}


/** @brief Neplatná konfigurace a registr mimo rozsah se odmítnou beze změny. */
void test_invalid_config_and_register ( void ) {
    st_PCG700 before;
    memcpy ( &before, &s_pcg, sizeof ( before ) );
    st_PCG700_CONFIG cfg = s_pcg.cfg;
    cfg.hsw = PCG700_HSW_COUNT;
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_set_config ( &s_pcg, &cfg ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_set_config ( &s_pcg, NULL ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, pcg700_core_write ( &s_pcg, 3, 0x10, s_cgrom ) );
    TEST_ASSERT_EQUAL_MEMORY ( &before, &s_pcg, sizeof ( s_pcg ) );
}


/** @brief Rozhodnutí o překreslení podle událostí. */
void test_needs_redraw ( void ) {
    TEST_ASSERT_TRUE ( pcg700_core_event_needs_redraw ( &s_pcg, PCG700_EVT_DISPLAY_CHANGED ) );
    TEST_ASSERT_FALSE ( pcg700_core_event_needs_redraw ( &s_pcg, PCG700_EVT_GLYPH_CHANGED ) ); /* PCG vypnuto */
    TEST_ASSERT_FALSE ( pcg700_core_event_needs_redraw ( &s_pcg, PCG700_EVT_NONE ) );
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0x00, s_cgrom );
    TEST_ASSERT_TRUE ( pcg700_core_event_needs_redraw ( &s_pcg, PCG700_EVT_GLYPH_CHANGED ) );
    TEST_ASSERT_FALSE ( pcg700_core_event_needs_redraw ( &s_pcg, PCG700_EVT_RAM_WRITTEN ) );
}


/* ========================================================================
 * Efektivní generátor
 * ======================================================================== */

/** @brief Náhodné zápisy (všechny režimy, COPY i data, poke): inkrementální == plné přestavění. */
void test_effective_incremental_equals_rebuild ( void ) {
    uint32_t seed = 12345;
    for ( unsigned mode = 0; mode < PCG700_STROBE_COUNT; mode++ ) {
        set_strobe ( (en_PCG700_STROBE) mode );
        for ( unsigned n = 0; n < 4000; n++ ) {
            seed = seed * 1103515245u + 12345u;
            unsigned reg = ( seed >> 16 ) % 4; /* 3 = poke */
            uint8_t val = (uint8_t) ( seed >> 8 );
            if ( reg == 3 ) {
                pcg700_core_ram_poke ( &s_pcg, seed >> 5, val );
            } else {
                pcg700_core_write ( &s_pcg, reg, val, s_cgrom );
            };
        };
        st_PCG700 copy = s_pcg;
        pcg700_core_rebuild ( &copy, s_cgrom );
        TEST_ASSERT_EQUAL_MEMORY ( copy.effective, s_pcg.effective, PCG700_CGROM_SIZE );
        assert_effective_consistent ( );
    };
}


/** @brief Rebuild po změně CG-ROM: dolní poloviny z nové ROM, horní z RAM. */
void test_rebuild_after_rom_change ( void ) {
    basic_write ( 0x123, 0xAA );
    for ( unsigned i = 0; i < PCG700_CGROM_SIZE; i++ ) {
        s_cgrom[i] = (uint8_t) ~s_cgrom[i];
    };
    pcg700_core_rebuild ( &s_pcg, s_cgrom );
    assert_effective_consistent ( );
    TEST_ASSERT_EQUAL_HEX8 ( 0x55, s_pcg.effective[pcg700_core_effective_index ( 0x123 )] );

    /* NULL CG-ROM = samé nuly */
    pcg700_core_rebuild ( &s_pcg, NULL );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, s_pcg.effective[0] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x55, s_pcg.effective[pcg700_core_effective_index ( 0x123 )] );
}


/** @brief Peek a poke RAM pro debugger: bez převracení, latche beze změny. */
void test_ram_peek_poke ( void ) {
    uint8_t ctrl = s_pcg.ctrl;
    unsigned evt = pcg700_core_ram_poke ( &s_pcg, 0x0FFF, 0x81 ); /* ořízne na 7FFh */
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_RAM_WRITTEN | PCG700_EVT_GLYPH_CHANGED, evt );
    TEST_ASSERT_EQUAL_HEX8 ( 0x81, pcg700_core_ram_peek ( &s_pcg, 0x7FF ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x81, s_pcg.effective[0xFFF] );
    TEST_ASSERT_EQUAL_HEX8 ( ctrl, s_pcg.ctrl );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_RAM_WRITTEN, pcg700_core_ram_poke ( &s_pcg, 0x7FF, 0x81 ) );
}


/* ========================================================================
 * Binární stav
 * ======================================================================== */

/** @brief Round-trip: uložení a načtení do instance s jinou konfigurací. */
void test_state_roundtrip ( void ) {
    for ( unsigned i = 0; i < 8; i++ ) {
        basic_write ( 0x400 + 888 + i, c_MANUAL_GLYPH[i] );
    };
    pcg700_core_write ( &s_pcg, PCG700_REG_DATA, 0x3C, s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_ADDR_LO, 0x99, s_cgrom );
    pcg700_core_write ( &s_pcg, PCG700_REG_CTRL, 0xD2, s_cgrom ); /* strobe, bity 6-7, PCG zap. */

    uint8_t buf[PCG700_STATE_SIZE + 16];
    memset ( buf, 0xEE, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_UINT ( PCG700_STATE_SIZE, pcg700_core_state_save ( &s_pcg, buf, sizeof ( buf ) ) );
    TEST_ASSERT_EQUAL_HEX8 ( 'P', buf[0] );
    TEST_ASSERT_EQUAL_HEX8 ( 'C', buf[1] );
    TEST_ASSERT_EQUAL_HEX8 ( 'G', buf[2] );
    TEST_ASSERT_EQUAL_HEX8 ( '7', buf[3] );
    TEST_ASSERT_EQUAL_HEX8 ( PCG700_STATE_VERSION & 0xFF, buf[4] );
    TEST_ASSERT_EQUAL_HEX8 ( PCG700_STATE_VERSION >> 8, buf[5] );
    TEST_ASSERT_EQUAL_HEX8 ( PCG700_STATE_SIZE & 0xFF, buf[6] );
    TEST_ASSERT_EQUAL_HEX8 ( PCG700_STATE_SIZE >> 8, buf[7] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x3C, buf[8] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x99, buf[9] );
    TEST_ASSERT_EQUAL_HEX8 ( 0xD2, buf[10] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, buf[11] );
    TEST_ASSERT_EQUAL_MEMORY ( s_pcg.ram, &buf[12], PCG700_RAM_SIZE );
    TEST_ASSERT_EQUAL_HEX8 ( 0xEE, buf[PCG700_STATE_SIZE] );

    st_PCG700 dst;
    st_PCG700_CONFIG cfg;
    pcg700_core_config_default ( &cfg );
    cfg.strobe = PCG700_STROBE_LEVEL;
    pcg700_core_power_on ( &dst, &cfg, s_cgrom );
    unsigned evt = 0;
    TEST_ASSERT_EQUAL_INT ( PCG700_OK, pcg700_core_state_load ( &dst, buf, PCG700_STATE_SIZE, s_cgrom, &evt ) );
    TEST_ASSERT_TRUE ( evt & PCG700_EVT_GLYPH_CHANGED );
    TEST_ASSERT_TRUE ( evt & PCG700_EVT_DISPLAY_CHANGED );
    TEST_ASSERT_EQUAL_HEX8 ( s_pcg.data, dst.data );
    TEST_ASSERT_EQUAL_HEX8 ( s_pcg.addr_lo, dst.addr_lo );
    TEST_ASSERT_EQUAL_HEX8 ( s_pcg.ctrl, dst.ctrl );
    TEST_ASSERT_EQUAL_MEMORY ( s_pcg.ram, dst.ram, PCG700_RAM_SIZE );
    TEST_ASSERT_EQUAL_MEMORY ( s_pcg.effective, dst.effective, PCG700_CGROM_SIZE );
    TEST_ASSERT_EQUAL_INT ( PCG700_STROBE_LEVEL, dst.cfg.strobe ); /* konfigurace se nenačítá */
    TEST_ASSERT_TRUE ( pcg700_core_display_enabled ( &dst ) );

    /* events smí být NULL */
    TEST_ASSERT_EQUAL_INT ( PCG700_OK, pcg700_core_state_load ( &dst, buf, PCG700_STATE_SIZE, s_cgrom, NULL ) );
}


/** @brief Poškozená data a malý buffer se odmítnou, instance zůstává beze změny. */
void test_state_errors ( void ) {
    uint8_t buf[PCG700_STATE_SIZE];
    TEST_ASSERT_EQUAL_UINT ( 0, pcg700_core_state_save ( &s_pcg, buf, PCG700_STATE_SIZE - 1 ) );
    TEST_ASSERT_EQUAL_UINT ( 0, pcg700_core_state_save ( &s_pcg, NULL, PCG700_STATE_SIZE ) );
    TEST_ASSERT_EQUAL_UINT ( PCG700_STATE_SIZE, pcg700_core_state_save ( &s_pcg, buf, sizeof ( buf ) ) );

    basic_write ( 1, 0xFF );
    st_PCG700 before;
    memcpy ( &before, &s_pcg, sizeof ( before ) );
    uint8_t bad[PCG700_STATE_SIZE];
    unsigned evt = 0xFFFF;

    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_FORMAT, pcg700_core_state_load ( &s_pcg, buf, PCG700_STATE_SIZE - 1, s_cgrom, &evt ) );
    TEST_ASSERT_EQUAL_HEX ( PCG700_EVT_NONE, evt );

    memcpy ( bad, buf, sizeof ( bad ) );
    bad[0] = 'X';
    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_FORMAT, pcg700_core_state_load ( &s_pcg, bad, sizeof ( bad ), s_cgrom, NULL ) );

    memcpy ( bad, buf, sizeof ( bad ) );
    bad[4] = PCG700_STATE_VERSION + 1;
    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_VERSION, pcg700_core_state_load ( &s_pcg, bad, sizeof ( bad ), s_cgrom, NULL ) );

    memcpy ( bad, buf, sizeof ( bad ) );
    bad[4] = 0;
    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_VERSION, pcg700_core_state_load ( &s_pcg, bad, sizeof ( bad ), s_cgrom, NULL ) );

    memcpy ( bad, buf, sizeof ( bad ) );
    bad[6] = (uint8_t) ( bad[6] + 1 );
    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_FORMAT, pcg700_core_state_load ( &s_pcg, bad, sizeof ( bad ), s_cgrom, NULL ) );

    TEST_ASSERT_EQUAL_INT ( PCG700_ERR_INVALID, pcg700_core_state_load ( &s_pcg, NULL, sizeof ( bad ), s_cgrom, NULL ) );
    TEST_ASSERT_EQUAL_MEMORY ( &before, &s_pcg, sizeof ( s_pcg ) );
}


/**
 * @brief Vstupní bod testu.
 * @param argc Nepoužito.
 * @param argv Nepoužito.
 * @return Počet selhaných testů (0 = úspěch).
 */
int main ( int argc, char *argv[] ) {
    (void) argc;
    (void) argv;

    UNITY_BEGIN ( );

    RUN_TEST ( test_bitrev );
    RUN_TEST ( test_effective_index );

    RUN_TEST ( test_power_on_defaults );
    RUN_TEST ( test_power_on_ram_cgrom );
    RUN_TEST ( test_power_on_invalid );
    RUN_TEST ( test_reset_keeps_ram );
    RUN_TEST ( test_reset_falling_no_write );

    RUN_TEST ( test_manual_basic_program );
    RUN_TEST ( test_manual_asm_prg1ch );
    RUN_TEST ( test_manual_copy );
    RUN_TEST ( test_manual_copy_set2 );
    RUN_TEST ( test_manual_set2_write );
    RUN_TEST ( test_rising_hold_no_write );
    RUN_TEST ( test_same_value_no_glyph_change );

    RUN_TEST ( test_level_mode );
    RUN_TEST ( test_falling_mode_basic_program );
    RUN_TEST ( test_falling_mode_copy );

    RUN_TEST ( test_ssw_on_bit3 );
    RUN_TEST ( test_hsw_nom );
    RUN_TEST ( test_ssw_off );
    RUN_TEST ( test_gsw_off );
    RUN_TEST ( test_invalid_config_and_register );
    RUN_TEST ( test_needs_redraw );

    RUN_TEST ( test_effective_incremental_equals_rebuild );
    RUN_TEST ( test_rebuild_after_rom_change );
    RUN_TEST ( test_ram_peek_poke );

    RUN_TEST ( test_state_roundtrip );
    RUN_TEST ( test_state_errors );

    return UNITY_END ( );
}
