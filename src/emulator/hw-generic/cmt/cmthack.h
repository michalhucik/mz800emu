/* 
 * File:   cmthack.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 2. července 2015, 20:50
 * 
 * 
 * ----------------------------- License -------------------------------------
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * ---------------------------------------------------------------------------
 */

#ifndef CMTHACK_H
#define CMTHACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "libs/generic_driver/generic_driver.h"

    /**
     * @brief Stav CMT hacku (patch rutin kazety v ROM a jeho volby).
     *
     * Invarianty: verify_patch_installed => save_patch_installed;
     * save_patch_installed => load_patch_installed a deck mimo RECORD
     * (patch zápisu je v ROM jen se zapnutým patchem čtení na ROM
     * s výchozím patchem, se zapnutým direct_save a bez nahrávání,
     * cmthack_save_patch_sync()). Mění jen vlákno emulátoru (nebo
     * inicializace) pod device_state_lock(); UI čte pod týmž zámkem.
     */
    typedef struct st_CMTHACK {
        st_HANDLER mzf_handler;          /**< rozčtený soubor MZF z dialogu (hlavička načtená, tělo čeká) */
        char *last_filename;             /**< poslední cesta z dialogu (výchozí pro další dialog; g_free) */
        unsigned load_patch_installed;   /**< 1 = patch čtecích rutin ROM (RHEAD, RDATA) je zapnutý */
        unsigned fix_fname_terminator;   /**< 1 = doplnit chybějící terminátor jména při načtení */
        unsigned save_patch_installed;   /**< 1 = patch zápisových rutin ROM (WHEAD, WDATA -> port 03h) je teď v ROM */
        unsigned verify_patch_installed; /**< 1 = patch rutiny VERFY (port 04h) je teď v ROM */
        unsigned direct_save;            /**< nastavení `[CMTHACK] direct_save`: 1 = s hackem a mimo RECORD dávat i patch zápisu */
    } st_CMTHACK;

    extern st_CMTHACK g_cmthack;

    extern void cmthack_reinstall_rom_patch ( void );
    extern void cmthack_load_rom_patch ( unsigned enabled );

    /**
     * @brief Podporuje vybraná ROM CMT hack (jde ROM patch nainstalovat)?
     *
     * Stejná podmínka, za které cmthack_mzarch_load_rom_patch() patch
     * nainstaluje: ROM nemá příznak ROM_CONFIG_FLAG_CMTHACK_INCOMP (MZ-800
     * Willy, MZ-1500 MZ-NEWMON) a uživatelská ROM nemá typ hacku
     * ROM_CMTHACK_DISABLED. Jinak cmthack_load_rom_patch(1) nic neudělá.
     * Bez vedlejších efektů.
     *
     * @return true = hack jde zapnout; false = vybraná ROM ho nepodporuje.
     * @pre Vlákno emulátoru (nebo inicializace); čte g_rom.
     */
    extern bool cmthack_rom_supported ( void );

    /**
     * @brief Vypne CMT hack jen pro aktuální běh emulátoru (INI hodnota zůstane).
     *
     * Odinstaluje ROM patch (cmthack_load_rom_patch(0)) a připne INI hodnotu
     * `[CMTHACK] enable` na stav před vypnutím (cfgelement_pin_save_value),
     * takže se při ukončení do INI zapíše původní hodnota. Když uživatel
     * hack během běhu sám zapne nebo vypne, připnutí se zruší a uloží se
     * jeho volba. Používá CLI volba --cmt (cmt_cli.c).
     *
     * @pre Volá se po cmthack_init() (INI element je zaregistrovaný), na EMU
     *      vlákně nebo před jeho rozběhem.
     * @post CMTHACK_TEST_IS_INSTALLED == 0. Byl-li hack před voláním vypnutý,
     *       nic se nemění (ani připnutí).
     */
    extern void cmthack_disable_for_session ( void );

    // Tyto funkce jsou pro mzarch specificke

    /**
     * @brief Dá znovu patch hacku do ROM po instalaci dat ROM (rom_reinstall()), je-li hack zapnutý.
     *
     * Implementace v mz{700,800,1500}_cmthack.c. ROM s výchozím patchem
     * (ne INCOMP, uživatelská ROM jen s typem hacku DEFAULT) se zapnutým
     * hackem dostane patch čtení a patch zápisu se srovná
     * (cmthack_save_patch_sync(): podle direct_save a stavu decku); u ROM
     * bez výchozího patche v ROM patch zápisu není.
     *
     * @pre Vlákno emulátoru nebo inicializace, data ROM právě nainstalovaná.
     */
    extern void cmthack_mzarch_reinstall_rom_patch ( void );

    /**
     * @brief Zapne nebo vypne patch hacku v ROM (implementace v mz{700,800,1500}_cmthack.c).
     *
     * ROM s výchozím patchem: zapnutí zapíše patch čtení (RHEAD, RDATA)
     * a srovná patch zápisu (cmthack_save_patch_sync()), vypnutí vrátí
     * původní bajty obou. Uživatelská ROM s vlastní ROM hacku
     * (CUSTOM): jen příznak a nová instalace ROM (rom_reinstall()), patch
     * zápisu se nedává. ROM INCOMP: nic.
     *
     * @param enabled 1 = zapnout, 0 = vypnout.
     * @pre Vlákno emulátoru nebo inicializace.
     */
    extern void cmthack_mzarch_load_rom_patch ( unsigned enabled );


/** @brief Část patche zápisu: WHEAD 0436h a WDATA 0475h (past OUT 03h). Bit masky cmthack_save_patch_signature(). */
#define CMTHACK_SAVE_PART_SAVE   0x01u
/** @brief Část patche zápisu: VERFY 0588h (past OUT 04h). Bit masky cmthack_save_patch_signature(). */
#define CMTHACK_SAVE_PART_VERIFY 0x02u

    /**
     * @brief Které části patche zápisu jdou na obraz dolní ROM (kontrola signatury).
     *
     * Kontroluje bajty dolní ROM 0000h-0FFFh tak, jak je v ROM výrobce
     * (původní obraz, ne obsah paměti ROM s patchem):
     * - zápis (CMTHACK_SAVE_PART_SAVE): přesně 0436h-0443h, s maskou 0444h
     *   (`CD ?? ?? CD ?? ?? 38 ??`), přesně 044Ch-0450h, s maskou 045Eh
     *   (`CD ?? ?? CD ?? ?? C3 ?? ??`), přesně 0475h-0489h;
     * - ověření (CMTHACK_SAVE_PART_VERIFY): přesně 0588h-059Ah a 059Eh-05ACh.
     * Bajty jsou ve všech ROM, které hack podporují, stejné (MZ-800 1Z-013B
     * a JSS, MZ-700 13A a 9B, MZ-1500); maskované jsou cíle CALL, které se
     * mezi ROM liší, a cíl JR C (JSS 1.x ho má jiný).
     *
     * @param rom Obraz dolní ROM, aspoň 4096 bajtů.
     * @return Bitová maska CMTHACK_SAVE_PART_*; 0 = žádná část nesedí.
     * @par Vedlejší efekty Žádné.
     */
    extern unsigned cmthack_save_patch_signature ( const uint8_t *rom );

    /**
     * @brief Zapíše patch zápisu do paměti ROM podle signatury původního obrazu.
     *
     * Části, jejichž signatura v @p orig sedí, zapíše do @p rom; ověření
     * jen spolu se zápisem (bez patche zápisu by ověření nemělo s čím
     * srovnávat).
     *
     * Zápis: `CALL @CHECK` na 0444h zůstává (ROM tak spočítá kontrolní
     * součet do MGCRC / MGCRCV jako při skutečném uložení), 0447h-044Bh
     * (`CALL @MGON; JR C`) = 5x NOP, `045Eh: D3 03` (OUT (03h),A)
     * a 0460h-0463h čtyři bajty výplně, které vyrovnají XOR celé části
     * zápisu (provedou se při každém uložení); původní `JP CMTEND` na
     * 0464h zůstává. Ověření: `059Eh: D3 04` (OUT (04h),A) a 11 bajtů
     * výplně do 05AAh; původní `JR CMTEND` na 05ABh zůstává. Výplň jsou NOP
     * a nejvýš čtyři neškodné jednobajtové instrukce (LD r,r' s cílem B až
     * L, EX DE,HL): CMTEND obnoví BC, DE i HL ze zásobníku, A a příznaky
     * nesou výsledek pasti a výplň je nemění.
     *
     * Všechny změněné bajty kromě obou OUT a jejich operandů jsou neškodné
     * jednobajtové instrukce. Změna patche, zatímco CPU je uvnitř rutiny:
     *
     * - Patch se nainstaluje, zatímco CPU je uvnitř původní rutiny
     *   (nahrávání skončilo během uložení nebo ověření): návratové adresy
     *   0447h, 044Ah, 0461h, 0464h, 05A1h, 05A6h a 05ABh padnou do patche
     *   a rutina doběhne do CMTEND bez pádu stroje. Z 0447h a 044Ah vede
     *   cesta přes 044Ch-045Dh na OUT (03h): past pak běží s registry
     *   přerušeného volání (E, HL, BC jsou ještě ty z WHEAD / WDATA), takže
     *   hlavičku nebo tělo uloží rovnou - to je přijatelné. Návrat na 0461h
     *   (za @WTMRK u WDATA) vrátí OK a tělo není nikde. Přerušené původní
     *   ověření, které se vrátí na 05A1h nebo 05A6h, přeskočí @VBLOK
     *   a CMTEND vrátí AF z @MGON / @RTMRK (typicky CY = 0): VERIFY ohlásí
     *   OK bez porovnání.
     * - Patch se odstraní (vstup do RECORD, vypnutí volby nebo hacku,
     *   výměna ROM), zatímco CPU je uvnitř jeho bajtů:
     *   cmthack_save_patch_sync() při namapované dolní ROM přesune PC na
     *   hranici původní instrukce (0448h-044Bh -> 0447h, původní
     *   `CALL @MGON` pak proběhne a uložení pokračuje jako signálové;
     *   0460h-0463h -> 0464h a 05A0h-05AAh -> 05ABh, past už proběhla
     *   a výsledek v AF zůstává). PC na 0447h, 045Eh a 059Eh se nemění
     *   (proběhne původní instrukce). Uložení přerušené mezi výpisem
     *   "WRITING" (044Ch-045Dh) a pastí pokračuje původním kódem jako
     *   signálové uložení (bez @MGON; s deckem v RECORD a stojícím motorem
     *   se nenahraje nic).
     *
     * XOR změněných bajtů každé části se rovná XOR původních bajtů, takže
     * XOR celé dolní ROM 0000h-0FFFh se patchem nezmění (JSS kontroluje XOR
     * dolní a horní ROM, viz mz800_cmthack.c).
     *
     * @param rom Paměť dolní ROM (cíl), aspoň 4096 bajtů.
     * @param orig Původní obraz téže ROM (zdroj signatury), aspoň 4096 bajtů.
     * @return Bitová maska zapsaných částí (CMTHACK_SAVE_PART_*), 0 = nic.
     * @post Bajty mimo rozsahy 0447h-044Bh, 045Eh-0463h a 059Eh-05AAh se nemění.
     */
    extern unsigned cmthack_save_patch_install ( uint8_t *rom, const uint8_t *orig );

    /**
     * @brief Vrátí do paměti ROM původní bajty rozsahů patche zápisu.
     *
     * Kopíruje z @p orig přesně rozsahy, které píše
     * cmthack_save_patch_install() (0447h-044Bh, 045Eh-0463h, 059Eh-05AAh),
     * bez ohledu na to, zda tam patch je.
     *
     * @param rom Paměť dolní ROM, aspoň 4096 bajtů.
     * @param orig Původní obraz téže ROM, aspoň 4096 bajtů.
     */
    extern void cmthack_save_patch_uninstall ( uint8_t *rom, const uint8_t *orig );

    /**
     * @brief Srovná patch zápisu v ROM stroje (g_memory.ROM) s tím, co má platit (jediný vstup).
     *
     * Chtěný stav spočítá sama: patch zápisu má být v ROM, právě když je
     * patch čtení zapnutý (CMTHACK_TEST_IS_INSTALLED), ROM bere výchozí
     * patch (ne INCOMP, uživatelská ROM jen s typem DEFAULT), nastavení
     * direct_save je zapnuté a deck není v RECORD (Record = nahrát signál
     * i rutinami ROM). Pak zapíše patch podle signatury g_rom.mz700rom
     * (cmthack_save_patch_install()), jinak vrátí původní bajty
     * (cmthack_save_patch_uninstall()); nastaví save_patch_installed
     * a verify_patch_installed podle obsahu ROM. Když patch v ROM není,
     * zapomene hlavičku WHEAD bez těla (vstup do RECORD, vypnutí volby).
     *
     * Volá se při každé změně vstupů: zapnutí, vypnutí a nové instalaci
     * patche čtení (tři cmthack_mzarch_*, rom.c), změně direct_save
     * (cmthack_direct_save_set(), propagace INI) a změně transportu do
     * a z RECORD (cmt_record(), cmt_stop(),
     * cmt_reset_transport_without_tape(), cmt_sanitize_state(), načtení
     * snapshotu v snap_cmt.c).
     *
     * Odstraňuje-li patch, zatímco PC hlavního Z80 leží uvnitř jeho bajtů
     * mimo hranici původní instrukce a na 0000h je namapovaná dolní ROM,
     * přesune PC (viz cmthack_save_patch_install()).
     *
     * Zámek: ROM a příznaky zapisuje pod device_state_lock(); vezme si
     * ho, nedrží-li ho volající. Bez patche a bez potřeby změny se nic
     * nezamyká ani nepřepisuje.
     *
     * @pre Vlákno emulátoru nebo inicializace (g_rom nastavené).
     * @post save_patch_installed a verify_patch_installed odpovídají obsahu
     *       ROM; v RECORD jsou 0.
     */
    extern void cmthack_save_patch_sync ( void );

    /**
     * @brief Srovná patch zápisu se stavem stroje právě načteným ze snapshotu, bez přesunu PC.
     *
     * Jako cmthack_save_patch_sync(), ale PC nemění. Při načtení snapshotu
     * je PC už obnovené ze snapshotu, kdežto příznaky patche jsou ještě ze
     * stroje před načtením: PC patří ROM, jakou měl stroj snapshotu, a ta
     * odpovídá obnovenému stavu decku. Přesun by posunul PC stojící na
     * skutečné hranici původní instrukce (např. 0461h `CALL @WBLOK`
     * snapshotu pořízeného při nahrávání) a uložení by přišlo o tělo.
     *
     * @pre Vlákno emulátoru, transport a ROM obnovené ze snapshotu.
     * @post Jako cmthack_save_patch_sync(); PC beze změny.
     */
    extern void cmthack_save_patch_sync_loaded_state ( void );

    /**
     * @brief Podporuje vybraná ROM přímé ukládání (patch zápisu jde nainstalovat)?
     *
     * ROM bere výchozí patch (není INCOMP, uživatelská ROM jen s typem
     * hacku DEFAULT) a signatura zápisu původního obrazu sedí.
     *
     * @return true = Direct Save jde s touto ROM zapnout.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (čte g_rom).
     */
    extern bool cmthack_save_rom_supported ( void );

    /**
     * @brief Nastaví Direct Save (nastavení `[CMTHACK] direct_save`) a srovná patch zápisu v ROM.
     *
     * Zapíše hodnotu do g_cmthack.direct_save i do INI elementu (settings_get
     * a uložení INI vidí totéž) a srovná patch zápisu
     * (cmthack_save_patch_sync(); bez hacku nebo v RECORD jen zapamatuje
     * volbu). Stav decku nekontroluje: to dělají volající (zásah menu
     * cmt_request_set_direct_save() a MCP settings_set, jen ve STOP).
     *
     * @param enabled 1 = zapnout, 0 = vypnout.
     * @pre Vlákno emulátoru (zásah fronty zařízení), nebo inicializace.
     * @post g_cmthack.direct_save == enabled; save_patch_installed odpovídá ROM.
     */
    extern void cmthack_direct_save_set ( unsigned enabled );

    /**
     * @brief Past ROM "ulož blok" (OUT 03h): WHEAD (E = CCh) nebo WDATA (E = 53h).
     *
     * E = CCh: 128 bajtů na HL se drží jako hlavička bez těla; s fsize 0
     * se soubor uloží hned s prázdným tělem (WDATA by se pak vrátila
     * dřív, než past zavolá). E = 53h: BC bajtů na HL je tělo k hlavičce
     * bez těla, bez ní k hlavičce na 10F0h; soubor se připojí jako nový
     * blok MZF na konec pásky Virtual CMT (cmt_direct_save_mzf()). Jiné E
     * = Break. Bajty se čtou tak, jak je vidí CPU (memory_read_byte_no_se()).
     *
     * @pre Vlákno emulátoru, uvnitř instrukce OUT (03h) s nainstalovaným
     *      patchem zápisu (CMTHACK_TEST_SAVE_INSTALLED).
     * @post ROM dostala výsledek v AF: OK, nebo Break (A = 2) při chybě.
     */
    extern void cmthack_save_block ( void );

    /**
     * @brief Past ROM "ověř blok" (OUT 04h): porovná BC bajtů na HL s tělem MZF otevřeného bloku pásky.
     *
     * Načetla-li past čtení hlavičku z pásky bez těla (monitor V čte RHEAD
     * a pak VERFY), porovná s MZF téže hlavičky a hlavičku zapomene bez
     * posunu pásky. Shoda = OK; rozdíl nebo kratší tělo bloku = chyba
     * (CY = 1, A = 1); bez pásky, bez otevřeného bloku, blok bez MZF a deck
     * v RECORD = Break (CY = 1, A = 2) s řádkem na stderr (v RECORD patch
     * v ROM není, větev je jen pojistka). Transport v PLAY se nejdřív
     * zastaví, jen je-li s čím porovnat (jako u pasti čtení). Páska se
     * neposouvá.
     *
     * @pre Vlákno emulátoru, uvnitř instrukce OUT (04h) s nainstalovaným
     *      patchem ověření (CMTHACK_TEST_VERIFY_INSTALLED).
     */
    extern void cmthack_verify_block ( void );

    /**
     * @brief Reset stroje: zavře rozčtený soubor MZF a vynuluje stav hacku nad páskou.
     *
     * Zapomene MZF rozčtené z pásky (hlavička bez těla), příznak konce
     * pásky i hlavičku pasti zápisu bez těla (cmthack_tape_state_clear()).
     * Páska se resetem nehýbe: další LOAD po resetu vezme blok, na
     * kterém páska stojí (po konci pásky poslední blok s MZF, tedy tentýž
     * program znovu), jako by uživatel resetoval a načítal znovu.
     * @pre Vlákno emulátoru.
     * @post cmthack_tape_state_get() vrací false.
     */
    extern void cmthack_reset ( void );
    extern void cmthack_init ( void );
    /** @brief Ukončení: zavře rozčtený soubor MZF, zapomene MZF z pásky, uvolní cestu dialogu (viz cmthack.c). */
    extern void cmthack_exit ( void );

    /**
     * @brief Past ROM "načti hlavičku" (OUT 01h): z aktuálního bloku pásky, jinak dialogem.
     *
     * Je-li ve Virtual CMT páska a její aktuální blok nese MZF, zapíše jeho
     * hlavičku do RAM na adresu z HL (běžící nebo pozastavený transport
     * nejdřív zastaví cmt_stop()); jinak (blok bez MZF, konec pásky, bez
     * pásky, nahrávání) otevře dialog pro výběr MZF, v headless režimu
     * skončí Break s varováním. Podrobnosti v cmthack.c.
     * @pre Vlákno emulátoru, uvnitř instrukce OUT (01h) s nainstalovaným patchem.
     */
    extern void cmthack_load_file ( void );
    /**
     * @brief Hlavička MZF ze souboru do RAM na HL a výsledek ROM; soubor zůstane otevřený pro tělo.
     *
     * Volá ji past hlavičky i --run-mzf a MCP media_load_mzf mimo past ROM
     * (HL nastaví volající). Vedlejší efekty (baseui_error, kontrola
     * velikosti MZF, událost Event Vieweru) popisuje cmthack.c.
     * @pre Vlákno emulátoru, HL ukazuje na cíl hlavičky v RAM.
     */
    extern void cmthack_load_mzf_filename ( const char *filename );

    /**
     * @brief Past ROM "načti tělo" (OUT 02h): tělo k poslední hlavičce (HL = adresa, BC = délka).
     *
     * Hlavička z pásky: tělo téhož bloku, pak posun pásky na další blok
     * s MZF. Hlavička ze souboru: tělo ze souboru. Bez hlavičky Break.
     * @pre Vlákno emulátoru, uvnitř instrukce OUT (02h) s nainstalovaným patchem.
     */
    extern void cmthack_read_mzf_body ( void );


    /**
     * @brief Stav CMT hacku nad páskou pro snapshot (devices/cmt.xml, element <tape>/<cmthack>).
     *
     * Odpovídá stavu, který hack drží mezi pastmi ROM (cmthack.c,
     * st_CMTHACK_TAPE), vyjádřenému vůči bloku pásky místo čítače
     * cmttmz_get_open_serial() (ten načtení snapshotu samo posune).
     *
     * Invarianty: nejvýš jedno z pending a end; pending => pending_block
     * je blok s MZF; !pending_moved => pending_block je otevřený blok pásky;
     * end => páska stojí na bloku, za kterým hack zjistil konec pásky
     * (otevřený blok).
     */
    typedef struct st_CMTHACK_TAPE_STATE {
        bool pending;      /**< hlavička z pásky načtená, tělo zatím ne */
        int pending_block; /**< blok, ze kterého hlavička pochází (tělo se bere z něj); jen s pending */
        bool pending_moved; /**< páskou mezi hlavičkou a tělem někdo hnul: po těle se neposouvá; jen s pending */
        bool end;          /**< hack zjistil konec pásky na otevřeném bloku: další hlavička = jako bez pásky */
    } st_CMTHACK_TAPE_STATE;


    /**
     * @brief Platný stav hacku nad páskou k uložení do snapshotu.
     *
     * Hlavička bez těla se vrací i tehdy, když páskou mezitím někdo hnul
     * (pending_moved): tělo pak dostane program tak jako tak. Konec pásky
     * jen platný (páskou od jeho zjištění nikdo nehnul).
     *
     * @param[out] st Stav (vynulovaný, pokud funkce vrátí false).
     * @return true = je co uložit (pending nebo end), jinak false.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (kopie stavu okna
     *      Virtual CMT, cmt_view_fill(): čte jen skalární hodnoty, výsledek je
     *      nanejvýš o snímek starý, jako cmt_motor_get_state()).
     */
    extern bool cmthack_tape_state_get ( st_CMTHACK_TAPE_STATE *st );


    /**
     * @brief Nastaví stav hacku nad páskou po obnovení pásky ze snapshotu.
     *
     * Volá loader snapshotu (snap_cmt.c) poté, co obnovil vloženou pásku
     * a otevřený blok; stav se váže na aktuální čítač otevření, takže ho
     * posun čítače během samotného obnovení pásky nezruší. Hodnoty ze
     * snapshotu se ověří: pending a end zároveň, pending_block mimo pásku
     * nebo bez MZF, pending bez pending_moved na jiném než otevřeném bloku,
     * end bez otevřeného bloku = neplatné; stav se pak vynuluje.
     *
     * @param st Stav ze snapshotu.
     * @return EXIT_SUCCESS = nastaveno; EXIT_FAILURE = neplatné hodnoty
     *         (stav je vynulovaný).
     * @pre Vlákno emulátoru, vložená páska cmt_tmz je obnovená.
     * @post Po úspěchu se pasti chovají jako ve stroji v okamžiku uložení.
     */
    extern int cmthack_tape_state_set ( const st_CMTHACK_TAPE_STATE *st );


    /**
     * @brief Vynuluje stav hacku nad páskou (zapomene hlavičku bez těla i konec pásky).
     *
     * Loader snapshotu ho volá na začátku načtení: stav z doby před
     * načtením nesmí přežít do načteného stroje (záznam vstupů pak začíná
     * při záznamu i přehrání ze stejného stavu). Zapomene i hlavičku pasti
     * zápisu WHEAD bez těla (Direct Save); past těla pak vezme hlavičku
     * z 10F0h.
     * @pre Vlákno emulátoru.
     * @post cmthack_tape_state_get() vrací false.
     */
    extern void cmthack_tape_state_clear ( void );

#define CMTHACK_TEST_IS_INSTALLED (g_cmthack.load_patch_installed)

/**
 * @brief Je v ROM patch zápisových rutin (Direct Save, past OUT 03h)?
 *
 * Jedno čtení proměnné: smí do obsluhy zápisu na port (ne do smyčky po taktech).
 */
#define CMTHACK_TEST_SAVE_INSTALLED (g_cmthack.save_patch_installed)

/**
 * @brief Je v ROM patch rutiny VERFY (past OUT 04h)?
 *
 * Platí jen spolu s CMTHACK_TEST_SAVE_INSTALLED. Jedno čtení proměnné.
 */
#define CMTHACK_TEST_VERIFY_INSTALLED (g_cmthack.verify_patch_installed)

/**
 * @brief Podvrhuje CMT hack právě motor kazety (PC3 / PC4 8255)?
 *
 * S nainstalovaným hackem náběžná hrana PC3 (povel motoru programu) jen
 * přepíná podvržený bit PC4 (st_PIO8255::signal_pc04) a PC4 ho čte; patch
 * čtecích rutin ROM pak hlášení motoru nepotřebuje skutečné (patch
 * zápisu, CMTHACK_TEST_SAVE_INSTALLED, @MGON vůbec nevolá). Neplatí při
 * nahrávání (deck v RECORD): tehdy v ROM patch zápisu není
 * (cmthack_save_patch_sync()), hrana PC3 řídí deck (cmt_motor_toggle())
 * a PC4 čte skutečný stav decku jako bez hacku, aby SAVE programu i SAVE
 * rutinami ROM nahrál (ROM @MGON 069Fh / @MGOFF 0700h motor zapne
 * a vypne podle PC4).
 *
 * Výraz čte g_cmt (vyžaduje cmt.h). Dvě porovnání proměnných: smí do
 * zápisu Port C a do čtení Port C (ne do smyčky po taktech).
 */
#define CMTHACK_TEST_FAKES_MOTOR (CMTHACK_TEST_IS_INSTALLED && !CMT_TEST_RECORD)

#define CMTHACK_TEST_FIX_FNAME_TERMINATOR (g_cmthack.fix_fname_terminator)

#define CMTHACK_SET_FIX_FNAME_TERMINATOR(value) (g_cmthack.fix_fname_terminator = value)

#ifdef __cplusplus
}
#endif

#endif /* CMTHACK_H */

