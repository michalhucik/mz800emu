/*
 * dbg_inline_asm.cpp — Modální dialog Inline Assembleru
 *
 * PŘEHLED
 * =======
 * Modální popup dialog uvnitř debugger okna pro editaci Z80 instrukcí.
 * Umožňuje zadat adresu a instrukci, zkompilovat ji a zapsat do paměti.
 *
 * LAYOUT (dle starého screenshotu)
 * ================================
 *
 *  ┌─── inLine Assembler ──────────────── X ─┐
 *  │  Addr:        Enter Z80 Instruction      │
 *  │               to Assemble:               │
 *  │  [0012]       [________________________] │
 *  │  ─────────────────────────────────────── │
 *  │  [Help]                 [Cancel] [Apply] │
 *  └─────────────────────────────────────────┘
 *
 * HELP OKNO
 * =========
 * Modální popup vnořený uvnitř IASM dialogu. Zavírá se křížkem v titulku
 * okna (ESC ho nezavře).
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

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "debugger/debugger.h"
#include "debugger/inline_asm.h"
#include "debugger/symbols/sym_db.h"

#include "dbg_inline_asm.h"
#include "../debugger_state.h"
#include "../dbgapi_helpers.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>


/*
 * Lokální stav inline assembler dialogu.
 */
static struct {
    bool open;                 /* Má se otevřít popup (one-shot) */
    bool just_opened;          /* One-shot: nastavit focus na instrukční pole */
    uint16_t addr;             /* Aktuální adresa */
    char addr_buf[5];          /* "0012" + '\0' */
    char instr_buf[IASM_MNEMONICS_MAXLEN + 1]; /* Textové pole instrukce */
    bool select_all_instr;     /* One-shot: AutoSelectAll na instrukčním poli */
} s_iasm;

/* Zpožděný přesun focusu z addr do instrukce (Enter v addr) */
static bool s_pending_focus_instr = false;

/* Stav help popupu */
static bool s_help_open = false;
static bool s_help_should_stay = false;

/* Pozice kurzoru v okamžiku otevření IASM dialogu - dialog se zobrazí
 * blízko klikajícího řádku (= ne na zapamatované pozici z minula).
 * Stejný pattern jako dbg_focus_to. */
static ImVec2 s_iasm_open_mouse_pos = ImVec2(0.0f, 0.0f);
static bool s_iasm_have_open_mouse_pos = false;

/* Pozice kurzoru pro IASM Help dialog - zachycená při OpenPopup. */
static ImVec2 s_iasm_help_open_mouse_pos = ImVec2(0.0f, 0.0f);
static bool s_iasm_help_have_open_mouse_pos = false;


static void update_addr_buf(uint16_t addr)
{
    s_iasm.addr = addr;
    snprintf(s_iasm.addr_buf, sizeof(s_iasm.addr_buf), "%04X", addr);
}


/*
 * Z80 registry, podmínky, mnemoniky a další rezervovaná slova IASM,
 * která NESMĚJÍ být přepsána symbolem stejného jména. Porovnání je
 * case-insensitive (text instrukce IASM normalizuje na uppercase).
 *
 * Účel: pokud uživatel definuje user-label se stejným jménem jako registr
 * (např. label "A"), preserveme registr - parser pak instrukci přeloží
 * korektně. Symbol s tímto jménem je v IASM kontextu prakticky nepoužitelný,
 * což akceptujeme (= konflikt vyřeší přejmenováním labelu).
 */
static const char *const IASM_RESERVED_TOKENS[] = {
    /* 8b registry */
    "A", "B", "C", "D", "E", "H", "L", "F", "I", "R",
    /* 16b registry + alternativy */
    "AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC",
    "IXH", "IXL", "IYH", "IYL", "HX", "LX", "XH", "XL", "HY", "LY", "YH", "YL",
    /* podmínky (NZ/Z/NC/C/PO/PE/P/M) - C/Z se kryjí s registry/flag, OK */
    "NZ", "NC", "PO", "PE", "M", "P",
    /* mnemoniky a operandy které IASM bere jako literál */
    "LD", "ADD", "ADC", "SUB", "SBC", "AND", "OR", "XOR", "CP", "INC", "DEC",
    "JP", "JR", "CALL", "RET", "RETI", "RETN", "RST", "DJNZ",
    "PUSH", "POP", "EX", "EXX", "LDI", "LDIR", "LDD", "LDDR",
    "CPI", "CPIR", "CPD", "CPDR", "OUTI", "OTIR", "OUTD", "OTDR",
    "INI", "INIR", "IND", "INDR", "DAA", "CPL", "NEG", "CCF", "SCF",
    "NOP", "HALT", "DI", "EI", "IM", "IN", "OUT",
    "RLCA", "RRCA", "RLA", "RRA", "RLD", "RRD",
    "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SL1", "SRL",
    "BIT", "SET", "RES",
    NULL
};


/**
 * @brief Vrátí true pokud token je rezervované IASM slovo (case-insensitive).
 */
static bool iasm_is_reserved_token(const char *token, size_t len)
{
    for (int i = 0; IASM_RESERVED_TOKENS[i] != NULL; i++)
    {
        const char *r = IASM_RESERVED_TOKENS[i];
        if (strlen(r) != len) continue;
        bool match = true;
        for (size_t k = 0; k < len; k++)
        {
            char a = (char)toupper((unsigned char)token[k]);
            char b = r[k];
            if (a != b) { match = false; break; }
        };
        if (match) return true;
    };
    return false;
}


/**
 * @brief Pre-pass: nahradí symbolické identifikátory jejich hex hodnotou.
 *
 * Projede vstupní text a pro každý souvislý token tvořený písmeny / číslicemi
 * / underscore zkusí lookup v sym_db (case-sensitive, mimo rezervovaná slova).
 * Pokud najde, nahradí token za "#XXXX" (hash hex prefix - IASM ho parsuje
 * jako hex literál).
 *
 * Adresa symbolu se ořeže na 16 bitů (sym_db může držet 32-bit physical
 * adresy ve V1.5 - pro inline assembler IASM má 16 bit).
 *
 * Výstup se zapíše do out_buf. Pokud se nevejde, vrátí false (bez chyby
 * volání pokračuje s původním textem). Pokud žádný symbol nebyl nahrazen,
 * out_buf obsahuje kopii src.
 *
 * @param[in]  src     vstupní text (např. "CALL MAIN_LOOP")
 * @param[out] out_buf cílový buffer (alespoň out_size bajtů)
 * @param[in]  out_size velikost out_buf
 * @return počet provedených substitucí (>= 0), nebo -1 při chybě bufferu
 */
static int iasm_preprocess_symbols(const char *src, char *out_buf, size_t out_size)
{
    if (!src || !out_buf || out_size == 0) return -1;
    size_t out_pos = 0;
    int substitutions = 0;
    size_t i = 0;
    size_t src_len = strlen(src);

    while (i < src_len)
    {
        unsigned char c = (unsigned char)src[i];

        /* Začátek tokenu identifikátoru: písmeno nebo '_' */
        if (isalpha(c) || c == '_')
        {
            size_t start = i;
            while (i < src_len)
            {
                unsigned char cc = (unsigned char)src[i];
                if (isalnum(cc) || cc == '_') i++;
                else break;
            };
            size_t tok_len = i - start;

            /* Není rezervované slovo? Zkusit sym_db. */
            if (!iasm_is_reserved_token(&src[start], tok_len))
            {
                /* Vytvoříme NUL-terminated kopii tokenu pro lookup. */
                char token[64];
                if (tok_len < sizeof(token))
                {
                    memcpy(token, &src[start], tok_len);
                    token[tok_len] = '\0';
                    const st_SYMBOL *sym = sym_db_lookup_by_name(token);
                    if (sym)
                    {
                        /* Nahrazení tokenu za "#XXXX" (4-hex 16-bit). */
                        char hexbuf[8];
                        snprintf(hexbuf, sizeof(hexbuf), "#%04X",
                                 (unsigned)(sym->addr & 0xFFFF));
                        size_t hex_len = strlen(hexbuf);
                        if (out_pos + hex_len + 1 > out_size) return -1;
                        memcpy(&out_buf[out_pos], hexbuf, hex_len);
                        out_pos += hex_len;
                        substitutions++;
                        continue;
                    };
                };
            };

            /* Token nebyl nahrazen - zkopírujeme jak je. */
            if (out_pos + tok_len + 1 > out_size) return -1;
            memcpy(&out_buf[out_pos], &src[start], tok_len);
            out_pos += tok_len;
        }
        else
        {
            /* Ostatní znaky (mezery, oddělovače, číslice mimo identifier) */
            if (out_pos + 2 > out_size) return -1;
            out_buf[out_pos++] = src[i];
            i++;
        };
    };

    if (out_pos + 1 > out_size) return -1;
    out_buf[out_pos] = '\0';
    return substitutions;
}


/*
 * apply_instruction — kompilace a zápis instrukce do paměti.
 * Vrací true při úspěchu (advance), false při chybě (dialog zůstane).
 *
 * Před vlastní kompilací proběhne pre-pass přes sym_db: každý identifikátor
 * který odpovídá symbolu se nahradí jeho 16-bit hex hodnotou (#XXXX).
 * Tím IASM library vidí už standardní hex literál a zbytek parseru
 * (registry, oddělovače, instrukční šablony) zůstává beze změny.
 */
static bool apply_instruction(void)
{
    uint16_t addr = (uint16_t)debuger_hextext_to_uint32(s_iasm.addr_buf);

    /* Pre-pass: substituce symbolů. Pokud cíl nepasuje do bufferu (= velmi
     * dlouhé jméno), fallback na původní text - IASM error message bude
     * srozumitelný (Invalid instruction). */
    char preprocessed[IASM_MNEMONICS_MAXLEN * 2 + 1];
    const char *to_assemble = s_iasm.instr_buf;
    if (iasm_preprocess_symbols(s_iasm.instr_buf, preprocessed,
                                 sizeof(preprocessed)) >= 0)
    {
        to_assemble = preprocessed;
    };

    st_IASMBIN compiled;
    unsigned num_bytes = debugger_iasm_assemble_line(addr, to_assemble, &compiled);

    if (num_bytes == 0)
        return false;

    /* Bloková zápis přes dbgapi CMD_MEM_WRITE - dispatcher přepošle
     * na debugger_memory_write_byte() v EMU vlákně, takže banking +
     * CDL flag jsou zachované. Auto refresh on edit + VRAM touch
     * detekce probíhá uvnitř debugger_memory_write_byte(). */
    dbg_ui_mem_write((uint16_t)addr, compiled.byte, (uint16_t)num_bytes);

    dbg_refresh_request();

    /* Advance */
    update_addr_buf((uint16_t)(addr + num_bytes));
    s_iasm.instr_buf[0] = '\0';
    s_iasm.just_opened = true;
    s_iasm.select_all_instr = false;

    return true;
}


/*
 * render_help_modal — modální help popup vnořený uvnitř IASM dialogu.
 *
 * ESC NEZAVÍRÁ toto okno — zavření je možné pouze křížkem v titulku.
 * Implementace: pokud se popup zavře přes ESC, okamžitě ho znovu otevřeme.
 * Křížek nastaví s_help_should_stay = false, což re-open zablokuje.
 */
static void render_help_modal(void)
{
    /* Otevření (z Help tlačítka) */
    if (s_help_open)
    {
        ImGui::OpenPopup("Z80 Inline Assembler Help");
        s_help_open = false;
        s_help_should_stay = true;
    };

    /* Blokování zavření přes ESC — pokud se popup zavřel jinak než křížkem, znovu otevřít */
    if (s_help_should_stay && !ImGui::IsPopupOpen("Z80 Inline Assembler Help"))
    {
        ImGui::OpenPopup("Z80 Inline Assembler Help");
    };

    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);

    /* Pozice dialogu při Appearing = u kurzoru. */
    if (s_iasm_help_have_open_mouse_pos)
    {
        ImGui::SetNextWindowPos(s_iasm_help_open_mouse_pos, ImGuiCond_Appearing);
        s_iasm_help_have_open_mouse_pos = false;
    };

    if (!ImGui::BeginPopupModal("Z80 Inline Assembler Help", &s_help_should_stay,
                                 ImGuiWindowFlags_NoSavedSettings))
    {
        return;
    };

    /*
     * Celý obsah v scrollovatelném child okně.
     * Bez Close tlačítka — celý prostor pro obsah.
     */
    if (ImGui::BeginChild("##iasm_help_content", ImVec2(0, 0),
                           ImGuiChildFlags_None, ImGuiWindowFlags_None))
    {
        /* === Line Assembler — formáty čísel === */
        ImGui::SeparatorText("Line Assembler");
        ImGui::Spacing();

        ImGui::TextWrapped(
            "Line assembler accepts several ways to enter numeric values "
            "in the different numerical bases designated by the prefix:"
        );

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Indent(40.0f);
        ImGui::Text("0x...  - hex values ( 0x0, 0x1234, 0xDEad )");
        ImGui::Text("#...   - hex values ( #0, #12aB, #beeF )");
        ImGui::Text("%%...   - bin values ( %%0, %%1010, %%11111111 )");
        ImGui::Unindent(40.0f);
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::TextWrapped(
            "Variables without a prefix are automatically considered as a decimal "
            "numbers, however if immediately after the number is followed "
            "another character of 'A' to 'F', so variable is understood "
            "as a hexadecimal number."
        );

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        /* === Supported Z80 instructions === */
        ImGui::SeparatorText("Supported Z80 instructions");
        ImGui::Spacing();

        ImGui::TextWrapped(
            "If any instruction requires some variables, so these are "
            "labeled in the optocode by a special character:"
        );

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Indent(40.0f);
        ImGui::Text("# - variable is 8 bit value 0 ... 255");
        ImGui::Text("@ - variable is 16 bit value 0 ... 65535");
        ImGui::Text("$ - variable is 8 bit signed value -128 ... +127");
        ImGui::Text("%% - variable is a 16 bit address, that the assembler");
        ImGui::Text("    converts to relative from the current row");
        ImGui::Unindent(40.0f);

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* === Kompletní abecední seznam Z80 instrukcí === */
        static const char *const instructions[] = {
            "A:", "",
            "  ADC A,(HL)", "  ADC A,A", "  ADC A,B", "  ADC A,C",
            "  ADC A,D", "  ADC A,E", "  ADC A,H", "  ADC A,IXH",
            "  ADC A,IXL", "  ADC A,IYH", "  ADC A,IYL", "  ADC A,L",
            "  ADC HL,BC", "  ADC HL,DE", "  ADC HL,HL", "  ADC HL,SP",
            "  ADC A,#", "  ADC A,(IX+$)", "  ADC A,(IY+$)",
            "  ADD A,(HL)", "  ADD A,A", "  ADD A,B", "  ADD A,C",
            "  ADD A,D", "  ADD A,E", "  ADD A,H", "  ADD A,IXH",
            "  ADD A,IXL", "  ADD A,IYH", "  ADD A,IYL", "  ADD A,L",
            "  ADD HL,BC", "  ADD HL,DE", "  ADD HL,HL", "  ADD HL,SP",
            "  ADD IX,BC", "  ADD IX,DE", "  ADD IX,IX", "  ADD IX,SP",
            "  ADD IY,BC", "  ADD IY,DE", "  ADD IY,IY", "  ADD IY,SP",
            "  ADD A,#", "  ADD A,(IX+$)", "  ADD A,(IY+$)",
            "  AND (HL)", "  AND A", "  AND B", "  AND C", "  AND D",
            "  AND E", "  AND H", "  AND IXH", "  AND IXL",
            "  AND IYH", "  AND IYL", "  AND L",
            "  AND #", "  AND (IX+$)", "  AND (IY+$)",
            "",
            "B:", "",
            "  BIT 0..7,r", "  BIT 0..7,(HL)",
            "  BIT 0..7,(IX+$)", "  BIT 0..7,(IY+$)",
            "",
            "C:", "",
            "  CALL @", "  CALL C,@", "  CALL M,@", "  CALL NC,@",
            "  CALL NZ,@", "  CALL P,@", "  CALL PE,@", "  CALL PO,@",
            "  CALL Z,@",
            "  CCF",
            "  CP (HL)", "  CP A", "  CP B", "  CP C", "  CP D",
            "  CP E", "  CP H", "  CP IXH", "  CP IXL",
            "  CP IYH", "  CP IYL", "  CP L",
            "  CP #", "  CP (IX+$)", "  CP (IY+$)",
            "  CPD", "  CPDR", "  CPI", "  CPIR", "  CPL",
            "",
            "D:", "",
            "  DAA",
            "  DEC (HL)", "  DEC A", "  DEC B", "  DEC BC", "  DEC C",
            "  DEC D", "  DEC DE", "  DEC E", "  DEC H", "  DEC HL",
            "  DEC IX", "  DEC IXH", "  DEC IXL",
            "  DEC IY", "  DEC IYH", "  DEC IYL",
            "  DEC L", "  DEC SP",
            "  DEC (IX+$)", "  DEC (IY+$)",
            "  DI", "  DJNZ %",
            "",
            "E:", "",
            "  EI",
            "  EX AF,AF'", "  EX DE,HL", "  EX (SP),HL",
            "  EX (SP),IX", "  EX (SP),IY",
            "  EXX",
            "",
            "H:", "",
            "  HALT",
            "",
            "I:", "",
            "  IM 0", "  IM 1", "  IM 2",
            "  IN A,(#)", "  IN B,(C)", "  IN C,(C)", "  IN D,(C)",
            "  IN E,(C)", "  IN H,(C)", "  IN L,(C)",
            "  INC (HL)", "  INC A", "  INC B", "  INC BC", "  INC C",
            "  INC D", "  INC DE", "  INC E", "  INC H", "  INC HL",
            "  INC IX", "  INC IXH", "  INC IXL",
            "  INC IY", "  INC IYH", "  INC IYL",
            "  INC L", "  INC SP",
            "  INC (IX+$)", "  INC (IY+$)",
            "  IND", "  INDR", "  INI", "  INIR",
            "",
            "J:", "",
            "  JP @", "  JP C,@", "  JP M,@", "  JP NC,@",
            "  JP NZ,@", "  JP P,@", "  JP PE,@", "  JP PO,@",
            "  JP Z,@",
            "  JP (HL)", "  JP (IX)", "  JP (IY)",
            "  JR %", "  JR C,%", "  JR NC,%", "  JR NZ,%", "  JR Z,%",
            "",
            "L:", "",
            "  LD r,r", "  LD r,#", "  LD r,(HL)",
            "  LD r,(IX+$)", "  LD r,(IY+$)",
            "  LD (HL),r", "  LD (HL),#",
            "  LD (IX+$),r", "  LD (IX+$),#",
            "  LD (IY+$),r", "  LD (IY+$),#",
            "  LD A,(BC)", "  LD A,(DE)", "  LD A,(@)",
            "  LD (BC),A", "  LD (DE),A", "  LD (@),A",
            "  LD rr,@", "  LD IX,@", "  LD IY,@",
            "  LD SP,HL", "  LD SP,IX", "  LD SP,IY",
            "  LD (@),rr", "  LD rr,(@)",
            "  LD (@),IX", "  LD IX,(@)",
            "  LD (@),IY", "  LD IY,(@)",
            "  LD A,I", "  LD A,R", "  LD I,A", "  LD R,A",
            "  LDD", "  LDDR", "  LDI", "  LDIR",
            "",
            "N:", "",
            "  NEG", "  NOP",
            "",
            "O:", "",
            "  OR (HL)", "  OR A", "  OR B", "  OR C", "  OR D",
            "  OR E", "  OR H", "  OR IXH", "  OR IXL",
            "  OR IYH", "  OR IYL", "  OR L",
            "  OR #", "  OR (IX+$)", "  OR (IY+$)",
            "  OTDR", "  OTIR",
            "  OUT (#),A", "  OUT (C),A", "  OUT (C),B", "  OUT (C),C",
            "  OUT (C),D", "  OUT (C),E", "  OUT (C),H", "  OUT (C),L",
            "  OUTD", "  OUTI",
            "",
            "P:", "",
            "  POP AF", "  POP BC", "  POP DE", "  POP HL",
            "  POP IX", "  POP IY",
            "  PUSH AF", "  PUSH BC", "  PUSH DE", "  PUSH HL",
            "  PUSH IX", "  PUSH IY",
            "",
            "R:", "",
            "  RES 0..7,r", "  RES 0..7,(HL)",
            "  RES 0..7,(IX+$)", "  RES 0..7,(IY+$)",
            "  RET", "  RET C", "  RET M", "  RET NC", "  RET NZ",
            "  RET P", "  RET PE", "  RET PO", "  RET Z",
            "  RETI", "  RETN",
            "  RL (HL)", "  RL A", "  RL B", "  RL C", "  RL D",
            "  RL E", "  RL H", "  RL L",
            "  RL (IX+$)", "  RL (IY+$)",
            "  RLA",
            "  RLC (HL)", "  RLC A", "  RLC B", "  RLC C", "  RLC D",
            "  RLC E", "  RLC H", "  RLC L",
            "  RLC (IX+$)", "  RLC (IY+$)",
            "  RLCA", "  RLD",
            "  RR (HL)", "  RR A", "  RR B", "  RR C", "  RR D",
            "  RR E", "  RR H", "  RR L",
            "  RR (IX+$)", "  RR (IY+$)",
            "  RRA",
            "  RRC (HL)", "  RRC A", "  RRC B", "  RRC C", "  RRC D",
            "  RRC E", "  RRC H", "  RRC L",
            "  RRC (IX+$)", "  RRC (IY+$)",
            "  RRCA", "  RRD",
            "  RST 0", "  RST 8", "  RST #10", "  RST #18",
            "  RST #20", "  RST #28", "  RST #30", "  RST #38",
            "",
            "S:", "",
            "  SBC A,(HL)", "  SBC A,A", "  SBC A,B", "  SBC A,C",
            "  SBC A,D", "  SBC A,E", "  SBC A,H", "  SBC A,L",
            "  SBC HL,BC", "  SBC HL,DE", "  SBC HL,HL", "  SBC HL,SP",
            "  SBC A,#", "  SBC A,(IX+$)", "  SBC A,(IY+$)",
            "  SCF",
            "  SET 0..7,r", "  SET 0..7,(HL)",
            "  SET 0..7,(IX+$)", "  SET 0..7,(IY+$)",
            "  SLA (HL)", "  SLA A", "  SLA B", "  SLA C", "  SLA D",
            "  SLA E", "  SLA H", "  SLA L",
            "  SLA (IX+$)", "  SLA (IY+$)",
            "  SRA (HL)", "  SRA A", "  SRA B", "  SRA C", "  SRA D",
            "  SRA E", "  SRA H", "  SRA L",
            "  SRA (IX+$)", "  SRA (IY+$)",
            "  SRL (HL)", "  SRL A", "  SRL B", "  SRL C", "  SRL D",
            "  SRL E", "  SRL H", "  SRL L",
            "  SRL (IX+$)", "  SRL (IY+$)",
            "  SUB (HL)", "  SUB A", "  SUB B", "  SUB C", "  SUB D",
            "  SUB E", "  SUB H", "  SUB L",
            "  SUB #", "  SUB (IX+$)", "  SUB (IY+$)",
            "",
            "X:", "",
            "  XOR (HL)", "  XOR A", "  XOR B", "  XOR C", "  XOR D",
            "  XOR E", "  XOR H", "  XOR L",
            "  XOR #", "  XOR (IX+$)", "  XOR (IY+$)",
        };

        for (int i = 0; i < IM_ARRAYSIZE(instructions); i++)
        {
            const char *line = instructions[i];

            /* Prázdný řádek → vertikální mezera mezi sekcemi */
            if (line[0] == '\0')
            {
                ImGui::Spacing();
                continue;
            };

            /* Písmeno sekce (A:, B:, ...) — zvýrazněné barvou */
            if (line[0] != ' ' && line[1] == ':')
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", line);
            }
            else
            {
                ImGui::TextUnformatted(line);
            };
        };
    };
    ImGui::EndChild();

    ImGui::EndPopup();
}


void dbg_iasm_open(uint16_t addr, const char *mnemonic, char first_char, IasmOpenMode mode)
{
    s_iasm.open = true;
    s_iasm.just_opened = true;
    s_pending_focus_instr = false;
    update_addr_buf(addr);

    /* Zachytíme pozici myši - dialog se zobrazí u kurzoru. */
    s_iasm_open_mouse_pos = ImGui::GetMousePos();
    s_iasm_have_open_mouse_pos = true;

    switch (mode)
    {
        case IASM_OPEN_DOUBLECLICK:
            if (mnemonic && mnemonic[0] != '\0')
            {
                strncpy(s_iasm.instr_buf, mnemonic, IASM_MNEMONICS_MAXLEN);
                s_iasm.instr_buf[IASM_MNEMONICS_MAXLEN] = '\0';
            }
            else
            {
                s_iasm.instr_buf[0] = '\0';
            };
            s_iasm.select_all_instr = true;
            break;

        case IASM_OPEN_TYPING:
            s_iasm.instr_buf[0] = first_char;
            s_iasm.instr_buf[1] = '\0';
            s_iasm.select_all_instr = false;
            break;
    };
}


void dbg_iasm_render(void)
{
    if (s_iasm.open)
    {
        ImGui::OpenPopup(_L("inLine Assembler##iasm"));
        s_iasm.open = false;
    };

    /* Pozice dialogu při Appearing = u kurzoru zachycený v open(). */
    if (s_iasm_have_open_mouse_pos)
    {
        ImGui::SetNextWindowPos(s_iasm_open_mouse_pos, ImGuiCond_Appearing);
        s_iasm_have_open_mouse_pos = false;
    };

    /*
     * AlwaysAutoResize — dialog se přizpůsobí obsahu.
     * Všechny šířky prvků jsou fixní, žádný -FLT_MIN ani GetContentRegionAvail
     * pro sizing → žádná zpětnovazební smyčka.
     */
    static bool s_iasm_popup_open = true;
    s_iasm_popup_open = true;
    if (!ImGui::BeginPopupModal(_L("inLine Assembler##iasm"), &s_iasm_popup_open,
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return;
    };

    if (!s_iasm_popup_open)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    };

    /* ESC → zavřít jen tento popup (ne rodičovské okno debuggeru).
     * Zpracováváme jen pokud NENÍ otevřený vnořený help popup. */
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)
        && !ImGui::IsPopupOpen("Z80 Inline Assembler Help"))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    };

    /*
     * Layout:
     *   Addr:          Enter Z80 Instruction to Assemble:
     *   [0012]         [__________________________________]
     *   ──────────────────────────────────────────────────
     *   [Help]                          [Cancel]  [Apply]
     *
     * Addr input šířka 70px (55 * 1.25), mezera 50px, instruction vyplní zbytek.
     */
    float addr_input_w = 70.0f;
    float gap = 50.0f;
    float instr_x = addr_input_w + gap;

    /* Zachycení pending focus z předchozího framu (Enter v addr) */
    bool do_focus_instr = s_pending_focus_instr;
    s_pending_focus_instr = false;

    /* Řádek 1: labels */
    ImGui::Text("Addr:");
    ImGui::SameLine(instr_x);
    ImGui::Text("%s", _("Enter Z80 Instruction to Assemble:"));

    /* Řádek 2: inputy */

    /* Addr input — Enter přesune focus do instrukčního pole */
    ImGui::SetNextItemWidth(addr_input_w);
    bool addr_enter = ImGui::InputText("##iasm_addr", s_iasm.addr_buf, sizeof(s_iasm.addr_buf),
                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase
                     | ImGuiInputTextFlags_EnterReturnsTrue);

    if (addr_enter)
    {
        /* Zpožděný přesun focusu — zpracuje se PŘÍŠTÍ frame,
         * aby Enter z addr nespustil Apply v instrukčním poli */
        s_pending_focus_instr = true;
    };

    ImGui::SameLine(instr_x);

    /* Focus management — nastavení focusu na instrukční pole */
    if (s_iasm.just_opened)
    {
        ImGui::SetKeyboardFocusHere();
        s_iasm.just_opened = false;
    }
    else if (do_focus_instr)
    {
        /* Focus z Enter v addr (zpožděný o 1 frame) — select all */
        ImGui::SetKeyboardFocusHere();
        s_iasm.select_all_instr = true;
    };

    ImGuiInputTextFlags instr_flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (s_iasm.select_all_instr)
    {
        instr_flags |= ImGuiInputTextFlags_AutoSelectAll;
        s_iasm.select_all_instr = false;
    };

    /* Instrukční pole — fixní šířka (s AutoResize nelze použít -FLT_MIN) */
    ImGui::SetNextItemWidth(250.0f);
    bool instr_enter = ImGui::InputText("##iasm_instr", s_iasm.instr_buf,
                                         sizeof(s_iasm.instr_buf), instr_flags);

    /* Enter v instrukčním poli → Apply */
    if (instr_enter)
    {
        apply_instruction();
    };

    /* Separator s 5px mezerou nad a pod */
    ImGui::Dummy(ImVec2(0, 5.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 5.0f));

    /*
     * Tlačítka roztažená přes celou šířku dialogu (dle starého screenshotu):
     *   [  Help  ]                        [ Cancel ] [ Apply  ]
     *
     * Šířka tlačítek se počítá z okna: Help ~25%, Cancel ~20%, Apply ~20%.
     * GetWindowWidth() je s AutoResize stabilní po prvním framu.
     */
    float pad = ImGui::GetStyle().WindowPadding.x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float content_w = ImGui::GetWindowWidth() - 2.0f * pad;

    float btn_help_w = content_w * 0.22f;
    float btn_cancel_w = content_w * 0.20f;
    float btn_apply_w = content_w * 0.20f;

    if (ImGui::Button(_L("Help##iasm"), ImVec2(btn_help_w, 0)))
    {
        s_help_open = true;
        /* Zachytíme pozici myši - help dialog se zobrazí u kurzoru. */
        s_iasm_help_open_mouse_pos = ImGui::GetMousePos();
        s_iasm_help_have_open_mouse_pos = true;
    };

    /* Cancel a Apply zarovnané doprava */
    float cancel_x = content_w - btn_cancel_w - spacing - btn_apply_w;
    ImGui::SameLine(cancel_x + pad);

    if (ImGui::Button(_L("Cancel##iasm"), ImVec2(btn_cancel_w, 0)))
    {
        ImGui::CloseCurrentPopup();
    };

    ImGui::SameLine();

    if (ImGui::Button(_L("Apply##iasm"), ImVec2(btn_apply_w, 0)))
    {
        apply_instruction();
    };

    /* Vnořený modální help popup */
    render_help_modal();

    ImGui::EndPopup();
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
