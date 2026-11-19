/**
 * @file fdc.h
 * @brief FDC subsystém - veřejné API.
 *
 * Pokrývá vrstvy BUS + Translation + Drives (mount stav). Vlastní chip
 * logiku deklaruje wd279x.h.
 *
 * Historicky existoval runtime dispatcher mezi dvěma implementacemi
 * (_old_ a _new_) volený za běhu; OLD byla po stabilizaci NEW odstraněna
 * a tenká dispatcher vrstva byla nakonec rozpuštěna - implementace z
 * `fdc_new.{c,h}` se sloučila přímo sem.
 *
 * ## State
 *
 * Stav drží pole `g_fdc[FDC_INSTANCE_COUNT]` struktur `st_FDC` (FDC0 =
 * primární, FDC1 = sekundární). Každá instance má:
 *  - `index` - vlastní index v poli (FDC0/FDC1).
 *  - `connected` - 0/1 podle toho zda je řadič připojený. Tento flag je
 *    dotazován z hot-path call sites (IORQ handler) a slouží k rychlé
 *    negaci volání impl.
 *  - `hd_patch` - 0/1 HD Patch obvod (port 0xDF EINT logika).
 *  - `bus_xlate` - polarita BUS translation vrstvy (INVERT/PASSTHROUGH).
 *  - `wd279x` - stav WD279x chipu (registry, buffer, state machine).
 *  - `drive[FDC_NUM_DRIVES]` - mechaniky 0..3 s mount stavem.
 *
 * @note Etapa 1: aktivní je pouze FDC0; FDC1 je inertní placeholder
 *       (disconnected), zatím bez napojení na IORQ/menu/konfiguraci.
 *
 * ## Macra
 *
 * `FDC_TEST_*`, `FDC_SET_*`, `FDC_CONNECTED`, `FDC_DISCONNECTED`
 * operují (zatím) nad `g_fdc[FDC0].connected`.
 *
 * `FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id)` expanduje na volání funkce
 * `fdc_test_drive_id_mounted(fdc, drive_id)`.
 *
 * ## Stejný obraz ve více mechanikách
 *
 * Tentýž soubor obrazu smí být vložený ve více mechanikách (i přes oba
 * řadiče), ale platí invariant: **do jednoho souboru smí v každém okamžiku
 * zapisovat nejvýš jedna mechanika.** Shodu souboru určuje identita souboru
 * (fdc_fileid.h), ne text cesty.
 *
 *  - Vložení souboru, který už drží jiná mechanika, dostane ochranu proti
 *    zápisu kvůli sdílení (`shared_readonly`); emulovaný stroj vidí disketu
 *    chráněnou proti zápisu a mechanika do souboru nikdy nezapíše.
 *  - Ochranu takové mechaniky nejde sundat, dokud tentýž soubor drží jiná
 *    mechanika se zápisem (fdc_drive_set_write_protect() vrátí chybu,
 *    fdc_drive_write_blocker() řekne, která to je).
 *  - Ochrana kvůli sdílení se sama nesundá: zůstane, dokud ji uživatel
 *    nevypne (fdc_drive_set_write_protect(..., 0)) nebo dokud se z mechaniky
 *    obraz nevysune či nenahradí jiným. Do INI se neukládá; po startu se
 *    obrazy z INI vkládají v pořadí FDC0 FD0..FD3, FDC1 FD0..FD3 a ochranu
 *    dostane každá další mechanika se stejným souborem.
 *  - Obrazy nasazené z paměti (fdc_mount_dsk_buffer(), přehrání vstupů)
 *    soubor nedrží a pravidlo se jich netýká.
 *
 * Známá omezení:
 *  - Záznam vstupů: vloží-li emulovaný správce Unicard během nahrávání
 *    obraz, který už drží jiná mechanika, dostane ochranu kvůli sdílení.
 *    Při přehrání běží mechaniky nad kopiemi z paměti, kterých se pravidlo
 *    netýká, takže totéž vložení chráněné není a přehrání se od záznamu
 *    může rozejít. Záznamy pořízené dřív se nemění.
 *  - Návrat disket uživatele po přehrání vstupů (replay_media_restore())
 *    vkládá v pořadí mechanik; role zapisující a chráněné mechaniky se
 *    proti stavu před přehráním mohou prohodit. O data se nepřijde, obrazy
 *    se čtou znovu ze souboru.
 *
 * Kontrola běží jen při vložení, vysunutí a změně ochrany; čtení a zápis
 * sektorů se řídí jen polem `readonly` jako dřív.
 *
 * Mechaniky se v hlášeních číslují přes oba řadiče jako v příkazové řádce
 * (fdc_cli.h): 0-3 = FDC0, 4-7 = FDC1 (FDC_GLOBAL_DRIVE_NO()).
 *
 * ## Disk se neuloží do jiného souboru, než ze kterého byl načten
 *
 * Kopie obrazu z paměti (režimy cached a discard) se do souboru `filename`
 * zapíše jen tehdy, když je to tentýž soubor, ze kterého mechanika obraz
 * načetla (porovnání s identitou `fileid` z okamžiku vložení). Pole
 * `filename` se s vloženým obrazem za běžného provozu nerozchází (načtení
 * snapshotu vložená média nemění, snap_fdc.c); rozejít se může jen při
 * načtení snapshotu záznamu vstupů (přehrání staršího záznamu bez seznamu
 * médií, návrat na checkpoint při nahrávání), které jméno ze snapshotu
 * mechanice vrací. Pojistka je druhá obrana. Při neshodě se
 * nezapíše nic (uložení, reset, vysunutí, ukončení, "Save and switch"),
 * změny zůstanou v paměti s příznakem neuložených změn a uživatel dostane
 * zprávu; vysunutí nebo výměna obrazu pak tyto změny zahodí. Režim direct
 * zapisuje přes soubor otevřený při vložení a na `filename` už nesahá,
 * takže se ho pojistka netýká.
 *
 * License: GPLv3.
 */

#ifndef FDC_H
#define FDC_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/dsk/dsk.h"
#include "wd279x.h"
#include "fdc_fileid.h"

    /** Hodnota flagu `connected` - radič je připojený. */
#define FDC_CONNECTED 1
    /** Hodnota flagu `connected` - radič není připojený. */
#define FDC_DISCONNECTED 0

    /** Počet emulovaných mechanik (per instance). */
#define FDC_NUM_DRIVES 4

    /** Počet instancí FDC řadiče (FDC0 = standard, FDC1 = secondary). */
#define FDC_INSTANCE_COUNT 2

    /**
     * @brief Číslo mechaniky přes oba řadiče (0-3 = FDC0, 4-7 = FDC1), jako `--fdd<N>`.
     * @param fdc_index index řadiče (FDC0 / FDC1)
     * @param drive_id  index mechaniky (0..3)
     * @note Každý argument se vyhodnotí jednou; bez vedlejších efektů.
     */
#define FDC_GLOBAL_DRIVE_NO(fdc_index, drive_id) ((int)((fdc_index) * FDC_NUM_DRIVES + (drive_id)))

    /**
     * @brief Index instance FDC řadiče.
     *
     * Emulátor drží `FDC_INSTANCE_COUNT` nezávislých FDC řadičů v poli
     * `g_fdc`. FDC0 je primární WD279x na portech 0xD8h - 0xDFh, FDC1 je
     * sekundární (Unicard-suppressed) na portech 0x58h - 0x5Fh.
     *
     * @note Etapa 1: aktivní je pouze FDC0; FDC1 je inicializován jako
     *       inertní (disconnected) placeholder a zatím není napojen na
     *       IORQ, menu ani konfiguraci.
     */
    typedef enum en_FDC_INSTANCE
    {
        FDC0 = 0, /**< Primární WD279x, porty 0xD8h - 0xDFh. */
        FDC1 = 1  /**< Sekundární WD279x, porty 0x58h - 0x5Fh. */
    } en_FDC_INSTANCE;

    /**
     * @brief Polarita BUS translation vrstvy.
     *
     * Sharp default je INVERT - mezi CPU sběrnicí a chipem se XOR-uje 0xFF.
     * PASSTHROUGH je experimentální mód pro budoucí "true bus" ROM, kdy
     * chip vidí stejné bity jako CPU.
     */
    typedef enum en_FDC_BUS_XLATE
    {
        FDC_BUS_XLATE_INVERT      = 0, /**< Sharp default: XOR 0xFF na BUS <-> chip. */
        FDC_BUS_XLATE_PASSTHROUGH = 1  /**< Experimentální: žádná inverze (pro "true bus" ROM). */
    } en_FDC_BUS_XLATE;

    /**
     * @brief Storage mode per drive - jak je DSK obraz držen za běhu emu.
     *
     * Default je CACHED. DIRECT je pro power-users (sync s filesystem real-time).
     * DISCARD je pro testovací režim (změny zapomenuty při unmount/exit).
     */
    typedef enum en_FDC_STORAGE_MODE
    {
        FDC_STORAGE_CACHED  = 0, /**< DSK image v RAM, sync při reset/umount/exit/manual. */
        FDC_STORAGE_DIRECT  = 1, /**< Operace přímo na souboru přes file_driver (immediate writes). */
        FDC_STORAGE_DISCARD = 2  /**< DSK image v RAM, NIKDY se nesynchronizuje (R/W in-session). */
    } en_FDC_STORAGE_MODE;

    /**
     * @brief Stav jedné mechaniky (drive).
     *
     * Drží mount stav, cestu k DSK obrazu, handler (paměťový NEBO souborový
     * podle storage_mode) a načtenou geometrii. Po úspěšném mountu
     * platí (mounted == 1) && (handler_valid == 1).
     *
     * R/O state - čtyři příznaky:
     *  - user_readonly:   persistent user preference (cfgkey wd279x_fddX_readonly)
     *  - fs_readonly:     runtime auto-detected (FS write-protect attribute)
     *  - shared_readonly: ochrana kvůli sdílení - tentýž soubor byl při vložení
     *                     už v jiné mechanice (viz popis souboru); stav běhu,
     *                     do INI se neukládá
     *  - readonly:        effective R/O = user_readonly OR fs_readonly OR
     *                     shared_readonly (= co chip respektuje pro WRITE odmítnutí)
     *
     * Invarianty:
     *  - mounted == 1   <=> filename neprázdné a handler je v READY stavu.
     *    Načtení snapshotu vložená média nemění (snap_fdc.c). Výjimka: při
     *    načtení snapshotu záznamu vstupů a u obrazu nasazeného z paměti se
     *    `mounted` a `filename` berou ze snapshotu a obraz se nemění;
     *    `filename` pak nemusí označovat soubor, jehož obsah mechanika drží.
     *    Co mechanika skutečně drží, říkají `handler_valid`, `file_backed`
     *    a `fileid`.
     *  - handler_valid == 1 vyžaduje generic_driver_close() před dalším open.
     *  - readonly == 1 mapuje na HANDLER_STATUS_READ_ONLY na handleru.
     *  - storage_mode == DIRECT vyžaduje file_driver (= file handle, ne mem buffer).
     *  - file_backed == 1 jen u obrazu vloženého ze souboru (fdc_mount_dskfile());
     *    jen tehdy je `fileid` neprázdná a platí handler_valid == 1. Obraz
     *    z paměti má file_backed == 0.
     *  - shared_readonly == 1 vyžaduje file_backed == 1 a znamená
     *    readonly == 1.
     *  - Přes všechny mechaniky obou řadičů: mezi mechanikami se shodnou
     *    `fileid` má nejvýš jedna readonly == 0.
     *
     * Ownership:
     *  - Paměť bufferu (CACHED/DISCARD) nebo FILE* (DIRECT) vlastní generic_driver
     *    (uvolní ji generic_driver_close()).
     *  - Strukturu st_HANDLER vlastní st_FDDrive (alokována inline).
     *  - `fileid` vlastní st_FDDrive (uvolní ji odpojení obrazu).
     *
     * Vlákna: stav mění emu vlákno (nebo příkaz MCP) pod device_state_lock();
     * UI ho čte pod stejným zámkem.
     */
    typedef struct st_FDDrive
    {
        unsigned mounted;            /**< 1 = drive má úspěšně otevřený DSK obraz. */
        char filename[1024];         /**< Cesta k DSK souboru (full path); u záznamu vstupů může nést jméno ze snapshotu (viz invarianty). */
        st_HANDLER handler;          /**< Handler s obsahem DSK obrazu (paměťový nebo souborový). */
        int handler_valid;           /**< 1 = handler je v platném stavu (otevřený, je nutné jej zavřít). */
        int readonly;                /**< Effective R/O = user_readonly || fs_readonly || shared_readonly (obraz z paměti: hodnota z nasazení nebo ze snapshotu). Chip odmítá WRITE. */
        int user_readonly;           /**< Persistent user pref z cfgkey wd279x_fddX_readonly. */
        int fs_readonly;             /**< Runtime auto-detect z FS atributů (write-protect). */
        int shared_readonly;         /**< 1 = ochrana kvůli sdílení: soubor byl při vložení už v jiné mechanice. */
        int file_backed;             /**< 1 = obraz je vložený ze souboru `filename` (ne z paměti). */
        int share_notice;            /**< Nevyzvednuté upozornění na sdílení: 0 = žádné, jinak číslo druhé mechaniky (0-7) + 1; viz fdc_drive_take_share_notice(). */
        st_FDC_FILEID fileid;        /**< Identita souboru obrazu z okamžiku vložení (prázdná bez file_backed). */
        en_FDC_STORAGE_MODE storage_mode; /**< Cached / direct / discard. */
        st_DSK_GEOMETRY geometry;    /**< Geometrie DSK obrazu (tracks, sides, total_data_bytes...). */
        int geometry_valid;          /**< 1 = pole `geometry` obsahuje validní data načtená z DSK. */
        /* TODO (fáze C): timing model (motor spin-up, step rate,
         *                head settling). */
    } st_FDDrive;

    /**
     * @brief Stav jedné instance FDC řadiče.
     *
     * Sdružuje connected flag, HW konfiguraci (hd_patch / bus_xlate),
     * vlastní WD279x chip a 4 mechaniky. Emulátor drží
     * `FDC_INSTANCE_COUNT` těchto struktur v poli `g_fdc`.
     *
     * @invariant `index` je platný index do `g_fdc` (FDC0..FDC_INSTANCE_COUNT-1).
     * @invariant `connected` je vždy `FDC_CONNECTED` nebo
     *            `FDC_DISCONNECTED`.
     * @invariant `bus_xlate` je vždy `FDC_BUS_XLATE_INVERT` nebo
     *            `FDC_BUS_XLATE_PASSTHROUGH`.
     * @invariant `hd_patch` je 0 nebo 1.
     */
    typedef struct st_FDC
    {
        unsigned index;                 /**< Index instance v `g_fdc` (FDC0/FDC1). Nastavuje `fdc_init`. */
        unsigned connected;             /**< 0/1 - radič připojen na sběrnici. */
        int hd_patch;                   /**< 1 = HD Patch obvod (port 0xDF EINT). Typ `int` kvůli `cfgelement` BOOL bind (ukládá `int *`). */
        en_FDC_BUS_XLATE bus_xlate;     /**< Polarita BUS xlate vrstvy. */
        st_WD279X wd279x;               /**< Vrstva Chip. */
        st_FDDrive drive[FDC_NUM_DRIVES]; /**< Mechaniky 0..3. */
    } st_FDC;

    extern st_FDC g_fdc[FDC_INSTANCE_COUNT];

    /* Connected test/set makra.
     * TODO (Etapa 2): parametrizovat instancí FDC; nyní implicitně FDC0. */

    /** Test: FDC0 není připojený. */
#define FDC_TEST_NOT_CONNECTED (g_fdc[FDC0].connected != FDC_CONNECTED)
    /** Test: FDC0 (WD279x) je připojený. */
#define FDC_TEST_WD279X_CONNECTED (g_fdc[FDC0].connected == FDC_CONNECTED)

    /** Nastav FDC0 na "nepřipojeno". */
#define FDC_SET_NOT_CONNECTED() (g_fdc[FDC0].connected = FDC_DISCONNECTED)
    /** Nastav FDC0 na "WD279x připojen". */
#define FDC_SET_WD279X_CONNECTED() (g_fdc[FDC0].connected = FDC_CONNECTED)

    /**
     * @brief Test, zda je v dané mechanice nasazený DSK obraz.
     * @param fdc instance FDC řadiče
     * @param drive_id index mechaniky (0..3)
     */
#define FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id) (fdc_test_drive_id_mounted((fdc), (drive_id)))

    /**
     * @brief Inicializuje FDC subsystém.
     *
     * Volá se jednou při startu emulátoru. Registruje cfgfile sekci FDC,
     * vytvoří implementační stav, mountne disky podle konfigurace.
     */
    extern void fdc_init(void);

    /**
     * @brief Reset FDC (HW reset emulovaného systému).
     *
     * Před resetem chipu sync cached DSK obrazů na disk (user očekává,
     * že reset nezahodí už zapsaná data). HW reset nezavírá DSK obrazy -
     * mount zůstává.
     */
    extern void fdc_reset(void);

    /**
     * @brief Ukončí FDC subsystém - uzavře všechny otevřené DSK soubory.
     *
     * Volá se při shutdown emulátoru. Před close flushne cached změny
     * zpět do souboru. INI klíče `wd279x_fddX_dskpath` se NEČISTÍ -
     * persistovaný mount stav se má zachovat pro příští start.
     */
    extern void fdc_exit(void);

    /**
     * @brief Čte byte z FDC registru (port handler).
     *
     * BUS read handler dané instance (FDC0: porty 0xD8h-0xDFh, FDC1: porty
     * 0x58h-0x5Fh). Volá chip read a aplikuje inverze D0..D7 podle
     * `fdc->bus_xlate`.
     *
     * @param fdc instance FDC řadiče (NULL = no-op, vrátí 0)
     * @param i_addroffset offset registru (0..7)
     * @param io_data výstupní byte (po inverzi - hodnota na sběrnici)
     * @return 0 = OK
     */
    extern int fdc_read_byte(st_FDC *fdc, int i_addroffset, uint8_t *io_data);

    /**
     * @brief Zapíše byte do FDC registru (port handler).
     *
     * BUS write handler dané instance. Aplikuje inverze D0..D7 vstupních
     * dat (BUS -> chip) a předá true-bus hodnotu chipu.
     *
     * @param fdc instance FDC řadiče (NULL = no-op, vrátí 0)
     * @param i_addroffset offset registru (0..7)
     * @param io_data vstupní byte (na sběrnici, před inverzí)
     * @return 0 = OK
     */
    extern int fdc_write_byte(st_FDC *fdc, int i_addroffset, uint8_t *io_data);

    /**
     * @brief Vrátí aktuální stav /INT signálu z FDC.
     *
     * Agreguje stav všech instancí (sdílená /INT linka). Etapa 1: vrací
     * pouze stav FDC0.
     *
     * @return 0 = INT není aktivní, nenulová = INT aktivní
     */
    extern int fdc_get_interrupt_state(void);

    /**
     * @brief Spustí UI dialog pro výběr DSK souboru pro danou mechaniku.
     * @param fdc instance FDC řadiče (NULL = no-op)
     * @param drive_id index mechaniky (0..3)
     */
    extern void fdc_ui_mount(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Odmountuje DSK z dané mechaniky.
     *
     * Zavře paměťový/file handler, vynuluje filename, mounted/handler_valid/
     * geometry_valid flagy a vyčistí INI klíč `wd279x_fddX_dskpath` (umount
     * se má persistovat). Pokud mechanika nebyla mountnutá, je operace no-op.
     *
     * @param fdc instance FDC řadiče (NULL = no-op)
     * @param drive_id index mechaniky (0..3)
     */
    extern void fdc_umount(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Aktualizuje UI informaci o nasazeném DSK.
     *
     * Volá se z implementace po změně stavu mechaniky.
     *
     * @param drive_id index mechaniky (0..3)
     * @param dsk_filename jméno DSK (nebo prázdný řetězec při umount)
     */
    extern void ui_fdc_set_dsk(unsigned drive_id, char *dsk_filename);

    /**
     * @brief Mountne DSK soubor do dané mechaniky.
     *
     * Otevře DSK obraz - způsob závisí na storage_mode (cached/discard
     * -> memory driver, direct -> file driver). Načte geometrii a uloží
     * stav do `g_fdc[FDC0].drive[drive_id]`. Při remount nejdřív zavře předchozí
     * mount. Při neúspěchu open zůstává mechanika v umounted stavu.
     *
     * Úspěšné vložení zapíše cestu do historie naposledy vložených médií
     * (recent_media_add(RECENT_MEDIA_DSK)); nezapisuje se obnova vložení
     * z INI při startu (fdc_init()) a nové otevření obrazu, který v mechanice
     * už je (změna volby read-only nebo režimu úložiště).
     *
     * Pokud filename[0] == 0, provede umount (ekvivalent fdc_umount).
     *
     * Stejný obraz ve více mechanikách (viz popis souboru): drží-li tentýž
     * soubor už jiná mechanika, vloží se obraz s ochranou kvůli sdílení
     * (`shared_readonly`), na stderr se vypíše záznam a mechanika si zapamatuje
     * upozornění pro volajícího (fdc_drive_take_share_notice()); okno se
     * zprávou tato funkce neotevírá. Nové otevření obrazu, který v mechanice
     * už je, ochranu kvůli sdílení zachová (a vynutí ji, kdyby tentýž soubor
     * mezitím držela se zápisem jiná mechanika).
     *
     * @param fdc instance FDC řadiče (NULL = no-op)
     * @param drive_id index mechaniky (0..3)
     * @param filename cesta k DSK souboru (prázdný řetězec = umount)
     * @pre Emu vlákno nebo pozastavená emulace; volající drží
     *      device_state_lock(), pokud běží UI.
     * @post Platí invariant jediné mechaniky se zápisem na soubor.
     */
    extern void fdc_mount_dskfile(st_FDC *fdc, unsigned drive_id, char *filename);

    /**
     * @brief Kopie obsahu nasazeného DSK obrazu (záznam vstupů).
     *
     * U režimů cached a discard je to obsah paměťového obrazu (včetně změn
     * dosud nezapsaných do souboru), u direct obsah souboru (po fflush).
     *
     * @param fdc      instance FDC řadiče
     * @param drive_id index mechaniky (0..3)
     * @param[out] size velikost kopie v bajtech
     * @return nová kopie (g_free), nebo NULL (mechanika bez obrazu, chyba čtení)
     * @pre Emu vlákno nebo pozastavená emulace.
     */
    extern uint8_t *fdc_drive_copy_image(st_FDC *fdc, unsigned drive_id, size_t *size);

    /**
     * @brief Nasadí DSK obraz z paměti bez zápisu do INI (přehrání vstupů).
     *
     * Předchozí obraz se zavře jako při běžném odpojení (cached se zapíše do
     * souboru). Nový obraz je paměťová kopie @p data v režimu discard: zápisy
     * emulovaného programu se do žádného souboru nedostanou. Hlavička DSK se
     * neopravuje (obraz je přesně ten ze záznamu). INI klíče mechaniky se
     * nemění, UI dostane @p label.
     *
     * @param fdc      instance FDC řadiče
     * @param drive_id index mechaniky (0..3)
     * @param data     obsah obrazu (zkopíruje se)
     * @param size     velikost v bajtech (> 0)
     * @param readonly 1 = jen pro čtení (WRITE odmítne)
     * @param label    jméno pro `filename` a UI (např. jméno původního souboru)
     * @return EXIT_SUCCESS; EXIT_FAILURE (alokace, neplatná geometrie) -
     *         mechanika pak zůstane bez obrazu
     * @pre Emu vlákno nebo pozastavená emulace.
     */
    extern int fdc_mount_dsk_buffer(st_FDC *fdc, unsigned drive_id, const uint8_t *data, size_t size,
                                    int readonly, const char *label);

    /**
     * @brief Odpojí obraz bez zápisu do INI (přehrání vstupů).
     *
     * Jako fdc_umount() (cached se zapíše do souboru), ale INI klíč cesty
     * zůstane, takže obraz jde později vrátit přes fdc_mount_dskfile().
     *
     * @param fdc      instance FDC řadiče
     * @param drive_id index mechaniky (0..3)
     */
    extern void fdc_umount_keep_cfg(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Vrátí cestu k aktuálně mountnutému DSK pro danou mechaniku.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí NULL)
     * @param drive_id index mechaniky (0..3)
     * @return ukazatel na vnitřní buffer, nebo NULL pokud drive_id > 3
     *
     * @note Vrácený ukazatel je platný pouze do dalšího volání
     *       `fdc_mount_dskfile` / `fdc_umount` pro stejnou mechaniku.
     */
    extern const char *fdc_get_dsk_filepath(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Test "mountnutí" disku v mechanice.
     *
     * Implementace makra `FDC_TEST_DRIVE_ID_MOUNTED`.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 0 = prázdná, nenulová = disk je nasazen
     */
    extern int fdc_test_drive_id_mounted(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Vrátí název cfgfile klíče DSK cesty pro danou mechaniku.
     *
     * Klíče FDC0: `wd279x_fdd0_dskpath` .. `wd279x_fdd3_dskpath`.
     * Klíče FDC1: `fdc1_fdd0_dskpath` .. `fdc1_fdd3_dskpath`. Statický
     * buffer per (instance, drive_id).
     *
     * @param fdc instance FDC řadiče (NULL = vrátí NULL)
     * @param drive_id index mechaniky (0..3)
     * @return ukazatel na C-string s názvem klíče, nebo NULL pokud
     *         drive_id mimo rozsah.
     */
    extern const char *fdc_dskpath_keyname(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Vrátí název cfgfile sekce pro danou instanci FDC.
     *
     * FDC0 používá historickou sekci "FDC", FDC1 sekci "FDC1".
     *
     * @param fdc instance FDC řadiče (NULL = vrátí "FDC")
     * @return statický C-string s názvem sekce, nikdy NULL.
     */
    extern const char *fdc_cfg_section_name(const st_FDC *fdc);

    /**
     * @brief Aktualizuje cfgfile element DSK cesty pro danou mechaniku.
     *
     * Volá se po mount/umount, aby cesta perzistovala do INI při exitu.
     *
     * @param fdc instance FDC řadiče (NULL = no-op)
     * @param drive_id index mechaniky (0..3)
     * @param value    C-string s cestou (NULL nebo prázdný = umounted).
     */
    extern void fdc_cfg_set_dskpath(st_FDC *fdc, unsigned drive_id, const char *value);

    /**
     * @brief Vrátí název cfgfile klíče "readonly" pro mechaniku.
     *
     * Klíče FDC0: `wd279x_fdd0_readonly` .. `wd279x_fdd3_readonly` (BOOL).
     * Klíče FDC1: `fdc1_fdd0_readonly` .. `fdc1_fdd3_readonly` (BOOL).
     * Persistent user preference - když ON, chip odmítá WRITE bez ohledu
     * na FS atributy souboru.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí NULL)
     * @param drive_id index mechaniky (0..3)
     * @return ukazatel na C-string s názvem klíče, nebo NULL pokud mimo rozsah.
     */
    extern const char *fdc_readonly_keyname(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Vrátí aktuální hodnotu "readonly" cfgfile klíče.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 1 = user chce R/O, 0 = RW (default).
     */
    extern int fdc_cfg_get_readonly(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Aktualizuje "readonly" cfgfile klíč (= persist user toggle).
     *
     * @param fdc instance FDC řadiče (NULL = no-op)
     * @param drive_id index mechaniky (0..3)
     * @param value 0/1
     */
    extern void fdc_cfg_set_readonly(st_FDC *fdc, unsigned drive_id, int value);

    /**
     * @brief Vrátí název cfgfile klíče "storage_mode" pro mechaniku.
     *
     * Klíče FDC0: `wd279x_fdd0_storage_mode` .. `wd279x_fdd3_storage_mode` (TEXT).
     * Klíče FDC1: `fdc1_fdd0_storage_mode` .. `fdc1_fdd3_storage_mode` (TEXT).
     * Hodnoty: "cached" | "direct" | "discard". Default "cached".
     *
     * @param fdc instance FDC řadiče (NULL = vrátí NULL)
     * @param drive_id index mechaniky (0..3)
     */
    extern const char *fdc_storage_mode_keyname(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Vrátí aktuální hodnotu "storage_mode" cfgfile klíče jako TEXT.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí "cached")
     * @param drive_id index mechaniky (0..3)
     * @return Statický TEXT string, nikdy NULL (default "cached").
     */
    extern const char *fdc_cfg_get_storage_mode(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Aktualizuje "storage_mode" cfgfile klíč.
     *
     * @param fdc instance FDC řadiče (NULL = no-op)
     * @param drive_id index mechaniky (0..3)
     * @param value TEXT hodnota ("cached" | "direct" | "discard")
     */
    extern void fdc_cfg_set_storage_mode(st_FDC *fdc, unsigned drive_id, const char *value);

    /**
     * @brief Vyžádá sync paměťového DSK obrazu do souboru pro danou mechaniku.
     *
     * Volá se z UI ("Sync now"), z reset cesty a z lifecycle exit. Funguje
     * pouze pro storage_mode == CACHED s memspec.updated == 1. Pro DIRECT
     * mode no-op (writes jdou rovnou přes file driver). Pro DISCARD mode
     * no-op (změny se zahodí).
     *
     * Zapisuje jen do souboru, ze kterého byl obraz načten (viz popis
     * souboru): když `filename` mechaniky označuje jiný soubor (po načtení
     * snapshotu záznamu vstupů), nezapíše nic, změny nechá v paměti, ohlásí to zprávou
     * uživateli a vrátí 0.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 1 = OK (nebo no-op), 0 = chyba zápisu nebo zápis odmítnutý
     *         pojistkou cílového souboru.
     */
    extern int fdc_sync_drive(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Test: má drive nesynchronizované změny (kandidát na "Sync now").
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 1 = ano (relevantní pro UI sync-now action), 0 = ne.
     */
    extern int fdc_drive_has_unsaved_changes(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Test: je DSK soubor FS-write-protected.
     *
     * Použito pro UI: pokud true, R/O checkbox je disabled + zobrazí
     * že drive je R/O auto-detect, bez modifikace persistent user pref.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 1 = ano, 0 = ne.
     */
    extern int fdc_drive_fs_readonly(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Test: má drive RAM změny (= bypass storage_mode gate).
     *
     * Vrátí 1 i pro Discard mode pokud byly zapsány bajty. Použito
     * pro UI pre-switch dialog (Discard -> Cached/Direct s pending changes).
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 1 = ano, 0 = ne.
     */
    extern int fdc_drive_has_ram_changes(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Vynucený zápis RAM bufferu do DSK souboru (bypass mode gate).
     *
     * Pro UI "Save and switch" v dialog při přepnutí z Discard módu.
     * Mechanika s ochranou kvůli sdílení (`shared_readonly`) do souboru
     * nezapíše nikdy: funkce vrátí 0. Nezapíše ani tehdy, když tentýž
     * soubor drží se zápisem jiná mechanika (fdc_drive_write_blocker();
     * se zprávou uživateli) - týká se mechaniky chráněné volbou uživatele
     * nebo právy souboru. Stejně tak nezapíše obraz nasazený
     * z paměti (soubor nedrží) a obraz, jehož `filename` označuje jiný
     * soubor, než ze kterého byl načten (viz popis souboru; se zprávou
     * uživateli). Příznak změn v paměti při odmítnutí zůstává.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 1 = OK, 0 = chyba nebo odmítnuto.
     */
    extern int fdc_drive_force_save_to_file(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Test: má mechanika ochranu proti zápisu kvůli sdílení obrazu.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí 0)
     * @param drive_id index mechaniky (0..3)
     * @return 1 = ano (`shared_readonly`), 0 = ne nebo prázdná mechanika.
     * @pre Volající drží device_state_lock(), pokud není emu vlákno.
     */
    extern int fdc_drive_shared_readonly(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Mechanika, se kterou tato sdílí soubor obrazu (pro poznámku v UI).
     *
     * Hledá jen pro mechaniku s ochranou kvůli sdílení. Drží-li soubor víc
     * jiných mechanik, vrátí tu se zápisem, jinak první v pořadí.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí -1)
     * @param drive_id index mechaniky (0..3)
     * @return Číslo mechaniky přes oba řadiče (FDC_GLOBAL_DRIVE_NO(), 0-7),
     *         nebo -1: mechanika ochranu kvůli sdílení nemá, nebo soubor už
     *         žádná jiná mechanika nedrží.
     * @pre Volající drží device_state_lock(), pokud není emu vlákno.
     * @note Porovnává uložené identity, na souborový systém nesahá.
     */
    extern int fdc_drive_shared_with(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Mechanika, která brání sundání ochrany proti zápisu.
     *
     * @param fdc instance FDC řadiče (NULL = vrátí -1)
     * @param drive_id index mechaniky (0..3)
     * @return Číslo jiné mechaniky přes oba řadiče (0-7), která drží tentýž
     *         soubor se zápisem (readonly == 0); -1 = žádná taková není
     *         nebo mechanika nemá obraz ze souboru.
     * @pre Volající drží device_state_lock(), pokud není emu vlákno.
     * @note Porovnává uložené identity, na souborový systém nesahá.
     */
    extern int fdc_drive_write_blocker(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Nastaví ochranu mechaniky proti zápisu (volba uživatele) a nasazený obraz znovu otevře.
     *
     * Jediná cesta, kterou se ochrana mění za běhu (menu, overlay; zásah
     * fdc_request_set_readonly()). Zapnutí: zapíše volbu do INI klíče
     * `<prefix>_fddX_readonly`. Vypnutí: zapíše volbu a zruší i ochranu kvůli
     * sdílení - pokud ale tentýž soubor drží jiná mechanika se zápisem,
     * nezmění nic a vrátí chybu. Prázdné mechanice se jen zapíše volba.
     *
     * @param fdc instance FDC řadiče
     * @param drive_id index mechaniky (0..3)
     * @param readonly 0 = povolit zápis, jinak chránit
     * @param[out] blocker při návratu -1 číslo mechaniky (0-7), která drží
     *             soubor se zápisem; jinak -1. Smí být NULL.
     * @return 0 = provedeno; -1 = odmítnuto kvůli jiné mechanice se zápisem;
     *         -2 = neplatný argument.
     * @pre Emu vlákno nebo pozastavená emulace; volající drží
     *      device_state_lock(), pokud běží UI.
     * @post Při 0 platí invariant jediné mechaniky se zápisem; cached změny
     *       obrazu se před novým otevřením zapsaly jako při odpojení.
     */
    extern int fdc_drive_set_write_protect(st_FDC *fdc, unsigned drive_id, int readonly, int *blocker);

    /**
     * @brief Vyzvedne upozornění, že obraz byl vložen s ochranou kvůli sdílení.
     *
     * Upozornění vznikne ve fdc_mount_dskfile() a platí do vyzvednutí nebo
     * do odpojení obrazu. Volá ten, kdo vložení vyvolal a umí ho ohlásit
     * (zásah z GUI oknem se zprávou, MCP polem `warning`).
     *
     * @param fdc instance FDC řadiče (NULL = vrátí -1)
     * @param drive_id index mechaniky (0..3)
     * @return Číslo mechaniky (0-7), která soubor už držela; -1 = žádné
     *         upozornění.
     * @post Upozornění mechaniky je smazané.
     * @pre Emu vlákno nebo pozastavená emulace.
     */
    extern int fdc_drive_take_share_notice(st_FDC *fdc, unsigned drive_id);

    /**
     * @brief Obnoví invariant jediné mechaniky se zápisem po zásahu, který přepsal pole `readonly`.
     *
     * Pojistka po načtení snapshotu. To vložená média ani pole `readonly`
     * mechaniky s obrazem ze souboru nemění (snap_fdc.c),
     * takže funkce za běžných okolností nic nezmění. Kdyby invariant přesto
     * neplatil: mechanice s ochranou kvůli sdílení vrátí readonly = 1;
     * kdyby tentýž soubor vycházel se zápisem ve více mechanikách, nechá
     * zápis první v pořadí (FDC0 FD0..FD3, FDC1 FD0..FD3) a ostatním dá
     * ochranu kvůli sdílení. Mechanik bez sdíleného souboru se netýká.
     *
     * @pre Emu vlákno nebo pozastavená emulace.
     */
    extern void fdc_enforce_single_writer(void);

#ifdef __cplusplus
}
#endif

#endif /* FDC_H */
