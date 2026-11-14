/**
 * @file sharpmz_ascii.c
 * @brief Implementace konverze znaků mezi Sharp MZ ASCII a standardním ASCII/UTF-8.
 *
 * Obsahuje konverzní tabulku pro rozsah MZ kódů 0x90-0xC0 (malá písmena
 * a speciální znaky) a funkce pro obousměrnou konverzi jednotlivých znaků.
 * MZ kódy 0x20-0x5D jsou identické s ASCII a procházejí beze změny.
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

#include <stdint.h>
#include <string.h>
#include "sharpmz_ascii.h"

/**
 * @brief Velikost jednoho slotu rotujícího bufferu v bajtech.
 *
 * Stačí na nejdelší dynamicky skládaný výstup: 3 bajty UTF-8 (codepoint
 * do U+FFFF) + null terminátor.
 */
#define SHARPMZ_UTF8_SLOT_SIZE 4

/**
 * @brief Vrátí další slot thread-local rotujícího bufferu pro UTF-8 výstup.
 *
 * Společný buffer všech funkcí knihovny, které skládají UTF-8 výstup za běhu
 * (ASCII větve *_convert_to_UTF8() a koi8cs_encode_utf8()). Sloty se
 * přidělují cyklicky, takže vrácený ukazatel zůstává platný přes dalších
 * SHARPMZ_UTF8_ROTATING_SLOTS - 1 volání této funkce ve stejném vlákně.
 *
 * @return Ukazatel na slot o velikosti SHARPMZ_UTF8_SLOT_SIZE bajtů.
 *         Obsah slotu není inicializován - volající musí zapsat i null
 *         terminátor. Nikdy nevrací NULL.
 *
 * @post Vnitřní index slotu je posunut o 1 (modulo počet slotů).
 * @note Thread-safe: buffer i index jsou v thread-local storage (__thread,
 *       rozšíření GCC/Clang). Ukazatel nesmí být předán jinému vláknu, které
 *       by ho používalo po skončení vlákna volajícího.
 */
static char *sharpmz_utf8_next_slot(void)
{
    static __thread char slots[SHARPMZ_UTF8_ROTATING_SLOTS][SHARPMZ_UTF8_SLOT_SIZE];
    static __thread unsigned int next_slot = 0;

    char *buf = slots[next_slot];
    next_slot = (next_slot + 1) % SHARPMZ_UTF8_ROTATING_SLOTS;
    return buf;
}


/**
 * @brief Konverzní tabulka Sharp MZ kódů 0x90-0xC0 na ASCII znaky.
 *
 * Index pole odpovídá MZ kódu minus 0x90 (tedy index 0 = MZ 0x90, atd.).
 * Položky s hodnotou ' ' (mezera) reprezentují MZ znaky, které nemají
 * přímý ASCII ekvivalent (grafické symboly).
 *
 * Mapování: 0x90='_', 0x92='e', 0x94='~', 0x96='t', 0x97='g', 0x98='h',
 * 0x9A='b', 0x9B='x', 0x9C='d', 0x9D='r', 0x9E='p', 0x9F='c', 0xA0='q',
 * 0xA1='a', 0xA2='z', 0xA3='w', 0xA4='s', 0xA5='u', 0xA6='i', 0xA9='k',
 * 0xAA='f', 0xAB='v', 0xAF='j', 0xB0='n', 0xB3='m', 0xB7='o', 0xB8='l',
 * 0xBD='y', 0xBE='{', 0xC0='|'.
 */
const uint8_t sharpmz_eu_ASCII_table[49] = {
    '_', ' ', 'e', '`', '~', ' ', 't', 'g',
    'h', ' ', 'b', 'x', 'd', 'r', 'p', 'c',
    'q', 'a', 'z', 'w', 's', 'u', 'i', ' ',
    ' ', 'k', 'f', 'v', ' ', ' ', ' ', 'j',
    'n', ' ', ' ', 'm', ' ', ' ', ' ', 'o',
    'l', ' ', ' ', ' ', ' ', 'y', '{', ' ',
    '|'};

/**
 * @brief Konvertuje jeden znak ze Sharp MZ ASCII do standardního ASCII.
 *
 * MZ kódy 0x00-0x5D procházejí beze změny (jsou identické s ASCII).
 * MZ kód 0x80 se mapuje na '}', 0x8B na '^', 0x93 na '`'.
 * MZ kódy 0x90-0xC0 se mapují přes konverzní tabulku sharpmz_eu_ASCII_table.
 * Neznámé znaky se nahrazují mezerou.
 *
 * @param[in] c Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @return Odpovídající ASCII znak. Neznámé znaky vrací ' ' (mezeru).
 */
uint8_t sharpmz_cnv_from(uint8_t c)
{
    if (c <= 0x5d)
        return (c);
    if (c == 0x80)
        return ('}');
    if (c == 0x8B)
        return ('^');
    if (c < 0x90 || c > 0xc0)
        return (' '); /* z neznámých znaků uděláme ' ' */
    return (sharpmz_eu_ASCII_table[c - 0x90]);
}

/**
 * @brief Konvertuje jeden znak ze standardního ASCII do Sharp MZ ASCII.
 *
 * Provádí zpětnou konverzi - hledá odpovídající MZ kód pro daný ASCII znak.
 * MZ kódy 0x00-0x5D jsou identické s ASCII. Znak '}' se mapuje na MZ 0x80.
 * Ostatní se hledají v konverzní tabulce sharpmz_eu_ASCII_table.
 *
 * @param[in] c Vstupní znak v ASCII (0x00-0xFF).
 * @return Odpovídající Sharp MZ ASCII kód. Neznámé znaky vrací ' ' (mezeru).
 */
uint8_t sharpmz_cnv_to(uint8_t c)
{
    uint8_t i;

    if (c <= 0x5d)
        return (c);
    if (c == '}')
        return (0x80);
    if (c == '^')
        return (0x8B);
    for (i = 0; i < sizeof(sharpmz_eu_ASCII_table); i++)
    {
        if (c == sharpmz_eu_ASCII_table[i])
        {
            return (i + 0x90);
        };
    };
    return (' '); /* z neznámých znaků uděláme ' ' */
}

/**
 * @brief Vrátí řetězec s verzí knihovny sharpmz_ascii.
 *
 * @return Ukazatel na statický řetězec s verzí (např. "2.2.1").
 *         Platný po celou dobu běhu programu.
 */
const char *sharpmz_ascii_version(void)
{
    return SHARPMZ_ASCII_VERSION;
}

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
uint8_t sharpmz_convert_to_ASCII(uint8_t c, int *converted, int *printable)
{
    if (c < 0x20)
    {
        *printable = 0;
        *converted = 1;
        return (' ');
    }
    else
    {
        *printable = 1;
    }

    if (c <= 0x5d)
    {
        *converted = 1;
        return (c);
    }

    if (c == 0x80)
    {
        *converted = 1;
        return ('}');
    }

    if (c == 0x8B)
    {
        *converted = 1;
        return ('^');
    }

    if (c < 0x90 || c > 0xc0)
    {
        *converted = 0;
        return (' '); /* z neznámých znaků uděláme ' ' */
    }

    const uint8_t *cnvchr = &sharpmz_eu_ASCII_table[c - 0x90];
    *converted = (*cnvchr != ' ');
    return (*cnvchr);
}

/**
 * @brief Konvertuje znak ze Sharp MZ ASCII (EU varianta) do UTF-8.
 *
 * Nejprve se pokusí o konverzi přes sharpmz_convert_to_ASCII(). Pokud ta
 * vrátí converted=1, použije se výsledek (ASCII znak je platný UTF-8).
 * Pokud ne, zkusí se mapování na evropské speciální znaky:
 *
 * - 0xA8: Ö (U+00D6)
 * - 0xAD: ü (U+00FC)
 * - 0xAE: ß (U+00DF)
 * - 0xB2: Ü (U+00DC)
 * - 0xB9: Ä (U+00C4)
 * - 0xBA: ö (U+00F6)
 * - 0xBB: ä (U+00E4)
 * - 0xFF: π (U+03C0)
 *
 * U ostatních nekonvertovatelných znaků vrací mezeru s converted=0.
 *
 * @param[in]  c         Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @param[out] converted 1 pokud se podařila konverze, 0 pokud ne.
 * @param[out] printable 1 pokud MZ znak reprezentuje tisknutelný glyf, 0 pokud ne.
 * @return Ukazatel na null-terminated UTF-8 řetězec. Pro speciální znaky
 *         jde o řetězcový literál, pro ASCII znaky o slot thread-local
 *         rotujícího bufferu (viz SHARPMZ_UTF8_ROTATING_SLOTS).
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 *
 * @note Funkce je thread-safe; rotující buffer umožňuje více volání v jednom
 *       výrazu. Pro uchování výsledku přes SHARPMZ_UTF8_ROTATING_SLOTS nebo
 *       více dalších volání UTF-8 funkcí knihovny proveďte vlastní kopii.
 */
const char *sharpmz_eu_convert_to_UTF8(uint8_t c, int *converted, int *printable)
{
    /* Zkusíme ASCII konverzi */
    uint8_t ascii_ch = sharpmz_convert_to_ASCII(c, converted, printable);
    if (*converted)
    {
        char *buf = sharpmz_utf8_next_slot();
        buf[0] = (char)ascii_ch;
        buf[1] = '\0';
        return buf;
    }

    /* EU speciální znaky */
    *printable = 1;
    *converted = 1;

    switch (c)
    {
        /* Šipky */
        case 0x5E: return "\xE2\x86\x91"; /* ↑ - šipka nahoru */
        case 0x5F: return "\xE2\x86\x90"; /* ← - šipka vlevo */
        case 0xC6: return "\xE2\x86\x92"; /* → - šipka vpravo */
        case 0xFC: return "\xE2\x86\x93"; /* ↓ - šipka dolů */

        /* Karetní symboly */
        case 0xE1: return "\xE2\x99\xA4"; /* ♤ - pika */
        case 0xF3: return "\xE2\x99\xA1"; /* ♡ - srdce */
        case 0xF8: return "\xE2\x99\xA7"; /* ♧ - kříž */
        case 0xFA: return "\xE2\x99\xA2"; /* ♢ - káry */

        /* Přehlásky a speciální znaky */
        case 0xA8: return "\xC3\x96"; /* Ö - velké O s přehláskou */
        case 0xAD: return "\xC3\xBC"; /* ü - malé u s přehláskou */
        case 0xAE: return "\xC3\x9F"; /* ß - ostré s (Eszett) */
        case 0xB2: return "\xC3\x9C"; /* Ü - velké U s přehláskou */
        case 0xB9: return "\xC3\x84"; /* Ä - velké A s přehláskou */
        case 0xBA: return "\xC3\xB6"; /* ö - malé o s přehláskou */
        case 0xBB: return "\xC3\xA4"; /* ä - malé a s přehláskou */
        case 0xFB: return "\xC2\xA3"; /* £ - symbol libry */
        case 0xFF: return "\xCF\x80"; /* π - malé pí */
    }

    /* Neznámý znak - grafický symbol bez UTF-8 ekvivalentu */
    *converted = 0;
    return " ";
}

/**
 * @brief Konvertuje jeden UTF-8 znak do Sharp MZ ASCII (EU varianta).
 *
 * Pokud je vstupní znak jednobajtový (ASCII, 0x00-0x7F), provede konverzi
 * přes sharpmz_cnv_to(). Pokud je vícebajtový, zkusí rozpoznat evropské
 * speciální znaky (přehlásky, Eszett, libra, pí). Neznámé znaky vrací
 * jako mezeru (0x20).
 *
 * @param[in] c Ukazatel na null-terminated UTF-8 řetězec. Zpracuje se
 *              pouze první znak (1-2 bajty).
 * @return Odpovídající Sharp MZ ASCII kód (0x00-0xFF).
 *         Neznámé znaky vrací 0x20 (mezeru).
 *
 * @pre c nesmí být NULL a musí ukazovat na platnou UTF-8 sekvenci.
 */
uint8_t sharpmz_eu_convert_UTF8_to(const char *c)
{
    const unsigned char *s = (const unsigned char *)c;

    /* Jednobajtový znak (ASCII) - konverze přes sharpmz_cnv_to() */
    if (s[0] < 0x80)
    {
        return sharpmz_cnv_to(s[0]);
    }

    /* Dvoubajtová UTF-8 sekvence (0xC2-0xC3 xx) */
    if (s[0] == 0xC3 && s[1])
    {
        switch (s[1])
        {
            case 0x96: return 0xA8; /* Ö */
            case 0xBC: return 0xAD; /* ü */
            case 0x9F: return 0xAE; /* ß */
            case 0x9C: return 0xB2; /* Ü */
            case 0x84: return 0xB9; /* Ä */
            case 0xB6: return 0xBA; /* ö */
            case 0xA4: return 0xBB; /* ä */
        }
    }

    if (s[0] == 0xC2 && s[1] == 0xA3)
    {
        return 0xFB; /* £ */
    }

    /* Dvoubajtová UTF-8 sekvence (0xCF 0x80) = π */
    if (s[0] == 0xCF && s[1] == 0x80)
    {
        return 0xFF; /* π */
    }

    /* Tříbajtové UTF-8 sekvence (0xE2 xx xx) - šipky a karetní symboly */
    if (s[0] == 0xE2 && s[1] && s[2])
    {
        if (s[1] == 0x86)
        {
            switch (s[2])
            {
                case 0x91: return 0x5E; /* ↑ */
                case 0x90: return 0x5F; /* ← */
                case 0x92: return 0xC6; /* → */
                case 0x93: return 0xFC; /* ↓ */
            }
        }
        if (s[1] == 0x99)
        {
            switch (s[2])
            {
                case 0xA4: return 0xE1; /* ♤ */
                case 0xA1: return 0xF3; /* ♡ */
                case 0xA7: return 0xF8; /* ♧ */
                case 0xA2: return 0xFA; /* ♢ */
            }
        }
    }

    /* Neznámý znak */
    return 0x20;
}

/**
 * @brief Konvertuje jeden znak ze Sharp MZ ASCII (JP varianta) do ASCII.
 *
 * JP varianta nemá malá písmena ani speciální evropské znaky - rozsah
 * nad 0x5D obsahuje pouze grafické symboly a katakanu, které nemají
 * ASCII ekvivalent. Proto se konvertují pouze MZ kódy 0x00-0x5D
 * (identické s ASCII), vše ostatní se nahrazuje mezerou.
 *
 * @param[in] c Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @return Odpovídající ASCII znak. Pro kódy > 0x5D vrací ' ' (mezeru).
 */
uint8_t sharpmz_jp_cnv_from(uint8_t c)
{
    if (c <= 0x5d)
        return (c);
    return (' ');
}

/**
 * @brief Konvertuje jeden znak ze standardního ASCII do Sharp MZ ASCII (JP varianta).
 *
 * JP varianta podporuje pouze rozsah 0x00-0x5D (identický s ASCII).
 * Znaky mimo tento rozsah se nahrazují mezerou.
 *
 * @param[in] c Vstupní znak v ASCII (0x00-0xFF).
 * @return Odpovídající Sharp MZ ASCII kód. Pro kódy > 0x5D vrací ' ' (mezeru).
 */
uint8_t sharpmz_jp_cnv_to(uint8_t c)
{
    if (c <= 0x5d)
        return (c);
    return (' ');
}

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
uint8_t sharpmz_jp_convert_to_ASCII(uint8_t c, int *converted, int *printable)
{
    if (c < 0x20)
    {
        *printable = 0;
        *converted = 1;
        return (' ');
    }

    *printable = 1;

    if (c <= 0x5d)
    {
        *converted = 1;
        return (c);
    }

    *converted = 0;
    return (' ');
}

/**
 * @brief Konvertuje znak ze Sharp MZ ASCII (JP varianta) do UTF-8.
 *
 * Nejprve se pokusí o konverzi přes sharpmz_jp_convert_to_ASCII(). Pokud ta
 * vrátí converted=1, použije se výsledek (ASCII znak je platný UTF-8).
 * Pokud ne, zkusí se mapování na japonské znaky:
 *
 * - 0x70-0x76: kanji dnů v týdnu (日月火水木金土)
 * - 0x77-0x7C: kanji (生年時分秒円)
 * - 0x7D: ¥ (U+00A5)
 * - 0x7E: £ (U+00A3)
 * - 0x86: ヲ, 0x87-0x8F: malé katakana (ァィゥェォャュョッ)
 * - 0x90: ー (chōon)
 * - 0x91-0xBD: katakana ア-ン
 * - 0xBE: ゛ (dakuten)
 * - 0xFF: π (U+03C0)
 *
 * U ostatních nekonvertovatelných znaků vrací mezeru s converted=0.
 *
 * @param[in]  c         Vstupní znak v Sharp MZ ASCII (0x00-0xFF).
 * @param[out] converted 1 pokud se podařila konverze, 0 pokud ne.
 * @param[out] printable 1 pokud MZ znak reprezentuje tisknutelný glyf, 0 pokud ne.
 * @return Ukazatel na null-terminated UTF-8 řetězec (pro ASCII vstup slot
 *         thread-local rotujícího bufferu - viz sharpmz_eu_convert_to_UTF8
 *         pro detaily životnosti).
 *
 * @pre converted nesmí být NULL.
 * @pre printable nesmí být NULL.
 *
 * @note Identifikace kanji 0x77-0x7C je orientační a může vyžadovat opravu.
 * @note Funkce je thread-safe (rotující buffer v thread-local storage).
 */
const char *sharpmz_jp_convert_to_UTF8(uint8_t c, int *converted, int *printable)
{
    /* Zkusíme ASCII konverzi */
    uint8_t ascii_ch = sharpmz_jp_convert_to_ASCII(c, converted, printable);
    if (*converted)
    {
        char *buf = sharpmz_utf8_next_slot();
        buf[0] = (char)ascii_ch;
        buf[1] = '\0';
        return buf;
    }

    /* Japonské znaky */
    *printable = 1;
    *converted = 1;

    switch (c)
    {
        /* Šipky */
        case 0x5E: return "\xE2\x86\x91"; /* ↑ - šipka nahoru */
        case 0x5F: return "\xE2\x86\x90"; /* ← - šipka vlevo */
        case 0x80: return "\xE2\x86\x93"; /* ↓ - šipka dolů */
        case 0xC0: return "\xE2\x86\x92"; /* → - šipka vpravo */

        /* Karetní symboly */
        case 0xE1: return "\xE2\x99\xA4"; /* ♤ - pika */
        case 0xF3: return "\xE2\x99\xA1"; /* ♡ - srdce */
        case 0xF8: return "\xE2\x99\xA7"; /* ♧ - kříž */
        case 0xFA: return "\xE2\x99\xA2"; /* ♢ - káry */

        /* Kanji - dny v týdnu */
        case 0x70: return "\xE6\x97\xA5"; /* 日 - neděle */
        case 0x71: return "\xE6\x9C\x88"; /* 月 - pondělí */
        case 0x72: return "\xE7\x81\xAB"; /* 火 - úterý */
        case 0x73: return "\xE6\xB0\xB4"; /* 水 - středa */
        case 0x74: return "\xE6\x9C\xA8"; /* 木 - čtvrtek */
        case 0x75: return "\xE9\x87\x91"; /* 金 - pátek */
        case 0x76: return "\xE5\x9C\x9F"; /* 土 - sobota */

        /* Kanji - další (identifikace orientační) */
        case 0x77: return "\xE7\x94\x9F"; /* 生 - život */
        case 0x78: return "\xE5\xB9\xB4"; /* 年 - rok */
        case 0x79: return "\xE6\x99\x82"; /* 時 - hodina */
        case 0x7A: return "\xE5\x88\x86"; /* 分 - minuta */
        case 0x7B: return "\xE7\xA7\x92"; /* 秒 - sekunda */
        case 0x7C: return "\xE5\x86\x86"; /* 円 - jen (měna) */

        /* Speciální znaky */
        case 0x7D: return "\xC2\xA5";     /* ¥ - jen */
        case 0x7E: return "\xC2\xA3";     /* £ - libra */

        /* ヲ */
        case 0x86: return "\xE3\x83\xB2"; /* ヲ */

        /* Malé katakana */
        case 0x87: return "\xE3\x82\xA1"; /* ァ */
        case 0x88: return "\xE3\x82\xA3"; /* ィ */
        case 0x89: return "\xE3\x82\xA5"; /* ゥ */
        case 0x8A: return "\xE3\x82\xA7"; /* ェ */
        case 0x8B: return "\xE3\x82\xA9"; /* ォ */
        case 0x8C: return "\xE3\x83\xA3"; /* ャ */
        case 0x8D: return "\xE3\x83\xA5"; /* ュ */
        case 0x8E: return "\xE3\x83\xA7"; /* ョ */
        case 0x8F: return "\xE3\x83\x83"; /* ッ */

        /* Chōon (prodloužení samohlásky) */
        case 0x90: return "\xE3\x83\xBC"; /* ー */

        /* Katakana ア-ン */
        case 0x91: return "\xE3\x82\xA2"; /* ア */
        case 0x92: return "\xE3\x82\xA4"; /* イ */
        case 0x93: return "\xE3\x82\xA6"; /* ウ */
        case 0x94: return "\xE3\x82\xA8"; /* エ */
        case 0x95: return "\xE3\x82\xAA"; /* オ */
        case 0x96: return "\xE3\x82\xAB"; /* カ */
        case 0x97: return "\xE3\x82\xAD"; /* キ */
        case 0x98: return "\xE3\x82\xAF"; /* ク */
        case 0x99: return "\xE3\x82\xB1"; /* ケ */
        case 0x9A: return "\xE3\x82\xB3"; /* コ */
        case 0x9B: return "\xE3\x82\xB5"; /* サ */
        case 0x9C: return "\xE3\x82\xB7"; /* シ */
        case 0x9D: return "\xE3\x82\xB9"; /* ス */
        case 0x9E: return "\xE3\x82\xBB"; /* セ */
        case 0x9F: return "\xE3\x82\xBD"; /* ソ */
        case 0xA0: return "\xE3\x82\xBF"; /* タ */
        case 0xA1: return "\xE3\x83\x81"; /* チ */
        case 0xA2: return "\xE3\x83\x84"; /* ツ */
        case 0xA3: return "\xE3\x83\x86"; /* テ */
        case 0xA4: return "\xE3\x83\x88"; /* ト */
        case 0xA5: return "\xE3\x83\x8A"; /* ナ */
        case 0xA6: return "\xE3\x83\x8B"; /* ニ */
        case 0xA7: return "\xE3\x83\x8C"; /* ヌ */
        case 0xA8: return "\xE3\x83\x8D"; /* ネ */
        case 0xA9: return "\xE3\x83\x8E"; /* ノ */
        case 0xAA: return "\xE3\x83\x8F"; /* ハ */
        case 0xAB: return "\xE3\x83\x92"; /* ヒ */
        case 0xAC: return "\xE3\x83\x95"; /* フ */
        case 0xAD: return "\xE3\x83\x98"; /* ヘ */
        case 0xAE: return "\xE3\x83\x9B"; /* ホ */
        case 0xAF: return "\xE3\x83\x9E"; /* マ */
        case 0xB0: return "\xE3\x83\x9F"; /* ミ */
        case 0xB1: return "\xE3\x83\xA0"; /* ム */
        case 0xB2: return "\xE3\x83\xA1"; /* メ */
        case 0xB3: return "\xE3\x83\xA2"; /* モ */
        case 0xB4: return "\xE3\x83\xA4"; /* ヤ */
        case 0xB5: return "\xE3\x83\xA6"; /* ユ */
        case 0xB6: return "\xE3\x83\xA8"; /* ヨ */
        case 0xB7: return "\xE3\x83\xA9"; /* ラ */
        case 0xB8: return "\xE3\x83\xAA"; /* リ */
        case 0xB9: return "\xE3\x83\xAB"; /* ル */
        case 0xBA: return "\xE3\x83\xAC"; /* レ */
        case 0xBB: return "\xE3\x83\xAD"; /* ロ */
        case 0xBC: return "\xE3\x83\xAF"; /* ワ */
        case 0xBD: return "\xE3\x83\xB3"; /* ン */

        /* Dakuten */
        case 0xBE: return "\xE3\x82\x9B"; /* ゛ - dakuten */

        /* Pí */
        case 0xFF: return "\xCF\x80";     /* π */
    }

    /* Neznámý znak - grafický symbol bez UTF-8 ekvivalentu */
    *converted = 0;
    return " ";
}

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
uint8_t sharpmz_jp_convert_UTF8_to(const char *c)
{
    const unsigned char *s = (const unsigned char *)c;

    /* Jednobajtový znak (ASCII) */
    if (s[0] < 0x80)
    {
        return sharpmz_jp_cnv_to(s[0]);
    }

    /* Dvoubajtové UTF-8 sekvence */
    if (s[0] == 0xC2 && s[1])
    {
        if (s[1] == 0xA5) return 0x7D; /* ¥ */
        if (s[1] == 0xA3) return 0x7E; /* £ */
    }

    if (s[0] == 0xCF && s[1] == 0x80)
    {
        return 0xFF; /* π */
    }

    /* Tříbajtové UTF-8 sekvence (0xE2 xx xx) - šipky a karetní symboly */
    if (s[0] == 0xE2 && s[1] && s[2])
    {
        if (s[1] == 0x86)
        {
            switch (s[2])
            {
                case 0x91: return 0x5E; /* ↑ */
                case 0x90: return 0x5F; /* ← */
                case 0x93: return 0x80; /* ↓ */
                case 0x92: return 0xC0; /* → */
            }
        }
        if (s[1] == 0x99)
        {
            switch (s[2])
            {
                case 0xA4: return 0xE1; /* ♤ */
                case 0xA1: return 0xF3; /* ♡ */
                case 0xA7: return 0xF8; /* ♧ */
                case 0xA2: return 0xFA; /* ♢ */
            }
        }
    }

    /* Tříbajtové UTF-8 sekvence (0xE3 xx xx) - katakana */
    if (s[0] == 0xE3 && s[1] && s[2])
    {
        if (s[1] == 0x82)
        {
            switch (s[2])
            {
                case 0x9B: return 0xBE; /* ゛ */
                case 0xA2: return 0x91; /* ア */
                case 0xA4: return 0x92; /* イ */
                case 0xA6: return 0x93; /* ウ */
                case 0xA8: return 0x94; /* エ */
                case 0xAA: return 0x95; /* オ */
                case 0xAB: return 0x96; /* カ */
                case 0xAD: return 0x97; /* キ */
                case 0xAF: return 0x98; /* ク */
                case 0xB1: return 0x99; /* ケ */
                case 0xB3: return 0x9A; /* コ */
                case 0xB5: return 0x9B; /* サ */
                case 0xB7: return 0x9C; /* シ */
                case 0xB9: return 0x9D; /* ス */
                case 0xBB: return 0x9E; /* セ */
                case 0xBD: return 0x9F; /* ソ */
                case 0xBF: return 0xA0; /* タ */
                /* Malé katakana */
                case 0xA1: return 0x87; /* ァ */
                case 0xA3: return 0x88; /* ィ */
                case 0xA5: return 0x89; /* ゥ */
                case 0xA7: return 0x8A; /* ェ */
                case 0xA9: return 0x8B; /* ォ */
            }
        }
        if (s[1] == 0x83)
        {
            switch (s[2])
            {
                case 0x81: return 0xA1; /* チ */
                case 0x83: return 0x8F; /* ッ */
                case 0x84: return 0xA2; /* ツ */
                case 0x86: return 0xA3; /* テ */
                case 0x88: return 0xA4; /* ト */
                case 0x8A: return 0xA5; /* ナ */
                case 0x8B: return 0xA6; /* ニ */
                case 0x8C: return 0xA7; /* ヌ */
                case 0x8D: return 0xA8; /* ネ */
                case 0x8E: return 0xA9; /* ノ */
                case 0x8F: return 0xAA; /* ハ */
                case 0x92: return 0xAB; /* ヒ */
                case 0x95: return 0xAC; /* フ */
                case 0x98: return 0xAD; /* ヘ */
                case 0x9B: return 0xAE; /* ホ */
                case 0x9E: return 0xAF; /* マ */
                case 0x9F: return 0xB0; /* ミ */
                case 0xA0: return 0xB1; /* ム */
                case 0xA1: return 0xB2; /* メ */
                case 0xA2: return 0xB3; /* モ */
                case 0xA3: return 0x8C; /* ャ */
                case 0xA4: return 0xB4; /* ヤ */
                case 0xA5: return 0x8D; /* ュ */
                case 0xA6: return 0xB5; /* ユ */
                case 0xA7: return 0x8E; /* ョ */
                case 0xA8: return 0xB6; /* ヨ */
                case 0xA9: return 0xB7; /* ラ */
                case 0xAA: return 0xB8; /* リ */
                case 0xAB: return 0xB9; /* ル */
                case 0xAC: return 0xBA; /* レ */
                case 0xAD: return 0xBB; /* ロ */
                case 0xAF: return 0xBC; /* ワ */
                case 0xB2: return 0x86; /* ヲ */
                case 0xB3: return 0xBD; /* ン */
                case 0xBC: return 0x90; /* ー */
            }
        }
    }

    /* Kanji */
    if (s[0] == 0xE5 && s[1] && s[2])
    {
        if (s[1] == 0x9C && s[2] == 0x9F) return 0x76; /* 土 */
        if (s[1] == 0xB9 && s[2] == 0xB4) return 0x78; /* 年 */
        if (s[1] == 0x88 && s[2] == 0x86) return 0x7A; /* 分 */
        if (s[1] == 0x86 && s[2] == 0x86) return 0x7C; /* 円 */
    }
    if (s[0] == 0xE6 && s[1] && s[2])
    {
        if (s[1] == 0x97 && s[2] == 0xA5) return 0x70; /* 日 */
        if (s[1] == 0x9C && s[2] == 0x88) return 0x71; /* 月 */
        if (s[1] == 0x9C && s[2] == 0xA8) return 0x74; /* 木 */
        if (s[1] == 0xB0 && s[2] == 0xB4) return 0x73; /* 水 */
        if (s[1] == 0x99 && s[2] == 0x82) return 0x79; /* 時 */
    }
    if (s[0] == 0xE7 && s[1] && s[2])
    {
        if (s[1] == 0x81 && s[2] == 0xAB) return 0x72; /* 火 */
        if (s[1] == 0x94 && s[2] == 0x9F) return 0x77; /* 生 */
        if (s[1] == 0xA7 && s[2] == 0x92) return 0x7B; /* 秒 */
    }
    if (s[0] == 0xE9 && s[1] == 0x87 && s[2] == 0x91)
    {
        return 0x75; /* 金 */
    }

    /* Neznámý znak */
    return 0x20;
}

/* ======================================================================== */
/* KOI8-CS implementace                                                     */
/* ======================================================================== */

/* Tabulka koi8cs_to_unicode[256] vygenerovaná z Cstools-3.44. */
#include "koi8cs_table.h"

/**
 * @brief Inverzní tabulka pro ASCII transliteraci KOI8-CS znaků.
 *
 * Index = KOI8-CS byte (0x00-0xFF). Hodnota = nejbližší ASCII písmeno
 * bez diakritiky (např. 'a' pro 0xC1=á, 'C' pro 0xE7=CH digraph).
 * 0 = nedefinováno / ne písmeno (pak vrátíme ' ').
 *
 * Pokrývá pouze KOI8-CS specifické bytes 0x80-0xFF. Pro 0x00-0x7F se
 * používá passthrough mimo tuto tabulku.
 */
static const uint8_t koi8cs_to_ascii_translit[256] = {
    /* 0x00-0x7F passthrough (nepoužito, tabulka jen pro 0x80-0xFF) */
    [0xC1] = 'a', [0xC3] = 'c', [0xC4] = 'd', [0xC5] = 'e',
    [0xC6] = 'r', [0xC7] = 'c', /* ch digraph -> 'c' (první písmeno) */
    [0xC8] = 'u', [0xC9] = 'i', [0xCA] = 'u', [0xCB] = 'l',
    [0xCC] = 'l', [0xCD] = 'o', [0xCE] = 'n', [0xCF] = 'o',
    [0xD0] = 'o', [0xD1] = 'a', [0xD2] = 'r', [0xD3] = 's',
    [0xD4] = 't', [0xD5] = 'u', [0xD7] = 'e', [0xD8] = 'a',
    [0xD9] = 'y', [0xDA] = 'z',
    [0xE1] = 'A', [0xE3] = 'C', [0xE4] = 'D', [0xE5] = 'E',
    [0xE6] = 'R', [0xE7] = 'C', /* CH digraph -> 'C' */
    [0xE8] = 'U', [0xE9] = 'I', [0xEA] = 'U', [0xEB] = 'L',
    [0xEC] = 'L', [0xED] = 'O', [0xEE] = 'N', [0xEF] = 'O',
    [0xF0] = 'O', [0xF1] = 'A', [0xF2] = 'R', [0xF3] = 'S',
    [0xF4] = 'T', [0xF5] = 'U', [0xF7] = 'E', [0xF8] = 'A',
    [0xF9] = 'Y', [0xFA] = 'Z',
    /* Modifier letters (caron, circumflex, acute) bez ASCII ekvivalentu:
     * 0xDC caron, 0xDE circumflex, 0xE0 acute, 0xFE degree -> 0 (mezera) */
};

/**
 * @brief Zakóduje Unicode codepoint do UTF-8 sekvence (rotující buffer).
 *
 * Podporuje rozsah U+0000 - U+FFFF (1-3 bajty UTF-8).
 *
 * @param[in] cp Unicode codepoint (0x0000-0xFFFF).
 * @return Ukazatel na null-terminated UTF-8 řetězec ve slotu thread-local
 *         rotujícího bufferu (viz sharpmz_utf8_next_slot()). Pro cp == 0
 *         vrací prázdný řetězec "".
 *
 * @note Funkce je thread-safe (rotující buffer v thread-local storage).
 */
static const char *koi8cs_encode_utf8(uint16_t cp)
{
    char *buf = sharpmz_utf8_next_slot();
    if (cp == 0) { buf[0] = 0; return buf; }
    if (cp < 0x80) {
        buf[0] = (char)cp;
        buf[1] = 0;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        buf[2] = 0;
    } else {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        buf[3] = 0;
    }
    return buf;
}

/**
 * @brief Konvertuje znak z KOI8-CS do ASCII s informací o úspěšnosti.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
uint8_t sharpmz_koi8cs_convert_to_ASCII(uint8_t c, int *converted, int *printable)
{
    if (c < 0x20) {
        *printable = 0;
        *converted = 1;
        return ' ';
    }
    if (c < 0x7F) {
        *printable = 1;
        *converted = 1;
        return c;
    }
    if (c == 0x7F) {
        *printable = 0;
        *converted = 1;
        return ' ';
    }
    /* 0x80-0xFF: KOI8-CS specifický rozsah */
    uint8_t a = koi8cs_to_ascii_translit[c];
    if (a != 0) {
        *printable = 1;
        *converted = 1;
        return a;
    }
    /* Nedefinováno nebo není ASCII písmeno (modifier letters, degree, ...) */
    *printable = 0;
    *converted = 0;
    return ' ';
}

/**
 * @brief Konvertuje jeden znak z KOI8-CS do ASCII (bez příznaků).
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
uint8_t sharpmz_koi8cs_cnv_from(uint8_t c)
{
    int conv, prn;
    return sharpmz_koi8cs_convert_to_ASCII(c, &conv, &prn);
}

/**
 * @brief Konvertuje jeden znak z ASCII do KOI8-CS.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
uint8_t sharpmz_koi8cs_cnv_to(uint8_t c)
{
    if (c < 0x80) return c;
    return ' ';
}

/**
 * @brief Konvertuje jeden znak z KOI8-CS do UTF-8.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
const char *sharpmz_koi8cs_convert_to_UTF8(uint8_t c, int *converted, int *printable)
{
    /* Řídicí znaky a ASCII printable - 1:1 do slotu rotujícího bufferu */
    if (c < 0x7F) {
        char *buf = sharpmz_utf8_next_slot();
        *printable = (c >= 0x20);
        *converted = 1;
        buf[0] = (char)c;
        buf[1] = '\0';
        return buf;
    }
    if (c == 0x7F) {
        *printable = 0;
        *converted = 1;
        return " ";
    }

    uint16_t cp = koi8cs_to_unicode[c];

    /* Digraf: "ch" pro 0xC7, "CH" pro 0xE7 (marker 0xFFFE) */
    if (cp == 0xFFFE) {
        *printable = 1;
        *converted = 1;
        if (c == 0xC7) return "ch";
        if (c == 0xE7) return "CH";
        return " ";
    }

    /* Nedefinovaný KOI8-CS byte */
    if (cp == 0x0000) {
        *printable = 0;
        *converted = 0;
        return " ";
    }

    *printable = 1;
    *converted = 1;
    return koi8cs_encode_utf8(cp);
}

/**
 * @brief Konvertuje UTF-8 znak na KOI8-CS byte.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
uint8_t sharpmz_koi8cs_convert_UTF8_to(const char *c)
{
    if (!c) return 0x20;
    const unsigned char *s = (const unsigned char *)c;

    /* ASCII (1 byte) - KOI8-CS sdílí 0x00-0x7F s ASCII.
     * Speciální handling digrafů: "ch" -> 0xC7, "CH" -> 0xE7. */
    if (s[0] < 0x80) {
        if (s[0] == 'c' && s[1] == 'h') return 0xC7;
        if (s[0] == 'C' && s[1] == 'H') return 0xE7;
        return s[0];
    }

    /* Vícebajtový UTF-8 - dekódujeme codepoint a hledáme v tabulce */
    uint16_t cp = 0;
    if (s[0] < 0xC0) return 0x20; /* nevalidní continuation */
    else if (s[0] < 0xE0) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    } else if (s[0] < 0xF0) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    } else {
        return 0x20; /* mimo BMP, KOI8-CS jen do U+FFFF */
    }

    /* Sekvenční lookup v tabulce (52 high-bit znaků, dostatečně rychlé) */
    for (int i = 0x80; i < 0x100; i++) {
        if (koi8cs_to_unicode[i] == cp && cp != 0 && cp != 0xFFFE) {
            return (uint8_t)i;
        }
    }
    return 0x20;
}

/* ======================================================================== */
/* Display kódy (CG-ROM index) - tabulka !ADCN                              */
/* ======================================================================== */

/**
 * @brief Převodní tabulka Sharp MZ ASCII -> display kód (EU varianta).
 *
 * Adapted from mzdisk mz_vcode.c (Michal Hucik, GPLv3). Zdrojová data:
 * ROM MZ-800, dolní monitor, tabulka !ADCN na adrese 0x0A92.
 *
 * Index = Sharp MZ ASCII (0x00-0xFF), hodnota = display kód (CG-ROM index).
 * Hodnota 0xF0 = "žádný znak" (nezobrazitelný).
 */
static const uint8_t sharpmz_adcn_eu[256] = {
    /* 0x00 */ 0xF0, 0xF0, 0xF0, 0xF3, 0xF0, 0xF5, 0xF0, 0xF0,
    /* 0x08 */ 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
    /* 0x10 */ 0xF0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xF0,
    /* 0x18 */ 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
    /* 0x20 */ 0x00, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    /* 0x28 */ 0x68, 0x69, 0x6B, 0x6A, 0x2F, 0x2A, 0x2E, 0x2D,
    /* 0x30 */ 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    /* 0x38 */ 0x28, 0x29, 0x4F, 0x2C, 0x51, 0x2B, 0x57, 0x49,
    /* 0x40 */ 0x55, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    /* 0x48 */ 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    /* 0x50 */ 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    /* 0x58 */ 0x18, 0x19, 0x1A, 0x52, 0x59, 0x54, 0x50, 0x45,
    /* 0x60 */ 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE,
    /* 0x68 */ 0xCF, 0xDF, 0xE7, 0xE8, 0xE5, 0xE9, 0xEC, 0xED,
    /* 0x70 */ 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    /* 0x78 */ 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xC0,
    /* 0x80 */ 0x80, 0xBD, 0x9D, 0xB1, 0xB5, 0xB9, 0xB4, 0x9E,
    /* 0x88 */ 0xB2, 0xB6, 0xBA, 0xBE, 0x9F, 0xB3, 0xB7, 0xBB,
    /* 0x90 */ 0xBF, 0xA3, 0x85, 0xA4, 0xA5, 0xA6, 0x94, 0x87,
    /* 0x98 */ 0x88, 0x9C, 0x82, 0x98, 0x84, 0x92, 0x90, 0x83,
    /* 0xA0 */ 0x91, 0x81, 0x9A, 0x97, 0x93, 0x95, 0x89, 0xA1,
    /* 0xA8 */ 0xAF, 0x8B, 0x86, 0x96, 0xA2, 0xAB, 0xAA, 0x8A,
    /* 0xB0 */ 0x8E, 0xB0, 0xAD, 0x8D, 0xA7, 0xA8, 0xA9, 0x8F,
    /* 0xB8 */ 0x8C, 0xAE, 0xAC, 0x9B, 0xA0, 0x99, 0xBC, 0xB8,
    /* 0xC0 */ 0x40, 0x3B, 0x3A, 0x70, 0x3C, 0x71, 0x5A, 0x3D,
    /* 0xC8 */ 0x43, 0x56, 0x3F, 0x1E, 0x4A, 0x1C, 0x5D, 0x3E,
    /* 0xD0 */ 0x5C, 0x1F, 0x5F, 0x5E, 0x37, 0x7B, 0x7F, 0x36,
    /* 0xD8 */ 0x7A, 0x7E, 0x33, 0x4B, 0x4C, 0x1D, 0x6C, 0x5B,
    /* 0xE0 */ 0x78, 0x41, 0x35, 0x34, 0x74, 0x30, 0x38, 0x75,
    /* 0xE8 */ 0x39, 0x4D, 0x6F, 0x6E, 0x32, 0x77, 0x76, 0x72,
    /* 0xF0 */ 0x73, 0x47, 0x7C, 0x53, 0x31, 0x4E, 0x6D, 0x48,
    /* 0xF8 */ 0x46, 0x7D, 0x44, 0x1B, 0x58, 0x79, 0x42, 0x60
};

/**
 * @brief Konvertuje Sharp MZ ASCII na display kód.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
uint8_t sharpmz_display_cnv_to(uint8_t ascii, sharpmz_charset_t charset)
{
    /* JP varianta - korekce na pozicích 0x6C a 0x6D (MZ-1500 CG-ROM rozdíl) */
    if (charset == SHARPMZ_CHARSET_DISPLAY_JP) {
        if (ascii == 0x6C) return 0xE9;
        if (ascii == 0x6D) return 0xEA;
    }
    return sharpmz_adcn_eu[ascii];
}

/**
 * @brief Konvertuje display kód na Sharp MZ ASCII (inverze !ADCN).
 *
 * Viz Doxygen v sharpmz_ascii.h. Implementace: sekvenční hledání prvního
 * ASCII bytu, jehož sharpmz_display_cnv_to() vrátí daný display_code.
 * Pro display kódy 0x80-0xFF (grafické symboly bez ASCII ekvivalentu)
 * vrací ' '.
 */
uint8_t sharpmz_display_cnv_from(uint8_t display_code, sharpmz_charset_t charset)
{
    for (int i = 0; i < 256; i++) {
        if (sharpmz_display_cnv_to((uint8_t)i, charset) == display_code) {
            return (uint8_t)i;
        }
    }
    return ' ';
}

/**
 * @brief Konvertuje display kód na UTF-8 řetězec.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
const char *sharpmz_display_to_utf8(uint8_t display_code, sharpmz_charset_t charset)
{
    uint8_t ascii = sharpmz_display_cnv_from(display_code, charset);
    /* Mapování ASCII -> UTF-8 přes EU/JP variantu (display_eu -> EU, display_jp -> JP) */
    sharpmz_charset_t ascii_set =
        (charset == SHARPMZ_CHARSET_DISPLAY_JP) ? SHARPMZ_CHARSET_JP
                                                : SHARPMZ_CHARSET_EU;
    int conv, prn;
    const char *result;
    if (ascii_set == SHARPMZ_CHARSET_JP) {
        result = sharpmz_jp_convert_to_UTF8(ascii, &conv, &prn);
    } else {
        result = sharpmz_eu_convert_to_UTF8(ascii, &conv, &prn);
    }
    if (!conv) return " ";
    return result;
}

/* ======================================================================== */
/* Charset-dispatchované wrappery a řetězcové funkce                        */
/* ======================================================================== */

/**
 * @brief Vrátí délku jednoho UTF-8 znaku v bajtech.
 *
 * Určí počet bajtů UTF-8 sekvence podle prvního bajtu.
 * Nevalidní lead bajty vrací 1 (přeskočení jednoho bajtu).
 *
 * @param[in] lead_byte První bajt UTF-8 sekvence.
 * @return Délka znaku v bajtech (1-4). Pro nevalidní bajty vrací 1.
 */
static int sharpmz_utf8_char_len(unsigned char lead_byte)
{
    if (lead_byte < 0x80) return 1;
    if (lead_byte < 0xC0) return 1; /* nevalidní continuation bajt */
    if (lead_byte < 0xE0) return 2;
    if (lead_byte < 0xF0) return 3;
    if (lead_byte < 0xF8) return 4;
    return 1; /* nevalidní */
}

/**
 * @brief Konvertuje jeden znak (libovolný charset) na UTF-8 řetězec.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
const char *sharpmz_to_utf8(uint8_t code, sharpmz_charset_t charset)
{
    int converted, printable;
    const char *result;

    switch (charset) {
        case SHARPMZ_CHARSET_JP:
            result = sharpmz_jp_convert_to_UTF8(code, &converted, &printable);
            break;
        case SHARPMZ_CHARSET_KOI8CS:
            result = sharpmz_koi8cs_convert_to_UTF8(code, &converted, &printable);
            break;
        case SHARPMZ_CHARSET_DISPLAY_EU:
        case SHARPMZ_CHARSET_DISPLAY_JP:
            return sharpmz_display_to_utf8(code, charset);
        case SHARPMZ_CHARSET_EU:
        default:
            result = sharpmz_eu_convert_to_UTF8(code, &converted, &printable);
            break;
    }

    if (!converted) return " ";
    return result;
}

/**
 * @brief Konvertuje jeden UTF-8 znak na byte v daném kódování.
 *
 * Viz Doxygen v sharpmz_ascii.h.
 */
int sharpmz_from_utf8(const char *utf8, sharpmz_charset_t charset)
{
    if (!utf8) return -1;

    const unsigned char *s = (const unsigned char *)utf8;

    /* ASCII mezera (U+0020) se legitimně mapuje na byte 0x20 */
    if (s[0] == 0x20) return 0x20;

    uint8_t result;
    switch (charset) {
        case SHARPMZ_CHARSET_JP:
            result = sharpmz_jp_convert_UTF8_to(utf8);
            break;
        case SHARPMZ_CHARSET_KOI8CS:
            result = sharpmz_koi8cs_convert_UTF8_to(utf8);
            break;
        case SHARPMZ_CHARSET_DISPLAY_EU:
            /* UTF-8 -> Sharp MZ ASCII (EU) -> display kód */
            result = sharpmz_eu_convert_UTF8_to(utf8);
            if (result == 0x20) return -1;
            return sharpmz_display_cnv_to(result, SHARPMZ_CHARSET_DISPLAY_EU);
        case SHARPMZ_CHARSET_DISPLAY_JP:
            result = sharpmz_jp_convert_UTF8_to(utf8);
            if (result == 0x20) return -1;
            return sharpmz_display_cnv_to(result, SHARPMZ_CHARSET_DISPLAY_JP);
        case SHARPMZ_CHARSET_EU:
        default:
            result = sharpmz_eu_convert_UTF8_to(utf8);
            break;
    }

    /* Konverzní funkce vrací 0x20 jako fallback pro neznámé znaky.
     * Protože skutečnou mezeru jsme zpracovali výše, 0x20 zde znamená
     * "nenalezeno". */
    if (result == 0x20) return -1;
    return result;
}

/**
 * @brief Konvertuje řetězec Sharp MZ znaků na UTF-8.
 *
 * Iteruje přes zdrojový řetězec Sharp MZ kódů a zapisuje odpovídající
 * UTF-8 znaky do cílového bufferu. Pro znaky bez Unicode ekvivalentu
 * se zapisuje mezera (" "). Výstup je vždy null-terminated (pokud dst_size > 0).
 *
 * @param[in]  src       Zdrojový řetězec Sharp MZ kódů.
 * @param[in]  src_len   Délka zdrojového řetězce v bajtech.
 * @param[out] dst       Cílový buffer pro UTF-8 výstup.
 * @param[in]  dst_size  Velikost cílového bufferu v bajtech.
 * @param[in]  charset   Varianta znakové sady (EU nebo JP).
 * @return Počet zapsaných bajtů (bez null terminátoru) při úspěchu,
 *         -1 pokud dst nebo src je NULL, nebo pokud dst_size je 0.
 *
 * @pre src nesmí být NULL.
 * @pre dst nesmí být NULL.
 * @pre dst_size musí být > 0.
 * @post dst je vždy null-terminated (pokud dst_size > 0).
 */
int sharpmz_str_to_utf8(const uint8_t *src, size_t src_len,
                         char *dst, size_t dst_size,
                         sharpmz_charset_t charset)
{
    if (!src || !dst || dst_size == 0) return -1;

    size_t written = 0;

    for (size_t i = 0; i < src_len; i++)
    {
        const char *utf8_char = sharpmz_to_utf8(src[i], charset);
        size_t char_len = strlen(utf8_char);

        /* kontrola, zda se znak vejde do bufferu (+ null terminátor) */
        if (written + char_len >= dst_size) break;

        memcpy(dst + written, utf8_char, char_len);
        written += char_len;
    }

    dst[written] = '\0';
    return (int)written;
}

/**
 * @brief Konvertuje UTF-8 řetězec na řetězec Sharp MZ kódů.
 *
 * Parsuje UTF-8 vstup a pro každý Unicode znak hledá odpovídající
 * Sharp MZ kód. Neznámé znaky se nahrazují mezerou (0x20).
 * Výstup je vždy null-terminated (pokud dst_size > 0).
 *
 * @param[in]  src       Zdrojový UTF-8 řetězec (null-terminated).
 * @param[out] dst       Cílový buffer pro Sharp MZ kódy.
 * @param[in]  dst_size  Velikost cílového bufferu v bajtech.
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
int sharpmz_str_from_utf8(const char *src,
                           uint8_t *dst, size_t dst_size,
                           sharpmz_charset_t charset)
{
    if (!src || !dst || dst_size == 0) return -1;

    size_t written = 0;

    while (*src && written < dst_size - 1)
    {
        int mz_code = sharpmz_from_utf8(src, charset);
        dst[written++] = (mz_code >= 0) ? (uint8_t)mz_code : 0x20;

        /* posun na další UTF-8 znak */
        int char_len = sharpmz_utf8_char_len((unsigned char)*src);
        src += char_len;
    }

    dst[written] = '\0';
    return (int)written;
}
