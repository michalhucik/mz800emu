/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file suc80_devbus.c
 * @brief Zařízení devbus "suc80" - lepidlo jádra SUC (kontrakt v suc80_devbus.h).
 *
 * - MMIO 0E00Ch - 0E00Fh jen ve stavu DEVBUS_MMIO_MAPPED (jako 8255 / 8253
 *   základní desky; po OUT (0E1h) latch nereaguje). Zařízení zápis jen
 *   pozoruje, deska ho zpracuje beze změny (zápis nad 0E008h zahodí).
 *   Reaguje stejně zápis CPU i debuggeru (DEVBUS_ACC_DEBUGGER), Prohibited
 *   (OUT 0E5h) oblast 0E000h - 0E7FFh neodpojuje, proto se nekontroluje.
 * - render_row: suc80_render_line() s VRAM g_memory.VRAM a generátorem,
 *   který dodá adaptér (zdroj glyfů jiného zařízení, např. PCG-700, nebo
 *   CG-ROM). Kreslí 40 i 80 sloupců podle latche, takže změna režimu platí
 *   od příštího kresleného řádku.
 * - reset stroje -> 40 sloupců; připojení (zapnutí napájení) -> 40 sloupců.
 * - Volba "attributes" (per_char / per_pair) je jediný zdroj granularity
 *   atributů; stav jádra ji jen zrcadlí (connect, option_changed, load).
 * - Snapshot: soubor "state.bin" ve formátu suc80_state_save() (verze 1).
 *
 * Vlákna: všechno kromě suc80_cat_read() běží v emu vlákně (nebo při
 * zastavené emulaci během načítání snapshotu). suc80_cat_read() čte jeden
 * bajt stavu z UI vlákna okna I/O; zápis bajtu je atomický a hodnota je
 * jen informativní.
 */

#include "suc80_devbus.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "suc80_core.h"
#include "hw-generic/devbus/devbus.h"
#include "hw-generic/memory/memory.h"


/** @brief Stav kitu (jediná instance, statický kontext zařízení). */
static st_SUC80 s_suc80;

/** @brief Slot zařízení v devbus (-1 = neregistrováno). */
static int s_slot = -1;


/* ------------------------------------------------------------------------- */
/*                                  Volby                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Klíčová slova volby "attributes" (pořadí = en_SUC80_ATTR_MODE).
 *
 * per_char = každý znak svůj atribut (0D800h + k / 0DC01h + k, výchozí),
 * per_pair = dvojice znaků sdílí atribut bloku A (0D800h + k).
 */
static const char *const s_attr_keywords[] = { "per_char", "per_pair", NULL };

/**
 * @brief Popisky hodnot volby "attributes" pro menu (keyword_labels).
 *
 * Stejné pořadí jako s_attr_keywords; anglické msgid, N_() je v UI souboru
 * src/ui-imgui/mz700/devices/suc80/suc80_ui_strings.cpp (texty musí být
 * stejné). INI, MCP a snapshot dál pracují s klíčovými slovy.
 */
static const char *const s_attr_labels[] = { "Per character", "Per pair of characters" };

/**
 * @brief Volby zařízení (popisky jsou anglické msgid, N_() je v UI souboru
 *        src/ui-imgui/mz700/devices/suc80/suc80_ui_strings.cpp).
 */
static const st_DEVBUS_OPTION s_options[] = {
    { "attributes", "Attributes in 80 columns", DEVBUS_OPT_KEYWORD, SUC80_ATTR_PER_CHAR, 0, 0,
      s_attr_keywords, DEVBUS_APPLY_LIVE, s_attr_labels },
};


/**
 * @brief Převezme volbu granularity atributů z devbus do stavu jádra.
 * @return true, když se volba změnila a obraz v 80 sloupcích je potřeba překreslit.
 */
static bool suc80_sync_attr_option(void)
{
    unsigned v = devbus_option_get(s_slot, SUC80_DEVBUS_OPT_ATTRIBUTES);
    en_SUC80_ATTR_MODE m = (v == SUC80_ATTR_PER_PAIR) ? SUC80_ATTR_PER_PAIR : SUC80_ATTR_PER_CHAR;
    if (suc80_get_attr_mode(&s_suc80) == m) return false;
    suc80_set_attr_mode(&s_suc80, m);
    return suc80_get_mode(&s_suc80) == SUC80_MODE_80;
}


/* ------------------------------------------------------------------------- */
/*                                  MMIO                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief Pozorovaný zápis na 0E00Ch - 0E00Fh: latch uloží bit D4.
 * @param ctx   Kontext (&s_suc80).
 * @param addr  Adresa zápisu (v rozsahu; adresu kit nedekóduje).
 * @param value Zapsaná hodnota.
 * @param flags Stav mapy a druh přístupu (DEVBUS_*); devbus volá handler jen
 *              ve stavu DEVBUS_MMIO_MAPPED.
 * @post Při změně režimu zneplatní obraz (překreslí se od dalšího řádku).
 */
static void suc80_mmio_write(void *ctx, uint16_t addr, uint8_t value, unsigned flags)
{
    (void) addr;
    (void) flags;
    if (suc80_latch_write((st_SUC80 *) ctx, value)) devbus_video_invalidate();
}

/** @brief MMIO rozsah latche, jen s mapovanou horní oblastí. */
static const st_DEVBUS_MMIO_RANGE s_mmio[] = {
    { 0xE00C, 0xE00F, DEVBUS_MMIO_MAPPED, suc80_mmio_write },
};


/* ------------------------------------------------------------------------- */
/*                                  Video                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Háček render_row: nakreslí řádek plátna 40 nebo 80 sloupců podle latche.
 * @param ctx      Kontext (&s_suc80).
 * @param canvas_y Řádek plátna 0 - 199.
 * @param cg       Platný generátor znaků 4 kB (zdroj glyfů, nebo CG-ROM).
 * @param dst      Začátek řádku plátna (640 bajtů).
 * @pre Emu vlákno, uvnitř zpracování události GDG nebo plného překreslení.
 */
static void suc80_render_row(void *ctx, unsigned canvas_y, const uint8_t *cg, uint8_t *dst)
{
    suc80_render_line((const st_SUC80 *) ctx, g_memory.VRAM, cg, devbus_mz700_colormap, canvas_y, dst);
}

/** @brief Video háčky: jen náhrada kreslení řádku, glyfy nemění. */
static const st_DEVBUS_VIDEO_OPS s_video = { NULL, suc80_render_row };


/* ------------------------------------------------------------------------- */
/*                              Katalog I/O                                  */
/* ------------------------------------------------------------------------- */

/** @brief Bit latche v katalogu I/O. */
static const st_IO_BIT_DESC s_cat_bits[] = {
    { 4, 1, "COL80", "1 = 80 columns, 0 = 40 columns (other bits ignored)" },
};

/**
 * @brief Hodnota latche pro okno I/O (zrcadlo stavu, latch na HW nejde přečíst).
 * @return 10h v režimu 80 sloupců, jinak 0.
 */
static uint8_t suc80_cat_read(void)
{
    return (s_suc80.mode == SUC80_MODE_80) ? SUC80_LATCH_BIT : 0;
}

/**
 * @brief Dekódování hodnoty latche pro okno I/O.
 * @param value Hodnota (zrcadlo nebo zapsaný bajt).
 * @return Statický řetězec "80 columns" nebo "40 columns".
 */
static const char *suc80_cat_decode(uint8_t value)
{
    return (value & SUC80_LATCH_BIT) ? "80 columns" : "40 columns";
}

/** @brief Položka katalogu I/O (prefix "Device - " = sekce Expansion devices). */
static const st_IO_PORT_DESC s_catalog[] = {
    { 0xE00C, "Device - SUC latch",
      "SUC 80-column kit: mode latch, any write to 0E00Ch-0E00Fh stores D4 (write-only, value is a mirror)",
      IO_PORT_DIR_W, s_cat_bits, 1, suc80_cat_read, suc80_cat_decode, MZ_AVAIL_700, IO_CROSS_NONE },
};


/* ------------------------------------------------------------------------- */
/*                                 Snapshot                                  */
/* ------------------------------------------------------------------------- */

/** @brief Jméno souboru stavu v devices/devbus/suc80/. */
#define SUC80_SNAP_FILE "state.bin"

/**
 * @brief Uloží stav kitu (formát suc80_state_save(), verze SUC80_STATE_VERSION).
 * @return Výsledek zápisu souboru.
 */
static en_SNAPSHOT_RESULT suc80_snap_save(void *ctx, st_DEVBUS_SNAP *s)
{
    uint8_t buf[SUC80_STATE_SIZE];
    size_t n = suc80_state_save((const st_SUC80 *) ctx, buf, sizeof(buf));
    if (n != sizeof(buf)) return SNAPSHOT_ERR_IO;
    return devbus_snap_write_bin(s, SUC80_SNAP_FILE, buf, n);
}

/**
 * @brief Načte stav kitu.
 *
 * Režim latche ze souboru stavu; granularitu atributů vždy z volby (volby
 * snapshotu nastavil devbus před voláním). Chybějící nebo neplatný soubor
 * = chyba (devbus zařízení odpojí a zapíše varování).
 *
 * @param ctx     Kontext (&s_suc80).
 * @param s       Kontext snapshotu.
 * @param version Verze stavu ze snapshotu (1).
 * @return SNAPSHOT_OK, nebo chyba čtení / SNAPSHOT_ERR_CORRUPTED pro neplatný obsah.
 */
static en_SNAPSHOT_RESULT suc80_snap_load(void *ctx, st_DEVBUS_SNAP *s, uint32_t version)
{
    (void) version;
    uint8_t buf[SUC80_STATE_SIZE];
    en_SNAPSHOT_RESULT r = devbus_snap_read_bin_into(s, SUC80_SNAP_FILE, buf, sizeof(buf));
    if (r != SNAPSHOT_OK) return r;
    if (suc80_state_load((st_SUC80 *) ctx, buf, sizeof(buf)) != SUC80_OK) return SNAPSHOT_ERR_CORRUPTED;
    suc80_sync_attr_option();
    return SNAPSHOT_OK;
}


/* ------------------------------------------------------------------------- */
/*                             Životní cyklus                                */
/* ------------------------------------------------------------------------- */

/** @brief Zapnutí napájení kitu: 40 sloupců, granularita z volby. */
static int suc80_connect(void *ctx)
{
    suc80_init((st_SUC80 *) ctx);
    suc80_sync_attr_option();
    return 0;
}

/** @brief Signál RESET stroje: latch -> 40 sloupců. */
static void suc80_reset_cb(void *ctx)
{
    if (suc80_reset((st_SUC80 *) ctx)) devbus_video_invalidate();
}

/** @brief Změna volby za běhu (jediná volba: granularita atributů). */
static void suc80_option_changed(void *ctx, unsigned opt, unsigned value)
{
    (void) ctx;
    (void) value;
    if (opt != SUC80_DEVBUS_OPT_ATTRIBUTES) return;
    if (suc80_sync_attr_option()) devbus_video_invalidate();
}

/** @brief Po načtení snapshotu: obraz překreslit s načteným režimem. */
static void suc80_after_load(void *ctx)
{
    (void) ctx;
    devbus_video_invalidate();
}

/** @brief Životní cyklus zařízení. */
static const st_DEVBUS_LIFECYCLE s_ops = {
    NULL,                 /* init: výchozí stav nastaví connect */
    suc80_connect,
    suc80_reset_cb,
    NULL,                 /* disconnect: háčky odpojí devbus */
    NULL,                 /* destroy */
    suc80_option_changed,
    NULL,                 /* frame_done */
    suc80_after_load,
    NULL,                 /* rom_changed: generátor dodává adaptér při každém řádku */
};


/** @brief Deskriptor zařízení (název je anglický msgid, N_() v UI souboru). */
static const st_DEVBUS_DEVICE_DESC s_desc = {
    .id = SUC80_DEVBUS_ID,
    .name = "SUC 80-column kit",
    .ini_section = SUC80_DEVBUS_INI_SECTION,
    .platforms = DEVBUS_PLATFORM_MZ700,
    .options = s_options,
    .option_count = sizeof(s_options) / sizeof(s_options[0]),
    .ports = NULL,
    .port_count = 0,
    .mmio = s_mmio,
    .mmio_count = sizeof(s_mmio) / sizeof(s_mmio[0]),
    .video = &s_video,
    .regions = NULL,
    .region_count = 0,
    .io_catalog = s_catalog,
    .io_catalog_count = sizeof(s_catalog) / sizeof(s_catalog[0]),
    .snap = { 1, suc80_snap_save, suc80_snap_load },
    .ops = &s_ops,
};


void suc80_devbus_register(void)
{
    suc80_init(&s_suc80);
    s_slot = devbus_register(&s_desc, &s_suc80);
    if (s_slot < 0) {
        fprintf(stderr, "suc80: devbus registration failed (error %d)\n", s_slot);
    }
}
