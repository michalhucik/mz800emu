/**
 * @file bootstrap_device.c
 * @brief Kód zařízení v registru BC při přímém spuštění MZF: tabulka, zápis a nastavení.
 *
 * Kontrakt a zdroje hodnot viz bootstrap_device.h (bootdev_name_bc()).
 * Tato část nezávisí na platformě buildu ani na konfiguraci INI (jen GLib),
 * aby ji šlo linkovat i do samostatných testů (MCP dispatch); hodnota pro
 * platformu buildu a klíč INI jsou v bootstrap_device_cfg.c.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "mzarch/bootstrap_device.h"


/** @brief Kód zařízení RD: / SRAM (báze memory-layout-user-prog.md). */
#define BOOTDEV_BC_RD   0x0000
/** @brief Kód zařízení CMT po IPL MZ-800 (E99Dh) a MZ-1500 (E9E6h). */
#define BOOTDEV_BC_CMT_IPL 0x0100
/** @brief BC po zavedení z kazety monitorem MZ-700 1Z-013A (příkaz L). */
#define BOOTDEV_BC_CMT_MZ700 0x0000
/** @brief Kód zařízení FD: (báze memory-layout-user-prog.md). */
#define BOOTDEV_BC_FD   0x0200
/** @brief Kód zařízení QD: (báze memory-layout-user-prog.md). */
#define BOOTDEV_BC_QD   0x0300


/**
 * @brief Nastavení modulu: text volby, jak se ukládá do INI.
 *
 * @invariant text vždy projde bootdev_parse() a je ukončený nulou.
 * Chrání ho s_lock (čte a mění UI vlákno, emu vlákno i uložení INI).
 */
static char s_text[BOOTDEV_TEXT_MAX] = BOOTDEV_DEFAULT_TEXT;

/** @brief Zámek s_text (statický GMutex, inicializace nulami je platná). */
static GMutex s_lock;


/**
 * @brief Jména zařízení a jejich řádky tabulky (alias `sram` = RD).
 */
static const struct
{
    const char *text;     /**< Zápis jména (malými písmeny). */
    en_BOOTDEV_NAME name; /**< Řádek tabulky. */
} s_names[] = {
    { "rd", BOOTDEV_NAME_RD },
    { "sram", BOOTDEV_NAME_RD },
    { "cmt", BOOTDEV_NAME_CMT },
    { "fd", BOOTDEV_NAME_FD },
    { "qd", BOOTDEV_NAME_QD },
};


/**
 * @brief Ořízne bílé znaky a zkopíruje zápis do bufferu.
 *
 * @param text Vstup (nesmí být NULL).
 * @param[out] buf Buffer o velikosti BOOTDEV_TEXT_MAX.
 * @return Délka oříznutého textu, -1 = je delší než BOOTDEV_TEXT_MAX - 1.
 */
static int bootdev_trim_copy(const char *text, char buf[BOOTDEV_TEXT_MAX])
{
    while (*text && isspace((unsigned char)*text)) text++;
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) len--;
    if (len >= BOOTDEV_TEXT_MAX) return -1;
    memcpy(buf, text, len);
    buf[len] = '\0';
    return (int)len;
}


/**
 * @brief Rozebere číslo (desítkově, `0x` hex, hex s příponou `h`).
 *
 * @param s Oříznutý text bez bílých znaků (nesmí být prázdný).
 * @param len Délka s.
 * @param[out] out Hodnota (jen při úspěchu).
 * @return BOOTDEV_PARSE_OK = platné číslo v 0..65535;
 *         BOOTDEV_PARSE_LEADING_ZERO = desítkový zápis s úvodní nulou
 *         (delší než jedna číslice); jinak BOOTDEV_PARSE_INVALID.
 */
static en_BOOTDEV_PARSE bootdev_parse_number(const char *s, size_t len, uint16_t *out)
{
    int base = 10;
    const char *body = s;
    size_t body_len = len;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        body = s + 2;
        body_len = len - 2;
    }
    else if (len >= 2 && (s[len - 1] == 'h' || s[len - 1] == 'H'))
    {
        /* jako watch_parse_address(): hex s příponou začíná číslicí */
        if (!isdigit((unsigned char)s[0])) return BOOTDEV_PARSE_INVALID;
        base = 16;
        body_len = len - 1;
    };
    if (body_len == 0) return BOOTDEV_PARSE_INVALID;
    for (size_t i = 0; i < body_len; i++)
    {
        unsigned char c = (unsigned char)body[i];
        if (base == 10 ? !isdigit(c) : !isxdigit(c)) return BOOTDEV_PARSE_INVALID;
    };
    /* desítkově s úvodní nulou (0200) by tiše znamenalo 200 = 00C8h */
    if (base == 10 && body_len > 1 && body[0] == '0') return BOOTDEV_PARSE_LEADING_ZERO;
    char tmp[BOOTDEV_TEXT_MAX];
    memcpy(tmp, body, body_len);
    tmp[body_len] = '\0';
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(tmp, &end, base);
    if (errno == ERANGE || !end || *end != '\0' || v > 0xFFFFul) return BOOTDEV_PARSE_INVALID;
    *out = (uint16_t)v;
    return BOOTDEV_PARSE_OK;
}


bool bootdev_parse(const char *text, st_BOOTDEV_CHOICE *out)
{
    return bootdev_parse_ex(text, out) == BOOTDEV_PARSE_OK;
}


const char *bootdev_parse_message(en_BOOTDEV_PARSE r)
{
    switch (r)
    {
    case BOOTDEV_PARSE_OK:
        return "";
    case BOOTDEV_PARSE_LEADING_ZERO:
        return "a number with a leading zero is not accepted: write hexadecimal as 0x0200 or 0200h, "
               "decimal without leading zeros";
    case BOOTDEV_PARSE_INVALID:
    default:
        return "expected cmt, fd, qd, rd, sram or a number 0-65535 (decimal 512, hexadecimal 0x0200 or 0200h)";
    };
}


en_BOOTDEV_PARSE bootdev_parse_ex(const char *text, st_BOOTDEV_CHOICE *out)
{
    if (!text) return BOOTDEV_PARSE_INVALID;
    char buf[BOOTDEV_TEXT_MAX];
    int len = bootdev_trim_copy(text, buf);
    if (len <= 0) return BOOTDEV_PARSE_INVALID;

    for (size_t i = 0; i < G_N_ELEMENTS(s_names); i++)
    {
        if (g_ascii_strcasecmp(buf, s_names[i].text) == 0)
        {
            if (out)
            {
                out->custom = false;
                out->name = s_names[i].name;
                out->value = 0;
            };
            return BOOTDEV_PARSE_OK;
        };
    };

    uint16_t v;
    en_BOOTDEV_PARSE r = bootdev_parse_number(buf, (size_t)len, &v);
    if (r != BOOTDEV_PARSE_OK) return r;
    if (out)
    {
        out->custom = true;
        out->name = BOOTDEV_NAME_CMT;
        out->value = v;
    };
    return BOOTDEV_PARSE_OK;
}


uint16_t bootdev_name_bc(en_BOOTDEV_NAME name, unsigned platform)
{
    switch (name)
    {
    case BOOTDEV_NAME_RD:
        return BOOTDEV_BC_RD;
    case BOOTDEV_NAME_FD:
        return BOOTDEV_BC_FD;
    case BOOTDEV_NAME_QD:
        return BOOTDEV_BC_QD;
    case BOOTDEV_NAME_CMT:
    default:
        return (platform == 700) ? BOOTDEV_BC_CMT_MZ700 : BOOTDEV_BC_CMT_IPL;
    };
}


uint16_t bootdev_choice_bc(const st_BOOTDEV_CHOICE *choice, unsigned platform)
{
    if (!choice) return bootdev_name_bc(BOOTDEV_NAME_CMT, platform);
    if (choice->custom) return choice->value;
    return bootdev_name_bc(choice->name, platform);
}


const char *bootdev_name_text(en_BOOTDEV_NAME name)
{
    switch (name)
    {
    case BOOTDEV_NAME_RD:
        return "rd";
    case BOOTDEV_NAME_FD:
        return "fd";
    case BOOTDEV_NAME_QD:
        return "qd";
    case BOOTDEV_NAME_CMT:
    default:
        return "cmt";
    };
}


bool bootdev_normalize(const char *text, char *out, size_t out_size)
{
    st_BOOTDEV_CHOICE c;
    if (!out || out_size < BOOTDEV_TEXT_MAX || !bootdev_parse(text, &c)) return false;
    char buf[BOOTDEV_TEXT_MAX];
    (void)bootdev_trim_copy(text, buf);
    if (!c.custom)
    {
        /* jméno malými písmeny (alias zůstane aliasem) */
        for (char *p = buf; *p; p++) *p = (char)g_ascii_tolower(*p);
    };
    g_strlcpy(out, buf, out_size);
    return true;
}


bool bootdev_setting_set_text(const char *text)
{
    char norm[BOOTDEV_TEXT_MAX];
    if (!bootdev_normalize(text, norm, sizeof(norm))) return false;
    g_mutex_lock(&s_lock);
    g_strlcpy(s_text, norm, sizeof(s_text));
    g_mutex_unlock(&s_lock);
    return true;
}


void bootdev_setting_get_text(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    g_mutex_lock(&s_lock);
    g_strlcpy(out, s_text, out_size);
    g_mutex_unlock(&s_lock);
}


bool bootdev_cfg_is_key(const char *module, const char *element)
{
    return module && element && strcmp(module, BOOTDEV_CFG_MODULE) == 0
           && strcmp(element, BOOTDEV_CFG_ELEMENT) == 0;
}
