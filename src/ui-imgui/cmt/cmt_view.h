/**
 * @file   cmt_view.h
 * @brief  Kopie stavu Virtual CMT pro okno (st_CMT_VIEW), plnění pod zámkem a testovací stavy.
 *
 * Okno Virtual CMT kreslí jen z této kopie: UI vlákno ji jednou za snímek
 * vyplní funkcí cmt_view_fill() s drženým device_state_lock() a kreslicí
 * kód pak g_cmt, g_cmttmz ani index pásky nečte. Kopie nevlastní nic
 * z jádra (jen std::string a std::vector), takže vysunutí nebo otevření
 * pásky jiným vláknem mezi dvěma snímky nenechá v kopii žádný ukazatel do
 * uvolněné paměti.
 *
 * Testy (tests/ui/test_cmt_window.cpp) vyplní kopii i bez jádra funkcí
 * cmt_view_make_fixture(): stavy odpovídají konceptům v2 okna
 * (gui/concepts/cmt-*-v2.png v emu-experiments).
 *
 * Texty v kopii (type_text, status_text, speed_text, jména bez MZF) jsou
 * přeložené (_()) v okamžiku plnění; kopie patří UI vrstvě.
 *
 * Délky bloků (length_s, tape_total_s): otevření pásky je nepočítá (není
 * kvůli oknu pomalejší). Známá je hned délka otevřeného bloku a délky, které
 * jádro spočítá levně (surový blok 0x18 a bloky TZX z parametrů, jen při
 * připojení záznamu, návratu na značku a v zásahu); blok, jehož stream se
 * vygeneroval pro přehrávání, dostane délku v tu chvíli. Délky nikdy
 * neotevřených bloků MZ se doplňují jen při zastaveném decku (STOP)
 * zásahy cmt_view_request_lengths(); do té doby je délka bloku i pásky
 * neznámá (-1).
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

#ifndef CMT_VIEW_H
#define CMT_VIEW_H

#include <stdint.h>
#include <string>
#include <vector>

#include "emulator/hw-generic/cmt/cmt_tmz_meta.h"


/** @brief Stav transportu v kopii (st_CMT_VIEW::state). */
typedef enum en_CMT_VIEW_STATE {
    CMT_VIEW_STATE_STOP = 0, /**< transport stojí (i bez pásky) */
    CMT_VIEW_STATE_PLAY,     /**< přehrávání (i v pauze, st_CMT_VIEW::paused) */
    CMT_VIEW_STATE_RECORD,   /**< nahrávání (i v pauze) */
} en_CMT_VIEW_STATE;


/** @brief Režim načítání (st_CMT_VIEW::mode), přepínač Tape / Boost / Direct. */
typedef enum en_CMT_VIEW_MODE {
    CMT_VIEW_MODE_TAPE = 0, /**< signál v reálném čase (CMT hack vypnutý, CPU Boost vypnutý) */
    CMT_VIEW_MODE_BOOST,    /**< signál při MAX SPEED během běhu motoru (CPU Boost zapnutý, hack vypnutý) */
    CMT_VIEW_MODE_DIRECT,   /**< CMT hack: program se kopíruje do paměti bez signálu (má přednost před Boost) */
} en_CMT_VIEW_MODE;


/** @brief Druh bloku (st_CMT_VIEW_BLOCK::kind). */
typedef enum en_CMT_VIEW_KIND {
    CMT_VIEW_KIND_MZF = 0,    /**< blok nese MZF (TMZ 0x40, 0x41, 0x46) */
    CMT_VIEW_KIND_TAP_HEADER, /**< hlavička ZX TAP */
    CMT_VIEW_KIND_TAP_DATA,   /**< data ZX TAP */
    CMT_VIEW_KIND_RAW,        /**< surový signál: blok 0x18 (záznam emulátoru nebo import WAV) */
    CMT_VIEW_KIND_TZX,        /**< jiný blok se signálem (0x10-0x15, 0x20 s pauzou, 0x45, MZ bez čitelného MZF) */
    CMT_VIEW_KIND_CONTROL,    /**< řídicí nebo informační blok bez signálu */
} en_CMT_VIEW_KIND;


/** @brief Režim rychlosti bloku (st_CMT_VIEW_BLOCK::speed_kind). */
typedef enum en_CMT_VIEW_SPEED_KIND {
    CMT_VIEW_SPEED_DEFAULT = 0, /**< blok hraje výchozí rychlostí Virtual CMT */
    CMT_VIEW_SPEED_SET,         /**< blok má vlastní rychlost (index pásky) */
    CMT_VIEW_SPEED_NONE,        /**< rychlost dává soubor nebo záznam, měnit nejde */
} en_CMT_VIEW_SPEED_KIND;


/**
 * @brief Stav bloku pro sloupec Status (st_CMT_VIEW_BLOCK::status).
 *
 * Priorita při plnění z jádra: UNSAVED, CONTROL, UNPLAYABLE, NOT_DECODED, OK
 * (dřív rozhodnutí D-G3-2 dávalo přednost NOT_DECODED).
 * UNSAVED se určuje z příznaku bloku (st_CMT_VIEW_BLOCK::unsaved), ne
 * z polohy bloku v pásce: každý neuložený blok (nahraný surový
 * záznam, importovaný řídicí nebo nepřehratelný blok) ukazuje Unsaved,
 * a stav tak přesně odpovídá tomu, na co se ptá modál mazání.
 *
 * @invariant status == CMT_VIEW_STATUS_UNSAVED <=> unsaved (u bloků z jádra).
 */
typedef enum en_CMT_VIEW_STATUS {
    CMT_VIEW_STATUS_OK = 0,      /**< blok v pořádku */
    CMT_VIEW_STATUS_NOT_DECODED, /**< surový signál, který se nedekódoval na MZF */
    CMT_VIEW_STATUS_UNSAVED,     /**< blok není v souboru pásky (nahraný, importovaný); přednost před ostatními stavy */
    CMT_VIEW_STATUS_UNPLAYABLE,  /**< player blok přehrát neumí ("Cannot play") */
    CMT_VIEW_STATUS_CONTROL,     /**< řídicí blok (bez signálu); status_text je popis bloku ("Loop start x3", "Stop the tape", ...) */
    CMT_VIEW_STATUS_CHECKSUM_ERROR, /**< chyba kontrolního součtu ("Checksum error" místo dřívějšího "CRC error"); jádro ho dnes neurčuje (jen fixture konceptu v2) */
} en_CMT_VIEW_STATUS;


/** @brief Původ bloku (st_CMT_VIEW_BLOCK::source). */
typedef enum en_CMT_VIEW_SOURCE {
    CMT_VIEW_SOURCE_IMPORTED_MZF = 0, /**< blok MZF vznikl importem souboru MZF / MZT */
    CMT_VIEW_SOURCE_RECORDED,         /**< blok připojil záznam v této relaci pásky */
    CMT_VIEW_SOURCE_TAPE_FILE,        /**< blok je v souboru pásky tak, jak je (TMZ, TZX, TAP, WAV) */
    CMT_VIEW_SOURCE_IMPORTED,         /**< neuložený blok vložený importem souboru do pásky ("Imported") */
} en_CMT_VIEW_SOURCE;


/**
 * @brief Jeden blok pásky v kopii pro okno.
 *
 * @invariant id je index bloku v souboru pásky (pořadí ve st_CMT_VIEW::blocks).
 * @invariant has_mzf <=> kind == CMT_VIEW_KIND_MZF; bez MZF jsou fsize,
 *            fstrt, fexec -1.
 * @invariant length_s < 0 <=> délka není známá (UI ukáže "-").
 * @invariant kind == CMT_VIEW_KIND_CONTROL => !playable && !selectable.
 */
typedef struct st_CMT_VIEW_BLOCK {
    int id;                          /**< index bloku v souboru pásky (od 0) */
    en_CMT_VIEW_KIND kind;           /**< druh bloku */
    std::string name;                /**< jméno pro UI (UTF-8): MZF ze Sharp MZ ASCII, jinak druh bloku */
    std::string type_text;           /**< typ slovně, např. "01 machine code"; "-" bez typu */
    bool has_mzf;                    /**< blok nese MZF */
    int fsize;                       /**< velikost těla MZF v B (-1 bez MZF) */
    int fstrt;                       /**< adresa načtení MZF (-1 bez MZF) */
    int fexec;                       /**< adresa spuštění MZF (-1 bez MZF) */
    en_CMT_VIEW_SPEED_KIND speed_kind; /**< režim rychlosti */
    int cmtspeed;                    /**< vlastní rychlost bloku (en_CMTSPEED) u CMT_VIEW_SPEED_SET, jinak CMTSPEED_NONE */
    bool speed_is_mz;                /**< rychlost se vybírá ze seznamu MZ (true) nebo ZX (false, bloky TAP) */
    std::string speed_text;          /**< rychlost, kterou blok hraje ("1:1", "Custom pulses"); "" u NONE */
    double pulses_us[4];             /**< délky pulzů v µs: long high, long low, short high, short low; 0 bez pulzů MZ */
    double length_s;                 /**< délka signálu bloku v s včetně ticha za blokem; -1 neznámá (nikdy neotevřený blok MZ před zásahem ve STOP) */
    en_CMT_VIEW_STATUS status;       /**< stav pro sloupec Status */
    std::string status_text;         /**< text stavu ("OK", "Not decoded", "Unsaved", "Cannot play"; u řídicího bloku jeho popis) */
    en_CMT_VIEW_SOURCE source;       /**< původ bloku */
    uint32_t sample_rate;            /**< surový blok 0x18: vzorkovací frekvence v Hz, jinak 0 */
    uint32_t pulse_count;            /**< surový blok 0x18: počet pulzů, jinak 0 */
    bool playable;                   /**< blok dává signál (ne řídicí, ne nepřehratelný) */
    bool selectable;                 /**< blok jde vybrat k přehrání (dvojklik, Play from here) */
    bool unsaved;                    /**< blok není v souboru pásky (záznam, import; příznak jádra cmttmz_block_is_unsaved(), nezávisle na poloze v pásce): smazání se ptá */
    bool profile_convertible;        /**< blok 0x46 jde převést na jiný profil zapisovače (cmttmz_block_profile_convertible()); false u ostatních bloků */
    std::string profile_id;          /**< ID profilu bloku 0x46 (cmttmz_block_profile()), "" u ostatních bloků a u bloku, který nejde přečíst */
} st_CMT_VIEW_BLOCK;


/**
 * @brief Kopie stavu Virtual CMT pro jedno vykreslení okna.
 *
 * Vlastní všechna svá data (řetězce, vektor bloků); platí, dokud ji
 * cmt_view_fill() nebo cmt_view_make_fixture() nepřepíše.
 *
 * @invariant !has_tape => blocks.empty(), block_count == 0, current_block == -1,
 *            state == CMT_VIEW_STATE_STOP.
 * @invariant block_count == (int) blocks.size().
 * @invariant -1 <= current_block < block_count; při RECORD current_block ==
 *            block_count (blok, který záznam teprve vytváří; v blocks není).
 * @invariant tape_total_s < 0 => délka pásky není známá; tape_elapsed_s < 0
 *            => pozice od začátku pásky není známá.
 * @invariant 0 <= block_progress <= 1.
 */
typedef struct st_CMT_VIEW {
    /* páska */
    bool has_tape;                   /**< páska vložená */
    std::string tape_name;           /**< základ jména souboru (UTF-8); "" u nové pásky bez souboru */
    std::string tape_path;           /**< celá cesta souboru pásky; "" bez souboru */
    bool modified;                   /**< páska má neuložené změny */
    unsigned open_serial;            /**< čítač otevření bloků jádra (cmttmz_get_open_serial()) v době plnění */
    unsigned tape_serial;            /**< čítač výměn pásky (cmttmz_get_tape_serial()): změna = čísla bloků neplatí, okno ruší výběr řádku */
    unsigned change_serial;          /**< čítač změn údajů bloků (cmttmz_get_change_serial()) v době sestavení blocks */
    /* transport */
    en_CMT_VIEW_STATE state;         /**< STOP / PLAY / RECORD */
    bool paused;                     /**< páska stojí v PLAY nebo RECORD (pauza uživatele nebo stojící motor) */
    bool user_paused;                /**< pauza uživatele (g_cmt.user_paused): okno ukáže "Paused" a tlačítko Play; bez ní a s paused "Waiting for motor" */
    bool motor_on;                   /**< motor běží (platí jen s motor_known) */
    bool motor_known;                /**< stav motoru je znám (ve STOP bez CMT hacku ne) */
    /* pozice */
    int block_count;                 /**< počet bloků pásky */
    int current_block;               /**< otevřený blok (index), -1 bez bloku; při RECORD block_count */
    double tape_elapsed_s;           /**< pozice od začátku pásky v s (součet délek bloků před aktuálním + pozice v něm); -1 neznámá */
    double tape_total_s;             /**< délka pásky v s (součet délek bloků); -1 neznámá */
    double block_pos_s;              /**< pozice v aktuálním bloku v s (známá i bez délek bloků, cmt_tape_get_ui_position()); -1 neznámá (bez bloku, RECORD) */
    double block_progress;           /**< průběh aktuálního bloku 0..1 (0 bez známé délky) */
    /* záznam */
    double rec_time_s;               /**< čas probíhajícího záznamu v s (bez doby stojícího motoru); 0 mimo RECORD */
    uint32_t rec_pulses;             /**< počet zaznamenaných pulzů; 0 mimo RECORD */
    /* režim a rychlost */
    en_CMT_VIEW_MODE mode;           /**< Tape / Boost / Direct */
    bool direct_block_loadable;      /**< v Direct: příští LOAD vezme aktuální blok (nese MZF a hack na něm nezjistil konec pásky) */
    bool echo;                       /**< echo kazety zapnuté (g_cmt.echo): přepínač Echo decku je zamáčknutý */
    bool direct_supported;           /**< přímé načtení (CMT hack) s vybranou ROM jde: hack je zapnutý nebo ho ROM podporuje (cmthack_rom_supported()); táž podmínka jako odmítnutí CMT_LOAD_DIRECTLY_NO_HACK. false = segment Direct a Load directly zakázané s důvodem cmt_view_direct_unsupported_reason() */
    int default_speed;               /**< výchozí rychlost Virtual CMT (en_CMTSPEED) */
    std::string default_speed_text;  /**< text výchozí rychlosti ("1:1", "Custom pulses") */
    double default_pulses_us[4];     /**< vlastní pulzy výchozí rychlosti v µs (g_cmt.mz_custom_pulses): long high, long low, short high, short low; počáteční hodnoty editoru Custom... */
    /* záznam a přehrávání vstupů (g_replay_mode) */
    bool replay_recording;           /**< běží záznam vstupů: vložení jiné pásky, rychlost a CMT hack se zamykají */
    bool replay_playing;             /**< přehrává se záznam vstupů: pásku ovládá záznam, okno je celé zamčené */
    /* zprávy */
    std::string message;             /**< poslední zpráva pro řádek zpráv (zůstává, dokud ji nenahradí další) */
    bool message_is_error;           /**< poslední zpráva je chyba (barva error) */
    bool message_is_warning;         /**< poslední zpráva je varování (barva warn; např. zahozené nahrávání); nikdy současně s message_is_error */
    unsigned record_serial;          /**< čítač výsledků záznamu při posledním plnění (interní; ohlášení výsledku řídí cmt_view_poll_record()) */
    unsigned post_serial;            /**< čítač zpráv cmt_view_post_message(), ke kterému patří message (interní, pro cmt_view_fill()) */
    /* bloky */
    std::vector<st_CMT_VIEW_BLOCK> blocks; /**< bloky pásky (sestaví se znovu jen při změně open_serial nebo change_serial) */
    bool lengths_pending;            /**< některý blok se signálem má neznámou délku (nikdy neotevřený blok MZ, změna rychlosti) */
    unsigned lengths_request_serial; /**< change_serial, při kterém cmt_view_request_lengths() naposledy zařadila zásah (interní) */
    unsigned lengths_frames;         /**< volání cmt_view_request_lengths() (snímků) od posledního zařazení zásahu (interní, nasycené) */
} st_CMT_VIEW;


/** @brief Nejméně snímků (volání cmt_view_request_lengths()) mezi dvěma zásahy výpočtu délek. */
#define CMT_VIEW_LENGTHS_INTERVAL_FRAMES 10u

/**
 * @brief Po kolika snímcích se zásah zopakuje, když předchozí nic nezměnil.
 *
 * Zásah odmítnutý emu vláknem (záznam nebo přehrávání vstupů začaly mezi
 * zařazením a provedením) nebo zahozený (device_request_clear()) čítač
 * změn nezvedne; bez opakování by délky zůstaly neznámé do další změny.
 */
#define CMT_VIEW_LENGTHS_RETRY_FRAMES 50u


/** @brief Testovací stavy kopie (cmt_view_make_fixture()), podle konceptů v2. */
typedef enum en_CMT_VIEW_FIXTURE {
    CMT_VIEW_FIXTURE_PLAYING_FULL = 0, /**< cmt-full-v2: games.tmz *, 5 bloků, PLAY, pozice blok 2 (FLAPPY) 40 %, 01:05.8 */
    CMT_VIEW_FIXTURE_PLAYING_COMPACT,  /**< cmt-compact-v2: tentýž stav jako PLAYING_FULL */
    CMT_VIEW_FIXTURE_NO_TAPE,          /**< cmt-compact-notape-v2: bez pásky */
    CMT_VIEW_FIXTURE_EMPTY_TAPE,       /**< cmt-full-empty-v2: nová prázdná páska, STOP, motor neznámý */
    CMT_VIEW_FIXTURE_RECORDING,        /**< cmt-full-recording-v2: RECORD, 5 bloků + vzniká 6., 00:12.4, 1 234 pulzů */
    CMT_VIEW_FIXTURE_WAITING_MOTOR,    /**< cmt-compact-waiting-v2: PLAY v pauze, motor off, blok 2, 00:21.0 */
    CMT_VIEW_FIXTURE_DIRECT,           /**< cmt-compact-direct-v2: režim Direct, blok 2 (FLAPPY) nese MZF */
    CMT_VIEW_FIXTURE_LONG_TEXTS,       /**< jméno souboru 200 znaků, jméno bloku 16 znaků, prázdné jméno, znaky mimo ASCII */
    CMT_VIEW_FIXTURE_MANY_BLOCKS,      /**< 300 bloků MZF, PLAY na bloku 150 */
    CMT_VIEW_FIXTURE_CONTROL_BLOCKS,   /**< loops.tzx: MZF, smyčka x3, MZF, konec smyčky, skok, blok, který player neumí, zastavení pásky; STOP */
    CMT_VIEW_FIXTURE_PAUSED,           /**< games.tmz, PLAY v pauze uživatele, motor běží, blok 2 (FLAPPY) 40 % */
    CMT_VIEW_FIXTURE_COUNT             /**< počet stavů (sentinel) */
} en_CMT_VIEW_FIXTURE;


/**
 * @brief Vyplní kopii stavu Virtual CMT z jádra (jednou za snímek).
 *
 * Skalární stav (páska, transport, motor, pozice, záznam, režim, rychlost)
 * se kopíruje vždy. Seznam bloků se sestaví znovu jen tehdy, když se od
 * minulého plnění změnil čítač otevření (cmttmz_get_open_serial()) nebo
 * čítač změn údajů bloků (cmttmz_get_change_serial(): délky, uložení,
 * rychlost, nepřehratelnost); jinak se v něm jen nic nemění. Bez pásky je
 * seznam prázdný. Zpráva (message) se přepíše jen novou zprávou
 * cmt_view_post() (čítač zpráv); nový výsledek nahrávání plnění ohlásí
 * přes cmt_view_poll_record() a zahozenou operaci okna přes
 * cmt_view_poll_dropped(). Přímo ji nastavuje cmt_view_set_message().
 *
 * @param v Kopie (nesmí být NULL); může obsahovat předchozí stav.
 * @pre UI vlákno drží device_state_lock() (index, cesta a jména pásky se
 *      mění jen pod ním).
 * @post v splňuje invarianty st_CMT_VIEW; neobsahuje žádný ukazatel do jádra.
 */
void cmt_view_fill ( st_CMT_VIEW *v );

/**
 * @brief Přečte informace o vložené pásce pro formulář Tape information.
 *
 * Čtení na vyžádání (otevření dialogu), ne každý snímek: zavolá
 * cmttmz_tape_info_get(). Kopie nese tape_serial, se kterým okno zařadí
 * zápis (cmt_request_tape_info_set()); zápis s jiným čítačem jádro odmítne.
 *
 * @param[out] out Informace (nesmí být NULL; při false vynulované, machine -1).
 * @return true = přečteno; false bez vložené pásky.
 * @pre UI vlákno drží device_state_lock() (jako cmt_view_fill()).
 * @post Kopie neobsahuje žádný ukazatel do jádra.
 */
bool cmt_view_read_tape_info ( st_CMTTMZ_TAPE_INFO *out );

/**
 * @brief Zkontroluje vloženou pásku pro okno Tape check.
 *
 * Čtení na vyžádání (otevření okna, Check again), ne každý snímek: zavolá
 * cmttmz_tape_check() a nálezy zkopíruje.
 *
 * @param[out] items Nálezy seřazené podle bloku (nesmí být NULL; přepíše se).
 * @param[out] blocks_checked Počet zkontrolovaných bloků (smí být NULL).
 * @return true = zkontrolováno (i bez nálezu); false bez vložené pásky (items prázdné).
 * @pre UI vlákno drží device_state_lock() (jako cmt_view_fill()).
 */
bool cmt_view_read_tape_check ( std::vector<st_CMTTMZ_CHECK_ITEM> *items, uint32_t *blocks_checked );

/**
 * @brief Přečte čítač výměn pásky, počet bloků a zda je páska vložená (okno Tape check pozná zastaralé nálezy).
 *
 * Levné čtení čítače (cmttmz_get_tape_serial()) a počtu bloků kontejneru
 * bez sestavení kopie stavu; okno kontroly ho volá každý snímek, nálezy
 * samy ne. Počet bloků je potřeba vedle čítače: připojení záznamu na konec
 * pásky čítač výměn nezvedá, jen přidá bloky. Blok,
 * který záznam teprve vytváří, se nepočítá (přibude až po STOP).
 *
 * @param[out] serial Čítač výměn pásky (smí být NULL).
 * @param[out] block_count Počet bloků pásky, 0 bez pásky (smí být NULL).
 * @return true = páska je vložená.
 * @pre UI vlákno drží device_state_lock() (jako cmt_view_fill()).
 */
bool cmt_view_read_tape_serial ( unsigned *serial, int *block_count );

/**
 * @brief Sestaví jeden blok vložené pásky stejně jako cmt_view_fill() (jméno bloku v řádku nálezu okna Tape check).
 *
 * Čtení na vyžádání (okno kontroly ho volá jen pro bloky s nálezem, ne
 * pro celou pásku); nečte transport ani zprávy a nic neohlašuje.
 *
 * @param block_id Index bloku. @param[out] out Blok (nesmí být NULL; při false beze změny).
 * @return true = blok existuje; false bez pásky nebo mimo rozsah.
 * @pre UI vlákno drží device_state_lock().
 * @post Blok neobsahuje žádný ukazatel do jádra.
 */
bool cmt_view_read_block ( int block_id, st_CMT_VIEW_BLOCK *out );

/**
 * @brief Má vložená páska právě jeden blok a je to surový záznam (CMT_VIEW_KIND_RAW)? (nápověda k analýze po otevření)
 * @return true = jeden blok, surový; false jinak i bez pásky.
 * @pre UI vlákno drží device_state_lock().
 */
bool cmt_view_tape_single_raw ( void );

/** @brief Profil zapisovače v nabídce Convert to profile (cmt_view_read_profiles()). */
typedef struct st_CMT_VIEW_PROFILE {
    std::string id;          /**< ID profilu (cmttmz_profile_get()), argument cmt_request_convert_block_on_tape() */
    std::string description; /**< popis anglicky z registru knihovny (nepřekládá se) s jednotkou µs (cmt_view_profile_description(); značka " [unverified]" zůstává) */
    bool accepts_speed;      /**< převod přijme rychlost (jinak jen výchozí rychlost profilu) */
} st_CMT_VIEW_PROFILE;

/**
 * @brief Popis profilu z registru knihovny pro uživatele.
 *
 * Text knihovny beze změny včetně značky nejistoty " [unverified]" (nejistota
 * je informace pro uživatele; okno ukazuje totéž co MCP), jen s jednotkou µs
 * místo "us" (jen "us" jako samostatné slovo hned za číslicí nebo za
 * číslicí a mezerou, např. "470/494 us" -> "470/494 µs"). Anglicky,
 * nepřekládá se.
 *
 * @param desc Popis z cmttmz_profile_get() (NULL = "").
 * @return Upravený text (UTF-8).
 */
std::string cmt_view_profile_description ( const char *desc );

/**
 * @brief Nabídka profilů zapisovače pro převod bloku (cmttmz_profile_count(), cmttmz_profile_get()).
 * @param[out] out Profily v pořadí registru (nesmí být NULL; přepíše se).
 * @note Libovolné vlákno, zámek není potřeba (nabídka se po prvním volání nemění).
 */
void cmt_view_read_profiles ( std::vector<st_CMT_VIEW_PROFILE> *out );

/**
 * @brief Požádá emu vlákno o další krok výpočtu neznámých délek bloků.
 *
 * Okno ho volá jednou za snímek po cmt_view_fill() (každé volání se počítá
 * jako snímek). Zařadí cmt_request_lengths() jen tehdy, když zároveň:
 * - v->lengths_pending (některý blok se signálem má neznámou délku);
 * - deck stojí (v->state == CMT_VIEW_STATE_STOP; ne PLAY ani PLAY
 *   v pauze, ne RECORD): krok běží na emu vláknu pod zámkem zařízení
 *   a zdržel by přehrávání a záznam;
 * - od posledního zařazení uběhlo aspoň CMT_VIEW_LENGTHS_INTERVAL_FRAMES
 *   snímků;
 * - od posledního zařazení se změnil čítač změn (předchozí krok něco
 *   spočítal, nebo je to jiná páska), nebo uběhlo aspoň
 *   CMT_VIEW_LENGTHS_RETRY_FRAMES snímků (opakování odmítnutého nebo
 *   zahozeného zásahu);
 * - neběží záznam ani přehrávání vstupů (tam by se zásah odmítl).
 * Délky nikdy neotevřených bloků tak přibývají jen při zastaveném decku,
 * po krocích CMTTMZ_LENGTHS_STEP_US (aspoň jeden blok na krok).
 *
 * @param v Kopie po cmt_view_fill() (nesmí být NULL).
 * @return true = zásah zařazen.
 * @pre UI vlákno.
 * @post Při true v->lengths_frames == 0 a v->lengths_request_serial == v->change_serial.
 */
bool cmt_view_request_lengths ( st_CMT_VIEW *v );

/**
 * @brief Nastaví poslední zprávu pro řádek zpráv okna.
 * @param v Kopie (nesmí být NULL). @param text Zpráva (UTF-8; NULL = ""). @param is_error true = chyba.
 */
void cmt_view_set_message ( st_CMT_VIEW *v, const char *text, bool is_error );

/** @brief Úroveň zprávy okna Virtual CMT (cmt_view_post()). */
typedef enum en_CMT_VIEW_MSG_LEVEL {
    CMT_VIEW_MSG_INFO = 0, /**< informace: řádek zpráv ztlumeně; při skrytém okně se zahodí */
    CMT_VIEW_MSG_WARNING,  /**< varování: řádek zpráv barvou warn; při skrytém okně modální okno zpráv (Warning) */
    CMT_VIEW_MSG_ERROR,    /**< chyba: řádek zpráv barvou error; při skrytém okně modální okno zpráv (Error) */
} en_CMT_VIEW_MSG_LEVEL;

/**
 * @brief Ohlásí zprávu okna Virtual CMT (výsledek zásahu, změna režimu, výsledek nahrávání).
 *
 * Je-li okno vidět (cmt_view_window_shown()), zprávu převezme nejbližší
 * cmt_view_fill() (čítač zpráv): nahradí poslední zprávu řádku zpráv
 * a zůstane, dokud ji nenahradí další (SPEC K18). Když okno vidět není
 * (zavřené nebo sbalené), jde varování a chyba do modálního okna zpráv
 * (imgui_message_create()) a informace se zahodí. Každá
 * událost se hlásí jen na jednom místě.
 *
 * @param text Zpráva (UTF-8, anglicky přes _(); NULL = ""). @param level Úroveň.
 * @pre UI vlákno.
 * @post Zpráva je buď v čítači zpráv pro řádek, nebo ve frontě okna zpráv, nebo zahozená (informace).
 */
void cmt_view_post ( const char *text, en_CMT_VIEW_MSG_LEVEL level );

/**
 * @brief Jako cmt_view_post() s úrovní chyba / informace (zpětně slučitelné volání).
 * @param text Zpráva (UTF-8, anglicky přes _(); NULL = ""). @param is_error true = CMT_VIEW_MSG_ERROR, jinak CMT_VIEW_MSG_INFO.
 * @pre UI vlákno.
 */
void cmt_view_post_message ( const char *text, bool is_error );

/**
 * @brief Okno Virtual CMT v tomto snímku nakreslilo obsah (je otevřené a nesbalené).
 *
 * Volá imgui_cmt_window_draw() po Begin(), které vrátilo true.
 *
 * @pre UI vlákno uvnitř snímku ImGui.
 */
void cmt_view_window_mark_shown ( void );

/**
 * @brief Okno Virtual CMT se v tomto snímku zavřelo (křížek, Alt+C) nebo je zavřené.
 *
 * Zruší viditelnost z cmt_view_window_mark_shown(): výsledek zásahu, který
 * přijde ve stejném snímku (zpracuje se až po kreslení oken), pak jde
 * podle pravidel skrytého okna (chyba a varování modálně),
 * ne do řádku zpráv zavřeného okna.
 *
 * @pre UI vlákno.
 * @post cmt_view_window_shown() vrací false, dokud okno znovu nenakreslí obsah.
 */
void cmt_view_window_mark_hidden ( void );

/**
 * @brief Je okno Virtual CMT vidět (nakreslilo obsah v tomto nebo minulém snímku ImGui)?
 *
 * Minulý snímek se počítá, protože výsledky zásahů a zprávy mohou přijít
 * dřív, než se okno v tomto snímku nakreslí. Zavřené okno se nekreslí,
 * sbalené obsah nekreslí: obojí = nevidět.
 *
 * @return true = vidět (zprávy jdou do řádku zpráv).
 * @pre UI vlákno.
 */
bool cmt_view_window_shown ( void );

/**
 * @brief Převezme nový výsledek nahrávání z jádra a ohlásí ho přes cmt_view_post().
 *
 * Volá se každý snímek (imgui_cmt_tape_dialogs(), i se zavřeným oknem)
 * a z cmt_view_fill(). Dokončené nahrávání = informace, zahozené
 * (cmttmz_get_record_discarded()) = varování. Každý
 * výsledek (čítač cmttmz_get_record_result()) se ohlásí jednou; výsledek
 * z doby před prvním voláním se neohlásí.
 *
 * @pre UI vlákno pod device_state_lock().
 */
void cmt_view_poll_record ( void );

/**
 * @brief Převezme nový výsledek přímého uložení ROM SAVE (Direct Save) a ohlásí ho přes cmt_view_post().
 *
 * Výsledek čte z jádra (cmt_direct_save_get_result()). Úspěch = informace
 * "ROM SAVE stored "NAME" (N bytes) as tape block K." (číslo bloku od 1
 * jako v okně), neúspěch = chyba. Přímé uložení není nahrávání: má vlastní
 * čítač, výsledek nahrávání (cmt_view_poll_record(), MCP last_recording)
 * nemění. Každý výsledek se ohlásí jednou; výsledek z doby před prvním
 * voláním se neohlásí.
 *
 * Volá se každý snímek (imgui_cmt_tape_dialogs(), i se zavřeným oknem)
 * a z cmt_view_fill().
 *
 * @pre UI vlákno pod device_state_lock().
 */
void cmt_view_poll_direct_save ( void );

/**
 * @brief Ohlásí operaci okna, kterou emu vlákno zahodilo kvůli změně stavu decku.
 *
 * Operace okna rozhodnuté ze starší kopie stavu (replay_host_cmt_guarded())
 * emu vlákno zahodí, změnil-li se mezitím stav decku, a zvedne čítač
 * cmt_request_get_window_drop_serial(). Při jeho změně ukáže jedno
 * varování "The tape state changed; the action was cancelled." přes
 * cmt_view_post() (řádek zpráv; při skrytém okně modálně). Každá změna
 * čítače zjištěná voláním = jedno varování, bez časového slučování; víc
 * operací zahozených mezi dvěma voláními (týž snímek) = jedno varování
 * (při skrytém okně by jinak vzniklo víc stejných modálních zpráv).
 * Operace zahozené při přehrávání záznamu vstupů čítač nemění, nehlásí se.
 * Změny před prvním voláním se neohlásí.
 *
 * Volá se každý snímek (imgui_cmt_tape_dialogs(), i se zavřeným oknem)
 * a z cmt_view_fill(); opakované volání týž stav čítače neohlásí znovu.
 *
 * @pre UI vlákno (čítač se čte atomicky, zámek není potřeba).
 * @post Změna čítače je převzatá; nejvýš jedno cmt_view_post() za volání.
 */
void cmt_view_poll_dropped ( void );

/**
 * @brief Ohlásí varování návratu pásky uživatele po přehrání záznamu vstupů.
 *
 * Rychlosti bloků se po přehrání vrací jen nad toutéž páskou; změnil-li se
 * soubor pásky mezitím na disku, jádro je nezapíše a zvedne čítač
 * replay_media_get_cmt_warning(). Při jeho změně ukáže
 * text jádra (anglicky) jako varování přes cmt_view_post(). Změny před
 * prvním voláním se neohlásí.
 *
 * Volá se každý snímek (imgui_cmt_tape_dialogs(), i se zavřeným oknem)
 * a z cmt_view_fill().
 *
 * @pre UI vlákno pod device_state_lock() (text zapisuje emu vlákno pod ním).
 * @post Změna čítače je převzatá; nejvýš jedno cmt_view_post() za volání.
 */
void cmt_view_poll_replay_warning ( void );

/**
 * @brief Zpráva řádku zpráv k otevřené pásce: "Opened <jméno>, blocks: <n>" (přeložená).
 *
 * Jediné místo formátu: skládá ji výsledek zásahu otevření
 * (cmt_tape_actions.cpp) i testovací stavy (cmt_view_make_fixture()),
 * takže snímky ukazují skutečný text aplikace. Projekt nemá ngettext,
 * text je bez množného čísla.
 *
 * @param name Jméno souboru pásky; NULL nebo "" = "(new tape)".
 * @param blocks Počet bloků.
 * @return Text zprávy.
 * @pre UI vlákno.
 */
std::string cmt_view_opened_message ( const char *name, int blocks );

/**
 * @brief Zpráva po otevření pásky s jediným surovým blokem: "Opened <jméno>, blocks: <n>. Use Analyze to find files.".
 *
 * Nápověda k analýze záznamu; jinak stejná jako cmt_view_opened_message().
 *
 * @param name Jméno souboru pásky; NULL nebo "" = "(new tape)".
 * @param blocks Počet bloků.
 * @return Text zprávy (přeložený).
 * @pre UI vlákno.
 */
std::string cmt_view_opened_raw_message ( const char *name, int blocks );

/**
 * @brief Zpráva řádku zpráv k importu souboru do pásky: "Imported <jméno>, blocks: <n>" (přeložená).
 *
 * Jediné místo formátu: skládá ji výsledek zásahu importu
 * (cmt_tape_actions.cpp) i snímky testu (řádek zpráv po importu). Bez
 * množného čísla jako cmt_view_opened_message().
 *
 * @param name Jméno souboru (bez adresáře, UTF-8); NULL = "".
 * @param blocks Počet importovaných bloků.
 * @return Text zprávy.
 * @pre UI vlákno.
 */
std::string cmt_view_imported_message ( const char *name, int blocks );

/**
 * @brief Text jádra s čísly bloků a řádků od 1, jak je ukazuje okno.
 *
 * Texty jádra (anglicky, nepřekládají se) číslují bloky a řádky od 0
 * ("Block 0 is not a raw signal block ...", "Jump to block 7 outside the
 * tape", "Row 2 cannot be added"); okno, jeho dialogy, okno Tape check
 * i řádek zpráv číslují od 1 jako sloupec # tabulky. Převádí se jen přesný
 * tvar, který jádro skládá:
 * - slovo "Block ", "block ", "Row " nebo "row " na začátku slova (za
 *   mezerou, "(" nebo na začátku textu), za ním desítkové číslo (i záporné,
 *   nejvýš 9 číslic) a za číslem konec textu nebo znak, který není ASCII
 *   písmeno ani číslice;
 * - seznam čísel ve větě, která začíná "Blocks " (na začátku textu, za "; "
 *   nebo hned za "("): každé číslo na začátku položky za ": " a ", " ("...
 *   skipped: 3, 5 and 2 more", "3 (ID 0x18), 7 (ID 0x20)"); věta končí
 *   středníkem nebo závorkou, která uzavře její úroveň.
 * Číslo se zvýší o 1. Cokoli jiného se nemění ("Block 0x18", "Block 12a",
 * "Unknown block ID 0x99", "64 B", "Subblock 2"). Text s cestou souboru
 * převádí cmt_view_core_text_path() (cesta se nesmí změnit).
 *
 * @param core Text jádra bez cesty souboru (NULL = "").
 * @return Text pro zobrazení.
 * @note Bez vedlejších účinků; nezávisí na locale (g_ascii_isalnum()).
 */
std::string cmt_view_core_text ( const char *core );

/**
 * @brief Jako cmt_view_core_text(), ale každý výskyt cesty @p path (bajtově shodný) zůstane beze změny.
 *
 * Volající zná cestu, kterou jádru předal (uložení, export, import), a tak
 * zná i strukturu zprávy ("Block 3 exported: <cesta>", "Tape saved: <cesta>
 * (Speed of block 2 ...)", "Can't import <cesta>: <důvod>"): převede se text
 * před cestou i za ní, cesta sama nikdy. Dlouhou cestu jádro zkrátí na
 * "...<konec cesty>" (chyba importu): pak se chrání nejdelší konec cesty
 * (aspoň 8 bajtů), který v textu je. Bez cesty (NULL, "") = cmt_view_core_text().
 *
 * @param core Text jádra (NULL = ""). @param path Cesta souboru ze zásahu (smí být NULL).
 * @return Text pro zobrazení.
 */
std::string cmt_view_core_text_path ( const char *core, const char *path );

/**
 * @brief Zapamatuje výběr řádku, který má okno obnovit po vlastní úpravě pásky.
 *
 * Každá úprava pásky zvedne čítač výměn pásky a okno tím výběr zruší.
 * Výsledek zásahu úpravy (imgui_cmt_device_request_result()) proto zapíše
 * blok k výběru (st_CMT_REQUEST_EDIT_RESULT::select_block) s čítačem výměn
 * pásky PO úpravě; okno ho převezme cmt_view_take_select_after_edit() při
 * nejbližším kreslení, jen když jeho kopie stavu má právě tento čítač.
 * Další volání přepíše předchozí (postupný import více souborů).
 *
 * @param tape_serial Čítač výměn pásky po úpravě (st_CMT_REQUEST_EDIT_RESULT::tape_serial).
 * @param block_id Blok k výběru; záporný = nic (zapamatované se zahodí).
 * @pre UI vlákno.
 * @post cmt_view_take_select_after_edit() vrátí @p block_id pro kopii s čítačem @p tape_serial.
 */
void cmt_view_select_after_edit ( unsigned tape_serial, int block_id );

/**
 * @brief Převezme výběr zapamatovaný cmt_view_select_after_edit() (jednou).
 *
 * Zapamatovaný výběr se vždy spotřebuje: platí jen pro kopii stavu se
 * stejným čítačem výměn pásky; s jiným čítačem (další úprava, jiná páska)
 * se zahodí a výběr se neobnoví (nad jinou páskou by vybral cizí blok).
 *
 * @param tape_serial Čítač výměn pásky kopie stavu, kterou okno kreslí.
 * @param[out] block_id Blok k výběru (jen při true).
 * @return true = výběr obnovit; false = nic zapamatované nebo jiná páska.
 * @pre UI vlákno.
 * @post Nic zapamatované nezůstane.
 */
bool cmt_view_take_select_after_edit ( unsigned tape_serial, int *block_id );

/**
 * @brief Zpráva řádku zpráv k provedené změně režimu načítání ("Loading mode: Tape / Boost / Direct").
 *
 * Jediné místo textů: výsledek zásahu cmt_request_set_loading_mode()
 * (cmt_tape_actions.cpp) i testovací stav Direct.
 *
 * @param mode Režim (en_CMT_LOADING_MODE z cmt_request.h, návratový kód zásahu).
 * @return Přeložený statický text; NULL = jiná hodnota (zásah režim nezměnil).
 */
const char *cmt_view_loading_mode_message ( int mode );

/**
 * @brief Důvod zákazu přímého načtení s vybranou ROM ("The selected ROM does not support direct loading.").
 *
 * Jediné místo textu: tooltip zakázaného segmentu Direct přepínače Loading
 * a položky Load directly kontextového menu okna (při
 * !st_CMT_VIEW::direct_supported) i zakázané položky Use CMT Hack hlavního
 * menu (menu_cmt.cpp). Text neříká "CMT hack": okno funkci ukazuje jako
 * Direct / Load directly.
 *
 * @return Přeložený statický text (nikdy NULL).
 * @pre UI vlákno.
 */
const char *cmt_view_direct_unsupported_reason ( void );

#ifdef MZTEST_HEADLESS
/**
 * @brief Jen pro testy: vynutí viditelnost okna pro směrování zpráv.
 * @param mode -1 = podle kreslení okna (výchozí), 0 = nevidět, 1 = vidět.
 */
void cmt_view_test_force_window_shown ( int mode );

/**
 * @brief Jen pro testy: zapne prodlužování přeložených textů okna o 40 % (pseudo-překlad).
 *
 * Platí pro _() a C_() v cmt_view.cpp a cmt_window.cpp (texty okna, typy
 * a stavy bloků v kopii stavu) a _L() v cmt_window.cpp; texty kopie se
 * prodlouží při jejím dalším sestavení (cmt_view_make_fixture()).
 *
 * @param on true = prodlužovat.
 */
void cmt_view_test_set_stretch ( bool on );

/** @brief Jen pro testy: je zapnuté prodlužování textů? @return true = ano. */
bool cmt_view_test_stretch ( void );

/**
 * @brief Jen pro testy: přeložený text, při zapnutém prodloužení o 40 % delší.
 *
 * Doplní ceil(0,4 x počet znaků) znaků vzatých cyklicky z písmen a číslic
 * textu samotného (šířka roste úměrně, '%' formátu se nikdy nepřidá).
 *
 * @param key Klíč mezipaměti (msgid, u kontextu "kontext\004msgid").
 * @param translated Přeložený text.
 * @return Text (ukazatel platí do konce běhu).
 */
const char *cmt_view_test_stretch_text ( const char *key, const char *translated );

/**
 * @brief Jen pro testy: _L() s prodloužením ("přeložený text##msgid"), společná pro okno a jeho dialogy.
 * @param msgid Klíč katalogu. @return Text se stabilním ID (platí do konce běhu).
 */
const char *cmt_view_test_trl ( const char *msgid );

/** @brief Jen pro testy: _() s prodloužením. @param msgid Klíč katalogu. @return Text (platí do konce běhu). */
__attribute__ ( ( format_arg ( 1 ) ) ) const char *cmt_view_test_tr ( const char *msgid );

/**
 * @brief Jen pro testy: C_() s prodloužením (emulace pgettext jako i18n.h).
 * @param ctx_msgid "kontext\004msgid". @param msgid Klíč bez kontextu (vrátí se, když katalog překlad nemá).
 * @return Text (platí do konce běhu).
 */
__attribute__ ( ( format_arg ( 2 ) ) ) const char *cmt_view_test_trc ( const char *ctx_msgid, const char *msgid );
#endif

/**
 * @brief Vynuluje kopii na stav bez pásky (jako po cmt_view_fill() bez pásky).
 * @param v Kopie (nesmí být NULL).
 * @post Čítače open_serial / change_serial jsou ~0u, takže další cmt_view_fill() seznam bloků sestaví.
 */
void cmt_view_reset ( st_CMT_VIEW *v );

/**
 * @brief Vyplní kopii testovacím stavem (jen testy a snímky; jádro nečte).
 *
 * Obsah odpovídá konceptům v2 okna (jména, počty, texty), viz
 * en_CMT_VIEW_FIXTURE. Řetězce typů a stavů jsou
 * přeložené (_()) stejně jako v cmt_view_fill().
 *
 * @param v Kopie (nesmí být NULL); přepíše se celá.
 * @param f Stav.
 */
void cmt_view_make_fixture ( st_CMT_VIEW *v, en_CMT_VIEW_FIXTURE f );

/**
 * @brief Výběr řádku tabulky, který koncept k testovacímu stavu ukazuje.
 *
 * Výběr je stav okna, ne kopie (výběr se liší od pozice pásky), proto ho
 * fixture nenese a test ho oknu předá zvlášť.
 *
 * @param f Stav.
 * @return Index vybraného bloku (PLAYING_FULL: 3 = řádek #4 Raw recording); -1 bez výběru.
 */
int cmt_view_fixture_selection ( en_CMT_VIEW_FIXTURE f );

/**
 * @brief Typ souboru MZF slovně pro sloupec Type (např. "01 machine code").
 *
 * Názvy podle báze formats/mzf.md (tabulka Typy souborů): 01h OBJ =
 * machine code, 02h BTX = BASIC program, 03h BSD = data, 04h BRD = BASIC
 * data, 22h = CP/M file; jiné typy jen číslem.
 *
 * @param ftype Typ z hlavičky MZF.
 * @return Text "XX popis" (hex dvěma číslicemi, popis přeložený).
 */
std::string cmt_view_mzf_type_text ( int ftype );

#endif /* CMT_VIEW_H */
