/**
 * @file   cmt_tokens.h
 * @brief  Pojmenované barevné tokeny okna Virtual CMT (návrh okna, SPEC v2).
 *
 * Jediné místo s barvami okna Virtual CMT (cmt_window.cpp). Hodnoty
 * jsou z návrhu okna (tabulka "Tokeny" a oddíl "Tokeny navíc
 * (K12, K13)"); tokeny označené "navíc" návrh nemá, rozhodnutí D-G2-2
 * a D-G3-1 shrnuje devdoc/cmt.md (oddíl "Rozhodnutí návrhu okna").
 *
 * Tokeny se aplikují PushStyleColor() jen uvnitř okna (cmt_window.cpp),
 * globální téma aplikace se nemění.
 *
 * Kontrasty hlavních dvojic (WCAG 2.2) jsou v devdoc/cmt.md, oddíl
 * "Rozhodnutí návrhu okna".
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef CMT_TOKENS_H
#define CMT_TOKENS_H

#include "libs/imgui/imgui.h"


/**
 * @brief Barva tokenu z hex zápisu 0xRRGGBB (neprůhledná).
 * @param hex Barva 0xRRGGBB.
 * @return ImU32 ve formátu ImGui (IM_COL32).
 */
static constexpr ImU32 cmt_tok_rgb ( unsigned hex )
{
    return IM_COL32 ( ( hex >> 16 ) & 0xFF, ( hex >> 8 ) & 0xFF, hex & 0xFF, 0xFF );
}

/* --- tokeny ze SPEC (tabulka Tokeny) --- */

/** @brief bg #0F0F0F: pozadí okna a řádku zpráv. */
static constexpr ImU32 CMT_TOK_BG = cmt_tok_rgb ( 0x0F0F0F );
/** @brief panel #1A1A1E: pozadí panelu decku. */
static constexpr ImU32 CMT_TOK_PANEL = cmt_tok_rgb ( 0x1A1A1E );
/** @brief border #3A3A44: dělicí čáry (1 px) a rámeček panelu; ne hrany ovládacích prvků (K13). */
static constexpr ImU32 CMT_TOK_BORDER = cmt_tok_rgb ( 0x3A3A44 );
/** @brief text #FFFFFF: hlavní text a ikony povolených tlačítek. */
static constexpr ImU32 CMT_TOK_TEXT = cmt_tok_rgb ( 0xFFFFFF );
/** @brief text-dim #9A9AA6: popisky, jednotky, ztlumené texty, řádek zpráv, tečka motoru. */
static constexpr ImU32 CMT_TOK_TEXT_DIM = cmt_tok_rgb ( 0x9A9AA6 );
/** @brief accent #4296FA: ukazatel průběhu bloku, stav Direct load (K13: jen značka pozice, průběh, výběr); čára místa vložení v tabulce bloků (tažení řádku, soubor z OS). */
static constexpr ImU32 CMT_TOK_ACCENT = cmt_tok_rgb ( 0x4296FA );
/** @brief play #5BD66B: stav Playing (text a tečka). */
static constexpr ImU32 CMT_TOK_PLAY = cmt_tok_rgb ( 0x5BD66B );
/** @brief rec #FF5A5A: stav Recording, ikona Record (kroužek, aktivní plný kruh). */
static constexpr ImU32 CMT_TOK_REC = cmt_tok_rgb ( 0xFF5A5A );
/** @brief warn #FFB333: Waiting for motor, Paused, Modified, "This block cannot be loaded directly". */
static constexpr ImU32 CMT_TOK_WARN = cmt_tok_rgb ( 0xFFB333 );
/** @brief error #FF6666: zpráva s chybou v řádku zpráv. */
static constexpr ImU32 CMT_TOK_ERROR = cmt_tok_rgb ( 0xFF6666 );

/* --- tokeny navíc ze SPEC (K12, K13) --- */

/** @brief control #2E2E36: plocha tlačítek, výběrů a neaktivních segmentů. */
static constexpr ImU32 CMT_TOK_CONTROL = cmt_tok_rgb ( 0x2E2E36 );
/** @brief control-border #6A6A78: hrana ovládacích prvků (3,26:1 na panel podle SPEC). */
static constexpr ImU32 CMT_TOK_CONTROL_BORDER = cmt_tok_rgb ( 0x6A6A78 );
/** @brief segment-on #2B6CC4: aktivní segment, primární tlačítko, zamáčknuté Blocks (bílý text). */
static constexpr ImU32 CMT_TOK_SEGMENT_ON = cmt_tok_rgb ( 0x2B6CC4 );

/* --- tokeny, které návrh nemá (D-G2-2, devdoc/cmt.md) --- */

/** @brief control-hover #3A3A46 (navíc): plocha ovládacího prvku pod myší. */
static constexpr ImU32 CMT_TOK_CONTROL_HOVER = cmt_tok_rgb ( 0x3A3A46 );
/** @brief control-active #464656 (navíc): plocha stisknutého ovládacího prvku. */
static constexpr ImU32 CMT_TOK_CONTROL_ACTIVE = cmt_tok_rgb ( 0x464656 );
/** @brief segment-on-hover #3070C8 (navíc): aktivní segment / primární tlačítko pod myší (bílý text 4,91:1). */
static constexpr ImU32 CMT_TOK_SEGMENT_ON_HOVER = cmt_tok_rgb ( 0x3070C8 );
/** @brief icon-disabled #5A5A64 (navíc, z promptů konceptů v2): ikona zakázaného tlačítka transportu. */
static constexpr ImU32 CMT_TOK_ICON_DISABLED = cmt_tok_rgb ( 0x5A5A64 );
/** @brief rec-active-bg #4A1C20 (navíc, koncept cmt-full-recording-v2 "dark red"): plocha tlačítka Record při nahrávání. */
static constexpr ImU32 CMT_TOK_REC_ACTIVE_BG = cmt_tok_rgb ( 0x4A1C20 );

/* --- tabulka bloků (návrh K14 a tokeny navíc, D-G3-1, devdoc/cmt.md) --- */

/** @brief text-dim-selected #B0B0BC (SPEC K14): ztlumený text ve vybraném řádku tabulky. */
static constexpr ImU32 CMT_TOK_TEXT_DIM_SELECTED = cmt_tok_rgb ( 0xB0B0BC );
/** @brief row-alt #202026 (navíc): pozadí lichého řádku tabulky (zebra; sudé = panel). */
static constexpr ImU32 CMT_TOK_ROW_ALT = cmt_tok_rgb ( 0x202026 );
/** @brief table-header #24242A (navíc): pozadí hlavičky tabulky. */
static constexpr ImU32 CMT_TOK_TABLE_HEADER = cmt_tok_rgb ( 0x24242A );
/**
 * @brief row-selected #28456B: neprůhledné pozadí vybraného řádku tabulky.
 *
 * Barva accent #4296FA s krytím 0x59 (35 %) složená na panel (koncept:
 * "accent blue background at low opacity"). Neprůhledná: průhledný
 * výběr na řádku zebry (row-alt) dával #2C4970 a ztlumenému textu
 * text-dim-selected jen 4,26:1; neprůhledný dává 4,54:1 na
 * každém řádku.
 */
static constexpr ImU32 CMT_TOK_ROW_SELECTED = cmt_tok_rgb ( 0x28456B );

/**
 * @brief error-selected #FF9999 (navíc): chybový text ve vybraném řádku tabulky.
 *
 * Text error #FF6666 má na row-selected #28456B jen 3,41:1; #FF9999 má
 * 4,77:1 (WCAG 2.2). Na nevybraném řádku zůstává
 * error.
 */
static constexpr ImU32 CMT_TOK_ERROR_SELECTED = cmt_tok_rgb ( 0xFF9999 );

/* --- dialog analýzy záznamu --- */

/**
 * @brief hist-bar #6A6A78: sloupce histogramu délek pulzů v dialogu Analyze recording.
 *
 * Neutrální šedá (hodnota control-border): accent je jen pro značky
 * krátkého a dlouhého pulzu vybraného
 * souboru, warn jen pro varování.
 */
static constexpr ImU32 CMT_TOK_HIST_BAR = cmt_tok_rgb ( 0x6A6A78 );

#endif /* CMT_TOKENS_H */
