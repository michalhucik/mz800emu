/**
 * @file   cmt_tmz_import.h
 * @brief  Import pásky (TMZ, TZX, MZF, MZT, TAP, WAV) do TMZ v paměti.
 *
 * Každá páska Virtual CMT se v emulátoru drží jako st_TZX_FILE (TMZ
 * v paměti), který přehrává tmz_player. Tento modul je jen převodní
 * vrstva: čisté funkce, které ze souboru nebo z bufferu sestaví
 * st_TZX_FILE. Nezná globální stav emulátoru (g_cmt a další), nic
 * neregistruje a nemění chování běžícího emulátoru; rychlost, vlastní
 * pulzy a pauzu dostává ve st_CMTTMZ_IMPORT_OPTS od volajícího.
 *
 * MZF a MZT se převádějí na bloky 0x46 s profilem MZ800EMU-CMT
 * (TMZ_PROFILE_ID_MZ800EMU_CMT), tedy na signál, který emulátor dosud hrál
 * z MZF přes mztape (formát MZTAPE_FORMATSET_MZ800_SANE).
 *
 * Texty v reportu (varování, chyby) jsou anglicky a nejsou lokalizované;
 * lokalizaci a zobrazení řeší volající.
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

#ifndef CMT_TMZ_IMPORT_H
#define CMT_TMZ_IMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/tzx/tzx.h"
#include "libs/mzf/mzf.h"
#include "libs/mztape/mztape.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/cmt_stream/cmt_stream_all.h"


    /**
     * @brief Formát zdroje importu.
     */
    typedef enum en_CMTTMZ_SOURCE {
        CMTTMZ_SOURCE_TMZ, /**< TMZ nebo TZX (přípony tmz, tzx) */
        CMTTMZ_SOURCE_MZF, /**< jeden soubor MZF (přípony mzf, m12) */
        CMTTMZ_SOURCE_MZT, /**< MZF za sebou, případně s hlavičkami CMTSPEED UniCMT (přípona mzt) */
        CMTTMZ_SOURCE_TAP, /**< páska TAP (přípona tap) */
        CMTTMZ_SOURCE_WAV, /**< zvukový záznam WAV (přípony wav, wave) */
    } en_CMTTMZ_SOURCE;


    /**
     * @brief Volby importu (hodnoty, které dodává volající z nastavení Virtual CMT).
     *
     * @invariant default_speed je CMTSPEED_NONE (= 1:1), platný poměr
     *            (cmtspeed_is_valid()) nebo CMTSPEED_CUSTOM.
     * @invariant Při default_speed == CMTSPEED_CUSTOM má každá délka
     *            v custom_pulses po zaokrouhlení na celé µs hodnotu 1 až 65535.
     */
    typedef struct st_CMTTMZ_IMPORT_OPTS {
        en_CMTSPEED default_speed;                 /**< výchozí rychlost bloků MZF bez vlastní rychlosti (g_cmt.mz_cmtspeed) */
        st_MZTAPE_PULSES_LENGTH custom_pulses;     /**< vlastní délky pulzů v sekundách; platí jen při default_speed == CMTSPEED_CUSTOM */
        en_CMT_STREAM_POLARITY wav_polarity;       /**< polarita prahování WAV */
        uint16_t pause_ms;                         /**< pauza za každým blokem MZF kromě posledního bloku pásky v ms (výchozí 1000) */
    } st_CMTTMZ_IMPORT_OPTS;


    /**
     * @brief Výsledek importu pro volajícího (zdroj, varování, chyba).
     *
     * @invariant warning i error jsou vždy ukončené nulou.
     */
    typedef struct st_CMTTMZ_IMPORT_REPORT {
        en_CMTTMZ_SOURCE source;  /**< zdroj, ze kterého se importovalo; u cmttmz_import_file() platí jen při rozpoznané příponě */
        char warning[256];        /**< varování anglicky, víc varování odděleno "; "; prázdné = bez varování */
        char error[256];          /**< chyba anglicky, vyplněná při návratu NULL; jinak prázdné */
    } st_CMTTMZ_IMPORT_REPORT;


    /**
     * @brief Odkud má blok importované pásky svou rychlost.
     *
     * Údaj nese jen import, v souboru TMZ uložený není. Rozšíření Virtual
     * CMT (cmt_tmz.c) z něj plní index pásky (blspeed) a pozná, které bloky
     * smí přestavět jinou rychlostí.
     */
    typedef enum en_CMTTMZ_SPEED_ORIGIN {
        CMTTMZ_SPEED_ORIGIN_NONE = 0, /**< rychlost dává soubor (blok z TMZ/TZX, WAV): import ji neurčoval */
        CMTTMZ_SPEED_ORIGIN_DEFAULT,  /**< blok sestaven výchozí rychlostí z voleb (opts->default_speed) */
        CMTTMZ_SPEED_ORIGIN_FIXED,    /**< blok má pevnou rychlost ze zdroje (hlavička CMTSPEED v MZT, blok TAP při 1:1) */
    } en_CMTTMZ_SPEED_ORIGIN;


    /**
     * @brief Rychlost, se kterou import sestavil jeden blok pásky.
     *
     * Pole těchto záznamů vrací cmttmz_import_file_speeds()
     * a cmttmz_import_buffer_speeds(), jeden záznam na blok výsledného
     * souboru (stejný index jako file->blocks).
     *
     * @invariant origin == CMTTMZ_SPEED_ORIGIN_NONE => speed == CMTSPEED_NONE,
     *            pause_ms == 0 a pulses jsou nulové.
     * @invariant pulses jsou platné jen při speed == CMTSPEED_CUSTOM, jinak nulové.
     */
    typedef struct st_CMTTMZ_BLOCK_SPEED {
        en_CMTTMZ_SPEED_ORIGIN origin;  /**< původ rychlosti */
        en_CMTSPEED speed;              /**< rychlost sestavení: poměr, CMTSPEED_CUSTOM (pak pulses), u TAP CMTSPEED_1_1, u ORIGIN_NONE CMTSPEED_NONE */
        st_MZTAPE_PULSES_LENGTH pulses; /**< vlastní délky pulzů v sekundách (jen CMTSPEED_CUSTOM) */
        uint16_t pause_ms;              /**< pauza za blokem v ms, se kterou import sestavil blok MZF nebo TAP; u ORIGIN_NONE 0 */
    } st_CMTTMZ_BLOCK_SPEED;


    /**
     * @brief Přidá do reportu varování; víc varování odděluje "; ".
     *
     * Import přes ni skládá rep->warning. Text, který se nevejde celý, se
     * ořízne. Když se za předchozí varování nevejde oddělovač "; " a aspoň
     * jeden znak (délka >= 253), další varování se nepřidá vůbec.
     *
     * @param rep Report (NULL se ignoruje); rep->warning musí být ukončené nulou,
     *        jinak se nemění.
     * @param text Varování anglicky (NULL se ignoruje).
     * @post rep->warning je ukončené nulou.
     */
    extern void cmttmz_report_add_warning ( st_CMTTMZ_IMPORT_REPORT *rep, const char *text );


    /**
     * @brief Nastaví výchozí volby importu.
     *
     * default_speed = CMTSPEED_1_1, custom_pulses = pulzy UniCMT 1x
     * (470 / 494 / 240 / 278 µs, mztape_pulses_set_us()), wav_polarity =
     * CMT_STREAM_POLARITY_NORMAL, pause_ms = 1000.
     *
     * @param opts Volby (NULL se ignoruje).
     * @post Všechny položky *opts mají výchozí hodnoty.
     */
    extern void cmttmz_import_opts_init ( st_CMTTMZ_IMPORT_OPTS *opts );


    /**
     * @brief Určí zdroj importu podle přípony jména souboru.
     *
     * Přípony bez ohledu na velikost písmen: tmz, tzx -> CMTTMZ_SOURCE_TMZ;
     * mzf, m12 -> CMTTMZ_SOURCE_MZF; mzt -> CMTTMZ_SOURCE_MZT; tap ->
     * CMTTMZ_SOURCE_TAP; wav, wave -> CMTTMZ_SOURCE_WAV. Přípona je část za
     * poslední tečkou v posledním prvku cesty (oddělovače '/' i '\\').
     *
     * @param filename Jméno nebo cesta souboru (UTF-8).
     * @param[out] source Rozpoznaný zdroj; při neúspěchu se nemění (smí být
     *        NULL, pak jde jen o test přípony).
     * @return EXIT_SUCCESS; EXIT_FAILURE při NULL filename, jménu bez
     *         přípony nebo neznámé příponě.
     */
    extern int cmttmz_source_from_filename ( const char *filename, en_CMTTMZ_SOURCE *source );


    /**
     * @brief Načte soubor pásky a převede ho na TMZ v paměti.
     *
     * Zdroj určí cmttmz_source_from_filename(). Soubor se načte celý do
     * paměti stejně jako v dosavadních rozšířeních CMT
     * (generic_driver_open_memory_from_file() s g_memory_driver_static, cesta
     * v UTF-8 i na Windows) a převede cmttmz_import_buffer(). Soubor
     * nulové délky paměťový driver neotevře, hlásí se tedy jako neotevřený.
     *
     * Chyby v rep->error: neznámá přípona "Unsupported tape file extension";
     * soubor nejde otevřít nebo přečíst "Can't open file " + path; soubor
     * větší než 4 GB (délka bufferu je 32bitová) "Tape file is too large";
     * ostatní jako cmttmz_import_buffer().
     *
     * @param path Cesta k souboru (UTF-8).
     * @param opts Volby (NULL = výchozí, cmttmz_import_opts_init()).
     * @param[out] rep Report (smí být NULL, pak se nic nehlásí); funkce ho
     *        na začátku vynuluje.
     * @return Nový soubor (uvolnit tzx_free()), nebo NULL při chybě.
     * @pre Proběhlo memory_driver_init() (dělá ho start emulátoru i mztest_init()).
     * @pre Volá se jen z vlákna emulátoru. Funkce používá sdílený paměťový
     *      driver g_memory_driver_static, jehož chybové pole err zapisuje každá
     *      operace bez zámku; souběžné volání z jiného vlákna (UI) by s ním
     *      závodilo. Vlastní instanci driveru knihovna generic_driver v tomto
     *      sestavení nenabízí (callbacky memory_driver jsou statické
     *      a GENERIC_DRIVER_MEMORY_CB není zapnuté).
     * @post Soubor na disku zůstává beze změny, načtená data se uvolní před návratem.
     */
    extern st_TZX_FILE* cmttmz_import_file ( const char *path, const st_CMTTMZ_IMPORT_OPTS *opts, st_CMTTMZ_IMPORT_REPORT *rep );


    /**
     * @brief Převede data pásky v paměti na TMZ v paměti.
     *
     * Výsledek má vždy hlavičku TMZ (tmz_header_init()), jen u zdroje
     * CMTTMZ_SOURCE_TMZ zůstává hlavička načteného souboru (i ZXTape!).
     *
     * Import TMZ/TZX a WAV čte data přímo (bez kopie) přes sdílený
     * g_memory_driver_static, platí tedy stejná podmínka vlákna jako
     * u cmttmz_import_file().
     *
     * MZF: jeden blok cmttmz_build_mzf_block() rychlostí opts->default_speed.
     * Tělo je fsize bajtů za hlavičkou; delší soubor dá varování "MZF file is
     * longer than header size, extra data ignored", kratší chybu "MZF body is
     * shorter than header size", data kratší než hlavička chybu "MZF file is
     * shorter than MZF header".
     *
     * MZT: každý soubor MZF jeden blok. Za hlavičkou CMTSPEED (UniCMT) dostanou
     * všechny další soubory až do další hlavičky vlastní pulzy z hlavičky,
     * soubory před první hlavičkou opts->default_speed. Hlavička CMTSPEED před
     * prvním souborem platí také, jen přidá varování "note: CMTSPEED header is
     * the first block; the real UniCMT does not play such a tape"; hlavička
     * s nulovou délkou pulzu se zahodí s varováním "warning: CMTSPEED header at
     * offset 0x... has a zero pulse length, ignored" (texty jako u dřívějšího
     * rozšíření MZT, které už neexistuje, bez předpony se jménem rozšíření). Zbytek
     * kratší než hlavička na konci dat se ignoruje (tmz_mzt_parse_tail()).
     * Chyby: "Can't parse MZT file" (tělo přes konec dat, žádná celá
     * hlavička), "No playable block in MZT file" (jen hlavičky CMTSPEED).
     *
     * Bloky MZF dostanou pauzu opts->pause_ms, poslední blok pásky (u MZF
     * jediný) pauzu 0: páska po něm skončí hned jako u dosavadních rozšíření
     * MZF a MZT a čas do zastavení pásky se nemění.
     *
     * TAP: každý blok TAP ([2 B délka LE][flag + data + checksum]) jeden blok
     * 0x11 (Turbo Speed Data) s časováním dnešního zxtape při 1:1 (pilot,
     * sync, bity v T-stavech při 3,5 MHz; pilot hlavičky nebo dat podle
     * flagu). Pauza jako dnešní rozšíření TAP: 350 ms za blokem s flagem
     * hlavičky (< 80h), 1000 ms za datovým blokem, poslední blok pásky 0.
     * Délka nebo data za koncem souboru dají varování "TAP file has a damaged
     * end, incomplete block ignored" a načtené bloky zůstanou; blok délky 0 se
     * přeskočí s varováním "TAP file contains an empty block, ignored". Bez
     * celého bloku chyba "No playable block in TAP file".
     * Dvě přijaté odchylky od dnešního zxtape (délky všech běhů jinak sedí
     * na vzorek): (a) signál bloku 0x11 má opačnou úroveň než zxtape, protože
     * player začíná blok TZX úrovní TZX_LEVEL_AFTER_PAUSE (první pulz HIGH)
     * a blok 0x11 si počáteční úroveň zvolit neumí, kdežto zxtape začíná
     * půlvlnou LOW; dopad na zavaděče ZX běžící na MZ je [neověřeno];
     * (b) za posledním blokem pásky chybí závěrečných 27 vzorků (při 44,1 kHz,
     * jedna půlvlna pilotu) úrovně LOW, které zxtape přidává za data.
     *
     * WAV: knihovna wav (PCM, IEEE float, EXTENSIBLE), jen první kanál,
     * prahování na 1 bit stejně jako dosavadní rozšíření WAV
     * (wav_get_bit_value_of_sample(): normální polarita = záporný vzorek
     * HIGH, obrácená = kladný vzorek HIGH, nula LOW) s opts->wav_polarity.
     * Délky běhů ve vzorcích tvoří jeden blok 0x18 (CSW, RLE) s frekvencí
     * souboru a pauzou 0; signál začínající HIGH má na začátku pulz LOW
     * délky 1 vzorek. Chyby: "Can't read WAV file: " + wav_error_string(),
     * "WAV contains no signal" (žádná hrana), "Can't create CSW block: " +
     * tzx_error_string() (např. frekvence nad 24 bitů). Data chunk delší než
     * soubor dá varování "WAV data chunk is longer than file, missing samples
     * ignored"; hlavička s počtem kanálů 0 dá chybu "WAV header is corrupted".
     *
     * TMZ/TZX: tzx_load(); výsledek si nechá hlavičku načteného souboru
     * (TapeMZ! i ZXTape!). Chyba knihovny jde do rep->error textem
     * tzx_error_string(), soubor bez bloků dá "No playable block in TMZ file".
     *
     * Neplatný zdroj dá chybu "Unknown tape source".
     *
     * Selhání sestavení bloku (neplatné opts) dá chybu "Can't create MZF tape
     * block: " + text chyby registru profilů (tmz_profile_error_string()),
     * selhání alokace "Not enough memory".
     *
     * @param source Formát dat.
     * @param data Data (NULL jen při size 0).
     * @param size Délka dat v bajtech.
     * @param opts Volby (NULL = výchozí, cmttmz_import_opts_init()).
     * @param[out] rep Report (smí být NULL); funkce ho na začátku vynuluje
     *        a nastaví rep->source = source.
     * @return Nový soubor (uvolnit tzx_free()), nebo NULL při chybě.
     * @pre Proběhlo memory_driver_init() (dělá ho start emulátoru i mztest_init()).
     * @pre Volá se z vlákna emulátoru (viz cmttmz_import_file()).
     * @post data se nemění a výsledek na ně neodkazuje (bloky mají vlastní kopie).
     */
    extern st_TZX_FILE* cmttmz_import_buffer ( en_CMTTMZ_SOURCE source, const uint8_t *data, uint32_t size,
                                               const st_CMTTMZ_IMPORT_OPTS *opts, st_CMTTMZ_IMPORT_REPORT *rep );


    /**
     * @brief cmttmz_import_buffer() a k tomu rychlost, se kterou import sestavil každý blok.
     *
     * Výsledný soubor je stejný jako z cmttmz_import_buffer(). Záznamy:
     * MZF: CMTTMZ_SPEED_ORIGIN_DEFAULT s opts->default_speed (a při
     * CMTSPEED_CUSTOM s opts->custom_pulses), pause_ms 0. MZT: bloky před
     * první platnou hlavičkou CMTSPEED DEFAULT jako MZF, bloky za ní
     * CMTTMZ_SPEED_ORIGIN_FIXED s CMTSPEED_CUSTOM a pulzy z hlavičky; pause_ms
     * podle pauzy bloku (opts->pause_ms, poslední blok 0). TAP:
     * CMTTMZ_SPEED_ORIGIN_FIXED s CMTSPEED_1_1 a pauzou bloku (350, 1000,
     * poslední 0 ms). WAV a bloky TMZ/TZX: CMTTMZ_SPEED_ORIGIN_NONE, kromě
     * bloku 0x46 s profilem MZ800EMU-CMT a čitelnými PROFILE_ARGS (rychlost
     * speed=a:b, nebo cmtspeed=LH/LL/SH/SL v µs, a pause=N do 65535 ms): ten
     * je CMTTMZ_SPEED_ORIGIN_FIXED s rychlostí a pauzou z PROFILE_ARGS
     * (emulátor ho umí přestavět jinou rychlostí).
     *
     * @param source Formát dat.
     * @param data Data (NULL jen při size 0).
     * @param size Délka dat v bajtech.
     * @param opts Volby (NULL = výchozí).
     * @param[out] rep Report (smí být NULL), jako cmttmz_import_buffer().
     * @param[out] speeds Při úspěchu nové pole file->block_count záznamů
     *        (uvolnit free()); při chybě NULL. Smí být NULL, pak se pole
     *        nevytváří a funkce se chová jako cmttmz_import_buffer().
     * @return Nový soubor (uvolnit tzx_free()), nebo NULL při chybě (i při
     *         selhání alokace pole, pak "Not enough memory").
     * @pre Stejné jako cmttmz_import_buffer() (vlákno emulátoru, memory_driver_init()).
     */
    extern st_TZX_FILE* cmttmz_import_buffer_speeds ( en_CMTTMZ_SOURCE source, const uint8_t *data, uint32_t size,
                                                      const st_CMTTMZ_IMPORT_OPTS *opts, st_CMTTMZ_IMPORT_REPORT *rep,
                                                      st_CMTTMZ_BLOCK_SPEED **speeds );


    /**
     * @brief cmttmz_import_file() a k tomu rychlost, se kterou import sestavil každý blok.
     *
     * Soubor se načte jako v cmttmz_import_file(), záznamy rychlosti jako
     * v cmttmz_import_buffer_speeds().
     *
     * @param path Cesta k souboru (UTF-8).
     * @param opts Volby (NULL = výchozí).
     * @param[out] rep Report (smí být NULL).
     * @param[out] speeds Při úspěchu nové pole file->block_count záznamů
     *        (uvolnit free()); při chybě NULL. Smí být NULL.
     * @return Nový soubor (uvolnit tzx_free()), nebo NULL při chybě.
     * @pre Stejné jako cmttmz_import_file().
     */
    extern st_TZX_FILE* cmttmz_import_file_speeds ( const char *path, const st_CMTTMZ_IMPORT_OPTS *opts,
                                                    st_CMTTMZ_IMPORT_REPORT *rep, st_CMTTMZ_BLOCK_SPEED **speeds );


    /**
     * @brief Sestaví z MZF blok 0x46 s profilem MZ800EMU-CMT.
     *
     * Bajty hlavičky se do bloku přenesou beze změny (jen fsize, fstrt
     * a fexec v pořadí souboru, fname.terminator 11h tak, jak přišel).
     *
     * Rychlost: CMTSPEED_NONE nebo platný poměr se předá profilu jako
     * speed (NONE = 1:1). CMTSPEED_CUSTOM použije custom_pulses: každá délka
     * se převede na celé µs zaokrouhlením lround ( sekundy * 1e6 ) a musí
     * vyjít 1 až 65535 µs (profil nese pulzy jen v celých µs; vlastní pulzy
     * s desetinami µs se tím zaokrouhlí).
     *
     * @param hdr Hlavička MZF v pořadí hostitele (jak ji vrací knihovna mzf).
     * @param body Tělo (NULL jen při body_size 0).
     * @param body_size Délka těla; musí se rovnat hdr->fsize.
     * @param speed Rychlost bloku.
     * @param custom_pulses Vlastní délky pulzů v sekundách; povinné jen při
     *        speed == CMTSPEED_CUSTOM, jinak se ignorují (smí být NULL).
     * @param pause_ms Pauza za blokem v ms (úroveň LOW).
     * @return Nový blok (uvolnit tmz_block_free(); po předání do
     *         tzx_file_append_block() uvolnit jen strukturu), nebo NULL při
     *         chybě: NULL hdr, NULL body s nenulovou délkou, body_size různé od
     *         fsize, neplatná rychlost, CMTSPEED_CUSTOM bez pulzů nebo s pulzem
     *         mimo rozsah, chyba registru profilů (alokace).
     */
    extern st_TZX_BLOCK* cmttmz_build_mzf_block ( const st_MZF_HEADER *hdr, const uint8_t *body, uint32_t body_size,
                                                  en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *custom_pulses,
                                                  uint16_t pause_ms );

    /**
     * @brief Sestaví z bloku TAP blok 0x11 (Turbo Speed Data) s časováním zxtape v dané rychlosti.
     *
     * Délky půlvln jsou konstanty dnešního zxtape při 1:1 (pilot, sync 1,
     * sync 2, bit 0, bit 1; cmt_tmz_import.c, CMTTMZ_ZXTAPE_*) vydělené
     * dělitelem rychlosti g_cmtspeed_divisor[speed] stejně jako v zxtape
     * (zxtape_create_cmt_vstream_from_tapblock(), zxtape.c:559-573: délka
     * = round ( sekundy * frekvence / dělitel )), zde v T-stavech při 3,5 MHz
     * zaokrouhlených k nejbližšímu. Počet půlvln pilotu rychlost nemění
     * (zxtape_get_pilot_length(), zxtape.c:200-208): flag < 80h = 2 x
     * ZXTAPE_HEADER_PILOT_LENGTH, jinak 2 x ZXTAPE_DATA_PILOT_LENGTH
     * (zxtape jiný flag než 00h/FFh odmítne, blok 0x11 ho přehraje podle
     * pravidla TZX). Použité bity posledního bajtu 8.
     *
     * Přijaté odchylky od zxtape (délky pilotu, sync a půlvln dat jinak sedí
     * při 44,1 kHz na vzorek, test_tap_timing_vs_zxtape a
     * test_tap_block_speed_vs_zxtape): (a) signál má opačnou úroveň (player
     * začíná blok TZX úrovní TZX_LEVEL_AFTER_PAUSE, první pulz HIGH,
     * tmz_player.c:1143; zxtape začíná LOW, zxtape.c:552, 480-487), dopad na
     * zavaděče ZX běžící na MZ je [neověřeno]; (b) chybí závěrečná půlvlna
     * LOW délky pilotu, kterou zxtape přidává za data (zxtape.c:600).
     *
     * @param tap_data Data bloku TAP (flag + data + checksum; NULL = chyba).
     * @param len Délka dat (1 až 65535; 0 = chyba).
     * @param speed Rychlost: CMTSPEED_NONE (= 1:1) nebo platný poměr
     *        (cmtspeed_is_valid()); CMTSPEED_CUSTOM a neplatná hodnota = chyba.
     * @param pause_ms Pauza za blokem v ms.
     * @return Nový blok (uvolnit tmz_block_free(); data i struktura z malloc,
     *         výchozí alokátor knihoven), nebo NULL při chybě parametru,
     *         délce půlvlny mimo 1 až 65535 T nebo selhání alokace.
     * @note Čistá funkce bez globálního stavu, volatelná z libovolného vlákna.
     */
    extern st_TZX_BLOCK* cmttmz_build_tap_block ( const uint8_t *tap_data, uint16_t len, en_CMTSPEED speed,
                                                  uint16_t pause_ms );

#ifdef __cplusplus
}
#endif

#endif /* CMT_TMZ_IMPORT_H */
