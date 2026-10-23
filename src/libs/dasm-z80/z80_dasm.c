/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: MIT
 * https://github.com/michalhucik/z80-mz800
 */
/**
 * @file z80_dasm.c
 * @brief Jadro Z80 disassembleru - dekodovani instrukci a utility.
 * @version 0.1
 *
 * Implementuje hlavni disassemblovaci funkce z80_dasm() a z80_dasm_block(),
 * heuristicky z80_dasm_find_inst_start() a prevody relativnich adres.
 *
 * Dekoder cte bajty pres uzivatelsky callback, vyhleda odpovidajici
 * zaznam v opcode tabulce a vyplni strukturu z80_dasm_inst_t vcetne
 * rozlozenych operandu.
 */

#include <string.h>
#include "z80_dasm_internal.h"

/* ======================================================================
 * Interni: parsovani operandu z format stringu
 * ====================================================================== */

/**
 * @brief Rozpozna 8bitovy registr podle retezce.
 *
 * @param[in] s   Retezec k rozpoznani.
 * @param[out] len Delka rozpoznaneho tokenu.
 * @return Index registru (z80_reg8_t) nebo -1 pri neuspechu.
 */
static int parse_reg8(const char *s, int *len)
{
    /* Vicepismenne registry (musi byt pred jednopismenymi) */
    if (s[0] == 'I' && s[1] == 'X' && s[2] == 'H') { *len = 3; return Z80_R8_IXH; }
    if (s[0] == 'I' && s[1] == 'X' && s[2] == 'L') { *len = 3; return Z80_R8_IXL; }
    if (s[0] == 'I' && s[1] == 'Y' && s[2] == 'H') { *len = 3; return Z80_R8_IYH; }
    if (s[0] == 'I' && s[1] == 'Y' && s[2] == 'L') { *len = 3; return Z80_R8_IYL; }

    *len = 1;
    switch (s[0]) {
        case 'B': return Z80_R8_B;
        case 'C': return Z80_R8_C;
        case 'D': return Z80_R8_D;
        case 'E': return Z80_R8_E;
        case 'H': return Z80_R8_H;
        case 'L': return Z80_R8_L;
        case 'A': return Z80_R8_A;
        case 'F': return Z80_R8_F;
        case 'I': *len = 1; return Z80_R8_I;
        case 'R': *len = 1; return Z80_R8_R;
        default:  return -1;
    }
}

/**
 * @brief Rozpozna 16bitovy registr podle retezce.
 *
 * @param[in] s   Retezec k rozpoznani.
 * @param[out] len Delka rozpoznaneho tokenu.
 * @return Index registru (z80_reg16_t) nebo -1 pri neuspechu.
 */
static int parse_reg16(const char *s, int *len)
{
    *len = 2;
    if (s[0] == 'B' && s[1] == 'C') return Z80_R16_BC;
    if (s[0] == 'D' && s[1] == 'E') return Z80_R16_DE;
    if (s[0] == 'H' && s[1] == 'L') return Z80_R16_HL;
    if (s[0] == 'S' && s[1] == 'P') return Z80_R16_SP;
    if (s[0] == 'A' && s[1] == 'F') return Z80_R16_AF;
    if (s[0] == 'I' && s[1] == 'X') return Z80_R16_IX;
    if (s[0] == 'I' && s[1] == 'Y') return Z80_R16_IY;
    return -1;
}

/**
 * @brief Rozpozna podminku vetveni podle retezce.
 *
 * @param[in] s   Retezec k rozpoznani.
 * @param[out] len Delka rozpoznaneho tokenu.
 * @return Index podminky (z80_condition_t) nebo -1 pri neuspechu.
 */
static int parse_condition(const char *s, int *len)
{
    /* Dvouznakove podminky maji prednost */
    if (s[0] == 'N' && s[1] == 'Z') { *len = 2; return Z80_CC_NZ; }
    if (s[0] == 'N' && s[1] == 'C') { *len = 2; return Z80_CC_NC; }
    if (s[0] == 'P' && s[1] == 'O') { *len = 2; return Z80_CC_PO; }
    if (s[0] == 'P' && s[1] == 'E') { *len = 2; return Z80_CC_PE; }

    *len = 1;
    if (s[0] == 'Z')  return Z80_CC_Z;
    if (s[0] == 'C')  return Z80_CC_C;
    if (s[0] == 'P')  return Z80_CC_P;
    if (s[0] == 'M')  return Z80_CC_M;
    return -1;
}

/**
 * @brief Urcuje, zda je instrukce podminena (ma podminku jako operand).
 *
 * Podminene instrukce: JP cc, JR cc, CALL cc, RET cc, DJNZ.
 * Detekce podle flow_type z tabulky.
 */
static int is_conditional_flow(uint8_t flow_type)
{
    return flow_type == TJC || flow_type == TCC || flow_type == TRC;
}

/**
 * @brief Parsuje jeden operand z format stringu.
 *
 * Cte format string od pozice *pos a plni operandovou strukturu.
 * Pokud operand obsahuje zastupny znak, cte bajty z pameti.
 *
 * @param[out]    op         Operand k naplneni.
 * @param[in]     fmt        Format string.
 * @param[in,out] pos        Aktualni pozice ve format stringu.
 * @param[in]     read_fn    Callback pro cteni pameti.
 * @param[in]     user_data  Uzivatelska data pro callback.
 * @param[in,out] addr       Aktualni adresa cteni.
 * @param[in,out] bytes_read Pocet prectenych bajtu.
 * @param[in]     have_disp  Zda byl displacement jiz precten (DDCB/FDCB).
 * @param[in]     disp_val   Hodnota predcteneho displacementu.
 * @param[in]     flow_type  Typ toku (pro rozliseni podminky vs registr C).
 * @param[in]     is_first   Zda jde o prvni operand (pro podminky).
 */
static void parse_one_operand(z80_operand_t *op, const char *fmt, int *pos,
                              z80_dasm_read_fn read_fn, void *user_data,
                              uint16_t *addr, int *bytes_read,
                              int have_disp, uint8_t disp_val,
                              uint8_t flow_type, int is_first)
{
    const char *s = fmt + *pos;
    int len;

    op->type = Z80_OP_NONE;

    /* Preskoc mezery */
    while (*s == ' ') { s++; (*pos)++; }

    if (*s == '\0' || *s == ',') return;

    /* Zastupne znaky pro operandy z pameti */
    if (*s == '@') {
        /* 16bitove slovo */
        uint8_t lo = read_fn((*addr)++, user_data);
        uint8_t hi = read_fn((*addr)++, user_data);
        *bytes_read += 2;
        op->type = Z80_OP_IMM16;
        op->val.imm16 = (uint16_t)(lo | (hi << 8));
        (*pos)++;
        return;
    }

    if (*s == '#') {
        /* 8bitovy bajt */
        uint8_t val = read_fn((*addr)++, user_data);
        *bytes_read += 1;
        op->type = Z80_OP_IMM8;
        op->val.imm8 = val;
        (*pos)++;
        return;
    }

    if (*s == '%') {
        /* Relativni offset (JR/DJNZ) */
        uint8_t raw = read_fn((*addr)++, user_data);
        *bytes_read += 1;
        op->type = Z80_OP_REL8;
        op->val.displacement = (int8_t)raw;
        (*pos)++;
        return;
    }

    /* Pametove operandy v zavorkach */
    if (*s == '(') {
        s++; (*pos)++;

        /* (@) - prime adresovani */
        if (*s == '@') {
            uint8_t lo = read_fn((*addr)++, user_data);
            uint8_t hi = read_fn((*addr)++, user_data);
            *bytes_read += 2;
            op->type = Z80_OP_MEM_IMM16;
            op->val.imm16 = (uint16_t)(lo | (hi << 8));
            (*pos) += 2; /* @ a ) */
            return;
        }

        /* (#) - I/O port s primou adresou */
        if (*s == '#') {
            uint8_t val = read_fn((*addr)++, user_data);
            *bytes_read += 1;
            op->type = Z80_OP_MEM_IMM8;
            op->val.imm8 = val;
            (*pos) += 2;
            return;
        }

        /* (IX+$) nebo (IY+$) */
        if (s[0] == 'I' && (s[1] == 'X' || s[1] == 'Y') && s[2] == '+') {
            int8_t disp;
            if (have_disp) {
                disp = (int8_t)disp_val;
            } else {
                uint8_t raw = read_fn((*addr)++, user_data);
                *bytes_read += 1;
                disp = (int8_t)raw;
            }
            op->type = (s[1] == 'X') ? Z80_OP_MEM_IX_D : Z80_OP_MEM_IY_D;
            op->val.displacement = disp;
            /* Preskoc "IX+$)" nebo "IY+$)" */
            *pos += 5;
            return;
        }

        /* (BC), (DE), (HL), (SP), (IX), (IY) */
        int r16 = parse_reg16(s, &len);
        if (r16 >= 0) {
            op->type = Z80_OP_MEM_REG16;
            op->val.reg16 = (uint8_t)r16;
            *pos += len + 1; /* registr + ')' */
            return;
        }

        /* (C) - I/O port z registru C (IN/OUT instrukce) */
        if (s[0] == 'C' && s[1] == ')') {
            op->type = Z80_OP_MEM_REG16;
            op->val.reg16 = Z80_R16_BC; /* IN r,(C) pouziva cely BC jako adresu */
            *pos += 2;
            return;
        }

        return;
    }

    /* Vicemistny hex literal (RST vektory: "00","08","10","18","20","28","30","38")
       a cislo bitu 0-7 (BIT/SET/RES) */
    if (*s >= '0' && *s <= '9') {
        /* Pokud nasleduje dalsi hex cislice -> vicemistne cislo (RST vektor) */
        if ((s[1] >= '0' && s[1] <= '9') ||
            (s[1] >= 'A' && s[1] <= 'F') ||
            (s[1] >= 'a' && s[1] <= 'f')) {
            uint8_t vec = 0;
            while ((*s >= '0' && *s <= '9') ||
                   (*s >= 'A' && *s <= 'F') ||
                   (*s >= 'a' && *s <= 'f')) {
                if (*s >= '0' && *s <= '9')
                    vec = vec * 16 + (*s - '0');
                else
                    vec = vec * 16 + ((*s & 0xDF) - 'A' + 10);
                s++; (*pos)++;
            }
            op->type = Z80_OP_RST_VEC;
            op->val.imm8 = vec;
            return;
        }

        /* Jednociferny: cislo bitu 0-7 (BIT/SET/RES) */
        if (*s >= '0' && *s <= '7' && (s[1] == ',' || s[1] == '\0')) {
        op->type = Z80_OP_BIT_INDEX;
            op->type = Z80_OP_BIT_INDEX;
            op->val.bit_index = (uint8_t)(*s - '0');
            (*pos)++;
            return;
        }

        return; /* nerozpoznany ciselny literal */
    }

    /* Podminky: NZ, Z, NC, C, PO, PE, P, M
       Podminky se vyskytuji pouze jako prvni operand podminenych instrukci.
       "C" je nejednoznacne (registr vs podminka) - rozlisujeme podle flow_type. */
    if (is_first && is_conditional_flow(flow_type)) {
        int cc = parse_condition(s, &len);
        if (cc >= 0) {
            op->type = Z80_OP_CONDITION;
            op->val.condition = (uint8_t)cc;
            *pos += len;
            return;
        }
    }

    /* 16bitovy registr (musi byt pred 8bitovym kvuli BC, DE, HL...) */
    int r16 = parse_reg16(s, &len);
    if (r16 >= 0 && (s[len] == ',' || s[len] == '\0' || s[len] == '\'')) {
        op->type = Z80_OP_REG16;
        op->val.reg16 = (uint8_t)r16;
        *pos += len;
        /* AF' - preskoc apostrof */
        if (s[len] == '\'') (*pos)++;
        return;
    }

    /* 8bitovy registr */
    int r8 = parse_reg8(s, &len);
    if (r8 >= 0) {
        op->type = Z80_OP_REG8;
        op->val.reg8 = (uint8_t)r8;
        *pos += len;
        return;
    }

    /* Literal "0" v "OUT (C),0" */
    if (*s == '0' && (s[1] == ',' || s[1] == '\0')) {
        op->type = Z80_OP_IMM8;
        op->val.imm8 = 0;
        (*pos)++;
        return;
    }
}

/**
 * @brief Extrahuje zakladni mnemoniku z format stringu do uziv. bufferu.
 *
 * Zapisuje do bufferu poskytnuteho volajicim - thread-safe a bez aliasing
 * mezi vice volanimi. Buffer musi mit minimalne 16 znaku (Z80 mnemonika
 * + '\0').
 *
 * @param format    Format string instrukce z opcode tabulky.
 * @param buf       Cilovy buffer (min 16 znaku).
 * @param buf_size  Velikost bufferu.
 */
void z80_dasm_extract_mnemonic_into(const char *format, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    if (!format) {
        if (buf_size >= 4) {
            buf[0] = '?'; buf[1] = '?'; buf[2] = '?'; buf[3] = '\0';
        } else {
            buf[0] = '\0';
        }
        return;
    }
    size_t i = 0;
    size_t maxw = (buf_size < 16) ? buf_size - 1 : 15;
    while (format[i] && format[i] != ' ' && i < maxw) {
        buf[i] = format[i];
        i++;
    }
    buf[i] = '\0';
}

/**
 * @brief Zpetne kompatibilni shim - vraci pointer na interni static buffer.
 *
 * @deprecated Pouzivej z80_dasm_extract_mnemonic_into() pro thread-safe
 *             kopii do volajicim spravovaneho bufferu.
 */
const char *z80_dasm_extract_mnemonic(const char *format)
{
    static char buf[16];
    z80_dasm_extract_mnemonic_into(format, buf, sizeof(buf));
    return buf;
}

/* ======================================================================
 * Verejne API: jadro disassembleru
 * ====================================================================== */

int z80_dasm(z80_dasm_inst_t *inst,
             z80_dasm_read_fn read_fn, void *user_data,
             uint16_t addr)
{
    uint8_t opc, next;
    uint8_t disp_u = 0;
    int have_disp = 0;
    int bytes = 0;
    uint16_t start_addr = addr;
    const z80_dasm_opc_t *entry = NULL;

    memset(inst, 0, sizeof(*inst));
    inst->addr = start_addr;

    /* Cti prvni bajt */
    opc = read_fn(addr++, user_data);
    inst->bytes[bytes++] = opc;

    switch (opc) {
    case 0xDD:
    case 0xFD:
        next = read_fn(addr++, user_data);
        inst->bytes[bytes++] = next;

        /* Ignorovany prefix: DD DD, DD FD, FD DD, FD FD, DD ED, FD ED.
         * CPU prefix vykona (4 T, R + 1) a dalsi bajt zpracuje jako
         * novou instrukci; jadro emulatoru je provede v jednom kroku.
         * Vrat prefix jako 1bajtovou instrukci "DD*" / "FD*". */
        if ((next | 0x20) == 0xFD || next == 0xED) {
            inst->length = 1;
            inst->t_states = 4;
            inst->t_states2 = 0;
            inst->flow = Z80_FLOW_NORMAL;
            inst->cls = Z80_CLASS_INVALID;
            strcpy(inst->mnemonic, (opc == 0xDD) ? "DD*" : "FD*");
            inst->op1.type = Z80_OP_NONE;
            inst->op2.type = Z80_OP_NONE;
            return 1;
        }

        if (next == 0xCB) {
            /* DD CB d opcode / FD CB d opcode */
            disp_u = read_fn(addr++, user_data);
            inst->bytes[bytes++] = disp_u;
            uint8_t final_opc = read_fn(addr++, user_data);
            inst->bytes[bytes++] = final_opc;
            bytes = 4;
            have_disp = 1;
            entry = (opc == 0xDD) ? &z80_dasm_ddcb[final_opc]
                                  : &z80_dasm_fdcb[final_opc];
        } else {
            /* Normalni DD/FD instrukce */
            entry = (opc == 0xDD) ? &z80_dasm_dd[next]
                                  : &z80_dasm_fd[next];
            /* NULL format = fallback na base tabulku (zrcadlena instrukce) */
            if (entry->format == NULL) {
                entry = &z80_dasm_base[next];
                /* Zrcadlena instrukce: DD prefix prida 4T */
                inst->t_states = 4;
            }
        }
        break;

    case 0xED:
        next = read_fn(addr++, user_data);
        inst->bytes[bytes++] = next;
        entry = &z80_dasm_ed[next];
        if (entry->format == NULL) {
            /* Neplatna ED instrukce */
            inst->length = 2;
            inst->t_states = 8;
            inst->t_states2 = 0;
            inst->flow = Z80_FLOW_NORMAL;
            inst->cls = Z80_CLASS_INVALID;
            strcpy(inst->mnemonic, "NOP*");
            inst->op1.type = Z80_OP_NONE;
            inst->op2.type = Z80_OP_NONE;
            return 2;
        }
        break;

    case 0xCB:
        next = read_fn(addr++, user_data);
        inst->bytes[bytes++] = next;
        entry = &z80_dasm_cb[next];
        break;

    default:
        /* Zakladni instrukce */
        entry = &z80_dasm_base[opc];
        /* Prefixy CB, DD, ED, FD maji NULL format v base tabulce */
        if (entry->format == NULL) {
            inst->length = 1;
            inst->t_states = 4;
            inst->t_states2 = 0;
            inst->flow = Z80_FLOW_NORMAL;
            inst->cls = Z80_CLASS_INVALID;
            strcpy(inst->mnemonic, "NOP*");
            inst->op1.type = Z80_OP_NONE;
            inst->op2.type = Z80_OP_NONE;
            return 1;
        }
        break;
    }

    /* Vyplneni metadata z tabulky */
    inst->t_states += entry->t_states;
    inst->t_states2 = entry->t_states2;
    inst->flags_affected = entry->flags_affected;
    inst->regs_read = entry->regs_read;
    inst->regs_written = entry->regs_written;
    inst->flow = (z80_flow_type_t)entry->flow_type;
    inst->cls = (z80_inst_class_t)entry->inst_class;

    /* Extrakce mnemoniky - zapis primo do per-instance bufferu */
    z80_dasm_extract_mnemonic_into(entry->format, inst->mnemonic, sizeof(inst->mnemonic));

    /* Parsovani operandu z format stringu */
    const char *fmt = entry->format;
    int pos = 0;

    /* Preskoc mnemoniku (vse pred prvni mezerou) */
    while (fmt[pos] && fmt[pos] != ' ') pos++;
    if (fmt[pos] == ' ') pos++;

    /* Prvni operand */
    if (fmt[pos] && fmt[pos] != '\0') {
        parse_one_operand(&inst->op1, fmt, &pos, read_fn, user_data,
                          &addr, &bytes, have_disp, disp_u,
                          entry->flow_type, 1);
    }

    /* Carka mezi operandy */
    if (fmt[pos] == ',') pos++;

    /* Druhy operand */
    if (fmt[pos] && fmt[pos] != '\0') {
        parse_one_operand(&inst->op2, fmt, &pos, read_fn, user_data,
                          &addr, &bytes, have_disp, disp_u,
                          entry->flow_type, 0);
    }

    inst->length = (uint8_t)bytes;

    /* Doplneni surovych bajtu (operandy prectenych navic) */
    /* bytes[0..prefix_len-1] uz jsou vyplneny, zbytek doplnime */
    /* Prepocitame bajty z adresy */
    for (int i = inst->bytes[0] == 0xDD || inst->bytes[0] == 0xFD ||
                 inst->bytes[0] == 0xED || inst->bytes[0] == 0xCB ? 2 : 1;
         i < bytes && i < 4; i++) {
        if (inst->bytes[i] == 0 && i >= 2) {
            /* Bajty operandu - precteme je znovu */
            inst->bytes[i] = read_fn((uint16_t)(start_addr + i), user_data);
        }
    }

    /* Korektni vyplneni vsech bajtu */
    for (int i = 0; i < (int)inst->length && i < 4; i++) {
        inst->bytes[i] = read_fn((uint16_t)(start_addr + i), user_data);
    }

    return (int)inst->length;
}

int z80_dasm_block(z80_dasm_inst_t *out, int max_inst,
                   z80_dasm_read_fn read_fn, void *user_data,
                   uint16_t start_addr, uint16_t end_addr)
{
    int count = 0;
    uint16_t addr = start_addr;

    if (start_addr > end_addr) return 0;

    while (count < max_inst && addr <= end_addr) {
        int len = z80_dasm(&out[count], read_fn, user_data, addr);

        /* Ochrana proti preteceni uint16_t */
        uint32_t next = (uint32_t)addr + (uint32_t)len;
        if (next > 0xFFFF && end_addr != 0xFFFF) break;

        count++;
        addr = (uint16_t)next;

        /* Zastaveni pokud dalsi instrukce by zacala za end_addr */
        if (addr > end_addr && addr != 0) break;
    }

    return count;
}

uint16_t z80_dasm_find_inst_start(z80_dasm_read_fn read_fn, void *user_data,
                             uint16_t target_addr, uint16_t search_from)
{
    /*
     * Heuristika: disassembluj z vice startovnich bodu
     * a sleduj, ktera adresa se objevuje nejcasteji
     * jako zacatek instrukce tesne pred target_addr.
     *
     * Z80 instrukce maji delku 1-4 bajty, proto hledame
     * konsensus z az 16 ruznych startovnich bodu.
     */
    uint16_t votes[4] = {0};  /* az 4 kandidati */
    int vote_count[4] = {0};
    int num_candidates = 0;
    z80_dasm_inst_t tmp;

    if (search_from >= target_addr) return target_addr;

    /* Zkusime startovat z kazde adresy v rozsahu [search_from, target_addr) */
    for (uint16_t start = search_from; start < target_addr; start++) {
        uint16_t a = start;

        /* Disassembluj dokud nedosahnes target_addr */
        while (a < target_addr) {
            int len = z80_dasm(&tmp, read_fn, user_data, a);
            uint16_t next_a = (uint16_t)(a + len);

            if (next_a == target_addr) {
                /* Tato cesta dosla presne na target_addr - dobry znak */
                /* Posledni instrukce pred cilem je nas kandidat */
                int found = 0;
                for (int i = 0; i < num_candidates; i++) {
                    if (votes[i] == a) {
                        vote_count[i]++;
                        found = 1;
                        break;
                    }
                }
                if (!found && num_candidates < 4) {
                    votes[num_candidates] = a;
                    vote_count[num_candidates] = 1;
                    num_candidates++;
                }
                break;
            }

            if (next_a > target_addr) break; /* presah */
            a = next_a;
        }
    }

    /* Najdi kandidata s nejvice hlasy */
    int best = 0;
    for (int i = 1; i < num_candidates; i++) {
        if (vote_count[i] > vote_count[best]) best = i;
    }

    return (num_candidates > 0) ? votes[best] : target_addr;
}

/* ======================================================================
 * Konverze relativnich adres
 * ====================================================================== */

uint16_t z80_rel_to_abs(uint16_t addr, int8_t offset)
{
    return (uint16_t)(addr + 2 + offset);
}

int z80_abs_to_rel(uint16_t addr, uint16_t target, int8_t *offset)
{
    int diff = (int)target - (int)addr - 2;

    if (diff < -128 || diff > 127) return -1;

    *offset = (int8_t)diff;
    return 0;
}
