/**
 * @file test_cmthack_save.c
 * @brief Unit testy Direct Save CMT hacku: patch zápisových rutin ROM a pasti OUT 03h / 04h.
 *
 * Patch (cmthack_save_patch_signature(), cmthack_save_patch_install(),
 * cmthack_save_patch_uninstall()) nad všemi vestavěnými obrazy dolní ROM
 * (MZ-800 1Z-013B, JSS 1.03 / 1.05C / 1.06A / 1.08C, Willy, MZ-700 13A
 * a 9B, MZ-1500 a MZ-NEWMON; obrazy MZ-700 a MZ-1500 jsou do testu
 * přilinkované jako data): výsledek signatury, XOR 0000h-0FFFh se
 * instalací nemění, bajty mimo rozsahy patche zůstávají, výplň patche
 * ověření jsou jen neškodné instrukce, odinstalace vrátí obraz bajtově.
 * Willy a MZ-NEWMON jsou INCOMP (kód architektury je nepatchuje), funkce
 * se na ně volá jen kvůli výsledku signatury.
 *
 * Pasti volá test přímo s registry, jaké nastaví patch ROM (E = CCh / 53h,
 * HL, BC), a čte výsledek z AF (CY = 0 OK; CY = 1, A = 1 chyba, A = 2
 * Break). Dále: patch zápisu jde s patchem čtení (zapnutí a vypnutí hacku,
 * nastavení direct_save, ROM bez podpory hacku) a není v ROM při RECORD,
 * zásah menu cmt_request_set_direct_save() jen ve STOP, vstup do patche
 * z návratových adres původní rutiny, VERIFY nad blokem bez MZF a načtení
 * snapshotu mezi WHEAD a WDATA, VERIFY po přečtení hlavičky pastí čtení
 * (monitor V) a přesun PC při odstranění patche, uvnitř kterého CPU stojí.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmthack.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "emulator/emulator.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include <glib/gstdio.h>
#include "hw-generic/cmt/cmt_request.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/memory/rom_config.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"
#include "libs/mzf/mzf.h"

/* Vestavěné obrazy dolní ROM (src/emulator/mzarch/<stroj>/memory/ROM/). */
extern uint8_t c_ROM_MZ800_0000[];
extern uint8_t c_ROM_JSS103_0000[];
extern uint8_t c_ROM_JSS105C_0000[];
extern uint8_t c_ROM_JSS106A_0000[];
extern uint8_t c_ROM_JSS108C_0000[];
extern uint8_t c_ROM_WILLY_0000[];
extern uint8_t c_ROM_MZ700_13A_0000[];
extern uint8_t c_ROM_MZ700_9B_0000[];
extern uint8_t c_ROM_MZ1500_0000[];
extern uint8_t c_ROM_MZNEWMON_0000[];

/** @brief Velikost dolní ROM. */
#define LOWER_ROM_SIZE 0x1000u
/** @brief Obě části patche. */
#define PARTS_ALL (CMTHACK_SAVE_PART_SAVE | CMTHACK_SAVE_PART_VERIFY)

/** @brief Obraz ROM a očekávaný výsledek signatury. */
typedef struct
{
    const char *name;      /**< jméno pro hlášení */
    const uint8_t *image;  /**< obraz 0000h-0FFFh */
    unsigned parts;        /**< očekávaná maska cmthack_save_patch_signature() */
} st_ROM_CASE;

/** @brief Všechny vestavěné obrazy dolní ROM. */
static const st_ROM_CASE s_roms[] = {
    {"MZ-800 1Z-013B", c_ROM_MZ800_0000, PARTS_ALL},
    {"JSS 1.03", c_ROM_JSS103_0000, PARTS_ALL},
    {"JSS 1.05C", c_ROM_JSS105C_0000, PARTS_ALL},
    {"JSS 1.06A", c_ROM_JSS106A_0000, PARTS_ALL},
    {"JSS 1.08C", c_ROM_JSS108C_0000, PARTS_ALL},
    /* INCOMP v konfiguraci ROM (hack jiný kód); signatura zápisových rutin přesto sedí */
    {"Willy", c_ROM_WILLY_0000, PARTS_ALL},
    {"MZ-700 13A", c_ROM_MZ700_13A_0000, PARTS_ALL},
    {"MZ-700 9B", c_ROM_MZ700_9B_0000, PARTS_ALL},
    {"MZ-1500", c_ROM_MZ1500_0000, PARTS_ALL},
    /* INCOMP: jiné rutiny kazety */
    {"MZ-NEWMON", c_ROM_MZNEWMON_0000, 0},
};

/** @brief Adresa hlavičky v RAM pro pasti. */
#define HDR_ADDR 0x2000
/** @brief Adresa těla v RAM pro pasti. */
#define BODY_ADDR 0x3000
/** @brief Délka testovacího těla. */
#define BODY_SIZE 64

/**
 * @brief Bajt paměti zapisovaný přes mapu zápisu (jako memory_load_block()).
 */
#define MEM(addr) (g_memory.memram_write[((addr) & 0xFFFF) >> 12][(addr) & 0x0FFF])

/** @brief Stav před testem (obnovuje tearDown). */
static unsigned s_saved_hack;
static unsigned s_saved_direct_save;
static st_ROM_CONFIG_ROW *s_saved_rom;


/** @brief XOR bajtů. @param p Data. @param n Délka. @return XOR. */
static uint8_t xor_of(const uint8_t *p, unsigned n)
{
    uint8_t x = 0;
    for (unsigned i = 0; i < n; i++)
        x ^= p[i];
    return x;
}


/** @brief Leží adresa v rozsahu patche zápisu nebo ověření? @param a Adresa. @return true = v rozsahu. */
static bool in_patch(unsigned a)
{
    return (a >= 0x0447 && a <= 0x044B) || (a >= 0x045E && a <= 0x0463) || (a >= 0x059E && a <= 0x05AA);
}


/** @brief Je bajt neškodná výplň (NOP, LD r,r' s cílem B-L a zdrojem registru, EX DE,HL)? @param op Bajt. @return true = ano. */
static bool harmless_filler(uint8_t op)
{
    if (op == 0x00 || op == 0xEB)
        return true;
    return (op >= 0x40) && (op <= 0x6F) && ((op & 0x07) != 0x06);
}


void setUp(void)
{
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    s_saved_hack = g_cmthack.load_patch_installed;
    s_saved_direct_save = g_cmthack.direct_save;
    s_saved_rom = g_rom.rom_cfg;
    cmt_eject();
    cmthack_reset();
    /* výchozí stav nové konfigurace: hack a Direct Save zapnuté */
    cmthack_direct_save_set(1);
    cmthack_load_rom_patch(1);
}


void tearDown(void)
{
    cmt_eject();
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    cmthack_reset();
    if (g_rom.rom_cfg != s_saved_rom)
        rom_reinstall(s_saved_rom);
    cmthack_direct_save_set(s_saved_direct_save);
    cmthack_load_rom_patch(s_saved_hack);
}


/* ---------------------------------------------------------------------- */
/*  Patch nad obrazy ROM                                                  */
/* ---------------------------------------------------------------------- */

/* Signatura podle očekávání pro všechny vestavěné obrazy. */
void test_signature_all_roms(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    for (size_t i = 0; i < sizeof(s_roms) / sizeof(s_roms[0]); i++)
        TEST_ASSERT_EQUAL_UINT_MESSAGE(s_roms[i].parts, cmthack_save_patch_signature(s_roms[i].image), s_roms[i].name);
}


/* Instalace: XOR 0000h-0FFFh beze změny, bajty patche, okolí beze změny, odinstalace vrátí obraz. */
void test_install_keeps_xor_and_uninstall_restores(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    for (size_t i = 0; i < sizeof(s_roms) / sizeof(s_roms[0]); i++)
    {
        const st_ROM_CASE *c = &s_roms[i];
        uint8_t rom[LOWER_ROM_SIZE];
        memcpy(rom, c->image, LOWER_ROM_SIZE);
        unsigned parts = cmthack_save_patch_install(rom, c->image);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(c->parts, parts, c->name);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(xor_of(c->image, LOWER_ROM_SIZE), xor_of(rom, LOWER_ROM_SIZE), c->name);
        for (unsigned a = 0; a < LOWER_ROM_SIZE; a++)
        {
            if (!in_patch(a))
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(c->image[a], rom[a], c->name);
        };
        if (parts & CMTHACK_SAVE_PART_SAVE)
        {
            static const uint8_t a[] = {0x00, 0x00, 0x00, 0x00, 0x00};
            /* CALL @CHECK na 0444h zůstává */
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&c->image[0x0444], &rom[0x0444], 3, c->name);
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(a, &rom[0x0447], sizeof(a), c->name);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xD3, rom[0x045E], c->name);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x03, rom[0x045F], c->name);
            for (unsigned a2 = 0x0460; a2 <= 0x0463; a2++)
                TEST_ASSERT_TRUE_MESSAGE(harmless_filler(rom[a2]), c->name);
            /* XOR zápisové části sám (výplň za OUT vyrovnává oba rozsahy) */
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(xor_of(&c->image[0x0447], 5) ^ xor_of(&c->image[0x045E], 6),
                                           xor_of(&rom[0x0447], 5) ^ xor_of(&rom[0x045E], 6), c->name);
        }
        else
        {
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(c->image, rom, LOWER_ROM_SIZE, c->name);
        };
        if (parts & CMTHACK_SAVE_PART_VERIFY)
        {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xD3, rom[0x059E], c->name);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x04, rom[0x059F], c->name);
            unsigned nonzero = 0;
            for (unsigned a = 0x05A0; a <= 0x05AA; a++)
            {
                TEST_ASSERT_TRUE_MESSAGE(harmless_filler(rom[a]), c->name);
                if (rom[a])
                    nonzero++;
            };
            TEST_ASSERT_TRUE_MESSAGE(nonzero <= 4, c->name);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(xor_of(&c->image[0x059E], 13), xor_of(&rom[0x059E], 13), c->name);
        };
        cmthack_save_patch_uninstall(rom, c->image);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(c->image, rom, LOWER_ROM_SIZE, c->name);
    };
}


/**
 * @brief Délka instrukce původního kódu ROM v 044Ch-045Dh (jen opkódy, které tam jsou).
 * @param op Opkód. @return Délka v bajtech, 0 = neznámý opkód.
 */
static unsigned orig_insn_len(uint8_t op)
{
    switch (op)
    {
        case 0x7B: /* LD A,E */
        case 0xD5: /* PUSH DE */
        case 0xDF: /* RST 18h */
        case 0xD1: /* POP DE */
            return 1;
        case 0xFE: /* CP n */
        case 0x20: /* JR NZ,e */
            return 2;
        case 0xCD: /* CALL nn */
        case 0x11: /* LD DE,nn */
            return 3;
        default:
            return 0;
    };
}


/**
 * @brief Projde kód s patchem od @p entry a ověří, že dojde na původní JP CMTEND (0464h) / JR CMTEND (05ABh).
 *
 * Bajty patche smí být jen neškodné jednobajtové instrukce nebo celé
 * OUT (n),A (operand se nesmí provést jako opkód). Původní kód
 * 044Ch-045Dh se prochází podle délek instrukcí; JR NZ na 044Fh vede
 * na 045Eh, takže obě cesty končí na OUT (03h).
 *
 * @param rom Dolní ROM s patchem. @param entry Vstupní adresa.
 * @param name Jméno ROM pro hlášení.
 */
static void walk_to_cmtend(const uint8_t *rom, unsigned entry, const char *name)
{
    unsigned a = entry;
    for (unsigned steps = 0; steps < 64; steps++)
    {
        if ((a == 0x0464) || (a == 0x05AB))
        {
            /* původní skok na CMTEND */
            TEST_ASSERT_TRUE_MESSAGE((a == 0x0464) ? (rom[a] == 0xC3) : (rom[a] == 0x18), name);
            return;
        };
        TEST_ASSERT_FALSE_MESSAGE((a == 0x045F) || (a == 0x059F), name); /* operand OUT jako opkód */
        if ((a == 0x045E) || (a == 0x059E))
        {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xD3, rom[a], name);
            a += 2;
            continue;
        };
        if (in_patch(a))
        {
            TEST_ASSERT_TRUE_MESSAGE(harmless_filler(rom[a]), name);
            a += 1;
            continue;
        };
        if ((a >= 0x044C) && (a <= 0x045D))
        {
            unsigned len = orig_insn_len(rom[a]);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, len, name);
            a += len;
            continue;
        };
        TEST_FAIL_MESSAGE(name); /* mimo známý kód */
    };
    TEST_FAIL_MESSAGE(name);
}


/*
 * Patch nainstalovaný, zatímco CPU je uvnitř původní rutiny (konec
 * nahrávání během SAVE nebo VERIFY): návratové adresy 0447h, 044Ah
 * (za @CHECK / @MGON), 0461h (za @WTMRK), 0464h a u ověření 05A1h, 05A6h,
 * 05ABh padnou do patche a rutina doběhne na původní skok do CMTEND, bez
 * provedení operandu OUT jako opkódu. Všechny změněné bajty kromě obou
 * OUT a jejich operandů jsou neškodné jednobajtové instrukce.
 */
void test_patch_entry_from_original_routine(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const unsigned entries[] = {0x0447, 0x044A, 0x0461, 0x0464, 0x05A1, 0x05A6, 0x05AB};
    for (size_t i = 0; i < sizeof(s_roms) / sizeof(s_roms[0]); i++)
    {
        const st_ROM_CASE *c = &s_roms[i];
        if (c->parts != PARTS_ALL)
            continue;
        uint8_t rom[LOWER_ROM_SIZE];
        memcpy(rom, c->image, LOWER_ROM_SIZE);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(PARTS_ALL, cmthack_save_patch_install(rom, c->image), c->name);
        for (unsigned a = 0; a < LOWER_ROM_SIZE; a++)
        {
            if (!in_patch(a) || (a == 0x045E) || (a == 0x045F) || (a == 0x059E) || (a == 0x059F))
                continue;
            TEST_ASSERT_TRUE_MESSAGE(harmless_filler(rom[a]), c->name);
        };
        for (size_t e = 0; e < sizeof(entries) / sizeof(entries[0]); e++)
            walk_to_cmtend(rom, entries[e], c->name);
    };
}


/* Signatura ověření nesedí: jen zápis. Signatura zápisu nesedí: nic (ani ověření). */
void test_partial_signature(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    uint8_t orig[LOWER_ROM_SIZE], rom[LOWER_ROM_SIZE];

    memcpy(orig, c_ROM_MZ800_0000, LOWER_ROM_SIZE);
    orig[0x05AC] ^= 0xFF; /* cíl JR CMTEND za rozsahem patche ověření */
    memcpy(rom, orig, LOWER_ROM_SIZE);
    TEST_ASSERT_EQUAL_UINT(CMTHACK_SAVE_PART_SAVE, cmthack_save_patch_install(rom, orig));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&orig[0x059E], &rom[0x059E], 13);
    TEST_ASSERT_EQUAL_HEX8(0xD3, rom[0x045E]);

    memcpy(orig, c_ROM_MZ800_0000, LOWER_ROM_SIZE);
    orig[0x047B] ^= 0xFF; /* LD E,53h ve WDATA */
    memcpy(rom, orig, LOWER_ROM_SIZE);
    TEST_ASSERT_EQUAL_UINT(CMTHACK_SAVE_PART_VERIFY, cmthack_save_patch_signature(orig));
    TEST_ASSERT_EQUAL_UINT(0, cmthack_save_patch_install(rom, orig));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, rom, LOWER_ROM_SIZE);
}


/* ---------------------------------------------------------------------- */
/*  Patch v ROM stroje podle hacku a nastavení                            */
/* ---------------------------------------------------------------------- */

/* Patch zápisu jen s hackem a s direct_save; vypnutí vrátí bajty z obrazu. */
void test_patch_follows_hack_and_setting(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_UINT(1, g_cmthack.load_patch_installed);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_VERIFY_INSTALLED);
    TEST_ASSERT_EQUAL_HEX8(0xD3, g_memory.ROM[0x045E]);
    TEST_ASSERT_EQUAL_HEX8(0x03, g_memory.ROM[0x045F]);
    TEST_ASSERT_EQUAL_HEX8(0x04, g_memory.ROM[0x059F]);

    cmthack_direct_save_set(0);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_VERIFY_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&g_rom.mz700rom[0x0447], &g_memory.ROM[0x0447], 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&g_rom.mz700rom[0x045E], &g_memory.ROM[0x045E], 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&g_rom.mz700rom[0x059E], &g_memory.ROM[0x059E], 13);

    cmthack_direct_save_set(1);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);

    /* vypnutí hacku: celá dolní ROM zpět jako obraz */
    cmthack_load_rom_patch(0);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_VERIFY_INSTALLED);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(g_rom.mz700rom, g_memory.ROM, LOWER_ROM_SIZE);

    /* bez hacku si nastavení jen pamatuje volbu */
    cmthack_direct_save_set(0);
    cmthack_direct_save_set(1);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(g_rom.mz700rom, g_memory.ROM, LOWER_ROM_SIZE);
    cmthack_load_rom_patch(1);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_TRUE(cmthack_save_rom_supported());

    /* RECORD: patch zápisu z ROM pryč (signál se nahrává), po STOP zpět */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_VERIFY_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(1, g_cmthack.direct_save);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&g_rom.mz700rom[0x0447], &g_memory.ROM[0x0447], 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&g_rom.mz700rom[0x045E], &g_memory.ROM[0x045E], 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&g_rom.mz700rom[0x059E], &g_memory.ROM[0x059E], 13);
    /* volba v RECORD se jen zapamatuje */
    cmthack_direct_save_set(0);
    cmthack_direct_save_set(1);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    cmt_stop();
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_VERIFY_INSTALLED);
    TEST_ASSERT_EQUAL_HEX8(0xD3, g_memory.ROM[0x045E]);
}


/**
 * @brief Najde řádek konfigurace ROM podle jména.
 * @param name Jméno řádku (rom_config). @return Řádek (test selže, není-li).
 */
static st_ROM_CONFIG_ROW *rom_row(const char *name)
{
    for (st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++)
    {
        if (r->type == ROM_CONFIG_TYPE_ROM && r->name && (0 == strcmp(r->name, name)))
            return r;
    };
    TEST_FAIL_MESSAGE(name);
    return NULL;
}


/* Výměna ROM: JSS dostane patch se stejným XOR, Willy (INCOMP) hack i patch zápisu vypne. */
void test_rom_change(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    rom_reinstall(rom_row("JSS108C"));
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    uint8_t x_with = xor_of(g_memory.ROM, LOWER_ROM_SIZE);
    cmthack_direct_save_set(0);
    TEST_ASSERT_EQUAL_HEX8(x_with, xor_of(g_memory.ROM, LOWER_ROM_SIZE));
    cmthack_direct_save_set(1);

    rom_reinstall(rom_row("WILLY_EN"));
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_VERIFY_INSTALLED);
    TEST_ASSERT_FALSE(cmthack_save_rom_supported());
}


/* Zásah menu: jen ve STOP (v PLAY odmítnut beze změny), při záznamu vstupů odmítnut. */
void test_request_only_in_stop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_DEVICE_REQ_RESULT res;

    g_cmt.state = CMT_STATE_PLAY;
    TEST_ASSERT_NOT_EQUAL(0, cmt_request_set_direct_save(false));
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_DIRECT_SAVE_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    TEST_ASSERT_EQUAL_INT(-1, res.rc);
    TEST_ASSERT_EQUAL_UINT(1, g_cmthack.direct_save);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    g_cmt.state = CMT_STATE_STOP;

    cmt_request_set_direct_save(false);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    TEST_ASSERT_EQUAL_UINT(0, g_cmthack.direct_save);
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);

    g_replay_mode = REPLAY_MODE_RECORDING;
    cmt_request_set_direct_save(true);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, res.status);
    TEST_ASSERT_EQUAL_UINT(0, g_cmthack.direct_save);
    g_replay_mode = REPLAY_MODE_OFF;

    cmt_request_set_direct_save(true);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(1, res.rc);
}


/* ---------------------------------------------------------------------- */
/*  Pasti OUT 03h a 04h                                                   */
/* ---------------------------------------------------------------------- */

/** @brief Bajt těla na pozici i. @param seed Odlišení souborů. @param i Pozice. @return Bajt. */
static uint8_t body_byte(int seed, unsigned i)
{
    return (uint8_t)((i * 41 + 7 + 13 * seed) & 0xFF);
}


/**
 * @brief Zapíše hlavičku (typ 01h, jméno "D<seed>", fsize) na addr a tělo na BODY_ADDR.
 * @param addr Adresa hlavičky. @param seed Odlišení. @param fsize fsize v hlavičce.
 * @param body_len Délka zapsaného těla.
 */
static void put_file(unsigned addr, int seed, uint16_t fsize, unsigned body_len)
{
    for (unsigned i = 0; i < 128; i++)
        MEM(addr + i) = 0;
    MEM(addr) = 0x01;
    for (unsigned i = 1; i <= 17; i++)
        MEM(addr + i) = 0x0D;
    MEM(addr + 1) = 'D';
    MEM(addr + 2) = (uint8_t)('0' + seed);
    MEM(addr + 0x12) = (uint8_t)(fsize & 0xFF);
    MEM(addr + 0x13) = (uint8_t)(fsize >> 8);
    MEM(addr + 0x14) = (uint8_t)(BODY_ADDR & 0xFF);
    MEM(addr + 0x15) = (uint8_t)(BODY_ADDR >> 8);
    for (unsigned i = 0; i < body_len; i++)
        MEM(BODY_ADDR + i) = body_byte(seed, i);
}


/**
 * @brief Past OUT 03h s registry E, HL, BC.
 * @param e Registr E. @param hl HL. @param bc BC. @return AF po pasti.
 */
static uint16_t trap_save(uint8_t e, uint16_t hl, uint16_t bc)
{
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_DE, (uint16_t)(0xD700 | e));
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, hl);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_BC, bc);
    cmthack_save_block();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}


/**
 * @brief Past OUT 04h: BC bajtů na BODY_ADDR.
 * @param bc Délka. @return AF po pasti.
 */
static uint16_t trap_verify(uint16_t bc)
{
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, BODY_ADDR);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_BC, bc);
    cmthack_verify_block();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}

/** @brief AF = OK. @param af AF. */
static void assert_ok(uint16_t af)
{
    TEST_ASSERT_EQUAL_HEX16(0, af & 0x01);
}

/** @brief AF = chyba (CY, A = 1). @param af AF. */
static void assert_error(uint16_t af)
{
    TEST_ASSERT_EQUAL_HEX16(0x01, af & 0x01);
    TEST_ASSERT_EQUAL_HEX8(1, af >> 8);
}

/** @brief AF = Break (CY, A = 2). @param af AF. */
static void assert_break(uint16_t af)
{
    TEST_ASSERT_EQUAL_HEX16(0x01, af & 0x01);
    TEST_ASSERT_EQUAL_HEX8(2, af >> 8);
}

/** @brief Počet bloků vložené pásky (0 bez pásky). @return Počet. */
static int block_count(void)
{
    const st_TZX_FILE *f = cmttmz_get_file();
    return (CMT_TEST_FILLED && f) ? (int)f->block_count : 0;
}

/** @brief Otevřený blok (-1 bez bloku). @return Číslo bloku. */
static int open_block(void)
{
    return (g_cmt.ext && g_cmt.ext->block) ? cmtext_block_get_block_id(g_cmt.ext->block) : -1;
}


/**
 * @brief Ověří MZF bloku: jméno D<seed>, fsize, tělo.
 * @param block Blok. @param seed Odlišení. @param fsize Očekávané fsize a délka těla.
 */
static void assert_block(int block, int seed, uint16_t fsize)
{
    st_MZF *m = cmttmz_block_get_mzf(block);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_HEX8(0x01, m->header.ftype);
    TEST_ASSERT_EQUAL_HEX8('D', m->header.fname.name[0]);
    TEST_ASSERT_EQUAL_HEX8('0' + seed, m->header.fname.name[1]);
    TEST_ASSERT_EQUAL_UINT16(fsize, m->header.fsize);
    TEST_ASSERT_EQUAL_UINT32(fsize, m->body_size);
    for (unsigned i = 0; i < fsize; i++)
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(body_byte(seed, i), m->body[i], "body byte");
    mzf_free(m);
}


/* Bez pásky: WHEAD + WDATA vloží novou pásku s blokem MZF, druhý soubor = další blok. */
void test_save_into_new_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_CMT_DIRECT_SAVE_RESULT r0, r;
    cmt_direct_save_get_result(&r0);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    put_file(HDR_ADDR, 1, BODY_SIZE, BODY_SIZE);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    TEST_ASSERT_FALSE(CMT_TEST_FILLED); /* jen hlavička: zatím nic */
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, block_count());
    TEST_ASSERT_EQUAL_INT(0, open_block());
    assert_block(0, 1, BODY_SIZE);
    cmt_direct_save_get_result(&r);
    TEST_ASSERT_EQUAL_UINT(r0.serial + 1, r.serial);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT(0, r.block_id);
    TEST_ASSERT_EQUAL_UINT32(BODY_SIZE, r.size);
    TEST_ASSERT_EQUAL_STRING("D1", r.name);

    put_file(HDR_ADDR, 2, BODY_SIZE, BODY_SIZE);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    TEST_ASSERT_EQUAL_INT(2, block_count());
    TEST_ASSERT_EQUAL_INT(1, open_block());
    assert_block(1, 2, BODY_SIZE);
}


/* fsize 0: soubor hned po hlavičce s prázdným tělem. */
void test_save_empty_body_at_header(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    put_file(HDR_ADDR, 3, 0, 0);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    TEST_ASSERT_EQUAL_INT(1, block_count());
    assert_block(0, 3, 0);
}


/* BC jiný než fsize hlavičky: blok dostane fsize = BC. */
void test_save_bc_differs_from_fsize(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    put_file(HDR_ADDR, 4, 200, BODY_SIZE);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    assert_block(0, 4, BODY_SIZE);
}


/* WDATA bez WHEAD: hlavička z 10F0h; reset hlavičku z WHEAD zapomene. */
void test_save_body_without_header_uses_10f0(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    put_file(0x10F0, 5, BODY_SIZE, BODY_SIZE);
    if (memory_read_byte_no_se(0x10F1) != 'D')
        TEST_IGNORE_MESSAGE("10F0h is not RAM in the current memory map of the test machine");
    put_file(HDR_ADDR, 6, BODY_SIZE, 0);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80)); /* hlavička D6 čeká na tělo */
    cmthack_reset();                             /* ... a reset ji zapomene */
    for (unsigned i = 0; i < BODY_SIZE; i++)
        MEM(BODY_ADDR + i) = body_byte(5, i);
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    assert_block(0, 5, BODY_SIZE);
}


/* Jiné E = Break, nic se neuloží. */
void test_save_unknown_e_breaks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    assert_break(trap_save(0x42, HDR_ADDR, 0x80));
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
}


/* PLAY: přímé uložení transport zastaví; RECORD: nahrávání běží dál, blok se připojí. */
void test_save_transport_states(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    put_file(HDR_ADDR, 1, BODY_SIZE, BODY_SIZE);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    put_file(HDR_ADDR, 2, BODY_SIZE, BODY_SIZE);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(2, block_count());

    /* hlavička WHEAD bez těla, pak Record: patch pryč, hlavička zapomenutá */
    put_file(0x10F0, 4, BODY_SIZE, BODY_SIZE);
    put_file(HDR_ADDR, 3, BODY_SIZE, 0); /* tělo na BODY_ADDR zůstane D4 */
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    /* VERIFY při nahrávání (jen přímé volání, port 04h je bez patche odpojený) = Break */
    assert_break(trap_verify(BODY_SIZE));
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(2, block_count()); /* nahrávání bez hran nic nepřidá */
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    if (memory_read_byte_no_se(0x10F1) != 'D')
        TEST_IGNORE_MESSAGE("10F0h is not RAM in the current memory map of the test machine");
    /* tělo bez hlavičky z WHEAD (ta se se vstupem do RECORD zapomněla) vezme 10F0h */
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    assert_block(2, 4, BODY_SIZE);
}


/* VERIFY: shoda OK, rozdíl a delší BC chyba, bez pásky Break; páska se nehýbe. */
void test_verify(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    assert_break(trap_verify(BODY_SIZE)); /* bez pásky */

    put_file(HDR_ADDR, 7, BODY_SIZE, BODY_SIZE);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    assert_ok(trap_verify(BODY_SIZE));
    assert_ok(trap_verify(BODY_SIZE / 2));
    assert_error(trap_verify(BODY_SIZE + 1));
    MEM(BODY_ADDR + 10) ^= 0x5A;
    assert_error(trap_verify(BODY_SIZE));
    MEM(BODY_ADDR + 10) ^= 0x5A;
    cmt_play();
    assert_ok(trap_verify(BODY_SIZE));
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, open_block());
}


/**
 * @brief Uloží TMZ s blokem 0x12 Pure Tone (bez MZF) a otevře ho ve Virtual CMT.
 * @param[out] path Cesta dočasného souboru (g_free, soubor smaže volající).
 */
static void open_tone_tape(gchar **path)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    st_TZX_BLOCK b;
    b.id = TZX_BLOCK_ID_PURE_TONE;
    b.length = 4;
    b.data = (uint8_t *)malloc(4);
    TEST_ASSERT_NOT_NULL(b.data);
    b.data[0] = 0x78; /* 2168 T */
    b.data[1] = 0x08;
    b.data[2] = 0x2C; /* 300 pulzů */
    b.data[3] = 0x01;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    gchar *name = g_strdup_printf("mztest_cmthack_save_%u.tmz", (unsigned)g_random_int());
    *path = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    TEST_ASSERT_TRUE(g_file_set_contents(*path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(*path));
}


/* VERIFY nad blokem bez MZF: Break; hrající páska se u takového bloku nezastaví. */
void test_verify_block_without_mzf(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    gchar *path = NULL;
    open_tone_tape(&path);
    TEST_ASSERT_EQUAL_INT(0, open_block());
    put_file(HDR_ADDR, 1, BODY_SIZE, BODY_SIZE);
    assert_break(trap_verify(BODY_SIZE));
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    assert_break(trap_verify(BODY_SIZE));
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    cmt_stop();
    cmt_eject();
    g_remove(path);
    g_free(path);
}


/* Načtení snapshotu mezi WHEAD a WDATA: hlavička z WHEAD se zapomene, tělo vezme hlavičku z 10F0h. */
void test_snapshot_between_header_and_body(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    put_file(0x10F0, 5, BODY_SIZE, BODY_SIZE);
    if (memory_read_byte_no_se(0x10F1) != 'D')
        TEST_IGNORE_MESSAGE("10F0h is not RAM in the current memory map of the test machine");
    put_file(HDR_ADDR, 6, BODY_SIZE, 0);
    uint8_t *buf = NULL;
    size_t size = 0;
    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("cmthack save", &buf, &size));
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80)); /* hlavička D6 čeká na tělo */
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, size));
    g_free(buf);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    assert_block(0, 5, BODY_SIZE);
}


/** @brief Adresa hlavičky pro past čtení (RHEAD) v testech ověření. */
#define READ_HDR_ADDR 0x2400

/** @brief Past čtení hlavičky (OUT 01h): HL = READ_HDR_ADDR. @return AF po pasti. */
static uint16_t trap_read_header(void)
{
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, READ_HDR_ADDR);
    cmthack_load_file();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}

/** @brief Past čtení těla (OUT 02h): HL = BODY_ADDR, BC = délka. @param bc Délka. @return AF po pasti. */
static uint16_t trap_read_body(uint16_t bc)
{
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, BODY_ADDR);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_BC, bc);
    cmthack_read_mzf_body();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}


/*
 * Monitor V (JSS 1.06A): RHEAD načte hlavičku z pásky, pak VERFY. Ověření
 * porovná s MZF té hlavičky a hlavičku bez těla zapomene bez posunu
 * pásky: druhé V dá znovu OK a páska stojí na bloku; RHEAD + VERFY
 * + RHEAD + RDATA pak načte týž blok.
 */
void test_verify_after_header_read(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    put_file(HDR_ADDR, 1, BODY_SIZE, BODY_SIZE);
    assert_ok(trap_save(0xCC, HDR_ADDR, 0x80));
    assert_ok(trap_save(0x53, BODY_ADDR, BODY_SIZE));
    TEST_ASSERT_EQUAL_INT(0, open_block());
    for (int i = 0; i < 2; i++)
    {
        assert_ok(trap_read_header());
        TEST_ASSERT_EQUAL_HEX8('D', MEM(READ_HDR_ADDR + 1));
        assert_ok(trap_verify(BODY_SIZE));
        TEST_ASSERT_EQUAL_INT(0, open_block());
    };
    /* rozdíl po přečtení hlavičky = chyba, páska dál stojí */
    assert_ok(trap_read_header());
    MEM(BODY_ADDR + 3) ^= 0x5A;
    assert_error(trap_verify(BODY_SIZE));
    MEM(BODY_ADDR + 3) ^= 0x5A;
    TEST_ASSERT_EQUAL_INT(0, open_block());

    assert_ok(trap_read_header());
    assert_ok(trap_verify(BODY_SIZE));
    assert_ok(trap_read_header());
    for (unsigned i = 0; i < BODY_SIZE; i++)
        MEM(BODY_ADDR + i) = 0x00;
    assert_ok(trap_read_body(BODY_SIZE));
    for (unsigned i = 0; i < BODY_SIZE; i++)
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(body_byte(1, i), MEM(BODY_ADDR + i), "body byte");
}


/*
 * Odstranění patche, zatímco PC leží v jeho bajtech: s dolní ROM na 0000h
 * se PC přesune na hranici původní instrukce (0448h-044Bh -> 0447h,
 * 0460h-0463h -> 0464h, 05A0h-05AAh -> 05ABh), na 0447h, 045Eh, 059Eh
 * a jinde zůstane; s RAM na 0000h se nemění. Platí pro vypnutí volby
 * i vstup do RECORD. Test běží poslední: mění mapu paměti (E0h odmapuje
 * i CG-ROM, E2h vrátí jen ROM na 0000h).
 */
void test_patch_removal_moves_pc(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const struct
    {
        uint16_t pc;   /**< PC před odstraněním patche */
        uint16_t want; /**< PC po odstranění */
    } cases[] = {
        {0x0447, 0x0447}, {0x0448, 0x0447}, {0x044A, 0x0447}, {0x044B, 0x0447},
        {0x045E, 0x045E}, {0x0460, 0x0464}, {0x0461, 0x0464}, {0x0463, 0x0464}, {0x0464, 0x0464},
        {0x059E, 0x059E}, {0x05A0, 0x05AB}, {0x05A6, 0x05AB}, {0x05AA, 0x05AB}, {0x05AB, 0x05AB},
        {0x2000, 0x2000},
    };
    const uint16_t saved_pc = (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_PC);

    memory_map_pwrite(0xE2, 0x00); /* ROM na 0000h */
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_ROM_LOW, memmap_query(0));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        cmthack_direct_save_set(1);
        TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_VERIFY_INSTALLED);
        z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, cases[i].pc);
        cmthack_direct_save_set(0);
        TEST_ASSERT_EQUAL_HEX16(cases[i].want, z80_get_reg(g_mzarch_main.cpu, Z80_REG_PC));
    };

    /* vstup do RECORD */
    cmthack_direct_save_set(1);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, 0x0462);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_EQUAL_HEX16(0x0464, z80_get_reg(g_mzarch_main.cpu, Z80_REG_PC));
    cmt_stop();
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);

    /* stav načtený ze snapshotu: patch se srovná, PC ze snapshotu zůstane
     * (0461h je v původním kódu hranice instrukce CALL @WBLOK) */
    cmthack_direct_save_set(1);
    TEST_ASSERT_EQUAL_UINT(1, CMTHACK_TEST_SAVE_INSTALLED);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, 0x0461);
    g_cmthack.direct_save = 0;
    cmthack_save_patch_sync_loaded_state();
    TEST_ASSERT_EQUAL_UINT(0, CMTHACK_TEST_SAVE_INSTALLED);
    TEST_ASSERT_EQUAL_HEX16(0x0461, z80_get_reg(g_mzarch_main.cpu, Z80_REG_PC));
    TEST_ASSERT_EQUAL_HEX8(g_rom.mz700rom[0x0461], g_memory.ROM[0x0461]);
    cmthack_direct_save_set(1);

    /* RAM na 0000h: kód v RAM, PC se nemění */
    memory_map_pwrite(0xE0, 0x00);
    TEST_ASSERT_NOT_EQUAL(MEMMAP_KIND_ROM_LOW, memmap_query(0));
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, 0x0448);
    cmthack_direct_save_set(0);
    TEST_ASSERT_EQUAL_HEX16(0x0448, z80_get_reg(g_mzarch_main.cpu, Z80_REG_PC));
    memory_map_pwrite(0xE2, 0x00);
    cmthack_direct_save_set(1);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_PC, saved_pc);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_signature_all_roms);
    RUN_TEST(test_install_keeps_xor_and_uninstall_restores);
    RUN_TEST(test_patch_entry_from_original_routine);
    RUN_TEST(test_partial_signature);
    RUN_TEST(test_patch_follows_hack_and_setting);
    RUN_TEST(test_rom_change);
    RUN_TEST(test_request_only_in_stop);
    RUN_TEST(test_save_into_new_tape);
    RUN_TEST(test_save_empty_body_at_header);
    RUN_TEST(test_save_bc_differs_from_fsize);
    RUN_TEST(test_save_body_without_header_uses_10f0);
    RUN_TEST(test_save_unknown_e_breaks);
    RUN_TEST(test_save_transport_states);
    RUN_TEST(test_verify);
    RUN_TEST(test_verify_block_without_mzf);
    RUN_TEST(test_snapshot_between_header_and_body);
    RUN_TEST(test_verify_after_header_read);
    RUN_TEST(test_patch_removal_moves_pc); /* poslední: mění mapu paměti */
    return UNITY_END();
}
