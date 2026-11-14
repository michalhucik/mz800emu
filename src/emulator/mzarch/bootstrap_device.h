/**
 * @file bootstrap_device.h
 * @brief Kód zařízení v registru BC při přímém spuštění MZF (bootstrap `--run-mzf`).
 *
 * Program zavedený z média dostane od ROM v registru BC kód zařízení, ze
 * kterého byl nahrán (báze mz800-knowledge
 * `public/reference/agent/conventions/memory-layout-user-prog.md`, oddíl
 * "BC kódy zařízení"); některé programy podle něj volí výchozí zařízení
 * (pozorováno u BASICu 1Z-016). Přímé spuštění MZF obchází ROM
 * (mzarch_bootstrap_run_mzf()), proto hodnotu BC určuje tento modul:
 *
 *   - jedno nastavení `[CMT] run_mzf_device` (INI, MCP `settings_set`,
 *     menu): jméno zařízení `rd` / `sram`, `cmt`, `fd`, `qd` (velikost
 *     písmen nehraje roli) nebo číslo 0..65535 desítkově bez úvodní nuly
 *     (`512`; samotná `0` platí, `0200` je chyba BOOTDEV_PARSE_LEADING_ZERO)
 *     nebo šestnáctkově (`0x0200`, `0200h`); výchozí `cmt`,
 *   - jednorázová volba pro jedno spuštění (CLI `--run-mzf-device`, MCP
 *     `media_run_mzf` s parametrem `device`), která nastavení nemění.
 *
 * Tabulka jméno -> hodnota je jen tady (bootdev_name_bc()). Modul je bez
 * lokalizačních maker (jádro). Implementace: bootstrap_device.c (zápis,
 * tabulka, nastavení; jen GLib, linkuje se i do samostatných testů) a
 * bootstrap_device_cfg.c (platforma buildu MZARCH, klíč INI).
 *
 * Vlákna: nastavení chrání vnitřní zámek (GMutex), čtou a mění ho UI
 * vlákno (menu), emu vlákno (bootstrap, MCP přes frontu dbgapi) i uložení
 * INI. Čisté funkce (parse, tabulka, formátování) jsou bez stavu.
 */

#ifndef BOOTSTRAP_DEVICE_H
#define BOOTSTRAP_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief Sekce INI nastavení (sdílí ji s volbami kazety [CMT]). */
#define BOOTDEV_CFG_MODULE "CMT"

    /** @brief Klíč INI nastavení v sekci BOOTDEV_CFG_MODULE. */
#define BOOTDEV_CFG_ELEMENT "run_mzf_device"

    /** @brief Výchozí hodnota nastavení (kazeta, jako ROM po zavedení z pásky). */
#define BOOTDEV_DEFAULT_TEXT "cmt"

    /**
     * @brief Největší délka textu nastavení včetně koncové nuly.
     *
     * Delší vstup bootdev_parse() odmítne; nejdelší platné zápisy jsou
     * kratší (`0x0000FFFF` s mezerami okolo se ořízne).
     */
#define BOOTDEV_TEXT_MAX 32

    /**
     * @brief Zařízení podle jména (řádky tabulky bootdev_name_bc()).
     */
    typedef enum en_BOOTDEV_NAME
    {
        BOOTDEV_NAME_RD = 0, /**< `rd` (alias `sram`): RD:, sériová paměť SRAM, kód 0000h. */
        BOOTDEV_NAME_CMT,    /**< `cmt`: kazeta, kód podle ROM platformy (MZ-700 0000h, jinak 0100h). */
        BOOTDEV_NAME_FD,     /**< `fd`: disketa, kód 0200h. */
        BOOTDEV_NAME_QD,     /**< `qd`: Quick Disk, kód 0300h. */
        BOOTDEV_NAME_COUNT   /**< Počet jmen (není to jméno). */
    } en_BOOTDEV_NAME;

    /**
     * @brief Rozebraná volba zařízení: jméno, nebo přímo zadaná hodnota BC.
     *
     * @invariant custom == false => name < BOOTDEV_NAME_COUNT (value se
     *            nepoužívá); custom == true => value je zadaná hodnota BC
     *            (name se nepoužívá).
     */
    typedef struct st_BOOTDEV_CHOICE
    {
        bool custom;          /**< true = číslo, false = jméno zařízení. */
        en_BOOTDEV_NAME name; /**< Jméno (platí při custom == false). */
        uint16_t value;       /**< Hodnota BC (platí při custom == true). */
    } st_BOOTDEV_CHOICE;

    /**
     * @brief Výsledek rozboru zápisu volby zařízení (bootdev_parse_ex()).
     */
    typedef enum en_BOOTDEV_PARSE
    {
        BOOTDEV_PARSE_OK = 0,       /**< Platný zápis. */
        BOOTDEV_PARSE_INVALID,      /**< Neplatný zápis (jméno, znaky, rozsah, délka, prázdný). */
        BOOTDEV_PARSE_LEADING_ZERO, /**< Desítkové číslo s úvodní nulou (`0200`): odmítnuto, protože
                                         se snadno splete se šestnáctkovým zápisem kódu (`0200h`). */
    } en_BOOTDEV_PARSE;

    /**
     * @brief Rozebere zápis volby zařízení a vrátí důvod odmítnutí.
     *
     * Přijímá (bílé znaky na začátku a na konci se ignorují):
     *   - jméno `rd`, `sram`, `cmt`, `fd`, `qd` bez ohledu na velikost písmen,
     *   - desítkové číslo z číslic 0-9 bez úvodní nuly (`512`; samotná `0`
     *     platí),
     *   - šestnáctkové číslo s předponou `0x` / `0X` (`0x0200`),
     *   - šestnáctkové číslo s příponou `h` / `H`, které začíná číslicí
     *     (`0200h`, `0FFh`) - stejné zápisy jako adresy v debuggeru
     *     (watch_parse_address(), bez symbolů).
     *
     * Hodnota musí ležet v 0..65535. Znaménko, prázdný text, jiné znaky,
     * text delší než BOOTDEV_TEXT_MAX - 1 a přetečení jsou BOOTDEV_PARSE_INVALID.
     * Desítkové číslo s úvodní nulou (`0200`, `00`) je
     * BOOTDEV_PARSE_LEADING_ZERO: kódy zařízení se píší šestnáctkově
     * (`0200h`) a `0200` by se tiše přečetlo jako 200 = 00C8h.
     *
     * @param text Vstup (NULL = BOOTDEV_PARSE_INVALID).
     * @param[out] out Výsledek; zapíše se jen při úspěchu (NULL = jen ověření).
     * @return Výsledek rozboru; při chybě out beze změny.
     *
     * Bez vedlejších efektů, bez stavu (lze volat z libovolného vlákna).
     */
    en_BOOTDEV_PARSE bootdev_parse_ex(const char *text, st_BOOTDEV_CHOICE *out);

    /**
     * @brief Rozebere zápis volby zařízení (bootdev_parse_ex() bez důvodu).
     * @param text Vstup. @param[out] out Výsledek (jen při úspěchu, smí být NULL).
     * @return true = platný zápis (BOOTDEV_PARSE_OK).
     */
    bool bootdev_parse(const char *text, st_BOOTDEV_CHOICE *out);

    /**
     * @brief Anglický popis chyby zápisu pro výpisy jádra (CLI, MCP, stderr).
     *
     * Jádro je bez lokalizace; GUI má vlastní přeložené texty.
     *
     * @param r Výsledek bootdev_parse_ex().
     * @return Statický řetězec bez tečky na konci; pro BOOTDEV_PARSE_OK "".
     */
    const char *bootdev_parse_message(en_BOOTDEV_PARSE r);

    /**
     * @brief Hodnota BC pro jméno zařízení na dané platformě.
     *
     * Jediná tabulka modulu. Zdroje:
     *   - RD 0000h, FD 0200h, QD 0300h: báze
     *     `conventions/memory-layout-user-prog.md`, "BC kódy zařízení"
     *     (tabulka platí pro ROM, která BC nastavuje, tj. IPL MZ-800
     *     a MZ-1500); na MZ-700 se použijí tytéž hodnoty (monitor 1Z-013A
     *     kód zařízení nenastavuje, zavádí jen z kazety; že programy MZ-700
     *     tyto kódy znají, je `[neověřeno]`),
     *   - CMT na MZ-800 0100h: IPL ]GOCMT E99Dh `LD BC,0100h` (bajty ROM,
     *     mz800/mz800_bootstrap.c) a báze `firmware/stav-po-zavedeni-z-cmt.md`,
     *   - CMT na MZ-1500 0100h: IPL E9E6h `LD BC,0100h` (bajty ROM,
     *     mz1500/mz1500_bootstrap.c) a táž báze,
     *   - CMT na MZ-700 0000h: stav po příkazu L monitoru 1Z-013A (báze
     *     `firmware/stav-po-zavedeni-z-cmt.md`, řádek "BC (kód zařízení)",
     *     měřeno v emulátoru se skutečnou ROM; běh s BASICem 1Z-013B: BC
     *     0000h po `L` se signálem i s Direct). Příkaz L (0111h-012Eh v bajtech
     *     ROM_MZ700_13A_0000.c) BC sám nenačítá a RHEAD / RDATA BC uchovají
     *     (PUSH / POP), hodnotu 0 tedy nechávají předchozí rutiny monitoru;
     *     která z nich, je `[neověřeno]`.
     *
     * @param name Jméno zařízení (mimo rozsah = hodnota pro CMT).
     * @param platform Platforma 700, 800 nebo 1500 (jiná = jako 800).
     * @return Hodnota BC.
     */
    uint16_t bootdev_name_bc(en_BOOTDEV_NAME name, unsigned platform);

    /**
     * @brief Hodnota BC pro rozebranou volbu na dané platformě.
     * @param choice Volba (NULL = výchozí CMT).
     * @param platform Platforma 700, 800 nebo 1500.
     * @return choice->value u vlastní hodnoty, jinak bootdev_name_bc().
     */
    uint16_t bootdev_choice_bc(const st_BOOTDEV_CHOICE *choice, unsigned platform);

    /**
     * @brief Kanonické jméno zařízení pro výpisy a INI (`rd`, `cmt`, `fd`, `qd`).
     * @param name Jméno (mimo rozsah = "cmt").
     * @return Statický řetězec.
     */
    const char *bootdev_name_text(en_BOOTDEV_NAME name);

    /**
     * @brief Text volby, jak se uloží do INI: jméno malými písmeny, číslo, jak bylo zadané (oříznuté).
     *
     * @param text Vstup (musí projít bootdev_parse()).
     * @param[out] out Buffer výsledku.
     * @param out_size Velikost bufferu (aspoň BOOTDEV_TEXT_MAX).
     * @return true = zapsáno; false = neplatný vstup nebo malý buffer (out
     *         beze změny).
     *
     * Jméno zůstane tak, jak ho uživatel napsal (`sram` zůstane `sram`), jen
     * malými písmeny; číslo si ponechá zápis (`0x0200` zůstane `0x0200`).
     */
    bool bootdev_normalize(const char *text, char *out, size_t out_size);

    /**
     * @brief Zaregistruje klíč nastavení do modulu INI sekce [CMT].
     *
     * Element typu TEXT s výchozí hodnotou BOOTDEV_DEFAULT_TEXT a callbacky
     * propagate (INI -> nastavení; neplatná hodnota = výchozí s varováním
     * na stderr) a save (nastavení -> INI).
     *
     * @param cmod_void Ukazatel na CFGMOD sekce [CMT] (void kvůli hlavičkám).
     * @pre Volá cmt_init() před cfgmodule_parse() své sekce, jednou za běh.
     * @post Klíč existuje; hodnotu z INI nastaví až cfgmodule_propagate().
     */
    void bootdev_cfg_register(void *cmod_void);

    /**
     * @brief Je "modul/element" klíčem tohoto nastavení?
     * @param module Jméno sekce INI. @param element Jméno klíče.
     * @return true pro BOOTDEV_CFG_MODULE / BOOTDEV_CFG_ELEMENT.
     */
    bool bootdev_cfg_is_key(const char *module, const char *element);

    /**
     * @brief Nastaví volbu zařízení (menu, MCP settings_set, INI).
     *
     * @param text Nový zápis (viz bootdev_parse()).
     * @return true = uloženo (v normalizovaném tvaru bootdev_normalize()),
     *         false = neplatný zápis, nastavení beze změny.
     *
     * Vlákna: libovolné (vnitřní zámek). Vedlejší efekty: změní hodnotu, se
     * kterou příští přímé spuštění MZF nastaví BC; INI se zapíše až při
     * ukládání konfigurace (save callback).
     */
    bool bootdev_setting_set_text(const char *text);

    /**
     * @brief Zkopíruje aktuální text nastavení.
     * @param[out] out Buffer (ukončený nulou, oříznutý na out_size).
     * @param out_size Velikost bufferu (0 = nic se nezapíše).
     *
     * Vlákna: libovolné (vnitřní zámek).
     */
    void bootdev_setting_get_text(char *out, size_t out_size);

    /**
     * @brief Hodnota BC podle nastavení pro platformu tohoto buildu (MZARCH).
     * @return Hodnota BC.
     *
     * Vlákna: libovolné (vnitřní zámek).
     */
    uint16_t bootdev_setting_bc(void);

    /**
     * @brief Hodnota BC pro platformu tohoto buildu (MZARCH) z rozebrané volby.
     * @param choice Volba (NULL = výchozí CMT).
     * @return Hodnota BC.
     */
    uint16_t bootdev_choice_bc_here(const st_BOOTDEV_CHOICE *choice);

#ifdef __cplusplus
}
#endif

#endif /* BOOTSTRAP_DEVICE_H */
