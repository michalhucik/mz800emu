/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file kp80_core.c
 * @brief Čisté jádro 80-Zeichen-Karte K&P: CRTC SY6845E, VRAM, časování, vykreslení, stav.
 *
 * Model (podrobně `devdoc/kp80.md`):
 *  - registry R0 - R19 maskované šířkou z datasheetu SY6545-1 (Figure 3);
 *    čitelné jsou jen R14 - R17 a status, ostatní vrací 0 (volba modelu);
 *  - transparentní adresování: R18/R19 jsou latch zapsané adresy, aktivní
 *    update adresa `upd_addr` se z nich nabije při přístupu k R31 (volba
 *    `addr_on_r31`), nebo hned při zápisu; zápis 71h jde do VRAM na
 *    `upd_addr` a adresu zvýší;
 *  - časování počítané z času `now` ve znakových taktech; stav časování se
 *    přepočítá jen při zápisu časovacích registrů a při resetu, jinak se
 *    pozice paprsku dopočítá čistou funkcí (čtení statusu stav nemění);
 *  - vykreslení snímku 640 x 250 z VRAM, fontu a registrů.
 *
 * Kód je napsán podle datasheetu SY6545-1, článku Weidenbrück (Anwendermagazin
 * 2/1985) a popisu BKK; kód jiných emulátorů ani firmwaru nepřebírá.
 */

#include <string.h>

#include "kp80_core.h"


/* ===== Konstanty modelu ===== */

/** @brief Maska 14bitové adresy CRTC (MA0 - MA13, R12/R13, R14/R15, R18/R19). */
#define KP80_ADDR_MASK 0x3FFFu

/** @brief Maska adresy VRAM (MA0 - MA10 do SRAM 6116). */
#define KP80_VRAM_MASK (KP80_VRAM_SIZE - 1u)

/** @brief Maska řádku znaku pro EPROM (RA0 - RA3; RA4 je v transparentním režimu update strobe). */
#define KP80_FONT_ROW_MASK 0x0Fu

/** @brief Příznak `upd_pending` v bajtu příznaků uloženého stavu. */
#define KP80_STATE_FLAG_PENDING 0x01u

/**
 * @brief Šířky registrů R0 - R19 podle datasheetu SY6545-1 (Figure 3).
 *
 * R16/R17 (light pen) mají masku 0: zápis CPU je neovlivní (jen pro čtení)
 * a karta světelné pero nemá, takže zůstávají 0.
 */
static const uint8_t s_reg_mask[KP80_NUM_REGS] = {
    0xFF, /* R0  Horizontal Total - 1 */
    0xFF, /* R1  Horizontal Displayed */
    0xFF, /* R2  Horizontal Sync Position */
    0xFF, /* R3  VSYNC, HSYNC Widths */
    0x7F, /* R4  Vertical Total - 1 */
    0x1F, /* R5  Vertical Total Adjust */
    0x7F, /* R6  Vertical Displayed */
    0x7F, /* R7  Vertical Sync Position */
    0xFF, /* R8  Mode Control */
    0x1F, /* R9  Scan Lines - 1 */
    0x7F, /* R10 Cursor Start + režim blikání (bity 6 - 5) */
    0x1F, /* R11 Cursor End */
    0x3F, /* R12 Display Start Address (H) */
    0xFF, /* R13 Display Start Address (L) */
    0x3F, /* R14 Cursor Position (H) */
    0xFF, /* R15 Cursor Position (L) */
    0x00, /* R16 Light Pen (H), jen pro čtení */
    0x00, /* R17 Light Pen (L), jen pro čtení */
    0x3F, /* R18 Update Address (H) */
    0xFF, /* R19 Update Address (L) */
};


/* ===== Časování ===== */

uint32_t kp80_line_period(const st_KP80 *kp)
{
    return (uint32_t) kp->reg[0] + 1u;
}


uint32_t kp80_frame_lines(const st_KP80 *kp)
{
    return ((uint32_t) kp->reg[4] + 1u) * ((uint32_t) kp->reg[9] + 1u) + (uint32_t) kp->reg[5];
}


uint32_t kp80_frame_period(const st_KP80 *kp)
{
    return kp80_line_period(kp) * kp80_frame_lines(kp);
}


/**
 * @brief Dopočítá pozici paprsku a počet snímků v čase @p now (bez změny stavu).
 *
 * Vychází z `tm_last`, `tm_phase`, `tm_fields` a aktuální periody snímku.
 * Čas menší než `tm_last` se bere jako `tm_last`.
 *
 * @param kp         Struktura karty.
 * @param now        Čas ve znakových taktech.
 * @param out_phase  [out] Pozice ve snímku (0 .. perioda - 1); smí být NULL.
 * @param out_fields [out] Počet celých snímků; smí být NULL.
 */
static void kp80_timing_at(const st_KP80 *kp, uint64_t now, uint32_t *out_phase, uint64_t *out_fields)
{
    uint64_t period = kp80_frame_period(kp);
    uint64_t delta = (now > kp->tm_last) ? (now - kp->tm_last) : 0;
    /* Rozklad delta = q * period + r, aby součet nepřetekl ani u obřího delta. */
    uint64_t pos = (uint64_t) kp->tm_phase + (delta % period);
    uint64_t fields = kp->tm_fields + (delta / period) + (pos / period);
    if (out_phase) {
        *out_phase = (uint32_t) (pos % period);
    }
    if (out_fields) {
        *out_fields = fields;
    }
}


/**
 * @brief Přepočítá stav časování k času @p now (před změnou časovacího registru).
 * @param kp  Struktura karty.
 * @param now Čas ve znakových taktech.
 * @post `tm_last` = max(tm_last, now), `tm_phase` a `tm_fields` odpovídají tomuto času.
 */
static void kp80_timing_sync(st_KP80 *kp, uint64_t now)
{
    uint32_t phase;
    uint64_t fields;
    kp80_timing_at(kp, now, &phase, &fields);
    if (now > kp->tm_last) {
        kp->tm_last = now;
    }
    kp->tm_phase = phase;
    kp->tm_fields = fields;
}


/**
 * @brief Restartuje čítače rozkladu (začátek snímku, počítadlo snímků 0).
 * @param kp  Struktura karty.
 * @param now Čas ve znakových taktech.
 */
static void kp80_timing_restart(st_KP80 *kp, uint64_t now)
{
    kp->tm_last = now;
    kp->tm_phase = 0;
    kp->tm_fields = 0;
}


bool kp80_vblank(const st_KP80 *kp, uint64_t now)
{
    uint32_t phase;
    kp80_timing_at(kp, now, &phase, NULL);
    uint32_t line = phase / kp80_line_period(kp);
    uint32_t displayed = (uint32_t) kp->reg[6] * ((uint32_t) kp->reg[9] + 1u);
    return line >= displayed;
}


uint64_t kp80_field_count(const st_KP80 *kp, uint64_t now)
{
    uint64_t fields;
    kp80_timing_at(kp, now, NULL, &fields);
    return fields;
}


bool kp80_cursor_visible(const st_KP80 *kp, uint64_t now)
{
    switch ((kp->reg[10] >> 5) & 0x03u) {
        case 0: /* bez blikání */
            return true;
        case 1: /* bez kurzoru */
            return false;
        case 2: /* 1/16 snímkové frekvence: 8 snímků svítí, 8 ne */
            return ((kp80_field_count(kp, now) >> 3) & 1u) == 0;
        default: /* 1/32: 16 snímků svítí, 16 ne */
            return ((kp80_field_count(kp, now) >> 4) & 1u) == 0;
    }
}


/* ===== Update cyklus a registry ===== */

/**
 * @brief Spustí update cyklus: status bit 7 = 0 po dobu `opt.update_delay`.
 * @param kp  Struktura karty.
 * @param now Čas ve znakových taktech.
 */
static void kp80_update_start(st_KP80 *kp, uint64_t now)
{
    kp->upd_ready_at = now + kp->opt.update_delay;
}


/**
 * @brief Přístup k R31 (čtení, zápis nebo výběr přes 72h): nabije čekající
 *        adresu z R18/R19 a spustí update cyklus.
 * @param kp  Struktura karty.
 * @param now Čas ve znakových taktech.
 */
static void kp80_r31_access(st_KP80 *kp, uint64_t now)
{
    if (kp->upd_pending) {
        kp->upd_addr = (uint16_t) ((((uint16_t) kp->reg[18] << 8) | kp->reg[19]) & KP80_ADDR_MASK);
        kp->upd_pending = false;
    }
    kp80_update_start(kp, now);
}


/**
 * @brief Zápis CPU do vybraného registru (73h, volbou i 71h).
 *
 * Časovací registry (R0, R4, R5, R9) nejdřív přepočítají stav časování k času
 * @p now a po změně se pozice paprsku zkrátí modulo nová perioda. R16/R17 a
 * R20 - R30 zápis ignorují, R31 je přístup k R31.
 *
 * @param kp  Struktura karty.
 * @param r   Číslo registru (0 - 31).
 * @param v   Zapisovaná hodnota.
 * @param now Čas ve znakových taktech.
 */
static void kp80_reg_write(st_KP80 *kp, unsigned r, uint8_t v, uint64_t now)
{
    if (r == KP80_REG_DUMMY) {
        kp80_r31_access(kp, now);
        return;
    }
    if (r >= KP80_NUM_REGS) {
        return; /* R20 - R30 na čipu nejsou */
    }

    bool timing = (r == 0 || r == 4 || r == 5 || r == 9);
    if (timing) {
        kp80_timing_sync(kp, now);
    }
    kp->reg[r] = (uint8_t) (v & s_reg_mask[r]);
    if (timing) {
        kp->tm_phase %= kp80_frame_period(kp);
    }

    if (r == 18 || r == 19) {
        if (kp->opt.addr_on_r31) {
            kp->upd_pending = true;
        } else if (r == 18) {
            kp->upd_addr = (uint16_t) (((uint16_t) kp->reg[18] << 8) | (kp->upd_addr & 0x00FFu));
        } else {
            kp->upd_addr = (uint16_t) ((kp->upd_addr & 0x3F00u) | kp->reg[19]);
        }
    }
}


/**
 * @brief Hodnota, kterou CPU přečte z vybraného registru (bez vedlejších účinků).
 * @param kp  Struktura karty.
 * @param r   Číslo registru (0 - 31).
 * @param bus Hodnota nebuzené sběrnice (vrací se pro R31).
 * @return R14 - R17 uložená hodnota, R31 @p bus, ostatní 0.
 */
static uint8_t kp80_reg_peek(const st_KP80 *kp, unsigned r, uint8_t bus)
{
    if (r == KP80_REG_DUMMY) {
        return bus;
    }
    if (r >= 14 && r <= 17) {
        return kp->reg[r];
    }
    return 0x00;
}


/* ===== Životní cyklus ===== */

void kp80_options_default(st_KP80_OPTIONS *opt)
{
    opt->read_autoinc = true;
    opt->data_write_to_reg = true;
    opt->addr_on_r31 = true;
    opt->update_delay = 0;
}


void kp80_power_on(st_KP80 *kp, uint64_t now)
{
    memset(kp->reg, 0, sizeof(kp->reg));
    memset(kp->vram, 0x00, sizeof(kp->vram));
    kp->ar = 0;
    kp->upd_pending = false;
    kp->upd_addr = 0;
    kp->upd_ready_at = now;
    kp80_timing_restart(kp, now);
}


void kp80_init(st_KP80 *kp, uint64_t now)
{
    memset(kp, 0, sizeof(*kp));
    kp80_options_default(&kp->opt);
    kp->font = g_kp80_font_rom;
    kp80_power_on(kp, now);
}


void kp80_reset(st_KP80 *kp, uint64_t now)
{
    kp80_timing_restart(kp, now);
    if (kp->upd_ready_at > now) {
        kp->upd_ready_at = now;
    }
}


void kp80_set_font(st_KP80 *kp, const uint8_t *font)
{
    kp->font = font ? font : g_kp80_font_rom;
}


/* ===== Porty ===== */

uint8_t kp80_status(const st_KP80 *kp, uint64_t now)
{
    uint8_t st = 0;
    if (now >= kp->upd_ready_at) {
        st |= KP80_STATUS_UPDATE_READY;
    }
    if (kp80_vblank(kp, now)) {
        st |= KP80_STATUS_VBLANK;
    }
    return st;
}


uint8_t kp80_peek(const st_KP80 *kp, unsigned port, uint64_t now, uint8_t bus)
{
    switch (port & 0x03u) {
        case KP80_PORT_VRAM:
            return kp->vram[kp->upd_addr & KP80_VRAM_MASK];
        case KP80_PORT_ADDR:
            return kp80_status(kp, now);
        default: /* KP80_PORT_DATA, KP80_PORT_REG */
            return kp80_reg_peek(kp, kp->ar, bus);
    }
}


uint8_t kp80_read(st_KP80 *kp, unsigned port, uint64_t now, uint8_t bus)
{
    uint8_t v = kp80_peek(kp, port, now, bus);
    switch (port & 0x03u) {
        case KP80_PORT_VRAM:
            if (kp->opt.read_autoinc) {
                kp->upd_addr = (uint16_t) ((kp->upd_addr + 1u) & KP80_ADDR_MASK);
            }
            break;
        case KP80_PORT_DATA:
        case KP80_PORT_REG:
            if (kp->ar == KP80_REG_DUMMY) {
                kp80_r31_access(kp, now);
            }
            break;
        default: /* status nemá vedlejší účinky (LPEN full je vždy 0) */
            break;
    }
    return v;
}


void kp80_write(st_KP80 *kp, unsigned port, uint8_t value, uint64_t now)
{
    switch (port & 0x03u) {
        case KP80_PORT_VRAM:
            break; /* 70h je jen pro čtení */
        case KP80_PORT_DATA:
            if (kp->opt.data_write_to_reg) {
                kp80_reg_write(kp, kp->ar, value, now);
            }
            kp->vram[kp->upd_addr & KP80_VRAM_MASK] = value;
            kp->upd_addr = (uint16_t) ((kp->upd_addr + 1u) & KP80_ADDR_MASK);
            kp80_update_start(kp, now);
            break;
        case KP80_PORT_ADDR:
            kp->ar = (uint8_t) (value & 0x1Fu);
            if (kp->ar == KP80_REG_DUMMY) {
                kp80_r31_access(kp, now);
            }
            break;
        default: /* KP80_PORT_REG */
            kp80_reg_write(kp, kp->ar, value, now);
            break;
    }
}


bool kp80_port_has_wait(unsigned port)
{
    return (port & 0x03u) != KP80_PORT_VRAM;
}


/* ===== Vykreslení ===== */

void kp80_render(const st_KP80 *kp, uint64_t now, uint8_t *fb)
{
    memset(fb, 0, KP80_FB_SIZE);

    const uint8_t *font = kp->font ? kp->font : g_kp80_font_rom;
    const unsigned cols = kp->reg[1];
    const unsigned rows = kp->reg[6];
    const unsigned scan = (unsigned) kp->reg[9] + 1u;
    const uint16_t start = (uint16_t) ((((uint16_t) kp->reg[12] << 8) | kp->reg[13]) & KP80_ADDR_MASK);
    const uint16_t cursor = (uint16_t) ((((uint16_t) kp->reg[14] << 8) | kp->reg[15]) & KP80_ADDR_MASK);
    const unsigned cur_start = kp->reg[10] & 0x1Fu;
    const unsigned cur_end = kp->reg[11] & 0x1Fu;
    const bool cur_on = kp80_cursor_visible(kp, now);

    unsigned vis_cols = cols;
    if (vis_cols > KP80_FB_WIDTH / KP80_CHAR_WIDTH) {
        vis_cols = KP80_FB_WIDTH / KP80_CHAR_WIDTH;
    }

    for (unsigned row = 0; row < rows; row++) {
        unsigned y0 = row * scan;
        if (y0 >= KP80_FB_HEIGHT) {
            break;
        }
        uint16_t row_ma = (uint16_t) ((start + row * cols) & KP80_ADDR_MASK);
        for (unsigned ra = 0; ra < scan && y0 + ra < KP80_FB_HEIGHT; ra++) {
            uint8_t *line = fb + (size_t) (y0 + ra) * KP80_FB_WIDTH;
            bool cur_row = cur_on && cur_start <= cur_end && ra >= cur_start && ra <= cur_end;
            for (unsigned col = 0; col < vis_cols; col++) {
                uint16_t ma = (uint16_t) ((row_ma + col) & KP80_ADDR_MASK);
                uint8_t code = kp->vram[ma & KP80_VRAM_MASK];
                uint8_t bits = font[(unsigned) (code & 0x7Fu) * KP80_FONT_CHAR_BYTES + (ra & KP80_FONT_ROW_MASK)];
                bool inv = (code & 0x80u) != 0;
                if (cur_row && ma == cursor) {
                    inv = !inv;
                }
                if (inv) {
                    bits = (uint8_t) ~bits;
                }
                uint8_t *p = line + col * KP80_CHAR_WIDTH;
                for (unsigned x = 0; x < KP80_CHAR_WIDTH; x++) {
                    p[x] = (uint8_t) ((bits >> (7u - x)) & 1u);
                }
            }
        }
    }
}


/* ===== Uložení stavu ===== */

/** @brief Zapíše u16 little-endian a posune ukazatel. */
static uint8_t *put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    return p + 2;
}

/** @brief Zapíše u32 little-endian a posune ukazatel. */
static uint8_t *put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
    return p + 4;
}

/** @brief Zapíše u64 little-endian a posune ukazatel. */
static uint8_t *put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
    return p + 8;
}

/** @brief Přečte u16 little-endian. */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

/** @brief Přečte u32 little-endian. */
static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) {
        v |= (uint32_t) p[i] << (8 * i);
    }
    return v;
}

/** @brief Přečte u64 little-endian. */
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
        v |= (uint64_t) p[i] << (8 * i);
    }
    return v;
}


size_t kp80_state_save(const st_KP80 *kp, uint8_t *buf, size_t size)
{
    if (!buf || size < KP80_STATE_SIZE) {
        return 0;
    }
    uint8_t *p = buf;
    memcpy(p, "KP80", 4);
    p += 4;
    p = put_u16(p, (uint16_t) KP80_STATE_VERSION);
    p = put_u16(p, (uint16_t) KP80_STATE_SIZE);
    memcpy(p, kp->reg, KP80_NUM_REGS);
    p += KP80_NUM_REGS;
    *p++ = kp->ar;
    *p++ = kp->upd_pending ? KP80_STATE_FLAG_PENDING : 0u;
    p = put_u16(p, kp->upd_addr);
    p = put_u64(p, kp->upd_ready_at);
    p = put_u64(p, kp->tm_last);
    p = put_u32(p, kp->tm_phase);
    p = put_u64(p, kp->tm_fields);
    memcpy(p, kp->vram, KP80_VRAM_SIZE);
    p += KP80_VRAM_SIZE;
    return (size_t) (p - buf);
}


bool kp80_state_load(st_KP80 *kp, const uint8_t *buf, size_t size)
{
    if (!buf || size < KP80_STATE_SIZE || memcmp(buf, "KP80", 4) != 0) {
        return false;
    }
    uint16_t version = get_u16(buf + 4);
    if (version < 1u || version > KP80_STATE_VERSION || get_u16(buf + 6) != KP80_STATE_SIZE) {
        return false;
    }

    /* Data jsou platná; načíst a vynutit invarianty. */
    const uint8_t *p = buf + 8;
    for (unsigned r = 0; r < KP80_NUM_REGS; r++) {
        kp->reg[r] = (uint8_t) (p[r] & s_reg_mask[r]);
    }
    p += KP80_NUM_REGS;
    kp->ar = (uint8_t) (*p++ & 0x1Fu);
    kp->upd_pending = (*p++ & KP80_STATE_FLAG_PENDING) != 0;
    kp->upd_addr = (uint16_t) (get_u16(p) & KP80_ADDR_MASK);
    p += 2;
    kp->upd_ready_at = get_u64(p);
    p += 8;
    kp->tm_last = get_u64(p);
    p += 8;
    kp->tm_phase = get_u32(p) % kp80_frame_period(kp);
    p += 4;
    kp->tm_fields = get_u64(p);
    p += 8;
    memcpy(kp->vram, p, KP80_VRAM_SIZE);
    return true;
}
