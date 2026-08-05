/*
 * File:   hid_keymap.c
 *
 * Implementace HID keymap modulu (mutant mcp-server V1.C.1). Detaily
 * v hid_keymap.h.
 *
 * Soubor je obalen guardem `MZ800EMU_CFG_MCP_SERVER_ENABLED`, aby při
 * sestavení s `NO_MCP=1` zůstal prázdný (= žádné nepotřebné závislosti).
 *
 * Pro standalone test buildy MCP dispatch testů (`MZ800EMU_MCP_TEST_BUILD`)
 * se modul také kompiluje - závisí pouze na pio8255 strukturách (přes
 * `hw-generic/pio8255/pio8255.h`) a volitelně joy.h.  V test buildu
 * dispatch volá stub místo skutečného press/release (= injection se
 * zachytí v stub state polích).
 *
 * Licence: GPLv3.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../mzarch/mzarch_config.h"
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include "hid_keymap.h"
#include "../mzarch/mzhal.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* PIO8255 vkbd matrix - z autotype API víme, že přesný layout je
 * `g_pio8255.vkbd_matrix[10]`, bit aktivní = 0 (matrix bit clear).
 * PIO8255_VKBDBIT_RESET / SET makra. */
#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../hw-generic/pio8255/pio8255.h"
#endif

/* V testovacím MCP stub buildu (MZ800EMU_MCP_TEST_BUILD) se skutečná
 * implementace nahrazuje stubem níže. */
#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../hw-generic/joy/joy.h"
#endif

/* Forward deklarace pio8255 API (z pio8255.c). V test buildu poskytne
 * test stub kompatibilní implementaci. */
#ifndef MZ800EMU_MCP_TEST_BUILD
extern int pio8255_autotype_get_matrix(char c, uint8_t *ret, bool *ret_shift);
#else
/* Test stub poskytne tuto funkci v hid_keymap_test_stub.c (= jednoduchý
 * mirror logiky reálné tabulky pro nejčastější znaky, viz testy V1.C.1). */
extern int pio8255_autotype_get_matrix(char c, uint8_t *ret, bool *ret_shift);
#endif


/* ============================================================================
 * Key name tabulka
 *
 * Z větší části sdílená napříč MZ-700 / MZ-800 / MZ-1500. Jediný rozdíl:
 * MZ-700 a MZ-1500 nemají klávesu TAB (HW), proto je entry "TAB" jen pro
 * MZARCH == 800 (viz iface_keyboard.c - bit (0, 3) se na MZ-700/1500
 * nikdy nevystavuje).
 * ============================================================================ */

typedef struct st_HID_KEYNAME_ENTRY {
    const char *name;
    int         col;
    int         bit;
    bool        needs_shift;
} st_HID_KEYNAME_ENTRY;

/* Hlavní tabulka - terminována name=NULL. Jména MUSÍ být UPPERCASE
 * (caller je zodpovědný za upper-case normalizaci). Aliasy (např.
 * RETURN / ENTER) jsou samostatné entries pro pohodlnou variant
 * akceptanci. */
static const st_HID_KEYNAME_ENTRY g_keyname_table[] = {
    /* col0 */
    { "BLANK",       0, 7, false },
    { "GRAPH",       0, 6, false },
    { "LIBRA",       0, 5, true  }, /* SHIFT + col0/bit5 */
    { "ALPHA",       0, 4, false },
    { "TAB",         0, 3, false }, /* jen MZ-800 - runtime filtr keyname_entry_supported() */
    { "RETURN",      0, 0, false },
    { "ENTER",       0, 0, false },
    { "CR",          0, 0, false },

    /* col6 */
    { "SPACE",       6, 4, false },

    /* col7 - kurzor + edit */
    { "INSERT",      7, 7, false },
    { "INS",         7, 7, false },
    { "DELETE",      7, 6, false },
    { "DEL",         7, 6, false },
    { "BACKSPACE",   7, 6, false },
    { "ARROW_UP",    7, 5, false },
    { "UP",          7, 5, false },
    { "ARROW_DOWN",  7, 4, false },
    { "DOWN",        7, 4, false },
    { "ARROW_RIGHT", 7, 3, false },
    { "RIGHT",       7, 3, false },
    { "ARROW_LEFT",  7, 2, false },
    { "LEFT",        7, 2, false },

    /* col8 - modifikátory + ESC */
    { "ESC",         8, 7, false },
    { "ESCAPE",      8, 7, false },
    { "BREAK",       8, 7, false }, /* MZ BREAK alias na ESC */
    { "END",         8, 7, false },
    { "CTRL",        8, 6, false },
    { "CONTROL",     8, 6, false },
    { "SHIFT",       8, 0, false },

    /* col9 - F1..F5 */
    { "F1",          9, 7, false },
    { "F2",          9, 6, false },
    { "F3",          9, 5, false },
    { "F4",          9, 4, false },
    { "F5",          9, 3, false },

    /* speciální klávesy mapované přes F-keys v iface_keyboard.c */
    { "F6",          1, 5, false }, /* @ */
    { "F7",          6, 7, false }, /* \ */
    { "F8",          7, 1, false }, /* ? */
    { "F9",          0, 5, false }, /* LIBRA bez SHIFT */

    { NULL,          0, 0, false }
};


/* ============================================================================
 * Public API - resolve
 * ============================================================================ */

/**
 * @brief Pomocná: ASCII upper-case bez locales (přenosné).
 *
 * Pouze pro a-z range. Vrátí původní znak pokud není písmeno.
 */
static char _to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}


bool hid_keymap_resolve_ascii(char c, st_HID_KEYMAP_RESOLVED *out_res) {
    if (!out_res) {
        return false;
    }
    out_res->col = -1;
    out_res->bit = 0;
    out_res->needs_shift = false;

    uint8_t row_mask = 0xff;
    bool needs_shift = false;
    int col = pio8255_autotype_get_matrix(c, &row_mask, &needs_shift);
    if (col < 0) {
        return false;
    }
    /* row_mask = 0xff & ~(1 << row), tj. invertní maska. Najdeme bit. */
    int bit = -1;
    for (int b = 0; b < 8; b++) {
        if ((row_mask & (1 << b)) == 0) {
            bit = b;
            break;
        }
    }
    if (bit < 0) {
        return false;
    }
    out_res->col = col;
    out_res->bit = bit;
    out_res->needs_shift = needs_shift;
    return true;
}


/**
 * @brief Test, zda je entry tabulky dostupná na aktuální platformě.
 *
 * Jediný platformní rozdíl: klávesa TAB (col 0, bit 3) existuje jen na
 * MZ-800 (g_mzhal.has_key_tab); MZ-700/MZ-1500 ji v HW matici nemají
 * (mzhal krok 8 - dříve compile-time #if MZARCH == 800 u entry).
 *
 * @param e  Entry tabulky g_keyname_table (nesmí být NULL).
 * @return true = entry na platformě existuje, false = filtrovat.
 */
static bool keyname_entry_supported(const st_HID_KEYNAME_ENTRY *e) {
    if (!g_mzhal.has_key_tab && e->col == 0 && e->bit == 3) {
        return false;
    }
    return true;
}


const char *hid_keymap_reverse_lookup(int col, int bit) {
    if (col < 0 || col > 9 || bit < 0 || bit > 7) {
        return NULL;
    }
    /* Lineární scan - tabulka má cca 40 entries, hot-path to není
     * (= volá se jen při explicit `keyboard/state` Resource read). */
    for (int i = 0; g_keyname_table[i].name != NULL; i++) {
        if (!keyname_entry_supported(&g_keyname_table[i])) continue;
        if (g_keyname_table[i].col == col
                && g_keyname_table[i].bit == bit
                && !g_keyname_table[i].needs_shift) {
            return g_keyname_table[i].name;
        }
    }
    /* Druhé kolo - povolíme i needs_shift entries (pro LIBRA atd.) */
    for (int i = 0; g_keyname_table[i].name != NULL; i++) {
        if (!keyname_entry_supported(&g_keyname_table[i])) continue;
        if (g_keyname_table[i].col == col
                && g_keyname_table[i].bit == bit) {
            return g_keyname_table[i].name;
        }
    }
    return NULL;
}


bool hid_keymap_get_entry(int index,
                           const char **out_name,
                           int *out_col,
                           int *out_bit,
                           bool *out_needs_shift) {
    if (index < 0) {
        return false;
    }
    /* Spočítej platná entries (= ne sentinel); nepodporované entries
     * (TAB mimo MZ-800) se přeskakují, indexy zůstávají husté. */
    int valid = 0;
    for (int i = 0; g_keyname_table[i].name != NULL; i++) {
        if (!keyname_entry_supported(&g_keyname_table[i])) continue;
        if (valid == index) {
            if (out_name)        *out_name        = g_keyname_table[i].name;
            if (out_col)         *out_col         = g_keyname_table[i].col;
            if (out_bit)         *out_bit         = g_keyname_table[i].bit;
            if (out_needs_shift) *out_needs_shift = g_keyname_table[i].needs_shift;
            return true;
        }
        valid++;
    }
    return false;
}


bool hid_keymap_resolve(const char *name, st_HID_KEYMAP_RESOLVED *out_res) {
    if (!name || !out_res) {
        return false;
    }
    out_res->col = -1;
    out_res->bit = 0;
    out_res->needs_shift = false;

    size_t nlen = strlen(name);
    if (nlen == 0) {
        return false;
    }

    /* "ASCII:<znak>" prefix - vezme jeden znak za prefixem. */
    if (nlen >= 7 && strncmp(name, "ASCII:", 6) == 0) {
        return hid_keymap_resolve_ascii(name[6], out_res);
    }

    /* Single-character jméno = ASCII fallback. */
    if (nlen == 1) {
        return hid_keymap_resolve_ascii(name[0], out_res);
    }

    /* Normalizace na UPPERCASE pro lookup v tabulce. */
    char upper[HID_KEYMAP_MAX_KEYNAME];
    if (nlen >= sizeof(upper)) {
        return false;
    }
    for (size_t i = 0; i < nlen; i++) {
        upper[i] = _to_upper(name[i]);
    }
    upper[nlen] = '\0';

    for (int i = 0; g_keyname_table[i].name != NULL; i++) {
        if (!keyname_entry_supported(&g_keyname_table[i])) continue;
        if (strcmp(upper, g_keyname_table[i].name) == 0) {
            out_res->col = g_keyname_table[i].col;
            out_res->bit = g_keyname_table[i].bit;
            out_res->needs_shift = g_keyname_table[i].needs_shift;
            return true;
        }
    }
    return false;
}


/* ============================================================================
 * Public API - press / release
 * ============================================================================ */

#ifndef MZ800EMU_MCP_TEST_BUILD

void hid_keymap_press(int col, int bit, bool also_shift) {
    if (col < 0 || col > 9 || bit < 0 || bit > 7) {
        return;
    }
    PIO8255_VKBDBIT_RESET(col, bit);
    if (also_shift) {
        PIO8255_VKBDBIT_RESET(8, 0); /* SHIFT */
    }
}


void hid_keymap_release(int col, int bit, bool also_shift) {
    if (col < 0 || col > 9 || bit < 0 || bit > 7) {
        return;
    }
    PIO8255_VKBDBIT_SET(col, bit);
    if (also_shift) {
        PIO8255_VKBDBIT_SET(8, 0);
    }
}


void hid_keymap_release_all(void) {
    /* Použij kanonický PIO8255 vkbd reset (makro PIO8255_VKBD_MATRIX_RESET
     * -> pio8255_vkbd_matrix_reset, pio8255.c). Dřív zde byl duplikovaný
     * memset(0xff); makro do té doby nemělo žádného runtime callera
     * (jen definici). Sjednocení odstraní duplikaci a dá makru caller
     * stejným vzorem jako PIO8255_KEYBOARD_MATRIX_RESET (fix 0016). */
    PIO8255_VKBD_MATRIX_RESET();
}


bool hid_keymap_joystick_set(int port, uint8_t mcp_mask) {
    if (port < 0 || port >= JOY_DEVID_COUNT) {
        return false;
    }
    /* Mapování bit-by-bit z aktivní-HIGH MCP masky na aktivní-LOW
     * native state byte. Bity 0..5 jsou definované (UP/DOWN/LEFT/RIGHT/
     * FIRE1/FIRE2), bity 6..7 se ignorují. */
    uint8_t native = 0xff;
    if (mcp_mask & (1 << 0)) native &= ~(1 << JOY_STATEBIT_UP);
    if (mcp_mask & (1 << 1)) native &= ~(1 << JOY_STATEBIT_DOWN);
    if (mcp_mask & (1 << 2)) native &= ~(1 << JOY_STATEBIT_LEFT);
    if (mcp_mask & (1 << 3)) native &= ~(1 << JOY_STATEBIT_RIGHT);
    if (mcp_mask & (1 << 4)) native &= ~(1 << JOY_STATEBIT_TRIG1);
    if (mcp_mask & (1 << 5)) native &= ~(1 << JOY_STATEBIT_TRIG2);
    g_joy.dev[port].state = native;
    return true;
}


bool hid_keymap_joystick_clear(int port) {
    return hid_keymap_joystick_set(port, 0);
}

#else /* MZ800EMU_MCP_TEST_BUILD */

/* V test buildu jsou press/release/joystick funkce poskytnuty test
 * stubem (= hid_keymap_test_stub.c), který zachytí parametry do
 * g_stub_state. Linker je nakontačí. */

#endif /* MZ800EMU_MCP_TEST_BUILD */


#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
