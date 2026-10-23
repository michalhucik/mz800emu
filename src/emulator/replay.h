/**
 * @file   replay.h
 * @brief  Záznam a přehrání vstupů (deterministický replay), jádro.
 *
 * Záznam = startovní snapshot + změny vstupů a zásahy na hranici snímku
 * + periodické hashe stavu (formát v replay_format.h). Přehrání načte
 * snapshot, na stejných místech aplikuje vstupy a porovnává hashe; první
 * rozdílný snímek nahlásí a emulaci zastaví.
 *
 * Body v emu vlákně (všechny mimo instrukční smyčku):
 *
 * | Funkce | Kde | Co |
 * |--------|-----|----|
 * | replay_on_frame_boundary() | per-event blok hlavní smyčky za run bounds, před testem pauzy (bod B_f) | reset, vstupy, hash, konec přehrávání |
 * | replay_on_drain_point() | za drainem fronty dbgapi (jen debugger) | přehrání zásahů `d` |
 * | replay_on_paused() | každý průchod paused smyčkou | start, stop, reset a autotype v pauze |
 *
 * Hlavní smyčka volá replay_on_frame_boundary() jen za příznakem, který
 * nastaví konec snímku, a to jen při běžícím záznamu / přehrávání nebo
 * čekajícím požadavku (replay_frame_hook_wanted()). Bez záznamu tak
 * v per-event bloku přibude jen test jednoho příznaku.
 *
 * Checkpointy (replay_ckpt.h): při záznamu i přehrávání se každých N snímků
 * v bodě B_f uloží snapshot do paměti (kruhový buffer K posledních).
 * Rewind při záznamu načte checkpoint a zahodí vstupy po něm, seek při
 * přehrávání skočí na snímek (zpět přes checkpoint nebo restart od
 * začátku, dopředu dohráním v MAX SPEED). Checkpoint za zápisem na médium
 * (media_writes.h) použít nejde.
 *
 * Řízení (GUI, MCP, CLI) jde přes frontu požadavků s pořadovým číslem
 * (replay_request_*()); emu vlákno je vyřídí v paused smyčce. Za běhu
 * emulace se proto emulace v nejbližší hranici snímku pozastaví, požadavek
 * se vyřídí a emulace se znovu rozběhne. Stav čte kdokoliv přes
 * replay_get_status() (kopie pod zámkem).
 *
 * Vstupy z UI vlákna (klávesnice, VKBD, joystick z numerické klávesnice,
 * autotype, reset) jdou přes replay_host_*(): mimo záznam se zapíší
 * hned jako dřív, při záznamu se převezmou v bodě B_f, při přehrávání se
 * zahodí (živý vstup se ignoruje).
 */

#ifndef REPLAY_H
#define REPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mzarch/mzarch_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Režim záznamu a přehrávání.
     */
    typedef enum en_REPLAY_MODE
    {
        REPLAY_MODE_OFF = 0,   /**< Nic neběží (živý vstup jako dřív). */
        REPLAY_MODE_RECORDING, /**< Probíhá záznam. */
        REPLAY_MODE_PLAYING    /**< Probíhá přehrávání. */
    } en_REPLAY_MODE;

    /**
     * @brief Výsledek posledního skončeného záznamu nebo přehrávání.
     */
    typedef enum en_REPLAY_OUTCOME
    {
        REPLAY_OUTCOME_NONE = 0,     /**< Zatím nic neskončilo. */
        REPLAY_OUTCOME_RECORDED,     /**< `recorded`: záznam uložen do souboru. */
        REPLAY_OUTCOME_ENDED,        /**< `ended`: přehrávání došlo na konec bez rozjetí. */
        REPLAY_OUTCOME_DESYNC,       /**< `desync`: hash se lišil, přehrávání zastaveno. */
        REPLAY_OUTCOME_STOPPED,      /**< `stopped`: přehrávání zastavil uživatel. */
        REPLAY_OUTCOME_ERROR         /**< `error`: start nebo uložení selhalo (viz `message`). */
    } en_REPLAY_OUTCOME;

    /** @brief Délka textových polí ve st_REPLAY_STATUS (včetně nuly). */
#define REPLAY_STATUS_TEXT_MAX 256

    /** @brief Délka cesty ve st_REPLAY_STATUS (včetně nuly). */
#define REPLAY_STATUS_PATH_MAX 1024

    /**
     * @brief Kopie stavu záznamu a přehrávání pro UI, MCP a CLI.
     *
     * @invariant Řetězce jsou vždy ukončené nulou (případně zkrácené).
     */
    typedef struct st_REPLAY_STATUS
    {
        en_REPLAY_MODE mode;         /**< Aktuální režim. */
        uint32_t frame;              /**< Snímek od startu (záznam i přehrávání). */
        uint32_t frames;             /**< Délka přehrávaného nebo naposledy přehraného záznamu (0 při záznamu). */
        uint32_t events;             /**< Počet zaznamenaných / přehrávaných událostí. */
        uint32_t hashes;             /**< Počet řádků hashů (zaznamenaných / ověřených). */
        uint32_t hash_interval;      /**< Interval hashů ve snímcích. */
        uint32_t warnings;           /**< Počet varování záznamu. */
        bool deviated;               /**< Přehrávání odchýleno zásahem (hashe se dál neověřují). */
        char deviation[REPLAY_STATUS_TEXT_MAX]; /**< Čím odchýleno (anglicky), "" = nic. */
        char path[REPLAY_STATUS_PATH_MAX];      /**< Soubor aktuálního nebo posledního záznamu. */
        uint32_t request_done;       /**< Číslo posledního vyřízeného požadavku (replay_request_*). */
        bool request_ok;             /**< Výsledek posledního vyřízeného požadavku. */
        char request_message[REPLAY_STATUS_TEXT_MAX]; /**< Chyba nebo poznámka k požadavku (anglicky). */
        en_REPLAY_OUTCOME outcome;   /**< Jak skončil poslední záznam / přehrávání. */
        uint32_t outcome_frame;      /**< Snímek konce nebo prvního rozjetí. */
        char outcome_message[REPLAY_STATUS_TEXT_MAX]; /**< Popis konce (anglicky). */
        char desync_parts[REPLAY_STATUS_TEXT_MAX];    /**< Rozjeté části oddělené čárkou (`cpu,ram,time`). */
        uint32_t checkpoint_interval; /**< Interval checkpointů ve snímcích (0 = vypnuto). */
        uint32_t checkpoint_capacity; /**< Největší počet uchovaných checkpointů. */
        uint32_t checkpoints;        /**< Počet uchovaných checkpointů. */
        uint32_t checkpoint_first;   /**< Snímek nejstaršího checkpointu (platí při `checkpoints` > 0). */
        uint32_t checkpoint_last;    /**< Snímek nejnovějšího checkpointu (platí při `checkpoints` > 0). */
        uint64_t checkpoint_bytes;   /**< Paměť checkpointů v bajtech. */
        uint32_t rerecord_count;     /**< Počet rewindů v probíhajícím nebo posledním záznamu. */
        bool seeking;                /**< Přehrávání právě dohrává na cíl seeku (MAX SPEED). */
        uint32_t seek_target;        /**< Cílový snímek seeku (platí při `seeking`). */
        bool goto_active;            /**< Goto právě běží (seek a dojetí na čas). */
        uint64_t goto_target;        /**< Cílový čas posledního goto v taktech GDG. */
        int32_t goto_seek_from;      /**< Odkud poslední goto vyšlo: snímek checkpointu (>= 0),
                                          REPLAY_GOTO_FROM_RESTART, REPLAY_GOTO_FROM_NONE. */
        bool goto_checkpoint;        /**< Poslední goto uložilo checkpoint v cílovém snímku. */
    } st_REPLAY_STATUS;

    /** @brief st_REPLAY_STATUS::goto_seek_from: goto dojelo bez seeku (cíl dopředu ve stejném snímku). */
#define REPLAY_GOTO_FROM_NONE (-2)
    /** @brief st_REPLAY_STATUS::goto_seek_from: seek zpět bez checkpointu = přehrávání od začátku. */
#define REPLAY_GOTO_FROM_RESTART (-1)
    /**
     * @brief Hranice snímku B_f přijde až po instrukci, která konec snímku
     *        přešla (a po přijetí přerušení): takty GDG od začátku snímku,
     *        do kterých goto začíná o snímek dřív, aby seek nepřejel cíl.
     */
#define REPLAY_GOTO_BOUNDARY_MARGIN 4096u

    /**
     * @brief Nastavení checkpointů jednoho záznamu nebo přehrávání.
     *
     * Záporná hodnota = převzít z konfigurace (sekce `[REPLAY]` INI,
     * výchozí REPLAY_CKPT_DEFAULT_INTERVAL a REPLAY_CKPT_DEFAULT_COUNT).
     */
    typedef struct st_REPLAY_CKPT_CFG
    {
        int interval; /**< Interval ve snímcích, 0 = checkpointy vypnout, < 0 = z INI. */
        int count;    /**< Počet uchovaných (1 .. REPLAY_CKPT_MAX_COUNT), < 0 = z INI. */
    } st_REPLAY_CKPT_CFG;

    /**
     * @brief Aktuální režim (en_REPLAY_MODE jako int).
     *
     * Zapisuje jen emu vlákno (atomicky); číst smí kdokoliv atomicky
     * (g_atomic_int_get) nebo emu vlákno přímo.
     */
    extern int g_replay_mode;

    /**
     * @brief Příznaky pro háček konce snímku: bit 0 = záznam nebo přehrávání
     *        běží, bit 1 = čeká požadavek (replay_request_*()).
     *
     * Bit 0 píše jen emu vlákno, bit 1 nastavuje žadatel a nuluje emu
     * vlákno; vše atomicky (g_atomic_int_*).
     */
    extern int g_replay_frame_hook;

    /**
     * @brief true = při konci snímku má hlavní smyčka zavolat replay_on_frame_boundary().
     * @note Emu vlákno, volá se z callbacku konce snímku (jedno čtení).
     */
#define replay_frame_hook_wanted() (g_replay_frame_hook != 0)

    /**
     * @brief true = běží záznam nebo přehrávání (atomické čtení, libovolné vlákno).
     */
    bool replay_is_active(void);

    /**
     * @brief Jméno výsledku (`recorded`, `ended`, `desync`, `stopped`, `error`, `none`).
     */
    const char *replay_outcome_name(en_REPLAY_OUTCOME outcome);

    /**
     * @brief Jméno režimu (`off`, `recording`, `playing`).
     */
    const char *replay_mode_name(en_REPLAY_MODE mode);

    /* ----------------------------------------------------------------
     * Inicializace
     * ---------------------------------------------------------------- */

    /**
     * @brief Inicializace modulu (výchozí stav vstupů). Volat jednou při startu
     *        emulátoru; zámek je statický a funguje i před ní.
     */
    void replay_init(void);

    /**
     * @brief Ukončení: běžící záznam uloží (se závěrečným hashem, jen když emulace
     *        stojí na hranici snímku), přehrávání zahodí.
     *
     * @pre Emu vlákno už neběží nebo stojí (volá se z emulator_quit*).
     * @post `g_replay_mode` == REPLAY_MODE_OFF.
     */
    void replay_shutdown(void);

    /* ----------------------------------------------------------------
     * Požadavky (libovolné vlákno)
     * ---------------------------------------------------------------- */

    /**
     * @brief Požádá o start záznamu do souboru.
     *
     * @param path          Cílový soubor `.mzr` (zkopíruje se).
     * @param hash_interval Interval hashů ve snímcích; 0 = výchozí (50).
     * @param description   Popis do manifestu nebo NULL.
     * @return Číslo požadavku pro replay_wait_request() (vždy > 0).
     */
    uint32_t replay_request_record(const char *path, uint32_t hash_interval, const char *description);

    /**
     * @brief Start záznamu s vlastním nastavením checkpointů.
     *
     * @param path          Cílový soubor `.mzr` (zkopíruje se).
     * @param hash_interval Interval hashů ve snímcích; 0 = výchozí (50).
     * @param description   Popis do manifestu nebo NULL.
     * @param ckpt          Nastavení checkpointů, NULL = z konfigurace.
     * @return Číslo požadavku pro replay_wait_request() (vždy > 0).
     */
    uint32_t replay_request_record_ex(const char *path, uint32_t hash_interval, const char *description,
                                      const st_REPLAY_CKPT_CFG *ckpt);

    /**
     * @brief Požádá o start přehrávání souboru.
     *
     * @param path            Soubor `.mzr`.
     * @param pause_after_load true = po načtení zůstat v pauze; false = rozběhnout.
     * @return Číslo požadavku.
     */
    uint32_t replay_request_play(const char *path, bool pause_after_load);

    /**
     * @brief Start přehrávání s vlastním nastavením checkpointů.
     *
     * @param path             Soubor `.mzr`.
     * @param pause_after_load true = po načtení zůstat v pauze.
     * @param ckpt             Nastavení checkpointů, NULL = z konfigurace.
     * @return Číslo požadavku.
     */
    uint32_t replay_request_play_ex(const char *path, bool pause_after_load, const st_REPLAY_CKPT_CFG *ckpt);

    /**
     * @brief Požádá o rewind (návrat na checkpoint).
     *
     * Při záznamu načte checkpoint, zahodí vstupy a hashe po něm, zvýší
     * `rerecord_count` a záznam pokračuje; při přehrávání skočí zpět (jako
     * replay_request_seek()). Po vyřízení emulace stojí na hranici snímku
     * checkpointu.
     *
     * @param frames 0 = o jeden checkpoint zpět (nejnovější se snímkem menším
     *               než aktuální); jinak nejnovější checkpoint aspoň o tolik
     *               snímků zpět.
     * @return Číslo požadavku. Odmítnutí (žádný checkpoint, zápis na médium
     *         po checkpointu) je ve stavu (`request_ok` = false).
     */
    uint32_t replay_request_rewind(uint32_t frames);

    /**
     * @brief Požádá o skok přehrávání na snímek.
     *
     * Dopředu: dohraje v MAX SPEED a zastaví na hranici cílového snímku
     * (pauza s důvodem EMU_PAUSE_REASON_REPLAY_SEEK). Zpět: načte nejbližší
     * použitelný checkpoint (jinak spustí přehrávání znovu od začátku
     * s čistými kopiemi médií) a dohraje. Cíl za koncem záznamu se omezí na
     * konec. Hashe se při dohrávání dál ověřují.
     *
     * @param frame Cílový snímek od startu záznamu.
     * @return Číslo požadavku (jen při přehrávání, jinak se odmítne).
     */
    uint32_t replay_request_seek(uint32_t frame);

    /**
     * @brief Požádá o skok přehrávání na čas (goto, stav v libovolném bodě záznamu).
     *
     * Spočítá snímek cíle (o snímek dřív, je-li cíl v prvních
     * REPLAY_GOTO_BOUNDARY_MARGIN taktech snímku), přesune se na jeho
     * hranici jako replay_request_seek() (checkpoint, start od začátku, nebo
     * bez seeku, je-li cíl dopředu ve stejném snímku) a dojede v MAX SPEED
     * se zastavením na čase (run_control_stop_at_arm()): pauza s důvodem
     * EMU_PAUSE_REASON_REPLAY_GOTO na začátku první instrukce s časem
     * >= @p ticks. Uživatelské breakpointy jsou po dobu goto potlačené
     * (g_debugger.bp_suppress), pokud @p keep_breakpoints není true.
     * Goto končí první pauzou (cíl, breakpoint s keep, konec nebo rozjetí
     * přehrávání, uživatel). Jen při přehrávání a v buildu s debuggerem.
     *
     * @param ticks                Cílový čas v taktech GDG (gdg_get_total_ticks()).
     * @param keep_breakpoints     true = uživatelské breakpointy platí dál.
     * @param checkpoint_at_target true = na hranici snímku seeku uložit
     *                             checkpoint (další goto v okolí bez seeku od
     *                             vzdálenějšího checkpointu); novější zůstanou.
     * @return Číslo požadavku. Odmítnutí (mimo přehrávání, čas před startem
     *         nebo za koncem záznamu) je ve stavu (`request_ok` = false).
     */
    uint32_t replay_request_goto(uint64_t ticks, bool keep_breakpoints, bool checkpoint_at_target);

    /**
     * @brief Požádá o konec: záznam uloží, přehrávání ukončí (výsledek `stopped`).
     * @return Číslo požadavku.
     */
    uint32_t replay_request_stop(void);

    /**
     * @brief Počká, až emu vlákno požadavek vyřídí.
     *
     * @param seq        Číslo z replay_request_*().
     * @param timeout_ms Nejdelší čekání.
     * @return true = vyřízeno (výsledek ve stavu: `request_ok`,
     *         `request_message`); false = vypršel čas.
     * @note Nevolat z emu vlákna (vyřizuje je právě ono).
     */
    bool replay_wait_request(uint32_t seq, int timeout_ms);

    /**
     * @brief Kopie stavu (pod zámkem, libovolné vlákno).
     */
    void replay_get_status(st_REPLAY_STATUS *out);

    /* ----------------------------------------------------------------
     * Vstupy z UI vlákna
     * ---------------------------------------------------------------- */

    /**
     * @brief Předá výsledek skenu klávesnice hostitele (celá matice).
     *
     * @param matrix    10 sloupců, 0 = stisknuto.
     * @param joy_dev   Index zařízení joysticku z numerické klávesnice, -1 = žádné.
     * @param joy_state Stav toho joysticku (platí jen při @p joy_dev >= 0).
     *
     * Mimo záznam uloží sken jako čekající a `g_pio8255.keyboard_matrix`
     * (a `g_joy`) zapíše emu vlákno v replay_ui_off_drain() na hranici
     * snímku nebo v paused smyčce (stisk se projeví nejvýš o snímek
     * později); revizi stavu zvedne jen změna matice nebo joysticku. Při
     * záznamu uloží jako čekající (převezme se v bodě B_f), při přehrávání
     * zahodí. Platí vždy poslední sken.
     *
     * @note UI vlákno; matici stroje samo nezapisuje.
     */
    void replay_host_keyboard_submit(const uint8_t *matrix, int joy_dev, uint8_t joy_state);

    /**
     * @brief Stisk nebo uvolnění klávesy okna virtuální klávesnice.
     *
     * Mimo záznam zařadí změnu bitu `g_pio8255.vkbd_matrix` pro emu vlákno
     * (provede ji nejbližší replay_on_frame_boundary() nebo
     * replay_on_paused(); platí poslední změna bitu), při záznamu změní
     * čekající matici okna, při přehrávání nic.
     *
     * @param col Sloupec matice (mimo rozsah = nic).
     * @param bit Bit sloupce 0-7 (mimo rozsah = nic).
     * @param pressed true = stisk.
     * @note UI vlákno.
     */
    void replay_host_vkbd_set(int col, int bit, bool pressed);

    /**
     * @brief Start (text != NULL) nebo zastavení (NULL) autotype z okna.
     *
     * @param text  Text (zkopíruje se, pokud jde do záznamu) nebo NULL.
     * @param kd_ms Doba stisku v ms.
     * @param ku_ms Doba uvolnění v ms.
     *
     * Text se zkopíruje. Mimo záznam ho spustí emu vlákno v nejbližším
     * replay_on_frame_boundary() nebo replay_on_paused() (vlastní kopie,
     * UI buffer může volající hned měnit); při záznamu se převezme v bodě
     * B_f a zapíše; při přehrávání se zahodí. Průběh ukáže
     * pio8255_autotype_progress().
     *
     * @note UI vlákno.
     */
    void replay_host_autotype(const char *text, double kd_ms, double ku_ms);

    /**
     * @brief Změna dob stisku a uvolnění běžícího autotype z okna.
     *
     * Mimo záznam je nastaví emu vlákno (jako replay_host_autotype());
     * při záznamu a přehrávání se změna neuplatní (záznam by ji
     * nezopakoval).
     *
     * @param kd_ms Doba stisku v ms.
     * @param ku_ms Doba uvolnění v ms.
     * @note UI vlákno.
     */
    void replay_host_autotype_delays(double kd_ms, double ku_ms);

    /**
     * @brief Zkopíruje zbytek textu běžícího autotype (pro okna GUI).
     *
     * Text autotype vlastní emu vlákno a nahrazuje ho; kopie běží pod
     * zámkem jeho životnosti, takže se nečte uvolněná paměť.
     *
     * @param buf  Cílový buffer (nesmí být NULL).
     * @param size Jeho velikost (0 = nic); delší zbytek se ořízne.
     * @return true = autotype běží (buf = zbytek textu); false = neběží
     *         (buf = "").
     * @note Libovolné vlákno.
     */
    bool replay_autotype_peek_rest(char *buf, size_t size);

    /**
     * @brief Komponenty snapshotu části `periph` (hash záznamu vstupů).
     *
     * Jediný seznam pro hash `periph` záznamu vstupů i stejnojmennou část
     * otisku stroje (machine_hash.h). Seznam se nemění: nová komponenta
     * by změnila hashe a starší záznamy `.mzr` by se při ověření rozjely.
     *
     * @param[out] out_count Počet jmen (nesmí být NULL).
     * @return Statické pole jmen komponent (snapshot_hash_components()).
     * @note Libovolné vlákno.
     */
    const char *const *replay_periph_components(size_t *out_count);

    /**
     * @brief Přesměrování žádosti o reset (volá mzarch_platform_fn_reset_request()).
     *
     * @return true = žádost převzal záznam (volající ji nemá zpracovat);
     *         false = zpracovat jako dřív (mimo záznam; při přehrávání se
     *         přehrávání předtím označí jako odchýlené).
     */
    bool replay_divert_reset_request(void);

    /* ----------------------------------------------------------------
     * Háčky emu vlákna
     * ---------------------------------------------------------------- */

    /**
     * @brief Bod B_f (viz tabulka v hlavičce souboru).
     *
     * Bez záznamu a přehrávání vyřídí vstupy a operace kazety z UI
     * (replay_host_cmt(), replay_host_autotype(),
     * replay_host_autotype_delays(), replay_host_vkbd_set()).
     *
     * @pre Emu vlákno, per-event blok, mezi instrukcemi; záznam nebo
     *      přehrávání běží, nebo čekají vstupy z UI (bit háčku).
     */
    void replay_on_frame_boundary(void);

    /**
     * @brief Přehrání zásahů `d` aktuálního snímku (za drainem fronty dbgapi).
     * @pre Emu vlákno; bez přehrávání nic nedělá.
     */
    void replay_on_drain_point(void);

    /**
     * @brief Průchod paused smyčkou: vyřízení požadavků, resetu a autotype v pauze.
     * @pre Emu vlákno, emulace v pauze.
     */
    void replay_on_paused(void);

    /**
     * @brief Oznámení zápisu do stavu mimo záznam (akce breakpointu, Freeze Bytes).
     *
     * @param what Krátký popis anglicky (`breakpoint action poke`, ...).
     *
     * Při záznamu přidá varování do manifestu, při přehrávání přejde do
     * stavu odchýleno. Mimo záznam nic.
     * @pre Emu vlákno.
     */
    void replay_note_external_write(const char *what);

    /**
     * @brief true = emulace stojí v pauze na hranici snímku záznamu.
     *
     * Hranice = od posledního bodu B_f (nebo od startu) se nezměnil čas GDG.
     * @pre Emu vlákno.
     */
    bool replay_paused_at_boundary(void);

    /**
     * @brief Snímek od startu záznamu / přehrávání (emu vlákno).
     */
    uint32_t replay_current_frame(void);

    /**
     * @brief Operace kazety, kterou záznam umí zaznamenat a přehrát.
     *
     * Jména (replay_cmt_op_name()) jsou hodnota `op` události `cmt`.
     */
    typedef enum en_REPLAY_CMT_OP
    {
        REPLAY_CMT_PLAY = 0,       /**< `play`: cmt_play(). */
        REPLAY_CMT_PLAY_PAUSED,    /**< `play_paused`: cmt_play_paused(). */
        REPLAY_CMT_STOP,           /**< `stop`: cmt_stop() (MCP). */
        REPLAY_CMT_STOP_BUTTON,    /**< `stop_button`: cmt_stop_button() (okno; při nahrávání uloží a znovu vloží pásku). */
        REPLAY_CMT_PAUSE,          /**< `pause`: cmt_pause(arg). */
        REPLAY_CMT_EJECT,          /**< `eject`: cmt_eject(). */
        REPLAY_CMT_SEEK,           /**< `seek`: otevření bloku arg (MCP `cmt_tape_seek`). */
        REPLAY_CMT_SELECT,         /**< `select`: cmt_tape_select_block(arg, 0) (kliknutí v seznamu bloků). */
        REPLAY_CMT_SELECT_FORCED,  /**< `select_forced`: cmt_tape_select_block(arg, 1) (dvojklik). */
        REPLAY_CMT_NEXT,           /**< `next`: cmt_tape_step_block(1). */
        REPLAY_CMT_PREV,           /**< `prev`: cmt_tape_step_block(0). */
        REPLAY_CMT_RECORD,         /**< `record`: cmt_record_to_file(path); při přehrání do dočasného souboru. */
        REPLAY_CMT_OP_COUNT        /**< Počet operací (není operace). */
    } en_REPLAY_CMT_OP;

    /**
     * @brief Jméno operace kazety (`play`, `stop_button`, ...).
     * @return Statický řetězec; mimo rozsah "unknown".
     */
    const char *replay_cmt_op_name(en_REPLAY_CMT_OP op);

    /**
     * @brief Operace kazety z okna Virtual CMT / Tape Filelist (UI vlákno).
     *
     * Operaci provede emu vlákno: za běhu v nejbližším bodě hranice snímku,
     * v pauze v paused smyčce (do ~20 ms). Při záznamu se zapíše jako
     * událost `cmt` (v pauze mimo hranici snímku čeká na další hranici, jako
     * reset). Při přehrávání se zahodí (okna jsou zamčená).
     *
     * @param op   Operace.
     * @param arg  Hodnota (pauza 0/1, číslo bloku), jinak 0.
     * @param path Cesta souboru u REPLAY_CMT_RECORD, jinak NULL (zkopíruje se).
     * @note Libovolné vlákno; pod zámkem fronty.
     */
    void replay_host_cmt(en_REPLAY_CMT_OP op, int32_t arg, const char *path);

    /**
     * @brief CMT hack při přehrávání: další soubor ze záznamu místo dialogu.
     *
     * Volá cmthack_load_file() před dialogem. Soubory se berou v pořadí
     * výběrů při záznamu (ne podle snímku: výběr nastává uvnitř instrukce,
     * kterou určuje běh programu). Chybějící soubor = přehrání odchýleno
     * a načtení se zruší.
     *
     * @param[out] out_path Nová cesta dočasné kopie (g_free), NULL = zrušit
     *                 načtení (v záznamu zrušeno, nebo soubor chybí).
     * @return true = přehrávání odpovědělo (dialog neotevírat);
     *         false = přehrávání neběží, otevřít dialog jako obvykle.
     * @pre Emu vlákno (uvnitř instrukce OUT s CMT hackem).
     */
    bool replay_cmthack_playback(char **out_path);

    /**
     * @brief CMT hack při záznamu: zapíše soubor vybraný v dialogu.
     * @param chosen Cesta souboru, NULL = dialog zrušen.
     * @pre Emu vlákno.
     */
    void replay_cmthack_note(const char *chosen);

    /**
     * @brief Místní čas pro RTC Unicardu při záznamu a přehrávání.
     *
     * Čas = místní čas hostitele při startu záznamu (`rtc_base` v manifestu)
     * + emulovaný čas od startu (takty GDG). Záznam i přehrání tak program
     * vidí stejné hodiny; počítá se bez časového pásma (přehrání v jiném
     * pásmu dá stejné hodnoty).
     *
     * @param[out] year, month, day, hour, min, sec Složky času (rok celý,
     *             měsíc 1-12).
     * @return true = vyplněno; false = záznam ani přehrání neběží, nebo
     *         záznam čas nemá (starší soubor) - pak platí hostitelský čas.
     * @pre Emu vlákno (čtení RTC programem).
     */
    bool replay_rtc_now(int *year, int *month, int *day, int *hour, int *min, int *sec);

    /**
     * @brief Konec přehrávání ukončí proces (`--exit-at-replay-end`).
     *
     * @param enable true = na konci přehrávání (i po rozjetí) požádat
     *               automatizaci o konec s kódem 0 (`replay_end`) nebo 4
     *               (`replay_desync`); false = jen pauza.
     * @pre Před startem emulace (CLI).
     */
    void replay_set_exit_at_end(bool enable);

    /**
     * @brief Text posledního odmítnutí příkazu branou záznamu (anglicky).
     *
     * Text si vezme jen jeden čtenář: po přečtení se vymaže, aby se
     * nepřipojil k pozdější nesouvisející chybě (MCP dispatch ho přidá
     * k chybě příkazu).
     *
     * @param[out] out Buffer; po návratu text nebo "" (žádné odmítnutí).
     * @param size     Velikost bufferu.
     */
    void replay_take_refusal(char *out, size_t size);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    struct st_DBGAPI_CMDRQ;

    /**
     * @brief Brána záznamu před provedením příkazu dbgapi.
     *
     * Při záznamu odmítne příkaz, který mění stav a záznam ho neumí
     * zopakovat, a příkaz měnící stav nebo vstup v pauze mimo hranici
     * snímku (text přes replay_take_refusal()).
     *
     * @return true = provést; false = odmítnuto (volající nastaví success = false).
     * @pre Emu vlákno; volá se jen při `g_replay_mode != REPLAY_MODE_OFF`.
     */
    bool replay_dbg_before(struct st_DBGAPI_CMDRQ *rq);

    /**
     * @brief Brána záznamu po provedení příkazu dbgapi.
     *
     * Při záznamu uloží zásah (`d` za běhu, `p` v pauze) nebo nový vzorek
     * vstupů po vstupním příkazu v pauze; při přehrávání označí odchýlení
     * po živém zásahu do stavu.
     *
     * @param rq         Provedený příkaz.
     * @param was_paused Emulace stála před provedením.
     * @pre Emu vlákno; jen pokud replay_dbg_before() vrátilo true.
     */
    void replay_dbg_after(struct st_DBGAPI_CMDRQ *rq, bool was_paused);
#endif

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_H */
