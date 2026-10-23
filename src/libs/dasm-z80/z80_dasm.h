/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: MIT
 * https://github.com/michalhucik/z80-mz800
 */
/**
 * @file z80_dasm.h
 * @brief Z80 disassembler pro emulator Sharp MZ-800.
 * @version 0.1
 * @date 2026-04-01
 *
 * Kompletni Z80 disassembler s podporou vsech dokumentovanych
 * i nedokumentovanych instrukci. Navrzen pro pouziti v debuggeru
 * emulatoru, poskytuje strukturovany vystup s analyzou toku rizeni,
 * mapou registru/flagu a volitelnou tabulkou symbolu.
 *
 * Architektura: "parsuj jednou, dotazuj se mnohokrat"
 * - z80_dasm() dekoduje instrukci do struktury z80_dasm_inst_t
 * - z80_dasm_to_str() / z80_dasm_to_str_sym() formatuji do textu
 * - z80_dasm_target_addr() / z80_dasm_branch_taken() analyzuji tok
 * - z80_dasm_regs_read() / z80_dasm_regs_written() analyzuji registry
 *
 * Zpetna kompatibilita s z80ex_dasm() je zajistena obalkou
 * v z80_dasm_compat.c.
 *
 * @note Knihovna je thread-safe - nepouziva globalni stav ani staticke
 *       buffery. Vsechny vystupy jsou zapisovany do uzivatelem
 *       poskytnutych bufferu.
 */

#ifndef Z80_DASM_H
#define Z80_DASM_H

/** Hlavni cislo verze knihovny dasm-z80. */
#define Z80_DASM_VERSION_MAJOR 0
/** Vedlejsi cislo verze knihovny dasm-z80. */
#define Z80_DASM_VERSION_MINOR 1
/** Retezec verze knihovny dasm-z80. */
#define Z80_DASM_VERSION_STR   "0.1"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * Konstanty a flagy
 * ====================================================================== */

/**
 * @defgroup z80_flags Bitove masky flagu registru F
 *
 * Odpovidaji fyzickemu rozlozeni bitu v registru F procesoru Z80.
 * Pouzivaji se jako bitove masky ve flags_affected, flags_read
 * a pro funkci z80_dasm_branch_taken().
 * @{
 */
#define Z80_FLAG_C   0x01  /**< Carry */
#define Z80_FLAG_N   0x02  /**< Subtract (N) */
#define Z80_FLAG_PV  0x04  /**< Parity / Overflow */
#define Z80_FLAG_3   0x08  /**< Nedokumentovany bit 3 (kopie bitu 3 vysledku) */
#define Z80_FLAG_H   0x10  /**< Half-carry */
#define Z80_FLAG_5   0x20  /**< Nedokumentovany bit 5 (kopie bitu 5 vysledku) */
#define Z80_FLAG_Z   0x40  /**< Zero */
#define Z80_FLAG_S   0x80  /**< Sign */
/** @} */

/** Vsechny dokumentovane flagy (bez bitu 3 a 5). */
#define Z80_FLAGS_DOCUMENTED (Z80_FLAG_C | Z80_FLAG_N | Z80_FLAG_PV | \
                              Z80_FLAG_H | Z80_FLAG_Z | Z80_FLAG_S)

/** Vsechny flagy vcetne nedokumentovanych. */
#define Z80_FLAGS_ALL 0xFF

/* ======================================================================
 * Registry - bitove masky
 * ====================================================================== */

/**
 * @defgroup z80_regs Bitove masky registru
 *
 * Pouzivaji se v polich regs_read a regs_written struktury
 * z80_dasm_inst_t. Umoznuji rychle zjisteni, ktere registry
 * instrukce cte nebo zapisuje.
 * @{
 */
typedef enum {
    Z80_DASM_REG_A    = (1 << 0),   /**< Akumulator */
    Z80_DASM_REG_F    = (1 << 1),   /**< Registr flagu */
    Z80_DASM_REG_B    = (1 << 2),   /**< Registr B */
    Z80_DASM_REG_C    = (1 << 3),   /**< Registr C */
    Z80_DASM_REG_D    = (1 << 4),   /**< Registr D */
    Z80_DASM_REG_E    = (1 << 5),   /**< Registr E */
    Z80_DASM_REG_H    = (1 << 6),   /**< Registr H */
    Z80_DASM_REG_L    = (1 << 7),   /**< Registr L */
    Z80_DASM_REG_IXH  = (1 << 8),   /**< Horni polovina IX */
    Z80_DASM_REG_IXL  = (1 << 9),   /**< Dolni polovina IX */
    Z80_DASM_REG_IYH  = (1 << 10),  /**< Horni polovina IY */
    Z80_DASM_REG_IYL  = (1 << 11),  /**< Dolni polovina IY */
    Z80_DASM_REG_SP   = (1 << 12),  /**< Stack pointer */
    Z80_DASM_REG_I    = (1 << 13),  /**< Interrupt vector registr */
    Z80_DASM_REG_R    = (1 << 14),  /**< Memory refresh registr */
    Z80_DASM_REG_WZ   = (1 << 15),  /**< Interni registr MEMPTR (WZ) */
} z80_dasm_reg_mask_t;

/** Registrovy par AF (akumulator + flagy). */
#define Z80_DASM_REG_AF  (Z80_DASM_REG_A | Z80_DASM_REG_F)
/** Registrovy par BC. */
#define Z80_DASM_REG_BC  (Z80_DASM_REG_B | Z80_DASM_REG_C)
/** Registrovy par DE. */
#define Z80_DASM_REG_DE  (Z80_DASM_REG_D | Z80_DASM_REG_E)
/** Registrovy par HL. */
#define Z80_DASM_REG_HL  (Z80_DASM_REG_H | Z80_DASM_REG_L)
/** Index registr IX (obe poloviny). */
#define Z80_DASM_REG_IX  (Z80_DASM_REG_IXH | Z80_DASM_REG_IXL)
/** Index registr IY (obe poloviny). */
#define Z80_DASM_REG_IY  (Z80_DASM_REG_IYH | Z80_DASM_REG_IYL)
/** @} */

/* ======================================================================
 * Enumerace
 * ====================================================================== */

/**
 * @brief Typ operandu instrukce.
 *
 * Urcuje, jak interpretovat hodnotu v unii z80_operand_t::val.
 */
typedef enum {
    Z80_OP_NONE,         /**< Zadny operand. */
    Z80_OP_REG8,         /**< 8bitovy registr (A, B, C, D, E, H, L, IXH, IXL, IYH, IYL). */
    Z80_OP_REG16,        /**< 16bitovy registr (BC, DE, HL, SP, IX, IY, AF). */
    Z80_OP_IMM8,         /**< 8bitova konstanta (napr. LD A,#nn). */
    Z80_OP_IMM16,        /**< 16bitova konstanta (napr. LD BC,#nnnn). */
    Z80_OP_MEM_REG16,    /**< Neprime adresovani registrem: (BC), (DE), (HL), (SP). */
    Z80_OP_MEM_IX_D,     /**< Indexovane adresovani (IX+d). */
    Z80_OP_MEM_IY_D,     /**< Indexovane adresovani (IY+d). */
    Z80_OP_MEM_IMM16,    /**< Prime adresovani pameti (nn): napr. LD A,(nn). */
    Z80_OP_MEM_IMM8,     /**< I/O port s primou adresou (n): IN A,(n) / OUT (n),A. */
    Z80_OP_CONDITION,    /**< Podminka vetveni (NZ, Z, NC, C, PO, PE, P, M). */
    Z80_OP_BIT_INDEX,    /**< Cislo bitu 0-7 pro instrukce BIT/SET/RES. */
    Z80_OP_REL8,         /**< Relativni offset pro JR a DJNZ. */
    Z80_OP_RST_VEC,      /**< Restart vektor (0x00, 0x08, ..., 0x38). */
} z80_operand_type_t;

/**
 * @brief Identifikatory 8bitovych registru.
 *
 * Pouzivaji se v z80_operand_t::val::reg8 kdyz type == Z80_OP_REG8.
 */
typedef enum {
    Z80_R8_B = 0,   /**< Registr B */
    Z80_R8_C,       /**< Registr C */
    Z80_R8_D,       /**< Registr D */
    Z80_R8_E,       /**< Registr E */
    Z80_R8_H,       /**< Registr H */
    Z80_R8_L,       /**< Registr L */
    Z80_R8_A,       /**< Akumulator */
    Z80_R8_F,       /**< Registr flagu (specialni pripady: IN F,(C)) */
    Z80_R8_IXH,     /**< Horni polovina IX (nedokumentovany) */
    Z80_R8_IXL,     /**< Dolni polovina IX (nedokumentovany) */
    Z80_R8_IYH,     /**< Horni polovina IY (nedokumentovany) */
    Z80_R8_IYL,     /**< Dolni polovina IY (nedokumentovany) */
    Z80_R8_I,       /**< Interrupt vector registr */
    Z80_R8_R,       /**< Memory refresh registr */
} z80_reg8_t;

/**
 * @brief Identifikatory 16bitovych registru a registrovych paru.
 *
 * Pouzivaji se v z80_operand_t::val::reg16 kdyz type == Z80_OP_REG16
 * nebo Z80_OP_MEM_REG16.
 */
typedef enum {
    Z80_R16_BC = 0,  /**< Registrovy par BC */
    Z80_R16_DE,      /**< Registrovy par DE */
    Z80_R16_HL,      /**< Registrovy par HL */
    Z80_R16_SP,      /**< Stack pointer */
    Z80_R16_AF,      /**< Registrovy par AF (pouzivan v PUSH/POP) */
    Z80_R16_IX,      /**< Index registr IX */
    Z80_R16_IY,      /**< Index registr IY */
} z80_reg16_t;

/**
 * @brief Podminky vetveni.
 *
 * Odpovidaji kodovani podminek v instrukcich JP cc, CALL cc, RET cc, JR cc.
 */
typedef enum {
    Z80_CC_NZ = 0,  /**< Non-zero (Z=0) */
    Z80_CC_Z,       /**< Zero (Z=1) */
    Z80_CC_NC,      /**< No carry (C=0) */
    Z80_CC_C,       /**< Carry (C=1) */
    Z80_CC_PO,      /**< Parity odd (PV=0) */
    Z80_CC_PE,      /**< Parity even (PV=1) */
    Z80_CC_P,       /**< Positive (S=0) */
    Z80_CC_M,       /**< Minus (S=1) */
} z80_condition_t;

/**
 * @brief Typ toku rizeni instrukce.
 *
 * Urcuje, jak instrukce ovlivnuje program counter. Klicove pro debugger -
 * umoznuje implementaci step-over, zobrazeni sipek skoku, atd.
 */
typedef enum {
    Z80_FLOW_NORMAL,        /**< Bezna instrukce, PC pokracuje na dalsi. */
    Z80_FLOW_JUMP,          /**< Bezpodminecny skok (JP nn, JR e). */
    Z80_FLOW_JUMP_COND,     /**< Podminecny skok (JP cc,nn / JR cc,e). */
    Z80_FLOW_CALL,          /**< Bezpodminecne volani (CALL nn). */
    Z80_FLOW_CALL_COND,     /**< Podminecne volani (CALL cc,nn). */
    Z80_FLOW_RET,           /**< Bezpodminecny navrat (RET). */
    Z80_FLOW_RET_COND,      /**< Podminecny navrat (RET cc). */
    Z80_FLOW_RST,           /**< Restart vektor (RST p). */
    Z80_FLOW_HALT,          /**< HALT - CPU zastaven do preruseni. */
    Z80_FLOW_JUMP_INDIRECT, /**< Neprimi skok: JP (HL), JP (IX), JP (IY). */
    Z80_FLOW_RETI,          /**< Navrat z maskovaneho preruseni (RETI). */
    Z80_FLOW_RETN,          /**< Navrat z nemaskovaneho preruseni (RETN). */
} z80_flow_type_t;

/**
 * @brief Klasifikace instrukce.
 *
 * Rozlisuje oficialne dokumentovane instrukce, nedokumentovane (ale realne
 * existujici na hardware) a neplatne sekvence.
 */
typedef enum {
    Z80_CLASS_OFFICIAL,      /**< Dokumentovana instrukce (Zilog manual). */
    Z80_CLASS_UNDOCUMENTED,  /**< Nedokumentovana ale funkcni na realnem HW.
                                  Priklady: SLL, IXH/IXL operace,
                                  DD CB d op s kopirovani do registru. */
    Z80_CLASS_INVALID,       /**< Neplatna nebo ignorovana sekvence:
                                  nedefinovana ED xx (NOP*, 2 bajty, 8 T)
                                  a ignorovany prefix DD/FD pred DD, FD
                                  nebo ED (DD*, FD*, 1 bajt, 4 T; CPU ho
                                  vykona spolu s nasledujici instrukci). */
} z80_inst_class_t;

/* ======================================================================
 * Operand
 * ====================================================================== */

/**
 * @brief Operand instrukce Z80.
 *
 * Obsahuje typ operandu a jeho hodnotu. Typ urcuje, ktery clen unie val
 * je platny.
 *
 * @invariant Kdyz type == Z80_OP_NONE, zadny clen unie neni platny.
 * @invariant Kdyz type == Z80_OP_REG8, platny je val.reg8.
 * @invariant Kdyz type == Z80_OP_REG16 nebo Z80_OP_MEM_REG16, platny je val.reg16.
 * @invariant Kdyz type == Z80_OP_IMM8 nebo Z80_OP_MEM_IMM8, platny je val.imm8.
 * @invariant Kdyz type == Z80_OP_IMM16 nebo Z80_OP_MEM_IMM16, platny je val.imm16.
 * @invariant Kdyz type == Z80_OP_MEM_IX_D nebo Z80_OP_MEM_IY_D, platny je val.displacement.
 * @invariant Kdyz type == Z80_OP_CONDITION, platny je val.condition.
 * @invariant Kdyz type == Z80_OP_BIT_INDEX, platny je val.bit_index.
 * @invariant Kdyz type == Z80_OP_REL8, platny je val.displacement.
 * @invariant Kdyz type == Z80_OP_RST_VEC, platny je val.imm8.
 */
typedef struct {
    z80_operand_type_t type;  /**< Typ operandu - urcuje interpretaci val. */
    union {
        uint8_t  reg8;             /**< Index 8bitoveho registru (z80_reg8_t). */
        uint8_t  reg16;            /**< Index 16bitoveho registru (z80_reg16_t). */
        uint8_t  imm8;             /**< 8bitova hodnota (konstanta, port, RST vektor). */
        uint16_t imm16;            /**< 16bitova hodnota (adresa, konstanta). */
        int8_t  displacement;     /**< Znamenkovy offset: IX/IY+d nebo relativni skok. */
        uint8_t  condition;        /**< Podminka vetveni (z80_condition_t). */
        uint8_t  bit_index;        /**< Cislo bitu 0-7 pro BIT/SET/RES. */
    } val;                    /**< Hodnota operandu. Interpretace zavisi na type. */
} z80_operand_t;

/* ======================================================================
 * Vysledek disassemblace
 * ====================================================================== */

/**
 * @brief Vysledek disassemblace jedne instrukce Z80.
 *
 * Obsahuje vsechny informace o dekodovane instrukci: surove bajty,
 * rozlozene operandy, casovani, tok rizeni a mapu pristupu k registrum.
 *
 * Struktura je vyplnena funkci z80_dasm() a nasledne muze byt dotazovana
 * pomoci ruznych analytickych funkci, nebo formatovana do textu.
 *
 * @invariant length je vzdy v rozsahu 1-4.
 * @invariant bytes[0..length-1] obsahuji platne bajty instrukce.
 * @invariant t_states > 0 pro kazdou platnou instrukci.
 * @invariant t_states2 == 0 pro instrukce bez vetveni.
 * @invariant mnemonic je null-terminovany retezec (max 15 znaku + '\0').
 */
typedef struct {
    uint16_t addr;                /**< Adresa prvniho bajtu instrukce v pameti. */
    uint8_t  bytes[4];            /**< Surove bajty instrukce (max 4 pro Z80). */
    uint8_t  length;              /**< Delka instrukce v bajtech (1-4). */
    uint8_t  t_states;            /**< Pocet T-stavu (zakladni doba provedeni). */
    uint8_t  t_states2;           /**< T-stavy pri vetveni (0 = neni vetveni).
                                  Pro podminecne instrukce: pocet T-stavu
                                  kdyz je podminka splnena a dojde ke skoku. */
    z80_flow_type_t flow;    /**< Typ toku rizeni (skok, volani, navrat...). */
    z80_inst_class_t cls;    /**< Klasifikace: official/undocumented/invalid. */
    z80_operand_t op1;       /**< Prvni (cilovy) operand. Z80_OP_NONE pokud chybi. */
    z80_operand_t op2;       /**< Druhy (zdrojovy) operand. Z80_OP_NONE pokud chybi. */
    char mnemonic[16];       /**< Zakladni mnemonika instrukce ("LD", "ADD", "JP"...).
                                  Embedded null-terminovany buffer (per-instance
                                  storage) - bezpecne pro kopirovani struktury
                                  a pouziti napric vlakny. Pro neplatne sekvence
                                  obsahuje "NOP*", pro ignorovany prefix
                                  "DD*" / "FD*". */
    uint8_t  flags_affected;      /**< Bitova maska flagu, ktere instrukce modifikuje.
                                  Pouziva Z80_FLAG_* konstanty. */
    uint16_t regs_read;           /**< Bitova maska registru, ktere instrukce cte.
                                  Pouziva z80_dasm_reg_mask_t hodnoty. */
    uint16_t regs_written;        /**< Bitova maska registru, ktere instrukce zapisuje.
                                  Pouziva z80_dasm_reg_mask_t hodnoty. */
} z80_dasm_inst_t;

/* ======================================================================
 * Callback pro cteni pameti
 * ====================================================================== */

/**
 * @brief Callback pro cteni bajtu z pameti.
 *
 * Disassembler vola tuto funkci pro kazdy bajt, ktery potrebuje precist.
 * Implementace musi byt schopna vratit bajt pro libovolnou adresu 0x0000-0xFFFF.
 *
 * @param[in] addr      Adresa bajtu ke cteni (0x0000-0xFFFF).
 * @param[in] user_data Uzivatelska data predana do z80_dasm().
 * @return Hodnota bajtu na dane adrese.
 *
 * @pre addr je platna 16bitova adresa.
 * @post Funkce nesmi modifikovat stav emulatoru (zadne vedlejsi efekty).
 */
typedef uint8_t (*z80_dasm_read_fn)(uint16_t addr, void *user_data);

/* ======================================================================
 * Jadro disassembleru
 * ====================================================================== */

/**
 * @brief Disassembluj jednu instrukci na dane adrese.
 *
 * Hlavni funkce disassembleru. Dekoduje instrukci vcetne prefixu a operandu,
 * vyplni strukturu z80_dasm_inst_t vsemi dostupnymi informacemi.
 *
 * Podporuje vsechny dokumentovane i nedokumentovane instrukce Z80 vcetne:
 * - Zakladni instrukce (bez prefixu)
 * - CB prefix (bitove operace a rotace)
 * - ED prefix (rozsirene instrukce)
 * - DD/FD prefix (IX/IY operace)
 * - DD CB / FD CB (indexovane bitove operace)
 * - Nedokumentovane: SLL, IXH/IXL/IYH/IYL, DD CB kopie do registru
 * - Ignorovany prefix (DD DD, DD FD, DD ED -> DD*, FD obdobne FD*),
 *   neplatne ED xx -> NOP*
 *
 * @param[out] inst      Struktura pro ulozeni vysledku. Nesmi byt NULL.
 * @param[in]  read_fn   Callback pro cteni bajtu z pameti. Nesmi byt NULL.
 * @param[in]  user_data Uzivatelska data predana do read_fn.
 * @param[in]  addr      Adresa prvniho bajtu instrukce.
 * @return Delka instrukce v bajtech (1-4). Stejna hodnota jako inst->length.
 *
 * @pre inst != NULL
 * @pre read_fn != NULL
 * @post inst je plne vyplnena platnymi hodnotami.
 * @post inst->length == navratova hodnota.
 * @post inst->addr == addr.
 */
int z80_dasm(z80_dasm_inst_t *inst,
             z80_dasm_read_fn read_fn, void *user_data,
             uint16_t addr);

/**
 * @brief Disassembluj blok instrukci v rozsahu adres.
 *
 * Sekvencne disassembluje instrukce od start_addr do end_addr (vcetne).
 * Zastavi se kdyz:
 * - Dosahne end_addr (dalsi instrukce by zacala za end_addr)
 * - Vyplni max_inst zaznamu
 *
 * Vhodne pro zobrazeni disassembly view v debuggeru.
 *
 * @param[out] out        Pole pro ulozeni vysledku. Nesmi byt NULL.
 * @param[in]  max_inst   Maximalni pocet instrukci k dekodovani. Musi byt > 0.
 * @param[in]  read_fn    Callback pro cteni bajtu z pameti. Nesmi byt NULL.
 * @param[in]  user_data  Uzivatelska data predana do read_fn.
 * @param[in]  start_addr Pocatecni adresa bloku.
 * @param[in]  end_addr   Koncova adresa bloku (vcetne).
 * @return Pocet skutecne disassemblovanych instrukci (0 az max_inst).
 *
 * @pre out != NULL
 * @pre max_inst > 0
 * @pre read_fn != NULL
 * @note Pokud start_addr > end_addr, vraci 0.
 */
int z80_dasm_block(z80_dasm_inst_t *out, int max_inst,
                   z80_dasm_read_fn read_fn, void *user_data,
                   uint16_t start_addr, uint16_t end_addr);

/**
 * @brief Heuristicky najdi zacatek instrukce obsahujici danou adresu.
 *
 * Z80 ma instrukce promenne delky (1-4 bajty) bez zarovnani, takze
 * zpetne dekodovani neni deterministicke. Tato funkce pouziva heuristiku:
 * disassembluje z vice startovnich bodu v rozsahu [search_from, target_addr]
 * a hleda konsensus - adresu, na ktere se shoduje nejvice pruchodu.
 *
 * Typicke pouziti: scrollovani zpet v disassembly view debuggeru.
 *
 * @param[in] read_fn     Callback pro cteni bajtu z pameti. Nesmi byt NULL.
 * @param[in] user_data   Uzivatelska data predana do read_fn.
 * @param[in] target_addr Adresa, pro kterou hledame zacatek instrukce.
 * @param[in] search_from Adresa odkud zacit zpetne hledani.
 *                        Doporucena hodnota: target_addr - 8 az -16.
 * @return Odhadnuta adresa zacatku instrukce. Muze byt rovna target_addr
 *         pokud target_addr je zacatek instrukce.
 *
 * @pre read_fn != NULL
 * @pre search_from <= target_addr
 * @warning Vysledek neni 100% spolehliva - Z80 nema zarovnani instrukci.
 *          V praxi vsak funguje spolehlive pro typicky strukturovany kod.
 */
uint16_t z80_dasm_find_inst_start(z80_dasm_read_fn read_fn, void *user_data,
                             uint16_t target_addr, uint16_t search_from);

/* ======================================================================
 * Formatovani vystupu
 * ====================================================================== */

/**
 * @brief Styl hexadecimalnich cisel ve vystupu.
 */
typedef enum {
    Z80_HEX_HASH,     /**< #FF, #1234 - z80ex vychozi styl. */
    Z80_HEX_0X,       /**< 0xFF, 0x1234 - C-style. */
    Z80_HEX_DOLLAR,   /**< $FF, $1234 - 6502/Motorola styl. */
    Z80_HEX_H_SUFFIX, /**< FFh, 1234h - Intel/Zilog styl. */
} z80_hex_style_t;

/**
 * @brief Styl renderování indexované adresace IX/IY.
 *
 * Většina assemblerů pro Z80 (pasmo, sjasmplus, z80ex, z88dk-z80asm)
 * používá Zilog konvenci "(IX+d)" / "(IY-d)" - displacement následuje
 * za názvem registru s explicitním znaménkem. Toolchain SDCC sdas-z80
 * (asxxxx rodina) ale dědí Motorola konvenci "(d,IX)" / "(-d,IY)" -
 * znaménkový displacement předchází před názvem registru oddělený
 * čárkou.
 *
 * Rozdíl je čistě syntaktický (oba kódují identický opcode), týká se
 * jen operandů typu @ref Z80_OP_MEM_IX_D a @ref Z80_OP_MEM_IY_D ve
 * formátovači @ref z80_dasm_to_str.
 *
 * - @c Z80_IX_ZILOG: "(IX+5)", "(IY-2)" - mainstream styl, default.
 * - @c Z80_IX_MOTOROLA: "(5,IX)", "(-2,IY)" - sdas/sdcc-asz80 styl.
 */
typedef enum {
    Z80_IX_ZILOG    = 0,  /**< Zilog konvence "(IX+d)" / "(IY-d)". */
    Z80_IX_MOTOROLA = 1,  /**< Motorola/sdas konvence "(d,IX)" / "(-d,IY)". */
} z80_ix_style_t;

/**
 * @brief Konfigurace formatovani vystupu disassembleru.
 *
 * Umoznuje prizpusobit textovy vystup ruznym konvencim a preferencim.
 * Pro inicializaci na vychozi hodnoty pouzijte z80_dasm_format_default().
 */
typedef struct {
    z80_hex_style_t hex_style; /**< Styl hexadecimalnich cisel. Vychozi: Z80_HEX_HASH. */
    int uppercase;             /**< Velka pismena pro mnemoniky a registry.
                                    0: "ld a,b", 1: "LD A,B". Vychozi: 1. */
    int show_bytes;            /**< Zobrazit surove bajty pred mnemonikou.
                                    0: "LD A,#FF", 1: "3E FF  LD A,#FF". Vychozi: 0. */
    int show_addr;             /**< Zobrazit adresu pred instrukci.
                                    0: "LD A,B", 1: "0100  LD A,B". Vychozi: 0. */
    int rel_as_absolute;       /**< Relativni skoky jako absolutni adresy.
                                    0: "JR $+5", 1: "JR #0105". Vychozi: 1. */
    int undoc_ix_style;        /**< Styl nedokumentovanych pul-registru IX/IY.
                                    0: "IXH"/"IXL" (nejbeznejsi),
                                    1: "HX"/"LX" (alternativni),
                                    2: "XH"/"XL" (zkraceny). Vychozi: 0. */
    z80_ix_style_t ix_iy_style; /**< Styl indexované adresace IX/IY.
                                    Z80_IX_ZILOG: "(IX+d)" / "(IY-d)" (default),
                                    Z80_IX_MOTOROLA: "(d,IX)" / "(-d,IY)" pro
                                    sdas/sdcc-asz80. Vychozi: Z80_IX_ZILOG. */
} z80_dasm_format_t;

/**
 * @brief Inicializuje formatovaci strukturu na vychozi hodnoty.
 *
 * Vychozi format je kompatibilni s z80ex_dasm():
 * - hex_style = Z80_HEX_HASH (#FF, #1234)
 * - uppercase = 1 (LD A,B)
 * - show_bytes = 0
 * - show_addr = 0
 * - rel_as_absolute = 1 (JR #1234)
 * - undoc_ix_style = 0 (IXH/IXL)
 * - ix_iy_style = Z80_IX_ZILOG ("(IX+5)" / "(IY-2)")
 *
 * @param[out] fmt Ukazatel na strukturu k inicializaci. Nesmi byt NULL.
 *
 * @pre fmt != NULL
 * @post Vsechny polozky fmt jsou nastaveny na vychozi hodnoty.
 */
void z80_dasm_format_default(z80_dasm_format_t *fmt);

/**
 * @brief Formatuj disassemblovanou instrukci do textoveho bufferu.
 *
 * Prevede strukturu z80_dasm_inst_t na citelny text podle zadane konfigurace
 * formatu. Vystup je vzdy null-terminated.
 *
 * @param[out] buf      Textovy buffer pro vystup. Nesmi byt NULL.
 * @param[in]  buf_size Velikost bufferu v bajtech. Musi byt >= 1.
 *                      Doporucena velikost: 64 bajtu (dostatek pro vsechny
 *                      instrukce vcetne adresy a surovych bajtu).
 * @param[in]  inst     Dekodovana instrukce. Nesmi byt NULL.
 * @param[in]  fmt      Konfigurace formatu. Pokud je NULL, pouzije se
 *                      vychozi format (z80_dasm_format_default).
 * @return Pocet zapsanych znaku (bez null terminatoru), nebo zaporna hodnota
 *         pokud buffer nestaci. V tomto pripade je vystup oriznuta ale vzdy
 *         null-terminated.
 *
 * @pre buf != NULL
 * @pre buf_size >= 1
 * @pre inst != NULL (musi byt vyplnena funkci z80_dasm)
 */
int z80_dasm_to_str(char *buf, int buf_size,
                    const z80_dasm_inst_t *inst,
                    const z80_dasm_format_t *fmt);

/* ======================================================================
 * Tabulka symbolu
 * ====================================================================== */

/**
 * @brief Opaque typ pro tabulku symbolu.
 *
 * Mapuje 16bitove adresy na symbolicky nazvy. Interne implementovana
 * jako serazene pole s binarnim vyhledavanim.
 *
 * Typicke pouziti: pojmenovani ROM rutin, I/O portu a systemovych adres
 * MZ-800 pro citelnejsi vystup debuggeru.
 *
 * @note Tabulka vlastni kopie vsech retezcu - volajici muze uvolnit
 *       originaly po pridani.
 */
typedef struct z80_symtab z80_symtab_t;

/**
 * @brief Vytvori novou prazdnou tabulku symbolu.
 *
 * @return Ukazatel na novou tabulku, nebo NULL pri selhani alokace.
 *
 * @post Vracenou tabulku je nutne uvolnit pomoci z80_symtab_destroy().
 */
z80_symtab_t *z80_symtab_create(void);

/**
 * @brief Zrusi tabulku symbolu a uvolni veskerou pamet.
 *
 * @param[in] tab Tabulka ke zruseni. Muze byt NULL (nic se nestane).
 *
 * @post Po volani je ukazatel neplatny.
 */
void z80_symtab_destroy(z80_symtab_t *tab);

/**
 * @brief Prida symbol do tabulky.
 *
 * Pokud na dane adrese jiz symbol existuje, bude prepsan novym nazvem.
 * Retezec name je interně zkopirovan - volajici muze original uvolnit.
 *
 * @param[in,out] tab  Tabulka symbolu. Nesmi byt NULL.
 * @param[in]     addr Adresa symbolu (0x0000-0xFFFF).
 * @param[in]     name Nazev symbolu. Nesmi byt NULL ani prazdny.
 * @return 0 pri uspechu, -1 pri selhani alokace.
 *
 * @pre tab != NULL
 * @pre name != NULL && name[0] != '\0'
 */
int z80_symtab_add(z80_symtab_t *tab, uint16_t addr, const char *name);

/**
 * @brief Odebere symbol z tabulky.
 *
 * Pokud na dane adrese zadny symbol neexistuje, nic se nestane.
 *
 * @param[in,out] tab  Tabulka symbolu. Nesmi byt NULL.
 * @param[in]     addr Adresa symbolu k odebrani.
 *
 * @pre tab != NULL
 */
void z80_symtab_remove(z80_symtab_t *tab, uint16_t addr);

/**
 * @brief Odebere vsechny symboly z tabulky.
 *
 * @param[in,out] tab Tabulka symbolu. Nesmi byt NULL.
 *
 * @pre tab != NULL
 * @post z80_symtab_count(tab) == 0
 */
void z80_symtab_clear(z80_symtab_t *tab);

/**
 * @brief Vyhledej symbol podle adresy.
 *
 * @param[in] tab  Tabulka symbolu. Nesmi byt NULL.
 * @param[in] addr Adresa k vyhledani.
 * @return Nazev symbolu, nebo NULL pokud na adrese zadny symbol neni.
 *         Vraceny retezec je vlastnen tabulkou - nesmi byt uvolnen
 *         ani modifikovan volanem.
 *
 * @pre tab != NULL
 * @note Vraceny ukazatel je platny dokud neni symbol odebran nebo
 *       tabulka zrusena.
 */
const char *z80_symtab_lookup(const z80_symtab_t *tab, uint16_t addr);

/**
 * @brief Vrati pocet symbolu v tabulce.
 *
 * @param[in] tab Tabulka symbolu. Nesmi byt NULL.
 * @return Pocet symbolu (>= 0).
 *
 * @pre tab != NULL
 */
int z80_symtab_count(const z80_symtab_t *tab);

/**
 * @brief Vysledek rozliseni symbolu pro instrukci.
 *
 * Obsahuje symbolicke nazvy pro adresy pouzite v instrukci.
 * Polozky jsou NULL pokud odpovidajici adresa nema symbol.
 */
typedef struct {
    const char *target_sym;  /**< Symbol cilove adresy pro JP/CALL/JR/RST/DJNZ.
                                  NULL pokud instrukce neni skok/volani,
                                  nebo cilova adresa nema symbol. */
    const char *mem_sym;     /**< Symbol adresy pameti pro LD r,(nn) / LD (nn),r.
                                  NULL pokud instrukce nepristupuje k prime
                                  adrese, nebo adresa nema symbol. */
} z80_dasm_symbols_t;

/**
 * @brief Rozlis symboly pro danou instrukci.
 *
 * Vyhleda v tabulce symboly odpovidajici adresam pouzivanym v instrukci:
 * - Cilova adresa skoku/volani (JP nn, CALL nn, JR e, RST p)
 * - Adresa prime pameti (LD A,(nn), LD (nn),HL, atd.)
 *
 * @param[in]  inst    Dekodovana instrukce. Nesmi byt NULL.
 * @param[in]  symbols Tabulka symbolu. Nesmi byt NULL.
 * @param[out] out     Vysledek rozliseni. Nesmi byt NULL.
 *
 * @pre inst != NULL
 * @pre symbols != NULL
 * @pre out != NULL
 * @post out->target_sym a out->mem_sym jsou bud NULL nebo ukazuji
 *       na retezce vlastnene tabulkou symbolu.
 */
void z80_dasm_resolve_symbols(const z80_dasm_inst_t *inst,
                              const z80_symtab_t *symbols,
                              z80_dasm_symbols_t *out);

/**
 * @brief Formatuj instrukci do textu s podporou symbolu.
 *
 * Funguje jako z80_dasm_to_str(), ale adresy nahrazuje symbolickymi nazvy
 * pokud jsou v tabulce nalezeny.
 *
 * Priklad: "CALL #00AD" -> "CALL rom_getchar" (pokud je symbol definovan).
 *
 * @param[out] buf      Textovy buffer pro vystup. Nesmi byt NULL.
 * @param[in]  buf_size Velikost bufferu v bajtech. Musi byt >= 1.
 * @param[in]  inst     Dekodovana instrukce. Nesmi byt NULL.
 * @param[in]  fmt      Konfigurace formatu. NULL = vychozi format.
 * @param[in]  symbols  Tabulka symbolu. Pokud je NULL, chova se
 *                      stejne jako z80_dasm_to_str().
 * @return Pocet zapsanych znaku (bez null terminatoru), nebo zaporna
 *         hodnota pokud buffer nestaci.
 *
 * @pre buf != NULL
 * @pre buf_size >= 1
 * @pre inst != NULL
 */
int z80_dasm_to_str_sym(char *buf, int buf_size,
                        const z80_dasm_inst_t *inst,
                        const z80_dasm_format_t *fmt,
                        const z80_symtab_t *symbols);

/* ======================================================================
 * Analyza toku rizeni
 * ====================================================================== */

/**
 * @brief Zjisti cilovou adresu instrukce skoku nebo volani.
 *
 * Pro instrukce, ktere meni tok rizeni (JP, JR, CALL, RST, DJNZ),
 * vraci cilovou adresu. Pro nepodminecne i podminecne varianty.
 *
 * Specialni pripady:
 * - JP (HL), JP (IX), JP (IY): vraci (uint16_t)-1, protoze cil zavisi
 *   na aktualnim stavu registru.
 * - RET, RET cc, RETI, RETN: vraci (uint16_t)-1 (cil je na zasobniku).
 * - Bezne instrukce (ne skok/volani): vraci (uint16_t)-1.
 *
 * @param[in] inst Dekodovana instrukce. Nesmi byt NULL.
 * @return Cilova adresa, nebo (uint16_t)-1 pokud cil neni staticky znamy.
 *
 * @pre inst != NULL
 */
uint16_t z80_dasm_target_addr(const z80_dasm_inst_t *inst);

/**
 * @brief Vyhodnot podminku vetveni podle aktualniho stavu flagu.
 *
 * Pro podminecne instrukce (JP cc, JR cc, CALL cc, RET cc) vyhodnoti,
 * zda by se skok provedl pri danych hodnotach registru flagu.
 *
 * Uzitecne v debuggeru pro vizualizaci: zvyrazneni zda se skok provede
 * (zelena/cervena sipka).
 *
 * @param[in] inst  Dekodovana instrukce. Nesmi byt NULL.
 * @param[in] flags Aktualni hodnota registru F procesoru Z80.
 * @return 1 pokud by se skok provedl, 0 pokud ne.
 *         Pro bezpodminecne instrukce (JP, JR, CALL, RST) vzdy 1.
 *         Pro nevetvi instrukce (LD, ADD, ...) vzdy 0.
 *
 * @pre inst != NULL
 */
int z80_dasm_branch_taken(const z80_dasm_inst_t *inst, uint8_t flags);

/* ======================================================================
 * Pristup k registrum a flagum (pohodlne obaly)
 * ====================================================================== */

/**
 * @brief Vrati bitovou masku registru, ktere instrukce cte.
 *
 * Ekvivalentni primo cteni inst->regs_read, ale poskytuje explicitni
 * rozhrani pro zapouzdreni.
 *
 * @param[in] inst Dekodovana instrukce. Nesmi byt NULL.
 * @return Bitova maska ctenych registru (z80_dasm_reg_mask_t hodnoty).
 *
 * @pre inst != NULL
 */
uint16_t z80_dasm_regs_read(const z80_dasm_inst_t *inst);

/**
 * @brief Vrati bitovou masku registru, ktere instrukce zapisuje.
 *
 * @param[in] inst Dekodovana instrukce. Nesmi byt NULL.
 * @return Bitova maska zapisovanych registru (z80_dasm_reg_mask_t hodnoty).
 *
 * @pre inst != NULL
 */
uint16_t z80_dasm_regs_written(const z80_dasm_inst_t *inst);

/**
 * @brief Vrati bitovou masku flagu, ktere instrukce ovlivnuje.
 *
 * @param[in] inst Dekodovana instrukce. Nesmi byt NULL.
 * @return Bitova maska ovlivnenych flagu (Z80_FLAG_* konstanty).
 *
 * @pre inst != NULL
 */
uint8_t z80_dasm_flags_affected(const z80_dasm_inst_t *inst);

/* ======================================================================
 * Konverze relativnich adres
 * ====================================================================== */

/**
 * @brief Prevede relativni offset na absolutni adresu.
 *
 * Vypocita cilovou adresu instrukce JR nebo DJNZ.
 * Vzorec: addr + 2 + offset (instrukce JR/DJNZ ma delku 2 bajty,
 * offset se pocita od konce instrukce).
 *
 * Priklady:
 * - z80_rel_to_abs(0x0100, +3)  -> 0x0105
 * - z80_rel_to_abs(0x0100, -2)  -> 0x0100 (skok sam na sebe)
 * - z80_rel_to_abs(0x0100, 0)   -> 0x0102
 * - z80_rel_to_abs(0xFFFE, +1)  -> 0x0001 (wraparound)
 *
 * @param[in] addr   Adresa instrukce JR/DJNZ (kde lezi opcode).
 * @param[in] offset Relativni offset (-128 az +127).
 * @return Absolutni cilova adresa (16bitova, s wraparound).
 */
uint16_t z80_rel_to_abs(uint16_t addr, int8_t offset);

/**
 * @brief Prevede absolutni adresu na relativni offset.
 *
 * Spocita offset pro instrukci JR/DJNZ na dane adrese, aby skocila
 * na cilovou adresu. Platny rozsah cile: addr-126 az addr+129
 * (po odecteni delky instrukce: offset -128 az +127).
 *
 * Uzitecne pri patchovani kodu nebo pri rucnim sestavovani instrukci.
 *
 * @param[in]  addr   Adresa instrukce JR/DJNZ (kde bude lezet opcode).
 * @param[in]  target Pozadovana cilova adresa.
 * @param[out] offset Vypocteny offset. Nesmi byt NULL.
 * @return 0 pri uspechu (offset ulozen do *offset),
 *         -1 pokud cil je mimo dosah.
 *
 * @pre offset != NULL
 * @post Pri navratu 0: z80_rel_to_abs(addr, *offset) == target.
 * @post Pri navratu -1: *offset neni modifikovan.
 */
int z80_abs_to_rel(uint16_t addr, uint16_t target, int8_t *offset);

/* ======================================================================
 * z80ex kompatibilni API
 * ====================================================================== */

/**
 * @defgroup z80ex_compat Zpetna kompatibilita s z80ex_dasm
 *
 * Drop-in nahrada za puvodni z80ex_dasm() funkci.
 * Interne pouziva z80_dasm() + z80_dasm_to_str() s vychozim formatem.
 *
 * Typy a funkce zachovavaji originalni pojmenovani a signaturu
 * pro bezbolestny prechod z z80ex.
 * @{
 */

/* Typy obalene guardy proti z80ex_common.h, ktery je stale
   includovany pres z80ex.h (CPU emulator). */
#ifndef _Z80EX_COMMON_H_INCLUDED
/** Kompatibilni typ: 8bitovy bezznamenkovy bajt. */
typedef uint8_t  Z80EX_BYTE;
/** Kompatibilni typ: 8bitovy znamenkovy bajt. */
typedef int8_t  Z80EX_SIGNED_BYTE;
/** Kompatibilni typ: 16bitove bezznamenkove slovo. */
typedef uint16_t Z80EX_WORD;
#endif

/* Callback guard proti puvodnimu z80ex_dasm.h */
#ifndef _Z80EX_DASM_H_INCLUDED
/**
 * @brief Kompatibilni callback pro cteni bajtu (z80ex signatura).
 *
 * @param[in] addr      Adresa bajtu ke cteni.
 * @param[in] user_data Uzivatelska data.
 * @return Hodnota bajtu na dane adrese.
 */
typedef Z80EX_BYTE (*z80ex_dasm_readbyte_cb)(Z80EX_WORD addr, void *user_data);
#endif

/**
 * @brief Formatovaci priznak: slova jako desitkova cisla.
 *
 * Misto "#1234" vystup "4660".
 */
#define Z80EX_DASM_WORDS_DEC 1

/**
 * @brief Formatovaci priznak: bajty jako desitkova cisla.
 *
 * Misto "#FF" vystup "255".
 */
#define Z80EX_DASM_BYTES_DEC 2

/**
 * @brief Disassembluj jednu instrukci (z80ex kompatibilni signatura).
 *
 * Drop-in nahrada za puvodni z80ex_dasm(). Vystup je formatovan identicky
 * s puvodnim z80ex pro zajisteni zpetne kompatibility.
 *
 * @param[out] output       Textovy buffer pro vystup. Nesmi byt NULL.
 * @param[in]  output_size  Velikost bufferu v bajtech.
 * @param[in]  flags        Formatovaci priznak (Z80EX_DASM_WORDS_DEC,
 *                          Z80EX_DASM_BYTES_DEC, nebo 0 pro vychozi hex).
 * @param[out] t_states     Pocet T-stavu instrukce. Nesmi byt NULL.
 * @param[out] t_states2    T-stavy pri vetveni (0 = neni vetveni). Nesmi byt NULL.
 * @param[in]  readbyte_cb  Callback pro cteni bajtu. Nesmi byt NULL.
 * @param[in]  addr         Adresa prvniho bajtu instrukce.
 * @param[in]  user_data    Uzivatelska data predana do readbyte_cb.
 * @return Delka instrukce v bajtech.
 *
 * @pre output != NULL
 * @pre t_states != NULL
 * @pre t_states2 != NULL
 * @pre readbyte_cb != NULL
 */
int z80ex_dasm(char *output, int output_size, unsigned flags,
               int *t_states, int *t_states2,
               z80ex_dasm_readbyte_cb readbyte_cb,
               Z80EX_WORD addr, void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* Z80_DASM_H */
