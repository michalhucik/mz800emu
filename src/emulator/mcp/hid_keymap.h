/*
 * File:   hid_keymap.h
 *
 * MCP server - Human Input Device (HID) keymap modul (mutant mcp-server
 * V1.C.1). Mapuje symbolická jména kláves (RETURN, BREAK, GRAPH, ...) a
 * Sharp ASCII znaky na Sharp MZ klávesovou matrix (col 0-9, bit 0-7).
 *
 * Strategie injekce:
 *   Press / release operují nad `g_pio8255.vkbd_matrix[]` (= virtuální
 *   klávesnice paralelní s fyzickou `keyboard_matrix[]`). PIO8255 read
 *   PORTB AND-uje obě matrix dohromady, takže VKBD bity se klientovi v
 *   Z80 jeví jako reálně držené klávesy. VKBD je perzistentní napříč
 *   frame scanováním (= `iface_keyboard_full_scan` resetuje pouze
 *   `keyboard_matrix`, ne `vkbd_matrix`).
 *
 * Per-platform poznámka:
 *   Klávesová matrix MZ-700 / MZ-800 / MZ-1500 sdílí stejný layout v
 *   `iface_keyboard.c` (= autor neudržuje per-MZARCH variantu).
 *   Mapování v tomto modulu proto NENÍ podmíněno makrem MZARCH. Pokud
 *   se v budoucnu prokáže rozdíl (= odlišný význam col0/bit0 mezi
 *   modely), tabulka se rozdělí. Zatím sdílená.
 *
 * Sharp ASCII:
 *   Pro ASCII mapping se reusuje existující `pio8255_autotype_get_matrix`
 *   z `hw-generic/pio8255/pio8255.c` (= autoritativní tabulka char ->
 *   col/bit/shift). Tento modul jen obaluje API.
 *
 * Licence: GPLv3 (sjednocená s celým projektem).
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MZ800EMU_MCP_HID_KEYMAP_H
#define MZ800EMU_MCP_HID_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Výsledek resolve klávesy.
 *
 * `col` v rozsahu 0..9 ukazuje na sloupec klávesové matrix. `bit` v
 * rozsahu 0..7 určuje konkrétní klávesu v sloupci. Pokud `needs_shift`
 * je true, MUSÍ se navíc stisknout SHIFT (= col 8 bit 0).
 *
 * Pro neznámé klávesy vrací resolver -1 v `col`.
 */
typedef struct st_HID_KEYMAP_RESOLVED {
    int  col;          /**< 0..9 sloupec, -1 = invalid. */
    int  bit;          /**< 0..7 bit v sloupci. */
    bool needs_shift;  /**< true = vyžaduje SHIFT modifier. */
} st_HID_KEYMAP_RESOLVED;


/**
 * @brief Maximální délka resolved key name (pro tabulky).
 *
 * Pojmy jako "ARROW_UP" / "BACKSPACE" / "CONTROL" mají max 16 znaků
 * včetně terminátoru. Hodnota je rezervovaná s rezervou.
 */
#define HID_KEYMAP_MAX_KEYNAME 24


/**
 * @brief Resolve klávesy podle symbolického jména.
 *
 * Podporované jména (case-sensitive, UPPERCASE):
 *   RETURN, ENTER       - col 0 bit 0 (CR)
 *   SPACE               - col 6 bit 4
 *   BACKSPACE, DEL      - col 7 bit 6 (DELETE)
 *   INSERT, INS         - col 7 bit 7
 *   ESC, ESCAPE         - col 8 bit 7
 *   TAB                 - col 0 bit 3
 *   SHIFT               - col 8 bit 0
 *   CONTROL, CTRL       - col 8 bit 6
 *   GRAPH               - col 0 bit 6
 *   ALPHA               - col 0 bit 4
 *   BLANK               - col 0 bit 7
 *   BREAK               - col 8 bit 7 (= ESC alias na MZ)
 *   LIBRA               - col 0 bit 5 (vyžaduje SHIFT)
 *   ARROW_UP            - col 7 bit 5
 *   ARROW_DOWN          - col 7 bit 4
 *   ARROW_LEFT          - col 7 bit 2
 *   ARROW_RIGHT         - col 7 bit 3
 *   F1..F5              - col 9 bit 7..3
 *   F6 (= @)            - col 1 bit 5
 *   F7 (= \)            - col 6 bit 7
 *   F8 (= ?)            - col 7 bit 1
 *   F9 (= LIBRA-key)    - col 0 bit 5
 *
 * Pro klávesy s prefixem `ASCII:<znak>` se použije ASCII fallback
 * přes `hid_keymap_resolve_ascii(<znak>, ...)`.
 *
 * Pro single-character jméno (např. "A", "1", "?") se použije ASCII
 * fallback s prvním znakem stringu.
 *
 * @param[in]  name      symbolické jméno klávesy (UPPERCASE) nebo
 *                       "ASCII:<znak>"
 * @param[out] out_res   vyplněný výsledek (col=-1 pokud klávesa
 *                       neznámá)
 * @return true při úspěchu (= col >= 0), false pokud klávesa neznámá
 *
 * @note Funkce je čistá (= bez side effects mimo `out_res`). Lze
 *       volat z libovolného vlákna.
 */
bool hid_keymap_resolve(const char *name, st_HID_KEYMAP_RESOLVED *out_res);


/**
 * @brief Resolve klávesy podle Sharp ASCII znaku.
 *
 * Obal nad `pio8255_autotype_get_matrix` z pio8255.c. Vrací též
 * `needs_shift` flag (pro velká písmena / shifted symboly).
 *
 * @param[in]  c         ASCII znak (8-bit), např. 'A', '1', '\r'
 * @param[out] out_res   vyplněný výsledek (col=-1 pokud znak nemá
 *                       Sharp ASCII mapping)
 * @return true pokud znak má mapping, false jinak
 */
bool hid_keymap_resolve_ascii(char c, st_HID_KEYMAP_RESOLVED *out_res);


/**
 * @brief Press klávesu v vkbd matrix.
 *
 * Vyresetuje (= clear na 0) bit v `g_pio8255.vkbd_matrix[col]`.
 * Pokud `also_shift` je true, navíc stiskne SHIFT (col 8 bit 0).
 *
 * @param[in] col          0..9 sloupec
 * @param[in] bit          0..7 bit
 * @param[in] also_shift   true = press i SHIFT
 *
 * @note NEsmí být voláno před `pio8255_init()` (= `g_pio8255` musí
 *       být inicializovaný).
 * @note Není thread-safe vůči emu vláknu; volat z dbgapi handleru
 *       běžícího v emulator threadu.
 */
void hid_keymap_press(int col, int bit, bool also_shift);


/**
 * @brief Release klávesu v vkbd matrix.
 *
 * Nastaví (= set na 1) bit v `g_pio8255.vkbd_matrix[col]`. Pokud
 * `also_shift` je true, navíc release SHIFT.
 *
 * @param[in] col          0..9 sloupec
 * @param[in] bit          0..7 bit
 * @param[in] also_shift   true = release i SHIFT
 */
void hid_keymap_release(int col, int bit, bool also_shift);


/**
 * @brief Release VŠECH kláves ve vkbd matrix.
 *
 * Vyplní `g_pio8255.vkbd_matrix` hodnotami 0xff (= vše uvolněné) přes
 * kanonické makro `PIO8255_VKBD_MATRIX_RESET()` (pio8255.c). Použití:
 * cleanup po test scenariu, nebo když AI klient žádá `input_release_key`
 * bez specifikace klávesy. Slouží též jako light input-reset pro
 * vyčištění případně přiklepeného vkbd bitu (= press bez paroveho
 * release, fix 0016).
 */
void hid_keymap_release_all(void);


/**
 * @brief Set joystick state byte pro daný port.
 *
 * Mapuje "MCP friendly" 8-bit masku (active-HIGH, bity 0..5 = UP /
 * DOWN / LEFT / RIGHT / FIRE1 / FIRE2) na native `g_joy.dev[].state`
 * (active-LOW). Bity 6 a 7 vstupní masky se ignorují.
 *
 * @param[in] port    0 nebo 1 (= JOY_DEVID_0 / JOY_DEVID_1)
 * @param[in] mcp_mask  8-bit aktivní-HIGH bitmaska (0 = nestisknuto)
 * @return true pokud port v rozsahu 0..1, false jinak
 *
 * @note V testovacím MCP stub buildu (MZ800EMU_MCP_TEST_BUILD) implementaci
 *       dodává test stub (zachytává parametry do g_stub_state).
 */
bool hid_keymap_joystick_set(int port, uint8_t mcp_mask);


/**
 * @brief Clear joystick state (= všechny bity uvolněné) pro daný port.
 *
 * Ekvivalent `hid_keymap_joystick_set(port, 0)`.
 *
 * @param[in] port  0 nebo 1
 * @return true při úspěchu
 */
bool hid_keymap_joystick_clear(int port);


/**
 * @brief Reverse lookup (col, bit) -> symbolické jméno klávesy.
 *
 * Pro V1.D.4 Resource `emulator://input/keyboard/state` potřebujeme z
 * bitové matice vytvořit lidsky čitelný seznam stisknutých kláves.
 * Funkce hledá první entry v interní tabulce, která má `col` a `bit`
 * shodné se zadanými parametry, a vrátí pointer na statický string s
 * UPPERCASE jménem. Aliasy (např. RETURN i ENTER pro stejný bit) - vrací
 * první nalezený, který je v tabulce nejdříve (= pro col 0 bit 0 to bude
 * "RETURN", ne "ENTER").
 *
 * @param[in] col  0..9 sloupec
 * @param[in] bit  0..7 bit
 * @return Pointer na statický UPPERCASE string s jménem klávesy, nebo
 *         NULL pokud kombinace (col, bit) nemá v tabulce mapping.
 *
 * @note Vracený pointer ukazuje na statickou paměť uvnitř modulu - caller
 *       NEsmí ho free. Žádné side effects.
 */
const char *hid_keymap_reverse_lookup(int col, int bit);


/**
 * @brief Iterace přes všechny entries v key name tabulce.
 *
 * Pro V1.D.4 Resource `emulator://input/keyboard/matrix_info` potřebujeme
 * vypsat kompletní mapping platformy. Iterátor poskytne pointer na n-tý
 * entry; po posledním vrátí false a `out_*` zůstanou beze změny.
 *
 * @param[in]  index         0-based pořadí entry
 * @param[out] out_name      ukazatel na jméno (statický string)
 * @param[out] out_col       0..9 sloupec
 * @param[out] out_bit       0..7 bit
 * @param[out] out_needs_shift  true = klávesa vyžaduje SHIFT
 * @return true pokud index byl validní, false po posledním entry
 *
 * @note Tabulka obsahuje aliasy (RETURN/ENTER, CTRL/CONTROL, ...);
 *       caller pokud chce unique col/bit kombinace musí sám filtrovat.
 */
bool hid_keymap_get_entry(int index,
                           const char **out_name,
                           int *out_col,
                           int *out_bit,
                           bool *out_needs_shift);


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_MCP_HID_KEYMAP_H */
