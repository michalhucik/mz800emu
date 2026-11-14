/**
 * @file   tmz_player.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.6.0
 * @brief  Verejne API TMZ playeru - generovani CMT audio streamu z TMZ/TZX bloku.
 *
 * Player iteruje bloky TMZ/TZX souboru a generuje CMT audio stream
 * pomoci knihoven mztape (Sharp MZ bloky) a tzx (standardni TZX bloky).
 * Vystupem je cmt_stream (bitstream nebo vstream), ktery lze exportovat
 * do WAV nebo pridat do emulatoru jako vstup kazetove jednotky.
 *
 * Player ma dve urovne API:
 * - Per-block: tmz_player_play_block() pro prehrani jednoho bloku
 * - Stavovy automat: tmz_player_state_init() + tmz_player_play_next()
 *   pro sekvencni prehravani celeho souboru vcetne ridicich bloku
 *   (Loop Start/End, Jump, Call Sequence/Return, Set Signal Level).
 *
 * Oba typy hlavicek (TapeMZ! i ZXTape!) jsou podporovany.
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


#ifndef TMZ_PLAYER_H
#define TMZ_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "tmz.h"
#include "tmz_blocks.h"
#include "../cmt_stream/cmt_stream.h"
#include "../mztape/mztape.h"
#include "../zxtape/zxtape.h"


    /** @brief Chybove kody TMZ playeru. */
    typedef enum en_TMZ_PLAYER_ERROR {
        TMZ_PLAYER_OK = 0,              /**< operace uspesna */
        TMZ_PLAYER_ERROR_NULL_INPUT,     /**< vstupni parametr je NULL */
        TMZ_PLAYER_ERROR_NO_BLOCKS,      /**< soubor neobsahuje zadne bloky */
        TMZ_PLAYER_ERROR_UNSUPPORTED,    /**< nepodporovany typ bloku */
        TMZ_PLAYER_ERROR_STREAM_CREATE,  /**< chyba pri vytvareni CMT streamu */
        TMZ_PLAYER_ERROR_ALLOC,          /**< selhani alokace pameti */
        TMZ_PLAYER_ERROR_UNREADABLE,     /**< signál by po převodu na vzorky čtecí rutina nepřečetla */
    } en_TMZ_PLAYER_ERROR;


    /** @brief Konfigurace playeru. */
    typedef struct st_TMZ_PLAYER_CONFIG {
        uint32_t sample_rate;            /**< vzorkovaci frekvence (Hz), 0 = CMTSTREAM_DEFAULT_RATE */
        en_CMT_STREAM_TYPE stream_type;  /**< typ vystupniho streamu (bitstream/vstream) */
        en_MZTAPE_PULSESET default_pulseset; /**< vychozi pulsni sada (pokud neni v bloku) */
        en_CMTSPEED default_speed;       /**< vychozi rychlost (pokud neni v bloku) */
        en_MZTAPE_FORMATSET default_formatset; /**< vychozi format zaznamu */
        uint32_t mz_cpu_clock;           /**< takt CPU z posledního bloku 0x43 (Hz) = výchozí časová základna částí bloku 0x46 bez TIMEBASE (spec 5.3); 0 = 3 546 895 */
        int v1_convert;                  /**< 1 = bloky 0x40, 0x41, 0x45 převést v paměti na 0x46 (spec 9.1, tmz_v1conv), nepřevoditelné hrát cestou v1.0; 0 = cesta v1.0 (0x41 CPM_TAPE a 0x45 se převádějí vždy) */
    } st_TMZ_PLAYER_CONFIG;


    /**
     * @brief Inicializuje konfiguraci playeru na vychozi hodnoty.
     *
     * Nastavi: sample_rate = 44100, stream_type = vstream,
     * default_pulseset = MZ-800, default_speed = 1:1,
     * default_formatset = MZ800_SANE, mz_cpu_clock = 0 (3 546 895 Hz),
     * v1_convert = 1 (bloky v1.0 přes převod na 0x46).
     *
     * @param config Ukazatel na konfiguraci k inicializaci.
     *
     * @pre config nesmi byt NULL.
     * @post Vsechna pole konfigurace jsou nastavena na rozumne vychozi hodnoty.
     */
    extern void tmz_player_config_init ( st_TMZ_PLAYER_CONFIG *config );


    /**
     * @brief Prehaje jeden MZ Standard Data blok (0x40) na CMT stream.
     *
     * Vytvori st_MZTAPE_MZF z dat bloku a pouzije mztape knihovnu
     * pro generovani CMT streamu ve standardni rychlosti 1:1. Za signál
     * připojí pause_ms bloku jako ticho na nízké úrovni (jen vstream).
     *
     * @param block TMZ blok s ID 0x40.
     * @param config Konfigurace playeru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy CMT stream, nebo NULL pri chybe.
     *
     * @pre block nesmi byt NULL a musi mit id == TMZ_BLOCK_ID_MZ_STANDARD_DATA.
     * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit pres cmt_stream_destroy().
     */
    extern st_CMT_STREAM* tmz_player_play_mz_standard ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config, en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Prehaje jeden MZ Turbo Data blok (0x41) na CMT stream.
     *
     * Dispatcher - podle pole format v bloku zvoli prislusny koder:
     * NORMAL (mztape), TURBO (mzcmt_turbo), FASTIPL (mzcmt_fastipl),
     * FSK (mzcmt_fsk), SLOW (mzcmt_slow), DIRECT (mzcmt_direct). CPM-TAPE
     * se vždy převede na 0x46 PEZIK-2.0A (tmz_v1conv) a přehraje jako 0x46
     * (kodér v1.0 odstraněn). Za signál připojí pause_ms bloku jako
     * ticho na nízké úrovni (jen vstream).
     *
     * FSK, SLOW a DIRECT při vzorkovací frekvenci, kterou emulátor změřil
     * jako nečitelnou, vrátí NULL
     * a TMZ_PLAYER_ERROR_UNREADABLE; při nezměřené frekvenci se přehrají
     * s varováním přes error callback.
     *
     * @param block TMZ blok s ID 0x41.
     * @param config Konfigurace playeru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy CMT stream, nebo NULL pri chybe.
     *
     * @pre block nesmí být NULL a musí mít id == TMZ_BLOCK_ID_MZ_TURBO_DATA.
     * @post Volající je vlastníkem vráceného streamu a musí ho uvolnit přes cmt_stream_destroy().
     */
    extern st_CMT_STREAM* tmz_player_play_mz_turbo ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config, en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Přehraje jeden MZ BASIC Data blok (0x45) na CMT stream.
     *
     * Blok se vždy převede na 0x46 s profilem BASIC-1Z016 (tmz_v1conv_block,
     * spec 9.2) a přehraje jako 0x46, i při v1_convert = 0.
     * Generátor v1.0 (mzcmt_bsd) se nepoužívá: BASIC jeho výstup za prvním
     * kusem nepřečetl. Pauza bloku je pauzou poslední části.
     *
     * @param block TMZ blok s ID 0x45.
     * @param config Konfigurace playeru.
     * @param err Výstupní chybový kód (může být NULL): TMZ_PLAYER_ERROR_UNSUPPORTED,
     *        když blok nejde převést (poškozený, žádný kus nebo víc než 254 kusů),
     *        jinak jako tmz_player_play_mz_file().
     * @return Nový CMT stream, nebo NULL při chybě.
     *
     * @pre block nesmí být NULL a musí mít id == TMZ_BLOCK_ID_MZ_BASIC_DATA.
     * @post Volající je vlastníkem vráceného streamu a musí ho uvolnit přes cmt_stream_destroy().
     */
    extern st_CMT_STREAM* tmz_player_play_mz_basic ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config, en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Přehraje blok 0x46 MZ File (TMZ v1.1).
     *
     * Části se přehrají v pořadí do jednoho vstreamu (rodina FM přes
     * mzcmt_fm_append_vstream, rodina MANCHESTER přes
     * mzcmt_manch_append_vstream, akumulace času v každé části). Počáteční
     * úroveň vstreamu je úroveň první půlvlny první přehrané části. Část bez
     * TIMEBASE dostane časovou základnu config->mz_cpu_clock (0 = 3 546 895).
     * Část neznámé rodiny nebo s chybou TLV se nepřehraje, chyba se ohlásí
     * přes error callback a pokračuje se další částí (spec 4.4). STOP se
     * zatím nepoužívá (signál zastavení pro emulátor, krok 9 roadmapy).
     *
     * Části FM s READER se před generováním zkontrolují při výstupní
     * vzorkovací frekvenci (tmz_part_fm_check_reader, spec 5.6): rezerva
     * pod 0 = celý blok se odmítne (TMZ_PLAYER_ERROR_UNREADABLE), rezerva pod
     * TMZ_READER_MIN_MARGIN_US = varování přes error callback, část se
     * přehraje.
     *
     * Mez celého bloku se ověří před generováním (spec 5.7): součet horních
     * odhadů délky přehratelných částí nejvýš CMT_VSTREAM_MAX_BLOCK_SECONDS
     * a součet pulzů FM a bitů MANCHESTER nejvýš TMZ_PLAYER_MAX_BLOCK_PULSES;
     * blok mimo mez = TMZ_PLAYER_ERROR_UNSUPPORTED s hlášením.
     *
     * @param block Blok s ID 0x46.
     * @param config Konfigurace (vzorkovací frekvence, typ streamu, mz_cpu_clock).
     * @param[out] err TMZ_PLAYER_OK; TMZ_PLAYER_ERROR_UNSUPPORTED (neplatný blok,
     *        žádná přehratelná část nebo blok mimo mez bloku); TMZ_PLAYER_ERROR_UNREADABLE (část
     *        nečitelná při dané vzorkovací frekvenci); TMZ_PLAYER_ERROR_STREAM_CREATE;
     *        TMZ_PLAYER_ERROR_ALLOC; TMZ_PLAYER_ERROR_NULL_INPUT.
     * @return Nový stream (uvolnit cmt_stream_destroy()), nebo NULL.
     */
    extern st_CMT_STREAM* tmz_player_play_mz_file ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config,
                                                    en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Prehaje libovolny TZX audio blok na CMT stream.
     *
     * Deleguje parsovani a generovani na TZX knihovnu (tzx_generate_block).
     * Podporuje bloky: 0x10 (Standard Speed), 0x11 (Turbo Speed),
     * 0x12 (Pure Tone), 0x13 (Pulse Sequence), 0x14 (Pure Data),
     * 0x15 (Direct Recording), 0x18 (CSW Recording, jen komprese RLE),
     * 0x20 (Pause). Signál začíná nízko a první pulz je vysoko, tedy s hranou
     * (TZX_LEVEL_AFTER_PAUSE, jako ZX); úroveň mezi bloky drží jen
     * tmz_player_play_next(). Bloky 0x15 a 0x18 mají úrovně pulzů dané
     * blokem (0x18: první pulz LOW), po nich je úroveň opakem posledního pulzu.
     *
     * @param block TMZ blok s TZX audio ID.
     * @param config Konfigurace playeru.
     * @param err Vystupni chybovy kod (muze byt NULL): TMZ_PLAYER_OK (i u Pause 0);
     *        TMZ_PLAYER_ERROR_NULL_INPUT; TMZ_PLAYER_ERROR_UNSUPPORTED (jiné ID);
     *        TMZ_PLAYER_ERROR_STREAM_CREATE (i neplatný blok a 0x18 s kompresí
     *        Z-RLE); TMZ_PLAYER_ERROR_ALLOC.
     * @return Novy CMT stream, nebo NULL pri chybe / Pause 0.
     *
     * @pre block nesmi byt NULL.
     * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit pres cmt_stream_destroy().
     */
    extern st_CMT_STREAM* tmz_player_play_tzx_block ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config, en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Prehaje jeden libovolny blok na CMT stream.
     *
     * Dispatcher - podle ID bloku zavola prislusnou specializovanou funkci.
     * Ridici bloky (pause, group, loop) jsou zpracovany, ale nevytvareji stream.
     * Bloky v1.0 se při config->v1_convert převedou v paměti na 0x46. Bloky
     * 0x42, 0x44 a rezervované 0x47-0x4F se přeskočí s varováním přes
     * error callback (NULL a TMZ_PLAYER_OK). Pause 0 vrátí NULL a TMZ_PLAYER_OK;
     * signál zastavení pásky dává jen tmz_player_play_next(). Blok TZX začíná
     * úrovní TZX_LEVEL_AFTER_PAUSE (signál nízko, první pulz s hranou; úroveň
     * mezi bloky drží tmz_player_play_next()).
     *
     * @param block TMZ blok k prehrani.
     * @param config Konfigurace playeru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy CMT stream, nebo NULL pokud blok negeneruje audio (ridici bloky)
     *         nebo pri chybe.
     *
     * @pre block nesmi byt NULL.
     * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit pres cmt_stream_destroy().
     */
    extern st_CMT_STREAM* tmz_player_play_block ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config, en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Vrati textovy popis chyboveho kodu TMZ playeru.
     *
     * Vzdy vraci platny retezec (nikdy NULL).
     *
     * @param err Chybovy kod.
     * @return Textovy popis chyby.
     */
    extern const char* tmz_player_error_string ( en_TMZ_PLAYER_ERROR err );


/* ========================================================================= */
/*  Stavovy automat pro sekvencni prehravani celeho souboru                  */
/* ========================================================================= */

/** @defgroup tmz_player_state Stavovy automat playeru
 *  Sekvencni prehravani s podporou ridicich bloku (loop, jump, call).
 *  @{ */

/** @brief Maximalni hloubka vnoreni smycek (Loop Start/End). */
#define TMZ_PLAYER_MAX_LOOP_DEPTH   8

/** @brief Maximalni hloubka vnoreni volani (Call Sequence/Return). */
#define TMZ_PLAYER_MAX_CALL_DEPTH   8


    /**
     * @brief Zaznam na zasobniku smycek.
     *
     * Pri Loop Start se ulozi index bloku za Loop Start a pocet opakovani.
     * Pri Loop End se dekrementuje remaining; pokud > 0, skok na start_block.
     */
    typedef struct st_TMZ_PLAYER_LOOP_ENTRY {
        uint32_t start_block; /**< index bloku hned za Loop Start */
        uint32_t remaining;   /**< zbyvajici opakovani (0 = hotovo) */
    } st_TMZ_PLAYER_LOOP_ENTRY;


    /**
     * @brief Zaznam na zasobniku volani.
     *
     * Pri Call Sequence se ulozi index Call bloku, celkovy pocet offsetu
     * a aktualni pozice v poli offsetu. Offsety se ctou primo z bloku.
     * Pri Return se posune na dalsi offset, nebo pop pokud vsechny zpracovany.
     */
    typedef struct st_TMZ_PLAYER_CALL_ENTRY {
        uint32_t call_block;         /**< index Call Sequence bloku */
        uint16_t current_offset_idx; /**< aktualni index v poli offsetu (0-based) */
        uint16_t total_offsets;      /**< celkovy pocet offsetu v Call Sequence */
    } st_TMZ_PLAYER_CALL_ENTRY;


    /**
     * @brief Stav playeru pro sekvencni prehravani.
     *
     * Obsahuje ukazatel na soubor, konfiguraci, aktualni pozici
     * a zasobniky pro smycky (loop) a volani (call).
     *
     * @par Invarianty:
     * - current_block < file->block_count dokud !finished
     * - loop_depth >= 0 && <= TMZ_PLAYER_MAX_LOOP_DEPTH
     * - call_depth >= 0 && <= TMZ_PLAYER_MAX_CALL_DEPTH
     *
     * @par Vlastnictvi:
     * State nevlastni soubor ani konfiguraci - volajici je zodpovedny
     * za jejich zivotnost po celou dobu prehravani.
     */
    typedef struct st_TMZ_PLAYER_STATE {
        const st_TZX_FILE *file;         /**< soubor k prehrani (nevlastni) */
        st_TMZ_PLAYER_CONFIG config;     /**< konfigurace playeru (kopie) */
        uint32_t current_block;          /**< aktualni pozice v blokove sekvenci */
        uint32_t last_played_block;      /**< index naposledy prehravaneho bloku */

        /** @brief Zasobnik smycek (Loop Start/End). */
        st_TMZ_PLAYER_LOOP_ENTRY loop_stack[TMZ_PLAYER_MAX_LOOP_DEPTH];
        int loop_depth;                  /**< aktualni hloubka vnoreni smycek */

        /** @brief Zasobnik volani (Call Sequence/Return). */
        st_TMZ_PLAYER_CALL_ENTRY call_stack[TMZ_PLAYER_MAX_CALL_DEPTH];
        int call_depth;                  /**< aktualni hloubka vnoreni volani */

        bool stopped;                    /**< true = poslední tmz_player_play_next() skončilo příkazem
                                              zastavení pásky (Pause 0 nebo 0x2A) a vrátilo NULL
                                              s TMZ_PLAYER_OK; další volání pokračuje za ním a vynuluje
                                              ho. Invariant: stopped => !finished */
        uint8_t signal_level;            /**< úroveň dalšího pulzu bloku TZX (0=LOW, 1=HIGH,
                                              TZX_LEVEL_AFTER_PAUSE = signál nízko, další pulz s hranou):
                                              start TZX_LEVEL_AFTER_PAUSE, mění ji bloky TZX (po pauze
                                              TZX_LEVEL_AFTER_PAUSE) a 0x2B; po bloku MZ s výstupem
                                              vstream opak poslední hodnoty vstreamu, končí-li blok
                                              tichem na nízké úrovni (pause_ms, pauza poslední části
                                              0x46) TZX_LEVEL_AFTER_PAUSE */
        bool finished;                   /**< prehravani dokonceno */
    } st_TMZ_PLAYER_STATE;


    /**
     * @brief Inicializuje stav playeru pro prehrani daneho souboru.
     *
     * Nastavi pocatecni pozici na blok 0, prazdne zasobniky,
     * signal_level = TZX_LEVEL_AFTER_PAUSE (signál nízko, první pulz s hranou),
     * finished = false.
     *
     * @param state   Stav k inicializaci.
     * @param file    TMZ/TZX soubor (musi zustat platny po celou dobu prehravani).
     * @param config  Konfigurace playeru (zkopiruje se do stavu).
     *
     * @pre state, file, config nesmi byt NULL.
     * @post state je pripraven pro prvni volani tmz_player_play_next().
     */
    extern void tmz_player_state_init ( st_TMZ_PLAYER_STATE *state,
                                        const st_TZX_FILE *file,
                                        const st_TMZ_PLAYER_CONFIG *config );


    /**
     * @brief Prehaje dalsi audio blok v sekvenci.
     *
     * Interně zpracovava ridici bloky (Loop Start/End, Jump,
     * Call Sequence/Return, Set Signal Level, Group Start/End)
     * a vraci CMT stream az pri narazeni na audio blok.
     *
     * Úroveň signálu drží mezi bloky ve state->signal_level: blok TZX
     * začíná aktuální úrovní a po něm platí úroveň podle TZX 1.20, kromě
     * pauzy: po ní první pulz vytvoří hranu (TZX_LEVEL_AFTER_PAUSE, jako ZX),
     * takže se hrany na přechodu bloků neztrácejí.
     *
     * Pokud je prehravani dokonceno (vsechny bloky zpracovany),
     * vraci NULL s TMZ_PLAYER_OK a nastavi state->finished = true.
     *
     * Pause 0 a 0x2A zastaví pásku: NULL s TMZ_PLAYER_OK,
     * state->stopped = true (tmz_player_state_stopped()), finished zůstane
     * false; další volání pokračuje za blokem zastavení. Rezervované bloky
     * 0x47-0x4F se přeskočí s varováním.
     *
     * Index prave prehravaneho bloku je dostupny pres
     * state->last_played_block po navratu.
     *
     * @param state Stav playeru.
     * @param err   Vystupni chybovy kod (muze byt NULL).
     * @return Novy CMT stream, nebo NULL pokud konec / chyba / neni audio.
     *
     * @pre state nesmi byt NULL a musi byt inicializovany.
     * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit
     *       pres cmt_stream_destroy().
     */
    extern st_CMT_STREAM* tmz_player_play_next ( st_TMZ_PLAYER_STATE *state,
                                                  en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Nejvyšší počet pulzů FM a bitů MANCHESTER všech částí jednoho bloku 0x46.
     *
     * Mez celého bloku (součet částí, mzcmt_fm_part_bounds() /
     * mzcmt_manch_part_bounds()), ověřená před generováním spolu s délkou
     * bloku CMT_VSTREAM_MAX_BLOCK_SECONDS. Stejná hodnota jako mez jedné
     * části (MZCMT_FM_MAX_PULSES): tělo 65 535 B se 2 kopiemi má asi 1,2
     * milionu pulzů, 255 částí v mezích části by bez meze bloku smělo přes
     * 4 miliardy pulzů. Blok mimo mez = TMZ_PLAYER_ERROR_UNSUPPORTED.
     */
#define TMZ_PLAYER_MAX_BLOCK_PULSES 16000000u


    /** @brief Pojistka iterací jednoho volání tmz_player_play_next() (řídicí bloky bez bloku se signálem). */
#define TMZ_PLAYER_MAX_ITERATIONS 1000000u


    /**
     * @brief tmz_player_play_next() s rozpočtem iterací sdíleným více voláními.
     *
     * Chová se jako tmz_player_play_next(), jen pojistku proti nekonečné
     * smyčce bere z @p budget: každá iterace vnitřní smyčky (zpracování
     * jednoho bloku) ho sníží o 1. Volající, který volá player opakovaně
     * (např. přeskakuje bloky, které přehrát nejdou), tak omezí celou
     * posloupnost volání jedním rozpočtem, ne jen každé volání zvlášť.
     *
     * Vyčerpaný rozpočet (i nulový při vstupu, pokud průchod neskončil):
     * hlášení "Iteration budget exhausted", state->finished = true, NULL
     * s TMZ_PLAYER_ERROR_STREAM_CREATE (jako pojistka tmz_player_play_next()).
     *
     * @param state  Stav playeru.
     * @param[in,out] budget Zbývající iterace; po návratu snížený o spotřebované.
     * @param err    Výstupní chybový kód (může být NULL); NULL @p budget =
     *               TMZ_PLAYER_ERROR_NULL_INPUT.
     * @return Jako tmz_player_play_next().
     * @pre state je inicializovaný (tmz_player_state_init()).
     * @post Volající vlastní vrácený stream (cmt_stream_destroy()).
     */
    extern st_CMT_STREAM* tmz_player_play_next_budget ( st_TMZ_PLAYER_STATE *state, uint32_t *budget,
                                                         en_TMZ_PLAYER_ERROR *err );


    /**
     * @brief Zjisti, zda je prehravani dokonceno.
     *
     * @param state Stav playeru.
     * @return true pokud vsechny bloky byly zpracovany, false jinak.
     */
    extern bool tmz_player_state_finished ( const st_TMZ_PLAYER_STATE *state );


    /**
     * @brief Zjistí, zda poslední tmz_player_play_next() zastavilo pásku.
     *
     * Pause 0 a blok 0x2A jsou příkaz zastavení pásky (pro emulátor). Index
     * bloku zastavení je v state->last_played_block. Převod do WAV zastavení
     * ignoruje a pokračuje dalším voláním.
     *
     * @param state Stav playeru (NULL = false).
     * @return true pokud poslední volání skončilo zastavením pásky.
     */
    extern bool tmz_player_state_stopped ( const st_TMZ_PLAYER_STATE *state );

/** @} */


#ifdef __cplusplus
}
#endif

#endif /* TMZ_PLAYER_H */
