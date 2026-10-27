/**
 * @file   pcg700_devbus.c
 * @brief  PCG-700 jako zařízení devbus (kontrakt v pcg700_devbus.h).
 *
 * Jedna statická instance modelu pcg700_core. Zápisy CPU na 0E010h-0E012h
 * přicházejí jako pozorovaný MMIO zápis (deska se chová jako bez karty,
 * čtení zůstává beze změny - registry jsou jen pro zápis). Obraz se mění
 * jen přes háček glyph_source: adaptér MZ-700 kreslí řádek z generátoru,
 * který vrací pcg700_core_glyphs() (efektivní generátor, nebo CG-ROM).
 *
 * Rozhodnutí lepidla (devdoc/pcg700.md, oddíl Lepidlo):
 * - Zápis debuggeru do paměti (DEVBUS_ACC_DEBUGGER) působí na kartu stejně
 *   jako zápis CPU: karta vidí jen sběrnici.
 * - Stav Prohibited (OUT 0E5h) se nerozlišuje, dekód řídí jen maska stavů
 *   mapy (volba decode_dram) [neověřeno].
 * - Konfigurace (přepínače, volby) není v binárním stavu jádra; snapshot ji
 *   nese jako volby zařízení v devices/devbus.xml a after_load() ji z voleb
 *   znovu promítne do modelu.
 *
 * @par Licence: GPLv3
 */

#include "pcg700_devbus.h"

#include <stddef.h>
#include <stdio.h>

#include "hw-generic/devbus/devbus.h"
#include "hw-generic/memory/rom.h"


/** @brief Instance modelu (žádné ukazatele uvnitř, stačí statická). */
static st_PCG700 s_pcg;

/** @brief Slot z devbus_register() (záporné = neregistrováno). */
static int s_slot = DEVBUS_ERR_NOT_FOUND;


/* ------------------------------------------------------------------------- */
/*                                 Volby                                     */
/* ------------------------------------------------------------------------- */

/*
 * Klíčová slova (INI, MCP) jsou v pořadí hodnot výčtů jádra; popisky hodnot
 * (keyword_labels) a popisky voleb jsou anglické msgid, do .pot je dostane
 * N_() v src/ui-imgui/mz700/devices/pcg700/pcg700_ui.cpp (texty musí být
 * stejné).
 */

/** @brief hsw: en_PCG700_HSW. */
static const char *const s_kw_hsw[] = { "nom", "pcg", NULL };
/** @brief Popisky hodnot hsw. */
static const char *const s_lb_hsw[] = { "NOM (standard character set)", "PCG" };
/** @brief ssw a gsw: en_PCG700_SSW / en_PCG700_GSW. */
static const char *const s_kw_onoff[] = { "off", "on", NULL };
/** @brief Popisky hodnot ssw. */
static const char *const s_lb_ssw[] = { "OFF (software cannot switch PCG)", "ON (bit 3 of 0E012h switches PCG)" };
/** @brief Popisky hodnot gsw. */
static const char *const s_lb_gsw[] = { "OFF (registers disconnected)", "ON (registers connected)" };
/** @brief ssw_off: en_PCG700_SSWOFF. */
static const char *const s_kw_sswoff[] = { "hsw", "force_off", NULL };
/** @brief Popisky hodnot ssw_off. */
static const char *const s_lb_sswoff[] = { "PCG on, HSW decides", "PCG stays off" };
/** @brief strobe: en_PCG700_STROBE. */
static const char *const s_kw_strobe[] = { "rising", "level", "falling", NULL };
/** @brief Popisky hodnot strobe. */
static const char *const s_lb_strobe[] = {
    "Rising edge of bit 4",
    "Every write while bit 4 is set",
    "Falling edge of bit 4 (experimental)",
};
/** @brief poweron_ram: en_PCG700_POWERON_RAM. */
static const char *const s_kw_poweron[] = { "zero", "cgrom", NULL };
/** @brief Popisky hodnot poweron_ram. */
static const char *const s_lb_poweron[] = { "Zeros", "Copy of CG-ROM characters" };

/** @brief Volby zařízení (indexy en_PCG700_DEVBUS_OPT). */
static const st_DEVBUS_OPTION s_options[PCG700_OPT_COUNT] = {
    [PCG700_OPT_HSW] = { "hsw", "HSW switch", DEVBUS_OPT_KEYWORD, PCG700_HSW_PCG, 0, 0,
                         s_kw_hsw, DEVBUS_APPLY_LIVE, s_lb_hsw },
    [PCG700_OPT_SSW] = { "ssw", "SSW jumper (software control)", DEVBUS_OPT_KEYWORD, PCG700_SSW_ON, 0, 0,
                         s_kw_onoff, DEVBUS_APPLY_LIVE, s_lb_ssw },
    [PCG700_OPT_GSW] = { "gsw", "GSW jumper (registers)", DEVBUS_OPT_KEYWORD, PCG700_GSW_ON, 0, 0,
                         s_kw_onoff, DEVBUS_APPLY_LIVE, s_lb_gsw },
    [PCG700_OPT_SSWOFF] = { "ssw_off", "HSW PCG with SSW OFF", DEVBUS_OPT_KEYWORD, PCG700_SSWOFF_HSW_DECIDES, 0, 0,
                            s_kw_sswoff, DEVBUS_APPLY_LIVE, s_lb_sswoff },
    [PCG700_OPT_STROBE] = { "strobe", "Write strobe", DEVBUS_OPT_KEYWORD, PCG700_STROBE_RISING, 0, 0,
                            s_kw_strobe, DEVBUS_APPLY_LIVE, s_lb_strobe },
    [PCG700_OPT_DECODE_DRAM] = { "decode_dram", "Decode writes also with DRAM at 0E000h", DEVBUS_OPT_BOOL, 0, 0, 0,
                                 NULL, DEVBUS_APPLY_LIVE, NULL },
    [PCG700_OPT_POWERON_RAM] = { "poweron_ram", "RAM after power on", DEVBUS_OPT_KEYWORD, PCG700_POWERON_RAM_ZERO, 0, 0,
                                 s_kw_poweron, DEVBUS_APPLY_CONNECT, s_lb_poweron },
};


void pcg700_devbus_config_from_options(int slot, st_PCG700_CONFIG *cfg)
{
    pcg700_core_config_default(cfg);
    if (slot < 0) return;
    cfg->hsw = (en_PCG700_HSW) devbus_option_get(slot, PCG700_OPT_HSW);
    cfg->ssw = (en_PCG700_SSW) devbus_option_get(slot, PCG700_OPT_SSW);
    cfg->gsw = (en_PCG700_GSW) devbus_option_get(slot, PCG700_OPT_GSW);
    cfg->sswoff = (en_PCG700_SSWOFF) devbus_option_get(slot, PCG700_OPT_SSWOFF);
    cfg->strobe = (en_PCG700_STROBE) devbus_option_get(slot, PCG700_OPT_STROBE);
    cfg->poweron_ram = (en_PCG700_POWERON_RAM) devbus_option_get(slot, PCG700_OPT_POWERON_RAM);
    /* devbus hodnoty KEYWORD validuje proti počtu slov = počtu hodnot výčtu;
     * pojistka pro případ neshody tabulek. */
    if (!pcg700_core_config_valid(cfg)) pcg700_core_config_default(cfg);
}


/**
 * @brief Promítne volbu decode_dram do masky stavů MMIO rozsahu.
 * @pre Emu vlákno; s_slot platný.
 * @post Maska MAPPED, nebo MAPPED | DRAM; devbus případně vymění obálku zápisu.
 */
static void pcg700_devbus_apply_decode(void)
{
    unsigned when = DEVBUS_MMIO_MAPPED;
    if (devbus_option_get(s_slot, PCG700_OPT_DECODE_DRAM)) when |= DEVBUS_MMIO_DRAM;
    (void) devbus_mmio_set_when(s_slot, 0, when);
}


/**
 * @brief Promítne všechny volby do modelu a dekódu, při potřebě invaliduje obraz.
 * @pre Emu vlákno, zařízení připojené (nebo v průběhu načtení snapshotu).
 */
static void pcg700_devbus_apply_options(void)
{
    st_PCG700_CONFIG cfg;
    pcg700_devbus_config_from_options(s_slot, &cfg);
    unsigned evt = pcg700_core_set_config(&s_pcg, &cfg);
    pcg700_devbus_apply_decode();
    if (pcg700_core_event_needs_redraw(&s_pcg, evt)) devbus_video_invalidate();
}


/* ------------------------------------------------------------------------- */
/*                              MMIO a obraz                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Pozorovaný zápis na 0E010h-0E012h.
 *
 * Volá ho devbus jen pro adresy rozsahu a stav mapy z masky (výchozí jen
 * mapovaná horní oblast, s volbou decode_dram i DRAM). Po zápisu, který
 * mění zobrazený obraz, vyžádá překreslení.
 *
 * @param ctx   Instance modelu.
 * @param addr  0E010h - 0E012h.
 * @param value Zapsaný bajt.
 * @param flags Stav mapy | DEVBUS_ACC_* (nerozlišuje se, viz hlavička souboru).
 */
static void pcg700_devbus_mmio_write(void *ctx, uint16_t addr, uint8_t value, unsigned flags)
{
    (void) flags;
    st_PCG700 *pcg = ctx;
    unsigned evt = pcg700_core_write(pcg, (unsigned) (addr - PCG700_MMIO_FIRST), value, g_rom.cgrom);
    if (pcg700_core_event_needs_redraw(pcg, evt)) devbus_video_invalidate();
}

/** @brief MMIO rozsah: tři registry, výchozí jen s mapovanou horní oblastí. */
static const st_DEVBUS_MMIO_RANGE s_mmio[] = {
    /* Registry jsou podle manuálu HAL jen pro zápis ("All bits are write
     * only"): deska sběrnici při čtení nebudí (DEVBUS_DRIVE_NONE). */
    { .first = PCG700_MMIO_FIRST, .last = PCG700_MMIO_LAST, .when_default = DEVBUS_MMIO_MAPPED,
      .drive = DEVBUS_DRIVE_NONE, .write = pcg700_devbus_mmio_write },
};

/**
 * @brief Zdroj glyfů (háček glyph_source): efektivní generátor, nebo CG-ROM.
 * @param ctx Instance modelu.
 * @return 4 kB ve formátu CG-ROM, nikdy NULL (g_rom.cgrom je platná po instalaci ROM).
 */
static const uint8_t *pcg700_devbus_glyphs(void *ctx)
{
    return pcg700_core_glyphs(ctx, g_rom.cgrom);
}

/** @brief Video háčky: jen zdroj glyfů, rozložení řádku beze změny. */
static const st_DEVBUS_VIDEO_OPS s_video = { pcg700_devbus_glyphs, NULL };


/* ------------------------------------------------------------------------- */
/*                         Region a katalog I/O                              */
/* ------------------------------------------------------------------------- */

/** @brief Přímý ukazatel na PCG RAM (formát CG-ROM, levý bod = bit 0). */
static uint8_t *pcg700_devbus_region_data(void *ctx)
{
    return ((st_PCG700 *) ctx)->ram;
}

/**
 * @brief Po zápisu debuggeru do PCG RAM: přestavět efektivní generátor.
 * @param ctx Instance modelu.
 * @param off Offset zápisu (nepoužito, přestavba je celá).
 * @param len Délka zápisu (nepoužito).
 */
static void pcg700_devbus_region_written(void *ctx, uint32_t off, uint32_t len)
{
    (void) off;
    (void) len;
    st_PCG700 *pcg = ctx;
    pcg700_core_rebuild(pcg, g_rom.cgrom);
    if (pcg700_core_display_enabled(pcg)) devbus_video_invalidate();
}

/**
 * @brief Region PCG RAM.
 *
 * Obsah je ve formátu CG-ROM emulátoru (levý bod = bit 0), tedy bitově
 * převrácený proti bajtu, který program zapisuje na 0E010h. Adresa
 * (kód - 80h) * 8 + řádek, sada 2 od 400h.
 */
static const st_DEVBUS_REGION s_regions[] = {
    { "PCG-700 RAM (2 KB, CG-ROM bit order)", PCG700_RAM_SIZE, 1,
      pcg700_devbus_region_data, pcg700_devbus_region_written },
};

/** @brief Katalog: poslední zapsaná hodnota 0E010h (registry jsou jen pro zápis). */
static uint8_t pcg700_devbus_cat_data(void) { return s_pcg.data; }
/** @brief Katalog: poslední zapsaná hodnota 0E011h. */
static uint8_t pcg700_devbus_cat_addr(void) { return s_pcg.addr_lo; }
/** @brief Katalog: poslední zapsaná hodnota 0E012h. */
static uint8_t pcg700_devbus_cat_ctrl(void) { return s_pcg.ctrl; }

/** @brief Bity registru řízení 0E012h. */
static const st_IO_BIT_DESC s_ctrl_bits[] = {
    { 0, 2, "ADDR9-8", "PCG RAM address bits 9-8" },
    { 2, 1, "SET", "Character set: 0 = set 1, 1 = set 2 (address bit 10)" },
    { 3, 1, "OFF", "0 = PCG on, 1 = PCG off (only with SSW ON)" },
    { 4, 1, "STROBE", "Write strobe: 1 = data valid, must be reset again" },
    { 5, 1, "COPY", "0 = write byte from 0E010h, 1 = copy byte from CG-ROM" },
};

/**
 * @brief Katalog I/O (sekce Expansion devices okna I/O, prefix "Device - ").
 *
 * read_value vrací latch modelu bez vedlejšího efektu; CPU z adres nic
 * nového nečte (čtení vrací horní ROM jako bez karty).
 */
static const st_IO_PORT_DESC s_catalog[] = {
    { 0xE010, "Device - PCG-700 data", "PCG-700 pattern byte (bit 7 = left dot), write only; shows the last written value",
      IO_PORT_DIR_W, NULL, 0, pcg700_devbus_cat_data, NULL, MZ_AVAIL_700, IO_CROSS_NONE },
    { 0xE011, "Device - PCG-700 address", "PCG-700 RAM address bits 7-0, write only; shows the last written value",
      IO_PORT_DIR_W, NULL, 0, pcg700_devbus_cat_addr, NULL, MZ_AVAIL_700, IO_CROSS_NONE },
    { 0xE012, "Device - PCG-700 control", "PCG-700 address bits 10-8 and control, write only; shows the last written value",
      IO_PORT_DIR_W, s_ctrl_bits, sizeof(s_ctrl_bits) / sizeof(s_ctrl_bits[0]), pcg700_devbus_cat_ctrl, NULL,
      MZ_AVAIL_700, IO_CROSS_NONE },
};


/* ------------------------------------------------------------------------- */
/*                                Snapshot                                   */
/* ------------------------------------------------------------------------- */

/** @brief Jméno souboru stavu pod devices/devbus/pcg700/. */
#define PCG700_SNAP_FILE "state.bin"

/**
 * @brief Uloží binární stav jádra (latche + RAM, PCG700_STATE_SIZE B).
 * @return SNAPSHOT_OK, nebo chyba zápisu.
 */
static en_SNAPSHOT_RESULT pcg700_devbus_save(void *ctx, st_DEVBUS_SNAP *s)
{
    uint8_t buf[PCG700_STATE_SIZE];
    size_t n = pcg700_core_state_save(ctx, buf, sizeof(buf));
    if (n != sizeof(buf)) return SNAPSHOT_ERR_COMPONENT;
    return devbus_snap_write_bin(s, PCG700_SNAP_FILE, buf, n);
}

/**
 * @brief Načte binární stav jádra.
 *
 * Verze devbus = PCG700_STATE_VERSION; hlavičku bloku (magic, verze, délka)
 * kontroluje pcg700_core_state_load(). Konfigurace z voleb se promítne
 * v after_load().
 *
 * @return SNAPSHOT_OK; SNAPSHOT_ERR_MISSING_FILE / _IO (chybí soubor),
 *         SNAPSHOT_ERR_CORRUPTED (neplatný blok, instance beze změny).
 */
static en_SNAPSHOT_RESULT pcg700_devbus_load(void *ctx, st_DEVBUS_SNAP *s, uint32_t version)
{
    (void) version;  /* zatím jediná verze 1 */
    uint8_t buf[PCG700_STATE_SIZE];
    en_SNAPSHOT_RESULT r = devbus_snap_read_bin_into(s, PCG700_SNAP_FILE, buf, sizeof(buf));
    if (r != SNAPSHOT_OK) return r;
    unsigned evt = 0;
    if (pcg700_core_state_load(ctx, buf, sizeof(buf), g_rom.cgrom, &evt) != PCG700_OK) return SNAPSHOT_ERR_CORRUPTED;
    return SNAPSHOT_OK;
}


/* ------------------------------------------------------------------------- */
/*                            Životní cyklus                                 */
/* ------------------------------------------------------------------------- */

/** @brief init(): výchozí stav instance, maska dekódu podle INI. */
static int pcg700_devbus_init(void *ctx)
{
    st_PCG700_CONFIG cfg;
    s_slot = devbus_find(PCG700_DEVBUS_ID);
    pcg700_devbus_config_from_options(s_slot, &cfg);
    if (pcg700_core_power_on(ctx, &cfg, NULL) != PCG700_OK) return -1;
    pcg700_devbus_apply_decode();
    return 0;
}

/**
 * @brief connect(): zapnutí napájení karty (RAM podle poweron_ram, latche, bit 3 = 1).
 * @return 0, nebo -1 při neplatné konfiguraci.
 */
static int pcg700_devbus_connect(void *ctx)
{
    st_PCG700_CONFIG cfg;
    pcg700_devbus_config_from_options(s_slot, &cfg);
    if (pcg700_core_power_on(ctx, &cfg, g_rom.cgrom) != PCG700_OK) return -1;
    pcg700_devbus_apply_decode();
    /* Obraz překreslí adaptér při změně háčku glyph_source. */
    return 0;
}

/** @brief reset(): RESET stroje nuluje latche (bit 3 = 1), RAM zůstává. */
static void pcg700_devbus_reset(void *ctx)
{
    unsigned evt = pcg700_core_reset(ctx);
    if (pcg700_core_event_needs_redraw(ctx, evt)) devbus_video_invalidate();
}

/** @brief option_changed(): volba LIVE připojeného zařízení. */
static void pcg700_devbus_option_changed(void *ctx, unsigned opt, unsigned value)
{
    (void) ctx;
    (void) opt;
    (void) value;
    pcg700_devbus_apply_options();
}

/** @brief after_load(): konfigurace z voleb snapshotu, generátor z aktuální CG-ROM. */
static void pcg700_devbus_after_load(void *ctx)
{
    pcg700_devbus_apply_options();
    pcg700_core_rebuild(ctx, g_rom.cgrom);
    devbus_video_invalidate();
}

/** @brief rom_changed(): nová CG-ROM, přestavět efektivní generátor. */
static void pcg700_devbus_rom_changed(void *ctx)
{
    pcg700_core_rebuild(ctx, g_rom.cgrom);
    devbus_video_invalidate();
}

/** @brief Životní cyklus. */
static const st_DEVBUS_LIFECYCLE s_lifecycle = {
    .init = pcg700_devbus_init,
    .connect = pcg700_devbus_connect,
    .reset = pcg700_devbus_reset,
    .option_changed = pcg700_devbus_option_changed,
    .after_load = pcg700_devbus_after_load,
    .rom_changed = pcg700_devbus_rom_changed,
};

/** @brief Deskriptor PCG-700. */
static const st_DEVBUS_DEVICE_DESC s_desc = {
    .id = PCG700_DEVBUS_ID,
    .name = "PCG-700 (HAL)",
    .ini_section = "PCG700",
    .platforms = DEVBUS_PLATFORM_MZ700,
    .options = s_options,
    .option_count = PCG700_OPT_COUNT,
    .mmio = s_mmio,
    .mmio_count = sizeof(s_mmio) / sizeof(s_mmio[0]),
    .video = &s_video,
    .regions = s_regions,
    .region_count = sizeof(s_regions) / sizeof(s_regions[0]),
    .io_catalog = s_catalog,
    .io_catalog_count = sizeof(s_catalog) / sizeof(s_catalog[0]),
    .snap = { PCG700_STATE_VERSION, pcg700_devbus_save, pcg700_devbus_load },
    .ops = &s_lifecycle,
};


int pcg700_devbus_register(void)
{
    s_slot = devbus_register(&s_desc, &s_pcg);
    if (s_slot < 0) fprintf(stderr, "PCG-700: device registration failed (%d)\n", s_slot);
    return s_slot;
}


int pcg700_devbus_slot(void)
{
    return s_slot;
}


const st_PCG700 *pcg700_devbus_instance(void)
{
    return &s_pcg;
}
