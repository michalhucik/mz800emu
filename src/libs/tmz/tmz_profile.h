/**
 * @file   tmz_profile.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.9.0
 * @brief  Registr profilů zapisovače pro blok 0x46 (TMZ v1.1).
 *
 * Profil popisuje, jak konkrétní program nebo ROM zapisuje soubor na
 * pásku (tmz-v1.1-specification.md kap. 8). Registr z MZF a parametrů sestaví
 * blok 0x46 se seznamem částí a efektivními hodnotami v TLV; ID profilu a
 * PROFILE_ARGS se uloží jen jako popisek. Player registr nepotřebuje.
 *
 * Profily: ROM-800-NOMINAL (nominální pulzy ROM MZ-800 v µs),
 * ROM-800-EXACT (časování z kódu ROM v T-stavech, 22 kontextů, shoda se
 * záznamem z emulátoru na T), TC-1.21 (TurboCopy V1.21, pulzy čítače CTC,
 * volitelně s předzavaděčem), TAPEMZ-V1-LEGACY (generátory TMZ v1.0 pro
 * převedené bloky v1.0, spec 9.2), UNICMT (přehrávač UniCMT, pulzy z hlavičky
 * CMTSPEED, struktura jako ROM-800-NOMINAL [neověřeno], spec 10), IC-10.2-NORMAL,
 * IC-10.2-TURBO a IC-10.2-FASTIPL (Intercopy V10.2, T-stavy z kódu ověřené
 * záznamem hran v emulátoru, pauza mezi bloky HIGH), IC-10.2-CPM, PEZIK-2.0A,
 * BASIC-1Z016 a MZ800EMU-CMT (signál, který emulátor mz800emu hraje z MZF:
 * mztape, formát MZ800_SANE, časová základna takt GDG).
 *
 * @par Licence:
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
 */


#ifndef TMZ_PROFILE_H
#define TMZ_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "../tzx/tzx.h"
#include "../mzf/mzf.h"
#include "../cmtspeed/cmtspeed.h"


    /**
     * @brief Chyba registru.
     */
    typedef enum en_TMZ_PROFILE_ERROR {
        TMZ_PROFILE_OK = 0,            /**< bez chyby */
        TMZ_PROFILE_ERROR_UNKNOWN,     /**< neznámé ID profilu */
        TMZ_PROFILE_ERROR_ARGS,        /**< parametr neplatný pro profil (rychlost, NULL vstup, pauza, parametr, který profil nepoužívá) */
        TMZ_PROFILE_ERROR_BUILD,       /**< sestavení selhalo (alokace, tělo delší než 65535 B) */
    } en_TMZ_PROFILE_ERROR;


    /**
     * @brief ID profilu MZ800EMU-CMT (signál, který emulátor mz800emu hraje z MZF přes mztape).
     *
     * Jediné místo, kde je ID zapsané: registr i zařazení pro export MZT
     * (tmz_mzt) a volající v emulátoru používají toto makro, takže
     * přejmenování profilu je změna jednoho řádku. Bloky už zapsané do
     * souborů ale nesou staré ID.
     */
#define TMZ_PROFILE_ID_MZ800EMU_CMT "MZ800EMU-CMT"


    /**
     * @brief Formát záznamu profilu TAPEMZ-V1-LEGACY (spec 9.2).
     */
    typedef enum en_TMZ_LEGACY_FORMAT {
        TMZ_LEGACY_NONE = -1,     /**< nenastaveno (jiný profil) */
        TMZ_LEGACY_NORMAL = 0,    /**< mztape: hlavička + tělo */
        TMZ_LEGACY_FASTIPL = 1,   /**< mzcmt_fastipl V07: hlavička $BB rychlostí 1:1 + tělo rychlostí bloku */
        TMZ_LEGACY_BSD = 2,       /**< mzcmt_bsd: hlavička + kusy po 258 B */
    } en_TMZ_LEGACY_FORMAT;

    /**
     * @brief Parametry sestavení bloku (odpovídají PROFILE_ARGS, spec 8.2).
     *
     * @invariant loader je 0 nebo 1; gap_rom je -1, 0 nebo 1
     * @invariant machine je -1 nebo en_TMZ_MACHINE; pulseset je -1 nebo en_TMZ_PULSESET
     */
    typedef struct st_TMZ_PROFILE_ARGS {
        en_CMTSPEED speed;   /**< TC-1.21: rychlost (CMTSPEED_NONE = výchozí 2:1); ROM: jen NONE nebo 1:1; LEGACY: NONE = 1:1;
                                  MZ800EMU-CMT: NONE = 1:1, jinak libovolný platný poměr včetně 2:1 CPM */
        int loader;          /**< TC-1.21: 1 = předzavaděč (Loader Y, výchozí), 0 = Loader N */
        int gap_rom;         /**< ROM: -1 = výchozí profilu, 1 = tóny ROM 22000/11000, 0 = poloviční 11000/5500 */
        uint32_t pause_ms;   /**< pauza za posledním dílem v ms (výchozí 1000) */
        int machine;         /**< en_TMZ_MACHINE, -1 = nezapisovat; všechny profily, jen informativní (spec 9.2) */
        int legacy_format;   /**< LEGACY: en_TMZ_LEGACY_FORMAT (povinný) */
        int pulseset;        /**< LEGACY: en_TMZ_PULSESET, -1 = MZ-800 */
        uint32_t lgap;       /**< LEGACY: počet SHORT úvodního tónu, 0 = výchozí formátu */
        uint32_t sgap;       /**< LEGACY: počet SHORT tónu před tělem, 0 = výchozí formátu */
        uint16_t pulses[4];  /**< LEGACY: vlastní půlvlny LONG H, LONG L, SHORT H, SHORT L v 0,1 µs; 0 = ze sady */
        uint16_t cmtspeed[4];/**< UNICMT: pulzy z hlavičky CMTSPEED, LONG H, LONG L, SHORT H, SHORT L v µs; všechny 0 = 1x;
                                  MZ800EMU-CMT: vlastní pulzy v µs (všechny nenulové, jen se speed NONE), všechny 0 = podle speed */
        uint16_t bd;         /**< IC-10.2-*: rychlost Intercopy 600, 1200, 2400, 2800, 3200 (popisek v Bd), 0 = 1200;
                                  PEZIK-2.0A: 2400 nebo 3200, IC-10.2-CPM: 1500, 2400, 2800, 3200, 3600 (0 = 2400) */
        uint8_t tc_a;        /**< TC-1.21: čitatel poměru a/b TurboCopy (1-9), 0 = rychlost podle speed */
        uint8_t tc_b;        /**< TC-1.21: jmenovatel poměru a/b (1-9); platí jen s tc_a != 0,
                                  1/2 <= a/b <= 3 (rozsah TurboCopy V1.21); má přednost před speed */
        const uint8_t *ck_header;  /**< 2 B checksumu hlavičky v pořadí zápisu (rodina FM), NULL = vypočítat; volající drží paměť po dobu volání */
        const uint8_t *ck_body;    /**< totéž pro tělo; do PROFILE_ARGS se nezapisuje, nese ho část bloku */
    } st_TMZ_PROFILE_ARGS;


    /**
     * @brief Veřejný popis profilu v registru.
     *
     * @par Vlastnictví:
     * Statická data knihovny, platná po celou dobu běhu programu.
     */
    typedef struct st_TMZ_PROFILE {
        const char *id;            /**< ID (spec 8.1) */
        const char *description;   /**< popis anglicky, pro výpisy nástrojů */
        const char *confidence;    /**< míra jistoty anglicky ("emulator-verified", "documented", ...) */
    } st_TMZ_PROFILE;


    /**
     * @brief Nastaví výchozí parametry (speed NONE, loader 1, gap_rom -1, pause 1000 ms,
     *        machine -1, legacy_format NONE, pulseset -1, lgap, sgap, pulses, cmtspeed a bd 0).
     * @param args Parametry (nesmí být NULL).
     */
    extern void tmz_profile_args_init ( st_TMZ_PROFILE_ARGS *args );

    /**
     * @brief Počet profilů v registru.
     * @return Počet.
     */
    extern size_t tmz_profile_count ( void );

    /**
     * @brief Profil podle pořadí.
     * @param index Index 0 .. tmz_profile_count() - 1.
     * @return Profil, nebo NULL mimo rozsah.
     */
    extern const st_TMZ_PROFILE* tmz_profile_get ( size_t index );

    /**
     * @brief Najde profil podle ID (rozlišuje velikost písmen).
     * @param id ID profilu (NULL = nenalezeno).
     * @return Profil, nebo NULL.
     */
    extern const st_TMZ_PROFILE* tmz_profile_find ( const char *id );

    /**
     * @brief Přečte text PROFILE_ARGS (`klíč=hodnota` oddělené čárkou).
     *
     * Klíče: speed (`a:b` jako cmtspeed_get_ratiotxt, přijme i `a/b`; 2:1 CPM
     * jako `2:1 (cp/m)`; jiný poměr
     * jednociferných a, b z 1..9 v rozsahu 1/2..3/1 = poměr TurboCopy do tc_a/tc_b), loader (Y/N), gap
     * (rom/short), pause (ms, desítkově), machine (generic, mz700, mz800,
     * mz1500, mz80b), format (normal, fastipl, bsd), pulseset (700, 800, 80b),
     * lgap, sgap (desítkově, nejvýš 1000000), pulses (4 hodnoty 0..65535
     * oddělené `/`, LONG H/LONG L/SHORT H/SHORT L v 0,1 µs, 0 = ze sady),
     * cmtspeed (4 hodnoty 1..65535 oddělené `/`, tytéž půlvlny v µs), bd
     * (600, 1200, 2400, 2800, 3200).
     * Nejdřív nastaví výchozí hodnoty (tmz_profile_args_init), pak uplatní
     * položky v pořadí.
     *
     * @param text Text (nemusí být ukončen nulou; NULL jen při length 0).
     * @param length Délka textu.
     * @param[out] args Výsledek; při chybě nedefinovaný.
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE při neznámém klíči, chybějícím
     *         `=` nebo neplatné hodnotě.
     */
    extern int tmz_profile_args_parse ( const char *text, size_t length, st_TMZ_PROFILE_ARGS *args );

    /**
     * @brief Sestaví blok 0x46 podle profilu.
     *
     * Části a jejich hodnoty určuje profil (spec 6.4, 6.6). Do bloku zapíše ID profilu a PROFILE_ARGS. Bajty MZF
     * hlavičky se nemění (převádí se jen pořadí bajtů fsize/fstrt/fexec;
     * fname.terminator na 11h zůstává, jak přišel). Hlavička předzavaděče
     * TurboCopy vzniká v mzcmt_turbo_build_preloader(), která 11h zkopíruje
     * z originálu.
     *
     * TAPEMZ-V1-LEGACY (spec 9.2) vyžaduje args->legacy_format. Formát
     * fastipl sestaví inline hlavičku $BB přes mzcmt_fastipl_build_header()
     * (V07, readpoint podle rychlosti), formát bsd čte tělo jako kusy po
     * 258 B. TMZ_PROFILE_ERROR_ARGS vrátí bez formátu, při rychlosti 2:1
     * CPM, u normal při částečně vyplněných vlastních pulzech, u bsd při
     * rychlosti jiné než 1:1, vlastních pulzech nebo těle, které není
     * násobkem 258 B; TMZ_PROFILE_ERROR_BUILD u bsd s víc než 254 kusy.
     *
     * IC-10.2-NORMAL: hlavička a tělo rychlostí bd. IC-10.2-TURBO: hlavička
     * a 90 B předzavaděče TurboCopy (mzcmt_turbo_build_preloader, čtecí bod
     * rp+5) při 1200, tělo rychlostí bd. IC-10.2-FASTIPL: hlavička $BB
     * (mzcmt_fastipl_build_header, V07, čtecí bod rp z tabulky Intercopy) při
     * 1200, tělo rychlostí bd. Pauza mezi částmi 1 222 500 T, za poslední
     * args->pause_ms, obě na úrovni HIGH. Rychlost args->speed nebo bd mimo
     * tabulku vrátí TMZ_PROFILE_ERROR_ARGS.
     *
     * PEZIK-2.0A a IC-10.2-CPM (rodina MANCHESTER): část hlavičky
     * a část těla (jen při body_size > 0) s parametry podle kódu TAPE.COM
     * a měření Intercopy (spec 7.2). Pezik: bd 2400 nebo 3200,
     * hlavička přejde rovnou do pilotu těla, tělo delší než 16 384 B vrátí
     * TMZ_PROFILE_ERROR_BUILD (TAPE.COM delší soubor dělí na díly). Intercopy:
     * bd 1500, 2400, 2800, 3200 nebo 3600 (časování a pauza za hlavičkou LOW
     * změřené pro každou rychlost, 2400: 1 221 688 T). Pauza za posledním dílem
     * args->pause_ms, LOW. Jiná rychlost nebo args->speed = TMZ_PROFILE_ERROR_ARGS.
     *
     * BASIC-1Z016 (spec 6.7): tělo = kusy po 258 B (1 až 254, jinak
     * TMZ_PROFILE_ERROR_BUILD, stejně jako prázdné tělo). Část HEADER (tón
     * 22000, tapemark 40/40) a pro každý kus BSD_CHUNK (tón 11000, tapemark
     * 20/20), všechny s kopií za 256 S, časování BASICu 1Z-016 v T-stavech.
     * Pauza LOW za hlavičkou a kusy 6 610 752 T, za posledním kusem
     * args->pause_ms. ID kusů se nekontrolují.
     *
     * UNICMT sestaví části jako ROM-800-NOMINAL (tóny podle gap, kopie,
     * pauzy), PULSES v µs z args->cmtspeed (všechny 0 = 1x). READER (ROM,
     * N = 82) dostanou jen pulzy 1x. Rychlost (args->speed) jiná než NONE
     * vrátí TMZ_PROFILE_ERROR_ARGS.
     *
     * MZ800EMU-CMT (TMZ_PROFILE_ID_MZ800EMU_CMT, spec 8.4) popíše signál,
     * který mztape generuje formátem MZTAPE_FORMATSET_MZ800_SANE: část
     * HEADER (tón lgap formátu, tapemark MZTAPE_LTM_*) a BODY (tón sgap,
     * tapemark MZTAPE_STM_*; i pro tělo 0 B, pak s checksumem 0000 jako
     * v mztape), bez kopií, bez READER
     * a STOP, TIMEBASE 17 721 600 (takt GDG MZ-800). PULSES jsou délky
     * z mztape_get_speed_pulses() pro args->speed (NONE = 1:1; u 2:1 CPM sada
     * cmt.com) v tikách GDG, round(sekundy * 17 721 600); s args->cmtspeed
     * (všechna čtyři pole nenulová) round(µs * 17,7216). Mezi hlavičkou
     * a tělem pauza 0, za posledním dílem args->pause_ms, LOW.
     * TMZ_PROFILE_ERROR_ARGS: neplatná rychlost (CMTSPEED_CUSTOM), cmtspeed
     * spolu se speed jiným než NONE, částečně vyplněné cmtspeed, pauza mimo
     * rozsah uint32 v tikách.
     *
     * Každý profil čte jen své parametry: pause_ms a machine všechny,
     * speed a gap_rom ROM-800-*, navíc cmtspeed UNICMT, speed a cmtspeed
     * MZ800EMU-CMT, speed a loader
     * TC-1.21, bd IC-10.2-* a PEZIK-2.0A, speed, legacy_format, pulseset, lgap, sgap a
     * pulses TAPEMZ-V1-LEGACY; BASIC-1Z016 jen pause_ms a machine. Jiný parametr s hodnotou odlišnou od
     * tmz_profile_args_init() vrátí TMZ_PROFILE_ERROR_ARGS (dřív se tiše
     * ignoroval). Rozhoduje hodnota, ne to, zda ji volající nastavil:
     * výchozí hodnota (např. loader 1) projde u každého profilu. Každá část nese TIMEBASE, takže časování nezávisí na taktu
     * z bloku 0x43.
     *
     * @param id ID profilu.
     * @param args Parametry (NULL = výchozí).
     * @param header MZF hlavička v pořadí hostitele (jak ji vrací knihovna mzf).
     * @param body Tělo (NULL jen při body_size 0).
     * @param body_size Velikost těla (nejvýš 65535).
     * @param[out] err Chyba (smí být NULL).
     * @return Nový blok (uvolnit tmz_block_free()), nebo NULL při chybě.
     */
    extern st_TZX_BLOCK* tmz_profile_build_block ( const char *id, const st_TMZ_PROFILE_ARGS *args,
                                                   const st_MZF_HEADER *header, const uint8_t *body, uint32_t body_size,
                                                   en_TMZ_PROFILE_ERROR *err );

    /**
     * @brief Text chyby registru (anglicky).
     * @param err Kód chyby.
     * @return Statický řetězec.
     */
    extern const char* tmz_profile_error_string ( en_TMZ_PROFILE_ERROR err );

    /**
     * @brief Historická pulzní sada generátorů v1.0 profilu TAPEMZ-V1-LEGACY (formát normal).
     *
     * Délky půlvln při rychlosti 1:1, se kterými profil převádí bloky v1.0.
     * Jsou to historické sady generátorů v1.0, zachované pro shodné přehrání
     * souborů v1.0 (tabulka výchozích pulzních sad specifikace v1.0, kap. 5.2);
     * živé cesty (mztape) používají jiné sady. Funkce jen čte statickou
     * tabulku, nemá vedlejší efekty.
     *
     * @param pulseset Pulzní sada: 0 = MZ-700/80K/80A, 1 = MZ-800/1500,
     *                 2 = MZ-80B (hodnoty en_MZTAPE_PULSESET).
     * @param[out] out LONG H, LONG L, SHORT H, SHORT L v sekundách.
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE (out je NULL, neplatná sada;
     *         out se pak nemění).
     */
    extern int tmz_profile_legacy_pulses ( int pulseset, double out[4] );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_PROFILE_H */
