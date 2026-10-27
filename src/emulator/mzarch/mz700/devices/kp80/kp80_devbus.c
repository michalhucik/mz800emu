/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file kp80_devbus.c
 * @brief Lepidlo devbus 80-Zeichen-Karte K&P pro MZ-700 (kontrakt v kp80_devbus.h).
 *
 * Deskriptor zařízení `kp80`: porty 70h - 73h (úplný dekód, bez zrcadel),
 * volby, sekundární výstup 640 x 250, regiony (VRAM karty, font, registry
 * CRTC), katalog I/O a snapshot. Stav karty je jedna statická instance
 * (karta je v systému nejvýš jednou, katalog I/O čte hodnoty bez kontextu).
 *
 * Model a rozhodnutí: `devdoc/kp80.md`, oddíly jádra a lepidla.
 */

#include "kp80_devbus.h"

#include <stdio.h>
#include <string.h>

#include "kp80_core.h"
#include "emulator.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/gdgclk.h"


/* ========================================================================= */
/*                                 Stav                                      */
/* ========================================================================= */

/** @brief Velikost regionu "registry CRTC" pro debugger (B). */
#define KP80_REGS_VIEW_SIZE 32u

/** @brief Offset AR v regionu registrů. */
#define KP80_REGS_VIEW_AR 20u
/** @brief Offset příznaků v regionu registrů (bit 0 čekající adresa, bit 1 update ready, bit 2 V-blank). */
#define KP80_REGS_VIEW_FLAGS 21u
/** @brief Offset aktivní update adresy (2 B, little-endian) v regionu registrů. */
#define KP80_REGS_VIEW_UPD 22u
/** @brief Offset start adresy obrazu R12:R13 (2 B, little-endian) v regionu registrů. */
#define KP80_REGS_VIEW_START 24u
/** @brief Offset pozice kurzoru R14:R15 (2 B, little-endian) v regionu registrů. */
#define KP80_REGS_VIEW_CURSOR 26u

/**
 * @brief Stav zařízení K&P 80 v emulátoru.
 *
 * @invariant `slot` je platný slot devbus od registrace (jinak -1);
 *            `vout` je výstup tohoto slotu nebo NULL; `phosphor` <
 *            KP80_PHOSPHOR_COUNT; `wait` <= KP80_DEVBUS_WAIT_MAX.
 *
 * Vlastnictví: výstup patří jádru devbus (uvolní ho devbus_exit()).
 */
typedef struct st_KP80_DEV {
    st_KP80 kp;                               /**< Čisté jádro karty (registry, VRAM, časování, volby modelu). */
    int slot;                                 /**< Slot v devbus, nebo -1. */
    st_DEVBUS_VOUT *vout;                     /**< Sekundární výstup (druhý monitor). */
    unsigned wait;                            /**< WAIT na 71h - 73h v T-stavech (volba `wait`). */
    unsigned phosphor;                        /**< Barva fosforu (en_KP80_PHOSPHOR, volba `phosphor`). */
    bool dirty;                               /**< Stav karty se od posledního kreslení změnil. */
    bool last_cursor;                         /**< Viditelnost kurzoru při posledním kreslení (blikání). */
    uint8_t regs_view[KP80_REGS_VIEW_SIZE];   /**< Zrcadlo registrů pro region debuggeru (plní se při čtení). */
} st_KP80_DEV;

/** @brief Jediná instance karty. */
static st_KP80_DEV s_kp80 = { .slot = -1 };


/* ========================================================================= */
/*                                  Čas                                      */
/* ========================================================================= */

/**
 * @brief Aktuální čas ve znakových taktech karty (2 MHz).
 *
 * Takty GDG od startu emulátoru (devbus_time_ticks(), neklesající, reset
 * stroje je nenuluje) přepočtené `ticks * 2 000 000 / GDGCLK_BASE`. Rozklad
 * na celé sekundy a zbytek se vyhne přetečení 64 bitů i kumulaci chyby
 * zaokrouhlení: výsledek je přesně dolní celá část podílu.
 *
 * @return Čas v taktech KP80_CHAR_CLOCK_HZ.
 */
static uint64_t kp80_dev_now(void)
{
    const uint64_t base = (uint64_t) GDGCLK_BASE;
    uint64_t ticks = devbus_time_ticks();
    uint64_t sec = ticks / base;
    uint64_t rem = ticks % base;
    return sec * KP80_CHAR_CLOCK_HZ + (rem * KP80_CHAR_CLOCK_HZ) / base;
}


/* ========================================================================= */
/*                                 Výstup                                    */
/* ========================================================================= */

/**
 * @brief Paleta výstupu (0xRRGGBBAA): 0 tma, 1 bílý, 2 zelený, 3 jantarový fosfor.
 *
 * Barvy jsou volba zobrazení (přibližné odstíny monitorů té doby), ne
 * vlastnost karty: karta dává jen jednobitový video signál.
 */
static const uint32_t s_palette[1 + KP80_PHOSPHOR_COUNT] = {
    0x000000FFu, /* tma */
    0xF0F0F0FFu, /* bílý fosfor */
    0x33FF66FFu, /* zelený fosfor */
    0xFFB000FFu, /* jantarový fosfor */
};

/**
 * @brief Popis výstupu: 640 x 250 bodů, obraz 4:3.
 *
 * Poměr stran bodu (šířka / výška) = (4 / 3) / (640 / 250) = 0,52: celý
 * zobrazený rastr na monitoru 4:3 (přibližně, okraje monitoru se nemodelují).
 */
static const st_DEVBUS_VOUT_DESC s_vout_desc = {
    KP80_DEVBUS_VOUT_ID,
    "K&P 80 Column Monitor",
    KP80_FB_WIDTH,
    KP80_FB_HEIGHT,
    (4.0f / 3.0f) / ((float) KP80_FB_WIDTH / (float) KP80_FB_HEIGHT),
    s_palette,
    (uint16_t) (1 + KP80_PHOSPHOR_COUNT),
};


/**
 * @brief Nakreslí snímek druhého monitoru a předá ho čtenářům výstupu.
 *
 * @param d   Zařízení.
 * @param now Čas ve znakových taktech.
 * @pre Emu vlákno mimo zpracování instrukce (frame_done, zásah, load).
 * @post dirty = false, last_cursor = viditelnost kurzoru v čase now.
 */
static void kp80_dev_render(st_KP80_DEV *d, uint64_t now)
{
    if (!d->vout) return;
    uint8_t *fb = devbus_vout_draw_buffer(d->vout);
    kp80_render(&d->kp, now, fb);
    /* Jádro kreslí 0 / 1; svítící bod dostane index barvy fosforu. */
    uint8_t lit = (uint8_t) (1u + d->phosphor);
    if (lit != 1u) {
        for (size_t i = 0; i < KP80_FB_SIZE; i++) {
            if (fb[i]) fb[i] = lit;
        }
    }
    devbus_vout_present(d->vout);
    d->dirty = false;
    d->last_cursor = kp80_cursor_visible(&d->kp, now);
}


/**
 * @brief Označí změnu obrazu; v pauze výstup překreslí hned.
 *
 * Za běhu kreslí konec snímku (frame_done). V pauze žádný snímek neskončí,
 * takže zásah debuggeru nebo MCP (io_write, zápis do regionu VRAM, změna
 * volby) by se na druhém monitoru neprojevil.
 *
 * @param d Zařízení.
 * @pre Emu vlákno.
 */
static void kp80_dev_touch(st_KP80_DEV *d)
{
    d->dirty = true;
    if (EMULATOR_TEST_PAUSED) kp80_dev_render(d, kp80_dev_now());
}


/* ========================================================================= */
/*                                 Volby                                     */
/* ========================================================================= */

/** @brief Klíčová slova volby `phosphor` (pořadí = en_KP80_PHOSPHOR). */
static const char *const s_phosphor_words[] = { "white", "green", "amber", NULL };

/**
 * @brief Popisky hodnot volby `phosphor` pro menu (keyword_labels).
 *
 * Stejné pořadí jako s_phosphor_words; anglické msgid, N_() je v UI souboru
 * okna karty (kp80_window.cpp). INI, MCP a snapshot pracují s klíčovými slovy.
 */
static const char *const s_phosphor_labels[] = { "White", "Green", "Amber" };

/**
 * @brief Volby zařízení (indexy en_KP80_DEVBUS_OPT).
 *
 * Popisky jsou anglické msgid; do .pot je značí UI soubor okna karty
 * (kp80_window.cpp). Důvody výchozích hodnot: `devdoc/kp80.md`.
 */
static const st_DEVBUS_OPTION s_options[KP80_OPT_COUNT] = {
    { "read_autoinc", "Read from 70h increments the address",
      DEVBUS_OPT_BOOL, 1, 0, 0, NULL, DEVBUS_APPLY_LIVE },
    { "data_to_reg", "Write to 71h also writes the selected CRTC register",
      DEVBUS_OPT_BOOL, 1, 0, 0, NULL, DEVBUS_APPLY_LIVE },
    { "addr_on_r31", "Update address (R18/R19) takes effect on R31 access",
      DEVBUS_OPT_BOOL, 1, 0, 0, NULL, DEVBUS_APPLY_LIVE },
    { "update_delay", "Update cycle length (character clocks)",
      DEVBUS_OPT_UNSIGNED, 0, 0, KP80_DEVBUS_UPDATE_DELAY_MAX, NULL, DEVBUS_APPLY_LIVE },
    { "wait", "WAIT on ports 71h - 73h (T-states)",
      DEVBUS_OPT_UNSIGNED, 0, 0, KP80_DEVBUS_WAIT_MAX, NULL, DEVBUS_APPLY_LIVE },
    { "phosphor", "Monitor phosphor color",
      DEVBUS_OPT_KEYWORD, KP80_PHOSPHOR_WHITE, 0, 0, s_phosphor_words, DEVBUS_APPLY_LIVE,
      s_phosphor_labels },
};


/**
 * @brief Nastaví jednu volbu do stavu zařízení a jádra.
 * @param d     Zařízení.
 * @param opt   Index volby (en_KP80_DEVBUS_OPT).
 * @param value Hodnota (devbus ji ověřil podle typu a rozsahu).
 */
static void kp80_dev_apply_option(st_KP80_DEV *d, unsigned opt, unsigned value)
{
    switch (opt) {
        case KP80_OPT_READ_AUTOINC: d->kp.opt.read_autoinc = (value != 0); break;
        case KP80_OPT_DATA_WRITE_TO_REG: d->kp.opt.data_write_to_reg = (value != 0); break;
        case KP80_OPT_ADDR_ON_R31: d->kp.opt.addr_on_r31 = (value != 0); break;
        case KP80_OPT_UPDATE_DELAY: d->kp.opt.update_delay = value; break;
        case KP80_OPT_WAIT: d->wait = (value <= KP80_DEVBUS_WAIT_MAX) ? value : KP80_DEVBUS_WAIT_MAX; break;
        case KP80_OPT_PHOSPHOR: d->phosphor = (value < KP80_PHOSPHOR_COUNT) ? value : KP80_PHOSPHOR_WHITE; break;
        default: break;
    }
}


/**
 * @brief Načte všechny volby z devbus (připojení, load snapshotu).
 * @param d Zařízení s platným slotem.
 */
static void kp80_dev_apply_all_options(st_KP80_DEV *d)
{
    for (unsigned o = 0; o < KP80_OPT_COUNT; o++) {
        kp80_dev_apply_option(d, o, devbus_option_get(d->slot, o));
    }
}


/* ========================================================================= */
/*                                 Porty                                     */
/* ========================================================================= */

/**
 * @brief IN z portu 70h - 73h.
 *
 * Nebuzená sběrnice (čtení R31) vrací latch datové sběrnice. Čtení nemění
 * obraz (jen update adresu a update cyklus), proto výstup neoznačuje.
 *
 * @param ctx  Zařízení.
 * @param port Adresa portu (jádro bere bity 0 - 1).
 * @return Hodnota portu.
 * @note Volba WAIT prodlouží přístup na 71h - 73h o `wait` T-stavů.
 */
static uint8_t kp80_dev_port_read(void *ctx, uint16_t port)
{
    st_KP80_DEV *d = ctx;
    uint8_t v = kp80_read(&d->kp, port & 0x03u, kp80_dev_now(), g_mzarch_main.regDBUS_latch);
    if (d->wait && kp80_port_has_wait(port & 0x03u)) devbus_cpu_wait(d->wait);
    return v;
}


/**
 * @brief Čtení portu bez vedlejších účinků (debugger, BP `port[N]`).
 * @param ctx  Zařízení.
 * @param port Adresa portu.
 * @return Totéž co kp80_dev_port_read() bez změny stavu a bez WAIT.
 */
static uint8_t kp80_dev_port_peek(void *ctx, uint16_t port)
{
    st_KP80_DEV *d = ctx;
    return kp80_peek(&d->kp, port & 0x03u, kp80_dev_now(), g_mzarch_main.regDBUS_latch);
}


/**
 * @brief OUT na port 70h - 73h.
 * @param ctx  Zařízení.
 * @param port Adresa portu.
 * @param v    Zapsaný bajt.
 * @post Výstup je označený ke kreslení (v pauze překreslený).
 */
static void kp80_dev_port_write(void *ctx, uint16_t port, uint8_t v)
{
    st_KP80_DEV *d = ctx;
    kp80_write(&d->kp, port & 0x03u, v, kp80_dev_now());
    if (d->wait && kp80_port_has_wait(port & 0x03u)) devbus_cpu_wait(d->wait);
    kp80_dev_touch(d);
}

/** @brief Porty karty: 70h - 73h (úplný dekód A7 - A2, bez zrcadel). */
static const st_DEVBUS_PORT_RANGE s_ports[] = {
    { 0x70, 0x73, kp80_dev_port_read, kp80_dev_port_peek, kp80_dev_port_write },
};


/* ========================================================================= */
/*                          Regiony debuggeru                                */
/* ========================================================================= */

/** @brief Region VRAM karty (2 048 B). */
static uint8_t *kp80_dev_region_vram(void *ctx)
{
    return ((st_KP80_DEV *) ctx)->kp.vram;
}

/** @brief Po zápisu debuggerem do VRAM karty: překreslit obraz. */
static void kp80_dev_region_vram_written(void *ctx, uint32_t off, uint32_t len)
{
    (void) off;
    (void) len;
    kp80_dev_touch((st_KP80_DEV *) ctx);
}

/** @brief Region fontu (2 048 B, jen pro čtení). */
static uint8_t *kp80_dev_region_font(void *ctx)
{
    st_KP80_DEV *d = ctx;
    /* Region je writable = 0, debugger do něj nezapisuje. */
    return (uint8_t *) (d->kp.font ? d->kp.font : g_kp80_font_rom);
}

/**
 * @brief Region registrů CRTC: zrcadlo naplněné v okamžiku čtení.
 *
 * Rozložení (32 B): 0 - 19 R0 - R19 (R18/R19 = latch), 20 AR, 21 příznaky
 * (bit 0 adresa čeká na R31, bit 1 update ready, bit 2 V-blank), 22 - 23
 * aktivní update adresa, 24 - 25 start obrazu R12:R13, 26 - 27 kurzor
 * R14:R15 (16bitové hodnoty little-endian), 28 - 31 nula.
 *
 * @param ctx Zařízení.
 * @return Zrcadlo `regs_view`.
 * @pre Emu vlákno (čtení regionu přes dbgapi).
 */
static uint8_t *kp80_dev_region_regs(void *ctx)
{
    st_KP80_DEV *d = ctx;
    uint64_t now = kp80_dev_now();
    uint8_t *v = d->regs_view;
    memset(v, 0, KP80_REGS_VIEW_SIZE);
    memcpy(v, d->kp.reg, KP80_NUM_REGS);
    v[KP80_REGS_VIEW_AR] = d->kp.ar;
    uint8_t st = kp80_status(&d->kp, now);
    v[KP80_REGS_VIEW_FLAGS] = (uint8_t) ((d->kp.upd_pending ? 0x01u : 0u)
                                         | ((st & KP80_STATUS_UPDATE_READY) ? 0x02u : 0u)
                                         | ((st & KP80_STATUS_VBLANK) ? 0x04u : 0u));
    v[KP80_REGS_VIEW_UPD] = (uint8_t) (d->kp.upd_addr & 0xFFu);
    v[KP80_REGS_VIEW_UPD + 1] = (uint8_t) (d->kp.upd_addr >> 8);
    v[KP80_REGS_VIEW_START] = d->kp.reg[13];
    v[KP80_REGS_VIEW_START + 1] = d->kp.reg[12];
    v[KP80_REGS_VIEW_CURSOR] = d->kp.reg[15];
    v[KP80_REGS_VIEW_CURSOR + 1] = d->kp.reg[14];
    return v;
}

/** @brief Regiony karty pro debugger a MCP (druh `device`). */
static const st_DEVBUS_REGION s_regions[] = {
    { "K&P 80 VRAM (2 KB)", KP80_VRAM_SIZE, 1, kp80_dev_region_vram, kp80_dev_region_vram_written },
    { "K&P 80 font (2 KB)", KP80_FONT_SIZE, 0, kp80_dev_region_font, NULL },
    { "K&P 80 CRTC registers", KP80_REGS_VIEW_SIZE, 0, kp80_dev_region_regs, NULL },
};


/* ========================================================================= */
/*                              Katalog I/O                                  */
/* ========================================================================= */

/** @brief Hodnota 70h pro okno I/O (bajt VRAM na update adrese, bez autoinkrementu). */
static uint8_t kp80_dev_cat_70(void)
{
    return kp80_dev_port_peek(&s_kp80, 0x70);
}

/** @brief Hodnota 71h / 73h pro okno I/O (vybraný registr podle čtení). */
static uint8_t kp80_dev_cat_reg(void)
{
    return kp80_dev_port_peek(&s_kp80, 0x73);
}

/** @brief Hodnota 72h pro okno I/O (status). */
static uint8_t kp80_dev_cat_status(void)
{
    return kp80_dev_port_peek(&s_kp80, 0x72);
}

/** @brief Bity portu 72h: čtení = status, zápis = výběr registru. */
static const st_IO_BIT_DESC s_bits_72[] = {
    { 7, 1, "UR", "IN: update ready (transparent VRAM access may continue)" },
    { 6, 1, "LPEN", "IN: light pen register full (always 0, the card has no light pen)" },
    { 5, 1, "VBLANK", "IN: vertical blanking (rows >= R6 * (R9 + 1))" },
    { 0, 5, "AR", "OUT: select CRTC register R0 - R19, R31 = update strobe" },
};

/** @brief Katalog I/O karty (sekce Expansion devices okna I/O). */
static const st_IO_PORT_DESC s_catalog[] = {
    { 0x0070, "Device - K&P 80 VRAM read (R)",
      "K&P 80 column card: IN = byte of card VRAM at the CRTC update address (option: address + 1)",
      IO_PORT_DIR_R, NULL, 0, kp80_dev_cat_70, NULL, MZ_AVAIL_700, IO_CROSS_NONE },
    { 0x0071, "Device - K&P 80 VRAM write / register (R/W)",
      "K&P 80 column card: OUT = byte to card VRAM at the update address, address + 1 "
      "(option: also into the selected CRTC register); IN = selected CRTC register",
      IO_PORT_DIR_RW, NULL, 0, kp80_dev_cat_reg, NULL, MZ_AVAIL_700, IO_CROSS_NONE },
    { 0x0072, "Device - K&P 80 CRTC status / address (R/W)",
      "K&P 80 column card, SY6845E: IN = status, OUT = register select (5 bits)",
      IO_PORT_DIR_RW, s_bits_72, sizeof(s_bits_72) / sizeof(s_bits_72[0]),
      kp80_dev_cat_status, NULL, MZ_AVAIL_700, IO_CROSS_NONE },
    { 0x0073, "Device - K&P 80 CRTC register (R/W)",
      "K&P 80 column card, SY6845E: selected register (R14 - R17 readable, others read 0, "
      "R31 = update strobe)",
      IO_PORT_DIR_RW, NULL, 0, kp80_dev_cat_reg, NULL, MZ_AVAIL_700, IO_CROSS_NONE },
};


/* ========================================================================= */
/*                                Snapshot                                   */
/* ========================================================================= */

/** @brief Jméno binárního souboru stavu v `devices/devbus/kp80/`. */
#define KP80_SNAP_FILE "kp80.bin"

/**
 * @brief Uloží stav karty (formát jádra, devdoc/kp80.md oddíl Uložený stav).
 * @param ctx Zařízení.
 * @param s   Kontext snapshotu devbus.
 * @return SNAPSHOT_OK nebo chyba zápisu.
 * @note Volby ukládá devbus jménem; výstup (obraz) se neukládá.
 */
static en_SNAPSHOT_RESULT kp80_dev_save(void *ctx, st_DEVBUS_SNAP *s)
{
    st_KP80_DEV *d = ctx;
    uint8_t buf[KP80_STATE_SIZE];
    if (kp80_state_save(&d->kp, buf, sizeof(buf)) != KP80_STATE_SIZE) return SNAPSHOT_ERR_COMPONENT;
    return devbus_snap_write_bin(s, KP80_SNAP_FILE, buf, sizeof(buf));
}

/**
 * @brief Načte stav karty.
 *
 * Verze 1 = stav jádra verze 1 (KP80_STATE_SIZE B). Neplatný stav (hlavička,
 * verze, velikost) se odmítne a stav karty se nezmění.
 *
 * @param ctx     Zařízení.
 * @param s       Kontext snapshotu devbus.
 * @param version Verze stavu zařízení ze snapshotu (1).
 * @return SNAPSHOT_OK, chyba čtení, nebo SNAPSHOT_ERR_CORRUPTED.
 */
static en_SNAPSHOT_RESULT kp80_dev_load(void *ctx, st_DEVBUS_SNAP *s, uint32_t version)
{
    st_KP80_DEV *d = ctx;
    (void) version;
    uint8_t buf[KP80_STATE_SIZE];
    en_SNAPSHOT_RESULT r = devbus_snap_read_bin_into(s, KP80_SNAP_FILE, buf, sizeof(buf));
    if (r != SNAPSHOT_OK) return r;
    if (!kp80_state_load(&d->kp, buf, sizeof(buf))) return SNAPSHOT_ERR_CORRUPTED;
    d->dirty = true;
    return SNAPSHOT_OK;
}


/* ========================================================================= */
/*                            Životní cyklus                                 */
/* ========================================================================= */

/**
 * @brief Registrace: výchozí stav jádra a výstup.
 * @param ctx Zařízení.
 * @return 0 = OK, -1 = výstup nejde vytvořit.
 */
static int kp80_dev_init(void *ctx)
{
    st_KP80_DEV *d = ctx;
    d->slot = devbus_find(KP80_DEVBUS_ID);
    kp80_init(&d->kp, 0);
    d->wait = 0;
    d->phosphor = KP80_PHOSPHOR_WHITE;
    d->dirty = true;
    d->vout = devbus_vout_create(d->slot, &s_vout_desc);
    return d->vout ? 0 : -1;
}

/**
 * @brief Zapnutí karty: power-on jádra (registry 0, VRAM 00h), volby, obraz.
 * @param ctx Zařízení.
 * @return 0.
 * @post Výstup obsahuje prázdný snímek (R6 = 0) i v pauze.
 */
static int kp80_dev_connect(void *ctx)
{
    st_KP80_DEV *d = ctx;
    uint64_t now = kp80_dev_now();
    kp80_dev_apply_all_options(d);
    kp80_power_on(&d->kp, now);
    kp80_dev_render(d, now);
    return 0;
}

/**
 * @brief Signál RESET MZ-700: restart čítačů CRTC, registry a VRAM zůstávají.
 * @param ctx Zařízení.
 */
static void kp80_dev_reset(void *ctx)
{
    st_KP80_DEV *d = ctx;
    kp80_reset(&d->kp, kp80_dev_now());
    kp80_dev_touch(d);
}

/**
 * @brief Změna volby za běhu.
 * @param ctx   Zařízení.
 * @param opt   Index volby.
 * @param value Nová hodnota.
 */
static void kp80_dev_option_changed(void *ctx, unsigned opt, unsigned value)
{
    st_KP80_DEV *d = ctx;
    kp80_dev_apply_option(d, opt, value);
    if (opt == KP80_OPT_PHOSPHOR) kp80_dev_touch(d);
}

/**
 * @brief Konec snímku MZ-700: nakreslí druhý monitor, pokud se obraz změnil.
 *
 * Obraz závisí na stavu karty a na fázi blikání kurzoru; bez změny se
 * nekreslí ani nepředává (čtenář výstupu podrží poslední snímek).
 *
 * @param ctx Zařízení.
 */
static void kp80_dev_frame_done(void *ctx)
{
    st_KP80_DEV *d = ctx;
    uint64_t now = kp80_dev_now();
    if (d->dirty || kp80_cursor_visible(&d->kp, now) != d->last_cursor) kp80_dev_render(d, now);
}

/**
 * @brief Po načtení snapshotu: volby do jádra (devbus je nastavil bez
 *        option_changed()) a překreslení výstupu.
 * @param ctx Zařízení.
 */
static void kp80_dev_after_load(void *ctx)
{
    st_KP80_DEV *d = ctx;
    kp80_dev_apply_all_options(d);
    kp80_dev_render(d, kp80_dev_now());
}

/** @brief Životní cyklus karty. */
static const st_DEVBUS_LIFECYCLE s_ops = {
    kp80_dev_init,
    kp80_dev_connect,
    kp80_dev_reset,
    NULL,                   /* disconnect: výstup podrží poslední snímek */
    NULL,                   /* destroy: výstup uvolní devbus */
    kp80_dev_option_changed,
    kp80_dev_frame_done,
    kp80_dev_after_load,
    NULL,                   /* rom_changed: font karty na ROM MZ-700 nezávisí */
};


/* ========================================================================= */
/*                               Deskriptor                                  */
/* ========================================================================= */

/** @brief Deskriptor karty K&P 80 (obraz MZ-700 nemění: video = NULL). */
static const st_DEVBUS_DEVICE_DESC s_desc = {
    .id = KP80_DEVBUS_ID,
    .name = "K&P 80 Column Card",
    .ini_section = "KP80",
    .platforms = DEVBUS_PLATFORM_MZ700,
    .options = s_options,
    .option_count = KP80_OPT_COUNT,
    .ports = s_ports,
    .port_count = sizeof(s_ports) / sizeof(s_ports[0]),
    .mmio = NULL,
    .mmio_count = 0,
    .video = NULL,
    .regions = s_regions,
    .region_count = sizeof(s_regions) / sizeof(s_regions[0]),
    .io_catalog = s_catalog,
    .io_catalog_count = sizeof(s_catalog) / sizeof(s_catalog[0]),
    .snap = { 1, kp80_dev_save, kp80_dev_load },
    .ops = &s_ops,
};


void kp80_devbus_register(void)
{
    int slot = devbus_register(&s_desc, &s_kp80);
    if (slot < 0) {
        fprintf(stderr, "kp80: device registration failed (error %d)\n", slot);
        s_kp80.slot = -1;
        s_kp80.vout = NULL;
    }
}


st_DEVBUS_VOUT *kp80_devbus_vout(void)
{
    return (s_kp80.slot >= 0) ? s_kp80.vout : NULL;
}
