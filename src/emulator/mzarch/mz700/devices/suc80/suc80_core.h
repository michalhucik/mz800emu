/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file suc80_core.h
 * @brief Čisté jádro modelu 80sloupcového kitu Sharp Users Club UK (SUC) pro MZ-700.
 *
 * Kit "MZ-700 80 Column Kit" (SUC 1990) nemá vlastní VRAM. V jedné 40sloupcové
 * znakové periodě zobrazí dva znaky: první z dolní poloviny znakové VRAM
 * (blok A, 0D000h - 0D3E7h), druhý z horní poloviny (blok B, 0D401h - 0D7E8h).
 * Režim 40 / 80 sloupců drží latch, který se nastaví bitem D4 při každém zápisu
 * do oblasti 0E00Ch - 0E00Fh; po /RESET je 40 sloupců.
 *
 * Modul obsahuje jen model (latch, mapování adres, kreslení řádku obrazu,
 * serializaci stavu). Nezávisí na emulátoru (jen <stdint.h>, <stdbool.h>,
 * <string.h>), nečte globální stav a nic nealokuje; napojení na sběrnici
 * a framebuffer dělá lepidlo zařízení (devbus).
 *
 * Formát vstupů a výstupu odpovídá 40sloupcovému kreslení MZ-700 v emulátoru
 * (src/emulator/mzarch/mz700/gdg/mz700_framebuffer.c, funkce
 * framebuffer_update_MZ700_screen_row a framebuffer_MZ700_screen_row_fill):
 *  - VRAM je jedno pole 4 kB, index = adresa - 0D000h (znaky 0D000h - 0D7FFh,
 *    atributy 0D800h - 0DFFFh);
 *  - generátor znaků 4 kB = 2 sady x 256 znaků x 8 bajtů, bajt = jeden mikrořádek
 *    znaku, bit 0 = levý bod;
 *  - atribut: bity 6-4 barva popředí, bity 2-0 barva pozadí, bit 7 sada znaků
 *    (adresa v generátoru | 800h);
 *  - výstup 640 bajtů na řádek plátna, hodnota = colormap[index barvy 0-7].
 *
 * Zdroj chování: báze mz800-knowledge, public/reference/agent/hw/28-mz700-suc-80-sloupcu.md
 * (SUC magazine 5/2015, str. 13 - 19). Na reálném HW nic neměřeno.
 *
 * @note Modul není vláknově bezpečný; stav mění i čte jedno vlákno
 *       (v emulátoru emu vlákno).
 */

#ifndef SUC80_CORE_H
#define SUC80_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Velikost VRAM MZ-700, kterou jádro čte (znaky + atributy, 0D000h - 0DFFFh). */
#define SUC80_VRAM_SIZE         0x1000

/** @brief Offset atributové VRAM ve VRAM (0D800h - 0D000h). */
#define SUC80_VRAM_ATTR_OFFSET  0x0800

/** @brief Offset bloku B znakové VRAM (0D401h - 0D000h); liché sloupce v 80 sloupcích. */
#define SUC80_VRAM_BLOCK_B      0x0401

/** @brief Velikost generátoru znaků (2 sady x 256 znaků x 8 bajtů). */
#define SUC80_CG_SIZE           0x1000

/** @brief Počet obrazových řádků plátna (25 textových řádků x 8 mikrořádků). */
#define SUC80_SCREEN_LINES      200

/** @brief Počet textových řádků. */
#define SUC80_TEXT_ROWS         25

/** @brief Počet sloupců v režimu 80 sloupců. */
#define SUC80_COLUMNS_80        80

/** @brief Počet sloupců v režimu 40 sloupců (obraz jako bez kitu). */
#define SUC80_COLUMNS_40        40

/** @brief Šířka výstupního řádku v bodech (80 x 8 = 40 x 16 = 640). */
#define SUC80_ROW_PIXELS        640

/** @brief Bit zapsané hodnoty, který latch kitu ukládá (datový bit D4). */
#define SUC80_LATCH_BIT         0x10

/** @brief Návratová hodnota mapovacích funkcí pro pozici mimo obrazovku. */
#define SUC80_OFFSET_INVALID    0xFFFFu

/** @brief Verze binárního formátu stavu (suc80_state_save / suc80_state_load). */
#define SUC80_STATE_VERSION     1

/** @brief Velikost uloženého stavu verze SUC80_STATE_VERSION v bajtech. */
#define SUC80_STATE_SIZE        4

/**
 * @brief Režim zobrazení, který drží latch kitu.
 */
typedef enum en_SUC80_MODE {
    SUC80_MODE_40 = 0, /**< 40 sloupců: obraz jako bez kitu (latch D4 = 0, stav po /RESET). */
    SUC80_MODE_80 = 1, /**< 80 sloupců: prokládání bloků A a B (latch D4 = 1). */
} en_SUC80_MODE;

/**
 * @brief Volba granularity atributů v režimu 80 sloupců.
 *
 * Pramen (SUC 2015) o barvách v 80 sloupcích mlčí; obě varianty jsou hypotézy
 * báze (hw/28, oddíl Rozložení obrazu ve VRAM). Režim 40 volba neovlivňuje.
 */
typedef enum en_SUC80_ATTR_MODE {
    SUC80_ATTR_PER_CHAR = 0, /**< Každý znak svůj atribut: sudé c 0D800h + k, liché c 0DC01h + k (výchozí). */
    SUC80_ATTR_PER_PAIR = 1, /**< Dvojice znaků sdílí atribut z bloku A: 0D800h + k. */
} en_SUC80_ATTR_MODE;

/**
 * @brief Výsledek načtení stavu (suc80_state_load); záporné hodnoty = chyba.
 */
typedef enum en_SUC80_RESULT {
    SUC80_OK = 0,           /**< Úspěch. */
    SUC80_ERR_INVALID = -1, /**< Chybný argument (NULL ukazatel). */
    SUC80_ERR_SIZE = -2,    /**< Buffer je kratší, než vyžaduje formát. */
    SUC80_ERR_VERSION = -3, /**< Neznámá verze formátu. */
    SUC80_ERR_VALUE = -4,   /**< Hodnota pole mimo povolený rozsah. */
} en_SUC80_RESULT;

/**
 * @brief Stav kitu SUC.
 *
 * Struktura má pevná pole bez ukazatelů, lze ji kopírovat přiřazením.
 *
 * @invariant mode je SUC80_MODE_40 nebo SUC80_MODE_80.
 * @invariant attr_mode je SUC80_ATTR_PER_CHAR nebo SUC80_ATTR_PER_PAIR.
 * @note Vlastnictví: struktura patří volajícímu (typicky statický kontext lepidla).
 */
typedef struct st_SUC80 {
    uint8_t mode;      /**< Stav latche, en_SUC80_MODE. Mění ho suc80_latch_write a suc80_reset. */
    uint8_t attr_mode; /**< Volba granularity atributů, en_SUC80_ATTR_MODE. /RESET ji nemění. */
} st_SUC80;

/**
 * @brief Inicializuje stav na výchozí hodnoty (zapnutí napájení).
 * @param s Stav; NULL se ignoruje.
 * @post mode = SUC80_MODE_40, attr_mode = SUC80_ATTR_PER_CHAR.
 */
void suc80_init(st_SUC80 *s);

/**
 * @brief Signál /RESET stroje: latch se vynuluje (40 sloupců).
 *
 * Odpovídá nulování klopného obvodu 7474 od /RESET (schéma SUC 2015, str. 18,
 * podle báze hw/28). Volba granularity atributů je nastavení emulátoru a zůstává.
 *
 * @param s Stav; NULL se ignoruje (vrací false).
 * @return true, když se režim změnil (byl 80 sloupců); volající má zneplatnit obraz.
 * @post s->mode == SUC80_MODE_40.
 */
bool suc80_reset(st_SUC80 *s);

/**
 * @brief Zápis do latche (lepidlo volá při každém zápisu CPU na 0E00Ch - 0E00Fh).
 *
 * Latch uloží bit D4 zapsané hodnoty (SUC80_LATCH_BIT): 0 = 40 sloupců (např. 28h),
 * 1 = 80 sloupců (např. 50h). Ostatní bity se ignorují. Funkce nedekóduje adresu,
 * o rozsahu a stavu mapy paměti rozhoduje volající. Čtení latche neexistuje.
 *
 * @param s Stav; NULL se ignoruje (vrací false).
 * @param value Zapsaná hodnota datové sběrnice.
 * @return true, když se režim změnil; volající má zneplatnit obraz.
 */
bool suc80_latch_write(st_SUC80 *s, uint8_t value);

/**
 * @brief Vrátí aktuální režim zobrazení.
 * @param s Stav (nesmí být NULL).
 * @return SUC80_MODE_40 nebo SUC80_MODE_80.
 */
en_SUC80_MODE suc80_get_mode(const st_SUC80 *s);

/**
 * @brief Nastaví volbu granularity atributů.
 * @param s Stav; NULL se ignoruje (vrací false).
 * @param attr_mode Nová volba.
 * @return true, když byla hodnota platná a uložila se; false pro neznámou hodnotu
 *         (stav se nemění).
 * @note Režim 40 sloupců volba neovlivňuje; v režimu 80 má volající zneplatnit obraz.
 */
bool suc80_set_attr_mode(st_SUC80 *s, en_SUC80_ATTR_MODE attr_mode);

/**
 * @brief Vrátí volbu granularity atributů.
 * @param s Stav (nesmí být NULL).
 * @return SUC80_ATTR_PER_CHAR nebo SUC80_ATTR_PER_PAIR.
 */
en_SUC80_ATTR_MODE suc80_get_attr_mode(const st_SUC80 *s);

/**
 * @brief Offset znaku na pozici (řádek, sloupec) v režimu 80 sloupců.
 *
 * `k = 40 * row + col / 2`; sudý sloupec -> k (blok A, 0D000h + k), lichý
 * -> 401h + k (blok B, 0D401h + k). Offset 400h (0D400h) nevrátí nikdy.
 *
 * @param row Textový řádek 0 - 24.
 * @param col Sloupec 0 - 79.
 * @return Offset ve VRAM (index = adresa - 0D000h), 0 - 7E8h; pro pozici mimo
 *         rozsah SUC80_OFFSET_INVALID.
 */
uint16_t suc80_char_offset(unsigned row, unsigned col);

/**
 * @brief Offset atributu na pozici (řádek, sloupec) v režimu 80 sloupců.
 *
 * SUC80_ATTR_PER_CHAR: sudý sloupec 800h + k, lichý C01h + k.
 * SUC80_ATTR_PER_PAIR: 800h + k pro oba sloupce dvojice.
 *
 * @param attr_mode Volba granularity (neznámá hodnota se bere jako PER_CHAR).
 * @param row Textový řádek 0 - 24.
 * @param col Sloupec 0 - 79.
 * @return Offset ve VRAM (index = adresa - 0D000h), 800h - 0FE8h; pro pozici mimo
 *         rozsah SUC80_OFFSET_INVALID.
 */
uint16_t suc80_attr_offset(en_SUC80_ATTR_MODE attr_mode, unsigned row, unsigned col);

/**
 * @brief Nakreslí jeden obrazový řádek v 80 sloupcích (bez ohledu na latch).
 *
 * Pro každý z 80 znaků: atribut podle volby granularity, adresa vzoru
 * `((attr & 80h) << 4) | (znak * 8)` plus `line % 8` v generátoru, 8 bodů bez
 * zdvojení, bit 0 vzoru = levý bod, bit 1 -> colormap[(attr >> 4) & 7],
 * bit 0 -> colormap[attr & 7].
 *
 * @param s Stav (čte se jen volba granularity atributů); nesmí být NULL.
 * @param vram VRAM 4 kB (SUC80_VRAM_SIZE), index = adresa - 0D000h.
 * @param cg Generátor znaků 4 kB (SUC80_CG_SIZE) ve formátu CG-ROM emulátoru
 *           (CG-ROM, nebo efektivní generátor jiného zařízení).
 * @param colormap 8 výstupních hodnot pro indexy barev 0 - 7 (c_MZ700_COLORMAP).
 * @param line Obrazový řádek plátna 0 - 199.
 * @param dst Výstup, SUC80_ROW_PIXELS bajtů (začátek řádku plátna bez okraje).
 * @return true, když řádek nakreslil; false pro NULL argument nebo line >= 200
 *         (dst se nemění).
 * @pre Vstupní pole se během volání nemění.
 * @note Nemění stav ani vstupy; čte jen pole předaná argumenty.
 */
bool suc80_render_line80(const st_SUC80 *s, const uint8_t *vram, const uint8_t *cg,
                         const uint8_t colormap[8], unsigned line, uint8_t *dst);

/**
 * @brief Nakreslí jeden obrazový řádek v 40 sloupcích jako MZ-700 bez kitu.
 *
 * Výstup odpovídá framebuffer_update_MZ700_screen_row (mz700_framebuffer.c),
 * jen zdroj glyfů a barevná mapa jsou z argumentů: k = 40 * r + c, znak z k,
 * atribut z 800h + k, 8 bodů vzoru zdvojených na 16.
 *
 * @param vram VRAM 4 kB, index = adresa - 0D000h.
 * @param cg Generátor znaků 4 kB ve formátu CG-ROM emulátoru.
 * @param colormap 8 výstupních hodnot pro indexy barev 0 - 7.
 * @param line Obrazový řádek plátna 0 - 199.
 * @param dst Výstup, SUC80_ROW_PIXELS bajtů.
 * @return true, když řádek nakreslil; false pro NULL argument nebo line >= 200
 *         (dst se nemění).
 */
bool suc80_render_line40(const uint8_t *vram, const uint8_t *cg,
                         const uint8_t colormap[8], unsigned line, uint8_t *dst);

/**
 * @brief Nakreslí obrazový řádek podle aktuálního stavu latche.
 *
 * Režim 80 -> suc80_render_line80, režim 40 -> suc80_render_line40. Je určena
 * jako tělo háčku render_row lepidla: změna režimu se projeví od příštího
 * kresleného řádku.
 *
 * @param s Stav; nesmí být NULL.
 * @param vram VRAM 4 kB, index = adresa - 0D000h.
 * @param cg Generátor znaků 4 kB.
 * @param colormap 8 výstupních hodnot pro indexy barev 0 - 7.
 * @param line Obrazový řádek plátna 0 - 199.
 * @param dst Výstup, SUC80_ROW_PIXELS bajtů.
 * @return true, když řádek nakreslil; false pro NULL argument nebo line >= 200
 *         (dst se nemění).
 */
bool suc80_render_line(const st_SUC80 *s, const uint8_t *vram, const uint8_t *cg,
                       const uint8_t colormap[8], unsigned line, uint8_t *dst);

/**
 * @brief Uloží stav do bajtového bufferu (snapshot).
 *
 * Formát verze 1, SUC80_STATE_SIZE bajtů, vícebajtová pole little-endian:
 * | offset | velikost | obsah |
 * |--------|----------|-------|
 * | 0 | 2 | verze formátu (SUC80_STATE_VERSION) |
 * | 2 | 1 | mode (en_SUC80_MODE) |
 * | 3 | 1 | attr_mode (en_SUC80_ATTR_MODE) |
 *
 * @param s Stav; nesmí být NULL.
 * @param buf Cílový buffer.
 * @param size Velikost bufferu v bajtech.
 * @return Počet zapsaných bajtů (SUC80_STATE_SIZE); 0 pro NULL argument nebo
 *         size < SUC80_STATE_SIZE (buffer se nemění).
 */
size_t suc80_state_save(const st_SUC80 *s, uint8_t *buf, size_t size);

/**
 * @brief Načte stav z bajtového bufferu uloženého suc80_state_save.
 *
 * Bajty za koncem formátu dané verze se ignorují.
 *
 * @param s Cílový stav; při chybě se nemění.
 * @param buf Zdrojový buffer.
 * @param size Velikost dat v bajtech.
 * @return SUC80_OK; SUC80_ERR_INVALID (NULL), SUC80_ERR_SIZE (krátký buffer),
 *         SUC80_ERR_VERSION (neznámá verze), SUC80_ERR_VALUE (mode nebo attr_mode
 *         mimo rozsah).
 * @post Při SUC80_OK platí invarianty st_SUC80.
 */
int suc80_state_load(st_SUC80 *s, const uint8_t *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SUC80_CORE_H */
