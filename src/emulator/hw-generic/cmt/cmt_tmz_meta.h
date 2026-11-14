/**
 * @file   cmt_tmz_meta.h
 * @brief  Nástroje nad vloženou páskou Virtual CMT: informace o pásce, kontrola pásky, převod profilu.
 *
 * Tři nezávislé části nad TMZ v paměti (cmttmz_get_file()):
 *
 * - **Informace o pásce**: čtení a zápis bloku TZX Archive Info
 *   (0x32) a bloku TMZ Machine Info (0x43) na začátku pásky. "Začátek
 *   pásky" je nejdelší úsek informačních bloků od bloku 0 (0x30, 0x31,
 *   0x32, 0x33, 0x35, 0x43, 0x5A); platí první čitelný blok 0x32 a první
 *   blok 0x43 aspoň 4 + 6 B v tomto úseku. Knihovna TapeMZ parser bloku
 *   0x32 nemá (jen tzx_block_create_archive_info()), modul má vlastní.
 *   Kódování textů 0x32: specifikace TZX v1.20 (repozitář TapeMZ,
 *   docs/tzx/tzx-specification-v1.20.md, obecná pravidla) říká "ASCII text
 *   používá kódování ISO 8859-1 (Latin 1)" a "Zalomení řádku v textu: ASCII
 *   kód 13 (0x0D)"; revize 1.10 povoluje u 0x32 víc řádků. Modul proto čte
 *   bajty jako ISO 8859-1 (0Dh = '\n', cizí zalomení 0Ah a 0Dh 0Ah také
 *   jeden '\n', 20h až 7Eh a A0h až FFh jako znaky U+0020 až U+00FF
 *   v UTF-8, ostatní bajty '?') a zapisuje zpět (UTF-8 -> ISO 8859-1,
 *   '\n' a "\r\n" = 0Dh, znak mimo ISO 8859-1 nebo řídicí znak = '?'
 *   s varováním). Položku, kterou formulář nezměnil, zapíše zpět bajtově
 *   beze změny (i cizí zalomení a bajty 80h až 9Fh); upravená položka, jejíž
 *   bajty formulář ukázal jako '?', se zapíše s 3Fh a s varováním.
 * - **Kontrola pásky**: jen čtení seznamu bloků, nezávisle na
 *   stavu playeru; výsledek je seznam nálezů s anglickým textem.
 * - **Převod profilu**: blok 0x46 se souborem MZF se přestaví
 *   jiným profilem zapisovače z registru TapeMZ (tmz_profile_*); obsah MZF
 *   (hlavička 128 B a tělo) zůstane bajtově stejný.
 *
 * Změny pásky jdou výhradně přes úpravy pásky v cmt_tmz.c
 * (cmttmz_insert_blocks(), cmttmz_remove_blocks(), cmttmz_replace_block()),
 * takže čísla bloků, kurzor, stav CMT hacku, příznaky neuloženo, Modified
 * a čítače udržuje jediné místo (cmttmz_edit_apply()). Chyby změn jdou
 * kanálem cmttmz_get_edit_error(), varování kanálem cmttmz_get_edit_warning().
 *
 * Vlákna: vše ve vlákně emulátoru; funkce, které pásku mění, pod
 * device_state_lock() (obaly v cmt.c si ho vezmou), čtení i bez něj ve
 * vlákně emulátoru, nebo z UI pod device_state_lock().
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

#ifndef CMT_TMZ_META_H
#define CMT_TMZ_META_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#include "libs/cmtspeed/cmtspeed.h"


    /* ------------------------------------------------------------------ */
    /*  Informace o pásce                                                 */
    /* ------------------------------------------------------------------ */

    /**
     * @brief Nejvíc znaků (bajtů ISO 8859-1) jednoho textu Archive Info.
     *
     * Položka TEXT bloku 0x32 nese délku v jednom bajtu (specifikace TZX).
     * Mez platí po převodu textu formuláře na ISO 8859-1.
     */
#define CMTTMZ_TAPE_INFO_MAX_CHARS 255

    /**
     * @brief Velikost bufferu jednoho textu Archive Info (UTF-8) včetně ukončovací nuly.
     *
     * Znak ISO 8859-1 zabere v UTF-8 nejvýš 2 B, takže 255 znaků se vejde do
     * 510 B + nula. Delší text UTF-8, který se do mezí po převodu vejde
     * (např. "\r\n" = jeden znak), se vejde také.
     */
#define CMTTMZ_TAPE_INFO_TEXT_SIZE 512

    /** @brief Počet textů Archive Info, které formulář zná (st_CMTTMZ_TAPE_INFO). */
#define CMTTMZ_TAPE_INFO_FIELDS 10


    /**
     * @brief Informace o pásce: texty bloku Archive Info (0x32) a stroj z bloku Machine Info (0x43).
     *
     * Texty odpovídají ID položek TZX 00h až 08h a FFh v tomto pořadí (title
     * 00h, publisher 01h, author 02h, year 03h, language 04h, type 05h, price
     * 06h, protection 07h, origin 08h, comment FFh). Prázdný text = položka
     * chybí. Z bloku 0x32 se do formuláře bere první položka každého
     * známého ID; ostatní položky (neznámé ID, další položka téhož ID) jsou
     * "cizí", formulář je neukazuje a cmttmz_tape_info_set() je zachová
     * bajtově v původním pořadí (počet nese other_entries); jen vymazání
     * textu odstraní všechny položky jeho ID.
     *
     * @invariant Každý text je ukončený nulou a v UTF-8; po
     *            cmttmz_tape_info_get() má nejvýš CMTTMZ_TAPE_INFO_MAX_CHARS
     *            znaků U+000A a U+0020 až U+00FF ('\n' = 0Dh v bloku).
     * @invariant machine je -1 nebo hodnota bajtu stroje bloku 0x43 (po
     *            cmttmz_tape_info_get() i hodnota mimo en_TMZ_MACHINE, je-li
     *            taková v souboru; cmttmz_tape_info_set() přijme jen -1,
     *            en_TMZ_MACHINE nebo tutéž hodnotu beze změny).
     * @invariant tape_serial je cmttmz_get_tape_serial() pásky, ze které
     *            informace četl cmttmz_tape_info_get(); zápis s jinou
     *            hodnotou se odmítne (zastaralý formulář).
     */
    typedef struct st_CMTTMZ_TAPE_INFO {
        char title[CMTTMZ_TAPE_INFO_TEXT_SIZE];      /**< 00h plný název */
        char publisher[CMTTMZ_TAPE_INFO_TEXT_SIZE];  /**< 01h vydavatel */
        char author[CMTTMZ_TAPE_INFO_TEXT_SIZE];     /**< 02h autor(i) */
        char year[CMTTMZ_TAPE_INFO_TEXT_SIZE];       /**< 03h rok vydání */
        char language[CMTTMZ_TAPE_INFO_TEXT_SIZE];   /**< 04h jazyk */
        char type[CMTTMZ_TAPE_INFO_TEXT_SIZE];       /**< 05h typ programu */
        char price[CMTTMZ_TAPE_INFO_TEXT_SIZE];      /**< 06h cena */
        char protection[CMTTMZ_TAPE_INFO_TEXT_SIZE]; /**< 07h ochrana / zavaděč */
        char origin[CMTTMZ_TAPE_INFO_TEXT_SIZE];     /**< 08h původ */
        char comment[CMTTMZ_TAPE_INFO_TEXT_SIZE];    /**< FFh komentář (i víc řádků oddělených '\n') */
        int machine;                   /**< -1 = neurčeno (bez bloku 0x43), jinak en_TMZ_MACHINE z bloku 0x43 */
        int storable;                  /**< 1 = formát souboru pásky metadata unese (.tmz, .tzx, páska bez souboru), 0 = ne (.mzf, .m12, .mzt, ...); jen čtení */
        uint32_t other_entries;        /**< počet cizích položek bloku 0x32, které set zachová; jen čtení */
        uint32_t other_archive_blocks; /**< počet dalších bloků 0x32 na pásce (mimo začátek pásky, nečitelné, druhý v úvodním úseku); set je nemění; jen čtení */
        int other_archive_block;       /**< číslo prvního takového bloku, -1 = žádný; jen čtení */
        unsigned tape_serial;          /**< čítač výměn pásky při čtení (cmttmz_get_tape_serial()); set ho ověří */
    } st_CMTTMZ_TAPE_INFO;


    /**
     * @brief Klíč textu informací o pásce (pro MCP a výpisy).
     * @param field Index 0 .. CMTTMZ_TAPE_INFO_FIELDS - 1 (pořadí položek st_CMTTMZ_TAPE_INFO).
     * @return "title", "publisher", "author", "year", "language", "type",
     *         "price", "protection", "origin", "comment"; NULL mimo rozsah.
     */
    extern const char* cmttmz_tape_info_key ( uint32_t field );

    /**
     * @brief Buffer textu informací o pásce podle indexu.
     * @param info Informace (NULL = NULL).
     * @param field Index 0 .. CMTTMZ_TAPE_INFO_FIELDS - 1.
     * @return Ukazatel do @p info (CMTTMZ_TAPE_INFO_TEXT_SIZE bajtů); NULL mimo rozsah.
     */
    extern char* cmttmz_tape_info_text ( st_CMTTMZ_TAPE_INFO *info, uint32_t field );

    /**
     * @brief Text informací o pásce podle indexu, jen pro čtení.
     * @param info Informace (NULL = NULL).
     * @param field Index 0 .. CMTTMZ_TAPE_INFO_FIELDS - 1.
     * @return Ukazatel do @p info (ukončený nulou); NULL mimo rozsah.
     */
    extern const char* cmttmz_tape_info_text_const ( const st_CMTTMZ_TAPE_INFO *info, uint32_t field );

    /**
     * @brief Přečte informace o vložené pásce (blok 0x32 a 0x43 na začátku pásky).
     *
     * Bez bloku 0x32 jsou texty prázdné, bez bloku 0x43 je machine -1.
     * Blok 0x32, jehož položky přesahují blok (nebo počet položek přesahuje
     * data), a blok 0x43 kratší než 4 + 6 B se neberou (zůstanou na pásce,
     * formulář je nevidí; nečitelný 0x32 se počítá do other_archive_blocks).
     * Blok 0x43 se čte tmz_block_parse_mz_machine_info() nad kopií dat
     * (funkce mění blok na místě). storable podle přípony cesty pásky,
     * tape_serial podle cmttmz_get_tape_serial(). Nic nemění, nic nevypisuje.
     *
     * @param[out] out Informace (při chybě vynulované, machine -1, other_archive_block -1).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky, s NULL nebo při
     *         selhání alokace kopie bloku 0x43.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_tape_info_get ( st_CMTTMZ_TAPE_INFO *out );

    /**
     * @brief Zapíše informace o pásce: blok 0x32 a 0x43 na začátku pásky.
     *
     * Postup:
     * - Zastaralý formulář: in->tape_serial různé od cmttmz_get_tape_serial()
     *   = chyba "The tape changed since the tape information was read; read it
     *   again", páska beze změny (formulář z jiné pásky nebo z doby před jinou
     *   úpravou pásky).
     * - Archive Info: z dosavadního bloku 0x32 se zachovají cizí položky
     *   (st_CMTTMZ_TAPE_INFO::other_entries) v původním pořadí; změněný text
     *   nahradí svou položku na jejím místě; vymazaný text odstraní všechny
     *   položky svého ID (i cizí duplicity); nový se vloží před první položku
     *   s vyšším ID (FFh je nejvyšší). Text, který se proti
     *   cmttmz_tape_info_get() nezměnil, se zapíše původními bajty. Změněný
     *   text se převede na ISO 8859-1 ('\n' a "\r\n" = 0Dh; řídicí znak nebo
     *   znak mimo ISO 8859-1 = '?', varování; varování i u změněného textu,
     *   jehož dosavadní položka měla bajty ukázané jako '?', např. 80h až
     *   9Fh). Bez bloku a s neprázdnou
     *   položkou se blok vloží jako blok 0; když nezůstane žádná položka,
     *   blok se odstraní. Další bloky 0x32 (other_archive_blocks) se nemění.
     * - Machine Info: -1 blok 0x43 odstraní; jiná hodnota změní jen bajt
     *   stroje (takt CPU, verze ROM a případné další bajty bloku zůstanou);
     *   bez bloku se vloží nový hned za blok 0x32 (bez něj jako blok 0) s taktem
     *   3 546 895 Hz (MZCMT_FM_DEFAULT_TIMEBASE, výchozí takt playeru, takže
     *   časování bloků za ním se nemění) a verzí ROM 0 (neznámá). Blok 0x43
     *   na pásce ZXTape! (.tzx) přepne hlavičku na TMZ 1.1 (úpravy pásky);
     *   uložení s příponou .tzx pak varuje, že soubor je ve formátu TMZ.
     * - Změní-li se tím takt CPU, který platí pro některý blok (odstranění
     *   bloku 0x43 s jiným taktem), varování "The tape clock setting now applies
     *   to different blocks; check their speed." (věta importu).
     * - Bez rozdílu proti pásce: *changed 0, páska se vůbec nedotkne
     *   (čítače, Modified, kurzor, otevřený blok beze změny).
     *
     * Páska otevřená z MZF, M12 nebo MZT (storable 0): zápis do pásky v paměti
     * projde, ale do vlastního souboru ji pak uložit nejde (MZF unese jediný
     * blok, MZT jen bloky s MZF) - uložit jde jen jako TMZ nebo TZX.
     *
     * Změna jde přes úpravy pásky (cmttmz_replace_block(),
     * cmttmz_remove_blocks(), cmttmz_insert_blocks()): změněné a vložené bloky
     * jsou neuložené, Modified 1, čítač výměn pásky vzroste (formulář je pak
     * zastaralý - číst znovu), otevřený blok se otevře znovu s pozicí 0, stav
     * CMT hacku se smaže. Všechny nové bloky se sestaví předem; změna
     * potřebuje nejvýš dvě úpravy (např. odstranění bloku 0x32 a vložení
     * bloku 0x43, nebo nahrazení bloku 0x32 a odstranění bloku 0x43).
     *
     * Zápis ze dvou úprav NENÍ "všechno, nebo nic": selže-li až druhá
     * úprava (jen selhání alokace), první
     * zůstane provedená a nevrací se. Volající pak vidí: návrat
     * EXIT_FAILURE, text chyby druhé úpravy v cmttmz_get_edit_error()
     * ("Not enough memory"), *changed 1, páska je konzistentní (Modified 1,
     * čítač výměn pásky vzrostl, takže formulář je zastaralý) a nese jen
     * část zápisu; varování o provedené části (znaky, takt) jsou
     * v cmttmz_get_edit_warning(). Okno Tape information ukáže chybu
     * a výzvu k Reload (nové čtení ukáže skutečný stav pásky), MCP
     * `cmt_tape_info` vrátí chybu.
     *
     * @param in Nový obsah (machine, texty, tape_serial; ostatní pole se ignorují).
     * @param[out] changed 1 = páska se změnila, 0 = beze změny (smí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky ("No tape inserted"),
     *         s NULL ("No tape information"), se zastaralým formulářem (viz
     *         výše), s textem bez ukončovací nuly v bufferu nebo delším než 255
     *         znaků po převodu ("The <klíč> text is longer than 255
     *         characters"), s neplatným strojem ("Invalid machine N"), s blokem
     *         0x32 nad 65535 B ("Archive info is too large"), s víc než 255
     *         položkami ("Archive info has too many entries") nebo při selhání
     *         alokace ("Not enough memory", chyby úprav pásky) - text
     *         v cmttmz_get_edit_error(); při chybě validace i při selhání
     *         první úpravy páska beze změny, při selhání druhé úpravy viz
     *         výše (první zůstane, *changed 1).
     * @post cmttmz_get_edit_warning() nese varování tohoto volání ("" bez varování).
     * @pre Vlákno emulátoru pod device_state_lock(), transport ve STOP, neběží
     *      nahrávání ani záznam vstupů (ověřuje obal cmt_tape_info_set()).
     */
    extern int cmttmz_tape_info_set ( const st_CMTTMZ_TAPE_INFO *in, int *changed );


    /* ------------------------------------------------------------------ */
    /*  Kontrola pásky                                                    */
    /* ------------------------------------------------------------------ */

    /** @brief Velikost textu nálezu kontroly pásky včetně ukončovací nuly. */
#define CMTTMZ_CHECK_TEXT_SIZE 160

    /**
     * @brief Nejvíc nálezů kontroly pásky.
     *
     * Další nálezy se nesbírají; za seřazené nálezy přibude jeden informační
     * nález s block_id -1 "More problems were found; only 500 of them are
     * listed.". Mez bere nálezy v pořadí vzniku (nálezy neuzavřených smyček
     * a skupin vznikají až na konci průchodu), ne prvních 500 podle čísla
     * bloku - text proto neříká "the first".
     */
#define CMTTMZ_CHECK_MAX_ITEMS 500

    /**
     * @brief Závažnost nálezu kontroly pásky (st_CMTTMZ_CHECK_ITEM::severity).
     */
    typedef enum en_CMTTMZ_CHECK_SEVERITY {
        CMTTMZ_CHECK_INFO = 0,    /**< informace: přehrávání neovlivní (např. profil, který emulátor nezná) */
        CMTTMZ_CHECK_WARNING = 1, /**< varování: struktura pásky nebo bloku neodpovídá formátu, přehrávání nebo čtení může selhat */
    } en_CMTTMZ_CHECK_SEVERITY;

    /**
     * @brief Jeden nález kontroly pásky.
     *
     * @invariant text je ukončený nulou, anglicky, bez čísla bloku (to nese block_id).
     * @invariant block_id >= 0, jen souhrnný nález o mezi CMTTMZ_CHECK_MAX_ITEMS má -1.
     */
    typedef struct st_CMTTMZ_CHECK_ITEM {
        int block_id;                       /**< číslo bloku, kterého se nález týká (-1 = souhrn) */
        int severity;                       /**< en_CMTTMZ_CHECK_SEVERITY */
        char text[CMTTMZ_CHECK_TEXT_SIZE];  /**< popis anglicky */
    } st_CMTTMZ_CHECK_ITEM;

    /**
     * @brief Zkontroluje vloženou pásku (jen čtení).
     *
     * Prochází seznam bloků od 0 do konce (ne průchod playeru), nezávisle na
     * stavu přehrávání. Nálezy seřazené stabilně podle bloku:
     * - smyčky: "Loop start without a loop end", "Loop end without a loop
     *   start", "Loop inside another loop (not allowed in TZX)";
     * - skupiny: "Group start without a group end", "Group end without a group
     *   start", "Group inside another group (not allowed in TZX)";
     * - skok: "Jump to block N outside the tape", "Jump to itself (endless
     *   loop)", "Jump block is damaged";
     * - volání: "Call to block N outside the tape" (každý cíl), "Call sequence
     *   block is damaged", "Call sequence without a return block on the
     *   tape", "Return without a call sequence on the tape" (párování jen
     *   statické - existence bloků 0x26 a 0x27 na pásce);
     * - "Unknown block ID 0xNN" (tmz_block_id_name() vrací pro neznámé ID
     *   "Unknown", smluvně podle tzx.h a tmz.h);
     * - bloky MZ s MZF (0x40, 0x41, 0x46): "MZ file block cannot be parsed:
     *   <důvod>" (0x46: tmz_mzfile_parse(); 0x40, 0x41: tmz_block_to_mzf() nad
     *   kopií - poškozený blok 0x40 / 0x41 vypíše knihovna na stderr), "File
     *   name in the header has no terminator 0Dh" (mzf_header_validate()),
     *   "Unknown file type 0xNN in the header" (typ mimo 01h až 05h), "Body
     *   length N B differs from the header size M B" (u typu 03h a 04h, BSD
     *   a BRD, se délka nekontroluje: tělo nese kusy po 258 B; blok 0x46 jinou
     *   délku dovoluje, spec TMZ v2 kap. 4.1, profily ROM ji ale čtou z fsize);
     * - informace: "Profile <ID> is not known to this emulator" (0x46).
     * Součet těla se nekontroluje. Nejvýš CMTTMZ_CHECK_MAX_ITEMS nálezů.
     *
     * @param[out] items Pole nálezů (g_malloc, uvolní volající g_free()); NULL bez nálezu.
     * @param[out] count Počet nálezů.
     * @param[out] blocks_checked Počet zkontrolovaných bloků (smí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky nebo s NULL (výstupy NULL / 0).
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_tape_check ( st_CMTTMZ_CHECK_ITEM **items, uint32_t *count, uint32_t *blocks_checked );


    /* ------------------------------------------------------------------ */
    /*  Převod profilu                                                    */
    /* ------------------------------------------------------------------ */

    /**
     * @brief Počet profilů zapisovače, které převod umí vyrobit (nabídka pro UI a MCP).
     *
     * Profily registru TapeMZ (tmz_profile_get()), které z obyčejného MZF
     * sestaví blok výchozími parametry - zjištěno zkušební stavbou
     * (tmz_profile_build_block()) při prvním volání, ne seznamem jmen.
     * V tmz 1.14.0 tak vypadne BASIC-1Z016 (tělo musí být z kusů po 258 B
     * a blok BSD převoditelný není, fsize se nerovná délce těla).
     *
     * @return Počet.
     * @note Zkušební stavba proběhne jednou (g_once), funkce je bezpečná z libovolného vlákna.
     */
    extern uint32_t cmttmz_profile_count ( void );

    /**
     * @brief Profil zapisovače z nabídky podle pořadí.
     *
     * accepts_speed = 1 u profilů, které zkušební stavba s rychlostí
     * CMTSPEED_2_1 přijme (tmz 1.14.0: TC-1.21, TAPEMZ-V1-LEGACY,
     * MZ800EMU-CMT); ostatní rychlost nepřijímají (knihovna je sestaví jen ve
     * výchozí rychlosti profilu; ROM-800-* přijme navíc jen 1:1 = výchozí).
     *
     * @param i Index 0 .. cmttmz_profile_count() - 1.
     * @param[out] id ID profilu (statický řetězec knihovny; smí být NULL).
     * @param[out] description Popis anglicky (statický; smí být NULL).
     * @param[out] accepts_speed 1 = převod přijme rychlost (smí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE mimo rozsah (výstupy se nemění).
     */
    extern int cmttmz_profile_get ( uint32_t i, const char **id, const char **description, int *accepts_speed );

    /**
     * @brief Jde blok vložené pásky převést na jiný profil?
     *
     * Blok 0x46, který tmz_mzfile_parse() přečte, s tělem nejvýš 65535 B
     * a fsize rovným délce těla - tytéž podmínky, za kterých tmz_block_to_mzf()
     * vyrobí MZF (tmz_blocks.c, větev 0x46), jen bez výpisu knihovny na stderr
     * a bez kopie těla.
     *
     * @param block_id Číslo bloku.
     * @return 1 = ano; 0 = ne, mimo rozsah nebo bez pásky.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_block_profile_convertible ( int block_id );

    /**
     * @brief ID profilu bloku 0x46 vložené pásky.
     * @param block_id Číslo bloku.
     * @param[out] id Buffer na ID (ukončený nulou, oříznutý). @param size Velikost bufferu.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, mimo rozsah, u jiného bloku
     *         než 0x46, u bloku, který tmz_mzfile_parse() nepřečte, nebo s NULL / 0.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_block_profile ( int block_id, char *id, size_t size );

    /**
     * @brief Převede blok 0x46 vložené pásky na profil zapisovače.
     *
     * MZF bloku (hlavička a tělo) se sestaví znovu vybraným profilem; hlavička
     * i tělo jdou do nového bloku bajtově beze změny, součty spočítá profil.
     *
     * **Tentýž profil** jako blok: se CMTSPEED_NONE se nic nemění (úspěch,
     * páska nedotčená). S rychlostí (jen profil, který ji přijímá) se blok
     * sestaví ze všech PROFILE_ARGS bloku a změní se jen rychlost (poměr
     * TurboCopy a vlastní pulzy se zruší, protože rychlost určují také).
     *
     * **Jiný profil**: parametry jsou výchozí cílového profilu (TAPEMZ-V1-LEGACY
     * navíc format=normal - zjištěno zkušební stavbou), přenese se pauza za
     * blokem, stroj a každé další nastavení dosavadního profilu (rychlost,
     * poměr TurboCopy, vlastní pulzy, předzavaděč, délka tónu, nastavení
     * TMZ 1.0, rychlost v Bd), které se liší od výchozího dosavadního profilu
     * a které cílový profil přijme (zkušební stavba). Rychlost zadaná
     * volajícím má přednost (rychlost, poměr a vlastní pulzy se pak
     * nepřenášejí). Co se nepřeneslo, vrátí varování v cmttmz_get_edit_warning():
     * "Settings of profile X not carried over: preloader setting, baud rate".
     * Nečitelné PROFILE_ARGS: pauza ze záznamu rychlosti, jinak 1000 ms,
     * ostatní výchozí cílového profilu, žádná skupina se nepřenáší ani nehlásí
     * a varování "The profile settings of the block could
     * not be read; profile defaults were used".
     *
     * Části bloku, které PROFILE_ARGS nepopisuje (vnucené součty ck_header /
     * ck_body, další TLV), přestavba spočítá znovu. Pozná se zkušební stavbou
     * dosavadním profilem s parametry bloku (ne u MZ800EMU-CMT, jehož blok
     * podle PROFILE_ARGS nevzniká); liší-li se od bloku, varování "Parts of
     * the block that its profile settings do not describe (for example
     * checksums) were rebuilt".
     *
     * MZ800EMU-CMT se staví jako záznam (cmttmz_build_mzf_block(), stroj
     * nenese): bez rychlosti = výchozí rychlost decku (g_cmt.mz_cmtspeed),
     * záznam rychlosti DEFAULT; poměr nebo vlastní pulzy = FIXED; obojí jde dál
     * měnit (Set speed). Jiný profil dá záznam ORIGIN_NONE: Set speed je
     * zakázané, profil vrátí jen další převod. CMTSPEED_CUSTOM převod
     * nepřijme (pulzy nemá kudy zadat).
     *
     * Výsledek stejný jako dosavadní blok (bajty i záznam rychlosti) = úspěch
     * beze změny pásky. Jinak cmttmz_replace_block(): blok neuložený,
     * Modified 1, čítač výměn pásky, kurzor (byl-li blok otevřený, otevře se
     * nový s pozicí 0), stav CMT hacku smazaný; společné pravidlo švu
     * (blok MZ800EMU-CMT s pauzou 0, za kterým je další blok, dostane 1000 ms).
     *
     * @param block_id Číslo bloku.
     * @param profile_id ID profilu z registru (rozlišuje velikost písmen).
     * @param speed Rychlost (viz výše; CMTSPEED_NONE = bez rychlosti).
     * @return EXIT_SUCCESS; EXIT_FAILURE s textem v cmttmz_get_edit_error() a páskou
     *         beze změny: "No tape inserted", "Block N does not exist", "Block N
     *         has no MZ file that can be converted", "Unknown profile: X",
     *         "Profile X does not accept a speed", "Invalid speed",
     *         "Can't build the block with profile X: <důvod registru>", chyby
     *         cmttmz_replace_block().
     * @post cmttmz_get_edit_warning() nese varování tohoto volání ("" bez varování).
     * @pre Vlákno emulátoru pod device_state_lock(), transport ve STOP, neběží
     *      nahrávání ani záznam vstupů (ověřuje obal cmt_tape_convert_block()).
     */
    extern int cmttmz_block_convert_profile ( int block_id, const char *profile_id, en_CMTSPEED speed );

#ifdef __cplusplus
}
#endif

#endif /* CMT_TMZ_META_H */
