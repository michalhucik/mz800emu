/* 
 * File:   cmt.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 11. srpna 2015, 12:08
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

#ifndef CMT_H
#define CMT_H


#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"

#include "cmtext.h"

#include "libs/cmtspeed/cmtspeed.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/mztape/mztape.h"

    typedef enum en_CMT_STATE {
        CMT_STATE_STOP = 0,
        CMT_STATE_PLAY,
        CMT_STATE_RECORD,
    } en_CMT_STATE;


    typedef enum en_CMT_CPU_BOOST {
        CMT_CPU_BOOST_DISABLED = 0,
        CMT_CPU_BOOST_ENABLED = 1
    } en_CMT_CPU_BOOST;


    typedef enum en_CMT_MZFSIZE_CHECK {
        CMT_MZFSIZE_CHECK_DISABLED = 0,
        CMT_MZFSIZE_CHECK_ENABLED = 1
    } en_CMT_MZFSIZE_CHECK;


    /** @brief Hodnoty volby automatického převinutí (st_CMT::autorewind, INI [CMT] autorewind). */
    typedef enum en_CMT_AUTOREWIND {
        CMT_AUTOREWIND_DISABLED = 0, /**< po konci pásky zůstane otevřený poslední blok (výchozí) */
        CMT_AUTOREWIND_ENABLED = 1   /**< po konci pásky se otevře první blok se signálem */
    } en_CMT_AUTOREWIND;


    /** @brief Výchozí doba ticha pro automatické ukončení záznamu v s (INI [CMT] rec_autostop_s); 0 = vypnuto. */
#define CMT_REC_AUTOSTOP_DEFAULT_S 0u

    /** @brief Nejvyšší doba ticha pro automatické ukončení záznamu v s (rozsah INI 0 až tato hodnota). */
#define CMT_REC_AUTOSTOP_MAX_S 600u


    /**
     * @brief Stav virtuálního kazetového magnetofonu (globální instance g_cmt).
     *
     * Drží vloženou pásku (ext), stav transportu (state, paused), stav
     * přehrávání aktuálního bloku (playsts), výstupní signál z pásky (output)
     * a časovou základnu přehrávání v GDG ticích (start_time, paused_time).
     *
     * Invarianty:
     * - Bez vložené pásky (ext == NULL) je transport vždy ve stavu
     *   CMT_STATE_STOP s paused == 0 a playsts == CMTEXT_BLOCK_PLAYSTS_STOP.
     *   Funkce, které ve stavu PLAY/RECORD čtou ext (cmt_update_output,
     *   cmt_write_data), na tento invariant spoléhají a navíc ho samy
     *   defenzivně testují. Za běhu ho udržují cmt_stop() a cmt_eject()
     *   (i bez vložené pásky přepnou do STOP), po načtení snapshotu ho
     *   obnoví cmt_sanitize_state().
     * - Ve stavu CMT_STATE_STOP je paused == user_paused == motor_off == 0.
     * - Při user_paused == 1 je paused == 1 (páska stojí, dokud uživatel
     *   pauzu nezruší, bez ohledu na motor).
     *
     * Ownership: ext ukazuje na statickou instanci cmtext rozšíření (nikdy
     * se neuvolňuje přes g_cmt), last_filename a ui_base_filename vlastní
     * g_cmt (uvolňují se přes baseui_tools_mem_free).
     *
     * Synchronizace: přístup jen z emulátorového vlákna (nebo s pozastavenou
     * emulací, např. při načítání snapshotu).
     */
    typedef struct st_CMT {
        st_CMTEXT *ext;
        /** Cesta vložené pásky, jak ji zadal uživatel (nekanonizovaná);
         *  "" bez pásky. Nastavuje cmt_open_file_by_extension(), maže
         *  cmt_eject(); ukládá ji snapshot (snap_cmt.c). Nikdy NULL po cmt_init(). */
        char *last_filename;
        char *ui_base_filename;
        en_CMT_STREAM_POLARITY polarity;
        /** Výchozí rychlost MZ bloků: poměr, nebo CMTSPEED_CUSTOM (pak platí
         *  mz_custom_pulses). Platí pro bloky s CMTEXT_BLOCK_SPEED_DEFAULT. */
        en_CMTSPEED mz_cmtspeed;
        /** Výchozí vlastní délky pulzů (cfg CMT/mz_custom_pulse_*_us). Uchovávají
         *  se i při výchozí rychlosti v poměru; vždy platné (kladné délky). */
        st_MZTAPE_PULSES_LENGTH mz_custom_pulses;
        en_CMT_STATE state;
        /** Páska stojí ve stavu PLAY / RECORD (1), jinak 0. Výsledek dvou
         *  nezávislých zdrojů: pauzy uživatele (user_paused) a stojícího
         *  motoru (klopný obvod přepínaný hranou PC3). Při user_paused == 0
         *  platí motor stojí == paused, při user_paused == 1 je paused == 1
         *  a stav motoru nese motor_off. Programu se jeví jen tato hodnota
         *  (PC4, signál pásky), viz cmt_motor_toggle(). */
        int paused;
        /** Pauza uživatele (1): nastavuje a ruší jen cmt_pause() (tlačítko
         *  okna, MCP, záznam vstupů REPLAY_CMT_PAUSE); hrana PC3 ji nemění.
         *  Ve STOP vždy 0. Snapshot ji ukládá (element user_paused, starší
         *  snapshot = 0). */
        int user_paused;
        /** Motor stojí (1), platné jen při user_paused == 1 (jinak se
         *  odvozuje z paused, viz cmt_pause_sources_sync()). Ve STOP 0. */
        int motor_off;
        en_CMTEXT_BLOCK_PLAYSTS playsts;
        int output;
        uint64_t start_time;
        uint64_t paused_time;
        int ui_player_update;
        /** Uživatelská preference "MAX SPEED během přehrávání" (cfg CMT/cpu_boost,
         *  cfg element ukazuje přímo sem). Snapshot ji zapisuje, ale při
         *  načtení neobnovuje (viz snap_cmt_load). */
        en_CMT_CPU_BOOST cpu_boost;
        en_CMT_MZFSIZE_CHECK mzfsize_check;
        int recording_to_stream; /**< při RECORD: 1 = program už zapsal aspoň jeden úsek (buffer hran neprázdný), řídí MAX SPEED cpu_boost */
        uint64_t recording_last_event;
        /** Automatické převinutí: 1 = po konci pásky (STOP) se otevře první
         *  blok se signálem (cmttmz_rewind()), 0 = zůstane poslední blok.
         *  Uživatelská volba, INI [CMT] autorewind (výchozí
         *  CMT_AUTOREWIND_DISABLED, en_CMT_AUTOREWIND; cfg element ukazuje
         *  přímo sem); snapshot ji neukládá. */
        int autorewind;
        /** Automatické ukončení záznamu: po tolika s emulovaného času bez
         *  hrany při běžícím motoru (a po aspoň jedné zaznamenané hraně) se
         *  záznam ukončí jako STOP (cmt_screen_done_period()); 0 = nikdy.
         *  Uživatelská volba, INI [CMT] rec_autostop_s (výchozí
         *  CMT_REC_AUTOSTOP_DEFAULT_S, rozsah 0 až CMT_REC_AUTOSTOP_MAX_S;
         *  cfg element ukazuje přímo sem); snapshot ji neukládá, záznam
         *  vstupů ano (manifest `cmt`, přehrání ji na svou dobu nastaví). */
        unsigned rec_autostop_s;
        /** Automatická oprava velikosti MZF: 1 = soubor MZF delší než podle
         *  hlavičky se při kontrole velikosti (mzfsize_check) zkrátí bez
         *  dotazu, 0 = zeptá se popup MZF Size Warning. Platí jen se zapnutou
         *  kontrolou. Uživatelská volba, INI [CMT] mzfsize_autofix (výchozí 0;
         *  cfg element ukazuje přímo sem); čte ji UI vlákno
         *  (cmt_fix_mzfsize.cpp), snapshot ji neukládá. */
        int mzfsize_autofix;
        /** Echo: 1 = signál pásky je při PLAY a RECORD bez pauzy slyšet
         *  v reproduktoru (audio_cmt_echo_changed(), sdílí zvukový kanál
         *  CTC0), 0 = ne. Na stav stroje nemá vliv (jen zvukový log).
         *  Uživatelská volba, INI [CMT] echo (výchozí 0; cfg element ukazuje
         *  přímo sem); snapshot ji neukládá. */
        int echo;
    } st_CMT;

    extern st_CMT g_cmt;


#define CMT_TEST_FILLED (g_cmt.ext != NULL)
#define CMT_TEST_STOP (g_cmt.state == CMT_STATE_STOP)
#define CMT_TEST_PLAY (g_cmt.state == CMT_STATE_PLAY)
#define CMT_TEST_RECORD (g_cmt.state == CMT_STATE_RECORD)
#define CMT_TEST_PAUSED (g_cmt.paused == 1)
#define CMT_TEST_MZFSIZE_CHECK_ENABLED (g_cmt.mzfsize_check == CMT_MZFSIZE_CHECK_ENABLED)

#define CMT_TEST_MZ_CMTSPEED(speed) (g_cmt.mz_cmtspeed == speed)
#define CMT_TEST_CPU_BOOST_ENABLED (g_cmt.cpu_boost == CMT_CPU_BOOST_ENABLED)
#define CMT_TEST_MZF_SIZE_CHECK_ENABLED (g_cmt.mzfsize_check == CMT_MZFSIZE_CHECK_ENABLED)

#define CMT_SET_MZF_SIZE_CHECK(value) (g_cmt.mzfsize_check = value)

#define CMT_TEST_POLARITY_NORMAL (g_cmt.polarity == CMT_STREAM_POLARITY_NORMAL)
#define CMT_TEST_POLARITY_INVERTED (g_cmt.polarity == CMT_STREAM_POLARITY_INVERTED)

#define cmt_on_screen_done_event( ) { if ( !CMT_TEST_STOP ) cmt_screen_done_period (); }

/**
 * @brief Echo kazety na konci snímku, před uzavřením zvukového logu snímku.
 *
 * Volá cmt_echo_frame(), jen když je echo zapnuté nebo jeho úroveň ještě
 * není v klidu (jinak jedno čtení dvou proměnných).
 */
#define cmt_on_frame_audio_event( ) { if ( g_cmt.echo || g_audio.cmt_echo ) cmt_echo_frame (); }

    static inline double cmt_get_playtime ( void ) {
        if ( !CMT_TEST_FILLED ) return 0;
        if ( CMT_TEST_STOP ) return 0;
        if ( CMT_TEST_PAUSED ) return g_cmt.paused_time * ( 1 / (double) GDGCLK_BASE );
        uint64_t now_ticks = gdg_get_total_ticks ( );
        uint64_t play_ticks = ( now_ticks - g_cmt.start_time );
        double play_time = play_ticks * ( 1 / (double) GDGCLK_BASE );
        return play_time;
    }

#define cmt_get_ui_base_filename() (g_cmt.ui_base_filename ? g_cmt.ui_base_filename : "")

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Inicializace kazety: sekce INI [CMT] a výchozí stav decku.
     *
     * Zaregistruje klíče sekce [CMT] (rychlost, vlastní pulzy, polarita,
     * CPU Boost, kontrola velikosti MZF, autorewind, rec_autostop_s a klíč
     * run_mzf_device modulu bootstrap_device.h), načte je z INI a propaguje
     * do stavu (g_cmt, nastavení bootdev_setting_*()), pak nastaví prázdný
     * deck ve STOP.
     *
     * @pre Volá se jednou při startu emulátoru, po cfgmain_init(), před
     *      spuštěním emu vlákna.
     * @post g_cmt je bez pásky ve STOP; nastavení z INI platí.
     */
    void cmt_init ( void );

    /**
     * @brief Konec emulátoru: vysune pásku (záznam se zahodí), uvolní stav kazety,
     *        CMT hacku, rozšíření a data zásahů kazety (cmt_request_exit()).
     * @pre Emu vlákno už neprovádí zásahy fronty zařízení.
     */
    void cmt_exit ( void );
    void cmt_rear_dip_switch_cmt_inverted_polarity ( unsigned value );

    /** @brief Varování MCP, když operace zahodila neuložené změny pásky nebo probíhající nahrávání. */
#define CMT_WARNING_DISCARDED "Unsaved tape changes were discarded"

    /**
     * @brief Má vložená páska něco, co by vysunutí ztratilo?
     * @return true = páska je Modified (cmttmz_is_modified()) nebo běží nahrávání (RECORD).
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    bool cmt_has_unsaved_changes ( void );

    /**
     * @brief Otevře soubor pásky; neúspěšné otevření vloženou pásku nemění.
     *
     * Pořadí: rozšíření podle přípony, pak načtení a převod souboru
     * (cmttmz_open_prepare()) bez zásahu do vložené pásky. Teprve když
     * soubor jde načíst, dosavadní páska se vysune (probíhající nahrávání se
     * zahodí s důvodem "another tape was opened", neuložené změny se ztratí)
     * a vloží se nová (cmttmz_open_prepared()). Neznámá přípona, chybějící,
     * poškozený nebo nepodporovaný soubor = chyba a vložená páska, její
     * Modified, otevřený blok, transport i běžící nahrávání zůstanou beze
     * změny (verze 2.0.6 pásku i při neúspěchu vysunula).
     *
     * Po úspěchu: g_cmt.ext, jméno pro UI, last_filename, transport STOP;
     * u .mzf / .m12 kontrola velikosti MZF (mimo tichý režim) a zápis cesty
     * do historie naposledy vložených médií (recent_media_add(RECENT_MEDIA_CMT),
     * mimo tichý režim).
     *
     * @param filename Cesta (UTF-8).
     * @param[out] discarded true = otevření zahodilo neuložené změny nebo
     *        nahrávání dosavadní pásky (cmt_has_unsaved_changes() před
     *        vysunutím); false, když soubor nejde načíst (páska se pak
     *        nevysouvá). Smí být NULL.
     * @return EXIT_SUCCESS; EXIT_FAILURE (text v logu přes baseui_error()).
     * @pre Vlákno emulátoru pod device_state_lock() (mění kontejner, který čte UI).
     */
    int cmt_open_file ( const char *filename, bool *discarded );

    /**
     * @brief Otevře soubor pásky (cmt_open_file() bez hlášení zahozených změn).
     * @param filename Cesta (UTF-8), funkce ji nemění.
     * @return Jako cmt_open_file().
     * @pre Jako cmt_open_file().
     */
    int cmt_open_file_by_extension ( char *filename );

    /**
     * @brief Filtr souborového dialogu pásky (syntaxe ImGuiFileDialog, viz cmt_init()).
     * @return Statický text platný do cmt_exit(); "" před cmt_init().
     * @pre UI vlákno.
     */
    const char *cmt_ui_open_filters(void);

    void cmt_play ( void );
    void cmt_play_paused ( void );

    /**
     * @brief Zahájí nahrávání do vložené pásky (bez pásky do nové prázdné).
     *
     * Jen ve STOP. Bez vložené pásky vloží cmttmz_new_tape() (cesta "",
     * jméno pro UI prázdné). Transport přejde do RECORD v pauze (rozběh
     * motoru PC3 pauzu zruší, cmt_pause(0)), buffer hran se vyprázdní
     * (cmttmz_rec_begin(), úroveň PC1 na začátku). Hrany zapisuje
     * cmt_write_data(), záznam dokončí cmt_stop() (bloky na konec pásky).
     * PC4 pak čte stav decku i se zapnutým CMT hackem (CMTHACK_TEST_FAKES_MOTOR);
     * změní-li se tím čtená hodnota (podvržený bit byl 1), odpálí se
     * událost cmt:mstate. Patch zápisu CMT hacku (Direct Save) se z ROM
     * sundá (cmthack_save_patch_sync()): s Record se signál nahrává i při
     * uložení rutinami ROM; po cmt_stop() se patch vrátí.
     *
     * @return EXIT_SUCCESS; EXIT_FAILURE mimo STOP nebo když novou pásku nejde
     *         vytvořit (alokace).
     * @pre Vlákno emulátoru; mění kontejner (nová páska), proto pod
     *      device_state_lock(), pokud ho volající drží (zásah GUI / MCP), jinak
     *      si ho funkce vezme.
     * @post Při úspěchu CMT_TEST_RECORD, paused == 1, páska vložená.
     */
    int cmt_record ( void );

    struct st_MZF;

    /**
     * @brief Připojí soubor MZF jako nový blok na konec pásky (Direct Save, past ROM SAVE CMT hacku).
     *
     * Postup:
     * - bez pásky vloží novou prázdnou pásku jako cmt_record() (cesta "");
     * - transport v PLAY (i v pauze) nejdřív zastaví cmt_stop() jako past
     *   čtení; v RECORD ho nechá běžet (pojistka: v RECORD patch zápisu
     *   v ROM není, past se tam normálně nevolá);
     * - blok 0x46 sestaví jako záznam (cmttmz_rec_build_mzf() s výchozí
     *   rychlostí Virtual CMT, záznam rychlosti CMTTMZ_SPEED_ORIGIN_DEFAULT,
     *   pauza cmttmz_rec_mzf_pause(&opts, 0)) a připojí ho pod
     *   device_state_lock() (cmttmz_append_blocks());
     * - mimo RECORD otevře nový blok (cmttmz_seek_block(), transport stojí
     *   na jeho začátku, jako po cmt_record_finish()) a vynuluje stav hacku
     *   nad páskou (cmthack_tape_state_clear()), protože se otevřený blok
     *   změnil;
     * - výsledek (úspěch i chyba) uloží pro okno Virtual CMT
     *   (cmt_direct_save_get_result()), vypíše řádek na stdout (úspěch) nebo
     *   stderr (chyba) a obnoví okno (ui_cmt_window_update()).
     *
     * Zápis na médium se nepočítá (media_writes.h): jako nahrávání jen
     * připojuje bloky na konec pásky, checkpoint záznamu vstupů ho vrátí
     * zkrácením pásky (cmttmz_tape_mark_restore()).
     *
     * @param mzf MZF (hlavička v pořadí hostitele, tělo aspoň fsize bajtů;
     *        při fsize 0 smí být tělo NULL). Funkce ho nemění ani nepřebírá.
     * @param[out] block_id Číslo nového bloku (smí být NULL); -1 při chybě.
     * @return EXIT_SUCCESS; EXIT_FAILURE při NULL @p mzf, když novou pásku
     *         nejde vytvořit nebo při chybě sestavení či připojení bloku
     *         (páska pak beze změny).
     * @pre Vlákno emulátoru (zámek stavu zařízení si vezme, nedrží-li ho).
     */
    int cmt_direct_save_mzf ( const struct st_MZF *mzf, int *block_id );


    /** @brief Velikost bufferu jména souboru ve výsledku přímého uložení (ASCII z mzf_tools_get_fname() včetně nuly). */
#define CMT_DIRECT_SAVE_NAME_SIZE 17

    /**
     * @brief Výsledek posledního přímého uložení (cmt_direct_save_mzf()) pro okno Virtual CMT.
     *
     * Invarianty: serial 0 = od startu emulátoru žádné uložení (ostatní
     * položky nulové); ok == false => block_id == -1.
     */
    typedef struct st_CMT_DIRECT_SAVE_RESULT {
        unsigned serial;                        /**< čítač výsledků, roste s každým voláním cmt_direct_save_mzf() */
        bool ok;                                /**< true = soubor je na pásce */
        int block_id;                           /**< číslo nového bloku (od 0), -1 při chybě */
        uint32_t size;                          /**< délka těla v bajtech (fsize uloženého bloku) */
        char name[CMT_DIRECT_SAVE_NAME_SIZE];   /**< jméno souboru v ASCII (mzf_tools_get_fname()) */
    } st_CMT_DIRECT_SAVE_RESULT;

    /**
     * @brief Přečte výsledek posledního přímého uložení.
     * @param[out] out Výsledek (nesmí být NULL); serial 0 = žádné uložení.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (výsledek se
     *      zapisuje pod ním).
     */
    void cmt_direct_save_get_result ( st_CMT_DIRECT_SAVE_RESULT *out );

    /**
     * @brief Vloží novou prázdnou pásku bez cesty (cmttmz_new_tape()).
     *
     * Dosavadní pásku vysune (jako cmt_eject(): probíhající záznam se zahodí
     * s důvodem "a new tape was inserted", neuložené změny se ztratí - dotaz
     * dělá UI, MCP vrací varování).
     * Nová páska má cestu "", 0 bloků, není Modified, transport STOP.
     *
     * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (pak bez pásky;
     *         chyba jde uživateli přes baseui_error(), protože okno ji
     *         zadává asynchronně přes replay_host_cmt() bez výsledku).
     * @pre Vlákno emulátoru (zámek stavu zařízení si vezme, nedrží-li ho).
     */
    int cmt_new_tape ( void );

    /**
     * @brief Zahodí probíhající záznam bez dekódování (vysunutí, jiná páska, snapshot, konec emulátoru).
     *
     * Buffer hran se zahodí (cmttmz_rec_discard()), cesta souboru záznamu se
     * zapomene (žádný WAV ani uložení pásky), zpráva záznamu "Recording
     * discarded (<why>)" s nulovými počty a příznakem zahození
     * (cmttmz_set_record_discarded()), řádek do logu. Transport zůstane
     * v RECORD; následný cmt_stop() už nic nedokončí (buffer neaktivní).
     *
     * @param why Důvod (anglicky, do zprávy a logu).
     * @pre Vlákno emulátoru, CMT_TEST_RECORD (zámek stavu si vezme, nedrží-li ho).
     */
    void cmt_record_discard ( const char *why );

    /**
     * @brief Zahodí probíhající záznam bez zprávy (návrat záznamu vstupů na checkpoint).
     *
     * Jako cmt_record_discard(), ale výsledek záznamu (cmttmz_get_record_result(),
     * čítač, který UI ohlašuje) se nemění: checkpoint nahrávání buď obnoví
     * z kopie, nebo se vrací do doby před ním - uživatel nic nezahodil.
     * Transport zůstane v RECORD bez aktivního bufferu; srovná ho načtení
     * snapshotu (cmt_sanitize_state()).
     *
     * @pre Vlákno emulátoru.
     */
    void cmt_record_drop ( void );

    /**
     * @brief Vysune pásku při návratu záznamu vstupů na checkpoint bez pásky.
     *
     * Probíhající nahrávání zahodí bez zprávy (cmt_record_drop()), vysune
     * v tichém režimu (bez BP eventů a výpisu stavu) a vysunutí nepočítá jako
     * zápis na médium (g_media_writes se vrátí): stav se vrací na checkpoint,
     * soubory uživatele se nezapisují a dialog se neukazuje.
     *
     * @pre Vlákno emulátoru pod device_state_lock() (mění kontejner).
     * @post Bez vložené pásky, transport STOP.
     */
    void cmt_eject_for_checkpoint ( void );

    /**
     * @brief Kopie probíhajícího nahrávání (buffer hran a cesta souboru) pro checkpoint záznamu vstupů.
     *
     * Neprůhledný typ, uvolní ho cmt_record_ckpt_free(). Do souboru snapshotu
     * se nikdy neukládá, jen do checkpointu v paměti.
     */
    typedef struct st_CMT_RECORD_CKPT st_CMT_RECORD_CKPT;

    struct st_CMTTMZ_TAPE_MARK; /* cmt_tmz.h */

    /**
     * @brief Zkopíruje probíhající nahrávání, nepřesáhne-li buffer hran @p max_bytes.
     * @param max_bytes Mez velikosti kopie bufferu v bajtech.
     * @param[out] too_big true = nahrávání běží, ale buffer je nad mezí (smí být NULL).
     * @return Kopie; NULL mimo RECORD, nad mezí nebo při selhání alokace.
     * @pre Vlákno emulátoru.
     */
    st_CMT_RECORD_CKPT *cmt_record_ckpt_take ( size_t max_bytes, bool *too_big );

    /** @brief Velikost kopie bufferu hran v bajtech. @param c Kopie (NULL = 0). @return Bajty. */
    size_t cmt_record_ckpt_bytes ( const st_CMT_RECORD_CKPT *c );

    /**
     * @brief Obnoví nahrávání z kopie: buffer hran a cesta souboru záznamu.
     *
     * Transport (RECORD, pauza, časy) obnovuje snapshot; tato funkce jen
     * vrací buffer, bez kterého by loader RECORD zrušil.
     *
     * @param c Kopie (zůstává volajícímu).
     * @return EXIT_SUCCESS; EXIT_FAILURE mimo RECORD nebo při selhání alokace.
     * @pre Vlákno emulátoru, transport už je v RECORD (obnovený snapshotem).
     */
    int cmt_record_ckpt_install ( const st_CMT_RECORD_CKPT *c );

    /** @brief Uvolní kopii nahrávání (NULL je přípustné). @param c Kopie. */
    void cmt_record_ckpt_free ( st_CMT_RECORD_CKPT *c );

    /**
     * @brief Vloží pásku z obrazu TMZ v paměti (páska vložená ve snapshotu).
     *
     * Dosavadní pásku vysune (cmt_eject()), obraz otevře cmttmz_open_image():
     * páska má cestu @p path (může být "" = bez souboru), je Modified,
     * transport STOP na prvním bloku. Soubor se neotevírá ani nečte. Bloky
     * 0x18 s příznakem v @p wav_blocks mají typ WAV (přepínač polarity na ně
     * působí, jako po otevření souboru WAV).
     *
     * @param data Obraz (nedůvěryhodný). @param size Délka (nad 4 GB = chyba).
     * @param path Cesta pásky (NULL = "").
     * @param wav_blocks Příznaky původu WAV po blocích (cmttmz_get_wav_blocks()), NULL = žádný.
     * @param wav_count Počet položek @p wav_blocks.
     * @return EXIT_SUCCESS; EXIT_FAILURE, když obraz nejde načíst (pak bez pásky).
     * @pre Vlákno emulátoru (zámek stavu si vezme, nedrží-li ho).
     */
    int cmt_open_image ( const uint8_t *data, size_t size, const char *path, const uint8_t *wav_blocks, size_t wav_count );

    /**
     * @brief Vrátí vloženou pásku na značku checkpointu (cmttmz_tape_mark_restore()).
     * @param mark Značka téže pásky.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky nebo když značka k pásce nepatří.
     * @pre Vlákno emulátoru (zámek stavu si vezme, nedrží-li ho).
     */
    int cmt_tape_mark_restore ( const struct st_CMTTMZ_TAPE_MARK *mark );

    /**
     * @brief Smí se vložená páska teď upravovat (mazání, přesun, vložení bloků)?
     *
     * Jen s vloženou páskou rozšíření cmt_tmz (s kontejnerem), transportem
     * ve STOP, bez probíhajícího nahrávání a mimo záznam a přehrávání
     * vstupů (replay_is_active(); značka checkpointu úpravy vrátit neumí).
     * Záznam vstupů hlídá navíc fronta zásahů (device_request_poll())
     * a třída příkazu MCP; tato kontrola chrání i volající mimo ně.
     *
     * @return 1 = úprava povolená, 0 = ne.
     * @pre Vlákno emulátoru (nebo pod device_state_lock()).
     */
    int cmt_tape_edit_allowed ( void );

    /**
     * @brief Ověří, že pásku jde teď upravit, a když ne, nastaví text chyby úpravy.
     *
     * Stejná kontrola jako na začátku cmt_tape_remove_blocks()
     * a cmt_tape_move_block() (cmt_tape_edit_allowed()), jen bez úpravy:
     * zásah mazání ji použije, když ví, že seznam bloků je neplatný, aby
     * stav decku ohlásil dřív než neexistující blok.
     *
     * @return 1 = úprava povolená (text chyby beze změny); 0 = ne, text
     *         v cmttmz_get_edit_error() ("No tape inserted", "The tape cannot
     *         be edited while recording", "The tape can be edited only when
     *         the deck is stopped", "The tape cannot be edited while an input
     *         replay is recorded or played").
     * @pre Vlákno emulátoru; zámek stavu zařízení si vezme, nedrží-li ho volající.
     */
    int cmt_tape_edit_verify ( void );

    /**
     * @brief Smaže bloky vložené pásky (obal cmttmz_remove_blocks()).
     *
     * Ověří cmt_tape_edit_allowed() (jinak chyba s textem
     * v cmttmz_get_edit_error(), páska beze změny), vezme device_state_lock(),
     * nedrží-li ho volající, smaže bloky (kurzor nastaví jádro,
     * pozice v bloku 0; jádro smaže i stav CMT hacku nad páskou,
     * cmthack_tape_state_clear(): hlavička načtená bez těla, konec pásky).
     *
     * @param block_ids Čísla bloků vzestupně bez opakování. @param count Počet (0 = nic, úspěch).
     * @return EXIT_SUCCESS; EXIT_FAILURE (páska beze změny, text v cmttmz_get_edit_error()).
     * @pre Vlákno emulátoru.
     */
    int cmt_tape_remove_blocks ( const int *block_ids, uint32_t count );

    /**
     * @brief Přesune blok vložené pásky (obal cmttmz_move_block()).
     *
     * Podmínky, zámek a úklid stavu CMT hacku (v jádře) jako
     * cmt_tape_remove_blocks(); přesun na vlastní místo nic nezmění (stav
     * hacku zůstane).
     *
     * @param block_id Číslo bloku. @param to Pozice po přesunu (0 .. počet - 1).
     * @return EXIT_SUCCESS; EXIT_FAILURE (páska beze změny, text v cmttmz_get_edit_error()).
     * @pre Vlákno emulátoru.
     */
    int cmt_tape_move_block ( int block_id, int to );

    /**
     * @brief Vloží všechny bloky souboru do vložené pásky před blok @p at (obal cmttmz_import_file_at()).
     *
     * Bez vložené pásky (deck ve STOP, mimo záznam a přehrávání vstupů)
     * jádro soubor nejdřív převede a novou prázdnou pásku (jako RECORD do
     * prázdného decku, @p at = 0) vloží až po úspěšném převodu; obal pak
     * nastaví g_cmt (ext, prázdné jméno). Neúspěšný import tak nezanechá
     * stopu: žádné vysunutí, události breakpointů CMT ani zápis na médium
     * S páskou ověří cmt_tape_edit_allowed(). Import běží
     * pod device_state_lock() (vezme ho, nedrží-li ho volající). Kurzor nastaví jádro: vložení do
     * prázdné pásky nechá otevřený první blok se signálem (PLAY funguje),
     * jinak sleduje původní aktuální blok; stav CMT hacku smaže jádro.
     *
     * @param path Cesta k souboru (UTF-8).
     * @param at Pozice 0 .. počet bloků (počet = na konec).
     * @param[out] imported Počet vložených bloků (smí být NULL).
     * @return EXIT_SUCCESS; EXIT_FAILURE (páska beze změny, text
     *         v cmttmz_get_edit_error(), důvod importu i v cmttmz_get_prepare_error()).
     * @pre Vlákno emulátoru.
     */
    int cmt_tape_import_file ( const char *path, int at, uint32_t *imported );

    /**
     * @brief Uplatní výsledek analýzy surového bloku na vloženou pásku (obal cmttmz_analysis_apply()).
     *
     * Ověří cmt_tape_edit_allowed() (jinak chyba s textem
     * v cmttmz_get_edit_error(), páska beze změny), vezme device_state_lock(),
     * nedrží-li ho volající, a uplatní výsledek (Add nebo Replace; ověření
     * čítače výměn pásky a bloku, stavba bloků, kurzor a úklid stavu CMT
     * hacku dělá jádro). Po úspěchu s aspoň jedním přidaným blokem souboru obnoví okno Virtual CMT.
     *
     * @param p Kopie pulzů (cmttmz_analyze_copy_pulses()).
     * @param a Analýza téže kopie (cmttmz_analyze_pulses()).
     * @param use Příznaky řádků k přidání (cmttmz_analysis_row_count() položek).
     * @param replace 0 = přidat soubory za surový blok, 1 = nahradit surový blok.
     * @param[out] added Počet přidaných bloků souborů (smí být NULL); při chybě 0.
     * @return EXIT_SUCCESS; EXIT_FAILURE (páska beze změny, text v cmttmz_get_edit_error()).
     * @pre Vlákno emulátoru.
     */
    struct st_CMTTMZ_PULSES;   /* cmt_tmz_analyze.h */
    struct st_CMTTMZ_ANALYSIS; /* cmt_tmz_analyze.h */
    int cmt_tape_analysis_apply ( const struct st_CMTTMZ_PULSES *p, const struct st_CMTTMZ_ANALYSIS *a, const uint8_t *use,
                                  int replace, uint32_t *added );

    /**
     * @brief Zapíše informace o vložené pásce (obal cmttmz_tape_info_set()).
     *
     * Ověří cmt_tape_edit_allowed() (jinak chyba s textem
     * v cmttmz_get_edit_error(), páska beze změny), vezme device_state_lock(),
     * nedrží-li ho volající, a zapíše bloky 0x32 a 0x43 na začátku pásky
     * (kurzor, úklid stavu CMT hacku a čítače dělá jádro). Po změně obnoví
     * okno Virtual CMT. Varování vrací cmttmz_get_edit_warning(). Formulář
     * z jiné pásky nebo z doby před jinou úpravou se odmítne (tape_serial).
     * Páska z MZF, M12 nebo MZT (storable 0): zápis projde, ale pásku pak jde
     * uložit jen jako TMZ nebo TZX; stroj (blok 0x43) na pásce .tzx přepne
     * hlavičku na TMZ a uložení .tzx varuje, že soubor je ve formátu TMZ.
     *
     * @param in Nový obsah. @param[out] changed 1 = páska se změnila (smí být NULL; při chybě obalu 0).
     * @return EXIT_SUCCESS; EXIT_FAILURE (text v cmttmz_get_edit_error()).
     * @pre Vlákno emulátoru.
     */
    struct st_CMTTMZ_TAPE_INFO; /* cmt_tmz_meta.h */
    int cmt_tape_info_set ( const struct st_CMTTMZ_TAPE_INFO *in, int *changed );

    /**
     * @brief Převede blok 0x46 vložené pásky na jiný profil zapisovače (obal cmttmz_block_convert_profile()).
     *
     * Podmínky a zámek jako cmt_tape_info_set(); po úspěchu obnoví okno Virtual CMT.
     * Nastavení dosavadního profilu, která se nepřenesla, vrací varováním
     * cmttmz_get_edit_warning().
     *
     * @param block_id Číslo bloku. @param profile_id ID profilu. @param speed Rychlost (CMTSPEED_NONE = bez rychlosti).
     * @return EXIT_SUCCESS; EXIT_FAILURE (páska beze změny, text v cmttmz_get_edit_error()).
     * @pre Vlákno emulátoru.
     */
    int cmt_tape_convert_block ( int block_id, const char *profile_id, en_CMTSPEED speed );

    /**
     * @brief Kompatibilní obálka: nahrávání s cestou souboru, který vznikne po STOP.
     *
     * Zahájí cmt_record() a zapamatuje cestu. Po STOP se záznam jako u
     * cmt_record() připojí k pásce a navíc se zapíše do souboru: přípona .wav
     * (bez ohledu na velikost písmen) = WAV 44 100 Hz z bufferu hran (konce
     * úseků zaokrouhlené kumulativně); .tmz, .tzx, .mzt, .mzf, .m12 = uložení
     * celé pásky cmttmz_save() (cesta pásky se změní, Modified 0); jiná
     * přípona = chyba. Zapisovatelnost cesty se ověří hned otevřením pro
     * připojení: existující soubor se nezkracuje (přepíše ho až zápis po
     * STOP); neexistující .wav vznikne prázdný a po dobu nahrávání zůstane,
     * neexistující soubor pásky se po zkoušce smaže. Bez zaznamenaného
     * úseku se po STOP nezapíše nic (ani WAV, ani páska); páska se neuloží
     * ani tehdy, když se nic nepřipojilo (méně než 64 pulzů). Prázdný WAV,
     * který vytvořila zkouška, smaže konec bez zápisu i zahození nahrávání
     * (vysunutí, otevření jiné pásky, nová páska, konec emulátoru, načtení
     * snapshotu); soubor, který existoval předem, zůstane.
     *
     * @param path Cílový soubor (UTF-8), nesmí být NULL ani prázdný.
     * @return EXIT_SUCCESS při zahájení nahrávání; EXIT_FAILURE mimo STOP, pro
     *         neplatnou nebo nezapisovatelnou cestu, nepodporovanou příponu
     *         nebo selhání cmt_record().
     * @pre Vlákno emulátoru (jako cmt_record()).
     */
    int cmt_record_to_file ( const char *path );

    /**
     * @brief Promítne novou cestu vložené pásky do g_cmt (po uložení pod jiným jménem).
     *
     * Nastaví g_cmt.last_filename (výchozí cesta dialogu) a jméno pro UI
     * (ui_base_filename = basename, prázdná cesta = NULL) a obnoví okno
     * kazety. Kontejner mění volající (cmttmz_save()).
     *
     * @param path Nová cesta (UTF-8); NULL se bere jako "".
     * @pre Vlákno emulátoru pod device_state_lock(), čte-li g_cmt UI.
     */
    void cmt_tape_path_changed ( const char *path );

    /**
     * @brief Pauza uživatele (tlačítko okna, MCP `cmt_transport pause`, záznam vstupů).
     *
     * Mění jen pauzu uživatele (st_CMT::user_paused), motor (hrana PC3,
     * cmt_motor_toggle()) je na ní nezávislý; páska se pohybuje, jen když
     * motor běží a pauza uživatele je vypnutá.
     * - @p value 1 v PLAY / RECORD: pauza uživatele zapnutá, páska stojí;
     *   hrany PC3 dál přepínají jen motor.
     * - @p value 1 ve STOP: PLAY čekající na motor (cmt_play_paused(),
     *   bez pauzy uživatele) jako dřív - první hrana PC3 pásku rozjede; bez
     *   přehratelné pásky zůstane STOP.
     * - @p value 0 v PLAY / RECORD s pauzou uživatele: pauza uživatele
     *   vypnutá, motor zůstane, jak ho nastavil počítač (stojí-li, páska dál
     *   čeká na motor).
     * - @p value 0 v PLAY / RECORD bez pauzy uživatele: motor se rozběhne
     *   (dosavadní "rozjeď čekající pásku", jako stisk tlačítka magnetofonu,
     *   který klopný obvod nastaví signálem SENSE, báze hw/14-cmt.md kap.
     *   Řízení motoru) - MCP `pause false` tím dál rozjíždí záznam čekající
     *   na motor.
     * - @p value 0 ve STOP: nic.
     *
     * Vedlejší efekty změny pohybu pásky: časová základna (paused_time /
     * start_time), CPU boost (cmt_cpu_boost_apply()), výpis, BP eventy
     * CMT_MSTATE a CMT_STATE_CHANGE, obnova okna.
     *
     * @param value 1 = pauza, 0 = pokračovat.
     * @pre Vlákno emulátoru.
     * @post Ve STOP user_paused == motor_off == paused == 0 (i po value 1:
     *       PLAY čekající na motor má user_paused == 0); jinak
     *       paused == (user_paused || motor stojí).
     */
    void cmt_pause ( int value );

    /**
     * @brief Náběžná hrana PC3 bez CMT hacku: přepne klopný obvod motoru.
     *
     * Volá ji pio8255_write() jen v PLAY / RECORD (ve STOP se hrana
     * ignoruje). Bez pauzy uživatele přepne pohyb pásky stejně jako dřív
     * (paused = !paused s týmiž vedlejšími efekty jako cmt_pause()); při
     * pauze uživatele přepne jen motor_off a páska dál stojí.
     *
     * @pre Vlákno emulátoru, !CMT_TEST_STOP, CMT hack není nainstalovaný.
     * @post Při user_paused == 0 je paused negované; jinak motor_off negované.
     */
    void cmt_motor_toggle ( void );

    void cmt_stop ( void );

    /**
     * @brief Vysune pásku; probíhající nahrávání zahodí s důvodem "the tape was ejected".
     *
     * Neuložené změny se ztratí (dotaz dělá UI, MCP vrací varování
     * CMT_WARNING_DISCARDED). Podrobnosti v cmt.c (cmt_eject_why()).
     *
     * @pre Vlákno emulátoru (pod device_state_lock(), čte-li pásku UI).
     * @post Bez vložené pásky, transport STOP.
     */
    void cmt_eject ( void );
    bool cmt_sanitize_state ( void );

    /**
     * @brief Zahájí tichý režim: přechody transportu bez BP eventů a bez výpisu.
     *
     * Do párového cmt_quiet_end() cmt_stop(), cmt_eject(), cmt_play()
     * a ostatní přechody neodpalují BP eventy cmt:mstate
     * a cmt:state_change a nevypisují "Virtual CMT is stopped" ani řádek
     * o otevřeném bloku pásky. Pro operace, které v emulovaném stroji
     * přechodem nejsou (načtení snapshotu, checkpoint záznamu vstupů).
     * Varování a chyby se vypisují dál. Volání se smí vnořovat.
     *
     * @pre Vlákno emulátoru.
     * @post cmt_quiet_active() == true.
     */
    void cmt_quiet_begin ( void );

    /**
     * @brief Ukončí tichý režim (pár k cmt_quiet_begin()).
     *
     * Po posledním vnořeném ukončení srovná cache hrany BP eventu
     * cmt:mstate (g_bp_event_state) na aktuální stav transportu, bez
     * odpálení: další skutečný přechod se porovná se stavem po tiché
     * operaci. Bez předchozího cmt_quiet_begin() nic nedělá.
     *
     * @pre Vlákno emulátoru.
     */
    void cmt_quiet_end ( void );

    /**
     * @brief Je tichý režim aktivní (cmt_quiet_begin() bez cmt_quiet_end())?
     * @return true v tichém režimu.
     */
    bool cmt_quiet_active ( void );

    /**
     * @brief Tlačítko STOP okna Virtual CMT.
     *
     * Totéž co cmt_stop(): při nahrávání se záznam dekóduje a připojí na
     * konec pásky (cmt_record_finish()), páska zůstane vložená a nevysouvá
     * se ani prázdná.
     *
     * @pre Emu vlákno (nebo pozastavená emulace).
     */
    void cmt_stop_button ( void );

    /**
     * @brief Další / předchozí blok pásky se zachováním transportu.
     *
     * Hraje-li páska, zastaví ji, otevře sousední blok a znovu spustí
     * (i v pauze, se zdroji pauzy user_paused a motor_off). Bez pásky
     * s kontejnerem nic. Když sousední blok otevřít nejde, hrající páska
     * se znovu spustí na původním bloku a místě (cmt_tape_get_position() /
     * cmt_tape_set_position()) se stejnými zdroji pauzy; transport tak
     * krátce projde STOP jako u úspěšného kroku (cmt_stop() odpálí
     * breakpoint cmt:state_change).
     *
     * @param forward 1 = další blok, 0 = předchozí.
     * @return EXIT_SUCCESS; EXIT_FAILURE (blok neexistuje, bez pásky, při nahrávání
     *         RECORD - tehdy se otevřený blok ani transport nemění).
     * @pre Emu vlákno (nebo pozastavená emulace).
     */
    int cmt_tape_step_block ( int forward );

    /**
     * @brief Výběr bloku pásky ze seznamu bloků (tabulka okna Virtual CMT, dvojklik a Play from here).
     *
     * Bez @p forced (jedno kliknutí) jen při zastaveném transportu otevře
     * blok. S @p forced (dvojklik) na jiném bloku ho otevře a spustí (v pauze
     * zůstane v pauze), na stejném bloku přepne play / pause. Výjimka: stejný
     * blok v pauze uživatele (user_paused) se jako jiný blok otevře znovu od
     * začátku a pauza zůstane se svými zdroji (pauza uživatele, motor);
     * nepokračuje z místa pauzy. Stejný blok čekající jen na motor ho rozjede
     * (cmt_pause(0)), ve STOP ho pustí.
     *
     * @param block_id Číslo bloku.
     * @param forced   1 = dvojklik.
     * @return EXIT_SUCCESS; EXIT_FAILURE (bez pásky, blok nejde otevřít, při nahrávání
     *         RECORD i s @p forced - tehdy se otevřený blok ani transport nemění).
     * @pre Emu vlákno (nebo pozastavená emulace).
     */
    int cmt_tape_select_block ( int block_id, int forced );

    /**
     * @brief Seek na blok pásky se zachováním stavu transportu (MCP emu_cmt_tape_seek).
     *
     * Přímo otevře blok @p block_id (container->cb_open_block, u cmt_tmz
     * přestavba playeru na blok) a nastaví pozici na začátek bloku:
     * - STOP: stav zůstane STOP, příští cmt_play() hraje od začátku bloku
     *   (nový blok má start_gdgticks = 0),
     * - PLAY bez pauzy: start_time = teď, blok hraje dál od začátku,
     * - PLAY v pauze (i po zastavení pásky): paused_time = 0, g_cmt.output
     *   dostane úroveň signálu na začátku bloku; pokračování (PAUSE,
     *   motor PC3) hraje od začátku bloku.
     * Ve všech případech playsts = BODY při PLAY a čtení streamu od začátku.
     * Seek na právě otevřený blok ho otevře znovu a začne od začátku.
     *
     * Na rozdíl od cmt_tape_select_block() transport nezastavuje a znovu
     * nespouští: stav (STOP / PLAY / pauza) se nemění, proto se neodpalují
     * BP eventy cmt:state_change a cmt:mstate a nic se nevypisuje.
     * Dříve MCP seek jen otevřel blok a při PLAY se nový blok četl od času
     * uplynulého ve starém bloku, v pauze od staré paused_time.
     *
     * @param block_id Index bloku v souboru pásky.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, bez kontejneru
     *         s cb_open_block, při nahrávání (RECORD) nebo když blok nejde
     *         otevřít (mimo rozsah, CONTROL, přeskakovaný) - pak se otevřený
     *         blok ani transport nemění.
     * @pre Emu vlákno pod device_state_lock() (blok čte UI vlákno; dbgapi
     *      zámek drží, viz dbgapi_cmd_needs_device_lock()).
     * @post Při úspěchu je otevřený blok @p block_id a pozice v něm 0.
     */
    int cmt_tape_seek_block ( int block_id );

    /**
     * @brief Stav bloku vložené pásky z hlediska přehrávání (cmt_tape_block_status()).
     *
     * Odpovídá sloupci Status okna Virtual CMT bez stavu Unsaved (ten nese
     * samostatný příznak bloku, cmttmz_block_is_unsaved()).
     */
    typedef enum en_CMT_BLOCK_STATUS {
        CMT_BLOCK_STATUS_OK = 0,      /**< blok se signálem, který player přehraje */
        CMT_BLOCK_STATUS_NOT_DECODED, /**< surový signál (WAV, TZX 0x18): přehraje se, soubory v něm nejsou rozpoznané */
        CMT_BLOCK_STATUS_CANNOT_PLAY, /**< blok se signálem, který player přehrát neumí (přeskočí ho) */
        CMT_BLOCK_STATUS_CONTROL,     /**< řídicí nebo informační blok bez signálu */
    } en_CMT_BLOCK_STATUS;

    /**
     * @brief Stav bloku vložené pásky (přehratelnost) bez generování signálu.
     *
     * Pořadí rozhodnutí: řídicí blok (typ CMTEXT_BLOCK_TYPE_CONTROL) ->
     * CONTROL; blok, který player přehrát neumí (st_CMTTMZ_BLOCK_UI::unplayable)
     * -> CANNOT_PLAY; surový signál (typ WAV nebo TZX 0x18) -> NOT_DECODED;
     * jinak OK. Přehratelný blok (lze ho otevřít, Next / Previous / Rewind
     * decku na něj míří) je OK nebo NOT_DECODED. Čte jen index pásky
     * a vedlejší pole rozšíření (cmttmz_get_block_ui()); nic nevypisuje ani
     * nemění.
     *
     * @param block_id Číslo bloku (od 0).
     * @param[out] status Stav (nesmí být NULL; při chybě CMT_BLOCK_STATUS_OK).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, bez indexu bloků nebo
     *         mimo rozsah.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    int cmt_tape_block_status ( int block_id, en_CMT_BLOCK_STATUS *status );

    /**
     * @brief První přehratelný blok vložené pásky (cíl Rewind decku a emu_cmt_tape_step "start").
     *
     * Přehratelný = cmt_tape_block_status() OK nebo NOT_DECODED; řídicí bloky
     * a bloky, které player neumí, se přeskočí stejně jako při Next /
     * Previous (cmt_tape_step_block()).
     *
     * @return Číslo bloku; -1 bez pásky, u prázdné pásky nebo bez přehratelného bloku.
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    int cmt_tape_first_playable_block ( void );

    int cmt_change_speed ( en_CMTSPEED cmtspeed );

    /**
     * @brief Nastaví výchozí rychlost Virtual CMT na vlastní délky pulzů.
     *
     * Uloží délky do g_cmt.mz_custom_pulses a výchozí rychlost přepne na
     * CMTSPEED_CUSTOM. Vložený blok s výchozí rychlostí se při zastaveném
     * transportu přegeneruje a převine na začátek (stejně jako
     * u cmt_change_speed(); pozice z cmt_tape_set_position() se zruší).
     *
     * @param pulses Délky pulzů (nesmí být NULL, všechny délky kladné).
     * @return EXIT_SUCCESS; EXIT_FAILURE, pokud se blok nepodařilo
     *         přegenerovat - nastavení se pak nemění.
     * @pre Voláno z emulátorového vlákna (nebo s pozastavenou emulací).
     */
    int cmt_change_custom_pulses ( const st_MZTAPE_PULSES_LENGTH *pulses );

    int cmt_tape_set_block_cmt_speed ( int block_id, en_CMTSPEED cmtspeed );

    /**
     * @brief Nastaví jednomu bloku vložené pásky vlastní délky pulzů.
     *
     * Obdoba cmt_tape_set_block_cmt_speed(): blok dostane pevnou rychlost
     * (CMTEXT_BLOCK_SPEED_SET) CMTSPEED_CUSTOM s danými délkami. Projeví se
     * při příštím otevření bloku; seznam bloků v GUI se označí k obnovení
     * a délky bloků pro okno, které tím přestaly platit, se označí jako
     * neznámé (cmttmz_lengths_invalidate(); doplní je zásah UI ve STOP).
     * Změnila-li se rychlost, páska je Modified.
     *
     * @param block_id Index bloku v páskovém kontejneru.
     * @param pulses Délky pulzů (nesmí být NULL).
     * @return 0 při úspěchu; -1 bez pásky, u pásky bez indexu bloků, mimo
     *         rozsah, u bloku, který není MZF, nebo u bloku MZF, jehož
     *         rychlost dává soubor (cmttmz_block_speed_changeable(); blok
     *         0x46 MZ800EMU-CMT z TMZ s čitelnými PROFILE_ARGS změnit jde).
     *         Při chybě se nic nemění.
     * @pre Voláno z emulátorového vlákna.
     */
    int cmt_tape_set_block_pulses ( int block_id, const st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Změní rychlost bloku pásky z výběru v seznamu bloků (GUI).
     *
     * Právě přehrávaný blok se nemění (EXIT_SUCCESS bez změny). Blok MZ
     * formátu dostane rychlost CMTEXT_BLOCK_SPEED_DEFAULT (pro
     * CMTSPEED_NONE), jinak CMTEXT_BLOCK_SPEED_SET; blok jiného formátu jen
     * rychlost. Je-li blok připravený (aktuální), otevře se znovu, aby se
     * stream přegeneroval. Nakonec obnoví seznam bloků v GUI a délky bloků
     * pro okno (cmttmz_note_change(), cmttmz_lengths_invalidate(): délka
     * znovu otevřeného bloku je známá z jeho streamu, ostatní neplatné jsou
     * neznámé). Změnila-li se rychlost bloku, páska je Modified (uložení
     * blok přestaví rychlostí z indexu, cmttmz_save()).
     *
     * @param block_id Index bloku v páskovém kontejneru.
     * @param cmtspeed CMTSPEED_NONE = výchozí rychlost Virtual CMT, poměr,
     *        nebo CMTSPEED_CUSTOM (pak je @p pulses povinné).
     * @param is_mz true = blok MZ formátu.
     * @param pulses Vlastní délky pulzů pro CMTSPEED_CUSTOM, jinak NULL.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, u pásky bez indexu
     *         bloků, mimo rozsah, u bloku, jehož rychlost na cmtspeed změnit
     *         nejde (cmttmz_block_speed_changeable(): rychlost ze souboru,
     *         WAV, CONTROL; blok TAP s CMTSPEED_NONE nebo CMTSPEED_CUSTOM),
     *         bez pulzů u CMTSPEED_CUSTOM nebo když se blok nepodařilo
     *         nastavit či znovu otevřít.
     * @pre Emu vlákno (zásah z GUI přes cmt_request_set_block_speed()).
     */
    int cmt_tape_change_block_speed ( int block_id, en_CMTSPEED cmtspeed, bool is_mz, const st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Vrátí délky pulzů, se kterými Virtual CMT přehrává danou rychlost.
     *
     * @param cmtspeed Poměr, nebo CMTSPEED_CUSTOM.
     * @param custom Vlastní délky pro CMTSPEED_CUSTOM (jinak se ignoruje, smí být NULL).
     * @param[out] pulses Výsledné délky v sekundách.
     * @return EXIT_SUCCESS; EXIT_FAILURE pro neplatnou rychlost nebo
     *         CMTSPEED_CUSTOM bez custom.
     */
    int cmt_get_speed_pulses ( en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *custom, st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Vrátí efektivní délky pulzů MZF bloku vložené pásky.
     *
     * Blok s pevnou rychlostí (CMTEXT_BLOCK_SPEED_SET) má svou rychlost,
     * blok s výchozí rychlostí výchozí rychlost Virtual CMT. Samostatné MZF
     * je páska s jediným blokem 0 s výchozí rychlostí.
     *
     * @param block_id Index bloku.
     * @param[out] pulses Délky pulzů v sekundách.
     * @return 0 při úspěchu; -1 bez pásky, u pásky bez indexu (nahrávání),
     *         mimo rozsah, u bloku, který není MZF, nebo u bloku MZF
     *         s CMTEXT_BLOCK_SPEED_NONE (rychlost dává soubor: jiné profily
     *         0x46, bloky 0x40/0x41; blok 0x46 MZ800EMU-CMT z TMZ je SET
     *         a pulzy vrací).
     * @pre Voláno z emulátorového vlákna (nebo z UI vlákna stejně jako
     *      ostatní čtení seznamu bloků).
     */
    int cmt_tape_get_block_pulses ( int block_id, st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Pozice pásky: otevřený blok a takty GDGCLK_BASE od začátku jeho streamu.
     *
     * Pozice zahrnuje i ticho za blokem, které přidal player; nese ji
     * časová základna transportu (STOP: st_CMTEXT_BLOCK::start_gdgticks,
     * pauza: g_cmt.paused_time, PLAY: teď - g_cmt.start_time). Bez
     * vedlejších efektů. Podrobně viz cmt.c.
     *
     * @param[out] block_id Číslo otevřeného bloku.
     * @param[out] ticks_in_block Pozice (nejvýš délka streamu bloku).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky nebo otevřeného bloku,
     *         u pásky jiného rozšíření než cmt_tmz, při nahrávání, NULL parametr.
     * @pre Jen vlákno emulátoru.
     */
    int cmt_tape_get_position ( int *block_id, uint64_t *ticks_in_block );

    /**
     * @brief Nastaví pozici pásky (blok a takty GDGCLK_BASE od začátku jeho streamu).
     *
     * Jen ve STOP (platí pro příští PLAY) nebo v pauze (i po zastavení
     * pásky; platí pro pokračování). Jiný blok se otevře přímo (player se
     * přestaví na blok). Pozice za koncem streamu se ořízne na konec.
     * Mění jen pozici (v pauze i úroveň výstupu podle pozice; ve STOP
     * výstup zůstává 0), nic nevypisuje. Změna rychlosti otevřeného bloku
     * pozici ve STOP zruší (blok se převine na začátek).
     *
     * @param block_id Index bloku v souboru pásky.
     * @param ticks_in_block Pozice v taktech GDGCLK_BASE.
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, u jiného rozšíření, při
     *         nahrávání, při PLAY bez pauzy, pro blok mimo rozsah, CONTROL
     *         nebo přeskakovaný (pak se nic nemění).
     * @pre Jen vlákno emulátoru. Při jiném bloku nahradí g_cmt.ext->block;
     *      UI ho čte pod device_state_lock(), funkce zámek nebere (viz cmt.c).
     */
    int cmt_tape_set_position ( int block_id, uint64_t ticks_in_block );

    /**
     * @brief Stav motoru kazetofonu, jak ho emulátor modeluje (čtení pro UI).
     *
     * Motor přepíná náběžná hrana PC3 8255 (báze hw/14-cmt.md, kap. Řízení
     * motoru). Emulátor ji modeluje takto (pio8255.c, pio8255_write()):
     * - s CMT hackem mimo nahrávání (CMTHACK_TEST_FAKES_MOTOR) přepíná hrana
     *   PC3 bit PC4 (signal_pc04), stav je znám a je to hodnota PC4,
     * - bez hacku (a s hackem při RECORD) hrana při PLAY / RECORD přepíná
     *   motor (cmt_motor_toggle());
     *   bez pauzy uživatele motor běží = transport není v pauze, při pauze
     *   uživatele nese stav motoru st_CMT::motor_off (stav je znám),
     * - bez hacku ve STOP se hrana ignoruje a klopný obvod motoru se
     *   nemodeluje: stav motoru není znám (PC4 pak čte 0, to je jen náhrada,
     *   ne stav klopného obvodu).
     *
     * Bez vedlejších efektů.
     *
     * @param[out] known true = stav motoru je znám (smí být NULL).
     * @param[out] on true = motor běží; při !known false (smí být NULL).
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (čte jen skalární
     *      hodnoty; výsledek je nanejvýš o snímek starý).
     */
    void cmt_motor_get_state ( bool *known, bool *on );

    /**
     * @brief Pozice pásky pro UI: otevřený blok a sekundy od začátku jeho streamu.
     *
     * Na rozdíl od cmt_tape_get_position() nedereferencuje otevřený blok
     * během PLAY (emu vlákno ho při přechodu na další blok vyměňuje bez
     * zámku): číslo bloku bere z cmttmz_get_open_block_id(), čas z časové
     * základny transportu (PLAY: teď - start_time, pauza: paused_time) a
     * strop z délky bloku v údajích pro UI (cmttmz_get_block_ui()). Ve STOP
     * čte st_CMTEXT_BLOCK::start_gdgticks (blok se ve STOP mění jen
     * zásahem pod device_state_lock()).
     *
     * @param[out] block_id Číslo otevřeného bloku; -1 bez bloku.
     * @param[out] pos_s Pozice v bloku v sekundách (0 bez bloku).
     * @return EXIT_SUCCESS; EXIT_FAILURE bez pásky, bez otevřeného bloku,
     *         při nahrávání, u jiného rozšíření než cmt_tmz nebo s NULL
     *         parametrem (výstupy pak -1 a 0, jsou-li ne-NULL).
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock().
     */
    int cmt_tape_get_ui_position ( int *block_id, double *pos_s );

    /**
     * @brief Průběh probíhajícího nahrávání pro UI: čas a počet pulzů.
     *
     * Čas je od začátku záznamu bez doby stojícího motoru (stejná základna
     * jako cmt_write_data()); počet pulzů = počet úseků bufferu hran / 2
     * (jeden pulz = dvě půlvlny; úseky z dělení dlouhého úseku se počítají
     * také).
     *
     * @param[out] time_s Čas záznamu v sekundách (smí být NULL).
     * @param[out] pulses Počet pulzů (smí být NULL).
     * @return EXIT_SUCCESS při RECORD; EXIT_FAILURE jinak (výstupy 0).
     * @pre Vlákno emulátoru, nebo UI pod device_state_lock() (čte jen
     *      skalární hodnoty; buffer hran mění emu vlákno bez zámku).
     */
    int cmt_record_get_progress ( double *time_s, uint32_t *pulses );

    void cmt_screen_done_period ( void );
    int cmt_read_data ( void );
    void cmt_update_output ( void );
    void cmt_write_data ( int value );

    void cmt_cpu_boost_set ( en_CMT_CPU_BOOST cpu_boost );

    /**
     * @brief Srovná MAX SPEED se stavem transportu podle volby cpu_boost.
     *
     * Sémantika cpu_boost: automatická MAX SPEED po dobu, kdy páska běží.
     * - cpu_boost zapnutý, páska vložená, PLAY nebo RECORD bez pauzy
     *   -> emulator_max_speed_boost(true) (zapne MAX SPEED, pokud neběží),
     * - jinak -> emulator_max_speed_boost(false): vypne MAX SPEED jen tehdy,
     *   když ji zapnul cpu_boost; MAX SPEED zvolenou uživatelem nechá.
     *
     * Volá se při každé změně transportu (play, pauza, stop), při změně
     * volby a po načtení snapshotu (snap_cmt_load) - snapshot obnoví stav
     * transportu, ale dříve rychlost emulace nesrovnal. Samotnou volbu
     * cpu_boost snapshot neobnovuje (uživatelská preference, do snapshotu
     * se zapisuje jen kvůli kompatibilitě); platí aktuální volba uživatele.
     * Při RECORD pak MAX SPEED dál řídí cmt_screen_done_period() podle
     * aktivity zápisu: po 5 s bez zápisu na pásku MAX SPEED od boostu
     * vypne, při další aktivitě (kontrola jednou za 50 snímků) ji zapne.
     * Vypne-li uživatel MAX SPEED během boostu, automatika ji znovu zapne
     * při nejbližší pauze/obnovení transportu nebo změně volby cpu_boost
     * (při RECORD i při další aktivitě zápisu).
     *
     * @pre Voláno z emulátorového vlákna nebo při pozastavené emulaci;
     *      platí invarianty st_CMT.
     * @post Bez aktivního transportu (nebo s vypnutým cpu_boost) neběží
     *       MAX SPEED zapnutá automatikou (g_emulator.max_speed_boost == false).
     */
    void cmt_cpu_boost_apply ( void );
    void cmt_mzfsize_check_set ( en_CMT_MZFSIZE_CHECK mzfsize_check );

    /**
     * @brief Nastaví automatickou opravu velikosti MZF (g_cmt.mzfsize_autofix).
     * @param autofix 0 = ptát se (popup MZF Size Warning), jinak zkrátit soubor bez dotazu.
     * @note Jen zápis příznaku; smí ho volat UI vlákno (příznak čte jen UI vlákno).
     */
    void cmt_mzfsize_autofix_set ( int autofix );

    /**
     * @brief Zapne nebo vypne echo kazety (g_cmt.echo).
     * @param echo 0 = vypnuto, jinak zapnuto.
     * @note Jen zápis příznaku; smí ho volat UI vlákno. Úroveň echa po
     *       vypnutí srovná do klidu emu vlákno na konci snímku
     *       (cmt_echo_frame()).
     */
    void cmt_echo_set ( int echo );

    /**
     * @brief Echo kazety na konci snímku: doručí hrany signálu pásky do zvukového logu.
     *
     * Při zapnutém echu a PLAY bez pauzy projde hrany přehrávaného bloku
     * až do aktuálního času (i když program PC5 nečte) a každou ohlásí
     * audio_cmt_echo_changed() s jejím časem. Mimo PLAY a RECORD bez
     * pauzy, bez pásky a s vypnutým echem vrátí úroveň echa do klidu (0).
     * Při RECORD hrany hlásí cmt_write_data().
     *
     * Stav stroje nemění: čtecí pozice streamu se posune jen tam, kam by ji
     * posunulo nejbližší cmt_update_output().
     *
     * @pre Vlákno emulátoru, konec snímku před audiolog_finish_20ms_frame()
     *      (makro cmt_on_frame_audio_event()).
     */
    void cmt_echo_frame ( void );

#ifdef __cplusplus
}
#endif

#endif /* CMT_H */

