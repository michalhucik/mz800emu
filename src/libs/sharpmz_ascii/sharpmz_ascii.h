/**
 * @file sharpmz_ascii.h
 * @brief Veřejné API pro konverzi znaků mezi Sharp MZ ASCII a standardním ASCII/UTF-8.
 *
 * Poskytuje funkce pro obousměrnou konverzi jednotlivých znaků mezi znakovou
 * sadou Sharp MZ-800/700 a standardním ASCII. Dále obsahuje funkci pro konverzi
 * Sharp MZ EU znaků do UTF-8 včetně evropských speciálních znaků (přehlásky,
 * Eszett, pí).
 *
 * MZ kódy 0x20-0x5D jsou identické s ASCII a procházejí beze změny.
 * MZ kódy 0x90-0xC0 se mapují na malá písmena a speciální znaky přes
 * konverzní tabulku sharpmz_ASCII_table.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 *
 * ----------------------------- License -------------------------------------
 *
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
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
 *
 * ---------------------------------------------------------------------------
 */

#ifndef SHARPMZ_ASCII_H
#define SHARPMZ_ASCII_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/** @brief Verze knihovny sharpmz_ascii. */
#define SHARPMZ_ASCII_VERSION "2.2.1"

/**
 * @brief Počet slotů rotujícího bufferu pro UTF-8 výstup konverzních funkcí.
 *
 * Funkce vracející UTF-8 řetězec (sharpmz_eu_convert_to_UTF8(),
 * sharpmz_jp_convert_to_UTF8(), sharpmz_koi8cs_convert_to_UTF8(),
 * sharpmz_display_to_utf8(), sharpmz_to_utf8()) vrací buď řetězcový literál,
 * nebo ukazatel do thread-local rotujícího bufferu společného pro celou
 * knihovnu. Vrácený ukazatel zůstává platný přes dalších
 * SHARPMZ_UTF8_ROTATING_SLOTS - 1 volání těchto funkcí ve stejném vlákně,
 * takže je lze volat vícekrát v jednom výrazu (např. printf s několika
 * %s argumenty).
 *
 * Hodnota 16 je kompromis mezi velikostí (64 B na vlákno) a použitelností.
 * Musí být > 0. Při změně je nutné překompilovat knihovnu.
 */
#define SHARPMZ_UTF8_ROTATING_SLOTS 16

/**
 * @brief Volba znakové sady / kódování pro funkce knihovny.
 *
 * Sharp MZ počítače používaly dvě varianty znakové sady - evropskou (EU)
 * a japonskou (JP). Hlavní rozdíl je v rozsahu 0x90-0xC0, kde EU má malá
 * písmena a-z a JP má znaky katakana. Knihovna dále podporuje:
 *
 * - KOI8-CS - 8-bit kódování s českou/slovenskou diakritikou používané
 *   v Sharp MZ CP/M ekosystému (LEC CP/M 1.4/4.1, NIPOS, LuckySoft).
 *   Bytes 0x00-0x7F jsou shodné s ASCII, bytes 0x80-0xFF obsahují
 *   diakritické znaky podle Cstools-3.44 specifikace.
 *
 * - DISPLAY_EU / DISPLAY_JP - "display kódy" (CG-ROM indexy zapisované
 *   přímo do VRAM 0xD000). Pro převod display -> ASCII se používá
 *   inverze tabulky !ADCN z dolního monitoru MZ-800/MZ-1500.
 *
 * @invariant Číselné hodnoty existujících konstant (EU=0, JP=1) se nesmí
 *            měnit kvůli binární kompatibilitě.
 */
typedef enum {
    SHARPMZ_CHARSET_EU = 0,         /**< Sharp MZ ASCII, evropská varianta */
    SHARPMZ_CHARSET_JP = 1,         /**< Sharp MZ ASCII, japonská varianta */
    SHARPMZ_CHARSET_KOI8CS = 2,     /**< KOI8-CS (čeština/slovenština pod CP/M) */
    SHARPMZ_CHARSET_DISPLAY_EU = 3, /**< Display kód (CG-ROM index), EU varianta */
    SHARPMZ_CHARSET_DISPLAY_JP = 4  /**< Display kód (CG-ROM index), JP varianta */
} sharpmz_charset_t;

/**
 * @brief Vrátí řetězec s verzí knihovny sharpmz_ascii.
 *
 * @return Ukazatel na statický řetězec s verzí (např. "2.2.1").
 *         Platný po celou dobu běhu programu.
 */
extern const char *sharpmz_ascii_version(void);

/**
 * @brief Konvertuje jeden znak ze Sharp MZ ASCII do standardního ASCII.
 *
 * MZ kódy 0x00-0x5D procházejí beze změny (jsou identické s ASCII).
 * MZ kód 0x80 se mapuje na '}'. MZ kódy 0x90-0xC0 se mapují přes
 * konverzní tabulku sharpmz_ASCII_table. Neznámé znaky se nahrazují mezerou.
 *
 * @param[in] c Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @return Odpovídající ASCII znak. Neznámé znaky vrací ' ' (mezeru).
 */
extern uint8_t sharpmz_cnv_from(uint8_t c);

/**
 * @brief Konvertuje jeden znak ze standardního ASCII do Sharp MZ ASCII.
 *
 * Provádí zpětnou konverzi - hledá odpovídající MZ kód pro daný ASCII znak.
 * MZ kódy 0x00-0x5D jsou identické s ASCII. Znak '}' se mapuje na MZ 0x80.
 * Ostatní se hledají v konverzní tabulce sharpmz_ASCII_table.
 *
 * @param[in] c Vstupní znak v ASCII (0x00-0xFF).
 * @return Odpovídající Sharp MZ ASCII kód. Neznámé znaky vrací ' ' (mezeru).
 */
extern uint8_t sharpmz_cnv_to(uint8_t c);

/**
 * @brief Konvertuje znak ze Sharp MZ ASCII do ASCII s informací o úspěšnosti.
 *
 * Rozšířená verze sharpmz_cnv_from(), která navíc vrací informaci o tom,
 * zda konverze proběhla úspěšně a zda je výsledný znak tisknutelný.
 *
 * - MZ kódy < 0x20: řídicí znaky - converted=1, printable=0, vrací ' '.
 * - MZ kódy 0x20-0x5D: ASCII kompatibilní - converted=1, printable=1.
 * - MZ kód 0x80: converted=1, printable=1, vrací '}'.
 * - MZ kódy 0x90-0xC0: z tabulky - converted=1 pokud nalezeno, jinak 0.
 * - Ostatní: grafické symboly - converted=0, printable=1, vrací ' '.
 *
 * @param[in]  c         Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @param[out] converted 1 pokud se podařila konverze do ASCII, 0 pokud ne.
 * @param[out] printable 1 pokud MZ znak reprezentuje tisknutelný glyf, 0 pokud ne.
 * @return Odpovídající ASCII znak. Při neúspěšné konverzi vrací ' ' (mezeru).
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 */
extern uint8_t sharpmz_convert_to_ASCII(uint8_t c, int *converted, int *printable);

/**
 * @brief Konvertuje znak ze Sharp MZ ASCII (EU varianta) do UTF-8.
 *
 * Nejprve se pokusí o konverzi přes sharpmz_convert_to_ASCII(). Pokud ta
 * vrátí converted=1, použije se výsledek (ASCII znak je platný UTF-8).
 * Pokud ne, zkusí se mapování na evropské speciální znaky:
 *
 * - 0x5E: ↑ (U+2191), 0x5F: ← (U+2190), 0xC6: → (U+2192), 0xFC: ↓ (U+2193)
 * - 0xE1: ♤ (U+2664), 0xF3: ♡ (U+2661), 0xF8: ♧ (U+2667), 0xFA: ♢ (U+2662)
 * - 0xA8: Ö (U+00D6), 0xAD: ü (U+00FC), 0xAE: ß (U+00DF)
 * - 0xB2: Ü (U+00DC), 0xB9: Ä (U+00C4), 0xBA: ö (U+00F6), 0xBB: ä (U+00E4)
 * - 0xFB: £ (U+00A3), 0xFF: π (U+03C0)
 *
 * U ostatních nekonvertovatelných znaků vrací mezeru s converted=0.
 *
 * @param[in]  c         Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @param[out] converted 1 pokud se podařila konverze, 0 pokud ne.
 * @param[out] printable 1 pokud MZ znak reprezentuje tisknutelný glyf, 0 pokud ne.
 * @return Ukazatel na null-terminated UTF-8 řetězec. Pro speciální znaky
 *         (vícebajtové UTF-8 sekvence) jde o řetězcový literál. Pro ASCII
 *         znaky jde o slot thread-local rotujícího bufferu; slot se
 *         recykluje po SHARPMZ_UTF8_ROTATING_SLOTS dalších voláních UTF-8
 *         funkcí knihovny ve stejném vlákně.
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 *
 * @note Funkce je thread-safe (rotující buffer je v thread-local storage).
 *       Pro uchování výsledku přes SHARPMZ_UTF8_ROTATING_SLOTS nebo více
 *       dalších volání proveďte kopii do vlastního bufferu.
 */
extern const char *sharpmz_eu_convert_to_UTF8(uint8_t c, int *converted, int *printable);

/**
 * @brief Konvertuje jeden UTF-8 znak do Sharp MZ ASCII (EU varianta).
 *
 * Pokud je vstupní znak jednobajtový (ASCII, 0x00-0x7F), provede konverzi
 * přes sharpmz_cnv_to(). Pokud je vícebajtový, zkusí rozpoznat evropské
 * speciální znaky (Ö, ü, ß, Ü, Ä, ö, ä, £, π). Neznámé znaky vrací
 * jako mezeru (0x20).
 *
 * @param[in] c Ukazatel na null-terminated UTF-8 řetězec. Zpracuje se
 *              pouze první znak (1-2 bajty).
 * @return Odpovídající Sharp MZ ASCII kód (0x00-0xFF).
 *         Neznámé znaky vrací 0x20 (mezeru).
 *
 * @pre c nesmí být NULL a musí ukazovat na platnou UTF-8 sekvenci.
 */
extern uint8_t sharpmz_eu_convert_UTF8_to(const char *c);

/**
 * @brief Konvertuje jeden znak ze Sharp MZ ASCII (JP varianta) do ASCII.
 *
 * JP varianta konvertuje pouze MZ kódy 0x00-0x5D (identické s ASCII).
 * Vše nad 0x5D (grafické symboly, katakana) se nahrazuje mezerou.
 *
 * @param[in] c Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @return Odpovídající ASCII znak. Pro kódy > 0x5D vrací ' ' (mezeru).
 */
extern uint8_t sharpmz_jp_cnv_from(uint8_t c);

/**
 * @brief Konvertuje jeden znak ze standardního ASCII do Sharp MZ ASCII (JP varianta).
 *
 * JP varianta podporuje pouze rozsah 0x00-0x5D (identický s ASCII).
 * Znaky mimo tento rozsah se nahrazují mezerou.
 *
 * @param[in] c Vstupní znak v ASCII (0x00-0xFF).
 * @return Odpovídající Sharp MZ ASCII kód. Pro kódy > 0x5D vrací ' ' (mezeru).
 */
extern uint8_t sharpmz_jp_cnv_to(uint8_t c);

/**
 * @brief Konvertuje znak ze Sharp MZ ASCII (JP varianta) do ASCII s informací o úspěšnosti.
 *
 * JP varianta konvertuje pouze rozsah 0x00-0x5D (identický s ASCII).
 * Vše nad 0x5D jsou grafické symboly nebo katakana bez ASCII ekvivalentu.
 *
 * - MZ kódy < 0x20: řídicí znaky - converted=1, printable=0, vrací ' '.
 * - MZ kódy 0x20-0x5D: ASCII kompatibilní - converted=1, printable=1.
 * - Ostatní: converted=0, printable=1, vrací ' '.
 *
 * @param[in]  c         Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @param[out] converted 1 pokud se podařila konverze do ASCII, 0 pokud ne.
 * @param[out] printable 1 pokud MZ znak reprezentuje tisknutelný glyf, 0 pokud ne.
 * @return Odpovídající ASCII znak. Při neúspěšné konverzi vrací ' ' (mezeru).
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 */
extern uint8_t sharpmz_jp_convert_to_ASCII(uint8_t c, int *converted, int *printable);

/**
 * @brief Konvertuje znak ze Sharp MZ ASCII (JP varianta) do UTF-8.
 *
 * Nejprve se pokusí o konverzi přes sharpmz_jp_convert_to_ASCII(). Pokud ta
 * vrátí converted=1, použije se výsledek. Pokud ne, zkusí se mapování na
 * japonské znaky (kanji, katakana, speciální znaky).
 *
 * Pokrytí:
 * - 0x5E: ↑ (U+2191), 0x5F: ← (U+2190), 0x80: ↓ (U+2193), 0xC0: → (U+2192)
 * - 0xE1: ♤ (U+2664), 0xF3: ♡ (U+2661), 0xF8: ♧ (U+2667), 0xFA: ♢ (U+2662)
 * - 0x70-0x76: kanji dnů v týdnu (日月火水木金土)
 * - 0x77-0x7C: kanji (生年時分秒円)
 * - 0x7D: ¥, 0x7E: £
 * - 0x86: ヲ, 0x87-0x8F: malé katakana (ァィゥェォャュョッ)
 * - 0x90: ー (chōon), 0x91-0xBD: katakana ア-ン, 0xBE: ゛
 * - 0xFF: π
 *
 * @param[in]  c         Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @param[out] converted 1 pokud se podařila konverze, 0 pokud ne.
 * @param[out] printable 1 pokud MZ znak reprezentuje tisknutelný glyf, 0 pokud ne.
 * @return Ukazatel na null-terminated UTF-8 řetězec (pro ASCII vstup slot
 *         thread-local rotujícího bufferu - viz sharpmz_eu_convert_to_UTF8()
 *         pro detaily životnosti).
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 * @note Identifikace kanji 0x77-0x7C je orientační a může vyžadovat opravu.
 * @note Funkce je thread-safe (rotující buffer v thread-local storage).
 */
extern const char *sharpmz_jp_convert_to_UTF8(uint8_t c, int *converted, int *printable);

/**
 * @brief Konvertuje jeden UTF-8 znak do Sharp MZ ASCII (JP varianta).
 *
 * Pokud je vstupní znak jednobajtový (ASCII, 0x00-0x7F), provede konverzi
 * přes sharpmz_jp_cnv_to(). Pokud je vícebajtový, zkusí rozpoznat japonské
 * znaky (kanji, katakana, šipky, karetní symboly, ¥, £, π).
 * Neznámé znaky vrací jako mezeru (0x20).
 *
 * @param[in] c Ukazatel na null-terminated UTF-8 řetězec. Zpracuje se
 *              pouze první znak (1-3 bajty).
 * @return Odpovídající Sharp MZ ASCII kód (0x00-0xFF).
 *         Neznámé znaky vrací 0x20 (mezeru).
 *
 * @pre c nesmí být NULL a musí ukazovat na platnou UTF-8 sekvenci.
 */
extern uint8_t sharpmz_jp_convert_UTF8_to(const char *c);

/* ======================================================================== */
/* KOI8-CS API                                                              */
/* ======================================================================== */

/**
 * @brief Konvertuje jeden znak z KOI8-CS do standardního ASCII.
 *
 * Bytes 0x00-0x7F procházejí beze změny (jsou shodné s ASCII).
 * Bytes 0x80-0xFF reprezentují diakritické znaky - provede se
 * transliterace na nejbližší ASCII písmeno (např. á -> 'a', č -> 'c',
 * Š -> 'S', ' digrafy "ch"/"CH" -> 'c'/'C' - první písmeno).
 * Pro nedefinované bytes vrací ' ' (mezeru).
 *
 * @param[in] c Vstupní znak v KOI8-CS (0x00-0xFF).
 * @return Odpovídající ASCII znak. Nedefinované znaky vrací ' '.
 *
 * @note Konverze je ztrátová (diakritika se odstraní). Pro plnohodnotnou
 *       konverzi použijte sharpmz_koi8cs_convert_to_UTF8().
 */
extern uint8_t sharpmz_koi8cs_cnv_from(uint8_t c);

/**
 * @brief Konvertuje jeden znak ze standardního ASCII do KOI8-CS.
 *
 * Pro bytes 0x00-0x7F probíhá passthrough (KOI8-CS sdílí ASCII rozsah).
 * Ostatní znaky vrací jako mezeru (ASCII nemá diakritiku, nelze mapovat
 * na KOI8-CS high-bit byte bezztrátově).
 *
 * @param[in] c Vstupní znak v ASCII (0x00-0xFF).
 * @return Odpovídající KOI8-CS byte. Pro c >= 0x80 vrací ' '.
 */
extern uint8_t sharpmz_koi8cs_cnv_to(uint8_t c);

/**
 * @brief Konvertuje znak z KOI8-CS do ASCII s informací o úspěšnosti.
 *
 * Rozšířená verze sharpmz_koi8cs_cnv_from() - vrací příznaky úspěšnosti.
 *
 * - bytes < 0x20: řídicí znaky, converted=1, printable=0, vrací ' '.
 * - bytes 0x20-0x7E: ASCII kompatibilní, converted=1, printable=1.
 * - byte 0x7F: DEL, converted=1, printable=0, vrací ' '.
 * - bytes 0x80-0xFF nedefinované (koi8cs_to_unicode[c] == 0): converted=0,
 *   printable=0, vrací ' '.
 * - bytes 0x80-0xFF s diakritikou: converted=1, printable=1, vrací
 *   transliterovaný ASCII znak (např. 'á' -> 'a').
 * - digrafy (0xC7 ch, 0xE7 CH): converted=1, printable=1, vrací 'c'/'C'
 *   (první písmeno digrafa).
 *
 * @param[in]  c         Vstupní KOI8-CS byte (0x00-0xFF).
 * @param[out] converted 1 pokud konverze úspěšná, 0 pokud ne.
 * @param[out] printable 1 pokud znak reprezentuje tisknutelný glyf.
 * @return ASCII znak nebo ' ' při neúspěchu.
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 */
extern uint8_t sharpmz_koi8cs_convert_to_ASCII(uint8_t c, int *converted, int *printable);

/**
 * @brief Konvertuje jeden znak z KOI8-CS do UTF-8.
 *
 * Vrací ukazatel na UTF-8 řetězec odpovídající KOI8-CS bytu.
 * Bytes 0x00-0x7F mapují 1:1 na ASCII. Bytes 0x80-0xFF s diakritikou
 * mapují na příslušný Unicode codepoint (U+00B0, U+010D, ...).
 * Digrafy "ch"/"CH" (0xC7/0xE7) se vrací jako dvouznaková UTF-8
 * sekvence "ch"/"CH" (input 1 byte -> output 2 bytes).
 *
 * Nedefinované bytes (mimo ASCII a mimo specifikované KOI8-CS znaky):
 * converted=0, printable=0, vrací mezeru " ".
 *
 * @param[in]  c         Vstupní KOI8-CS byte (0x00-0xFF).
 * @param[out] converted 1 pokud konverze úspěšná, 0 pokud ne.
 * @param[out] printable 1 pokud znak reprezentuje tisknutelný glyf.
 * @return Ukazatel na null-terminated UTF-8 řetězec. Nikdy nevrací NULL.
 *         Jde buď o řetězcový literál (mezera, digrafy), nebo o slot
 *         thread-local rotujícího bufferu - viz sharpmz_eu_convert_to_UTF8()
 *         pro detaily životnosti.
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 *
 * @note Funkce je thread-safe (rotující buffer v thread-local storage).
 */
extern const char *sharpmz_koi8cs_convert_to_UTF8(uint8_t c, int *converted, int *printable);

/**
 * @brief Konvertuje jeden UTF-8 znak do KOI8-CS.
 *
 * Pokud je vstupní znak ASCII (1 byte, 0x00-0x7F), vrací ho beze změny.
 * Pokud je vícebajtový, hledá odpovídající KOI8-CS byte v inverzní tabulce
 * (sekvenční search; KOI8-CS má jen 52 diakritických znaků).
 *
 * Digrafy: pokud vstup začíná "ch" nebo "CH", vrací 0xC7 / 0xE7 (sekvence
 * spotřebuje 2 ASCII bytes ze vstupu, ale této funkci stačí prefix - volající
 * musí o digrafu vědět při posunu).
 *
 * @param[in] c Ukazatel na null-terminated UTF-8 řetězec. Zpracuje
 *              první znak (1-3 bytes pro KOI8-CS rozsah U+0000-U+FFFF).
 * @return KOI8-CS byte (0x00-0xFF) nebo 0x20 pokud znak není v KOI8-CS.
 *
 * @pre c nesmí být NULL a musí ukazovat na validní UTF-8 sekvenci.
 */
extern uint8_t sharpmz_koi8cs_convert_UTF8_to(const char *c);

/* ======================================================================== */
/* Display kódy (CG-ROM index) API                                          */
/* ======================================================================== */

/**
 * @brief Konvertuje display kód (CG-ROM index) na Sharp MZ ASCII.
 *
 * Provede inverzi tabulky !ADCN z dolního monitoru MZ-800/MZ-1500.
 * Display kódy se zapisují přímo do VRAM 0xD000 a přímo indexují
 * CG-ROM generátor znaků. Inverze hledá první ASCII byte, jehož
 * mz_vcode_from_ascii() vrací zadaný display kód.
 *
 * @param[in] display_code Display kód (0x00-0xFF).
 * @param[in] charset      SHARPMZ_CHARSET_DISPLAY_EU nebo _DISPLAY_JP.
 * @return Odpovídající Sharp MZ ASCII (0x00-0xFF). Pro display kódy
 *         bez Sharp MZ ASCII ekvivalentu (např. grafické symboly)
 *         vrací ' ' (0x20).
 *
 * @pre charset musí být SHARPMZ_CHARSET_DISPLAY_EU nebo _DISPLAY_JP.
 */
extern uint8_t sharpmz_display_cnv_from(uint8_t display_code, sharpmz_charset_t charset);

/**
 * @brief Konvertuje Sharp MZ ASCII na display kód (CG-ROM index).
 *
 * Přímé použití tabulky !ADCN. Pro JP variantu se aplikuje korekce
 * na pozicích 0x6C a 0x6D.
 *
 * @param[in] ascii   Sharp MZ ASCII byte (0x00-0xFF).
 * @param[in] charset SHARPMZ_CHARSET_DISPLAY_EU nebo _DISPLAY_JP.
 * @return Display kód (0x00-0xFF). Hodnota 0xF0 = MZ_VCODE_NO_KEY
 *         (nezobrazitelný znak).
 *
 * @pre charset musí být SHARPMZ_CHARSET_DISPLAY_EU nebo _DISPLAY_JP.
 */
extern uint8_t sharpmz_display_cnv_to(uint8_t ascii, sharpmz_charset_t charset);

/**
 * @brief Konvertuje display kód (CG-ROM index) na UTF-8 řetězec.
 *
 * Nejprve provede display -> Sharp MZ ASCII přes sharpmz_display_cnv_from(),
 * pak Sharp MZ ASCII -> UTF-8 přes sharpmz_to_utf8(). Pro display kódy
 * bez Sharp MZ ASCII ekvivalentu (grafické symboly v rozsahu 0x80-0xFF
 * CG-ROM) vrací mezeru " ".
 *
 * @param[in] display_code Display kód (0x00-0xFF).
 * @param[in] charset      SHARPMZ_CHARSET_DISPLAY_EU nebo _DISPLAY_JP.
 * @return Ukazatel na null-terminated UTF-8 řetězec. Nikdy nevrací NULL.
 *         Životnost viz sharpmz_eu_convert_to_UTF8().
 *
 * @pre charset musí být SHARPMZ_CHARSET_DISPLAY_EU nebo _DISPLAY_JP.
 * @note Funkce je thread-safe (rotující buffer v thread-local storage).
 */
extern const char *sharpmz_display_to_utf8(uint8_t display_code, sharpmz_charset_t charset);

/* ======================================================================== */
/* Charset-dispatchované wrappery a řetězcové funkce                        */
/* ======================================================================== */

/**
 * @brief Konvertuje jeden znak (libovolný charset) na UTF-8 řetězec.
 *
 * Dispatch wrapper - vybírá správnou implementaci podle parametru charset:
 * - EU / JP: sharpmz_eu_convert_to_UTF8 / sharpmz_jp_convert_to_UTF8
 * - KOI8CS: sharpmz_koi8cs_convert_to_UTF8
 * - DISPLAY_EU / DISPLAY_JP: sharpmz_display_to_utf8
 *
 * Pro znaky bez Unicode ekvivalentu vrací mezeru (" ").
 *
 * @param[in] code     Vstupní byte (0x00-0xFF). Interpretace záleží na charset.
 * @param[in] charset  Volba kódování.
 * @return Ukazatel na null-terminated UTF-8 řetězec. Nikdy nevrací NULL.
 *         Životnost viz sharpmz_eu_convert_to_UTF8().
 *
 * @pre charset musí být platná hodnota sharpmz_charset_t.
 * @note Funkce je thread-safe (rotující buffer v thread-local storage).
 */
extern const char *sharpmz_to_utf8(uint8_t code, sharpmz_charset_t charset);

/**
 * @brief Konvertuje jeden UTF-8 znak na byte v daném kódování.
 *
 * Dispatch wrapper:
 * - EU / JP: hledá Sharp MZ ASCII byte
 * - KOI8CS: hledá KOI8-CS byte (per sharpmz_koi8cs_convert_UTF8_to)
 * - DISPLAY_EU / DISPLAY_JP: nejprve UTF-8 -> Sharp MZ ASCII (EU/JP),
 *   poté ASCII -> display kód
 *
 * @param[in] utf8     Ukazatel na null-terminated UTF-8 řetězec (zpracuje
 *                     první znak, 1-4 bajty).
 * @param[in] charset  Volba kódování.
 * @return Byte (0x00-0xFF) při úspěchu, -1 pokud znak není v daném
 *         kódování nalezen.
 *
 * @pre utf8 nesmí být NULL a musí ukazovat na validní UTF-8 sekvenci.
 * @pre charset musí být platná hodnota sharpmz_charset_t.
 */
extern int sharpmz_from_utf8(const char *utf8, sharpmz_charset_t charset);

/**
 * @brief Konvertuje řetězec Sharp MZ znaků na UTF-8.
 *
 * Iteruje přes zdrojový řetězec Sharp MZ kódů a zapisuje odpovídající
 * UTF-8 znaky do cílového bufferu. Pro znaky bez Unicode ekvivalentu
 * se zapisuje mezera (" "). Výstup je vždy
 * null-terminated (pokud dst_size > 0).
 *
 * @param[in]  src       Zdrojový řetězec Sharp MZ kódů.
 * @param[in]  src_len   Délka zdrojového řetězce v bajtech.
 * @param[out] dst       Cílový buffer pro UTF-8 výstup.
 * @param[in]  dst_size  Velikost cílového bufferu v bajtech (včetně místa
 *                       pro null terminátor).
 * @param[in]  charset   Varianta znakové sady (EU nebo JP).
 * @return Počet zapsaných bajtů (bez null terminátoru) při úspěchu,
 *         -1 pokud dst nebo src je NULL, nebo pokud dst_size je 0.
 *
 * @pre src nesmí být NULL.
 * @pre dst nesmí být NULL.
 * @pre dst_size musí být > 0.
 * @post dst je vždy null-terminated (pokud dst_size > 0).
 * @note Pokud cílový buffer nestačí, konverze se zastaví před znakem,
 *       který by způsobil přetečení. Výstup zůstane null-terminated.
 */
extern int sharpmz_str_to_utf8(const uint8_t *src, size_t src_len,
                                char *dst, size_t dst_size,
                                sharpmz_charset_t charset);

/**
 * @brief Konvertuje UTF-8 řetězec na řetězec Sharp MZ kódů.
 *
 * Parsuje UTF-8 vstup a pro každý Unicode znak hledá odpovídající
 * Sharp MZ kód. Neznámé znaky se nahrazují mezerou (0x20).
 * Výstup je vždy null-terminated (pokud dst_size > 0).
 *
 * @param[in]  src       Zdrojový UTF-8 řetězec (null-terminated).
 * @param[out] dst       Cílový buffer pro Sharp MZ kódy.
 * @param[in]  dst_size  Velikost cílového bufferu v bajtech (včetně místa
 *                       pro null terminátor).
 * @param[in]  charset   Varianta znakové sady (EU nebo JP).
 * @return Počet zapsaných bajtů (bez null terminátoru) při úspěchu,
 *         -1 pokud dst nebo src je NULL, nebo pokud dst_size je 0.
 *
 * @pre src nesmí být NULL a musí být validní null-terminated UTF-8.
 * @pre dst nesmí být NULL.
 * @pre dst_size musí být > 0.
 * @post dst je vždy null-terminated (pokud dst_size > 0).
 * @note Nevalidní UTF-8 sekvence se přeskakují (posun o 1 bajt).
 */
extern int sharpmz_str_from_utf8(const char *src,
                                  uint8_t *dst, size_t dst_size,
                                  sharpmz_charset_t charset);

#ifdef __cplusplus
}
#endif

#endif /* SHARPMZ_ASCII_H */
