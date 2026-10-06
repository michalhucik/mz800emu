/**
 * @file fdc.c
 * @brief FDC subsystém - jediná implementace pro Sharp MZ-800/MZ-700/MZ-1500.
 *
 * Pokrývá vrstvy:
 *  - Sharp BUS (porty 0xD8-0xDF, IORQ handler)
 *  - Translation (inverze D0..D7 mezi BUS a chipem)
 *  - Drives (mechaniky 0..3, DSK image lifecycle)
 *  - cfgfile a CLI integrace ([FDC] sekce)
 *
 * Vrstvu Chip (vlastní WD279x logika) implementuje wd279x.c.
 *
 * Historicky existoval runtime dispatcher mezi dvěma implementacemi
 * (_old_ a _new_). OLD byla po stabilizaci NEW odstraněna a tenká
 * dispatcher vrstva byla nakonec rozpuštěna - implementace z fdc_new.c
 * se sloučila přímo sem.
 *
 * ## Konfigurační volby
 *
 *  - `[FDC] hd_patch=on|off` - HD Patch obvod (port 0xDF EINT logika).
 *    Default `off`. CLI override: `--fdc-hd-patch=on|off`.
 *  - `[FDC] bus_xlate=invert|passthrough` - polarita BUS translation
 *    vrstvy. Default `invert` (= Sharp default). `passthrough` je
 *    experimentální mód pro budoucí "true bus" ROM, kdy chip vidí
 *    stejné bity jako CPU. CLI override: `--fdc-bus-xlate=...`.
 *
 * ## Inverze konvence (per-port pravidlo)
 *
 * Sharp HW invertuje datovou sběrnici (D0..D7) **pouze** mezi CPU a
 * MB8876A (alias WD279x) chipem. Inverze je fyzicky umístěna mezi
 * chipem a sběrnicí, takže externí Sharp logic porty (MOTOR, SIDE,
 * DENSITY, EINT) za invertorem **nejsou** - prochází bez inverze.
 *
 * Pravidlo pro BUS xlate vrstvu (při `bus_xlate == INVERT`):
 *
 *  - Offsety 0..3 (porty 0xD8h - 0xDBh = CMDSTS/TRACK/SECTOR/DATA):
 *    inverze VŽDY v obou směrech (READ i WRITE).
 *  - Offsety 4..7 (porty 0xDCh - 0xDFh = MOTOR/SIDE/DENSITY/EINT):
 *    BYPASS - žádná inverze.
 *
 * Důsledek pro chip dispatch: pracuje s "true-bus" datasheet hodnotami
 * (RESTORE = 0x00, READ SECTOR = 0x80, atd.). Toto byla odlišnost vs.
 * _old_ impl, která měla inverzi per-register uvnitř chipu (CMDSTS port
 * byl "half-inverted" - Command write bez inverze, Status read s inverzí).
 * Současná varianta je ekvivalentní, jen symetrická vůči datasheetu.
 *
 * Reference: mz800-knowledge/public/reference/agent/hw/16-floppy.md
 * (Sharp invertuje data + side).
 *
 * License: GPLv3.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>             /* W_OK pro g_access() (Linux: glib/gstdio.h nezahrnuje tranzitivně) */

#include <glib.h>
#include <glib/gstdio.h>

#include "fdc.h"
#include "wd279x.h"
#include "baseui/baseui.h"
#include "baseui/baseui_filechooser.h"
#include "cfgmain.h"
#include "emulator/emulator_measuring.h"
#include "emulator/mzarch/interrupt.h"
#include "generic_driver/memory_driver.h"
#include "generic_driver/file_driver.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "libs/dsk/dsk.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "i18n.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#endif

/**
 * @brief Stav všech instancí FDC subsystému.
 *
 * Pole `FDC_INSTANCE_COUNT` řadičů (FDC0 = primární, FDC1 = sekundární).
 * Default `bus_xlate` je `INVERT` (Sharp), `hd_patch` je 1 (on) -
 * typický stav reálné Sharp MZ-800 sestavy s instalovaným HD Patch
 * obvodem (= port 0xDF EINT logika připojená). User který používá
 * raw / unpatched FDC vypne v menu Devices -> FDC -> "HD Patch enabled".
 * Pole FDC0 se přepíše cfgfile + CLI v `fdc_init`. Zbývající fields
 * (wd279x, drive[]) se nulují v `fdc_init` přes memset.
 *
 * @note Etapa 1: aktivní je pouze FDC0; FDC1 je inertní placeholder
 *       (disconnected) - zatím bez napojení na IORQ/menu/konfiguraci.
 */
st_FDC g_fdc[FDC_INSTANCE_COUNT] = {
    [FDC0] = {
        .index = FDC0,
        .connected = FDC_DISCONNECTED,
        .hd_patch = 1,
        .bus_xlate = FDC_BUS_XLATE_INVERT
    },
    [FDC1] = {
        .index = FDC1,
        .connected = FDC_DISCONNECTED,
        .hd_patch = 1,
        .bus_xlate = FDC_BUS_XLATE_INVERT
    }
};

/* Forward declarations: pomocné funkce implementované v Drives / Mount
 * sekci níže. */
static void fdc_drive_close(st_FDC *fdc, unsigned drive_id);
static int fdc_drive_flush_to_file(st_FDDrive *drv);

/**
 * @brief Vrátí název cfgfile sekce pro danou instanci FDC.
 *
 * FDC0 používá historickou sekci "FDC" (beze změny kvůli zpětné
 * kompatibilitě INI). FDC1 používá novou sekci "FDC1".
 *
 * @param fdc instance FDC řadiče (NULL = vrátí "FDC").
 * @return statický C-string s názvem sekce, nikdy NULL.
 */
const char *fdc_cfg_section_name(const st_FDC *fdc)
{
    if (fdc && fdc->index == FDC1)
        return "FDC1";
    return "FDC";
}

/* ------------------------------------------------------------------ */
/* cfgfile registrace a CLI override                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Parsuje řetězec bus_xlate ("invert"/"passthrough") na enum.
 *
 * Case-insensitive. Při neznámé hodnotě vrátí INVERT.
 *
 * @param s vstupní řetězec (NULL = vrátí INVERT bez varování)
 * @return en_FDC_BUS_XLATE hodnota
 */
static en_FDC_BUS_XLATE fdc_parse_bus_xlate_string(const char *s)
{
    if (!s)
        return FDC_BUS_XLATE_INVERT;
    if (strcasecmp(s, "passthrough") == 0)
        return FDC_BUS_XLATE_PASSTHROUGH;
    if (strcasecmp(s, "invert") == 0)
        return FDC_BUS_XLATE_INVERT;
    fprintf(stderr, "WARN: unknown FDC bus_xlate '%s', falling back to 'invert'\n", s);
    return FDC_BUS_XLATE_INVERT;
}

/**
 * @brief Vrací string reprezentaci bus_xlate pro INI uložení.
 */
static const char *fdc_bus_xlate_to_string(en_FDC_BUS_XLATE x)
{
    return (x == FDC_BUS_XLATE_PASSTHROUGH) ? "passthrough" : "invert";
}

const char *fdc_dskpath_keyname(st_FDC *fdc, unsigned drive_id)
{
    /* Per-instance klíče. FDC0 zachovává historické "wd279x_fddX_dskpath"
     * kvůli zpětné kompatibilitě INI; FDC1 má vlastní namespace "fdc1_...". */
    static const char *names_fdc0[4] = {
        "wd279x_fdd0_dskpath",
        "wd279x_fdd1_dskpath",
        "wd279x_fdd2_dskpath",
        "wd279x_fdd3_dskpath"
    };
    static const char *names_fdc1[4] = {
        "fdc1_fdd0_dskpath",
        "fdc1_fdd1_dskpath",
        "fdc1_fdd2_dskpath",
        "fdc1_fdd3_dskpath"
    };
    if (!fdc || drive_id >= 4)
        return NULL;
    return (fdc->index == FDC1) ? names_fdc1[drive_id] : names_fdc0[drive_id];
}

const char *fdc_readonly_keyname(st_FDC *fdc, unsigned drive_id)
{
    static const char *names_fdc0[4] = {
        "wd279x_fdd0_readonly",
        "wd279x_fdd1_readonly",
        "wd279x_fdd2_readonly",
        "wd279x_fdd3_readonly"
    };
    static const char *names_fdc1[4] = {
        "fdc1_fdd0_readonly",
        "fdc1_fdd1_readonly",
        "fdc1_fdd2_readonly",
        "fdc1_fdd3_readonly"
    };
    if (!fdc || drive_id >= 4)
        return NULL;
    return (fdc->index == FDC1) ? names_fdc1[drive_id] : names_fdc0[drive_id];
}

int fdc_cfg_get_readonly(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= 4)
        return 0;
    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)fdc_cfg_section_name(fdc));
    if (!cmod) return 0;
    const char *keyname = fdc_readonly_keyname(fdc, drive_id);
    if (!keyname) return 0;
    CFGELM *elm = cfgmodule_get_element_by_name(cmod, (char *)keyname);
    if (!elm) return 0;
    return cfgelement_get_bool_value(elm) ? 1 : 0;
}

void fdc_cfg_set_readonly(st_FDC *fdc, unsigned drive_id, int value)
{
    if (!fdc || drive_id >= 4) return;
    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)fdc_cfg_section_name(fdc));
    if (!cmod) return;
    const char *keyname = fdc_readonly_keyname(fdc, drive_id);
    if (!keyname) return;
    CFGELM *elm = cfgmodule_get_element_by_name(cmod, (char *)keyname);
    if (!elm) return;
    cfgelement_set_bool_value(elm, value ? 1 : 0);
}

const char *fdc_storage_mode_keyname(st_FDC *fdc, unsigned drive_id)
{
    static const char *names_fdc0[4] = {
        "wd279x_fdd0_storage_mode",
        "wd279x_fdd1_storage_mode",
        "wd279x_fdd2_storage_mode",
        "wd279x_fdd3_storage_mode"
    };
    static const char *names_fdc1[4] = {
        "fdc1_fdd0_storage_mode",
        "fdc1_fdd1_storage_mode",
        "fdc1_fdd2_storage_mode",
        "fdc1_fdd3_storage_mode"
    };
    if (!fdc || drive_id >= 4)
        return NULL;
    return (fdc->index == FDC1) ? names_fdc1[drive_id] : names_fdc0[drive_id];
}

const char *fdc_cfg_get_storage_mode(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= 4) return "cached";
    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)fdc_cfg_section_name(fdc));
    if (!cmod) return "cached";
    const char *keyname = fdc_storage_mode_keyname(fdc, drive_id);
    if (!keyname) return "cached";
    char *v = cfgmodule_get_element_text_value_by_name(cmod, (char *)keyname);
    if (!v || !*v) return "cached";
    /* Sanitize - vrátit jen známé hodnoty (volající očekává validní string). */
    if (strcmp(v, "direct") == 0) return "direct";
    if (strcmp(v, "discard") == 0) return "discard";
    return "cached";
}

void fdc_cfg_set_storage_mode(st_FDC *fdc, unsigned drive_id, const char *value)
{
    if (!fdc || drive_id >= 4) return;
    if (!value) value = "cached";
    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)fdc_cfg_section_name(fdc));
    if (!cmod) return;
    const char *keyname = fdc_storage_mode_keyname(fdc, drive_id);
    if (!keyname) return;
    CFGELM *elm = cfgmodule_get_element_by_name(cmod, (char *)keyname);
    if (!elm) return;
    cfgelement_set_text_value(elm, value);
}

/**
 * @brief Zapíše DSK cestu dané mechaniky do cfgfile elementu.
 *
 * Aktualizuje TEXT element `wd279x_fddX_dskpath` v sekci `[FDC]`, aby
 * se mount perzistoval do INI při exitu. Pokud sekce nebo klíč nejsou
 * registrované, funkce je no-op.
 *
 * @param fdc      instance FDC řadiče (NULL = no-op).
 * @param drive_id index mechaniky (0..3).
 * @param value    C-string s cestou (prázdný řetězec = umounted).
 */
void fdc_cfg_set_dskpath(st_FDC *fdc, unsigned drive_id, const char *value)
{
    if (!fdc || drive_id >= 4)
        return;

    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)fdc_cfg_section_name(fdc));
    if (!cmod)
        return;

    const char *keyname = fdc_dskpath_keyname(fdc, drive_id);
    if (!keyname)
        return;

    CFGELM *elm = cfgmodule_get_element_by_name(cmod, (char *)keyname);
    if (!elm)
        return;

    cfgelement_set_text_value(elm, (value ? value : ""));
}

/**
 * @brief Zaregistruje cfgfile sekci dané instance FDC a její klíče.
 *
 * Vytvoří modul sekce `fdc_cfg_section_name(fdc)` v g_cfgmain (FDC0 =
 * `[FDC]`, FDC1 = `[FDC1]`) a registruje klíče:
 *  - `hd_patch` (BOOL) - 0/1, handler bind na `fdc->hd_patch`
 *  - `bus_xlate` (TEXT) - "invert" / "passthrough"
 *  - `<prefix>_fdd0_dskpath` .. `_fdd3_dskpath` (TEXT) - cesty k DSK
 *    obrazům per drive (prefix dle fdc_dskpath_keyname).
 *  - `<prefix>_fdd0_readonly` .. `_fdd3_readonly` (BOOL) - persistent
 *    user R/O toggle.
 *  - `<prefix>_fdd0_storage_mode` .. `_fdd3_storage_mode` (TEXT) -
 *    "cached" | "direct" | "discard".
 *
 * @param fdc instance FDC řadiče (určuje sekci i prefix klíčů).
 * @return ukazatel na modul (nikdy NULL - chyba alokace = abort).
 */
static CFGMOD *fdc_register_cfgmodule(st_FDC *fdc, unsigned connected_default)
{
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, (char *)fdc_cfg_section_name(fdc));

    CFGELM *elm;

    /* connected - BOOL, default připojen pro FDC0, odpojen pro FDC1.
     * Persistuje stav připojení řadiče přes restart (dřív se nastavoval
     * natvrdo a uživatelská volba se zapomněla). Handler bind na
     * fdc->connected -> menu toggle se uloží do INI při exitu. */
    elm = cfgmodule_register_new_element(cmod, "connected", CFGENTYPE_BOOL,
                                         connected_default ? 1 : 0);
    cfgelement_set_handlers(elm, (void *)&fdc->connected, (void *)&fdc->connected);

    /* hd_patch - BOOL, default 1 (= HD Patch obvod osazen, typický stav
     * Sharp MZ-800 sestavy). User vypne pro raw / unpatched FDC. */
    elm = cfgmodule_register_new_element(cmod, "hd_patch", CFGENTYPE_BOOL, 1);
    cfgelement_set_handlers(elm, (void *)&fdc->hd_patch, (void *)&fdc->hd_patch);

    /* bus_xlate - TEXT, default "invert". */
    elm = cfgmodule_register_new_element(cmod, "bus_xlate", CFGENTYPE_TEXT, "invert");
    (void)elm;

    /* <prefix>_fddX_dskpath - TEXT, default "". Bez handlerů / propagate_cb -
     * implementace si je čte manuálně po cfgmodule_parse(). */
    for (unsigned i = 0; i < 4; i++)
    {
        elm = cfgmodule_register_new_element(cmod, (char *)fdc_dskpath_keyname(fdc, i), CFGENTYPE_TEXT, "");
        (void)elm;
    }

    /* <prefix>_fddX_readonly - BOOL, default 0. Persistent user preference.
     * Když ON, chip odmítá WRITE bez ohledu na FS atributy souboru. */
    for (unsigned i = 0; i < 4; i++)
    {
        elm = cfgmodule_register_new_element(cmod, (char *)fdc_readonly_keyname(fdc, i), CFGENTYPE_BOOL, 0);
        (void)elm;
    }

    /* <prefix>_fddX_storage_mode - TEXT, default "cached". Hodnoty:
     *  - "cached"  = DSK obraz v RAM, sync při reset/umount/exit/manual
     *  - "direct"  = file_driver, immediate writes do souboru
     *  - "discard" = DSK obraz v RAM, NIKDY se nesynchronizuje
     */
    for (unsigned i = 0; i < 4; i++)
    {
        elm = cfgmodule_register_new_element(cmod, (char *)fdc_storage_mode_keyname(fdc, i), CFGENTYPE_TEXT, "cached");
        (void)elm;
    }

    return cmod;
}

/**
 * @brief Aplikuje cfgfile + CLI override na `fdc->bus_xlate` a `fdc->hd_patch`.
 *
 * Volá se po `cfgmodule_parse()`. Vytahuje TEXT hodnoty z cfgfile
 * (bus_xlate) a přeloží je na enum. CLI flagy mají prioritu - pokud
 * jsou zadány, přepíší hodnotu z cfgfile i samotný cfgfile záznam,
 * aby se volba persistovala při exitu.
 *
 * CLI flagy `--fdc-bus-xlate` / `--fdc-hd-patch` se aplikují pouze na
 * FDC0; FDC1 zatím vlastní CLI flagy nemá (konfiguruje se jen přes INI
 * sekci `[FDC1]`).
 *
 * @param cmod registrovaný cfg modul dané instance.
 * @param fdc  instance FDC řadiče.
 */
static void fdc_apply_cfg_and_cli(CFGMOD *cmod, st_FDC *fdc)
{
    /* === bus_xlate === */
    char *xlate_str = cfgmodule_get_element_text_value_by_name(cmod, "bus_xlate");
    fdc->bus_xlate = fdc_parse_bus_xlate_string(xlate_str);

    /* CLI override jen pro FDC0 (FDC1 nemá registrované --fdc1-* flagy). */
    if (fdc->index != FDC0)
        return;

    const char *cli_xlate = sdlapp_option_value("--fdc-bus-xlate");
    if (cli_xlate)
    {
        fdc->bus_xlate = fdc_parse_bus_xlate_string(cli_xlate);
        CFGELM *e = cfgmodule_get_element_by_name(cmod, "bus_xlate");
        if (e)
            cfgelement_set_text_value(e, fdc_bus_xlate_to_string(fdc->bus_xlate));
    }

    /* === hd_patch === BOOL je propagován přes set_handlers automaticky.
     * CLI ho přebije pokud zadán. */
    const char *cli_hd = sdlapp_option_value("--fdc-hd-patch");
    if (cli_hd)
    {
        /* sdlapp_options validátor BOOL_ON_OFF zaručil přesně "on" | "off". */
        int v = (strcmp(cli_hd, "on") == 0) ? 1 : 0;
        fdc->hd_patch = v;
        CFGELM *e = cfgmodule_get_element_by_name(cmod, "hd_patch");
        if (e)
            cfgelement_set_bool_value(e, v);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Inicializuje jednu instanci FDC řadiče.
 *
 * Registruje cfg sekci instance, naparsuje konfiguraci, inicializuje
 * chip + mechaniky, napojí HD Patch ref a auto-mountne DSK obrazy z INI.
 *
 * @param fdc               instance FDC řadiče.
 * @param default_connected výchozí stav connected (FDC_CONNECTED /
 *                          FDC_DISCONNECTED) - použije se jako default
 *                          cfg klíče `connected`, pokud INI hodnotu nemá.
 *                          Uživatelská volba z menu persistuje přes restart.
 */
static void fdc_init_instance(st_FDC *fdc, unsigned default_connected)
{
    /* Krok 1: registrace cfg sekce instance ([FDC] / [FDC1]). Default
     * connect stavu (FDC0 on / FDC1 off) je defaultní hodnota cfg klíče. */
    CFGMOD *cmod = fdc_register_cfgmodule(fdc, default_connected);

    /* Krok 2: parse cfgfile + propagate hodnot do bind variables
     * (connected, hd_patch jdou přes set_handlers). */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    /* Krok 3: dotáhnout TEXT klíče (bus_xlate) ručně + CLI override. */
    fdc_apply_cfg_and_cli(cmod, fdc);

    /* Krok 4: vynuluj wd279x a drive[] (cfg fields hd_patch/bus_xlate
     * už jsou nastavené, ostatní stav začíná z čistého). */
    memset(&fdc->wd279x, 0, sizeof(fdc->wd279x));
    memset(&fdc->drive[0], 0, sizeof(fdc->drive));

    wd279x_init(&fdc->wd279x);

    /* Drives: vše umounted. memset výše už struct vynuloval; tyto
     * explicit zápisy slouží jako čitelná dokumentace invariantů. */
    for (unsigned i = 0; i < FDC_NUM_DRIVES; i++)
    {
        fdc->drive[i].mounted = 0;
        fdc->drive[i].handler_valid = 0;
        fdc->drive[i].geometry_valid = 0;
        fdc->drive[i].readonly = 0;
        fdc->drive[i].filename[0] = 0x00;
    }

    /* Napoj chip na drives array - chip potřebuje přístup pro READ/WRITE
     * SECTOR. Musí být po init, protože init si pamatuje původní pointer. */
    wd279x_attach_drives(&fdc->wd279x, &fdc->drive[0]);

    /* Napoj per-instance HD Patch konfiguraci na chip (port 0xDFh EINT
     * INT logika). Musí být po wd279x_init (ten ref přežívá memset). */
    fdc->wd279x.hd_patch_ref = &fdc->hd_patch;

    /* Connect stav: nastaven už v Kroku 2 přes cfg propagate klíče
     * `connected` (persistuje uživatelskou volbu z menu přes restart;
     * memset wd279x/drive[] výše se ho netýká). */

    /* Auto-mount z cfgfile: pro každou mechaniku vytáhni `<prefix>_fddX_dskpath`
     * TEXT klíč a pokud není prázdný, otevři DSK obraz. Mount je nezávislý
     * na connect stavu (obrazy jsou připravené, jakmile user řadič připojí).
     * Při neúspěchu open zůstane drive umounted a chyba se vypíše na stderr. */
    for (unsigned i = 0; i < FDC_NUM_DRIVES && i < 4; i++)
    {
        char *path = cfgmodule_get_element_text_value_by_name(cmod, (char *)fdc_dskpath_keyname(fdc, i));
        if (path && path[0] != 0x00)
        {
            fdc_mount_dskfile(fdc, i, path);
        }
    }
}

void fdc_init(void)
{
    /* FDC0 (primární, porty 0xD8h-0xDFh) - defaultně připojen. */
    fdc_init_instance(&g_fdc[FDC0], FDC_CONNECTED);

    /* FDC1 (sekundární, porty 0x58h-0x5Fh) - defaultně odpojen; uživatel
     * jej připojí v menu Devices -> FD Controller. */
    fdc_init_instance(&g_fdc[FDC1], FDC_DISCONNECTED);
}

void fdc_reset(void)
{
    /* HW reset emulovaného stroje - před resetem chipu sync cached
     * DSK obrazů na disk. User očekává, že reset nezahodí už zapsaná
     * data (sync se děje při reset, umount, exit, manual). Aplikuje se
     * na všechny instance. */
    for (unsigned n = 0; n < FDC_INSTANCE_COUNT; n++)
    {
        st_FDC *fdc = &g_fdc[n];
        for (unsigned i = 0; i < FDC_NUM_DRIVES; i++)
        {
            (void)fdc_drive_flush_to_file(&fdc->drive[i]);
        }
        wd279x_reset(&fdc->wd279x);
    }
    /* Pozn.: HW reset nezavírá DSK obrazy - mount zůstává. */
}

void fdc_exit(void)
{
    /* Zavři handlery všech mechanik = uvolní RAM buffery s obsahem
     * DSK obrazů (případně zavře FILE* u DIRECT módu). NEvoláme
     * `fdc_umount()` - ten by smazal INI klíče `<prefix>_fddX_dskpath`,
     * čímž by se persistovaný mount stav ztratil pro příští start.
     * Lifecycle exit zavírá jen runtime stav. Pokrývá všechny instance. */
    for (unsigned n = 0; n < FDC_INSTANCE_COUNT; n++)
    {
        for (unsigned i = 0; i < FDC_NUM_DRIVES; i++)
        {
            fdc_drive_close(&g_fdc[n], i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* BUS / Translation vrstva                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Test zda port-offset spadá do rozsahu portů MB8876A chipu
 *        (= za invertorem na Sharp boardu).
 *
 * Sharp inverze datové sběrnice se týká pouze portů 0xD8h - 0xDBh
 * (CMDSTS, TRACK, SECTOR, DATA = offsety 0..3). Externí Sharp logic
 * porty 0xDCh - 0xDFh (MOTOR, SIDE, DENSITY, EINT = offsety 4..7)
 * NEJSOU za invertorem - prochází bez inverze.
 *
 * @param offset port offset (0..7, již maskovaný před voláním).
 * @return 1 pokud offset je chip port (inverze povolena), 0 jinak.
 */
static inline int fdc_offset_is_chip_port(int offset)
{
    return (offset >= 0 && offset <= 3) ? 1 : 0;
}

int fdc_read_byte(st_FDC *fdc, int i_addroffset, uint8_t *io_data)
{
    if (!fdc || !io_data)
        return 0;

    uint8_t chip_value = 0xFF;
    int rc = wd279x_read_byte(&fdc->wd279x, i_addroffset, &chip_value);

    /* BUS xlate inverze: aplikuje se jen na chip porty (offsety 0..3).
     * Externí Sharp logic porty (4..7) jdou bez inverze. Viz file-level
     * komentář, sekce "Inverze konvence". */
    int do_invert = (fdc->bus_xlate == FDC_BUS_XLATE_INVERT)
                    && fdc_offset_is_chip_port(i_addroffset & 0x07);
    if (do_invert)
    {
        *io_data = (uint8_t)~chip_value;
    }
    else
    {
        *io_data = chip_value;
    }

    /* Po I/O na FDC se může změnit stav /INT (HD Patch EINT režim);
     * notifikuj mzarch interrupt manager. */
    mzarch_interrupt_manager();
    return rc;
}

int fdc_write_byte(st_FDC *fdc, int i_addroffset, uint8_t *io_data)
{
    if (!fdc || !io_data)
        return 0;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat FDC write. Payload (per HW-log_format_CZ.md):
     *   [0] = addroffset (0..7)
     *   [1] = value (raw byte na sběrnici; před BUS xlate inverzí)
     *   [2..5] = rezervováno
     */
    if (TEST_TRACE_HWLOG_DISPATCH)
    {
        uint8_t payload[6] = {
            (uint8_t)(i_addroffset & 0xff),
            *io_data, 0, 0, 0, 0
        };
        hwlog_record(HWLOG_CHIP_FDC, HWLOG_FDC_REGISTER_WRITE, payload);
    }
#endif

    uint8_t chip_value = *io_data;
    /* BUS xlate inverze: aplikuje se jen na chip porty (offsety 0..3).
     * Externí Sharp logic porty (4..7) jdou bez inverze. Viz file-level
     * komentář, sekce "Inverze konvence". */
    int do_invert = (fdc->bus_xlate == FDC_BUS_XLATE_INVERT)
                    && fdc_offset_is_chip_port(i_addroffset & 0x07);
    if (do_invert)
    {
        chip_value = (uint8_t)~chip_value;
    }

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: HWLOG_FDC_COMMAND_ISSUED.
     * Emit dekódovaného command typu při zápisu na FDCPORT_CMDSTS (= command
     * write). REGISTER_WRITE výše loguje raw BUS byte (před inverzí),
     * COMMAND_ISSUED loguje true-bus dekódovaný typ + kontext mechaniky.
     *
     * Payload (6 B):
     *   [0] cmd_type    - en_WD279X_COMMAND_TYPE
     *   [1] side        - chip->SIDE & 0x01
     *   [2] track_reg   - chip->regTRACK (true-bus)
     *   [3] sector_reg  - chip->regSECTOR (true-bus)
     *   [4] cmd_flags   - dolní 4 bity raw cmd (= update / side / multi)
     *   [5] raw_cmd     - true-bus command byte
     *
     * Pozn.: eventlog ring přenese jen prvních 4 B (= cmd_type/side/T/S).
     * Flags + raw cmd zůstávají v hwlog disk chunku pro off-line parser. */
    /* FDCPORT_CMDSTS = 0 (= wd279x_internal.h, neimportujeme - veřejné API
     * fdc.c drží jen wd279x.h vlastní mapping bus offsetu). */
    if (TEST_TRACE_HWLOG_DISPATCH
        && (i_addroffset & 0x07) == 0)
    {
        en_WD279X_COMMAND_TYPE cmd_type =
            wd279x_decode_command_type(chip_value);
        uint8_t cmd_payload[6] = {
            (uint8_t) cmd_type,
            (uint8_t)(fdc->wd279x.SIDE & 0x01),
            fdc->wd279x.regTRACK,
            fdc->wd279x.regSECTOR,
            (uint8_t)(chip_value & 0x0F),
            chip_value
        };
        hwlog_record(HWLOG_CHIP_FDC, HWLOG_FDC_COMMAND_ISSUED, cmd_payload);
    }
#endif

    int rc = wd279x_write_byte(&fdc->wd279x, i_addroffset, &chip_value);

    /* Po I/O na FDC se může změnit stav /INT (HD Patch EINT režim);
     * notifikuj mzarch interrupt manager. */
    mzarch_interrupt_manager();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* HWE - HW event BP hook (irq:fdc). Předáváme aktuální IRQ level
     * (0/1); HWE.2 enforce vrstva rozhodne dle trigger condition
     * (level/edge). */
    if (g_bp_event_active[BP_EVENT_IRQ_FDC])
    {
        int cur_irq = wd279x_get_interrupt_state(&fdc->wd279x) ? 1 : 0;
        bp_event_fire(BP_EVENT_IRQ_FDC, cur_irq);
    }
#endif
    return rc;
}

int fdc_get_interrupt_state(void)
{
    /* /INT linka je sdílená - agreguj (OR) přes všechny připojené
     * instance. Odpojený řadič na linku nepřispívá. */
    int state = 0;
    for (unsigned n = 0; n < FDC_INSTANCE_COUNT; n++)
    {
        if (g_fdc[n].connected)
            state |= wd279x_get_interrupt_state(&g_fdc[n].wd279x);
    }
    return state;
}

/* ------------------------------------------------------------------ */
/* Drives / Mount                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Opraví vadné DSK hlavičky známých distribuovaných formátů.
 *
 * Některé HD DSK obrazy distribuované v Sharp MZ-800 komunitě mají v
 * Extended CPC DSK hlavičce vadný `tsize` array - typicky deklarovaná
 * velikost stopy neodpovídá reálné velikosti následujícího `Track-Info`
 * bloku. Bez opravy `dsk_compute_track_offset` vrací špatné offsety,
 * `dsk_read_short_sector_info` čte data z prostředku jiné stopy a vrací
 * `ssize=0` (nebo nesmyslné hodnoty).
 *
 * Algoritmus: prohledá DSK obraz v paměti, najde všechny `"Track-Info"`
 * magic stringy, určí reálné velikosti stop jako rozdíly sousedních
 * offsetů a opraví byte v `tsize` array v hlavičce.
 *
 * Pracuje přímo nad RAM bufferem (`handler.spec.memspec.ptr`) - opravy
 * jsou snapshot-friendly (= žijí v paměti, sync na FS je opt-in).
 *
 * Pokud DSK obraz není v paměti (handler není MEMORY type), funkce
 * je no-op.
 *
 * @param drv mechanika s úspěšně otevřeným handlerem (handler_valid=1).
 */
static void fdc_repair_dsk_header(st_FDDrive *drv)
{
    if (drv->handler.type != HANDLER_TYPE_MEMORY)
        return;

    uint8_t *image = drv->handler.spec.memspec.ptr;
    size_t image_size = drv->handler.spec.memspec.size;
    if (!image || image_size < 0x100)
        return;

    /* Hlavička Extended CPC DSK: offset 0x30 = tracks, 0x31 = sides,
     * 0x34..0xFF = tsize array (1 byte per abstrack). */
    uint8_t tracks = image[0x30];
    uint8_t sides = image[0x31];
    if (tracks == 0 || sides == 0)
        return;

    unsigned total = (unsigned)tracks * (unsigned)sides;
    if (total > (0x100 - 0x34))
        return; /* příliš mnoho stop pro tsize tabulku v hlavičce */

    /* Walk: spočítej kumulativní offset podle declared tsize, na každé
     * pozici ověř "Track-Info" magic. Pokud chybí, najdi nejbližší další
     * Track-Info magic za touto pozicí a oprav tsize[i-1] na (next - prev)/256. */
    uint32_t cur_offset = 0x100;
    int repaired = 0;
    for (unsigned i = 0; i < total; i++)
    {
        if (cur_offset + 10 > image_size)
            break;

        if (memcmp(image + cur_offset, "Track-Info", 10) != 0)
        {
            /* Magic chybí na declared offsetu. Najdi nejbližší další Track-Info
             * za pozicí (cur_offset - last declared * 0x100) tj. od začátku
             * předchozí stopy. */
            uint32_t prev_offset = cur_offset - ((uint32_t)image[0x34 + i - 1] * 0x100);
            uint32_t search_from = prev_offset + 0x100;
            uint32_t found = 0;
            for (uint32_t scan = search_from; scan + 10 <= image_size; scan += 0x100)
            {
                if (memcmp(image + scan, "Track-Info", 10) == 0)
                {
                    found = scan;
                    break;
                }
            }
            if (!found)
                break; /* nepodařilo se najít další stopu - dál nic neopravujeme */

            /* Reálná velikost předchozí stopy. */
            uint32_t real_size_blocks = (found - prev_offset) / 0x100;
            if (real_size_blocks == 0 || real_size_blocks > 0xFF)
                break;

            uint8_t old_tsize = image[0x34 + i - 1];
            image[0x34 + i - 1] = (uint8_t)real_size_blocks;
#ifdef FDC_DIAG
            fprintf(stderr, "fdc: DSK header repair - tsize[%u]: 0x%02X -> 0x%02X "
                            "(declared %u bytes, real %u bytes)\n",
                    i - 1, old_tsize, (uint8_t)real_size_blocks,
                    (unsigned)old_tsize * 0x100, (unsigned)real_size_blocks * 0x100);
#else
            (void)old_tsize;
#endif
            cur_offset = found;
            repaired = 1;
        }
        cur_offset += (uint32_t)image[0x34 + i] * 0x100;
    }

    if (repaired)
    {
        /* Označ paměťový handler jako modifikovaný - při sync na FS
         * (pokud někdy bude implementován) se opravy propíší. */
        drv->handler.spec.memspec.updated = 1;
    }
}

/**
 * @brief Test zda má drive nesynchronizované změny v cached módu.
 *
 * @param drv mechanika.
 * @return 1 = ano (CACHED mode + memspec.updated), 0 = ne (jiný mode nebo bez změn).
 */
static int drive_has_unsaved_changes_internal(st_FDDrive *drv)
{
    if (!drv || !drv->handler_valid)
        return 0;
    if (drv->readonly)
        return 0;
    if (drv->storage_mode != FDC_STORAGE_CACHED)
        return 0;
    /* CACHED mode používá memory handler - kontroluj jen tam.
     * DIRECT / DISCARD používají file handler (DIRECT) nebo memory bez sync (DISCARD). */
    if (drv->handler.type != HANDLER_TYPE_MEMORY)
        return 0;
    return drv->handler.spec.memspec.updated ? 1 : 0;
}

/**
 * @brief Pokud byl obsah DSK obrazu v paměti modifikován, ulož ho zpět
 *        do původního souboru.
 *
 * Sync je relevantní pouze pro CACHED storage mode:
 *  - CACHED: save_memory přepíše soubor obsahem RAM bufferu
 *  - DIRECT: no-op (writes už jdou přímo na disk přes file_driver)
 *  - DISCARD: no-op (změny se zahodí - úmysl)
 *
 * @param drv mechanika s otevřeným handlerem.
 * @return 1 = OK (nebo nebylo nutné), 0 = selhání zápisu.
 */
static int fdc_drive_flush_to_file(st_FDDrive *drv)
{
    if (!drv || !drv->handler_valid)
        return 1;
    if (drv->storage_mode != FDC_STORAGE_CACHED)
        return 1; /* DIRECT = už na disku, DISCARD = úmyslně zahodit. */
    if (drv->readonly)
        return 1; /* R/O = updated by neměl být nastaven, pro jistotu skip. */
    if (!drive_has_unsaved_changes_internal(drv))
        return 1; /* Žádné změny - nic neflushovat. */
    if (drv->filename[0] == 0x00)
        return 1;

    if (EXIT_SUCCESS != generic_driver_save_memory(&drv->handler, drv->filename))
    {
        fprintf(stderr, "%s(%d): failed to flush DSK image '%s' to file\n",
                __FILE__, __LINE__, drv->filename);
        return 0;
    }
    drv->handler.spec.memspec.updated = 0;
    return 1;
}

int fdc_sync_drive(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return 0;
    return fdc_drive_flush_to_file(&fdc->drive[drive_id]);
}

int fdc_drive_has_unsaved_changes(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return 0;
    return drive_has_unsaved_changes_internal(&fdc->drive[drive_id]);
}

int fdc_drive_fs_readonly(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return 0;
    return fdc->drive[drive_id].fs_readonly ? 1 : 0;
}

int fdc_drive_has_ram_changes(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return 0;
    st_FDDrive *drv = &fdc->drive[drive_id];
    if (!drv->handler_valid)
        return 0;
    if (drv->handler.type != HANDLER_TYPE_MEMORY)
        return 0;
    return drv->handler.spec.memspec.updated ? 1 : 0;
}

int fdc_drive_force_save_to_file(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return 0;
    st_FDDrive *drv = &fdc->drive[drive_id];
    if (!drv->handler_valid)
        return 0;
    if (drv->handler.type != HANDLER_TYPE_MEMORY)
        return 0;
    if (drv->filename[0] == 0x00)
        return 0;
    if (EXIT_SUCCESS != generic_driver_save_memory(&drv->handler, drv->filename))
    {
        fprintf(stderr, "%s(%d): failed to force-save DSK image '%s' to file\n",
                __FILE__, __LINE__, drv->filename);
        return 0;
    }
    drv->handler.spec.memspec.updated = 0;
    return 1;
}

/**
 * @brief Interní: zavře otevřený handler a vyresetuje mount stav mechaniky.
 *
 * Pokud je `handler_valid != 0`, před close flushne modifikace zpět do
 * souboru (CACHED mode) a zavolá generic_driver_close() (uvolní RAM
 * buffer / file handle). Následně vynuluje filename, geometrii a všechny
 * stavové flagy. UI notifikaci NEVOLÁ - to je odpovědnost volajícího.
 *
 * Idempotentní - opakované volání na již umounted drive je no-op.
 *
 * @param fdc instance FDC řadiče (NULL = no-op).
 * @param drive_id index mechaniky (0..FDC_NUM_DRIVES-1).
 */
static void fdc_drive_close(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return;

    st_FDDrive *drv = &fdc->drive[drive_id];

    if (drv->handler_valid)
    {
        /* Před close flushni modifikace zpět do souboru - jinak by změny
         * z WRITE SECTOR / WRITE TRACK byly při unmount nebo exitu emu
         * ztracené (memory_driver_close_cb jen uvolní RAM buffer). */
        (void)fdc_drive_flush_to_file(drv);
        generic_driver_close(&drv->handler);
        drv->handler_valid = 0;
    }
    drv->filename[0] = 0x00;
    drv->mounted = 0;
    drv->readonly = 0;
    drv->user_readonly = 0;
    drv->fs_readonly = 0;
    drv->storage_mode = FDC_STORAGE_CACHED;
    drv->geometry_valid = 0;
    memset(&drv->geometry, 0, sizeof(drv->geometry));
    /* st_HANDLER strukturu nemažeme - po close je její obsah neplatný
     * a další open ji přepíše. */
}

void fdc_mount_dskfile(st_FDC *fdc, unsigned drive_id, char *filename)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return;
    if (!filename)
        return;

    st_FDDrive *drv = &fdc->drive[drive_id];

    /* Remount: nejdřív zavřít předchozí mount (uvolní RAM buffer). */
    fdc_drive_close(fdc, drive_id);

    if (filename[0] == 0x00)
    {
        /* Prázdné jméno = umount. Stav je už vyresetovaný, jen UI a INI. */
        fdc_cfg_set_dskpath(fdc, drive_id, "");
        ui_fdc_set_dsk(drive_id, drv->filename);
        return;
    }

    /* Uložíme filename ještě před open - usnadní debug v případě chyby. */
    size_t maxlen = sizeof(drv->filename) - 1;
    strncpy(drv->filename, filename, maxlen);
    drv->filename[maxlen] = 0x00;

    /* Načti konfiguraci storage_mode + user_readonly. */
    const char *mode_str = fdc_cfg_get_storage_mode(fdc, drive_id);
    if (strcmp(mode_str, "direct") == 0)
        drv->storage_mode = FDC_STORAGE_DIRECT;
    else if (strcmp(mode_str, "discard") == 0)
        drv->storage_mode = FDC_STORAGE_DISCARD;
    else
        drv->storage_mode = FDC_STORAGE_CACHED;

    drv->user_readonly = fdc_cfg_get_readonly(fdc, drive_id) ? 1 : 0;
    drv->fs_readonly = (g_access(drv->filename, W_OK) == 0) ? 0 : 1;
    drv->readonly = (drv->user_readonly || drv->fs_readonly) ? 1 : 0;

    /* Otevři DSK obraz - způsob závisí na storage_mode:
     *  - CACHED / DISCARD: memory driver (in-RAM kopie)
     *  - DIRECT: file driver (operace přímo na FS souboru)
     *
     * DIRECT s readonly (= FS write-protected nebo user pref):
     *   open mode = RO ("rb"), writes selžou.
     * DIRECT s RW: open mode = RW ("r+b"). */
    st_HANDLER *h = NULL;
    st_DRIVER *driver = NULL;
    if (drv->storage_mode == FDC_STORAGE_DIRECT)
    {
        driver = &g_file_driver;
        en_FILE_DRIVER_OPEN_MODE om = drv->readonly ? FILE_DRIVER_OPMODE_RO : FILE_DRIVER_OPMODE_RW;
        h = generic_driver_open_file(&drv->handler, driver, drv->filename, om);
    }
    else
    {
        driver = &g_memory_driver_static;
        h = generic_driver_open_memory_from_file(&drv->handler, driver, drv->filename);
    }

    if ((h == NULL) || (driver->err != GENERIC_DRIVER_ERROR_NONE) || (drv->handler.err != HANDLER_ERROR_NONE))
    {
        fprintf(stderr, "%s(%d): failed to open DSK image '%s' (mode=%s): %s\n",
                __FILE__, __LINE__, drv->filename, mode_str,
                generic_driver_error_message(&drv->handler, driver));
        /* Při neúspěchu vrátíme drive do umounted stavu. INI vyčistíme. */
        drv->filename[0] = 0x00;
        fdc_cfg_set_dskpath(fdc, drive_id, "");
        ui_fdc_set_dsk(drive_id, drv->filename);
        return;
    }
    drv->handler_valid = 1;

    /* DSK header repair (in-memory walk přes Track-Info magic stringy)
     * funguje jen pro memory handler. Pro DIRECT mode skip - oprava by
     * vyžadovala read/walk celého obrazu přes file driver, což je drahé;
     * power-users s DIRECT módem by měli mít DSK image bez header bugů. */
    if (drv->storage_mode != FDC_STORAGE_DIRECT)
    {
        fdc_repair_dsk_header(drv);
    }

    generic_driver_set_handler_readonly_status(&drv->handler, drv->readonly);

    /* Načti geometrii (počet stop, stran, velikost obrazu). */
    if (EXIT_SUCCESS != dsk_get_geometry(&drv->handler, &drv->geometry))
    {
        fprintf(stderr, "%s(%d): failed to read DSK geometry '%s': %s\n",
                __FILE__, __LINE__, drv->filename,
                dsk_error_message(&drv->handler, driver));
        /* Geometrie selhala - obraz považujeme za nepoužitelný. Zavřeme
         * handler a vrátíme se do umounted stavu. INI vyčistíme. */
        fdc_drive_close(fdc, drive_id);
        fdc_cfg_set_dskpath(fdc, drive_id, "");
        ui_fdc_set_dsk(drive_id, drv->filename);
        return;
    }
    drv->geometry_valid = 1;
    drv->mounted = 1;

    /* Úspěšný mount - aktualizuj INI klíč. Při auto-mount z init flow
     * zapisujeme tu samou hodnotu (idempotent), při ručním UI mount
     * persistujeme novou cestu pro příští spuštění. */
    fdc_cfg_set_dskpath(fdc, drive_id, drv->filename);
    ui_fdc_set_dsk(drive_id, drv->filename);
}

void fdc_umount(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return;

    fdc_drive_close(fdc, drive_id);
    /* Vyčisti INI klíč - umount se má persistovat (jinak by se při dalším
     * startu obraz znovu auto-mountnul z perzistentní cesty). */
    fdc_cfg_set_dskpath(fdc, drive_id, "");
    ui_fdc_set_dsk(drive_id, fdc->drive[drive_id].filename);
}

/**
 * @brief Callback file chooseru po výběru DSK obrazu.
 *
 * Předaný `user_data` je drive_id zabalený přes `GINT_TO_POINTER`.
 * Po výběru souboru zavolá `fdc_mount_dskfile` který filename uloží
 * a otevře DSK image.
 *
 * @note user_data nese zabalený identifikátor `(fdc_index << 8) | drive_id`
 *       (viz fdc_ui_mount) - callback tak ví, do které instance FDC a
 *       mechaniky mountovat.
 */
static void fdc_ui_mount_cb(baseui_fchooser_t *fch)
{
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
        return;
    };

    int packed = GPOINTER_TO_INT(fch->user_data);
    unsigned fdc_index = ((unsigned)packed >> 8) & 0xFF;
    unsigned drive_id = (unsigned)packed & 0xFF;
    if (fdc_index >= FDC_INSTANCE_COUNT)
    {
        baseui_filechooser_destroy(fch);
        return;
    }
    st_FDC *fdc = &g_fdc[fdc_index];

    if (!fdc->connected)
    {
        baseui_show_message(0, "Can't mount DSK image into FDC%u FD%d, because the FD controller is not connected.", fdc_index, drive_id);
        baseui_filechooser_destroy(fch);
        return;
    };

    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        /* Cancel / zavření dialogu bez výběru - mount necháváme BEZE ZMĚNY
         * (dřív se zde volal umount, takže Cancel omylem odmountoval
         * stávající obraz). */
        baseui_filechooser_destroy(fch);
        return;
    };

    char *filename = fch->selected_filePathName;

    if (strlen(filename) < sizeof(fdc->drive[drive_id].filename))
    {
        fdc_mount_dskfile(fdc, drive_id, filename);
    }
    else
    {
        baseui_show_message(0, "Sorry, filepath is too big (%d).", (int)strlen(filename));
    };

    baseui_filechooser_destroy(fch);

    emulator_measuring_frame_timing_reset();
}

void fdc_ui_mount(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return;

    if (!fdc->connected)
    {
        baseui_show_message(0, "Can't mount DSK image into FDC%u FD%d, because the FD controller is not connected.", fdc->index, drive_id);
        return;
    };

    GString *str = g_string_new(NULL);
    g_string_append_printf(str, _("Select DSK image file for FDC%u drive FD%d"), fdc->index, drive_id);
    /* user_data nese zabalený (fdc_index << 8) | drive_id - callback tak ví,
     * do které instance FDC mountovat (jinak má jen drive_id). */
    int packed = (int)((fdc->index << 8) | drive_id);
    baseui_fchooser_t *fch = baseui_filechooser_open_file(str->str, ".dsk", NULL, NULL, fdc->drive[drive_id].filename, fdc_ui_mount_cb, GINT_TO_POINTER(packed));
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };
    g_string_free(str, TRUE);
}

const char *fdc_get_dsk_filepath(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return NULL;
    return fdc->drive[drive_id].filename;
}

int fdc_test_drive_id_mounted(st_FDC *fdc, unsigned drive_id)
{
    if (!fdc || drive_id >= FDC_NUM_DRIVES)
        return 0;
    return (int)fdc->drive[drive_id].mounted;
}

/* ------------------------------------------------------------------ */
/* UI callback fallback (no-op stub)                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Default UI hook - no-op stub.
 *
 * Volání `ui_fdc_set_dsk` je interně používáno implementací při
 * mount/umount jako notifikace UI. Pokud aplikace nemá UI vrstvu
 * registrující vlastní symbol, použije se tento weak fallback.
 */
__attribute__((weak)) void ui_fdc_set_dsk(unsigned drive_id, char *dsk_filename)
{
    (void)drive_id;
    (void)dsk_filename;
}
