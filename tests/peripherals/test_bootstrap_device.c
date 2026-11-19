/**
 * @file test_bootstrap_device.c
 * @brief Kód zařízení v BC při přímém spuštění MZF: zápis volby, tabulka a nastavení.
 *
 * Ověřuje modul bootstrap_device.h:
 *  - bootdev_parse(): jména (velikost písmen, alias sram), desítkově,
 *    `0x` hex, hex s příponou `h`, bílé znaky, mezní hodnoty, mimo rozsah,
 *    nesmysly, prázdný text, NULL, příliš dlouhý text,
 *  - bootdev_name_bc(): tabulka pro platformy 700 / 800 / 1500 (CMT na
 *    MZ-700 0000h, jinde 0100h; RD 0000h, FD 0200h, QD 0300h),
 *  - bootdev_normalize(): jméno malými písmeny, číslo v původním zápisu,
 *  - nastavení: výchozí `cmt`, zápis platné / odmítnutí neplatné hodnoty,
 *    bootdev_setting_bc() pro MZ-800 (testovací jádro je MZARCH=800),
 *  - klíč INI: element `[CMT] run_mzf_device` existuje (registrace
 *    v cmt_init()), save callback zapíše aktuální text (do dočasného
 *    souboru podstrčeného kořeni konfigurace),
 *  - bootstrap: mzarch_main_reset_and_run_mzf() s volbou i bez ní nastaví
 *    BC (MZF ze souboru v dočasném adresáři).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "cfgmain.h"
#include "mzarch/mzarch.h"
#include "mzarch/bootstrap_device.h"
#include "libs/cpu-z80/z80.h"

#if MZARCH != 800
#error "test_bootstrap_device: testovací jádro je jen pro MZARCH=800"
#endif


/** @brief Text nastavení před testem (obnoví se v tearDown). */
static char s_saved[BOOTDEV_TEXT_MAX];

void setUp(void)
{
    bootdev_setting_get_text(s_saved, sizeof(s_saved));
    TEST_ASSERT_TRUE(bootdev_setting_set_text(BOOTDEV_DEFAULT_TEXT));
}

void tearDown(void)
{
    (void)bootdev_setting_set_text(s_saved);
}


/** @brief Pomocník: rozebere text a vrátí BC pro platformu, -1 = neplatný. */
static int parse_bc(const char *text, unsigned platform)
{
    st_BOOTDEV_CHOICE c;
    if (!bootdev_parse(text, &c)) return -1;
    return (int)bootdev_choice_bc(&c, platform);
}


/**
 * @brief Jména zařízení bez ohledu na velikost písmen, alias sram, bílé znaky.
 */
void test_parse_names(void)
{
    st_BOOTDEV_CHOICE c;
    TEST_ASSERT_TRUE(bootdev_parse("cmt", &c));
    TEST_ASSERT_FALSE(c.custom);
    TEST_ASSERT_EQUAL_INT(BOOTDEV_NAME_CMT, c.name);
    TEST_ASSERT_TRUE(bootdev_parse("CMT", &c));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_NAME_CMT, c.name);
    TEST_ASSERT_TRUE(bootdev_parse("Fd", &c));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_NAME_FD, c.name);
    TEST_ASSERT_TRUE(bootdev_parse("qD", &c));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_NAME_QD, c.name);
    TEST_ASSERT_TRUE(bootdev_parse("rd", &c));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_NAME_RD, c.name);
    TEST_ASSERT_TRUE(bootdev_parse("SRAM", &c));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_NAME_RD, c.name);
    TEST_ASSERT_TRUE(bootdev_parse("  qd\t", &c));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_NAME_QD, c.name);
    TEST_ASSERT_TRUE_MESSAGE(bootdev_parse("cmt", NULL), "validation only (out NULL)");
}


/**
 * @brief Čísla: desítkově, 0x hex, hex s příponou h; meze 0 a 65535.
 */
void test_parse_numbers(void)
{
    TEST_ASSERT_EQUAL_INT(512, parse_bc("512", 800));
    TEST_ASSERT_EQUAL_INT(0x0200, parse_bc("0x0200", 800));
    TEST_ASSERT_EQUAL_INT(0x0200, parse_bc("0X200", 800));
    TEST_ASSERT_EQUAL_INT(0x0200, parse_bc("0200h", 800));
    TEST_ASSERT_EQUAL_INT(0x00FF, parse_bc("0FFH", 800));
    TEST_ASSERT_EQUAL_INT(0xABCD, parse_bc("0xabcd", 800));
    TEST_ASSERT_EQUAL_INT(0, parse_bc("0", 800));
    TEST_ASSERT_EQUAL_INT(0, parse_bc("0x0", 800));
    TEST_ASSERT_EQUAL_INT(65535, parse_bc("65535", 800));
    TEST_ASSERT_EQUAL_INT(0xFFFF, parse_bc("0xFFFF", 800));
    TEST_ASSERT_EQUAL_INT(0xFFFF, parse_bc("0FFFFh", 800));
    TEST_ASSERT_EQUAL_INT(0x0200, parse_bc("0200h", 800));
    TEST_ASSERT_EQUAL_INT(0x1234, parse_bc(" 0x1234 ", 800));
    /* vlastní hodnota nezávisí na platformě */
    TEST_ASSERT_EQUAL_INT(0x0100, parse_bc("0x0100", 700));
    st_BOOTDEV_CHOICE c;
    TEST_ASSERT_TRUE(bootdev_parse("256", &c));
    TEST_ASSERT_TRUE(c.custom);
    TEST_ASSERT_EQUAL_HEX16(0x0100, c.value);
}


/**
 * @brief Neplatné zápisy: mimo rozsah, znaménko, nesmysly, prázdný text, NULL, dlouhý text.
 */
void test_parse_invalid(void)
{
    const char *bad[] = {
        "", "   ", "65536", "0x10000", "10000h", "-1", "+5", "0x", "h", "H", "ABh", "FFh",
        "12ab", "0x12g", "1 2", "fd x", "floppy", "c m t", "cmt1", "0b101", "1.5", "$0200",
        "99999999999999999999", "0x00000000000000000000000000000001",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        st_BOOTDEV_CHOICE c = { .custom = true, .name = BOOTDEV_NAME_QD, .value = 0x5A5A };
        char msg[96];
        g_snprintf(msg, sizeof(msg), "'%s' must be refused", bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(bootdev_parse(bad[i], &c), msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x5A5A, c.value, "out unchanged on error");
    }
    TEST_ASSERT_FALSE(bootdev_parse(NULL, NULL));
}


/**
 * @brief Desítkové číslo s úvodní nulou je odmítnuté s vlastním důvodem; samotná 0 a hex s nulou platí.
 */
void test_parse_leading_zero(void)
{
    const char *lz[] = { "0200", "00", "0512", " 0300 ", "000000" };
    for (size_t i = 0; i < sizeof(lz) / sizeof(lz[0]); i++)
    {
        st_BOOTDEV_CHOICE c = { .custom = true, .name = BOOTDEV_NAME_QD, .value = 0x5A5A };
        TEST_ASSERT_EQUAL_INT_MESSAGE(BOOTDEV_PARSE_LEADING_ZERO, bootdev_parse_ex(lz[i], &c), lz[i]);
        TEST_ASSERT_EQUAL_HEX16(0x5A5A, c.value);
        TEST_ASSERT_FALSE(bootdev_parse(lz[i], NULL));
    }
    TEST_ASSERT_EQUAL_INT(BOOTDEV_PARSE_OK, bootdev_parse_ex("0", NULL));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_PARSE_OK, bootdev_parse_ex("0x0200", NULL));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_PARSE_OK, bootdev_parse_ex("0200h", NULL));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_PARSE_OK, bootdev_parse_ex("10", NULL));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_PARSE_INVALID, bootdev_parse_ex("bogus", NULL));
    TEST_ASSERT_EQUAL_INT(BOOTDEV_PARSE_INVALID, bootdev_parse_ex(NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(bootdev_parse_message(BOOTDEV_PARSE_LEADING_ZERO), "0x0200 or 0200h"));
    TEST_ASSERT_NOT_NULL(strstr(bootdev_parse_message(BOOTDEV_PARSE_INVALID), "expected cmt"));
    TEST_ASSERT_EQUAL_STRING("", bootdev_parse_message(BOOTDEV_PARSE_OK));
    /* nastavení zápis s úvodní nulou nepřijme */
    TEST_ASSERT_FALSE(bootdev_setting_set_text("0200"));
}


/**
 * @brief Tabulka jméno -> BC pro MZ-700, MZ-800 a MZ-1500.
 */
void test_name_table(void)
{
    const unsigned platforms[] = { 700, 800, 1500 };
    for (size_t i = 0; i < 3; i++)
    {
        unsigned p = platforms[i];
        TEST_ASSERT_EQUAL_HEX16(0x0000, bootdev_name_bc(BOOTDEV_NAME_RD, p));
        TEST_ASSERT_EQUAL_HEX16(0x0200, bootdev_name_bc(BOOTDEV_NAME_FD, p));
        TEST_ASSERT_EQUAL_HEX16(0x0300, bootdev_name_bc(BOOTDEV_NAME_QD, p));
    }
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0000, bootdev_name_bc(BOOTDEV_NAME_CMT, 700),
                                    "MZ-700 monitor 1Z-013A leaves BC 0000h after L");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0100, bootdev_name_bc(BOOTDEV_NAME_CMT, 800), "MZ-800 IPL E99Dh");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0100, bootdev_name_bc(BOOTDEV_NAME_CMT, 1500), "MZ-1500 IPL E9E6h");
    TEST_ASSERT_EQUAL_HEX16(0x0000, (uint16_t)parse_bc("cmt", 700));
    TEST_ASSERT_EQUAL_HEX16(0x0100, (uint16_t)parse_bc("cmt", 1500));
    TEST_ASSERT_EQUAL_HEX16(0x0100, bootdev_choice_bc(NULL, 800));
    TEST_ASSERT_EQUAL_HEX16(0x0000, bootdev_choice_bc(NULL, 700));
    TEST_ASSERT_EQUAL_STRING("rd", bootdev_name_text(BOOTDEV_NAME_RD));
    TEST_ASSERT_EQUAL_STRING("cmt", bootdev_name_text(BOOTDEV_NAME_CMT));
    TEST_ASSERT_EQUAL_STRING("fd", bootdev_name_text(BOOTDEV_NAME_FD));
    TEST_ASSERT_EQUAL_STRING("qd", bootdev_name_text(BOOTDEV_NAME_QD));
}


/**
 * @brief Normalizace: jméno malými písmeny, číslo v původním zápisu bez okolních mezer.
 */
void test_normalize(void)
{
    char out[BOOTDEV_TEXT_MAX];
    TEST_ASSERT_TRUE(bootdev_normalize(" FD ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("fd", out);
    TEST_ASSERT_TRUE(bootdev_normalize("SRAM", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("sram", out);
    TEST_ASSERT_TRUE(bootdev_normalize("\t0X00ab", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0X00ab", out);
    TEST_ASSERT_TRUE(bootdev_normalize("0200h", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0200h", out);
    strcpy(out, "keep");
    TEST_ASSERT_FALSE(bootdev_normalize("bogus", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("keep", out);
    char small[4];
    TEST_ASSERT_FALSE_MESSAGE(bootdev_normalize("cmt", small, sizeof(small)), "buffer smaller than BOOTDEV_TEXT_MAX");
}


/**
 * @brief Nastavení: výchozí cmt, platný zápis se uloží normalizovaný, neplatný se odmítne.
 */
void test_setting(void)
{
    char t[BOOTDEV_TEXT_MAX];
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("cmt", t);
    TEST_ASSERT_EQUAL_HEX16(0x0100, bootdev_setting_bc());

    TEST_ASSERT_TRUE(bootdev_setting_set_text("QD"));
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("qd", t);
    TEST_ASSERT_EQUAL_HEX16(0x0300, bootdev_setting_bc());

    TEST_ASSERT_FALSE(bootdev_setting_set_text("nonsense"));
    TEST_ASSERT_FALSE(bootdev_setting_set_text(""));
    TEST_ASSERT_FALSE(bootdev_setting_set_text(NULL));
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("qd", t, "a refused value keeps the setting");

    TEST_ASSERT_TRUE(bootdev_setting_set_text("4660"));
    TEST_ASSERT_EQUAL_HEX16(0x1234, bootdev_setting_bc());
    char tiny[3];
    bootdev_setting_get_text(tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_STRING("46", tiny);
}


/**
 * @brief Klíč INI [CMT] run_mzf_device existuje (TEXT) a save callback zapíše aktuální text.
 *
 * cfgelement_save() zapisuje do st_CFGROOT::ini_fp, který je otevřený jen
 * uvnitř cfgroot_save(). Test proto kořeni na dobu volání podstrčí dočasný
 * soubor (jinak fprintf() s NULL) a ověří i zapsaný řádek.
 */
void test_cfg_element(void)
{
    TEST_ASSERT_TRUE(bootdev_cfg_is_key("CMT", "run_mzf_device"));
    TEST_ASSERT_FALSE(bootdev_cfg_is_key("CMT", "mzfsize_check"));
    TEST_ASSERT_FALSE(bootdev_cfg_is_key("CMTHACK", "run_mzf_device"));
    TEST_ASSERT_FALSE(bootdev_cfg_is_key(NULL, NULL));

    CFGMOD *mod = cfgroot_get_module_by_name(g_cfgmain, (char *)BOOTDEV_CFG_MODULE);
    TEST_ASSERT_NOT_NULL(mod);
    CFGELM *elm = cfgmodule_get_element_by_name(mod, (char *)BOOTDEV_CFG_ELEMENT);
    TEST_ASSERT_NOT_NULL_MESSAGE(elm, "cmt_init() registers [CMT] run_mzf_device");
    TEST_ASSERT_EQUAL_INT(CFGENTYPE_TEXT, elm->type);

    TEST_ASSERT_TRUE(bootdev_setting_set_text("0200h"));
    gchar *dir = g_dir_make_tmp("mz_bootdev_XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(dir);
    gchar *ini = g_build_filename(dir, "save.ini", NULL);
    FILE *fp = g_fopen(ini, "wb");     /* binárně: na Windows by "w" zapsalo CR LF */
    TEST_ASSERT_NOT_NULL(fp);
    g_cfgmain->ini_fp = fp;
    cfgelement_save(elm);
    g_cfgmain->ini_fp = NULL;
    fclose(fp);
    gchar *written = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(ini, &written, NULL, NULL));
    g_unlink(ini);
    g_rmdir(dir);
    g_free(ini);
    g_free(dir);
    TEST_ASSERT_EQUAL_STRING(BOOTDEV_CFG_ELEMENT " = 0200h\n", written);
    g_free(written);
    TEST_ASSERT_EQUAL_STRING("0200h", cfgelement_get_text_value(elm));
    /* propagate z INI: neplatná hodnota -> výchozí cmt */
    cfgelement_set_text_value(elm, "garbage");
    cfgelement_propagate(elm);
    char t[BOOTDEV_TEXT_MAX];
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("cmt", t);
    cfgelement_set_text_value(elm, "0200");
    cfgelement_propagate(elm);
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("cmt", t, "a leading zero in the INI falls back to cmt");
    cfgelement_set_text_value(elm, "FD");
    cfgelement_propagate(elm);
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("fd", t);
    cfgelement_set_text_value(elm, BOOTDEV_DEFAULT_TEXT);
}


/**
 * @brief Bootstrap: BC podle nastavení a podle jednorázové volby (MZ-800).
 */
void test_bootstrap_sets_bc(void)
{
    gchar *dir = g_dir_make_tmp("mz_bootdev_XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(dir);
    gchar *path = g_build_filename(dir, "t.mzf", NULL);
    uint8_t mzf[130];
    memset(mzf, 0, sizeof(mzf));
    mzf[0] = 0x01;
    memcpy(&mzf[1], "BOOTDEV\r", 8);
    mzf[0x12] = 2;              /* délka těla */
    mzf[0x14] = 0x00; mzf[0x15] = 0x12; /* fstrt 1200h */
    mzf[0x16] = 0x00; mzf[0x17] = 0x12; /* fexec 1200h */
    mzf[128] = 0x18; mzf[129] = 0xFE;   /* JR $ */
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)mzf, sizeof(mzf), NULL));

    mzarch_main_reset_and_run_mzf(path, NULL);
    TEST_ASSERT_EQUAL_HEX16(0x1200, z80_get_reg(g_mzarch_main.cpu, Z80_REG_PC));
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0100, z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC), "default cmt");

    TEST_ASSERT_TRUE(bootdev_setting_set_text("qd"));
    mzarch_main_reset_and_run_mzf(path, NULL);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0300, z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC), "setting qd");

    st_BOOTDEV_CHOICE c;
    TEST_ASSERT_TRUE(bootdev_parse("0x4321", &c));
    mzarch_main_reset_and_run_mzf(path, &c);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x4321, z80_get_reg(g_mzarch_main.cpu, Z80_REG_BC), "one-time choice");
    char t[BOOTDEV_TEXT_MAX];
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("qd", t, "a one-time choice keeps the setting");

    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_parse_names);
    RUN_TEST(test_parse_numbers);
    RUN_TEST(test_parse_invalid);
    RUN_TEST(test_parse_leading_zero);
    RUN_TEST(test_name_table);
    RUN_TEST(test_normalize);
    RUN_TEST(test_setting);
    RUN_TEST(test_cfg_element);
    RUN_TEST(test_bootstrap_sets_bc);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
