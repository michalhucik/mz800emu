/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: MIT
 * https://github.com/michalhucik/z80-mz800
 */
/**
 * @file z80_dasm_format.c
 * @brief Formatovaci modul Z80 disassembleru - prevod struktury na text.
 * @version 0.1
 *
 * Prevadi dekodovanou strukturu z80_dasm_inst_t na lidsky citelny text
 * s konfigurovatelnym formatovanim (styl hex cisel, velka/mala pismena,
 * zobrazeni surovych bajtu, adres, atd.).
 *
 * Modul nepouziva globalni stav ani staticke buffery - vystup je vzdy
 * zapisovan do uzivatelem poskytnuteho bufferu. Funkce jsou thread-safe.
 */

#include <string.h>
#include <stdio.h>
#include "z80_dasm_internal.h"

/* ======================================================================
 * Interni: tabulky nazvu pro formatovani
 * ====================================================================== */

/**
 * @brief Nazvy 8bitovych registru indexovane hodnotou z80_reg8_t.
 *
 * Obsahuje nazvy ve velkych pismenech - prevod na mala pismena
 * se provadi az pri vystupu podle nastaveni formatu.
 */
static const char *const reg8_names[] = {
    "B", "C", "D", "E", "H", "L", "A", "F",
    "IXH", "IXL", "IYH", "IYL", "I", "R"
};

/**
 * @brief Alternativni nazvy nedokumentovanych pul-registru IX/IY (styl 1: HX/LX).
 *
 * Pouziva se kdyz undoc_ix_style == 1 ve formatovaci konfiguraci.
 */
static const char *const reg8_names_hx[] = {
    "B", "C", "D", "E", "H", "L", "A", "F",
    "HX", "LX", "HY", "LY", "I", "R"
};

/**
 * @brief Alternativni nazvy nedokumentovanych pul-registru IX/IY (styl 2: XH/XL).
 *
 * Pouziva se kdyz undoc_ix_style == 2 ve formatovaci konfiguraci.
 */
static const char *const reg8_names_xh[] = {
    "B", "C", "D", "E", "H", "L", "A", "F",
    "XH", "XL", "YH", "YL", "I", "R"
};

/**
 * @brief Nazvy 16bitovych registru indexovane hodnotou z80_reg16_t.
 */
static const char *const reg16_names[] = {
    "BC", "DE", "HL", "SP", "AF", "IX", "IY"
};

/**
 * @brief Nazvy podminek vetveni indexovane hodnotou z80_condition_t.
 */
static const char *const condition_names[] = {
    "NZ", "Z", "NC", "C", "PO", "PE", "P", "M"
};

/* ======================================================================
 * Interni: pomocne funkce pro zapis do bufferu
 * ====================================================================== */

/**
 * @brief Kontext pro bezpecny zapis do bufferu s omezenou velikosti.
 *
 * Sleduje aktualni pozici a zbyvajici misto v bufferu.
 * Vsechny funkce buf_* zapisuji pres tento kontext a automaticky
 * hlídaji preteceni.
 *
 * @invariant pos <= size
 * @invariant buf[pos] == '\0' po kazdem zapisu (pokud size > 0)
 */
typedef struct {
    char *buf;   /**< Ukazatel na zacatek vystupniho bufferu. */
    int   size;  /**< Celkova velikost bufferu v bajtech. */
    int   pos;   /**< Aktualni pozice zapisu (pocet zapsanych znaku). */
} buf_ctx_t;

/**
 * @brief Zapise jeden znak do bufferu.
 *
 * Pokud v bufferu neni misto, znak se ticha zahodí a pozice se inkrementuje
 * pro spravne sledovani celkove delky vystupu.
 *
 * @param[in,out] ctx Kontext bufferu. Nesmi byt NULL.
 * @param[in]     ch  Znak k zapisu.
 *
 * @pre ctx != NULL
 * @post ctx->pos je inkrementovano o 1.
 * @post Pokud bylo misto, buf[pos-1] == ch a buf[pos] == '\0'.
 */
static void buf_putc(buf_ctx_t *ctx, char ch)
{
    if (ctx->pos < ctx->size - 1) {
        ctx->buf[ctx->pos] = ch;
        ctx->buf[ctx->pos + 1] = '\0';
    }
    ctx->pos++;
}

/**
 * @brief Zapise retezec do bufferu.
 *
 * Zapisuje znak po znaku pres buf_putc(). Pokud buffer nestaci,
 * prebytecne znaky se zahodi ale pozice se spravne aktualizuje.
 *
 * @param[in,out] ctx Kontext bufferu. Nesmi byt NULL.
 * @param[in]     s   Retezec k zapisu. Nesmi byt NULL.
 *
 * @pre ctx != NULL
 * @pre s != NULL
 */
static void buf_puts(buf_ctx_t *ctx, const char *s)
{
    while (*s) {
        buf_putc(ctx, *s++);
    }
}

/**
 * @brief Zapise retezec do bufferu s prevodem na mala pismena.
 *
 * Znaky A-Z prevede na a-z, ostatni znaky zapise beze zmeny.
 *
 * @param[in,out] ctx Kontext bufferu. Nesmi byt NULL.
 * @param[in]     s   Retezec k zapisu. Nesmi byt NULL.
 *
 * @pre ctx != NULL
 * @pre s != NULL
 */
static void buf_puts_lower(buf_ctx_t *ctx, const char *s)
{
    while (*s) {
        char ch = *s++;
        if (ch >= 'A' && ch <= 'Z') {
            ch = ch + ('a' - 'A');
        }
        buf_putc(ctx, ch);
    }
}

/* ======================================================================
 * Interni: formatovani hexadecimalnich cisel
 * ====================================================================== */

/**
 * @brief Prevede nibble (0-15) na hexadecimalni znak.
 *
 * @param[in] nibble Hodnota 0-15.
 * @param[in] upper  Nenulova hodnota pro velke pismena (A-F), 0 pro mala (a-f).
 * @return Hexadecimalni znak ('0'-'9', 'A'-'F' nebo 'a'-'f').
 *
 * @pre nibble <= 15
 */
static char hex_digit(int nibble, int upper)
{
    if (nibble < 10) return '0' + nibble;
    return (upper ? 'A' : 'a') + nibble - 10;
}

/**
 * @brief Zapise 8bitovou hodnotu jako hexadecimalni cislo podle zvoleneho stylu.
 *
 * Formatuje bajt do bufferu s prislusnym prefixem/suffixem podle hex_style:
 * - Z80_HEX_HASH:     "#FF" nebo "#ff"
 * - Z80_HEX_0X:       "0xFF" nebo "0xff"
 * - Z80_HEX_DOLLAR:   "$FF" nebo "$ff"
 * - Z80_HEX_H_SUFFIX: "FFh" nebo "ffh"
 *
 * @param[in,out] ctx       Kontext bufferu. Nesmi byt NULL.
 * @param[in]     val       8bitova hodnota k formatovani.
 * @param[in]     hex_style Styl hexadecimalnich cisel.
 * @param[in]     upper     Velka pismena pro hex cifry (A-F vs a-f).
 *
 * @pre ctx != NULL
 */
static void fmt_hex8(buf_ctx_t *ctx, uint8_t val,
                     z80_hex_style_t hex_style, int upper)
{
    switch (hex_style) {
    case Z80_HEX_HASH:
        buf_putc(ctx, '#');
        break;
    case Z80_HEX_0X:
        buf_putc(ctx, '0');
        buf_putc(ctx, upper ? 'X' : 'x');
        break;
    case Z80_HEX_DOLLAR:
        buf_putc(ctx, '$');
        break;
    case Z80_HEX_H_SUFFIX:
        /* Prefix az za cislicemi; pokud hodnota zacina A-F, pridame '0' */
        if (val >= 0xA0) {
            buf_putc(ctx, '0');
        }
        break;
    }

    buf_putc(ctx, hex_digit((val >> 4) & 0x0F, upper));
    buf_putc(ctx, hex_digit(val & 0x0F, upper));

    if (hex_style == Z80_HEX_H_SUFFIX) {
        buf_putc(ctx, upper ? 'H' : 'h');
    }
}

/**
 * @brief Zapise 16bitovou hodnotu jako hexadecimalni cislo podle zvoleneho stylu.
 *
 * Formatuje 16bitove slovo do bufferu s prislusnym prefixem/suffixem.
 * Format je analogicky k fmt_hex8(), ale se 4 hexadecimalnimi ciframi.
 *
 * @param[in,out] ctx       Kontext bufferu. Nesmi byt NULL.
 * @param[in]     val       16bitova hodnota k formatovani.
 * @param[in]     hex_style Styl hexadecimalnich cisel.
 * @param[in]     upper     Velka pismena pro hex cifry.
 *
 * @pre ctx != NULL
 */
static void fmt_hex16(buf_ctx_t *ctx, uint16_t val,
                      z80_hex_style_t hex_style, int upper)
{
    switch (hex_style) {
    case Z80_HEX_HASH:
        buf_putc(ctx, '#');
        break;
    case Z80_HEX_0X:
        buf_putc(ctx, '0');
        buf_putc(ctx, upper ? 'X' : 'x');
        break;
    case Z80_HEX_DOLLAR:
        buf_putc(ctx, '$');
        break;
    case Z80_HEX_H_SUFFIX:
        /* Pokud horni nibble je A-F, pridame '0' pred cislo */
        if ((val >> 12) >= 0x0A) {
            buf_putc(ctx, '0');
        }
        break;
    }

    buf_putc(ctx, hex_digit((val >> 12) & 0x0F, upper));
    buf_putc(ctx, hex_digit((val >> 8) & 0x0F, upper));
    buf_putc(ctx, hex_digit((val >> 4) & 0x0F, upper));
    buf_putc(ctx, hex_digit(val & 0x0F, upper));

    if (hex_style == Z80_HEX_H_SUFFIX) {
        buf_putc(ctx, upper ? 'H' : 'h');
    }
}

/* ======================================================================
 * Interni: formatovani surovych bajtu a adresy
 * ====================================================================== */

/**
 * @brief Zapise adresu instrukce jako 4ciferny hex s oddelovacem.
 *
 * Format: "XXXX  " (adresa + 2 mezery). Hex cifry jsou vzdy velke
 * (adresa nema hex_style prefix, je to vzdy plain hex).
 *
 * @param[in,out] ctx  Kontext bufferu. Nesmi byt NULL.
 * @param[in]     addr 16bitova adresa.
 *
 * @pre ctx != NULL
 */
static void fmt_addr_prefix(buf_ctx_t *ctx, uint16_t addr)
{
    buf_putc(ctx, hex_digit((addr >> 12) & 0x0F, 1));
    buf_putc(ctx, hex_digit((addr >> 8) & 0x0F, 1));
    buf_putc(ctx, hex_digit((addr >> 4) & 0x0F, 1));
    buf_putc(ctx, hex_digit(addr & 0x0F, 1));
    buf_putc(ctx, ' ');
    buf_putc(ctx, ' ');
}

/**
 * @brief Zapise surove bajty instrukce s paddingem na fixni sirku.
 *
 * Bajty se vypisuji jako dvojice hexadecimalnich cislic oddelene mezerou.
 * Celkovy vystup je paddovan mezerami na 12 znaku (max 4 bajty = "XX XX XX XX"
 * = 11 znaku + 1 mezera oddelovac = 12).
 *
 * Hex cifry surovych bajtu jsou vzdy velke (nezavisle na nastaveni uppercase).
 *
 * @param[in,out] ctx    Kontext bufferu. Nesmi byt NULL.
 * @param[in]     bytes  Pole bajtu instrukce.
 * @param[in]     length Pocet bajtu (1-4).
 *
 * @pre ctx != NULL
 * @pre length >= 1 && length <= 4
 */
static void fmt_raw_bytes(buf_ctx_t *ctx, const uint8_t *bytes, int length)
{
    int written = 0;

    for (int i = 0; i < length; i++) {
        if (i > 0) {
            buf_putc(ctx, ' ');
            written++;
        }
        buf_putc(ctx, hex_digit((bytes[i] >> 4) & 0x0F, 1));
        buf_putc(ctx, hex_digit(bytes[i] & 0x0F, 1));
        written += 2;
    }

    /* Padding na 12 znaku (4 bajty: "XX XX XX XX" = 11 + 1 oddelovac) */
    while (written < 12) {
        buf_putc(ctx, ' ');
        written++;
    }
}

/* ======================================================================
 * Interni: formatovani jednoho operandu
 * ====================================================================== */

/**
 * @brief Vybere tabulku nazvu 8bitovych registru podle stylu pul-registru.
 *
 * @param[in] undoc_ix_style Styl nedokumentovanych IX/IY pul-registru (0, 1 nebo 2).
 * @return Ukazatel na tabulku nazvu registru.
 *
 * @post Vraceny ukazatel je vzdy platny (nikdy NULL).
 */
static const char *const *get_reg8_table(int undoc_ix_style)
{
    switch (undoc_ix_style) {
    case 1:  return reg8_names_hx;
    case 2:  return reg8_names_xh;
    default: return reg8_names;
    }
}

/**
 * @brief Zapise retezec do bufferu s respektovanim nastaveni uppercase.
 *
 * Pokud je uppercase nenulove, zapise retezec jak je (predpoklada velka pismena
 * ve zdrojovych tabulkach). Pokud je nulove, prevede na mala pismena.
 *
 * @param[in,out] ctx       Kontext bufferu. Nesmi byt NULL.
 * @param[in]     s         Retezec k zapisu. Nesmi byt NULL.
 * @param[in]     uppercase Rezim velkych/malych pismen.
 *
 * @pre ctx != NULL
 * @pre s != NULL
 */
static void buf_puts_case(buf_ctx_t *ctx, const char *s, int uppercase)
{
    if (uppercase) {
        buf_puts(ctx, s);
    } else {
        buf_puts_lower(ctx, s);
    }
}

/**
 * @brief Zapise displacement pro indexovane adresovani (IX+d / IX-d).
 *
 * Formatuje displacement jako "+dd" nebo "-dd" kde dd je hexadecimalni
 * hodnota absolutni hodnoty displacementu.
 *
 * @param[in,out] ctx       Kontext bufferu. Nesmi byt NULL.
 * @param[in]     disp      Znamenkovy displacement (-128 az +127).
 * @param[in]     hex_style Styl hexadecimalnich cisel.
 * @param[in]     upper     Velka pismena pro hex cifry.
 *
 * @pre ctx != NULL
 */
static void fmt_displacement(buf_ctx_t *ctx, int8_t disp,
                             z80_hex_style_t hex_style, int upper)
{
    if (disp >= 0) {
        buf_putc(ctx, '+');
        fmt_hex8(ctx, (uint8_t)disp, hex_style, upper);
    } else {
        buf_putc(ctx, '-');
        fmt_hex8(ctx, (uint8_t)(-disp), hex_style, upper);
    }
}

/**
 * @brief Zapíše znaménkový displacement jako dekadické číslo pro Motorola styl.
 *
 * Pomocná funkce pro renderování indexované adresace ve stylu sdas/sdcc:
 * formát "(d,IX)" / "(-d,IY)". Sdas-z80 přijímá hexadecimální i dekadický
 * zápis displacementu, ale dekadická forma je čitelnější a pro typické
 * displacement v rozsahu -128..+127 jednoznačná. Záporné hodnoty mají
 * leading mínus, kladné a nula jsou bez znaménka.
 *
 * Příklady: 5 -> "5", -2 -> "-2", 0 -> "0", -128 -> "-128", 127 -> "127".
 *
 * @param[in,out] ctx  Kontext bufferu. Nesmí být NULL.
 * @param[in]     disp Znaménkový displacement (-128 až +127).
 *
 * @pre ctx != NULL
 */
static void fmt_displacement_dec_signed(buf_ctx_t *ctx, int8_t disp)
{
    /* Rozsah int8_t je -128..+127. snprintf produkuje max 4 znaky ("-128"). */
    char tmp[8];
    snprintf(tmp, sizeof tmp, "%d", (int)disp);
    buf_puts(ctx, tmp);
}

/**
 * @brief Zapise jeden operand instrukce do bufferu.
 *
 * Formatuje operand podle jeho typu s respektovanim formatovaci konfigurace.
 * Podporuje vsechny typy operandu definovane v z80_operand_type_t.
 *
 * @param[in,out] ctx  Kontext bufferu. Nesmi byt NULL.
 * @param[in]     op   Operand k formatovani. Nesmi byt NULL.
 * @param[in]     inst Dekodovana instrukce (potrebna pro vypocet absolutni adresy
 *                     u relativnich skoku). Nesmi byt NULL.
 * @param[in]     fmt  Formatovaci konfigurace. Nesmi byt NULL.
 *
 * @pre ctx != NULL
 * @pre op != NULL
 * @pre inst != NULL
 * @pre fmt != NULL
 */
static void fmt_operand(buf_ctx_t *ctx, const z80_operand_t *op,
                        const z80_dasm_inst_t *inst,
                        const z80_dasm_format_t *fmt)
{
    const char *const *r8tab = get_reg8_table(fmt->undoc_ix_style);

    switch (op->type) {
    case Z80_OP_NONE:
        /* Zadny operand - nic nezapisujeme */
        break;

    case Z80_OP_REG8:
        buf_puts_case(ctx, r8tab[op->val.reg8], fmt->uppercase);
        break;

    case Z80_OP_REG16:
        buf_puts_case(ctx, reg16_names[op->val.reg16], fmt->uppercase);
        /* AF' - specialni pripad: detekujeme podle kontextu.
           EX AF,AF' je jedina instrukce s dvema AF operandy. Druhy je AF'. */
        break;

    case Z80_OP_IMM8:
        fmt_hex8(ctx, op->val.imm8, fmt->hex_style, fmt->uppercase);
        break;

    case Z80_OP_IMM16:
        fmt_hex16(ctx, op->val.imm16, fmt->hex_style, fmt->uppercase);
        break;

    case Z80_OP_MEM_REG16:
        buf_putc(ctx, '(');
        /* IN r,(C) / OUT (C),r - zobrazime jako "(C)" ne "(BC)" */
        if (op->val.reg16 == Z80_R16_BC &&
            inst->mnemonic[0] != '\0' &&
            (strcmp(inst->mnemonic, "IN") == 0 ||
             strcmp(inst->mnemonic, "OUT") == 0)) {
            buf_puts_case(ctx, "C", fmt->uppercase);
        } else {
            buf_puts_case(ctx, reg16_names[op->val.reg16], fmt->uppercase);
        }
        buf_putc(ctx, ')');
        break;

    case Z80_OP_MEM_IX_D:
        buf_putc(ctx, '(');
        if (fmt->ix_iy_style == Z80_IX_MOTOROLA) {
            /* Motorola/sdas styl: "(d,IX)" - signed decimal displacement
             * první, oddělený čárkou. Sdas-z80 ho takto vyžaduje. */
            fmt_displacement_dec_signed(ctx, op->val.displacement);
            buf_putc(ctx, ',');
            buf_puts_case(ctx, "IX", fmt->uppercase);
        } else {
            /* Zilog styl: "(IX+d)" / "(IX-d)" - mainstream. */
            buf_puts_case(ctx, "IX", fmt->uppercase);
            fmt_displacement(ctx, op->val.displacement, fmt->hex_style,
                             fmt->uppercase);
        }
        buf_putc(ctx, ')');
        break;

    case Z80_OP_MEM_IY_D:
        buf_putc(ctx, '(');
        if (fmt->ix_iy_style == Z80_IX_MOTOROLA) {
            fmt_displacement_dec_signed(ctx, op->val.displacement);
            buf_putc(ctx, ',');
            buf_puts_case(ctx, "IY", fmt->uppercase);
        } else {
            buf_puts_case(ctx, "IY", fmt->uppercase);
            fmt_displacement(ctx, op->val.displacement, fmt->hex_style,
                             fmt->uppercase);
        }
        buf_putc(ctx, ')');
        break;

    case Z80_OP_MEM_IMM16:
        buf_putc(ctx, '(');
        fmt_hex16(ctx, op->val.imm16, fmt->hex_style, fmt->uppercase);
        buf_putc(ctx, ')');
        break;

    case Z80_OP_MEM_IMM8:
        buf_putc(ctx, '(');
        fmt_hex8(ctx, op->val.imm8, fmt->hex_style, fmt->uppercase);
        buf_putc(ctx, ')');
        break;

    case Z80_OP_CONDITION:
        buf_puts_case(ctx, condition_names[op->val.condition], fmt->uppercase);
        break;

    case Z80_OP_BIT_INDEX:
        buf_putc(ctx, '0' + op->val.bit_index);
        break;

    case Z80_OP_REL8:
        if (fmt->rel_as_absolute) {
            /* Zobrazit cilovou adresu misto offsetu */
            uint16_t target = (uint16_t)(inst->addr + inst->length +
                               op->val.displacement);
            fmt_hex16(ctx, target, fmt->hex_style, fmt->uppercase);
        } else {
            /* Zobrazit jako relativni offset "$+n" nebo "$-n" */
            int offset = (int)inst->length + (int)op->val.displacement;
            buf_putc(ctx, '$');
            if (offset >= 0) {
                buf_putc(ctx, '+');
                fmt_hex8(ctx, (uint8_t)offset, fmt->hex_style, fmt->uppercase);
            } else {
                buf_putc(ctx, '-');
                fmt_hex8(ctx, (uint8_t)(-offset), fmt->hex_style, fmt->uppercase);
            }
        }
        break;

    case Z80_OP_RST_VEC:
        fmt_hex8(ctx, op->val.imm8, fmt->hex_style, fmt->uppercase);
        break;
    }
}

/* ======================================================================
 * Interni: detekce AF' pro EX AF,AF'
 * ====================================================================== */

/**
 * @brief Zjisti, zda instrukce je EX AF,AF'.
 *
 * EX AF,AF' je jedina instrukce, ktera pouziva AF' (AF s apostrofem).
 * Detekce na zaklade mnemoniky a obou operandu byt AF.
 *
 * @param[in] inst Dekodovana instrukce. Nesmi byt NULL.
 * @return Nenulova hodnota pokud instrukce je EX AF,AF', jinak 0.
 *
 * @pre inst != NULL
 */
static int is_ex_af_af(const z80_dasm_inst_t *inst)
{
    return inst->mnemonic[0] != '\0' &&
           strcmp(inst->mnemonic, "EX") == 0 &&
           inst->op1.type == Z80_OP_REG16 &&
           inst->op1.val.reg16 == Z80_R16_AF &&
           inst->op2.type == Z80_OP_REG16 &&
           inst->op2.val.reg16 == Z80_R16_AF;
}

/* ======================================================================
 * Verejne API: formatovani vystupu
 * ====================================================================== */

void z80_dasm_format_default(z80_dasm_format_t *fmt)
{
    fmt->hex_style = Z80_HEX_HASH;
    fmt->uppercase = 1;
    fmt->show_bytes = 0;
    fmt->show_addr = 0;
    fmt->rel_as_absolute = 1;
    fmt->undoc_ix_style = 0;
    fmt->ix_iy_style = Z80_IX_ZILOG;
}

int z80_dasm_to_str(char *buf, int buf_size,
                    const z80_dasm_inst_t *inst,
                    const z80_dasm_format_t *fmt)
{
    z80_dasm_format_t default_fmt;
    buf_ctx_t ctx;

    /* NULL format -> pouzijeme vychozi */
    if (!fmt) {
        z80_dasm_format_default(&default_fmt);
        fmt = &default_fmt;
    }

    /* Inicializace kontextu bufferu */
    ctx.buf = buf;
    ctx.size = buf_size;
    ctx.pos = 0;

    /* Zajisteni null-terminace i pro prazdny buffer */
    if (buf_size > 0) {
        buf[0] = '\0';
    }

    /* Neplatna sekvence (NOP*) nebo ignorovany prefix (DD*, FD*) -
     * zjednoduseny vystup: jen mnemonika z dekoderu */
    if (inst->cls == Z80_CLASS_INVALID) {
        /* Adresa */
        if (fmt->show_addr) {
            fmt_addr_prefix(&ctx, inst->addr);
        }

        /* Surove bajty */
        if (fmt->show_bytes) {
            fmt_raw_bytes(&ctx, inst->bytes, inst->length);
        }

        /* Mnemonika */
        buf_puts_case(&ctx, inst->mnemonic, fmt->uppercase);

        /* Vyhodnoceni navratove hodnoty */
        if (ctx.pos >= buf_size) {
            return -(ctx.pos);
        }
        return ctx.pos;
    }

    /* Adresa pred instrukci */
    if (fmt->show_addr) {
        fmt_addr_prefix(&ctx, inst->addr);
    }

    /* Surove bajty pred mnemonikou */
    if (fmt->show_bytes) {
        fmt_raw_bytes(&ctx, inst->bytes, inst->length);
    }

    /* Mnemonika */
    if (inst->mnemonic[0] != '\0') {
        buf_puts_case(&ctx, inst->mnemonic, fmt->uppercase);
    }

    /* Prvni operand (pokud existuje) */
    if (inst->op1.type != Z80_OP_NONE) {
        buf_putc(&ctx, ' ');
        fmt_operand(&ctx, &inst->op1, inst, fmt);

        /* Druhy operand (pokud existuje) */
        if (inst->op2.type != Z80_OP_NONE) {
            buf_putc(&ctx, ',');

            /* Specialni pripad: EX AF,AF' - druhy AF ma apostrof */
            if (is_ex_af_af(inst)) {
                buf_puts_case(&ctx, "AF'", fmt->uppercase);
            } else {
                fmt_operand(&ctx, &inst->op2, inst, fmt);
            }
        }
    }

    /* Vyhodnoceni navratove hodnoty */
    if (ctx.pos >= buf_size) {
        return -(ctx.pos);
    }
    return ctx.pos;
}
