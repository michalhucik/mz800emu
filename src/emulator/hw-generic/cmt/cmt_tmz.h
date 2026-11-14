/**
 * @file   cmt_tmz.h
 * @brief  Rozšíření Virtual CMT, které přehrává každou pásku jako TMZ přes tmz_player.
 *
 * Jediné přehrávací rozšíření Virtual CMT (přípony tmz, tzx, mzf, m12,
 * mzt, tap, wav, wave). Soubor pásky se při otevření převede na
 * st_TZX_FILE (TMZ v paměti, cmt_tmz_import.h), rozšíření z něj sestaví
 * kontejner typu CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE s indexem všech bloků
 * souboru (i řídicích, ty mají typ CMTEXT_BLOCK_TYPE_CONTROL) a otevírá
 * bloky přes stavový automat tmz_player: stream bloku (vstream s frekvencí
 * GDGCLK_BASE, včetně ticha pauzy za blokem) zabalí do st_CMTEXT_BLOCK.
 *
 * Vlákna: vše (otevření, vysunutí, otevření bloku, změna rychlosti,
 * připojení bloků záznamu, úpravy pásky, dotazy cmttmz_*) běží ve vlákně
 * emulátoru, stejně jako ostatní operace g_cmt. Soubor ani stav playeru
 * nejsou chráněné zámkem; index kontejneru čte UI pod device_state_lock(),
 * proto se mění (cmttmz_append_blocks(), cmttmz_insert_blocks(),
 * cmttmz_remove_blocks(), cmttmz_move_block(), cmttmz_new_tape()) pod ním.
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

#ifndef CMT_TMZ_H
#define CMT_TMZ_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libs/tzx/tzx.h"
#include "libs/mzf/mzf.h"
#include "libs/mztape/mztape.h"
#include "libs/tmz/tmz_player.h"

#include "cmtext.h"
#include "cmt_tmz_import.h"


/**
 * @brief Formátová varianta signálu MZF, který Virtual CMT přehrává (blok 0x46 MZ800EMU-CMT).
 *
 * Délky pulzů pro poměr rychlosti (mztape_get_speed_pulses()) a poměr
 * vlastních pulzů (mztape_pulses_get_ratio()) se počítají v této variantě.
 */
#define CMTTMZ_FORMATSET MZTAPE_FORMATSET_MZ800_SANE


    /**
     * @brief Rozšíření Virtual CMT pro všechny přehrávané pásky.
     *
     * Ukazuje na g_cmt_tmz_extension_def. container a block jsou NULL bez
     * vložené pásky; block je NULL i u pásky bez bloku se signálem
     * (otevření takové pásky projde, přehrávání zůstane ve STOP).
     */
    extern st_CMTEXT *g_cmt_tmz_extension;

    /**
     * @brief Úložiště rozšíření pro statický registr v cmtext.c (adresa jde do inicializátoru pole).
     *
     * Jinde se používá g_cmt_tmz_extension.
     */
    extern st_CMTEXT g_cmt_tmz_extension_def;


    /**
     * @brief Výsledek cmttmz_advance() (přechod na další blok při přehrávání).
     */
    typedef enum en_CMTTMZ_ADVANCE {
        CMTTMZ_ADVANCE_BLOCK = 0,  /**< otevřen další blok se signálem, páska hraje dál */
        CMTTMZ_ADVANCE_STOP_TAPE,  /**< player zastavil pásku (Pause 0 nebo 0x2A); otevřen blok se signálem za příkazem, páska má čekat v pauze */
        CMTTMZ_ADVANCE_END,        /**< konec pásky (i zastavení pásky, za kterým už žádný blok se signálem není); otevřený blok se nemění */
        CMTTMZ_ADVANCE_ERROR,      /**< chyba (jiná chyba playeru, přestavění rychlosti, alokace, pojistka iterací playeru); otevřený blok se nemění, hlášení v logu */
    } en_CMTTMZ_ADVANCE;


    /**
     * @brief Postupné přehrávání: otevře další blok podle průchodu playeru.
     *
     * Volá cmt.c (cmt_update_output()) na konci streamu otevřeného bloku.
     * Pokračuje tmz_player_play_next() na stejném stavu playeru jako
     * předchozí blok, takže následnost bloků (smyčky, skoky, volání),
     * úroveň signálu mezi bloky a zastavení pásky určuje player, ne číslo
     * bloku + 1; číslo nového bloku je state.last_played_block. Bloky
     * s příznakem přeskočení a bloky, které player trvale odmítne
     * (TMZ_PLAYER_ERROR_UNSUPPORTED, TMZ_PLAYER_ERROR_UNREADABLE), se
     * přeskočí (dostanou příznak); jiná chyba playeru vrátí
     * CMTTMZ_ADVANCE_ERROR s hlášením v logu.
     *
     * Přímé otevření bloku (cb_open_block, cb_next_block, cb_previous_block)
     * naopak player přestaví na daný blok s prázdnými zásobníky a výchozí
     * úrovní signálu; cmttmz_advance() pak pokračuje za ním.
     *
     * @return en_CMTTMZ_ADVANCE; bez vložené pásky nebo bez otevřeného bloku
     *         CMTTMZ_ADVANCE_END.
     * @pre Volá se z vlákna emulátoru.
     * @post Při CMTTMZ_ADVANCE_BLOCK a CMTTMZ_ADVANCE_STOP_TAPE je
     *       g_cmt_tmz_extension->block nový blok (starý uvolněn), stream
     *       čte od začátku. Při CMTTMZ_ADVANCE_ERROR se stav playeru vrátí
     *       na stav před voláním (otevřený blok se nemění), takže další
     *       PLAY a konec bloku zkusí tentýž přechod znovu a blok s přechodnou
     *       chybou se nepřeskočí. Při CMTTMZ_ADVANCE_END zůstává stav
     *       playeru na konci pásky.
     */
    extern en_CMTTMZ_ADVANCE cmttmz_advance ( void );


    /**
     * @brief Převine pásku: otevře první blok se signálem průchodem playeru od začátku.
     *
     * Player začne blokem 0 s prázdnými zásobníky a výchozí úrovní signálu
     * a zpracuje řídicí bloky na začátku pásky (smyčka, skok, zastavení
     * pásky se při hledání prvního bloku přeskočí). Používá ho otevření
     * pásky a automatické převinutí po konci pásky (g_cmt.autorewind).
     * Transport (g_cmt.state) nemění.
     *
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky nebo když žádný
     *         blok otevřít nejde (otevřený blok i stav playeru se pak nemění).
     * @pre Volá se z vlákna emulátoru.
     */
    extern int cmttmz_rewind ( void );


    /**
     * @brief Přímo otevře blok vložené pásky bez informačního výpisu (pro cmt_tape_set_position()).
     *
     * Stejné jako cb_open_block kontejneru (přímé otevření: player se
     * přestaví na blok, prázdné zásobníky, výchozí úroveň signálu; blok
     * CONTROL, blok s příznakem přeskočení a blok mimo rozsah otevřít
     * nejde), jen bez řádku "TMZ block N: ..." na stdout. Varování
     * a chyby se vypisují dál.
     *
     * @param block_id Index bloku v souboru.
     * @return EXIT_SUCCESS; EXIT_FAILURE (otevřený blok i stav playeru se nemění).
     * @pre Volá se z vlákna emulátoru.
     */
    extern int cmttmz_seek_block ( int block_id );

    /**
     * @brief Kopie stavu playeru postupného přehrávání (pro snapshot).
     *
     * Stav nese pozici v průchodu páskou (current_block, last_played_block),
     * zásobníky smyček a volání, příznaky stopped / finished, úroveň
     * signálu a takt CPU z bloku 0x43 (config.mz_cpu_clock). Přímé otevření
     * bloku ho přestaví, cmttmz_advance() z něj pokračuje.
     *
     * @param[out] out Kopie stavu; ukazatel file a ostatní config jsou
     *        vnitřní údaje rozšíření, volající je nemá ukládat.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky nebo s NULL.
     * @pre Volá se z vlákna emulátoru.
     */
    extern int cmttmz_get_player_state ( st_TMZ_PLAYER_STATE *out );

    /**
     * @brief Nastaví stav playeru postupného přehrávání (ze snapshotu) po ověření proti vložené pásce.
     *
     * Ověří: current_block <= počet bloků souboru (i bez finished: po
     * posledním bloku se signálem je current_block == počet bloků
     * a finished se nastaví až dalším průchodem; skok mimo pásku nastaví
     * finished na bloku skoku); last_played_block < počet bloků a bez
     * finished rovný číslu otevřeného bloku (po konci pásky,
     * CMTTMZ_ADVANCE_END, je to poslední blok, který player po přehrání
     * otevřeného bloku zpracoval jako datový - zastavení pásky, blok
     * s příznakem skip, blok bez signálu; po skoku zpět může v souboru ležet
     * i před otevřeným blokem, ověřuje se jen rozsah); hloubky zásobníků 0 až
     * TMZ_PLAYER_MAX_LOOP_DEPTH / TMZ_PLAYER_MAX_CALL_DEPTH; u každé
     * smyčky je blok před start_block Loop Start (0x24) a remaining je
     * 1 až jeho počet opakování; u každého volání je call_block Call
     * Sequence (0x26) s čitelnými offsety, total_offsets je jeho počet
     * offsetů a current_offset_idx < total_offsets; config.mz_cpu_clock
     * je 0 nebo takt z některého bloku 0x43 souboru (který blok platí,
     * závisí na cestě páskou, proto se ověřuje jen původ hodnoty);
     * signal_level je 0, 1 nebo TZX_LEVEL_AFTER_PAUSE; stopped a finished
     * nejsou zároveň.
     * Příznaky se normalizují na bool. Ukazatel file a config (kromě
     * mz_cpu_clock) se z in neberou, zůstávají z rozšíření. Volá se po
     * otevření bloku (to stav playeru přestaví).
     *
     * @param in Stav (např. přečtený ze snapshotu).
     * @return EXIT_SUCCESS; EXIT_FAILURE při neplatné hodnotě, bez vložené
     *         pásky, bez otevřeného bloku nebo s NULL (stav playeru se
     *         pak nemění).
     * @pre Volá se z vlákna emulátoru.
     */
    extern int cmttmz_set_player_state ( const st_TMZ_PLAYER_STATE *in );

    /**
     * @brief Zjistí, zda jde blok vložené pásky přepnout na danou rychlost (jediné místo pravidla).
     *
     * Blok 0x46 MZ800EMU-CMT z importu MZF/MZT nebo z TMZ s čitelnými
     * PROFILE_ARGS (ve vedlejším poli rychlostí původ DEFAULT nebo FIXED,
     * v indexu typ MZF): CMTSPEED_NONE (= výchozí rychlost Virtual CMT),
     * platný poměr i CMTSPEED_CUSTOM. Blok 0x11 z importu TAP (typ
     * TAPHEADER / TAPDATA): platný poměr (cmtspeed_is_valid()), ne
     * CMTSPEED_NONE ani CMTSPEED_CUSTOM. Ostatní bloky (rychlost ze souboru:
     * jiné profily 0x46, bloky 0x10-0x18 ze souboru, WAV, CONTROL) ne.
     *
     * Volá ho cmt.c (cmt_tape_set_block_cmt_speed(),
     * cmt_tape_set_block_pulses(), cmt_tape_change_block_speed()).
     *
     * @param block_id Index bloku (mimo rozsah = 0).
     * @param cmtspeed Požadovaná rychlost.
     * @return 1 = změnu lze provést, 0 = nelze (i bez vložené pásky).
     * @pre Volá se z vlákna emulátoru.
     */
    extern int cmttmz_block_speed_changeable ( int block_id, en_CMTSPEED cmtspeed );

    /**
     * @brief Záznam rychlosti bloku vložené pásky (vedlejší pole: původ, rychlost sestavení, pulzy, pauza).
     *
     * Kopie údaje, který import nebo úprava pásky k bloku uložila
     * (st_CMTTMZ_BLOCK_SPEED); převod profilu (cmt_tmz_meta.c) z něj bere
     * pauzu bloku a porovnává ho s novým záznamem (převod beze změny).
     *
     * @param block_id Číslo bloku.
     * @param[out] out Záznam (při chybě se nemění).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, mimo rozsah nebo s NULL.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_get_block_speed_record ( int block_id, st_CMTTMZ_BLOCK_SPEED *out );


    /**
     * @brief Přestaví v souboru všechny bloky s výchozí rychlostí na novou výchozí rychlost.
     *
     * Bloky MZF s CMTEXT_BLOCK_SPEED_DEFAULT v indexu, které jsou sestavené
     * jinou rychlostí, se sestaví znovu (cmttmz_build_mzf_block()) a vymění
     * na témže indexu souboru. Nejdřív se sestaví všechny; když některý
     * sestavit nejde (např. vlastní pulzy mimo 1 až 65535 µs), soubor se
     * nemění. Stream otevřeného bloku se tu nepřegeneruje (to dělá
     * cb_set_speed bloku z cmt_change_speed()).
     *
     * @param speed Nová výchozí rychlost (poměr nebo CMTSPEED_CUSTOM).
     * @param pulses Vlastní délky pulzů pro CMTSPEED_CUSTOM (jinak smí být NULL).
     * @return EXIT_SUCCESS (i bez vložené pásky nebo bez bloků DEFAULT);
     *         EXIT_FAILURE, když některý blok sestavit nejde nebo CMTSPEED_CUSTOM
     *         bez pulzů (soubor beze změny).
     * @pre Volá se z vlákna emulátoru, transport ve STOP (volá cmt_change_speed()).
     */
    extern int cmttmz_apply_default_speed ( en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *pulses );


    /**
     * @brief Vložená páska jako TMZ v paměti.
     *
     * @return Soubor vlastněný rozšířením, nebo NULL bez vložené pásky.
     *         Platí do vysunutí nebo otevření jiné pásky; volající ho nesmí
     *         měnit ani uvolnit. Bloky, jejichž rychlost jde měnit
     *         (cmttmz_block_speed_changeable(): 0x46 MZ800EMU-CMT z importu
     *         MZF/MZT i z TMZ, 0x11 z importu TAP), se při změně rychlosti
     *         mohou přestavět (data bloku se vymění), ukazatele do dat bloků
     *         proto neplatí přes změnu rychlosti.
     * @pre Volá se z vlákna emulátoru.
     */
    extern const st_TZX_FILE* cmttmz_get_file ( void );


    /**
     * @brief Čítač výměn otevřeného bloku pásky.
     *
     * Roste při každém otevření bloku rozšířením (přímé otevření: Next,
     * Prev, seek, snapshot, změna rychlosti; postupné přehrávání; otevření
     * a převinutí pásky), při vysunutí pásky, připojení bloků a úpravě
     * pásky (vložení, mazání, přesun), takže i otevření téhož
     * bloku znovu dá jinou hodnotu. Nikdy se nenuluje. CMT hack jím pozná, že
     * páskou mezi dvěma pastmi ROM pohnul uživatel nebo MCP.
     *
     * @return Hodnota čítače (přetečení po 2^32 otevřeních se neřeší).
     * @pre Volá se z vlákna emulátoru.
     */
    extern unsigned cmttmz_get_open_serial ( void );


    /**
     * @brief Čítač výměn pásky: roste při otevření pásky, vysunutí, nové pásce,
     *        při zkrácení pásky návratem na značku (cmttmz_tape_mark_restore())
     *        a při úpravě pásky (mazání, přesun, vložení cmttmz_insert_blocks()
     *        i na konec).
     *
     * Na rozdíl od cmttmz_get_open_serial() se nemění při výměně otevřeného
     * bloku (přehrávání, Next, Prev, seek) ani při připojení bloků záznamem
     * (cmttmz_append_blocks()): čísla bloků zůstávají platná. Okno Virtual CMT jím ruší výběr řádku,
     * když čísla bloků přestanou platit. Nikdy se nenuluje.
     *
     * @return Hodnota čítače.
     * @pre Volá se pod device_state_lock() (UI) nebo z vlákna emulátoru.
     */
    extern unsigned cmttmz_get_tape_serial ( void );


    /**
     * @brief Zjistí, zda blok vložené pásky nese MZF.
     *
     * @param block_id Index bloku v souboru (cmttmz_get_file()).
     * @return 1 pro blok 0x40, 0x41 nebo 0x46; 0 pro jiný blok, index mimo
     *         rozsah nebo bez vložené pásky.
     * @pre Volá se z vlákna emulátoru.
     */
    extern int cmttmz_block_is_mzf ( int block_id );


    /**
     * @brief Vrátí MZF (hlavičku a tělo) bloku vložené pásky.
     *
     * Převod dělá tmz_block_to_mzf() (hlavička v pořadí hostitele).
     *
     * @param block_id Index bloku v souboru.
     * @return Nová kopie (uvolnit mzf_free()); NULL, pokud blok MZF nenese,
     *         nejde převést, index je mimo rozsah nebo není vložená páska.
     * @pre Volá se z vlákna emulátoru.
     */
    extern st_MZF* cmttmz_block_get_mzf ( int block_id );


    /** @brief Velikost bufferu varování otevření pásky (včetně ukončovací nuly). */
#define CMTTMZ_WARNING_SIZE 512


    /**
     * @brief Varování naposledy otevřené pásky (MCP emu_cmt_open, pole warning).
     *
     * Anglicky, víc varování odděleno "; ", v tomto pořadí:
     * - varování importu (cmt_tmz_import.h, st_CMTTMZ_IMPORT_REPORT::warning),
     * - "Blocks the player cannot play (skipped): <blok> (ID 0x<id>), ..."
     *   - bloky, jejichž druh player odmítne, zjištěné bez generování
     *   signálu (ID mimo podporované, např. 0x16, 0x17, 0x19; blok 0x18
     *   s kompresí jinou než RLE, s neplatnými daty nebo se signálem delším
     *   než CMT_VSTREAM_MAX_BLOCK_SECONDS); nejvýš 8, zbytek
     *   "and N more",
     * - bez otevřeného bloku: "The tape has no playable block", "Block <n>
     *   could not be opened, no block is open" nebo "Block <n> could not
     *   be rebuilt at the requested speed, no block is open".
     *
     * Blok podporovaného druhu, který signál nedá (např. data mimo meze
     * generátoru), se ohlásí až při přehrávání, a to jen do logu (stderr).
     * Stejné texty jdou při otevření do logu.
     *
     * @return Statický buffer rozšíření (nejvýš CMTTMZ_WARNING_SIZE - 1
     *         znaků, delší text se ořízne); prázdný řetězec = bez varování
     *         (i bez vložené pásky). Platí do dalšího otevření pásky.
     * @pre Volá se z vlákna emulátoru.
     */
    extern const char* cmttmz_get_open_warning ( void );


    /**
     * @brief Text kódu hlavičky ZX TAP pro seznam bloků.
     *
     * @param code Kód typu z hlavičky ZX (první bajt za flagem), -1 = datový blok.
     * @return "Program Header", "Num Array Header", "Char Array Header",
     *         "File Header" pro kódy 0 až 3, jinak "Data". Statický řetězec.
     */
    extern const char* cmttmz_tap_code_txt ( int code );


    /**
     * @brief Soubor pásky převedený importem, ještě nevložený (cmttmz_open_prepare()).
     *
     * Neprůhledný typ. Vlastní ho volající, dokud ho nepředá
     * cmttmz_open_prepared() (ta ho převezme i při chybě), jinak ho uvolní
     * cmttmz_prepared_free().
     */
    typedef struct st_CMTTMZ_PREPARED st_CMTTMZ_PREPARED;

    /**
     * @brief Načte a převede soubor pásky bez zásahu do vložené pásky (první fáze otevření).
     *
     * Import (cmttmz_import_file_speeds()) s výchozí rychlostí a vlastními
     * pulzy z g_cmt a normální polaritou WAV, stejně jako otevření souboru.
     * Vložená páska, její příznak Modified, otevřený blok, probíhající
     * nahrávání ani varování otevření (cmttmz_get_open_warning()) se
     * nemění, takže neúspěšné otevření nic neztratí.
     *
     * @param filename Cesta k souboru (UTF-8).
     * @return Připravený soubor; NULL, když soubor nejde přečíst nebo
     *         převést (chybí, je poškozený, nepodporovaný) - důvod jde na
     *         stderr a do cmttmz_get_prepare_error(); hlášení uživateli
     *         dává volající (jedno, s důvodem i jménem souboru).
     * @pre Vlákno emulátoru.
     */
    extern st_CMTTMZ_PREPARED* cmttmz_open_prepare ( const char *filename );

    /**
     * @brief Důvod posledního neúspěšného cmttmz_open_prepare() (anglicky, z importu).
     * @return Statický text; "" po úspěchu. Platí do dalšího cmttmz_open_prepare().
     * @pre Vlákno emulátoru.
     */
    extern const char* cmttmz_get_prepare_error ( void );


    /**
     * @brief Druhá fáze otevření: vysune vloženou pásku a vloží připravený soubor.
     *
     * Převezme @p prep (uvolní ho i při chybě). Nastaví varování otevření
     * (varování importu, nepřehratelné bloky, žádný otevřený blok), sestaví
     * index a otevře první blok se signálem. Nastavení g_cmt (ext, jméno pro
     * UI, last_filename) dělá volající (cmt_open_file()).
     *
     * @param prep Výsledek cmttmz_open_prepare() (NULL = chyba).
     * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace nebo s NULL
     *         (rozšíření pak nemá vloženou pásku).
     * @pre Vlákno emulátoru pod device_state_lock() (mění kontejner); vložená
     *      páska už vysunutá přes cmt_eject() (transport STOP, nahrávání
     *      ukončené).
     */
    extern int cmttmz_open_prepared ( st_CMTTMZ_PREPARED *prep );

    /**
     * @brief Uvolní připravený soubor, který se nevložil.
     * @param prep Připravený soubor (NULL = nic).
     */
    extern void cmttmz_prepared_free ( st_CMTTMZ_PREPARED *prep );

    /**
     * @brief Vejde se vložená páska do formátu souboru podle přípony cesty?
     *
     * Stejná pravidla jako cmttmz_save(), bez zápisu: .tmz a .tzx vždy;
     * .mzt jen páska s aspoň jedním blokem, jejíž každý blok nese MZF
     * (tmz_mzt_block_info()); .mzf a .m12 jen páska s jediným blokem MZF.
     * Varování (rychlost v MZT, bloky MZ v .tzx) se nezjišťují, uložení
     * s nimi projde.
     *
     * @param path Cesta nebo jméno souboru (rozhoduje přípona).
     * @return 1 = cmttmz_save() do tohoto formátu obsah uloží; 0 = ne
     *         (formát pásku neunese, nepodporovaná přípona, bez pásky).
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_save_fits ( const char *path );


    /**
     * @brief Vloží novou prázdnou pásku bez cesty (TMZ 1.1, 0 bloků).
     *
     * Dosavadní pásku rozšíření vysune (cmttmz_eject(): soubor, index,
     * otevřený blok). Kontejner SIMPLE_TAPE má cestu "" a jméno "", nemá
     * otevřený blok; páska není Modified. Nastavení g_cmt (ext, jméno pro UI,
     * transport) dělá volající (cmt_record()).
     *
     * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (rozšíření pak
     *         nemá vloženou pásku).
     * @pre Vlákno emulátoru, transport ve STOP; mění kontejner, proto pod
     *      device_state_lock(), pokud ho UI může číst.
     */
    extern int cmttmz_new_tape ( void );

    /**
     * @brief Vloží pásku z obrazu TMZ / TZX v paměti (vložená páska snapshotu).
     *
     * Dosavadní pásku vysune (cmttmz_eject()). Obraz se importuje jako
     * soubor .tmz (cmttmz_import_buffer_speeds(), stejně jako
     * cmt_open_file_by_extension() u .tmz: bloky 0x46 MZ800EMU-CMT dostanou
     * měnitelnou rychlost), kontejner dostane cestu @p path (může být ""
     * = páska bez souboru, jméno pak "") a páska je Modified (obsah se liší
     * od souboru, nebo soubor nemá). Páska bez cesty má všechny bloky
     * neuložené, páska s cestou všechny uložené (které bloky soubor nese,
     * obraz neříká; snapshot příznaky obnoví cmttmz_set_block_unsaved()).
     * Otevře se první blok jako po otevření
     * souboru; varování importu jsou v cmttmz_get_open_warning(). Obraz jen
     * z hlavičky (10 B, páska bez bloků) dá prázdnou pásku s hlavičkou
     * z obrazu (signatura ZXTape! / TapeMZ! a verze se zachovají).
     *
     * Obraz (tzx_save()) nenese, že blok 0x18 vznikl importem WAV (typ
     * CMTEXT_BLOCK_TYPE_WAV, na který působí přepínač polarity); ten nese
     * @p wav_blocks (cmttmz_get_wav_blocks() pásky, ze které obraz vznikl).
     * Bez příznaku je blok 0x18 typu TZX (jako ze záznamu nebo z TZX).
     *
     * Nastavení g_cmt (ext, jméno pro UI, last_filename) dělá volající
     * (cmt_open_image()).
     *
     * @param data Obraz (nedůvěryhodný vstup; vadný = chyba, ne pád). @param size Délka v bajtech.
     * @param path Cesta pásky (NULL = "").
     * @param wav_blocks Příznaky původu WAV po blocích obrazu (1 = blok 0x18
     *        z importu WAV; u jiného bloku se nepoužije), NULL = žádný.
     *        Nedůvěryhodný vstup smí být libovolný (jen typ bloku 0x18).
     * @param wav_count Počet položek @p wav_blocks (bloky za ním bez příznaku).
     * @return EXIT_SUCCESS; EXIT_FAILURE, když obraz nejde načíst (rozšíření pak nemá pásku).
     * @pre Vlákno emulátoru pod device_state_lock() (mění kontejner).
     */
    extern int cmttmz_open_image ( const uint8_t *data, uint32_t size, const char *path, const uint8_t *wav_blocks,
                                   uint32_t wav_count );

    /**
     * @brief Příznaky původu WAV bloků vložené pásky (typ indexu CMTEXT_BLOCK_TYPE_WAV).
     *
     * Doplněk obrazu cmttmz_save_image(): obraz typ bloku 0x18 nenese,
     * snapshot a záznam vstupů ho předají cmttmz_open_image().
     *
     * @param[out] count Počet položek (= počet bloků pásky), 0 při NULL.
     * @return Pole jedné položky na blok (1 = blok WAV; g_malloc, uvolní
     *         volající g_free()), NULL bez pásky nebo bez bloku WAV.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern uint8_t* cmttmz_get_wav_blocks ( uint32_t *count );

    /**
     * @brief Obraz vložené pásky v paměti: tzx_save() celého souboru (formát TMZ / TZX podle hlavičky).
     * @param[out] data Obraz (g_malloc, uvolní volající g_free()). @param[out] size Délka.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky nebo při chybě sestavení (*data NULL).
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern int cmttmz_save_image ( uint8_t **data, size_t *size );

    /**
     * @brief Značka vložené pásky pro checkpoint záznamu vstupů (návrat po připojení záznamu).
     *
     * Nahrávání pásku mění jen připojením bloků na konec (cmttmz_append_blocks()),
     * které navíc může dosavadnímu poslednímu bloku 0x46 dát pauzu
     * a změnit hlavičku souboru (TZX -> TMZ 1.1); nahrávání s příponou pásky
     * pak pásku uloží (cmttmz_save(): cesta, Modified). Značka nese, co je
     * třeba k přesnému návratu: počet bloků, pauzu dosavadního posledního
     * bloku, jeho bajty jen tehdy, když ho připojení může přestavět (blok
     * 0x46 MZ800EMU-CMT s pauzou 0; jiný poslední blok, např. velký 0x18
     * pásky WAV, se nekopíruje - značka vzniká při každém checkpointu),
     * hlavičku, Modified, cestu pásky a příznaky neuloženo bloků
     * (st_CMTTMZ_BLOCK_UI::unsaved; uložení pásky nahrávání s příponou je
     * vynuluje, návrat je vrátí).
     *
     * Úpravy pásky (cmttmz_insert_blocks() mimo konec, cmttmz_remove_blocks(),
     * cmttmz_move_block()) značka vrátit neumí; při záznamu a přehrávání
     * vstupů jsou zakázané (fronta zásahů, třída příkazu MCP). Značka je jen
     * v paměti (checkpoint záznamu vstupů, replay.c), nikam se neukládá.
     *
     * Vlastnictví: path, last_data a unsaved patří značce, uvolní je
     * cmttmz_tape_mark_free().
     *
     * @invariant block_count <= počet bloků pásky, ke které značka patří.
     * @invariant last_data != NULL => block_count > 0 a last_data obsahuje
     *            last_len bajtů bloku block_count - 1, který připojení
     *            záznamu může přestavět, sestavených rychlostí last_speed
     *            (a last_pulses): blok může být v té chvíli sestavený jinou
     *            rychlostí, než má hrát, a po značce se přestavět (otevření,
     *            uložení), proto značka nese i záznam rychlosti.
     * @invariant unsaved != NULL <=> block_count > 0; unsaved má block_count
     *            položek; některá 1 => modified.
     */
    typedef struct st_CMTTMZ_TAPE_MARK {
        uint32_t block_count;      /**< počet bloků pásky */
        uint8_t *unsaved;          /**< příznaky neuloženo bloků 0 .. block_count - 1 (1 = blok není v souboru pásky; g_free), NULL bez bloků */
        uint16_t last_pause_ms;    /**< pauza posledního bloku ve vedlejším poli rychlostí (bez bloků 0) */
        int modified;              /**< příznak Modified */
        st_TZX_HEADER header;      /**< hlavička souboru pásky */
        bool is_tmz;               /**< signatura TapeMZ! (true) / ZXTape! */
        char *path;                /**< cesta pásky (kontejner, "" = bez souboru; g_free) */
        uint8_t last_id;           /**< ID dosavadního posledního bloku */
        uint32_t last_len;         /**< délka dat dosavadního posledního bloku */
        uint8_t *last_data;        /**< kopie dat dosavadního posledního bloku (g_free); NULL bez bloků nebo u bloku, který připojení nepřestaví */
        en_CMTSPEED last_speed;    /**< rychlost, kterou jsou last_data sestavená (vedlejší pole rychlostí); platí jen s last_data */
        st_MZTAPE_PULSES_LENGTH last_pulses; /**< pulzy k last_speed CMTSPEED_CUSTOM (vedlejší pole rychlostí); platí jen s last_data */
    } st_CMTTMZ_TAPE_MARK;

    /**
     * @brief Zapamatuje značku vložené pásky (kopie cesty, posledního bloku a příznaků neuloženo).
     * @param[out] mark Značka (vynulovaná při neúspěchu); uvolnit cmttmz_tape_mark_free().
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky nebo při selhání alokace.
     * @pre Vlákno emulátoru.
     */
    extern int cmttmz_tape_mark_get ( st_CMTTMZ_TAPE_MARK *mark );

    /** @brief Uvolní data značky (cmttmz_tape_mark_get(): cesta, poslední blok, příznaky neuloženo); značka pak je vynulovaná. @param mark Značka (NULL = nic). */
    extern void cmttmz_tape_mark_free ( st_CMTTMZ_TAPE_MARK *mark );

    /**
     * @brief Vrátí vloženou pásku na značku: počet bloků, poslední blok bajtově, hlavičku, Modified, cestu a příznaky neuloženo.
     *
     * Bloky za značkou se uvolní (data, položky indexu). Příznaky neuloženo
     * bloků do značky se vrátí podle značky, při Modified 0 jsou uložené
     * všechny; délka vráceného posledního bloku se
     * spočítá znovu jen levně (z parametrů bloku nebo průchodem dat 0x18,
     * bez generování signálu), jinak zůstane neznámá. Otevřený blok za
     * značkou se zavře (otevřený blok pak chybí, otevře ho volající); stav
     * playeru se nemění (nastaví ho snapshot checkpointu). Dosavadní poslední
     * blok dostane zpět přesně svá data z doby značky, nese-li je značka
     * (připojení mu mohlo přestavbou dát pauzu), s nimi rychlost a pulzy,
     * kterými jsou sestavená (vedlejší pole rychlostí; jinak by záznam
     * rychlosti po přestavbě po značce neodpovídal datům a blok by se už
     * nepřestavěl), a pauzu ve vedlejším poli.
     * Liší-li se cesta kontejneru od cesty značky (uložení po nahrávání),
     * vrátí se i ona (cmt_tape_path_changed()), a to jako první: kopie dat
     * i cesta se alokují dřív, než se páska zkrátí, takže selhání alokace
     * nechá pásku celou beze změny.
     *
     * @param mark Značka téže pásky (cmttmz_tape_mark_get()).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, má-li páska méně bloků
     *         než značka (jiná páska) nebo při selhání alokace (páska pak
     *         beze změny).
     * @pre Vlákno emulátoru pod device_state_lock() (mění kontejner).
     */
    extern int cmttmz_tape_mark_restore ( const st_CMTTMZ_TAPE_MARK *mark );

    /**
     * @brief Připojí bloky na konec vložené pásky (záznam).
     *
     * Společná údržba stavu s cmttmz_insert_blocks() (pozice = počet bloků),
     * ale jako jediná úprava nechává čísla bloků, otevřený blok, pozici,
     * stav playeru, stav CMT hacku i čítač výměn pásky (původní chování
     * záznamu); nové bloky mají v údajích pro UI příznak "vznikl záznamem".
     * Text chyby úpravy (cmttmz_get_edit_error()) nemění.
     *
     * Bloky se přidají do souboru (tzx_file_append_block()), vedlejší pole
     * rychlostí, příznaků přeskočení a neuloženo se zvětší (záznamy rychlosti z @p speeds,
     * skip 0), index kontejneru dostane položky nových bloků (blok 0x18 jako
     * CMTEXT_BLOCK_TYPE_TZX, ne WAV: nese úroveň PC1 a přepínač polarity na něj
     * nepůsobí) a count_blocks se zvýší. Páska TMZ 1.0 dostane při připojení
     * bloku 0x40 až 0x4F verzi 1.1, páska ZXTape! (TZX) hlavičku TMZ 1.1
     * (blok MZ v TZX není; uložení s příponou .tzx pak varuje). Dosavadní poslední blok 0x46 MZ800EMU-CMT
     * s pauzou 0 (rychlost DEFAULT nebo SET) se přestaví s pauzou 1000 ms,
     * aby mezi ním a novými bloky byla mezera (jiné bloky se nemění). Zvedne
     * se čítač otevření (cmttmz_get_open_serial()) a nastaví Modified. Nové
     * bloky mají v údajích pro UI příznak "vznikl záznamem" a jsou neuložené
     * (cmttmz_get_block_ui()); jejich délky (a délka přestavěného
     * dosavadního posledního bloku) se spočítají jen levně, bez generování
     * signálu (surový blok 0x18 jedním průchodem dat RLE); blok MZ zůstane
     * s neznámou délkou pro zásah UI cmt_request_lengths().
     * Otevřený blok, pozice v něm, stav playeru (odkazuje bloky číslem
     * a stabilním ukazatelem na soubor), čítač výměn pásky
     * (cmttmz_get_tape_serial()) i transport zůstávají.
     *
     * @param blocks Bloky (st_TZX_BLOCK po hodnotě). Při úspěchu jejich data
     *        převezme páska a funkce v poli nastaví data na NULL; pole samo
     *        uvolní volající. Při chybě zůstane vše volajícímu.
     * @param speeds Záznamy rychlosti nových bloků (count položek; MZF ze
     *        záznamu CMTTMZ_SPEED_ORIGIN_DEFAULT s výchozí rychlostí, surový blok
     *        ORIGIN_NONE). Kopírují se.
     * @param count Počet bloků (0 = nic se nestane, úspěch).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky, s NULL parametrem
     *         nebo při selhání alokace - pak páska (soubor, index, vedlejší
     *         pole, počet bloků) zůstane beze změny.
     * @pre Vlákno emulátoru pod device_state_lock() (index čte UI vlákno).
     */
    extern int cmttmz_append_blocks ( st_TZX_BLOCK *blocks, const st_CMTTMZ_BLOCK_SPEED *speeds, uint32_t count );


    /**
     * @brief Vloží bloky do vložené pásky před blok @p at (úprava pásky).
     *
     * Vložené bloky dostanou záznamy rychlosti z @p speeds, příznak skip 0,
     * položky indexu (blok 0x18 jako CMTEXT_BLOCK_TYPE_TZX), údaje pro UI bez
     * příznaku "vznikl záznamem" a příznak neuloženo (cmttmz_block_is_unsaved()).
     * Stávající bloky si nesou své položky indexu (režim a rychlost bloku,
     * typ WAV), vedlejší pole a příznak neuloženo s sebou. Společná údržba
     * stavu se cmttmz_append_blocks(), cmttmz_remove_blocks()
     * a cmttmz_move_block():
     * - hlavička: blok 0x40 až 0x4F v pásce ZXTape! = TMZ 1.1, v TMZ 1.0 verze 1.1,
     * - pauza na švu: blok 0x46 MZ800EMU-CMT s pauzou 0 před vloženými
     *   bloky a poslední vložený blok, za kterým další blok následuje, se
     *   přestaví s pauzou 1000 ms (pravidlo cmttmz_append_blocks()),
     * - délky: vložené a přestavěné bloky (s blokem 0x43 na pásce všechny)
     *   jsou neznámé, levně se spočítají hned,
     * - čítač otevření a změn vzrostou, Modified 1,
     * - pro každé @p at (i na konec): vzroste čítač výměn
     *   pásky (cmttmz_get_tape_serial()), smaže se stav CMT hacku nad páskou
     *   (cmthack_tape_state_clear()), otevřený blok se zavře a otevře se
     *   znovu blok, který byl otevřený (kurzor sleduje původní aktuální
     *   blok, pozice v bloku 0; stav playeru se přestaví jako při přímém
     *   otevření); bez otevřeného bloku (prázdná páska) se páska převine,
     *   takže po vložení do prázdné pásky je otevřený první blok se signálem
     *   a PLAY funguje.
     *
     * @param at Pozice 0 .. počet bloků (počet = na konec).
     * @param blocks Bloky (st_TZX_BLOCK po hodnotě). Při úspěchu jejich data
     *        převezme páska a funkce v poli nastaví data na NULL; pole samo
     *        uvolní volající. Při chybě zůstane vše volajícímu.
     * @param speeds Záznamy rychlosti nových bloků (count položek, kopírují se).
     * @param count Počet bloků (0 = nic se nestane, úspěch beze změny).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky, s NULL parametrem,
     *         s @p at mimo rozsah nebo při selhání alokace - páska (soubor,
     *         index, vedlejší pole, čítače, Modified) zůstane beze změny,
     *         text chyby vrací cmttmz_get_edit_error().
     * @pre Vlákno emulátoru pod device_state_lock() (index čte UI vlákno),
     *      transport ve STOP, neběží nahrávání (ověřuje volající,
     *      cmt_tape_edit_allowed()).
     */
    extern int cmttmz_insert_blocks ( int at, st_TZX_BLOCK *blocks, const st_CMTTMZ_BLOCK_SPEED *speeds, uint32_t count );

    /**
     * @brief Smaže bloky vložené pásky (úprava pásky).
     *
     * Data bloků se uvolní (tzx_file_remove_block()), položky indexu
     * a vedlejších polí zmizí, ostatní bloky si nesou své údaje (i příznak
     * neuloženo) s sebou. Pauzy ostatních bloků se nemění (pravidlo švu se
     * u mazání neuplatní). Vzroste čítač
     * výměn pásky, otevření a změn, Modified 1, délky jsou při bloku 0x43 na
     * pásce neznámé. Otevřený blok se zavře; kurzor: byl-li smazán
     * otevřený blok, otevře se blok, který zaujal jeho místo (bez něj nový
     * poslední blok, prázdná páska = žádný otevřený blok), jinak se otevře
     * znovu týž blok; pozice v bloku 0. Nejde-li cílový blok otevřít (řídicí
     * blok, přeskakovaný), otevře se nejbližší další blok se signálem, pak
     * nejbližší předchozí, jinak žádný. Bez otevřeného bloku před úpravou se
     * páska převine (první blok se signálem průchodem playeru od začátku,
     * jako cmttmz_rewind()). Smaže se stav CMT hacku nad páskou
     * (cmthack_tape_state_clear()). Stav playeru se přestaví jako při
     * přímém otevření.
     *
     * Řídicí bloky (smyčky, skupiny, volání) se mažou jako každý jiný blok;
     * nevyvážená struktura se neodmítá (player ji přehraje bez pádu
     * a zacyklení - test_cmt_tmz_edit).
     *
     * @param block_ids Čísla bloků vzestupně bez opakování (NULL jen při @p count 0).
     * @param count Počet (0 = nic se nestane, úspěch beze změny).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky, s NULL, s číslem
     *         mimo rozsah, s pořadím, které není vzestupné bez opakování, nebo
     *         při selhání alokace - páska pak beze změny, text chyby vrací
     *         cmttmz_get_edit_error().
     * @pre Jako cmttmz_insert_blocks().
     */
    extern int cmttmz_remove_blocks ( const int *block_ids, uint32_t count );

    /**
     * @brief Nahradí jeden blok vložené pásky posloupností bloků (úprava pásky, Replace analýzy).
     *
     * Jedna úprava (všechno, nebo nic): nové bloky se nejdřív připojí
     * (jediný krok, který alokuje), pak se bez alokace smaže nahrazovaný
     * blok (tzx_file_remove_block() uvolní jeho data) a nové bloky se posunou
     * na jeho místo. Údržba stavu jako cmttmz_insert_blocks(): nové bloky
     * dostanou záznamy rychlosti z @p speeds, skip 0, příznak neuloženo,
     * položky indexu (blok 0x18 typu WAV při @p wav_type, jinak TZX; bloky
     * MZF podle obsahu), hlavička TMZ 1.1 při bloku 0x4x, čítač výměn pásky,
     * otevření a změn vzrostou, Modified 1, stav CMT hacku nad páskou se
     * smaže a kurzor sleduje otevřený blok (byl-li otevřený nahrazovaný blok,
     * otevře se první nový blok nebo nejbližší blok se signálem; pozice 0).
     * Pravidlo švu platí jen za posledním novým blokem (blok 0x46
     * MZ800EMU-CMT s pauzou 0, za kterým následuje blok, dostane 1000 ms);
     * blok před nahrazeným blokem na něj navazoval už dřív a nemění se.
     *
     * @param block_id Nahrazovaný blok (0 .. počet bloků - 1).
     * @param blocks Nové bloky (st_TZX_BLOCK po hodnotě). Při úspěchu jejich
     *        data převezme páska a funkce v poli nastaví data na NULL; pole
     *        uvolní volající. Při chybě zůstane vše volajícímu.
     * @param speeds Záznamy rychlosti nových bloků (count položek, kopírují se).
     * @param count Počet nových bloků (> 0).
     * @param wav_type 1 = nové bloky 0x18 mají v indexu typ WAV (jako blok
     *        z importu WAV; působí na ně přepínač polarity), 0 = TZX.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky ("No tape
     *         inserted"), s blokem mimo rozsah ("Block N does not exist"), bez
     *         bloků nebo s NULL ("No blocks to insert"), "Too many blocks" nebo
     *         při selhání alokace ("Not enough memory") - páska pak beze
     *         změny, text chyby vrací cmttmz_get_edit_error().
     * @pre Jako cmttmz_insert_blocks().
     */
    extern int cmttmz_replace_block ( int block_id, st_TZX_BLOCK *blocks, const st_CMTTMZ_BLOCK_SPEED *speeds, uint32_t count,
                                      int wav_type );

    /**
     * @brief Má blok vložené pásky v indexu typ WAV (blok 0x18 z importu WAV)?
     * @param block_id Číslo bloku.
     * @return 1 = typ CMTEXT_BLOCK_TYPE_WAV; 0 = jiný typ, bez pásky nebo mimo rozsah.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern int cmttmz_block_is_wav ( int block_id );

    /**
     * @brief Přesune blok vložené pásky na jinou pozici (úprava pásky).
     *
     * @p to je pozice bloku po přesunu (0 .. počet - 1; blok se vyjme a vloží
     * tak, aby měl číslo @p to). Blok si nese své údaje (i příznak neuloženo)
     * s sebou. Pauza na švu: blok 0x46 MZ800EMU-CMT s pauzou 0 před
     * novou pozicí a přesunutý blok, za kterým další blok následuje, dostanou
     * pauzu 1000 ms. Čítače, Modified, délky a kurzor jako
     * cmttmz_remove_blocks() (kurzor sleduje původní aktuální blok, pozice
     * v bloku 0).
     *
     * @param block_id Číslo přesouvaného bloku.
     * @param to Cílová pozice; @p to == @p block_id = úspěch beze změny (ani Modified, ani čítače).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky, s číslem nebo
     *         pozicí mimo rozsah nebo při selhání alokace - páska pak beze
     *         změny, text chyby vrací cmttmz_get_edit_error().
     * @pre Jako cmttmz_insert_blocks().
     */
    extern int cmttmz_move_block ( int block_id, int to );

    /**
     * @brief Je blok vložené pásky neuložený (není v souboru pásky)?
     *
     * Neuložený je blok připojený záznamem nebo vložený (cmttmz_insert_blocks())
     * po posledním otevření nebo uložení, a každý blok pásky bez souboru
     * (obraz snapshotu bez cesty). Přesun a mazání příznaky nemění; uložení
     * pásky a cmttmz_set_modified(0) je vynulují.
     *
     * @param block_id Index bloku.
     * @return 1 = neuložený; 0 = uložený, mimo rozsah nebo bez vložené pásky.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_block_is_unsaved ( int block_id );

    /**
     * @brief Nastaví příznak neuloženo bloku vložené pásky (obnovení ze snapshotu).
     *
     * Hodnota 1 nastaví i Modified (neuložený blok => páska Modified).
     * Čítač cmttmz_get_change_serial() zvedne jen při skutečné změně.
     *
     * @param block_id Index bloku. @param unsaved 1 = neuložený, 0 = uložený.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez vložené pásky nebo mimo rozsah (nic se nemění).
     * @pre Vlákno emulátoru pod device_state_lock() (příznak čte UI).
     */
    extern int cmttmz_set_block_unsaved ( int block_id, int unsaved );

    /**
     * @brief Kopie příznaků neuloženo všech bloků vložené pásky (doplněk obrazu cmttmz_save_image()).
     *
     * Obraz pásky příznaky nenese; kdo pásku z obrazu vrací (návrat pásky
     * uživatele po přehrání záznamu vstupů, replay_media.c), je obnoví
     * cmttmz_set_block_unsaved().
     *
     * @param[out] count Počet položek (= počet bloků pásky), 0 při NULL.
     * @return Pole jedné položky na blok (1 = neuložený; g_malloc, uvolní
     *         volající g_free()), NULL bez pásky nebo bez neuloženého bloku.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern uint8_t* cmttmz_get_unsaved_blocks ( uint32_t *count );

    /** @brief Velikost bufferu textu chyby úpravy pásky (včetně ukončovací nuly). */
#define CMTTMZ_EDIT_ERROR_SIZE 256

    /**
     * @brief Text chyby poslední úpravy pásky (anglicky), "" po úspěchu.
     *
     * Nastavují ho cmttmz_insert_blocks(), cmttmz_remove_blocks(),
     * cmttmz_move_block(), cmttmz_replace_block(), nástroje nad páskou
     * (cmt_tmz_analyze.h, cmt_tmz_meta.h: informace o pásce, převod
     * profilu) a obaly v cmt.c (cmt_tape_remove_blocks(),
     * cmt_tape_move_block(), cmt_tape_info_set(), cmt_tape_convert_block():
     * odmítnutí mimo STOP, při nahrávání, bez pásky) přes
     * cmttmz_set_edit_error(). Např. "Block 7 does not exist",
     * "The tape can be edited only when the deck is stopped".
     *
     * @return Statický buffer (nejvýš CMTTMZ_EDIT_ERROR_SIZE - 1 znaků); platí do další úpravy.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_edit_error ( void );

    /**
     * @brief Nastaví text chyby úpravy pásky (cmttmz_get_edit_error()).
     * @param text Text anglicky (NULL = ""), delší se ořízne.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern void cmttmz_set_edit_error ( const char *text );

    /**
     * @brief Věta varování o změně účinného taktu CPU bloků 0x46, anglicky (jediná definice).
     *
     * Používají ji import souboru (varování importu), mazání, přesun
     * a nahrazení bloků (cmttmz_edit_apply()), uplatnění analýzy
     * (cmttmz_warn_new_blocks_clock()) a zápis informací o pásce
     * (cmt_tmz_meta.c).
     */
#define CMTTMZ_CLOCK_WARNING "The tape clock setting now applies to different blocks; check their speed."

    /**
     * @brief Varování poslední úpravy pásky nebo nástroje nad páskou (anglicky, "; " mezi větami), "" = žádné.
     *
     * Jeden kanál pro všechny úpravy pásky i nástroje nad ní. Nastavuje ho:
     * - mazání, přesun a nahrazení bloků (cmttmz_edit_apply()): hraje-li po
     *   úpravě některý dosavadní blok 0x46 jiným účinným taktem CPU než před
     *   ní (přesunutý nebo smazaný blok 0x43, program přesunutý přes něj),
     *   věta CMTTMZ_CLOCK_WARNING (stejná jako varování importu); úpravu to
     *   nezastaví,
     * - uplatnění analýzy (cmttmz_analysis_apply()): nový blok souboru,
     *   který hraje jiným taktem než výchozím (blok 0x43 s jiným taktem
     *   před ním), tatáž věta (cmttmz_warn_new_blocks_clock()),
     * - nástroje cmt_tmz_meta.h: zápis informací o pásce (znaky mimo
     *   ISO 8859-1, změna taktu CPU - nástroj ji určí sám a varování svých
     *   dílčích úprav zahodí) a převod profilu (nastavení dosavadního
     *   profilu, která se nepřenesla).
     *
     * Kanál se vymaže na začátku každé úpravy v cmttmz_edit_apply() (i dílčí
     * úpravy nástroje, proto nástroje přidávají svá varování až po svých
     * úpravách), na začátku obou nástrojů cmt_tmz_meta.h, na začátku každé
     * úpravy pásky přes obaly v cmt.c (i odmítnuté; import do prázdného
     * decku; změna rychlosti bloku) a při vložení, vysunutí nebo nahrazení
     * pásky novou (cmttmz_eject(), vložení kontejneru), takže varování nikdy
     * nepatří jiné, pozdější operaci.
     *
     * @return Statický buffer (nejvýš CMTTMZ_WARNING_SIZE - 1 znaků); platí do další úpravy pásky.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_edit_warning ( void );

    /**
     * @brief Nastaví varování úprav pásky a nástrojů nad páskou (cmttmz_get_edit_warning()).
     * @param text Text anglicky (NULL = ""), delší se ořízne.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern void cmttmz_set_edit_warning ( const char *text );

    /**
     * @brief Účinný takt CPU, kterým player přehrává každý blok souboru (jedním průchodem).
     *
     * Stejné pravidlo jako player (tmz_player.c, větev 0x43: poslední blok
     * 0x43 aspoň 9 B před blokem, data + 4 + 1, 32 bitů LE). Bez bloku 0x43
     * (nebo s taktem 0) je to výchozí takt playeru MZCMT_FM_DEFAULT_TIMEBASE,
     * takže blok 0x43 s výchozím taktem se od pásky bez něj neliší.
     * Používá import (varování o taktu) a zápis informací
     * o pásce (cmt_tmz_meta.c).
     *
     * @param f Soubor. @param[out] out Pole f->block_count položek (takt v Hz, nikdy 0).
     * @pre Vlákno emulátoru nebo pod device_state_lock() (čte jen @p f).
     */
    extern void cmttmz_clock_contexts ( const st_TZX_FILE *f, uint32_t *out );

    /**
     * @brief Kontrola taktu CPU nových bloků souborů po úpravě (uplatnění analýzy).
     *
     * Stejné pravidlo jako import souboru pro importované bloky: nový blok
     * 0x46 vznikl pro výchozí takt playeru (MZCMT_FM_DEFAULT_TIMEBASE, jako
     * soubor bez bloku 0x43); hraje-li na pásce jiným účinným taktem
     * (cmttmz_clock_contexts(): blok 0x43 s jiným taktem před ním), připojí
     * se k varování úprav (cmttmz_get_edit_warning(), oddělovač "; ") věta
     * CMTTMZ_CLOCK_WARNING - jen jednou (už obsažená věta se neopakuje,
     * např. po nahrazení, které ji dalo pro dosavadní bloky). Ostatní bloky
     * rozsahu (0x18 a další) se nekontrolují: jejich signál na taktu
     * nezávisí. Pásku nemění.
     *
     * @param first Číslo prvního nového bloku. @param count Počet nových bloků
     *        (rozsah se omezí na pásku; 0 nebo prázdný rozsah = nic).
     * @post Kanál varování nese větu o taktu, liší-li se takt některého nového bloku 0x46
     *       od výchozího; jinak beze změny. Bez pásky nebo při selhání alokace beze změny.
     * @pre Vlákno emulátoru pod device_state_lock(), po úspěšné úpravě, která bloky vložila.
     */
    extern void cmttmz_warn_new_blocks_clock ( uint32_t first, uint32_t count );

    /**
     * @brief Je vložená páska změněná proti souboru (Modified)?
     *
     * Nastavuje cmttmz_append_blocks(), úpravy pásky (cmttmz_insert_blocks(),
     * cmttmz_remove_blocks(), cmttmz_move_block()), načtení obrazu
     * (cmttmz_open_image()) a cmttmz_set_block_unsaved(..., 1); nuluje
     * otevření, vysunutí, nová páska a cmttmz_set_modified(0) (úspěšné
     * uložení). Skutečnou změnu rychlosti bloku označí obaly v cmt.c
     * (cmt_tape_change_block_speed(), cmt_tape_set_block_cmt_speed(),
     * cmt_tape_set_block_pulses()) přes cmttmz_set_modified(1).
     *
     * @return 1 = změněná, 0 = ne nebo bez vložené pásky.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (příznak se mění
     *      jen pod zámkem: připojení záznamu, úpravy, uložení, otevření, vysunutí).
     */
    extern int cmttmz_is_modified ( void );

    /**
     * @brief Nastaví příznak Modified vložené pásky.
     *
     * Hodnota 0 (páska shodná se souborem) zároveň označí všechny bloky jako
     * uložené (st_CMTTMZ_BLOCK_UI::unsaved 0); hodnota 1 příznaky neuloženo
     * bloků nemění.
     *
     * @param modified 1 = změněná, 0 = shodná se souborem. Bez vložené pásky se nic nenastaví.
     * @pre Vlákno emulátoru.
     * @post Zvedne čítač cmttmz_get_change_serial().
     */
    extern void cmttmz_set_modified ( int modified );


    /**
     * @brief Stav délky bloku v údajích pro UI (st_CMTTMZ_BLOCK_UI::length_state).
     */
    typedef enum en_CMTTMZ_LENGTH_STATE {
        CMTTMZ_LENGTH_UNKNOWN = 0, /**< délka zatím nezjištěná (blok nikdy neotevřený a levně nespočítaný, změna rychlosti) */
        CMTTMZ_LENGTH_KNOWN,       /**< délka platí pro rychlost, kterou by blok teď hrál */
        CMTTMZ_LENGTH_NO_SIGNAL,   /**< blok signál nedá (řídicí blok, blok, který player odmítl, pauza 0); délka 0 */
    } en_CMTTMZ_LENGTH_STATE;


    /**
     * @brief Údaje jednoho bloku vložené pásky pro okno Virtual CMT (kopie, nic nevlastní).
     *
     * Doplňují index kontejneru o to, co index nenese: délku signálu bloku,
     * příznak "vznikl záznamem", příznak "od posledního uložení nebo otevření
     * neuložený", nepřehratelnost, údaje surového bloku 0x18 a jméno MZF
     * v původních kódech Sharp MZ ASCII (index nese jen převod do ASCII).
     *
     * @invariant length_state != CMTTMZ_LENGTH_KNOWN => length_s == 0.
     * @invariant mzf_name_len <= MZF_FILE_NAME_LENGTH; mzf_name_len > 0 jen u bloku s MZF.
     */
    typedef struct st_CMTTMZ_BLOCK_UI {
        en_CMTTMZ_LENGTH_STATE length_state; /**< stav délky */
        double length_s;          /**< délka signálu bloku v s včetně ticha za blokem (stream playeru); 0, není-li známá */
        int unplayable;           /**< 1 = player blok přehrát neumí (druh bloku, chyba při výpočtu délky, příznak přeskočení) */
        int recorded;             /**< 1 = blok připojil záznam (cmttmz_append_blocks()) v této relaci pásky */
        int imported;             /**< 1 = blok vložil import souboru (cmttmz_import_file_at()) v této relaci pásky; bloky analýzy záznamu, informací o pásce a převodu profilu ne; přesun ho nese, snapshot ho neobnoví */
        int unsaved;              /**< 1 = blok není v souboru pásky: připojený nebo vložený po posledním otevření nebo uložení, nebo páska bez souboru (cmttmz_block_is_unsaved()) */
        uint32_t csw_rate;        /**< blok 0x18: vzorkovací frekvence v Hz, jinak 0 */
        uint32_t csw_pulses;      /**< blok 0x18: počet pulzů (po dekompresi RLE), jinak 0 */
        uint8_t mzf_name[MZF_FILE_NAME_LENGTH]; /**< jméno MZF v kódech Sharp MZ ASCII (bez terminátoru 0Dh) */
        uint8_t mzf_name_len;     /**< počet platných bajtů mzf_name (0 = blok bez MZF nebo prázdné jméno) */
    } st_CMTTMZ_BLOCK_UI;


    /**
     * @brief Údaje bloku vložené pásky pro UI (délka, záznam, neuloženo, nepřehratelnost, CSW, jméno).
     *
     * Čte jen vedlejší pole rozšíření (ne data bloků v souboru, ta emu vlákno
     * při přehrávání přestavuje bez zámku).
     *
     * Odkud délka bloku pochází (délka = délka streamu, který dá
     * tmz_player_play_block() při GDGCLK_BASE z bloku sestaveného rychlostí,
     * kterou by blok teď hrál):
     * - otevření bloku (přímé, postupné při PLAY, změna rychlosti otevřeného
     *   bloku): délka jeho streamu, zadarmo;
     * - připojení záznamu a návrat na značku: jen dotčené bloky a jen levně
     *   (parametry bloku, průchod dat 0x18; bez generování signálu);
     * - zásah UI cmt_request_lengths() (cmttmz_lengths_update_budget()): levně,
     *   jinak generováním streamu, po krocích. Okno ho zařazuje jen ve STOP,
     *   takže délky nikdy neotevřených bloků MZ přibývají jen při zastaveném
     *   decku.
     * Otevření pásky žádnou délku nepočítá (kromě otevřeného bloku). Do té
     * doby má blok CMTTMZ_LENGTH_UNKNOWN. Délky nikdy neovlivní emulaci,
     * snapshot, otisk stroje ani záznam vstupů.
     *
     * @param block_id Index bloku v souboru.
     * @param[out] out Údaje (při chybě vynulované).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, mimo rozsah nebo s NULL.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (pole se
     *      realokují a uvolňují jen pod ním; jednotlivé hodnoty zapisuje emu
     *      vlákno i bez zámku, čtení je pak nanejvýš o snímek staré).
     */
    extern int cmttmz_get_block_ui ( int block_id, st_CMTTMZ_BLOCK_UI *out );

    /**
     * @brief Označí délky, které neplatí pro aktuální rychlost bloků, jako neznámé (nic nepočítá).
     *
     * Volá ji cmt.c po změně výchozí rychlosti a rychlosti bloku, až když
     * má otevřený blok nový stream (jeho délka je pak známá zadarmo). Jen
     * průchod vedlejším polem (bez generování signálu, bez průchodu daty
     * bloků). Při změně zvedne čítač cmttmz_get_change_serial(). Do
     * přehrávání (cmt_update_output(), cmt_write_data()) se nevolá.
     *
     * @pre Vlákno emulátoru pod device_state_lock() (zásahy GUI / MCP ho drží).
     */
    extern void cmttmz_lengths_invalidate ( void );

    /**
     * @brief Jeden krok doplnění neznámých délek bloků (zásah UI cmt_request_lengths()).
     *
     * Od začátku pásky každému bloku s neznámou nebo zastaralou délkou
     * spočítá délku levně (parametry bloku, průchod dat 0x18), jinak
     * generováním streamu z bloku sestaveného rychlostí, kterou by blok teď
     * hrál (blok s měnitelnou rychlostí sestavený jinak se sestaví do
     * dočasné kopie jako u exportu WAV; soubor, player ani otevřený blok se
     * nemění). Před každým blokem kontroluje rozpočet budget_us času
     * hostitele; aspoň jeden blok se spočítá vždy, je-li co (jeden velký blok
     * MZ tak může rozpočet překročit). Zbytek doplní další krok nebo otevření
     * bloku. Při změně zvedne čítač cmttmz_get_change_serial().
     *
     * @param budget_us Rozpočet v µs.
     * @pre Vlákno emulátoru pod device_state_lock() (zásah ho drží).
     */
    extern void cmttmz_lengths_update_budget ( int64_t budget_us );

    /** @brief Rozpočet času hostitele jednoho kroku výpočtu délek (cmt_request_lengths()) v µs. */
#define CMTTMZ_LENGTHS_STEP_US 20000

    /**
     * @brief Čítač změn údajů bloků, které nemění cmttmz_get_open_serial().
     *
     * Roste při doplnění délek, uložení pásky, změně Modified, změně
     * rychlosti bloku nebo výchozí rychlosti (cmttmz_note_change()),
     * označení bloku jako nepřehratelného a při každé změně čítače otevření.
     * UI podle dvojice (čítač otevření, čítač změn) pozná, že má kopii
     * seznamu bloků sestavit znovu.
     *
     * @return Hodnota čítače (jen roste, přetečení se neřeší).
     * @pre Libovolné vlákno (čtení skalární hodnoty).
     */
    extern unsigned cmttmz_get_change_serial ( void );

    /**
     * @brief Ohlásí změnu údajů bloků mimo rozšíření (rychlost bloku, výchozí rychlost).
     * @post cmttmz_get_change_serial() vrací vyšší hodnotu.
     * @pre Vlákno emulátoru.
     */
    extern void cmttmz_note_change ( void );

    /**
     * @brief Číslo otevřeného bloku bez čtení struktury bloku.
     *
     * Hodnotu zapisuje rozšíření při každé výměně otevřeného bloku (i při
     * postupném přehrávání bez zámku), čtení tedy nedereferencuje blok,
     * který emu vlákno mezitím uvolnilo.
     *
     * @return Index otevřeného bloku v souboru; -1 bez pásky nebo bez otevřeného bloku.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_get_open_block_id ( void );

    /**
     * @brief Formát zdroje, ze kterého vznikla vložená páska.
     * @return Zdroj importu otevřeného souboru; CMTTMZ_SOURCE_TMZ u nové pásky,
     *         pásky z obrazu snapshotu a bez pásky.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern en_CMTTMZ_SOURCE cmttmz_get_source ( void );


    /** @brief Velikost bufferu zprávy posledního záznamu (včetně ukončovací nuly). */
#define CMTTMZ_RECORD_MESSAGE_SIZE 512

    /**
     * @brief Nastaví zprávu posledního záznamu (anglicky) s nulovými počty bloků.
     *
     * Totéž co cmttmz_set_record_result ( text, 0, 0 ).
     *
     * @param text Text (NULL = prázdný); delší se ořízne na CMTTMZ_RECORD_MESSAGE_SIZE - 1 znaků.
     * @pre Vlákno emulátoru (cmt_stop() v RECORD).
     */
    extern void cmttmz_set_record_message ( const char *text );

    /**
     * @brief Nastaví výsledek posledního záznamu: zprávu a počty připojených bloků.
     *
     * Zvedne čítač záznamů (cmttmz_get_record_result()), podle kterého UI
     * pozná nový výsledek.
     *
     * @param text Zpráva (NULL = prázdná), delší se ořízne.
     * @param mzf Počet připojených bloků 0x46. @param raw Počet připojených bloků 0x18.
     * @pre Vlákno emulátoru pod device_state_lock() (čte UI).
     */
    extern void cmttmz_set_record_result ( const char *text, uint32_t mzf, uint32_t raw );

    /**
     * @brief Nastaví výsledek zahozeného záznamu: zprávu, nulové počty a příznak zahození.
     *
     * Jako cmttmz_set_record_result ( text, 0, 0 ), navíc
     * cmttmz_get_record_discarded() vrací 1 (UI podle něj volí nadpis
     * oznámení). Příznak smaže další cmttmz_set_record_result().
     *
     * @param text Zpráva (NULL = prázdná), delší se ořízne.
     * @pre Vlákno emulátoru pod device_state_lock() (čte UI).
     */
    extern void cmttmz_set_record_discarded ( const char *text );

    /**
     * @brief Byl poslední záznam zahozený (vysunutí, otevření jiné pásky, snapshot, konec)?
     * @return 1 = zahozený (cmttmz_set_record_discarded()), 0 = dokončený STOPem nebo žádný.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    extern int cmttmz_get_record_discarded ( void );

    /**
     * @brief Počty bloků posledního záznamu a čítač dokončených záznamů.
     * @param[out] mzf Bloky 0x46 (smí být NULL). @param[out] raw Bloky 0x18 (smí být NULL).
     * @param[out] serial Čítač (smí být NULL); roste s každým cmttmz_set_record_result().
     * @pre Vlákno emulátoru nebo pod device_state_lock().
     */
    extern void cmttmz_get_record_result ( uint32_t *mzf, uint32_t *raw, unsigned *serial );

    /**
     * @brief Zpráva posledního záznamu pro log, UI a MCP.
     *
     * Např. `Recorded "NAME", 1024 B`, `Recording could not be decoded,
     * stored as a raw block`, s předponou `Recording stopped: length limit
     * reached; ` při záznamu ukončeném mezí bufferu. Přežije vysunutí pásky,
     * přepíše ji až další záznam.
     *
     * @return Statický buffer rozšíření (prázdný = žádný záznam). Platí do
     *         dalšího záznamu.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_record_message ( void );

    /**
     * @brief Výsledek posledního nahrávání od startu emulátoru (resource MCP periph/cmt/tape, last_recording).
     *
     * Nahrávání, které skončilo samo (automatické ukončení, mez), MCP
     * emu_cmt_stop nevrátí; agent ho přečte odsud. Platí výsledek
     * posledního dokončeného i zahozeného nahrávání (zpráva
     * cmttmz_get_record_message()); vysunutí ani vložení pásky ho nesmaže
     * (otevření jiné pásky, nová páska a načtení snapshotu nahrávání
     * nejdřív zahodí a pak pásku vloží). Nové poznáš podle čítače @p serial.
     *
     * @param[out] serial Čítač výsledků (cmttmz_get_record_result()), smí být NULL.
     * @param[out] mzf Připojené bloky 0x46 (smí být NULL). @param[out] raw Připojené bloky 0x18 (smí být NULL).
     * @param[out] discarded 1 = nahrávání zahozené (smí být NULL).
     * @return 1 = takový výsledek je (výstupy vyplněné); 0 = od startu
     *         emulátoru žádné nahrávání neskončilo (výstupy nulové).
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern int cmttmz_get_last_recording ( unsigned *serial, uint32_t *mzf, uint32_t *raw, int *discarded );


    /** @brief Velikost bufferu zprávy uložení pásky a exportu bloku (včetně ukončovací nuly). */
#define CMTTMZ_SAVE_MESSAGE_SIZE 512

    /**
     * @brief Uloží vloženou pásku do souboru.
     *
     * Formát podle přípony cíle (bez ohledu na velikost písmen):
     * - `.tmz`, `.tzx`: tzx_save() celého souboru (hlavička tak, jak páska
     *   je; páska TMZ uložená s příponou .tzx zůstane TMZ a při blocích
     *   0x40 až 0x4F zpráva nese varování "Tape contains MZ blocks; file
     *   saved in TMZ format with .tzx extension"),
     * - `.mzt`: všechny bloky musí nést MZF (tmz_mzt_block_info()), jinak
     *   chyba "Tape contains blocks that cannot be stored as MZT". Hlavičky
     *   a těla MZF bajt po bajtu z bloků; před souborem s jinou rychlostí
     *   než předchozí (začátek 1x) hlavička CMTSPEED UniCMT: blok 0x46
     *   MZ800EMU-CMT s rychlostí SET (vlastní pulzy v celých µs, poměr jako
     *   floor ( 1x / dělitel )), DEFAULT = 1x; jiný blok s MZF rychlostí ze
     *   souboru. Jako tmz_mzt_export(): první soubor s rychlostí jinou než
     *   1x, blok 2:1 CPM a blok bez struktury ROM se zapíší bez hlavičky
     *   CMTSPEED a uložení projde s varováním "Speed of block N cannot be
     *   stored in MZT" (cmttmz_get_save_warning()),
     * - `.mzf`, `.m12`: jen páska s jediným blokem s MZF (jinak chyba "Only
     *   a tape with a single MZF block can be saved as MZF"),
     * - jiná přípona = chyba.
     *
     * Bloky s měnitelnou rychlostí sestavené jinou rychlostí, než mají hrát
     * (rychlost bloku změněná v indexu, výchozí rychlost decku změněná mimo
     * STOP), se před zápisem .tmz a .tzx v souboru pásky přestaví, takže
     * znovu otevřený soubor hraje stejně jako páska v paměti (i bloky DEFAULT
     * výchozí rychlostí decku, kterou hrají). Když některý přestavět nejde,
     * uložení selže ("Block N cannot be rebuilt at its speed") a soubor pásky
     * se nemění. .mzt přestavbu nepotřebuje: nese bajty MZF z bloku
     * a rychlost z indexu (cmttmz_mzt_block_pulses()), ne signál.
     *
     * Zápis je atomický: obsah vznikne v paměti, zapíše se do dočasného
     * souboru ve stejném adresáři a ten g_rename() nahradí cíl. Při jakékoli
     * chybě zůstane původní soubor celý a cesta, Modified i příznaky
     * neuloženo pásky beze změny; selže-li zápis až po přestavbě (.tmz,
     * .tzx), bloky zůstanou v paměti přestavěné rychlostí, kterou hrají
     * (přehrávání se nemění). Po úspěchu je cesta pásky
     * (kontejner, g_cmt.last_filename a jméno pro UI) @p path, Modified 0
     * a všechny bloky uložené (st_CMTTMZ_BLOCK_UI::unsaved 0).
     * Výsledek (anglicky) vrací cmttmz_get_save_message(), jde i do logu.
     *
     * @param path Cíl (UTF-8), nebo NULL = dosavadní cesta pásky (chyba "Tape
     *        has no file name", když páska cestu nemá).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, bez cesty, s nepodporovanou
     *         příponou, s obsahem, který formát neunese, nebo při chybě zápisu.
     * @pre Vlákno emulátoru; mění cestu kontejneru (čte UI), proto pod
     *      device_state_lock() (cmt_request_save(), dbgapi), u testů bez UI
     *      volné.
     */
    extern int cmttmz_save ( const char *path );

    /**
     * @brief Exportuje jeden blok vložené pásky do souboru.
     *
     * `.mzf`, `.m12`: MZF bloku (cmttmz_block_get_mzf(), mzf_save()), zápis
     * atomicky jako cmttmz_save(); blok bez MZF = chyba. `.wav`: signál
     * bloku z tmz_player_play_block() při 44 100 Hz (blok s měnitelnou
     * rychlostí se pro export sestaví rychlostí, kterou by teď hrál, kopie
     * mimo soubor pásky), cmt_stream_save_wav(); otevřený blok, stav playeru
     * ani soubor pásky se nemění. Modified ani cesta pásky se nemění.
     *
     * @param block_id Index bloku v souboru. @param path Cíl (UTF-8).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, mimo rozsah, s nepodporovanou
     *         příponou, blokem bez MZF / signálu nebo při chybě zápisu
     *         (text v cmttmz_get_save_message()).
     * @pre Vlákno emulátoru.
     */
    extern int cmttmz_export_block ( int block_id, const char *path );

    /**
     * @brief Vloží všechny bloky souboru před blok @p at vložené pásky (import).
     *
     * Soubor (MZF, M12, MZT, TAP, WAV, TMZ, TZX) se převede jako při otevření
     * (cmttmz_import_file_speeds() s volbami cmttmz_open_opts(): výchozí
     * rychlost a pulzy g_cmt, normální polarita WAV) a jeho bloky se vloží
     * společnou údržbou úprav (jako cmttmz_insert_blocks(): čítač výměn pásky,
     * kurzor, bloky neuložené, šev, hlavička TMZ 1.1). Rychlosti bloků jsou z importu
     * (MZF DEFAULT, MZT podle CMTSPEED, TAP FIXED, TMZ 0x46 MZ800EMU-CMT
     * s PROFILE_ARGS FIXED). Blok 0x18 z importu WAV má v indexu typ
     * CMTEXT_BLOCK_TYPE_WAV (přepínač polarity na něj působí, jako po
     * otevření WAV); hlavička importovaného souboru (TZX / TMZ, verze) se
     * nepřebírá. Varování importu, počet importovaných bloků, které player
     * přehrát neumí, a změnu taktu CPU vrací cmttmz_get_import_warning().
     *
     * Takt CPU (blok 0x43 platí pro bloky za ním): liší-li se po importu takt
     * některého dosavadního bloku od taktu před importem, nebo takt
     * importovaného bloku od taktu, který měl ve vlastním souboru, import
     * projde a varování dostane větu "The tape clock setting now applies to
     * different blocks; check their speed.".
     *
     * Bez vložené pásky (kontejner rozšíření NULL) se soubor nejdřív převede
     * a teprve po úspěšném převodu se vloží nová prázdná páska
     * (cmttmz_new_tape()), do které se importuje (@p at se bere jako 0);
     * selže-li převod, páska nevznikne, selže-li až vložení, nová páska se
     * zase vysune. Nastavení g_cmt (ext, jméno) dělá volající
     * (cmt_tape_import_file()).
     *
     * @param path Cesta k souboru (UTF-8).
     * @param at Pozice 0 .. počet bloků (počet = na konec).
     * @param[out] imported Počet vložených bloků (smí být NULL; 0 při chybě).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez cesty, s @p at mimo rozsah,
     *         u souboru, který nejde přečíst nebo převést (chybí,
     *         nepodporovaná přípona, poškozený), u souboru bez bloků a při
     *         selhání alokace - páska pak beze změny. Text chyby vrací
     *         cmttmz_get_edit_error() ("Can't import <cesta>: <důvod>"; u dlouhé
     *         cesty se zkrátí cesta, ne důvod), důvod samotný
     *         i cmttmz_get_prepare_error() (jako otevření).
     * @pre Vlákno emulátoru pod device_state_lock() (synchronně, velký WAV
     *      zastaví emulaci i okno jako otevření), transport ve STOP (ověřuje
     *      volající, cmt_tape_import_file()).
     */
    extern int cmttmz_import_file_at ( const char *path, int at, uint32_t *imported );

    /**
     * @brief Varování posledního cmttmz_import_file_at() (anglicky, "; " mezi varováními).
     * @return Statický buffer; "" = bez varování (i po chybě). Platí do dalšího importu.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_import_warning ( void );

    /**
     * @brief Exportuje celou vloženou pásku do WAV tak, jak ji přehraje player.
     *
     * 44 100 Hz, 8 bit mono (cmt_stream_save_wav()). Pásku projde player od
     * bloku 0 (tmz_player_play_next_budget()): bloky a pauzy za nimi, smyčky,
     * skoky, volání; zastavení pásky (Pause 0, 0x2A) export nepřeruší, další
     * blok následuje hned. Bloky s měnitelnou rychlostí se exportují
     * rychlostí, kterou mají hrát (dočasná kopie, jako cmttmz_export_block());
     * soubor pásky, otevřený blok ani player se nemění; blok, který nejde
     * přestavět rychlostí, kterou hraje, je chyba (export se neprovede).
     * Blok, který player přehrát neumí, se přeskočí s varováním, každý jednou
     * i ve smyčce ("Blocks that cannot be played were skipped: 3, 5"; blok,
     * jehož signál by při 44 100 Hz čtecí rutina nepřečetla,
     * TMZ_PLAYER_ERROR_UNREADABLE: "Blocks that cannot be rendered at 44100
     * Hz were skipped: ..."); smyčka řídicích bloků bez signálu export
     * ukončí s varováním. Polarita přepínače se na bloky WAV neuplatní (jako
     * export bloku; v emulátoru se na ně uplatní). Délka ve zprávě je
     * v desetinách sekundy s desetinnou tečkou bez ohledu na locale. Zápis je
     * atomický (dočasný soubor a g_rename(), existující cíl při chybě
     * zůstane).
     *
     * @param path Cíl s příponou .wav (UTF-8).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, s jinou příponou, u prázdné
     *         pásky ("The tape is empty"), u pásky bez signálu, nad mez
     *         CMT_VSTREAM_MAX_BLOCK_SECONDS (4 h, "The tape is longer than 4
     *         hours, not exported"), při selhání alokace nebo zápisu. Zpráva
     *         v cmttmz_get_export_message() ("Tape exported: <cesta>, <s> s"
     *         a varování v závorce), varování i v cmttmz_get_export_warning();
     *         zprávu uložení (cmttmz_get_save_message()) export nemění.
     * @pre Vlákno emulátoru pod device_state_lock() (zpráva čte UI pod týmž
     *      zámkem; zásah fronty i dbgapi ho drží). Celý signál se drží
     *      v paměti a zapisuje najednou: emulace i okno po dobu exportu stojí
     *      (4 h ~ 635 MB vzorků + 80 MB bitstreamu, výpočet, [neměřeno]).
     */
    extern int cmttmz_export_tape_wav ( const char *path );

    /**
     * @brief Zpráva posledního cmttmz_export_tape_wav() (anglicky).
     * @return Statický buffer rozšíření; platí do dalšího exportu celé pásky.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_export_message ( void );

    /**
     * @brief Varování posledního úspěšného cmttmz_export_tape_wav() (anglicky, "; " mezi varováními).
     * @return Statický buffer; prázdný = bez varování (i po neúspěchu).
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_export_warning ( void );

    /**
     * @brief Zpráva posledního cmttmz_save() nebo cmttmz_export_block() (anglicky; export celé pásky má vlastní, cmttmz_get_export_message()).
     * @return Statický buffer rozšíření; platí do dalšího uložení nebo exportu.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_save_message ( void );

    /**
     * @brief Varování posledního úspěšného cmttmz_save() (anglicky, "; " mezi varováními).
     *
     * Např. "Speed of block 0 cannot be stored in MZT" (MZT: první soubor
     * jiný než 1x, blok 2:1 CPM, blok bez struktury ROM - zapíší se bez
     * hlavičky CMTSPEED) nebo "Tape contains MZ blocks; file saved in TMZ
     * format with .tzx extension". Text je i v cmttmz_get_save_message().
     *
     * @return Statický buffer; prázdný = bez varování (i po neúspěchu
     *         a exportu bloku). Platí do dalšího uložení nebo exportu.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    extern const char* cmttmz_get_save_warning ( void );

#ifdef MZTEST_HEADLESS
    /**
     * @brief Jen pro testy: atomický zápis selže po zápisu dočasného souboru, před nahrazením cíle.
     * @param fail 1 = selhat, 0 = normálně.
     */
    extern void cmttmz_test_fail_before_replace ( int fail );

    /**
     * @brief Jen pro testy: mez délky cmttmz_export_tape_wav() v sekundách (0 = výchozí 4 h).
     * @param seconds Mez.
     */
    extern void cmttmz_test_set_export_limit ( uint32_t seconds );

    /**
     * @brief Jen pro testy: funkce volaná v cmttmz_append_blocks() hned po výměně indexu (počet bloků už nový).
     * @param hook Funkce, nebo NULL.
     */
    extern void cmttmz_test_set_append_hook ( void ( *hook ) ( void ) );

    /**
     * @brief Jen pro testy: záznam vedlejšího pole rychlostí a příznak skip bloku.
     * @param block_id Blok. @param[out] speed Záznam (smí být NULL). @param[out] skip Příznak (smí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky nebo mimo rozsah.
     */
    extern int cmttmz_test_get_side ( int block_id, st_CMTTMZ_BLOCK_SPEED *speed, int *skip );

    /**
     * @brief Jen pro testy: skutečné velikosti alokací vedlejších polí a indexu v bajtech.
     *
     * Čte velikost z alokátoru (_msize() na Windows, malloc_usable_size()
     * s glibc), ne z počtu bloků, takže test pozná chybějící realloc() při
     * připojení bloků deterministicky (_msize() vrací přesně požadovanou
     * velikost; malloc_usable_size() může být větší).
     *
     * @param[out] speeds_bytes Pole rychlostí (smí být NULL). @param[out] skip_bytes Pole skip (smí být NULL).
     * @param[out] index_bytes Pole indexu kontejneru (smí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky nebo na platformě, kde velikost alokace zjistit nejde.
     */
    extern int cmttmz_test_get_side_alloc ( long long *speeds_bytes, long long *skip_bytes, long long *index_bytes );

    /**
     * @brief Jen pro testy: kolik streamů vygenerovala drahá cesta výpočtu délek od startu procesu.
     *
     * Počítá jen streamy pro délky (zásah cmt_request_lengths(),
     * cmttmz_test_length_paths()), ne stream otevřeného bloku. Test tak
     * ověří, že otevření pásky žádný průchod délek nedělá.
     *
     * @return Počet (jen roste).
     */
    extern unsigned cmttmz_test_get_length_streams ( void );

    /**
     * @brief Jen pro testy: délka bloku levnou cestou a generováním streamu (údaje bloků se nemění).
     *
     * @param block_id Blok.
     * @param[out] cheap_known 1 = levně spočítaná délka, 0 = levně zjištěno "bez signálu", -1 = levně nejde.
     * @param[out] cheap_s Levná délka v s (0 mimo cheap_known 1).
     * @param[out] stream_known 1 = stream vznikl, 0 = blok signál nedá.
     * @param[out] stream_s Délka streamu v s.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky nebo mimo rozsah.
     * @pre Jako cmttmz_lengths_update_budget(); zvedne čítač cmttmz_test_get_length_streams().
     */
    extern int cmttmz_test_length_paths ( int block_id, int *cheap_known, double *cheap_s, int *stream_known, double *stream_s );
#endif

#ifdef __cplusplus
}
#endif

#endif /* CMT_TMZ_H */
