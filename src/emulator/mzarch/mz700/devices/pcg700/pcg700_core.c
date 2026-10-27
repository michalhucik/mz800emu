/**
 * @file   pcg700_core.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Čisté jádro modelu PCG-700 (HAL Laboratory) pro Sharp MZ-700.
 *
 * Implementace registrů 0E010h - 0E012h, PCG RAM 2 kB, efektivního generátoru
 * znaků, přepínačů HSW / SSW / GSW a binárního stavu. Popis modelu, formát
 * CG-ROM emulátoru a zdroje rozhodnutí jsou v devdoc/pcg700.md; veřejné
 * kontrakty v pcg700_core.h.
 *
 * Soubor nesmí includovat nic z emulátoru (testuje se jako STANDALONE).
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

#include <string.h>

#include "pcg700_core.h"


/** @brief Maska adresy PCG RAM (11 bitů). */
#define PCG700_ADDR_MASK        ( PCG700_RAM_SIZE - 1 )

/** @brief Bity řídicího registru, které tvoří adresu PCG RAM bity 10 - 8. */
#define PCG700_CTRL_ADDR_BITS   ( PCG700_CTRL_ADDR_HI | PCG700_CTRL_SET )

/** @brief Offset hlavičky: magic (4 B). */
#define PCG700_ST_OFS_MAGIC     0
/** @brief Offset hlavičky: verze (u16 LE). */
#define PCG700_ST_OFS_VERSION   4
/** @brief Offset hlavičky: celková délka bloku (u16 LE). */
#define PCG700_ST_OFS_LENGTH    6
/** @brief Offset latche data. */
#define PCG700_ST_OFS_DATA      8
/** @brief Offset latche addr_lo. */
#define PCG700_ST_OFS_ADDR_LO   9
/** @brief Offset latche ctrl. */
#define PCG700_ST_OFS_CTRL      10
/** @brief Offset rezervovaného bajtu (zapisuje se 0, při čtení se ignoruje). */
#define PCG700_ST_OFS_RESERVED  11
/** @brief Offset PCG RAM. */
#define PCG700_ST_OFS_RAM       12

/** @brief Magic binárního stavu. */
static const uint8_t c_PCG700_STATE_MAGIC[4] = { 'P', 'C', 'G', '7' };


void pcg700_core_config_default ( st_PCG700_CONFIG *cfg ) {
    cfg->strobe = PCG700_STROBE_RISING;
    cfg->hsw = PCG700_HSW_PCG;
    cfg->ssw = PCG700_SSW_ON;
    cfg->gsw = PCG700_GSW_ON;
    cfg->sswoff = PCG700_SSWOFF_HSW_DECIDES;
    cfg->poweron_ram = PCG700_POWERON_RAM_ZERO;
}


bool pcg700_core_config_valid ( const st_PCG700_CONFIG *cfg ) {
    if ( cfg == NULL ) return false;
    /* enum může být signed i unsigned - porovnání přes unsigned pokryje obě strany */
    if ( (unsigned) cfg->strobe >= PCG700_STROBE_COUNT ) return false;
    if ( (unsigned) cfg->hsw >= PCG700_HSW_COUNT ) return false;
    if ( (unsigned) cfg->ssw >= PCG700_SSW_COUNT ) return false;
    if ( (unsigned) cfg->gsw >= PCG700_GSW_COUNT ) return false;
    if ( (unsigned) cfg->sswoff >= PCG700_SSWOFF_COUNT ) return false;
    if ( (unsigned) cfg->poweron_ram >= PCG700_POWERON_RAM_COUNT ) return false;
    return true;
}


unsigned pcg700_core_effective_index ( unsigned addr ) {
    addr &= PCG700_ADDR_MASK;
    /* bit 10 adresy (sada) -> bit 11 indexu; kódy 80h - 0FFh = bit 10 indexu = 1 */
    return ( ( addr & 0x400 ) << 1 ) | 0x400 | ( addr & 0x3FF );
}


uint8_t pcg700_core_bitrev ( uint8_t value ) {
    value = (uint8_t) ( ( ( value & 0xF0 ) >> 4 ) | ( ( value & 0x0F ) << 4 ) );
    value = (uint8_t) ( ( ( value & 0xCC ) >> 2 ) | ( ( value & 0x33 ) << 2 ) );
    value = (uint8_t) ( ( ( value & 0xAA ) >> 1 ) | ( ( value & 0x55 ) << 1 ) );
    return value;
}


bool pcg700_core_display_enabled ( const st_PCG700 *pcg ) {
    if ( pcg->cfg.hsw != PCG700_HSW_PCG ) return false;
    if ( pcg->cfg.ssw == PCG700_SSW_ON ) {
        return ( ( pcg->ctrl & PCG700_CTRL_OFF ) == 0 );
    };
    return ( pcg->cfg.sswoff == PCG700_SSWOFF_HSW_DECIDES );
}


const uint8_t* pcg700_core_glyphs ( const st_PCG700 *pcg, const uint8_t *cgrom ) {
    return pcg700_core_display_enabled ( pcg ) ? pcg->effective : cgrom;
}


bool pcg700_core_event_needs_redraw ( const st_PCG700 *pcg, unsigned events ) {
    if ( events & PCG700_EVT_DISPLAY_CHANGED ) return true;
    if ( ( events & PCG700_EVT_GLYPH_CHANGED ) && pcg700_core_display_enabled ( pcg ) ) return true;
    return false;
}


/**
 * @brief Vrátí masku PCG700_EVT_DISPLAY_CHANGED, pokud se zobrazení změnilo.
 *
 * @param pcg    Instance po změně; nesmí být NULL.
 * @param before Hodnota pcg700_core_display_enabled() před změnou.
 * @return PCG700_EVT_DISPLAY_CHANGED nebo PCG700_EVT_NONE.
 */
static unsigned pcg700_core_display_event ( const st_PCG700 *pcg, bool before ) {
    return ( pcg700_core_display_enabled ( pcg ) != before ) ? PCG700_EVT_DISPLAY_CHANGED : PCG700_EVT_NONE;
}


/**
 * @brief Uloží bajt do PCG RAM a promítne ho do efektivního generátoru.
 *
 * @param pcg   Instance; nesmí být NULL.
 * @param addr  Adresa v PCG RAM (ořízne se na 11 bitů).
 * @param value Bajt ve formátu CG-ROM.
 * @return PCG700_EVT_RAM_WRITTEN, plus PCG700_EVT_GLYPH_CHANGED, pokud se
 *         bajt lišil od předchozího obsahu.
 * @post Invariant `effective` st_PCG700 zůstává zachován.
 */
static unsigned pcg700_core_store ( st_PCG700 *pcg, unsigned addr, uint8_t value ) {
    unsigned evt = PCG700_EVT_RAM_WRITTEN;
    addr &= PCG700_ADDR_MASK;
    if ( pcg->ram[addr] != value ) {
        evt |= PCG700_EVT_GLYPH_CHANGED;
    };
    pcg->ram[addr] = value;
    pcg->effective[pcg700_core_effective_index ( addr )] = value;
    return evt;
}


/**
 * @brief Provede jeden zápis do PCG RAM podle zadaného obrazu řídicího registru.
 *
 * Adresa = bity 2 - 0 z `ctrl` a latch addr_lo; zdroj podle bitu 5 `ctrl`:
 * bitově převrácený latch data, nebo bajt CG-ROM téže pozice.
 *
 * @param pcg   Instance; nesmí být NULL.
 * @param ctrl  Hodnota řídicího registru, ze které se bere adresa a COPY
 *              (u sestupné hrany hodnota před uvolněním).
 * @param cgrom CG-ROM 4 kB; NULL = COPY zapíše 00h.
 * @return Události pcg700_core_store().
 */
static unsigned pcg700_core_strobe ( st_PCG700 *pcg, uint8_t ctrl, const uint8_t *cgrom ) {
    unsigned addr = ( (unsigned) ( ctrl & PCG700_CTRL_ADDR_BITS ) << 8 ) | pcg->addr_lo;
    uint8_t value;
    if ( ctrl & PCG700_CTRL_COPY ) {
        value = ( cgrom != NULL ) ? cgrom[pcg700_core_effective_index ( addr )] : 0x00;
    } else {
        value = pcg700_core_bitrev ( pcg->data );
    };
    return pcg700_core_store ( pcg, addr, value );
}


unsigned pcg700_core_write ( st_PCG700 *pcg, unsigned reg, uint8_t value, const uint8_t *cgrom ) {

    if ( pcg->cfg.gsw != PCG700_GSW_ON ) return PCG700_EVT_NONE;
    if ( reg >= PCG700_REG_COUNT ) return PCG700_EVT_NONE;

    bool disp_before = pcg700_core_display_enabled ( pcg );
    uint8_t ctrl_before = pcg->ctrl;
    unsigned evt = PCG700_EVT_NONE;

    switch ( reg ) {
        case PCG700_REG_DATA:
            pcg->data = value;
            break;
        case PCG700_REG_ADDR_LO:
            pcg->addr_lo = value;
            break;
        default:
            pcg->ctrl = value;
            break;
    };

    bool strobe_before = ( ctrl_before & PCG700_CTRL_STROBE ) != 0;
    bool strobe_now = ( pcg->ctrl & PCG700_CTRL_STROBE ) != 0;

    switch ( pcg->cfg.strobe ) {
        case PCG700_STROBE_LEVEL:
            if ( strobe_now ) {
                evt |= pcg700_core_strobe ( pcg, pcg->ctrl, cgrom );
            };
            break;

        case PCG700_STROBE_FALLING:
            if ( reg == PCG700_REG_CTRL && strobe_before && !strobe_now ) {
                evt |= pcg700_core_strobe ( pcg, ctrl_before, cgrom );
            };
            break;

        default: /* PCG700_STROBE_RISING */
            if ( reg == PCG700_REG_CTRL && !strobe_before && strobe_now ) {
                evt |= pcg700_core_strobe ( pcg, pcg->ctrl, cgrom );
            };
            break;
    };

    evt |= pcg700_core_display_event ( pcg, disp_before );
    return evt;
}


void pcg700_core_rebuild ( st_PCG700 *pcg, const uint8_t *cgrom ) {
    if ( cgrom != NULL ) {
        memcpy ( pcg->effective, cgrom, PCG700_CGROM_SIZE );
    } else {
        memset ( pcg->effective, 0x00, PCG700_CGROM_SIZE );
    };
    /* horní polovina sady 1 (400h - 7FFh) a sady 2 (0C00h - 0FFFh) z RAM */
    memcpy ( &pcg->effective[0x400], &pcg->ram[0x000], 0x400 );
    memcpy ( &pcg->effective[0xC00], &pcg->ram[0x400], 0x400 );
}


/**
 * @brief Nastaví latche na hodnoty po zapnutí a po RESET.
 * @param pcg Instance; nesmí být NULL.
 */
static void pcg700_core_latches_reset ( st_PCG700 *pcg ) {
    pcg->data = 0x00;
    pcg->addr_lo = 0x00;
    pcg->ctrl = PCG700_CTRL_RESET_VALUE;
}


int pcg700_core_power_on ( st_PCG700 *pcg, const st_PCG700_CONFIG *cfg, const uint8_t *cgrom ) {
    st_PCG700_CONFIG c;

    if ( pcg == NULL ) return PCG700_ERR_INVALID;
    if ( cfg != NULL ) {
        if ( !pcg700_core_config_valid ( cfg ) ) return PCG700_ERR_INVALID;
        c = *cfg; /* kopie dřív než zápis: cfg smí ukazovat do pcg->cfg */
    } else {
        pcg700_core_config_default ( &c );
    };

    pcg->cfg = c;
    pcg700_core_latches_reset ( pcg );

    if ( c.poweron_ram == PCG700_POWERON_RAM_CGROM && cgrom != NULL ) {
        memcpy ( &pcg->ram[0x000], &cgrom[0x400], 0x400 );
        memcpy ( &pcg->ram[0x400], &cgrom[0xC00], 0x400 );
    } else {
        /* ZERO, nebo CGROM bez dodané ROM (NULL = ROM ze samých nul) */
        memset ( pcg->ram, 0x00, PCG700_RAM_SIZE );
    };

    pcg700_core_rebuild ( pcg, cgrom );
    return PCG700_OK;
}


unsigned pcg700_core_reset ( st_PCG700 *pcg ) {
    bool disp_before = pcg700_core_display_enabled ( pcg );
    pcg700_core_latches_reset ( pcg );
    return pcg700_core_display_event ( pcg, disp_before );
}


unsigned pcg700_core_set_config ( st_PCG700 *pcg, const st_PCG700_CONFIG *cfg ) {
    if ( !pcg700_core_config_valid ( cfg ) ) return PCG700_EVT_NONE;
    bool disp_before = pcg700_core_display_enabled ( pcg );
    pcg->cfg = *cfg;
    return pcg700_core_display_event ( pcg, disp_before );
}


uint8_t pcg700_core_ram_peek ( const st_PCG700 *pcg, unsigned addr ) {
    return pcg->ram[addr & PCG700_ADDR_MASK];
}


unsigned pcg700_core_ram_poke ( st_PCG700 *pcg, unsigned addr, uint8_t value ) {
    return pcg700_core_store ( pcg, addr, value );
}


/**
 * @brief Zapíše u16 little-endian.
 * @param dst   Cíl (2 B); nesmí být NULL.
 * @param value Hodnota.
 */
static void pcg700_core_put_u16le ( uint8_t *dst, unsigned value ) {
    dst[0] = (uint8_t) ( value & 0xFF );
    dst[1] = (uint8_t) ( ( value >> 8 ) & 0xFF );
}


/**
 * @brief Přečte u16 little-endian.
 * @param src Zdroj (2 B); nesmí být NULL.
 * @return Hodnota 0 - 0FFFFh.
 */
static unsigned pcg700_core_get_u16le ( const uint8_t *src ) {
    return (unsigned) src[0] | ( (unsigned) src[1] << 8 );
}


size_t pcg700_core_state_save ( const st_PCG700 *pcg, uint8_t *buf, size_t buf_size ) {
    if ( pcg == NULL || buf == NULL || buf_size < PCG700_STATE_SIZE ) return 0;

    memcpy ( &buf[PCG700_ST_OFS_MAGIC], c_PCG700_STATE_MAGIC, sizeof ( c_PCG700_STATE_MAGIC ) );
    pcg700_core_put_u16le ( &buf[PCG700_ST_OFS_VERSION], PCG700_STATE_VERSION );
    pcg700_core_put_u16le ( &buf[PCG700_ST_OFS_LENGTH], PCG700_STATE_SIZE );
    buf[PCG700_ST_OFS_DATA] = pcg->data;
    buf[PCG700_ST_OFS_ADDR_LO] = pcg->addr_lo;
    buf[PCG700_ST_OFS_CTRL] = pcg->ctrl;
    buf[PCG700_ST_OFS_RESERVED] = 0x00;
    memcpy ( &buf[PCG700_ST_OFS_RAM], pcg->ram, PCG700_RAM_SIZE );
    return PCG700_STATE_SIZE;
}


int pcg700_core_state_load ( st_PCG700 *pcg, const uint8_t *buf, size_t size, const uint8_t *cgrom, unsigned *events ) {

    if ( events != NULL ) *events = PCG700_EVT_NONE;
    if ( pcg == NULL || buf == NULL ) return PCG700_ERR_INVALID;
    if ( size < PCG700_ST_OFS_DATA ) return PCG700_ERR_FORMAT;
    if ( memcmp ( &buf[PCG700_ST_OFS_MAGIC], c_PCG700_STATE_MAGIC, sizeof ( c_PCG700_STATE_MAGIC ) ) != 0 ) return PCG700_ERR_FORMAT;

    unsigned version = pcg700_core_get_u16le ( &buf[PCG700_ST_OFS_VERSION] );
    if ( version == 0 || version > PCG700_STATE_VERSION ) return PCG700_ERR_VERSION;

    /* verze 1: pevná délka; blok kratší než deklarovaná délka je useknutý */
    unsigned length = pcg700_core_get_u16le ( &buf[PCG700_ST_OFS_LENGTH] );
    if ( length != PCG700_STATE_SIZE || size < length ) return PCG700_ERR_FORMAT;

    bool disp_before = pcg700_core_display_enabled ( pcg );

    pcg->data = buf[PCG700_ST_OFS_DATA];
    pcg->addr_lo = buf[PCG700_ST_OFS_ADDR_LO];
    pcg->ctrl = buf[PCG700_ST_OFS_CTRL];
    memcpy ( pcg->ram, &buf[PCG700_ST_OFS_RAM], PCG700_RAM_SIZE );
    pcg700_core_rebuild ( pcg, cgrom );

    if ( events != NULL ) {
        *events = PCG700_EVT_GLYPH_CHANGED | pcg700_core_display_event ( pcg, disp_before );
    };
    return PCG700_OK;
}
