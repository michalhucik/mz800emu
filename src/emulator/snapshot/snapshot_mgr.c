/**
 * @file snapshot_mgr.c
 * @brief Snapshot Manager — registrace komponent, orchestrace save/load
 */

#include "snapshot_mgr.h"
#include "snapshot_io.h"
#include "snapshot_xml.h"
#include "snapshot_screenshot.h"

#include "emulator.h"
#include "cfgmain.h"
#include "mzarch/mzarch.h"
#include "mzarch/mzcommon_config.h"

#include "mzarch/mzhal.h"
#include "mzarch/mztvsys_values.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/eventlog.h"
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


/* ========================================================================= */
/*                        Globální nastavení                                 */
/* ========================================================================= */

st_SNAPSHOT_SETTINGS g_snapshot_settings = {
    .include_ramdisk = false,
    .include_memext = true,
    .compression_level = 6,
    .default_directory = "",
    .quicksave_filename = "quicksave",
    .quicksave_mode = 0,        /* Basic */
    .quicksave_max_slots = 5,
    /* Default: po loadu/quickloadu nechat emulaci v pauze. Uživatel typicky
     * chce načtený stav nejdřív vidět/prozkoumat, ne ho hned rozběhnout.
     * Lze přepnout v Snapshot Setup dialogu (Always Run / Always Pause /
     * Previous State). */
    .load_resume_mode = SNAPSHOT_RESUME_ALWAYS_PAUSE,
    .quickload_resume_mode = SNAPSHOT_RESUME_ALWAYS_PAUSE,
};


/* ========================================================================= */
/*                         Registr komponent                                 */
/* ========================================================================= */

#define SNAPSHOT_MAX_COMPONENTS 32

static struct {
    const char *name;
    en_SNAPSHOT_PRIORITY priority;
    snapshot_save_cb_t save_cb;
    snapshot_load_cb_t load_cb;
    bool is_optional;
} g_snapshot_registry[SNAPSHOT_MAX_COMPONENTS];

static int g_snapshot_registry_count = 0;


int snapshot_register_component(const char *name,
                                en_SNAPSHOT_PRIORITY priority,
                                snapshot_save_cb_t save_cb,
                                snapshot_load_cb_t load_cb,
                                bool is_optional)
{
    if (g_snapshot_registry_count >= SNAPSHOT_MAX_COMPONENTS) {
        SNAP_ERR("mgr", "component registry is full (max %d)", SNAPSHOT_MAX_COMPONENTS);
        return -1;
    }

    /* Najít správnou pozici pro vložení (seřazeno dle priority) */
    int pos = g_snapshot_registry_count;
    for (int i = 0; i < g_snapshot_registry_count; i++) {
        if (priority < g_snapshot_registry[i].priority) {
            pos = i;
            break;
        }
    }

    /* Posunout existující záznamy */
    for (int i = g_snapshot_registry_count; i > pos; i--) {
        g_snapshot_registry[i] = g_snapshot_registry[i - 1];
    }

    /* Vložit nový záznam */
    g_snapshot_registry[pos].name = name;
    g_snapshot_registry[pos].priority = priority;
    g_snapshot_registry[pos].save_cb = save_cb;
    g_snapshot_registry[pos].load_cb = load_cb;
    g_snapshot_registry[pos].is_optional = is_optional;
    g_snapshot_registry_count++;

    return 0;
}


int snapshot_mgr_get_component_count(void)
{
    return g_snapshot_registry_count;
}


/* ========================================================================= */
/*                         Manifest save/load                                */
/* ========================================================================= */

/**
 * Uložení manifestu do archivu
 */
static en_SNAPSHOT_RESULT snapshot_save_manifest(st_SNAPSHOT_CONTEXT *ctx,
                                                  const char *description,
                                                  const char *checksum)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    /* Kořenový element s verzí formátu */
    char ver_str[16];
    snprintf(ver_str, sizeof(ver_str), "%d", SNAPSHOT_FORMAT_VERSION);
    snapshot_xml_open_element_attr(w, "mzs_snapshot", "version", ver_str);

    /* Identifikace emulátoru - runtime z g_mzhal (mzhal krok 7); hodnoty
     * jsou zmrazený on-disk kontrakt, runtime podoba je s dřívějším
     * compile-time MZARCH_NAME "emu" / MZARCH identická. */
    char emu_name[32];
    snprintf(emu_name, sizeof(emu_name), "%semu", g_mzhal.arch_name);
    snapshot_xml_open_element(w, "emulator");
    snapshot_xml_write_string(w, "name", emu_name);
    snapshot_xml_write_string(w, "version", CFGMAIN_EMULATOR_VERSION);
    snapshot_xml_write_string(w, "build_date", cfgmain_get_build_date());
    snapshot_xml_close_element(w); /* /emulator */

    /* Architektura (osa arch 700/800/1500, sdílená oběma mz700 targety) */
    snapshot_xml_write_int(w, "architecture", g_mzhal.arch);

    /* TV systém (osa tvsys 50=PAL/60=NTSC; mzhal krok 9, rozhodnutí
     * 2026-07-30: snapshoty mz700-pal a mz700-ntsc jsou nekompatibilní).
     * Starší emulátory element neznají a ignorují ho, verze formátu se
     * proto nemění. */
    snapshot_xml_write_int(w, "tvsys", g_mzhal.tvsys);

    /* Metadata */
    /* cfgmain_create_timestamp() vrací statický buffer — nepoužívat g_free! */
    const char *timestamp = cfgmain_create_timestamp();
    snapshot_xml_write_string(w, "created", timestamp ? timestamp : "unknown");

    snapshot_xml_write_string(w, "description", description ? description : "");

    /* Nastavení snapshotu */
    snapshot_xml_open_element(w, "settings");
    snapshot_xml_write_bool(w, "include_ramdisk", g_snapshot_settings.include_ramdisk);
    snapshot_xml_write_bool(w, "include_memext", g_snapshot_settings.include_memext);
    snapshot_xml_close_element(w); /* /settings */

    /* SHA-256 checksum — jako element s atributem a child elementem pro hodnotu */
    if (checksum) {
        snapshot_xml_open_element_attr(w, "checksum", "algorithm", "sha256");
        snapshot_xml_write_string(w, "value", checksum);
        snapshot_xml_close_element(w);
    }

    snapshot_xml_close_element(w); /* /mzs_snapshot */

    char *xml = snapshot_xml_writer_finish(w);

    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "manifest.xml", xml);
    g_free(xml);
    return res;
}


/**
 * @brief Historický default TV systému pro legacy snapshoty bez pole <tvsys>.
 *
 * Policy (rozhodnutí 2026-07-30): snapshot bez pole <tvsys> se posuzuje
 * jako by vznikl na defaultní variantě své architektury - mz800 a mz1500
 * mají jedinou tvsys variantu (PAL, resp. NTSC), u mz700 je default PAL
 * (evropská varianta). Legacy mz700 snapshot z NTSC buildu se tedy na
 * mz700emu-ntsc odmítne - vzácný případ, cenou za něj je jistota, že se
 * PAL stav nikdy tiše nenačte do NTSC časování.
 *
 * @param architecture MZARCH hodnota ze snapshot metadat (700/800/1500)
 * @return MZTVSYS_PAL/MZTVSYS_NTSC; 0 pro neznámou architekturu (vede
 *         na reject v tvsys checku, arch check ale selže dřív)
 */
static int snapshot_legacy_default_tvsys(int architecture)
{
    switch (architecture) {
        case 700:  return MZTVSYS_PAL;
        case 800:  return MZTVSYS_PAL;
        case 1500: return MZTVSYS_NTSC;
        default:   return 0;
    }
}


/**
 * Načtení manifestu z archivu
 */
static en_SNAPSHOT_RESULT snapshot_load_manifest_from_io(snapshot_io_t *io,
                                                          st_SNAPSHOT_METADATA *metadata)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT result = snapshot_io_read_xml(io, "manifest.xml", &xml);
    if (result != SNAPSHOT_OK) return result;

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    if (!r) {
        g_free(xml);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Vstoupit do kořenového elementu */
    if (!snapshot_xml_enter_element(r, "mzs_snapshot")) {
        snapshot_xml_reader_free(r);
        g_free(xml);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Verze formátu z atributu */
    char *version_str = NULL;
    if (snapshot_xml_read_attr(r, "version", &version_str)) {
        metadata->format_version = (uint32_t)atoi(version_str);
        g_free(version_str);
    } else {
        metadata->format_version = 1;
    }

    /* Emulator info */
    if (snapshot_xml_enter_element(r, "emulator")) {
        char *ver = NULL;
        if (snapshot_xml_read_string(r, "version", &ver)) {
            snprintf(metadata->emulator_version, sizeof(metadata->emulator_version), "%s", ver);
            g_free(ver);
        }
        snapshot_xml_leave_element(r);
    }

    /* Architektura */
    snapshot_xml_read_int(r, "architecture", &metadata->architecture);

    /* TV systém - legacy snapshoty element nemají, pak zůstává 0
     * (= unknown, vyhodnotí se přes historický default architektury) */
    metadata->tvsys = 0;
    snapshot_xml_read_int(r, "tvsys", &metadata->tvsys);

    /* Časové razítko */
    char *created = NULL;
    if (snapshot_xml_read_string(r, "created", &created)) {
        snprintf(metadata->created, sizeof(metadata->created), "%s", created);
        g_free(created);
    }

    /* Popis */
    char *desc = NULL;
    if (snapshot_xml_read_string(r, "description", &desc)) {
        snprintf(metadata->description, sizeof(metadata->description), "%s", desc);
        g_free(desc);
    }

    /* Checksum — uložen jako <checksum algorithm="sha256"><value>HASH</value></checksum> */
    if (snapshot_xml_enter_element(r, "checksum")) {
        char *cs_value = NULL;
        if (snapshot_xml_read_string(r, "value", &cs_value)) {
            snprintf(metadata->checksum, sizeof(metadata->checksum), "%s", cs_value);
            g_free(cs_value);
        }
        snapshot_xml_leave_element(r);
    }

    snapshot_xml_leave_element(r); /* /mzs_snapshot */
    snapshot_xml_reader_free(r);
    g_free(xml);

    return SNAPSHOT_OK;
}


/* ========================================================================= */
/*                         Textový popis chyby                               */
/* ========================================================================= */

const char *snapshot_result_to_string(en_SNAPSHOT_RESULT result)
{
    switch (result) {
        case SNAPSHOT_OK:               return "OK";
        case SNAPSHOT_ERR_IO:           return "I/O error";
        case SNAPSHOT_ERR_ZIP:          return "ZIP archive error";
        case SNAPSHOT_ERR_XML_PARSE:    return "XML parse error";
        case SNAPSHOT_ERR_XML_WRITE:    return "XML write error";
        case SNAPSHOT_ERR_VERSION:      return "Unsupported snapshot version";
        case SNAPSHOT_ERR_ARCHITECTURE: return "Incompatible architecture";
        case SNAPSHOT_ERR_MISSING_FILE: return "Missing file in archive";
        case SNAPSHOT_ERR_CORRUPTED:    return "Data corrupted (checksum mismatch)";
        case SNAPSHOT_ERR_COMPONENT:    return "Component save/load error";
        case SNAPSHOT_ERR_ALLOC:        return "Memory allocation error";
        case SNAPSHOT_ERR_EXTERNAL_REF: return "Missing external reference";
        case SNAPSHOT_ERR_NOT_PAUSED:   return "Emulator is not paused";
        case SNAPSHOT_ERR_TVSYS:        return "Incompatible TV system (PAL vs NTSC)";
        default:                        return "Unknown error";
    }
}


/* ========================================================================= */
/*                        Inicializace / ukončení                            */
/* ========================================================================= */

void snapshot_init(void)
{
    g_snapshot_registry_count = 0;

    /* Registrace všech handlerů */
    snap_z80_register();
    snap_memory_register();
    snap_gdg_register();
    snap_mzarch_register();
    snap_framebuffer_register();
    snap_vramctrl_register();

    snap_ctc8253_register();
    snap_pio8255_register();
    snap_psg_register();

    snap_pioz80_register();

    snap_audio_register();

    snap_cmt_register();

    snap_fdc_register();

    snap_qdisk_register();

    snap_ramdisk_register();

    snap_ide8_register();

#if 1 /* MEMEXT */
    snap_memext_register();
#endif

    snap_unicard_register();

    snap_hwconfig_register();

    fprintf(stderr, "Snapshot: registered %d component(s)\n", g_snapshot_registry_count);
}


void snapshot_exit(void)
{
    g_snapshot_registry_count = 0;
}


/* ========================================================================= */
/*                        Save / Load                                        */
/* ========================================================================= */

/**
 * Společná save logika - zápis všech komponent + screenshot + manifest
 *
 * Pracuje výhradně přes opaque @ref snapshot_io_t handle, takže nezávisí
 * na tom, zda je backed file nebo paměťovým bufferem. Volající (file
 * varianta nebo buffer varianta) si handle otevře a po návratu zavře sám.
 *
 * @param io Otevřený writer handle
 * @param description Popis snapshotu (může být NULL)
 * @return SNAPSHOT_OK při úspěchu
 *
 * @pre Emulátor musí být v pauze (kontroluje volající).
 * @pre @p io musí být platný writer handle.
 */
static en_SNAPSHOT_RESULT snapshot_save_through_io(snapshot_io_t *io,
                                                   const char *description)
{
    st_SNAPSHOT_CONTEXT ctx = {
        .io = io,
        .format_version = SNAPSHOT_FORMAT_VERSION,
        .architecture = g_mzhal.arch,
        .saving = true
    };

    en_SNAPSHOT_RESULT result;

    /* 1. Zavolat save handlery všech registrovaných komponent (priorita) */
    for (int i = 0; i < g_snapshot_registry_count; i++) {
        result = g_snapshot_registry[i].save_cb(&ctx);
        if (result != SNAPSHOT_OK) {
            SNAP_ERR("save", "error in component '%s': %s",
                     g_snapshot_registry[i].name,
                     snapshot_result_to_string(result));
            return result;
        }
    }

    /* 2. Uložit screenshot framebufferu (nepovinné) */
    result = snapshot_screenshot_save(io);
    if (result != SNAPSHOT_OK) {
        SNAP_WARN("save", "failed to save screenshot: %s",
                  snapshot_result_to_string(result));
        /* pokračujeme i při chybě */
    }

    /* 3. Vypočítat SHA-256 checksum ze zapsaných dat */
    char *checksum = snapshot_io_compute_checksum(io);

    /* 4. Uložit manifest (poslední, obsahuje checksum) */
    result = snapshot_save_manifest(&ctx, description, checksum);
    g_free(checksum);

    return result;
}


en_SNAPSHOT_RESULT snapshot_save(const char *filepath, const char *description)
{
    /* 1. Ověřit safe-point: emulátor v pauze NEBO dedikovaný snapshot
     * safe-point (0019). Druhý kanál umožňuje BP-action FWD_SNAPSHOT
     * z pokračujícího BP, aniž by sahal na paused (= actual_frames fix). */
    if (!EMULATOR_TEST_PAUSED && !EMULATOR_TEST_SNAPSHOT_SAFEPOINT)
        return SNAPSHOT_ERR_NOT_PAUSED;

    /* 2. Otevřít ZIP archiv pro zápis do TEMP souboru (atomicita) */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);
    snapshot_io_t *io = snapshot_io_open_write(tmp_path,
                                              g_snapshot_settings.compression_level);
    if (!io) return SNAPSHOT_ERR_IO;

    /* 3. Společná save logika */
    en_SNAPSHOT_RESULT result = snapshot_save_through_io(io, description);

    /* 4. Zavřít archiv */
    snapshot_io_close(io);

    if (result != SNAPSHOT_OK) {
        g_remove(tmp_path);
        return result;
    }

    /* 5. Atomicky přejmenovat tmp → cílový soubor */
    g_remove(filepath);  /* Windows vyžaduje smazat cíl před rename */
    if (g_rename(tmp_path, filepath) != 0) {
        g_remove(tmp_path);
        return SNAPSHOT_ERR_IO;
    }

    fprintf(stderr, "Snapshot: saved to '%s'\n", filepath);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Vlna 5 Commit 31 - SYS lifecycle event do Event Viewer. */
    eventlog_sys_event ( EVENTLOG_SYS_SNAPSHOT_SAVE, eventlog_filename_hash ( filepath ) );
#endif
    return SNAPSHOT_OK;
}


en_SNAPSHOT_RESULT snapshot_save_to_buffer(const char *description,
                                           uint8_t **out_data,
                                           size_t *out_size)
{
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;

    if (!out_data || !out_size) return SNAPSHOT_ERR_IO;

    /* 1. Ověřit, že je emulátor v pauze */
    if (!EMULATOR_TEST_PAUSED) return SNAPSHOT_ERR_NOT_PAUSED;

    /* 2. Otevřít ZIP archiv pro zápis do paměti */
    snapshot_io_t *io = snapshot_io_open_write_buffer(
            g_snapshot_settings.compression_level);
    if (!io) return SNAPSHOT_ERR_IO;

    /* 3. Společná save logika */
    en_SNAPSHOT_RESULT result = snapshot_save_through_io(io, description);
    if (result != SNAPSHOT_OK) {
        snapshot_io_close(io);
        return result;
    }

    /* 4. Extrakce hotových dat (close + alokace + memcpy) */
    result = snapshot_io_close_to_buffer(io, out_data, out_size);
    if (result != SNAPSHOT_OK) return result;

    fprintf(stderr, "Snapshot: saved to buffer (%zu B)\n", *out_size);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* SYS lifecycle event - bez filepath, použijeme nulový hash */
    eventlog_sys_event(EVENTLOG_SYS_SNAPSHOT_SAVE, 0);
#endif
    return SNAPSHOT_OK;
}


/**
 * Společná load logika - manifest + checksum + komponenty
 *
 * Pracuje výhradně přes opaque @ref snapshot_io_t handle. Volající
 * (file varianta nebo buffer varianta) si handle otevře a po návratu
 * zavře sám.
 *
 * @param io Otevřený reader handle
 * @return SNAPSHOT_OK při úspěchu
 *
 * @pre Emulátor musí být v pauze (kontroluje volající).
 * @pre @p io musí být platný reader handle.
 */
static en_SNAPSHOT_RESULT snapshot_load_through_io(snapshot_io_t *io)
{
    /* 1. Načíst a ověřit manifest */
    st_SNAPSHOT_METADATA metadata;
    memset(&metadata, 0, sizeof(metadata));
    en_SNAPSHOT_RESULT result = snapshot_load_manifest_from_io(io, &metadata);
    if (result != SNAPSHOT_OK) return result;

    /* 2. Ověřit kompatibilitu architektury */
    if (metadata.architecture != g_mzhal.arch) {
        SNAP_ERR("load", "incompatible architecture: snapshot=%d, emulator=%d",
                 metadata.architecture, g_mzhal.arch);
        return SNAPSHOT_ERR_ARCHITECTURE;
    }

    /* 2b. Ověřit kompatibilitu TV systému (rozhodnutí 2026-07-30:
     * snapshoty mz700-pal a mz700-ntsc jsou nekompatibilní). Legacy
     * snapshot bez pole <tvsys> (tvsys == 0) se posuzuje podle
     * historického defaultu své architektury. */
    {
        const int snap_tvsys = (metadata.tvsys != 0)
                ? metadata.tvsys
                : snapshot_legacy_default_tvsys(metadata.architecture);
        if (snap_tvsys != g_mzhal.tvsys) {
            SNAP_ERR("load", "incompatible TV system: snapshot=%d, emulator=%d (%s)",
                     metadata.tvsys, g_mzhal.tvsys, g_mzhal.tvsys_name);
            return SNAPSHOT_ERR_TVSYS;
        }
    }

    /* 3. Ověřit SHA-256 checksum */
    if (metadata.checksum[0] != '\0') {
        char *computed = snapshot_io_compute_checksum(io);
        if (computed && g_strcmp0(computed, metadata.checksum) != 0) {
            SNAP_ERR("load", "checksum mismatch - snapshot was modified or corrupted");
            g_free(computed);
            return SNAPSHOT_ERR_CORRUPTED;
        }
        g_free(computed);
    }

    /* 4. Vytvořit kontext */
    st_SNAPSHOT_CONTEXT ctx = {
        .io = io,
        .format_version = metadata.format_version,
        .architecture = metadata.architecture,
        .saving = false
    };

    /* 5. Zavolat load handlery všech registrovaných komponent (priorita) */
    for (int i = 0; i < g_snapshot_registry_count; i++) {
        result = g_snapshot_registry[i].load_cb(&ctx);
        if (result != SNAPSHOT_OK) {
            if (g_snapshot_registry[i].is_optional) {
                SNAP_WARN("load", "optional component '%s': %s",
                          g_snapshot_registry[i].name,
                          snapshot_result_to_string(result));
                continue;
            }
            SNAP_ERR("load", "error in component '%s': %s",
                     g_snapshot_registry[i].name,
                     snapshot_result_to_string(result));
            return result;
        }
    }

    /* Po obnově kompletního stavu bezpodmínečně přepočítat RAM fast-path
     * page-table. snap_gdg obnovuje regDMD surovým přiřazením (mimo
     * banking-switch cestu s rebuildem) a běží AŽ PO snap_memory, jehož
     * reconnect_ram tedy rebuildoval ještě nad starým DMD; snap_memext
     * (další reconnect) běžet nemusí. Bez tohoto by jádro až do nejbližšího
     * banking/DMD switche používalo tabulku z doby před loadem = tichý
     * přístup mimo obnovené mapování. */
    /* Per-arch post-load hook (mzhal 11e): MZ-800 rebuiduje RAM
     * fast-path, ostatní no-op - viz mzarch.h. */
    mzarch_snapshot_post_load();

    /* Po obnově celého stavu vynutit kompletní překreslení obrazovky z nově
     * načteného VRAM/GDG stavu. Snapshot se typicky načítá do pauzy, kdy se
     * framebuffer neaktualizuje, takže bez tohoto refreshe by obraz na ploše
     * zůstal starý. Core funkce mzarch_forced_full_screen_refresh() funguje
     * i v buildu bez debuggeru (dřív byl ekvivalent jen v debuggeru přes
     * debugger_forced_screen_update). Společná cesta pro file i buffer load
     * (quickload) - pokrývá všechny varianty jedním voláním. */
    mzarch_forced_full_screen_refresh();

    return SNAPSHOT_OK;
}


en_SNAPSHOT_RESULT snapshot_load(const char *filepath)
{
    /* 1. Ověřit, že je emulátor v pauze */
    if (!EMULATOR_TEST_PAUSED) return SNAPSHOT_ERR_NOT_PAUSED;

    /* 2. Otevřít ZIP archiv pro čtení */
    snapshot_io_t *io = snapshot_io_open_read(filepath);
    if (!io) return SNAPSHOT_ERR_IO;

    /* 3. Společná load logika */
    en_SNAPSHOT_RESULT result = snapshot_load_through_io(io);

    /* 4. Zavřít archiv */
    snapshot_io_close(io);

    if (result == SNAPSHOT_OK) {
        fprintf(stderr, "Snapshot: loaded from '%s'\n", filepath);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* Vlna 5 Commit 31 - SYS lifecycle event do Event Viewer. */
        eventlog_sys_event ( EVENTLOG_SYS_SNAPSHOT_LOAD, eventlog_filename_hash ( filepath ) );
#endif
    }
    return result;
}


en_SNAPSHOT_RESULT snapshot_load_from_buffer(const uint8_t *data, size_t size)
{
    if (!data || size == 0) return SNAPSHOT_ERR_IO;

    /* 1. Ověřit, že je emulátor v pauze */
    if (!EMULATOR_TEST_PAUSED) return SNAPSHOT_ERR_NOT_PAUSED;

    /* 2. Otevřít ZIP archiv pro čtení z paměti */
    snapshot_io_t *io = snapshot_io_open_read_buffer(data, size);
    if (!io) return SNAPSHOT_ERR_ZIP;

    /* 3. Společná load logika */
    en_SNAPSHOT_RESULT result = snapshot_load_through_io(io);

    /* 4. Zavřít archiv */
    snapshot_io_close(io);

    if (result == SNAPSHOT_OK) {
        fprintf(stderr, "Snapshot: loaded from buffer (%zu B)\n", size);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        eventlog_sys_event(EVENTLOG_SYS_SNAPSHOT_LOAD, 0);
#endif
    }
    return result;
}


en_SNAPSHOT_RESULT snapshot_read_metadata(const char *filepath,
                                         st_SNAPSHOT_METADATA *metadata)
{
    snapshot_io_t *io = snapshot_io_open_read(filepath);
    if (!io) return SNAPSHOT_ERR_IO;

    memset(metadata, 0, sizeof(*metadata));
    en_SNAPSHOT_RESULT result = snapshot_load_manifest_from_io(io, metadata);

    snapshot_io_close(io);
    return result;
}


en_SNAPSHOT_RESULT snapshot_extract_screenshot(const char *filepath,
                                              uint8_t **png_data,
                                              size_t *png_size)
{
    snapshot_io_t *io = snapshot_io_open_read(filepath);
    if (!io) return SNAPSHOT_ERR_IO;

    en_SNAPSHOT_RESULT result;

    if (!snapshot_io_entry_exists(io, "screenshot.png")) {
        snapshot_io_close(io);
        return SNAPSHOT_ERR_MISSING_FILE;
    }

    result = snapshot_io_read_bin(io, "screenshot.png", png_data, png_size);
    snapshot_io_close(io);
    return result;
}


en_SNAPSHOT_RESULT snapshot_recalculate_checksum(const char *filepath)
{
    /* TODO: implementovat v pozdější fázi — vyžaduje otevření, přepočet a
     * znovuzápis manifestu, což je netriviální s minizip-ng */
    (void)filepath;
    SNAP_WARN("recalc", "checksum recompute not yet implemented");
    return SNAPSHOT_ERR_IO;
}
