/*
 * File:   cmthack.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 2. července 2015, 20:49
 *
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <strings.h>

// Lokalizace
#include "i18n.h"

#include "mzarch/mzarch_config.h"
#include "cmthack.h"
#include "cmtext.h"

#include "libs/cpu-z80/z80.h"
#include "memory/memory.h"
#include "mzarch/mzarch.h"
#include "cmt.h"
#include "cmt_tmz.h"
#include "replay.h"
#include "device_request.h"
#include "hw-generic/memory/rom.h"
#include "ui-imgui/cmt/imgui_cmt.h"

#include "iface/iface_audio.h"
#include "hw-generic/gdg/framebuffer.h"
#include "emulator_measuring.h"

#include "cfgmain.h"

#include "baseui/baseui.h"
#include "baseui/baseui_filechooser.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/eventlog.h"
#include "debugger/trace/ramlog.h"
#include "debugger/coverage/coverage_hook.h"
#include "mzarch/mzarch.h"
/**
 * @brief Záznam zápisů: načtení CMT hackem se zdrojem CMT_HACK, insn_pc = PC
 *        CPU v okamžiku pasti ROM.
 * @param call Volání memory_load_block().
 */
#define CMTHACK_RAMLOG_LOAD(call) do {         uint32_t cmthack_rl_saved = ramlog_source_push(RAMLOG_SRC_CMT_HACK, (uint16_t) g_mzarch_main.cpu->pc);         call;         ramlog_source_pop(cmthack_rl_saved);     } while (0)
#else
#define CMTHACK_RAMLOG_LOAD(call) call
#endif

st_CMTHACK g_cmthack;

static st_DRIVER *g_driver = &g_memory_driver_static;
#define CMT_HACK_DEFAULT_HANDLER_TYPE HANDLER_TYPE_MEMORY

typedef enum en_LOADRET
{
    LOADRET_OK = 0,
    LOADRET_ERROR,
    LOADRET_BREAK
} en_LOADRET;


/**
 * @brief Stav CMT hacku nad vloženou páskou (mezi pastí hlavičky a těla).
 *
 * Hack čte MZF z aktuálního (otevřeného) bloku pásky. Když z pásky čte
 * a transport není ve STOP (PLAY, i v pauze a po bloku zastavení pásky),
 * nejdřív ho zastaví běžným cmt_stop() (skutečný přechod: BP eventy
 * a výpis zastavení patří k němu) - přímé načtení pásku přebírá, páska
 * hrající v reálném čase spolu s okamžitým načtením nemá smysl. Páska se
 * pak posouvá jen přímým otevřením bloku ve STOP (cmttmz_seek_block(),
 * jako seek z MCP). Při nahrávání (RECORD) hack pásku nepoužívá. Kdo
 * páskou mezi pastmi hnul, pozná hack podle čítače
 * cmttmz_get_open_serial().
 *
 * Stav se ukládá do snapshotu vyjádřený vůči bloku pásky
 * (cmthack_tape_state_get() / cmthack_tape_state_set()), reset stroje ho
 * vynuluje (cmthack_reset()).
 *
 * Invarianty: pending <=> mzf != NULL; mzf vlastní hack (mzf_free()).
 * Jen vlákno emulátoru (pasti ROM uvnitř instrukce OUT).
 */
typedef struct st_CMTHACK_TAPE {
    st_MZF *mzf;             /**< MZF bloku, jehož hlavičku dostala ROM (tělo se bere odsud), jinak NULL */
    bool pending;            /**< hlavička načtená z pásky, tělo zatím ne */
    int pending_block;       /**< blok, ze kterého hlavička pochází (pro snapshot) */
    unsigned pending_serial; /**< cmttmz_get_open_serial() v okamžiku načtení hlavičky */
    bool end;                /**< hack přečetl poslední blok s MZF: další hlavička = konec pásky */
    unsigned end_serial;     /**< cmttmz_get_open_serial() v okamžiku zjištění konce */
} st_CMTHACK_TAPE;

/** @brief Stav čtení z pásky (viz st_CMTHACK_TAPE). */
static st_CMTHACK_TAPE s_tape;

static int cmthack_tape_block(void);


/**
 * @brief Hlavička z pasti zápisu WHEAD (OUT 03h, E = CCh), která čeká na tělo z WDATA.
 *
 * Do snapshotu se neukládá: past těla bez ní vezme hlavičku na 10F0h, kam
 * ji WHEAD ROM zapisuje (LD HL,10F0h na 043Eh). Vynuluje ji reset stroje,
 * cmthack_tape_state_clear() (načtení snapshotu, vypnutí hacku, ROM bez
 * podpory hacku, úpravy pásky) a ukončení. Jen vlákno emulátoru.
 *
 * Invariant: !pending => obsah raw nemá význam.
 */
static struct
{
    bool pending;                       /**< hlavička je načtená, tělo zatím ne */
    uint8_t raw[sizeof(st_MZF_HEADER)]; /**< hlavička tak, jak je v paměti (pořadí souboru MZF) */
} s_save;


/**
 * @brief Zapomene hlavičku pasti zápisu čekající na tělo.
 * @post !s_save.pending.
 */
static void cmthack_save_drop(void)
{
    s_save.pending = false;
}


/**
 * @brief Zahodí MZF rozčtené z pásky (hlavička bez těla se zapomene).
 * @post s_tape.mzf == NULL, !s_tape.pending; příznak konce pásky zůstává.
 */
static void cmthack_tape_drop(void)
{
    if (s_tape.mzf)
        mzf_free(s_tape.mzf);
    s_tape.mzf = NULL;
    s_tape.pending = false;
}

void cmthack_reinstall_rom_patch(void)
{
    cmthack_mzarch_reinstall_rom_patch();
}

void cmthack_load_rom_patch(unsigned enabled)
{
    cmthack_mzarch_load_rom_patch(enabled);
}

/*
 * Patch zápisových rutin ROM (Direct Save). Adresy a bajty: dolní monitor
 * 1Z-013B (komentovaný výpis v bázi, ROM/komentovany-vypis/
 * dolni_monitor_0000-0FFF.txt: WHEAD 0436h, J0444, J045E, CMTEND 0554h,
 * VERIF 0588h), ověřené v obrazech ROM tohoto repa (MZ-800 1Z-013B a JSS
 * 1.03 / 1.05C / 1.06A / 1.08C, MZ-700 13A a 9B, MZ-1500).
 */

/** @brief Rozsah A patche zápisu: 0447h-044Bh (CALL @MGON, JR C - 5 bajtů; CALL @CHECK na 0444h zůstává). */
#define CMTHACK_SAVE_A_ADDR 0x0447u
/** @brief Délka rozsahu A patche zápisu. */
#define CMTHACK_SAVE_A_LEN 5u
/** @brief Rozsah B patche zápisu: 045Eh-0463h (CALL @WTMRK, CALL @WBLOK - 6 bajtů). */
#define CMTHACK_SAVE_B_ADDR 0x045Eu
/** @brief Délka rozsahu B patche zápisu. */
#define CMTHACK_SAVE_B_LEN 6u
/** @brief Rozsah patche ověření: 059Eh-05AAh (CALL @MGON až CALL @VBLOK - 13 bajtů). */
#define CMTHACK_VERIFY_ADDR 0x059Eu
/** @brief Délka rozsahu patche ověření. */
#define CMTHACK_VERIFY_LEN 13u
/** @brief Hodnota masky signatury "libovolný bajt". */
#define CMTHACK_SIG_ANY (-1)

/**
 * @brief Úsek signatury: bajty od adresy, CMTHACK_SIG_ANY = libovolný bajt.
 */
typedef struct st_CMTHACK_SIG
{
    uint16_t addr;       /**< adresa prvního bajtu v dolní ROM */
    uint8_t len;         /**< počet bajtů */
    const int16_t *b;    /**< očekávané bajty (0-255) nebo CMTHACK_SIG_ANY */
} st_CMTHACK_SIG;

/** @brief WHEAD 0436h-0443h: DI; PUSH DE,BC,HL; LD D,0D7h; LD E,0CCh; LD HL,10F0h; LD BC,80h. */
static const int16_t s_sig_whead[] = {0xF3, 0xD5, 0xC5, 0xE5, 0x16, 0xD7, 0x1E, 0xCC, 0x21, 0xF0, 0x10, 0x01, 0x80, 0x00};
/** @brief J0444 0444h-044Bh: CALL @CHECK; CALL @MGON; JR C (cíle se mezi ROM liší). */
static const int16_t s_sig_0444[] = {0xCD, CMTHACK_SIG_ANY, CMTHACK_SIG_ANY, 0xCD, CMTHACK_SIG_ANY, CMTHACK_SIG_ANY, 0x38, CMTHACK_SIG_ANY};
/** @brief 044Ch-0450h: LD A,E; CP 0CCh; JR NZ,045Eh. */
static const int16_t s_sig_044c[] = {0x7B, 0xFE, 0xCC, 0x20, 0x0D};
/** @brief J045E 045Eh-0466h: CALL @WTMRK; CALL @WBLOK; JP CMTEND. */
static const int16_t s_sig_045e[] = {0xCD, CMTHACK_SIG_ANY, CMTHACK_SIG_ANY, 0xCD, CMTHACK_SIG_ANY, CMTHACK_SIG_ANY, 0xC3, CMTHACK_SIG_ANY, CMTHACK_SIG_ANY};
/** @brief WDATA 0475h-0489h: DI; PUSH; LD D,0D7h; LD E,53h; LD BC,(1102h); LD HL,(1104h); LD A,B; OR C; JR Z; JR J0444. */
static const int16_t s_sig_wdata[] = {0xF3, 0xD5, 0xC5, 0xE5, 0x16, 0xD7, 0x1E, 0x53, 0xED, 0x4B, 0x02, 0x11, 0x2A, 0x04, 0x11,
                                      0x78, 0xB1, 0x28, 0x4A, 0x18, 0xBA};
/** @brief VERIF 0588h-059Ah: DI; PUSH; LD BC,(1102h); LD HL,(1104h); LD D,0D2h; LD E,53h; LD A,B; OR C; JR Z,CMTEND. */
static const int16_t s_sig_verfy[] = {0xF3, 0xD5, 0xC5, 0xE5, 0xED, 0x4B, 0x02, 0x11, 0x2A, 0x04, 0x11, 0x16, 0xD2, 0x1E, 0x53,
                                      0x78, 0xB1, 0x28, 0xB9};
/** @brief 059Eh-05ACh: CALL @MGON; JR C; CALL @RTMRK; JR C; CALL @VBLOK; JR CMTEND (059Bh se v JSS liší, mimo signaturu). */
static const int16_t s_sig_059e[] = {0xCD, 0x9F, 0x06, 0x38, 0xCF, 0xCD, 0x5B, 0x06, 0x38, 0xCA, 0xCD, 0xAD, 0x05, 0x18, 0xA7};

/** @brief Signatura části zápisu (CMTHACK_SAVE_PART_SAVE). */
static const st_CMTHACK_SIG s_sig_save[] = {
    {0x0436, sizeof(s_sig_whead) / sizeof(s_sig_whead[0]), s_sig_whead},
    {0x0444, sizeof(s_sig_0444) / sizeof(s_sig_0444[0]), s_sig_0444},
    {0x044C, sizeof(s_sig_044c) / sizeof(s_sig_044c[0]), s_sig_044c},
    {0x045E, sizeof(s_sig_045e) / sizeof(s_sig_045e[0]), s_sig_045e},
    {0x0475, sizeof(s_sig_wdata) / sizeof(s_sig_wdata[0]), s_sig_wdata},
};

/** @brief Signatura části ověření (CMTHACK_SAVE_PART_VERIFY). */
static const st_CMTHACK_SIG s_sig_verify[] = {
    {0x0588, sizeof(s_sig_verfy) / sizeof(s_sig_verfy[0]), s_sig_verfy},
    {0x059E, sizeof(s_sig_059e) / sizeof(s_sig_059e[0]), s_sig_059e},
};


/**
 * @brief Sedí všechny úseky signatury na obraz ROM?
 * @param rom Obraz dolní ROM. @param sig Úseky. @param count Počet úseků.
 * @return true = všechny bajty mimo masku sedí.
 */
static bool cmthack_sig_match(const uint8_t *rom, const st_CMTHACK_SIG *sig, size_t count)
{
    for (size_t s = 0; s < count; s++)
    {
        for (unsigned i = 0; i < sig[s].len; i++)
        {
            if ((sig[s].b[i] != CMTHACK_SIG_ANY) && (rom[sig[s].addr + i] != (uint8_t)sig[s].b[i]))
                return false;
        };
    };
    return true;
}


/**
 * @brief XOR bajtů úseku.
 * @param p Začátek. @param len Délka. @return XOR všech bajtů.
 */
static uint8_t cmthack_xor(const uint8_t *p, unsigned len)
{
    uint8_t x = 0;
    for (unsigned i = 0; i < len; i++)
        x ^= p[i];
    return x;
}


/**
 * @brief Je bajt neškodná instrukce výplně patche?
 *
 * NOP, LD r,r' s cílem B, C, D, E, H nebo L a zdrojem registru (ne (HL),
 * které čte paměť) a EX DE,HL: nemění A ani příznaky (výsledek pasti),
 * a BC, DE, HL obnoví CMTEND ze zásobníku (POP HL, BC, DE na 0554h).
 *
 * @param op Bajt. @return true = smí do výplně.
 */
static bool cmthack_filler_ok(uint8_t op)
{
    if ((op == 0x00) || (op == 0xEB))
        return true;
    if ((op < 0x40) || (op > 0x6F))
        return false;
    return (op & 0x07u) != 0x06u; /* zdroj (HL) */
}


/**
 * @brief Výplň patche s daným XOR z neškodných jednobajtových instrukcí (cmthack_filler_ok()).
 *
 * Nejvýš čtyři nenulové bajty (EX DE,HL pro bit 7, jedna LD r,r' pro bit
 * 6, dvojice LD r,r' pro zbytek), ostatní NOP. Pro každou hodnotu @p x
 * taková výplň existuje: dvojice LD r,r' (0x40-0x6F bez zdroje (HL))
 * pokryje XOR 01h-3Fh (bity 5-3 i 2-0 jdou složit ze dvou povolených
 * hodnot).
 *
 * @param x Požadovaný XOR výplně.
 * @param[out] fill Výplň, @p len bajtů.
 * @param len Délka výplně, aspoň 4.
 */
static void cmthack_filler(uint8_t x, uint8_t *fill, unsigned len)
{
    unsigned n = 0;
    memset(fill, 0x00, len);
    if (x & 0x80)
    {
        fill[n++] = 0xEB; /* EX DE,HL */
        x ^= 0xEB;
    };
    if (x & 0x40)
    {
        if (cmthack_filler_ok(x))
        {
            fill[n++] = x;
            x = 0;
        }
        else
        {
            fill[n++] = 0x40; /* LD B,B */
            x ^= 0x40;
        };
    };
    if (x)
    {
        for (unsigned a = 0x40; a <= 0x6F; a++)
        {
            if (cmthack_filler_ok((uint8_t)a) && cmthack_filler_ok((uint8_t)(a ^ x)) && ((a ^ x) >= 0x40))
            {
                fill[n++] = (uint8_t)a;
                fill[n++] = (uint8_t)(a ^ x);
                break;
            };
        };
    };
}


unsigned cmthack_save_patch_signature(const uint8_t *rom)
{
    unsigned parts = 0;
    if (cmthack_sig_match(rom, s_sig_save, sizeof(s_sig_save) / sizeof(s_sig_save[0])))
        parts |= CMTHACK_SAVE_PART_SAVE;
    if (cmthack_sig_match(rom, s_sig_verify, sizeof(s_sig_verify) / sizeof(s_sig_verify[0])))
        parts |= CMTHACK_SAVE_PART_VERIFY;
    return parts;
}


unsigned cmthack_save_patch_install(uint8_t *rom, const uint8_t *orig)
{
    unsigned parts = cmthack_save_patch_signature(orig);
    /* ověření bez zápisu ne: srovnávalo by s páskou, kterou nahrál signál */
    if (!(parts & CMTHACK_SAVE_PART_SAVE))
        return 0;

    /* zápis: 0447h-044Bh (CALL @MGON; JR C) pět NOP, 045Eh OUT (03h),A
     * a za ním čtyři bajty výplně, které vyrovnají XOR obou rozsahů
     * (provedou se při každém uložení) */
    uint8_t want = cmthack_xor(&orig[CMTHACK_SAVE_A_ADDR], CMTHACK_SAVE_A_LEN) ^ cmthack_xor(&orig[CMTHACK_SAVE_B_ADDR], CMTHACK_SAVE_B_LEN);
    uint8_t a[CMTHACK_SAVE_A_LEN] = {0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t b[CMTHACK_SAVE_B_LEN];
    b[0] = 0xD3; /* OUT (03h),A */
    b[1] = 0x03;
    cmthack_filler(want ^ b[0] ^ b[1], &b[2], CMTHACK_SAVE_B_LEN - 2);
    memcpy(&rom[CMTHACK_SAVE_A_ADDR], a, CMTHACK_SAVE_A_LEN);
    memcpy(&rom[CMTHACK_SAVE_B_ADDR], b, CMTHACK_SAVE_B_LEN);

    if (parts & CMTHACK_SAVE_PART_VERIFY)
    {
        /* ověření: OUT (04h),A a výplň, jejíž XOR rozsah vyrovná */
        uint8_t v[CMTHACK_VERIFY_LEN];
        v[0] = 0xD3;
        v[1] = 0x04;
        cmthack_filler(cmthack_xor(&orig[CMTHACK_VERIFY_ADDR], CMTHACK_VERIFY_LEN) ^ v[0] ^ v[1], &v[2], CMTHACK_VERIFY_LEN - 2);
        memcpy(&rom[CMTHACK_VERIFY_ADDR], v, CMTHACK_VERIFY_LEN);
    };
    return parts;
}


void cmthack_save_patch_uninstall(uint8_t *rom, const uint8_t *orig)
{
    memcpy(&rom[CMTHACK_SAVE_A_ADDR], &orig[CMTHACK_SAVE_A_ADDR], CMTHACK_SAVE_A_LEN);
    memcpy(&rom[CMTHACK_SAVE_B_ADDR], &orig[CMTHACK_SAVE_B_ADDR], CMTHACK_SAVE_B_LEN);
    memcpy(&rom[CMTHACK_VERIFY_ADDR], &orig[CMTHACK_VERIFY_ADDR], CMTHACK_VERIFY_LEN);
}


static bool cmthack_rom_takes_default_patch(void);


/**
 * @brief Posune PC hlavního Z80 na hranici původní instrukce, když se odstraňuje patch, uvnitř kterého CPU stojí.
 *
 * Po odstranění patche (vstup do RECORD, vypnutí volby nebo hacku, výměna
 * ROM) by CPU s PC uvnitř bajtů patche pokračovalo uprostřed původní
 * instrukce. Jen je-li na 0000h namapovaná dolní ROM (memmap_query();
 * jinak tam běží kód v RAM a PC se nemění):
 * - zápis: PC 0448h-044Bh -> 0447h (původní `CALL @MGON` proběhne
 *   s registry volání, uložení pokračuje jako signálové), PC 0460h-0463h
 *   -> 0464h (`JP CMTEND`; past už proběhla, výsledek v AF zůstává);
 * - ověření: PC 05A0h-05AAh -> 05ABh (`JR CMTEND`; past už proběhla).
 * PC na 0447h, 045Eh a 059Eh (před OUT) i jinde se nemění: tam proběhne
 * původní instrukce.
 *
 * @param removed Bitová maska odstraňovaných částí (CMTHACK_SAVE_PART_*).
 * @pre Vlákno emulátoru mimo instrukci (pasti patch neodstraňují).
 * @post PC neleží uvnitř odstraňovaných rozsahů mimo hranici původní instrukce.
 */
static void cmthack_save_patch_pc_fixup(unsigned removed)
{
    if (!removed || (memmap_query(0) != MEMMAP_KIND_ROM_LOW))
        return;
    unsigned pc = (unsigned)z80_get_reg(g_mzarch_main.cpu, Z80_REG_PC);
    unsigned to = pc;
    if (removed & CMTHACK_SAVE_PART_SAVE)
    {
        if ((pc >= 0x0448u) && (pc <= 0x044Bu))
            to = 0x0447u;
        else if ((pc >= 0x0460u) && (pc <= 0x0463u))
            to = 0x0464u;
    };
    if ((removed & CMTHACK_SAVE_PART_VERIFY) && (pc >= 0x05A0u) && (pc <= 0x05AAu))
        to = 0x05ABu;
    if (to != pc)
    {
        printf("CMT hack: the Direct Save patch was removed while the CPU was inside it, PC 0x%04x -> 0x%04x\n", pc, to);
        z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, (uint16_t)to);
    };
}


/**
 * @brief Společné tělo cmthack_save_patch_sync() a cmthack_save_patch_sync_loaded_state().
 * @param move_pc true = při sundání patche přesunout PC z bajtů patche na
 *        hranici původní instrukce (cmthack_save_patch_pc_fixup()); false =
 *        PC nechat (stav stroje právě načtený ze snapshotu).
 * @pre Jako cmthack_save_patch_sync().
 */
static void cmthack_save_patch_sync_impl(bool move_pc)
{
    /* chtěný stav: hack se zapnutým patchem čtení na ROM s výchozím patchem,
     * volba direct_save a deck mimo RECORD (Record = nahrát signál) */
    const bool wanted = g_rom.mz700rom && CMTHACK_TEST_IS_INSTALLED && cmthack_rom_takes_default_patch() &&
                        g_cmthack.direct_save && !CMT_TEST_RECORD;
    /* bez patche zůstává ROM původní (invariant): nic přepisovat ani zamykat */
    if (!wanted && !g_cmthack.save_patch_installed && !g_cmthack.verify_patch_installed)
    {
        cmthack_save_drop();
        return;
    };

    /* příznaky a ROM čte UI pod zámkem stavu zařízení */
    bool taken = false;
    if (!device_state_lock_held())
    {
        device_state_lock();
        taken = true;
    };
    unsigned parts = 0;
    if (g_rom.mz700rom)
    {
        /* nejdřív původní bajty: patch se pak skládá vždy z čistého obrazu
         * (po instalaci nových dat ROM je paměť už čistá, kopie nic nemění) */
        cmthack_save_patch_uninstall(g_memory.ROM, g_rom.mz700rom);
        if (wanted)
        {
            parts = cmthack_save_patch_install(g_memory.ROM, g_rom.mz700rom);
            static const uint8_t *s_reported_rom = NULL;
            if (!parts && (s_reported_rom != g_rom.mz700rom))
            {
                printf("CMT hack: the selected ROM has no standard tape write routines, Direct Save is not available\n");
                s_reported_rom = g_rom.mz700rom;
            };
        };
    };
    /* odstraněné části: CPU uvnitř nich přesunout na hranici původní instrukce */
    unsigned removed = 0;
    if (g_cmthack.save_patch_installed && !(parts & CMTHACK_SAVE_PART_SAVE))
        removed |= CMTHACK_SAVE_PART_SAVE;
    if (g_cmthack.verify_patch_installed && !(parts & CMTHACK_SAVE_PART_VERIFY))
        removed |= CMTHACK_SAVE_PART_VERIFY;
    if (move_pc)
        cmthack_save_patch_pc_fixup(removed);
    g_cmthack.save_patch_installed = (parts & CMTHACK_SAVE_PART_SAVE) ? 1u : 0u;
    g_cmthack.verify_patch_installed = (parts & CMTHACK_SAVE_PART_VERIFY) ? 1u : 0u;
    if (taken)
        device_state_unlock();
    /* hlavička WHEAD bez těla nepřežije sundání patche (vstup do RECORD,
     * vypnutí volby nebo hacku) */
    if (!g_cmthack.save_patch_installed)
        cmthack_save_drop();
}


void cmthack_save_patch_sync(void)
{
    cmthack_save_patch_sync_impl(true);
}


void cmthack_save_patch_sync_loaded_state(void)
{
    cmthack_save_patch_sync_impl(false);
}


bool cmthack_rom_supported(void)
{
    /* táž podmínka jako cmthack_mzarch_load_rom_patch() ve všech třech architekturách */
    if (TEST_ROM_CMTHACK_INCOMP)
        return false;
    if (TEST_ROM_USER_DEFINED && (g_rom.user_defined_cmthack_type == ROM_CMTHACK_DISABLED))
        return false;
    return true;
}

/** INI element `[CMTHACK] enable` (nastaví cmthack_init), pro připnutí INI hodnoty. */
static CFGELM *s_cmthack_enable_elm = NULL;

void cmthack_disable_for_session(void)
{
    if (!CMTHACK_TEST_IS_INSTALLED)
    {
        return;
    };
    cmthack_load_rom_patch(0);
    if (s_cmthack_enable_elm)
    {
        /* Do INI se při ukončení zapíše 1 (stav před vypnutím), dokud
         * uživatel hack sám nepřepne. */
        cfgelement_pin_save_value(s_cmthack_enable_elm, 1);
    };
}

void cmthack_reset(void)
{
    /* pokud jsme rozecteny MZF soubor, tak ho zavrem */
    generic_driver_close(&g_cmthack.mzf_handler);
    /* rozčtené MZF z pásky i konec pásky: páska stojí, kde stojí, další LOAD
     * vezme její aktuální blok (reset a nové načtení téhož programu) */
    cmthack_tape_state_clear();
}


void cmthack_tape_state_clear(void)
{
    cmthack_tape_drop();
    s_tape.end = false;
    /* hlavička pasti zápisu bez těla: past těla vezme hlavičku z 10F0h */
    cmthack_save_drop();
}


bool cmthack_tape_state_get(st_CMTHACK_TAPE_STATE *st)
{
    memset(st, 0, sizeof(*st));
    unsigned serial = cmttmz_get_open_serial();
    if (s_tape.pending && s_tape.mzf)
    {
        st->pending = true;
        st->pending_block = s_tape.pending_block;
        st->pending_moved = (s_tape.pending_serial != serial);
        return true;
    };
    if (s_tape.end && (s_tape.end_serial == serial) && (cmthack_tape_block() >= 0))
    {
        st->end = true;
        return true;
    };
    return false;
}


int cmthack_tape_state_set(const st_CMTHACK_TAPE_STATE *st)
{
    cmthack_tape_state_clear();
    if (!st->pending && !st->end)
        return EXIT_SUCCESS;
    int block = cmthack_tape_block();
    unsigned serial = cmttmz_get_open_serial();
    if (st->pending && st->end)
        return EXIT_FAILURE;
    if (st->end)
    {
        if (block < 0)
            return EXIT_FAILURE;
        s_tape.end = true;
        s_tape.end_serial = serial;
        return EXIT_SUCCESS;
    };
    const st_TZX_FILE *f = cmttmz_get_file();
    if (!f || (st->pending_block < 0) || ((uint32_t)st->pending_block >= f->block_count))
        return EXIT_FAILURE;
    if (!st->pending_moved && (st->pending_block != block))
        return EXIT_FAILURE;
    st_MZF *mzf = cmttmz_block_get_mzf(st->pending_block);
    if (!mzf)
        return EXIT_FAILURE;
    s_tape.mzf = mzf;
    s_tape.pending = true;
    s_tape.pending_block = st->pending_block;
    /* páskou mezitím někdo hnul: čítač, který se s aktuálním neshoduje (čítač
     * jen roste, hodnota o jedna menší už znovu nenastane) */
    s_tape.pending_serial = st->pending_moved ? (serial - 1u) : serial;
    return EXIT_SUCCESS;
}

/**
 * @brief Ukončení emulátoru: zavře rozčtený soubor MZF, zapomene MZF z pásky a uvolní poslední cestu dialogu.
 * @pre Vlákno emulátoru; po návratu se už hack nepoužívá.
 */
void cmthack_exit(void)
{
    /* pokud jsme rozecteny MZF soubor, tak ho zavrem */
    generic_driver_close(&g_cmthack.mzf_handler);
    cmthack_tape_drop();
    cmthack_save_drop();
    if (g_cmthack.last_filename)
    {
        g_free(g_cmthack.last_filename);
    }
}

#define DEFAULT_CMT_HACK_ENABLE 1
#define DEFAULT_CMT_HACK_FIX_TERMINATOR 1
/** @brief Výchozí hodnota `[CMTHACK] direct_save`: ROM SAVE s hackem ukládá přímo. */
#define DEFAULT_CMT_HACK_DIRECT_SAVE 1

void cmthack_propagatecfg_load_rom_patch(void *e, void *data)
{
    (void)data;
    cmthack_load_rom_patch(cfgelement_get_bool_value((CFGELM *)e));
}


/** INI element `[CMTHACK] direct_save` (nastaví cmthack_init), aby změna z menu došla i do hodnoty elementu. */
static CFGELM *s_cmthack_direct_save_elm = NULL;


/**
 * @brief Bere vybraná ROM výchozí patch hacku (ne INCOMP, ne vlastní ROM hacku uživatele)?
 *
 * Táž podmínka, za které tři cmthack_mzarch_load_rom_patch() zapisují
 * patch do ROM; u uživatelské ROM s typem CUSTOM je v paměti její vlastní
 * ROM hacku a výchozí patch se nedává.
 *
 * @return true = výchozí patch (čtení i zápisu) jde do ROM.
 * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (čte g_rom).
 */
static bool cmthack_rom_takes_default_patch(void)
{
    if (TEST_ROM_CMTHACK_INCOMP)
        return false;
    if (TEST_ROM_USER_DEFINED && (g_rom.user_defined_cmthack_type != ROM_CMTHACK_DEFAULT))
        return false;
    return true;
}


/**
 * @brief Propagace `[CMTHACK] direct_save`: nastavení a patch zápisu v ROM.
 *
 * Volá ji cmthack_init() (INI při startu). Za běhu volbu mění jen
 * cmthack_direct_save_set() (zásah menu cmt_request_set_direct_save()
 * a MCP settings_set, obojí jen ve STOP).
 *
 * @param e Element. @param data Nepoužito.
 * @pre Vlákno emulátoru nebo inicializace.
 */
static void cmthack_propagatecfg_direct_save(void *e, void *data)
{
    (void)data;
    g_cmthack.direct_save = cfgelement_get_bool_value((CFGELM *)e) ? 1u : 0u;
    cmthack_save_patch_sync();
}


void cmthack_direct_save_set(unsigned enabled)
{
    g_cmthack.direct_save = enabled ? 1u : 0u;
    if (s_cmthack_direct_save_elm)
        cfgelement_set_bool_value(s_cmthack_direct_save_elm, (int)g_cmthack.direct_save);
    cmthack_save_patch_sync();
}


bool cmthack_save_rom_supported(void)
{
    if (!cmthack_rom_takes_default_patch() || !g_rom.mz700rom)
        return false;
    return (cmthack_save_patch_signature(g_rom.mz700rom) & CMTHACK_SAVE_PART_SAVE) != 0;
}

void cmthack_init(void)
{
    generic_driver_register_handler(&g_cmthack.mzf_handler, CMT_HACK_DEFAULT_HANDLER_TYPE);
    generic_driver_set_handler_readonly_status(&g_cmthack.mzf_handler, 1);
    g_cmthack.last_filename = g_malloc0(1);
    g_cmthack.load_patch_installed = 0;
    g_cmthack.fix_fname_terminator = 0;
    g_cmthack.save_patch_installed = 0;
    g_cmthack.verify_patch_installed = 0;
    g_cmthack.direct_save = 0;

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "CMTHACK");

    CFGELM *elm;
    elm = cfgmodule_register_new_element(cmod, "enable", CFGENTYPE_BOOL, DEFAULT_CMT_HACK_ENABLE);
    cfgelement_set_propagate_cb(elm, cmthack_propagatecfg_load_rom_patch, NULL);
    cfgelement_set_handlers(elm, NULL, (void *)&g_cmthack.load_patch_installed);
    s_cmthack_enable_elm = elm;

    elm = cfgmodule_register_new_element(cmod, "fix_fname_terminator", CFGENTYPE_BOOL, DEFAULT_CMT_HACK_FIX_TERMINATOR);
    cfgelement_set_handlers(elm, (void *)&g_cmthack.fix_fname_terminator, (void *)&g_cmthack.fix_fname_terminator);

    /* po "enable": propagace hacku dá patch čtení, direct_save pak patch zápisu */
    elm = cfgmodule_register_new_element(cmod, "direct_save", CFGENTYPE_BOOL, DEFAULT_CMT_HACK_DIRECT_SAVE);
    cfgelement_set_propagate_cb(elm, cmthack_propagatecfg_direct_save, NULL);
    cfgelement_set_handlers(elm, NULL, (void *)&g_cmthack.direct_save);
    s_cmthack_direct_save_elm = elm;

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}

void cmthack_result(en_LOADRET result)
{

    uint16_t reg_af = z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);

    if (result == LOADRET_OK)
    {
        reg_af &= ~0x01; /* reset CARRY flag */
    }
    else
    {
        reg_af |= 0x01;                    /* set CARRY flag */
        reg_af |= ((uint16_t)result << 8); /* result do regA */
        reg_af &= ((uint16_t)result << 8) | 0xff;
    };
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, reg_af);
}

/**
 * @brief Číslo otevřeného bloku vložené pásky, ze kterého může hack číst.
 * @return Číslo bloku; -1 bez pásky, bez otevřeného bloku nebo s páskou
 *         jiného rozšíření (nahrávání).
 */
static int cmthack_tape_block(void)
{
    if ((!CMT_TEST_FILLED) || (g_cmt.ext != g_cmt_tmz_extension) || (!g_cmt.ext->block))
        return -1;
    return cmtext_block_get_block_id(g_cmt.ext->block);
}


/**
 * @brief Posune pásku za blok @p from na další blok s MZF.
 *
 * Bere bloky, které nesou MZF (cmttmz_block_get_mzf() uspěje) a jdou
 * otevřít (cmttmz_seek_block(): přeskakované a nepřehratelné bloky ne).
 * Není-li žádný, páska zůstane, kde je, a nastaví se příznak konce pásky.
 * Neběží-li transport ve STOP (rozjel ho mezi pastmi program nebo
 * uživatel), nejdřív ho zastaví cmt_stop(): přímé otevření bloku za
 * běhu by přehrávání počítalo pozici z času starého bloku.
 *
 * @param from Blok, za kterým se hledá.
 * @pre Vlákno emulátoru, volající nedrží device_state_lock() (funkce ho
 *      bere kolem výměny bloku, UI čte pásku pod týmž zámkem).
 * @post Otevřený je další blok s MZF, nebo s_tape.end s aktuálním čítačem.
 */
static void cmthack_tape_advance(int from)
{
    if (!CMT_TEST_STOP)
        cmt_stop();
    const st_TZX_FILE *f = cmttmz_get_file();
    int count = f ? (int)f->block_count : 0;
    device_state_lock();
    for (int id = from + 1; id < count; id++)
    {
        st_MZF *m = cmttmz_block_get_mzf(id);
        if (!m)
            continue;
        mzf_free(m);
        if (EXIT_SUCCESS == cmttmz_seek_block(id))
        {
            device_state_unlock();
            return;
        };
    };
    device_state_unlock();
    s_tape.end = true;
    s_tape.end_serial = cmttmz_get_open_serial();
}


/**
 * @brief Past hlavičky nad vloženou páskou: načte hlavičku MZF aktuálního bloku.
 *
 * Postup:
 * - Při nahrávání (RECORD) vrátí false (pásku nepoužije).
 * - Bude-li hack z pásky číst (posun za minulou hlavičku, nebo aktuální
 *   blok s MZF) a transport není ve STOP, zastaví ho cmt_stop().
 * - Přečetl-li program minule jen hlavičku (bez těla) a páskou mezitím
 *   nikdo nehnul, posune se nejdřív páska za ten blok (hledání programu
 *   podle jména čte hlavičky za sebou; páska se tak posune jen jednou).
 * - Stojí-li páska na bloku, za kterým hack zjistil konec pásky, a nikdo
 *   jí mezitím nehnul, je to konec pásky: vrátí false.
 * - Aktuální blok bez MZF (TAP, WAV, jiný blok TZX, 0x46 s fsize jiným
 *   než tělo): vrátí false (transport se nemění).
 * - Jinak hlavičku zapíše do RAM na @p reg_hl (s opravou terminátoru jména
 *   jako u souboru), ROM dostane OK a MZF se drží pro tělo. Páska se po
 *   hlavičce nehýbe.
 *
 * Hlavička se zapisuje tak, jak je v bloku (i fsize jiný než délka těla
 * u bloků 0x40 / 0x41). Kontrola velikosti MZF (mzfsize_check) se u pásky
 * nedělá - tělo je to, které blok má.
 *
 * @param reg_hl Adresa hlavičky v RAM (HL z patche ROM).
 * @return true = hlavička načtená z pásky (výsledek je v registrech);
 *         false = chování jako bez pásky (volající pokračuje dialogem,
 *         headless Break, záznamem vstupů).
 * @pre Vlákno emulátoru, uvnitř pasti OUT (01h).
 */
static bool cmthack_tape_load_header(unsigned reg_hl)
{
    int block = cmthack_tape_block();
    if ((block < 0) || CMT_TEST_RECORD)
    {
        cmthack_tape_drop();
        return false;
    };

    /* přímé načtení pásku přebírá: transport do STOP, než se s páskou hne */
    if (!CMT_TEST_STOP)
    {
        unsigned serial = cmttmz_get_open_serial();
        bool skip = s_tape.pending && (s_tape.pending_serial == serial);
        bool at_end = s_tape.end && (s_tape.end_serial == serial);
        bool has_mzf = false;
        if (!skip && !at_end)
        {
            st_MZF *m = cmttmz_block_get_mzf(block);
            has_mzf = (m != NULL);
            if (m)
                mzf_free(m);
        };
        if (skip || has_mzf)
            cmt_stop(); /* nemění otevřený blok ani čítač */
    };

    if (s_tape.pending && (s_tape.pending_serial == cmttmz_get_open_serial()))
    {
        /* minule jen hlavička: program tenhle soubor přeskočil */
        cmthack_tape_advance(block);
        block = cmthack_tape_block();
    };
    cmthack_tape_drop();

    if (s_tape.end)
    {
        if (s_tape.end_serial == cmttmz_get_open_serial())
            return false;
        s_tape.end = false; /* páskou mezitím hnul uživatel nebo MCP */
    };

    st_MZF *mzf = cmttmz_block_get_mzf(block);
    if (!mzf)
        return false;

    /* rozčtený soubor z dialogu se zahodí: tělo patří hlavičce z pásky */
    generic_driver_close(&g_cmthack.mzf_handler);

    st_MZF_HEADER raw = mzf->header;
    mzf_header_swap_endianity(&raw); /* do pořadí souboru (RAM Sharp MZ) */
    CMTHACK_RAMLOG_LOAD(memory_load_block((uint8_t *)&raw, reg_hl, sizeof(st_MZF_HEADER), MEMORY_LOAD_MAPED));

    bool has_terminator = false;
    for (unsigned i = 0; i < sizeof(st_MZF_FILENAME); i++)
    {
        if (((const uint8_t *)&raw.fname)[i] == MZF_FNAME_TERMINATOR)
        {
            has_terminator = true;
            break;
        };
    };

    char ascii_filename[MZF_FNAME_FULL_LENGTH];
    mzf_tools_get_fname(&mzf->header, ascii_filename);
    printf("\nCMT hack: load MZF header from tape block %d on 0x%04x.\n\n", block, reg_hl);
    printf("fname: %s", ascii_filename);
    if (!has_terminator)
    {
        uint8_t term = MZF_FNAME_TERMINATOR;
        if (g_cmthack.fix_fname_terminator)
        {
            CMTHACK_RAMLOG_LOAD(memory_load_block(&term, (reg_hl + sizeof(st_MZF_FILENAME) - 2), 1, MEMORY_LOAD_MAPED));
        }
        printf(" (!mising 0x0d%s)", (g_cmthack.fix_fname_terminator) ? " - fixed" : "");
    };
    printf("\n");
    printf("ftype: 0x%02x\n", mzf->header.ftype);
    printf("fstrt: 0x%04x\n", mzf->header.fstrt);
    printf("fsize: 0x%04x\n", mzf->header.fsize);
    printf("fexec: 0x%04x\n", mzf->header.fexec);

    s_tape.mzf = mzf;
    s_tape.pending = true;
    s_tape.pending_block = block;
    s_tape.pending_serial = cmttmz_get_open_serial();
    cmthack_result(LOADRET_OK);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    eventlog_sys_event ( EVENTLOG_SYS_MZF_INJECT, eventlog_filename_hash ( g_cmt.last_filename ? g_cmt.last_filename : "" ) );
#endif
    return true;
}


/**
 * @brief Past těla nad vloženou páskou: zapíše tělo MZF, jehož hlavičku dostala ROM.
 *
 * ROM chce @p reg_bc bajtů (fsize z hlavičky v RAM). Tělo je to, které
 * blok má: je-li delší, zapíše se prvních @p reg_bc bajtů; je-li kratší
 * (blok 0x40 / 0x41 s fsize větším než tělo), ROM dostane chybu (A = 1)
 * a RAM se nezmění. V obou případech se páska posune na další blok s MZF
 * (po chybě jako skutečná páska, která už blok přejela; se zapnutým
 * hackem okno kazety nenabízí, jak pásku posunout, a stání na takovém
 * bloku by každé další načtení skončilo stejnou chybou), pokud jí mezi
 * hlavičkou a tělem nikdo nehnul; když ano, zůstane, kam ji uživatel
 * nebo MCP dal (tělo i tak patří hlavičce, kterou program dostal).
 *
 * @param reg_hl Cílová adresa v RAM.
 * @param reg_bc Počet bajtů.
 * @pre Vlákno emulátoru, s_tape.mzf != NULL.
 * @post s_tape.mzf == NULL, !s_tape.pending.
 */
static void cmthack_tape_read_body(unsigned reg_hl, unsigned reg_bc)
{
    st_MZF *mzf = s_tape.mzf;
    bool moved = (s_tape.pending_serial != cmttmz_get_open_serial());
    int block = cmthack_tape_block();

    if (reg_bc > mzf->body_size)
    {
        fprintf(stderr, "CMT hack: the tape block has a body of %u bytes, ROM requests %u bytes - load error\n",
                (unsigned)mzf->body_size, reg_bc);
        cmthack_tape_drop();
        if ((!moved) && (block >= 0))
            cmthack_tape_advance(block);
        cmthack_result(LOADRET_ERROR);
        return;
    };

    if (reg_bc)
        CMTHACK_RAMLOG_LOAD(memory_load_block(mzf->body, reg_hl, reg_bc, MEMORY_LOAD_MAPED));
    printf("\nCMT hack: load 0x%04x bytes from tape block body to addr 0x%04x.\n", reg_bc, reg_hl);
    cmthack_tape_drop();
    if ((!moved) && (block >= 0))
        cmthack_tape_advance(block);
    cmthack_result(LOADRET_OK);
}


/**
 * @brief Obslouží požadavek ROM na načtení hlavičky z kazety (port 01h).
 *
 * Volá se z port_write_cb() uvnitř instrukce OUT (EMU vlákno), kterou do ROM
 * vložil CMT hack patch. S GUI otevře blokující dialog pro výběr MZF a čeká,
 * dokud ho uživatel nezavře; emulace po tu dobu stojí. Výsledek předá ROM
 * přes registry (cmthack_result()): zrušení = Break.
 *
 * Je-li ve Virtual CMT páska a její aktuální blok nese MZF, vezme se
 * hlavička z něj (cmthack_tape_load_header(); bez dialogu, bez záznamu do
 * záznamu vstupů - obsah pásky nese záznam sám) a po těle se páska posune
 * na další blok s MZF. Aktuální blok bez MZF nebo konec pásky = chování
 * jako bez pásky (dialog, headless Break, záznam vstupů).
 *
 * Bez interaktivního GUI (--headless, --mcp-pipe) dialog nikdo nezavře.
 * Dřív EMU vlákno čekalo navždy: emulace stála, fronta dbgapi se už
 * nevybrala a každý příkaz MCP vracel "Emulator busy". Nyní se požadavek
 * okamžitě vyřídí jako zrušený (ROM dostane Break), na stderr se vypíše
 * varování a emulace běží dál. Načíst pásku v takovém režimu lze reálnou
 * páskou (CMT hack vypnout, [CMTHACK] enable = 0 nebo MCP cmt_hack_set).
 *
 * Záznam vstupů: výběr v dialogu (i zrušení a headless Break) se zapíše do
 * záznamu i s obsahem souboru (replay_cmthack_note()); při přehrávání se
 * dialog neotevře a použije se další soubor ze záznamu
 * (replay_cmthack_playback()), i bez GUI.
 *
 * @pre EMU vlákno, uvnitř instrukce OUT (01h) se zapnutým CMT hack patchem.
 * @post Hlavička je v RAM na adrese z HL a ROM dostala výsledek OK, nebo ROM
 *       dostala Break či chybu. Zvuk a měření MAX SPEED jsou obnovené.
 * @par Side effects Pozastaví zvuk a měření po dobu dialogu, zapisuje do RAM
 *      a registrů CPU, při headless výpis na stderr.
 */
void cmthack_load_file(void)
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Pokrytí kódu: hack zasáhl během právě vykonávané instrukce (OUT). */
    coverage_hook_note_cmt_trap();
#endif
    /* Vložená páska: MZF z aktuálního bloku (před záznamem vstupů - obsah
     * a pozici pásky nese záznam, při přehrání dá páska totéž). */
    if (cmthack_tape_load_header(z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL)))
        return;

    /* Přehrání vstupů: soubor ze záznamu místo dialogu (i bez GUI). */
    char *replay_file = NULL;
    if (replay_cmthack_playback(&replay_file))
    {
        const char *ext = replay_file ? cmtext_get_filename_extension(replay_file) : NULL;
        if (ext && ((0 == strcasecmp(ext, "mzf")) || (0 == strcasecmp(ext, "m12"))))
        {
            cmthack_load_mzf_filename(replay_file);
        }
        else
        {
            cmthack_result(LOADRET_BREAK);
        };
        g_free(replay_file);
        return;
    };

    if (!baseui_filechooser_can_wait())
    {
        /* Headless: dialog by nikdo nezavřel a EMU vlákno by stálo navždy. */
        fprintf(stderr, "WARNING: CMT hack needs the GUI file dialog to load a tape file; "
                        "no GUI in headless mode, load cancelled (Break). "
                        "Disable the CMT hack to load from the virtual tape.\n");
        replay_cmthack_note(NULL);
        cmthack_result(LOADRET_BREAK);
        return;
    };

    iface_audio_pause_emulation(1);
    /* CMT-hack stall: file chooser dialog + IO stojí emulaci - vyloučit z benchmarku */
    emulator_measuring_maxspeed_stall_begin();
    framebuffer_flush_full_screen();

    char *filename = baseui_filechooser_open_file_wait(BASEUI_FCHOOSER_CAT_MZF, _("Select a MZF File"), ".mzf, .m12, .*", NULL, NULL, g_cmthack.last_filename, NULL);
    /* Záznam vstupů: výběr (i zrušení) se uloží i s obsahem souboru. */
    replay_cmthack_note(filename);

    if (filename == NULL)
    {
        /* Zruseno: nastavit Err + Break */
        cmthack_result(LOADRET_BREAK);
        iface_audio_pause_emulation(0);
        emulator_measuring_frame_timing_reset();
        emulator_measuring_maxspeed_stall_end();
        return;
    };

    const char *file_ext = cmtext_get_filename_extension(filename);

    if ((0 == strcasecmp(file_ext, "mzf")) || (0 == strcasecmp(file_ext, "m12")))
    {
        cmthack_load_mzf_filename(filename);
    }
    else
    {
        baseui_show_message(true, "This file can't be open in CMTHACK.\nPlease, load this file by virtual CMT.");
        cmthack_result(LOADRET_BREAK);
    };

    g_free(filename);
    iface_audio_pause_emulation(0);
    emulator_measuring_frame_timing_reset();
    emulator_measuring_maxspeed_stall_end();
}

/**
 * @brief Načte hlavičku MZF ze souboru do RAM na adresu z HL (cesta dialogu a záznamu vstupů).
 *
 * Otevře soubor (zavře dříve rozčtený soubor i MZF rozčtené z pásky),
 * zapíše 128 B hlavičky na HL, doplní terminátor jména podle
 * fix_fname_terminator, při zapnuté kontrole velikosti MZF zkontroluje
 * soubor a výsledek zapíše do registrů CPU (OK); soubor zůstane otevřený
 * pro cmthack_read_mzf_body(). Zapamatuje si cestu jako výchozí pro dialog.
 *
 * Volající: past hlavičky cmthack_load_file() (dialog, záznam vstupů),
 * mimo past ROM také --run-mzf (mzarch_bootstrap_run_mzf(),
 * mzarch/bootstrap.c) a MCP media_load_mzf (debugger/dbgapi.c); ti
 * nastaví HL sami a výsledek čtou ze stavu g_cmthack.mzf_handler.
 *
 * @param filename Cesta k MZF (UTF-8).
 * @pre Vlákno emulátoru, HL ukazuje na cíl hlavičky v RAM (uvnitř pasti
 *      OUT (01h), nebo nastavené volajícím mimo past).
 * @post ROM (registry CPU) dostala OK, nebo Break (soubor nejde otevřít)
 *       či chybu (nejde přečíst hlavičku); při chybě je soubor zavřený.
 * @par Side effects Zapisuje do RAM (hlavička, záznam zápisů zdrojem
 *      CMT_HACK) a do AF; zahodí MZF rozčtené z pásky; při chybě
 *      baseui_error() (log a v GUI zpráva); při zapnuté kontrole velikosti
 *      MZF baseui_cmt_check_mzf_filesize() (v GUI dialog opravy);
 *      s debuggerem událost EVENTLOG_SYS_MZF_INJECT v Event Vieweru;
 *      výpis hlavičky na stdout.
 */
void cmthack_load_mzf_filename(const char *filename)
{

    unsigned reg_hl;

    /* pokud jsme rozecteny MZF soubor, tak ho zavrem; rozčtené MZF z pásky také */
    generic_driver_close(&g_cmthack.mzf_handler);
    cmthack_tape_drop();

    if ((NULL == generic_driver_open_memory_from_file(&g_cmthack.mzf_handler, g_driver, filename)) || (g_cmthack.mzf_handler.err) || (g_driver->err))
    {
        /* Nejde otevrit soubor, nebo inicializovat handler: nastavit Err + Break */
        baseui_error("CMT HACK can't open MZF file '%s': %s, gdriver_err: %s\n", filename, strerror(errno), generic_driver_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        cmthack_result(LOADRET_BREAK);
        return;
    };

    g_cmthack.last_filename = g_realloc(g_cmthack.last_filename, strlen(filename) + 1);
    snprintf(g_cmthack.last_filename, strlen(filename) + 1, "%s", filename);

    /* precteme prvnich 128 bajtu z MZF souboru a ulozime je do RAM na adresu z regHL */

    reg_hl = z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL);

    uint8_t buff[sizeof(st_MZF_HEADER)];
    if (EXIT_SUCCESS != generic_driver_read(&g_cmthack.mzf_handler, 0, &buff, sizeof(buff)))
    {
        /* Vadny soubor: nastavit Err + Checksum */
        baseui_error("CMT HACK can't load MZF header '%s': %s, gdriver_err: %s\n", filename, strerror(errno), generic_driver_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        generic_driver_close(&g_cmthack.mzf_handler);
        cmthack_result(LOADRET_ERROR);
        return;
    };
    CMTHACK_RAMLOG_LOAD(memory_load_block(buff, reg_hl, sizeof(st_MZF_HEADER), MEMORY_LOAD_MAPED));

    int test_fname_term = mzf_header_test_fname_terminator(&g_cmthack.mzf_handler);

    st_MZF_HEADER mzfhdr;

    if (EXIT_SUCCESS == mzf_read_header(&g_cmthack.mzf_handler, &mzfhdr))
    {
        char ascii_filename[MZF_FNAME_FULL_LENGTH];
        mzf_tools_get_fname(&mzfhdr, ascii_filename);
        printf("\nCMT hack: load MZF header on 0x%04x.\n\n", reg_hl);
        printf("fname: %s", ascii_filename);
        if (test_fname_term == EXIT_FAILURE)
        {
            uint8_t buff = MZF_FNAME_TERMINATOR;
            if (g_cmthack.fix_fname_terminator)
            {
                CMTHACK_RAMLOG_LOAD(memory_load_block(&buff, (reg_hl + sizeof(st_MZF_FILENAME) - 2), 1, MEMORY_LOAD_MAPED));
            }
            printf(" (!mising 0x0d%s)", (g_cmthack.fix_fname_terminator) ? " - fixed" : "");
        }
        printf("\n");
        printf("ftype: 0x%02x\n", mzfhdr.ftype);
        printf("fstrt: 0x%04x\n", mzfhdr.fstrt);
        printf("fsize: 0x%04x\n", mzfhdr.fsize);
        printf("fexec: 0x%04x\n", mzfhdr.fexec);

        if (CMT_TEST_MZFSIZE_CHECK_ENABLED)
        {
            unsigned mzf_size = mzfhdr.fsize + sizeof(st_MZF_HEADER);
            baseui_cmt_check_mzf_filesize(filename, mzf_size);
        };
    }
    else
    {
        baseui_error("CMT HACK can't read MZF header '%s': %s, gdriver_err: %s\n", filename, strerror(errno), mzf_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        generic_driver_close(&g_cmthack.mzf_handler);
        cmthack_result(LOADRET_ERROR);
        return;
    };

    /* Nacteno OK */
    cmthack_result(LOADRET_OK);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Vlna 5 Commit 31 - SYS lifecycle event do Event Viewer. Filename hash
     * (djb2 z basename) zaznamenáme v payloadu. */
    eventlog_sys_event ( EVENTLOG_SYS_MZF_INJECT, eventlog_filename_hash ( filename ) );
#endif
}

/**
 * @brief Obslouží požadavek ROM na načtení těla (port 02h): HL = adresa, BC = délka.
 *
 * Pochází-li poslední hlavička z pásky, vezme tělo z téhož bloku
 * (cmthack_tape_read_body(): pak posun pásky na další blok s MZF); jinak
 * čte tělo z rozčteného souboru MZF. Bez načtené hlavičky Break.
 *
 * @pre Vlákno emulátoru, uvnitř instrukce OUT (02h) se zapnutým CMT hack patchem.
 * @post ROM dostala výsledek v registrech (OK, chyba, Break).
 */
void cmthack_read_mzf_body(void)
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Pokrytí kódu: hack zasáhl během právě vykonávané instrukce (OUT). */
    coverage_hook_note_cmt_trap();
#endif
    unsigned reg_hl;
    unsigned reg_bc;

    /* Hlavička z pásky: tělo z téhož bloku. */
    if (s_tape.mzf)
    {
        cmthack_tape_read_body(z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL), z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC));
        return;
    };

    iface_audio_pause_emulation(1);
    /* CMT-hack stall: čtení MZF body z disku stojí emulaci - vyloučit z benchmarku */
    emulator_measuring_maxspeed_stall_begin();

    /* Mame otevren nejaky MZF? */
    if (!(g_cmthack.mzf_handler.status & HANDLER_STATUS_READY))
    {
        /* Zruseno: nastavit Err + Break */
        cmthack_result(LOADRET_BREAK);
        iface_audio_pause_emulation(0);
        emulator_measuring_frame_timing_reset();
        emulator_measuring_maxspeed_stall_end();
        return;
    };

    /* precteme prvnich regBC bajtu z MZF souboru a ulozime je do RAM na adresu z regHL */
    reg_hl = z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL);
    reg_bc = z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC);

    /* precteme prvnich regBC bajtu z MZF body a ulozime je do RAM na adresu z regHL */
    uint8_t data[0x10000];
    if (EXIT_SUCCESS != mzf_read_body(&g_cmthack.mzf_handler, data, reg_bc))
    {
        /* Vadny soubor: nastavit Err + Checksum */
        baseui_error("CMT HACK can't load MZF body '%s': %s, gdriver_err: %s\n", g_cmthack.last_filename, strerror(errno), mzf_error_message(&g_cmthack.mzf_handler, g_cmthack.mzf_handler.driver));
        generic_driver_close(&g_cmthack.mzf_handler);
        cmthack_result(LOADRET_ERROR);
        iface_audio_pause_emulation(0);
        emulator_measuring_frame_timing_reset();
        emulator_measuring_maxspeed_stall_end();
        return;
    };

    CMTHACK_RAMLOG_LOAD(memory_load_block(data, reg_hl, reg_bc, MEMORY_LOAD_MAPED));

    printf("\nCMT hack: load 0x%04x bytes from MZF body to addr 0x%04x.\n", reg_bc, reg_hl);

    /* Nacteno OK */
    generic_driver_close(&g_cmthack.mzf_handler);
    cmthack_result(LOADRET_OK);
    iface_audio_pause_emulation(0);
    emulator_measuring_frame_timing_reset();
    emulator_measuring_maxspeed_stall_end();
}


/** @brief Adresa hlavičky, kterou WHEAD ROM zapisuje (LD HL,10F0h na 043Eh); past těla ji bere bez hlavičky z WHEAD. */
#define CMTHACK_SAVE_HEAD_ADDR 0x10F0u
/** @brief Registr E při WHEAD (LD E,0CCh na 043Ch): zápis hlavičky. */
#define CMTHACK_SAVE_E_HEAD 0xCCu
/** @brief Registr E při WDATA (LD E,53h na 047Bh): zápis těla. */
#define CMTHACK_SAVE_E_DATA 0x53u


/**
 * @brief Přečte bajty z paměti tak, jak je vidí CPU (bez vedlejších efektů).
 * @param addr Začátek (16bitová adresa, za FFFFh pokračuje od 0000h).
 * @param dst Cíl. @param len Počet bajtů.
 */
static void cmthack_read_cpu_memory(unsigned addr, uint8_t *dst, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        dst[i] = memory_read_byte_no_se((uint16_t)((addr + i) & 0xFFFFu));
}


/**
 * @brief Uloží MZF z hlavičky (pořadí souboru) a těla na pásku a dá ROM výsledek.
 * @param raw Hlavička tak, jak je v paměti. @param body Tělo (smí být NULL při @p size 0).
 * @param size Délka těla = fsize uloženého bloku.
 * @post ROM dostala OK, nebo Break (chyba vypsaná na stderr cmt_direct_save_mzf()).
 */
static void cmthack_save_store(const uint8_t *raw, uint8_t *body, unsigned size)
{
    st_MZF mzf;
    memcpy(&mzf.header, raw, sizeof(st_MZF_HEADER));
    mzf_header_swap_endianity(&mzf.header); /* do pořadí hostitele */
    mzf.header.fsize = (uint16_t)size;
    mzf.body = body;
    mzf.body_size = size;
    cmthack_result((EXIT_SUCCESS == cmt_direct_save_mzf(&mzf, NULL)) ? LOADRET_OK : LOADRET_BREAK);
}


/**
 * @brief Obslouží past ROM "ulož blok" (port 03h, patch WHEAD / WDATA).
 *
 * E = CCh (WHEAD): 128 bajtů na HL se drží jako hlavička bez těla
 * (s_save; dřívější hlavička bez těla se zahodí s řádkem na stdout). Je-li
 * fsize hlavičky 0, ROM past pro tělo nezavolá (WDATA skončí na 0486h
 * dřív): soubor se uloží hned s prázdným tělem. Výsledek OK.
 *
 * E = 53h (WDATA): tělo BC bajtů na HL. Hlavička je ta z WHEAD; bez ní
 * (program volá jen WDATA, nebo mezi pastmi přišel snapshot) se vezme 128
 * bajtů na 10F0h. Liší-li se BC od fsize hlavičky, uložený blok dostane
 * fsize = BC a na stderr jde varování. Soubor se připojí na konec pásky
 * (cmt_direct_save_mzf()); OK, nebo Break při chybě alokace či připojení.
 *
 * Jiné E: Break a řádek na stderr.
 *
 * Bajty se čtou tak, jak je vidí CPU (memory_read_byte_no_se(): banking,
 * bez breakpointů MEM_R). Záznam vstupů nic nepotřebuje: obsah paměti
 * i pásky nese záznam sám, past se při přehrání chová stejně.
 *
 * @pre Vlákno emulátoru, uvnitř instrukce OUT (03h), CMTHACK_TEST_SAVE_INSTALLED.
 * @post ROM dostala výsledek v AF; hlavička bez těla je zapamatovaná jen po WHEAD s fsize != 0.
 * @par Vedlejší efekty Mění pásku Virtual CMT (cmt_direct_save_mzf(): nová páska, zastavení PLAY,
 *      nový blok), registry AF, výpis na stdout / stderr.
 */
void cmthack_save_block(void)
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Pokrytí kódu: hack zasáhl během právě vykonávané instrukce (OUT). */
    coverage_hook_note_cmt_trap();
#endif
    unsigned reg_e = (unsigned)z80_get_reg(g_mzarch_main.cpu, Z80_REG_DE) & 0xFFu;
    unsigned reg_hl = (unsigned)z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL);
    unsigned reg_bc = (unsigned)z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC);

    if (reg_e == CMTHACK_SAVE_E_HEAD)
    {
        if (s_save.pending)
            printf("CMT hack: the previous saved header got no body, it is dropped\n");
        cmthack_read_cpu_memory(reg_hl, s_save.raw, sizeof(s_save.raw));
        s_save.pending = true;
        st_MZF_HEADER hdr;
        memcpy(&hdr, s_save.raw, sizeof(hdr));
        mzf_header_swap_endianity(&hdr);
        if (hdr.fsize == 0)
        {
            /* WDATA s délkou 0 past nezavolá: soubor celý hned */
            cmthack_save_drop();
            cmthack_save_store(s_save.raw, NULL, 0);
            return;
        };
        cmthack_result(LOADRET_OK);
        return;
    };

    if (reg_e == CMTHACK_SAVE_E_DATA)
    {
        uint8_t raw[sizeof(st_MZF_HEADER)];
        if (s_save.pending)
        {
            memcpy(raw, s_save.raw, sizeof(raw));
        }
        else
        {
            printf("CMT hack: no header from WHEAD, using the header at 0x%04x\n", CMTHACK_SAVE_HEAD_ADDR);
            cmthack_read_cpu_memory(CMTHACK_SAVE_HEAD_ADDR, raw, sizeof(raw));
        };
        cmthack_save_drop();

        st_MZF_HEADER hdr;
        memcpy(&hdr, raw, sizeof(hdr));
        mzf_header_swap_endianity(&hdr);
        if (hdr.fsize != reg_bc)
            fprintf(stderr, "CMT hack: the header says 0x%04x bytes, ROM saves 0x%04x bytes - the block gets fsize 0x%04x\n",
                    (unsigned)hdr.fsize, reg_bc, reg_bc);

        uint8_t *body = NULL;
        if (reg_bc)
        {
            body = (uint8_t *)g_try_malloc(reg_bc);
            if (!body)
            {
                fprintf(stderr, "CMT hack: can't allocate 0x%04x bytes to save the body - Break\n", reg_bc);
                cmthack_result(LOADRET_BREAK);
                return;
            };
            cmthack_read_cpu_memory(reg_hl, body, reg_bc);
        };
        cmthack_save_store(raw, body, reg_bc);
        g_free(body);
        return;
    };

    fprintf(stderr, "CMT hack: unknown save request (E = 0x%02x) - Break\n", reg_e);
    cmthack_save_drop();
    cmthack_result(LOADRET_BREAK);
}


/**
 * @brief Obslouží past ROM "ověř blok" (port 04h, patch VERFY): BC bajtů na HL proti tělu otevřeného bloku.
 *
 * Postup:
 * - deck v RECORD: Break (pojistka; v RECORD patch v ROM není);
 * - načetla-li past čtení hlavičku z pásky a tělo ještě ne (monitor V
 *   čte RHEAD a pak VERFY), porovná s MZF téže hlavičky a hlavičku bez
 *   těla zapomene bez posunu pásky (druhé V nebo LOAD najde týž blok);
 * - jinak: bez pásky nebo bez otevřeného bloku Break; blok bez MZF
 *   (cmttmz_block_get_mzf() selže) Break; obojí s řádkem na stderr;
 * - transport v PLAY (i v pauze) zastaví cmt_stop() jako past čtení (jen
 *   je-li s čím porovnat);
 * - tělo kratší než BC: chyba (A = 1); jinak porovná prvních BC bajtů
 *   těla s pamětí tak, jak ji vidí CPU: shoda OK, rozdíl chyba.
 * Páska se neposouvá; konec pásky zjištěný pastí čtení zůstává.
 *
 * @pre Vlákno emulátoru, uvnitř instrukce OUT (04h), CMTHACK_TEST_VERIFY_INSTALLED.
 * @post ROM dostala výsledek v AF (OK, chyba A = 1, Break A = 2).
 */
void cmthack_verify_block(void)
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Pokrytí kódu: hack zasáhl během právě vykonávané instrukce (OUT). */
    coverage_hook_note_cmt_trap();
#endif
    unsigned reg_hl = (unsigned)z80_get_reg(g_mzarch_main.cpu, Z80_REG_HL);
    unsigned reg_bc = (unsigned)z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC);

    if (CMT_TEST_RECORD)
    {
        fprintf(stderr, "CMT hack: VERIFY while the deck is recording - Break\n");
        cmthack_result(LOADRET_BREAK);
        return;
    };
    int block;
    st_MZF *mzf;
    if (s_tape.pending && s_tape.mzf)
    {
        /* hlavičku už načetla past čtení (monitor V čte RHEAD a pak VERFY):
         * porovnat s tímtéž MZF a hlavičku bez těla zapomenout bez posunu
         * pásky, jinak by další čtení hlavičky blok přeskočilo */
        mzf = s_tape.mzf;
        block = s_tape.pending_block;
        s_tape.mzf = NULL;
        s_tape.pending = false;
    }
    else
    {
        block = cmthack_tape_block();
        if (block < 0)
        {
            fprintf(stderr, "CMT hack: VERIFY without a tape block to compare with - Break\n");
            cmthack_result(LOADRET_BREAK);
            return;
        };
        mzf = cmttmz_block_get_mzf(block);
        if (!mzf)
        {
            fprintf(stderr, "CMT hack: VERIFY - tape block %d has no MZF file - Break\n", block);
            cmthack_result(LOADRET_BREAK);
            return;
        };
    };
    /* přímé ověření pásku přebírá jako přímé načtení */
    if (!CMT_TEST_STOP)
        cmt_stop(); /* nemění otevřený blok */

    bool ok = (reg_bc <= mzf->body_size);
    unsigned diff = 0;
    for (unsigned i = 0; ok && (i < reg_bc); i++)
    {
        if (memory_read_byte_no_se((uint16_t)((reg_hl + i) & 0xFFFFu)) != mzf->body[i])
        {
            ok = false;
            diff = i;
        };
    };
    if (ok)
        printf("CMT hack: verify 0x%04x bytes at 0x%04x against tape block %d - OK\n", reg_bc, reg_hl, block);
    else if (reg_bc > mzf->body_size)
        printf("CMT hack: verify - tape block %d has a body of 0x%04x bytes, ROM verifies 0x%04x bytes - error\n",
               block, (unsigned)mzf->body_size, reg_bc);
    else
        printf("CMT hack: verify 0x%04x bytes at 0x%04x against tape block %d - differs at offset 0x%04x\n",
               reg_bc, reg_hl, block, diff);
    mzf_free(mzf);
    cmthack_result(ok ? LOADRET_OK : LOADRET_ERROR);
}
