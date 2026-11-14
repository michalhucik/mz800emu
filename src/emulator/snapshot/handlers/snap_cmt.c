/**
 * @file snap_cmt.c
 * @brief Snapshot handler: CMT kazeta - uložení a načtení stavu kazetového magnetofonu
 *
 * Ukládá stav transportu (state, paused, při pauze uživatele navíc
 * user_paused a motor_off, playsts, časy), nastavení a cestu
 * vložené pásky (last_filename) do devices/cmt.xml. Je-li vložená páska
 * rozšíření cmt_tmz, přidá element <tape> s blokem a pozicí, rychlostmi
 * bloků, výchozí rychlostí a pulzy Virtual CMT a stavem playeru
 * postupného přehrávání (smyčky, volání, úroveň signálu). Obsah pásky se
 * do snapshotu vkládá jen u pásky Modified nebo bez cesty (element
 * <image>, base64 obrazu tzx_save(), nejvýš SNAP_CMT_EMBED_MAX bajtů
 * před kódováním; větší = varování "Snapshot: unsaved tape is too large
 * to embed" a element <image_omitted>); jinak jen cesta. Snapshoty
 * záznamu vstupů obraz nevkládají (snap_cmt_set_embed_tape()), pásku
 * nasazuje záznam sám. Snapshot pořízený během přehrání
 * záznamu vstupů uloží místo dočasné kopie pásky cestu původního souboru
 * při záznamu (replay_media_cmt_snapshot_path()).
 *
 * Načtení snapshotu s <tape>: je-li stejná páska (cesta a počet bloků)
 * už vložená, použije se; jinak se soubor otevře
 * (cmt_open_file_by_extension()). Obnoví se rychlosti bloků přes
 * dosavadní funkce cmt_tape_set_block_cmt_speed() /
 * cmt_tape_set_block_pulses() / cmt_tape_change_block_speed(), blok se
 * otevře přímo (cmttmz_seek_block()), nastaví se stav playeru
 * (cmttmz_set_player_state(), s ověřením) a pozice: ve STOP a v pauze přes
 * cmt_tape_set_position(), při PLAY se start_time obnoví přímo z XML
 * (časová základna je takt GDG, který obnovuje snapshot GDG, a
 * cmt_tape_set_position() při PLAY bez pauzy nastavení odmítá).
 * Chybí-li soubor, transport skončí ve STOP bez pásky s varováním
 * "Snapshot tape file not found: <cesta>"; neodpovídá-li soubor
 * (počet nebo druh bloků), obnoví se rychlosti bloků, které odpovídají
 * indexem i druhem, páska zůstane vložená ve STOP na prvním bloku a vypíše
 * se "Snapshot tape does not match file: <cesta>". Páska bez otevřeného
 * bloku při uložení (dohraná bez převinutí, jen řídicí bloky) se obnoví
 * jako po otevření, ve STOP, bez varování.
 *
 * Stav CMT hacku nad páskou (hlavička načtená bez těla, konec pásky;
 * cmthack_tape_state_get()) se ukládá do <tape> jako element <cmthack>
 * a obnoví se po obnovení pásky (cmthack_tape_state_set(), s ověřením
 * hodnot; neplatný stav = varování "Snapshot CMT hack tape state is
 * invalid, ignored"). Načtení snapshotu stav hacku nejdřív vždy vynuluje:
 * bez <cmthack>, bez <tape> nebo bez obnovené pásky začíná hack od nuly.
 *
 * Cesta pásky pochází ze snapshotu (soubor .mzs je vstup od uživatele nebo
 * cizí soubor) a otevírá se tak, jak je, bez kanonizace. Síťová cesta
 * a cesta zařízení (začíná \\ nebo //: UNC, \\?\, \\.\) se automaticky
 * neotevírá: loader se zachová jako u chybějícího souboru s varováním
 * "Snapshot tape on a network path is not opened automatically: <cesta>",
 * ledaže je vložená páska s přesně touto cestou (pak se použije). Místní
 * cesty a namapovaná písmena disků se otevírají. Ostatní handlery média z cesty ve snapshotu
 * neotevírají (FDC, QD a IDE8 cestu jen zapíšou nebo ohlásí, Unicard
 * mění pracovní adresář jen uvnitř SD kořene).
 *
 * Načtení s <image>: obraz má přednost před cestou. Páska se vloží
 * z obrazu (cmt_open_image()) bez cesty, jako neuložená páska bez jména:
 * cestu z nedůvěryhodného snapshotu by převzal Save tape. Jméno souboru
 * ze snapshotu jde jen do zprávy "Unsaved tape restored from the snapshot
 * (was: <jméno>)". Je-li ale v kazetofonu páska s přesně touto cestou
 * (snapshot téže relace, Quick Save -> Quick Load), cesta zůstane. Páska je vždy Modified (element <modified> se při
 * načtení nepoužívá - snapshot je nedůvěryhodný vstup); síťová cesta nebo
 * cesta zařízení dá navíc varování "Snapshot tape path on a network
 * location was dropped". Bloky 0x18 importované z WAV (typ WAV v <block_N>/
 * <type>) zůstanou typu WAV, přepínač polarity na ně dál působí; starší
 * snapshot bez typu = blok TZX. Příznaky neuloženo bloků (<block_N>/
 * <unsaved>, zapisují se jen s obrazem) se obnoví jen u pásky, která
 * si cestu ponechala (snap_cmt_restore_unsaved()); páska bez cesty je celá
 * neuložená, snapshot bez elementu = všechny bloky uložené (jako dřív).
 * Soubor na disku se nečte. Obraz je nedůvěryhodný vstup: délka base64
 * nad mez, znak mimo abecedu base64 nebo obraz, který nejde načíst =
 * varování "Snapshot embedded tape is invalid: <cesta>" a postup jako
 * u chybějícího souboru (STOP bez pásky). <image_omitted> = varování
 * "Snapshot tape was too large to embed: <cesta>", také bez pásky.
 * Varování importu obrazu jdou do varování snapshotu ("Snapshot embedded
 * tape: <text>"). Mez 16 MB se kontroluje na textu base64 až po jeho
 * přečtení z XML (snapshot_xml_read_string() kopíruje celý text uzlu;
 * čtečka XML délku předem nevrací); horní mez kopie dává mez velikosti
 * položky archivu snapshotu.
 *
 * Snapshoty záznamu vstupů (bez obrazu) nesou <tape> i u pásky bez cesty;
 * load s vloženou páskou (keep-inserted) ji použije a obnoví blok, pozici,
 * rychlosti, stav playeru a stav CMT hacku.

 *
 * Nahrávání (RECORD): buffer hran se do souboru snapshotu neukládá.
 * Načtení snapshotu nejdřív zahodí probíhající nahrávání
 * (cmt_record_discard(), snap_cmt_drop_recording()); když devices/cmt.xml
 * chybí nebo je vadný (komponenta je volitelná, stroj se načte i tak),
 * transport skončí ve STOP (snap_cmt_transport_rest()). Snapshot pořízený během nahrávání se načte
 * s transportem ve STOP a varováním "Snapshot was taken while recording;
 * the recording was not restored"; výjimkou je checkpoint záznamu vstupů,
 * který kopii bufferu předá snap_cmt_set_record_restore().
 *
 * Snapshot bez <tape> (starší verze, nebo bez pásky) se načte jako dřív:
 * páska se neotevírá; pokud obnovený stav transportu neodpovídá aktuálně
 * vložené pásce, loader přepne transport do STOP (cmt_sanitize_state).
 * Starší emulátor element <tape> ignoruje: čtečka XML hledá potomky podle
 * jména (snapshot_xml_read_* / snapshot_xml_enter_element), neznámé
 * elementy se nečtou.
 *
 * Volby cpu_boost (automatická MAX SPEED během přehrávání), polarity
 * (zadní přepínač polarity čtení), mz_cmtspeed (výchozí rychlost pásky)
 * a mzfsize_check jsou uživatelské preference (cfg sekce [CMT]), ne stav
 * stroje: do snapshotu se dál zapisují (kvůli kompatibilitě se staršími
 * verzemi emulátoru, které je čtou), při načtení se ale ignorují a platí
 * aktuální volby uživatele. Obnova rychlostí bloků příznak Modified pásky
 * nemění (změna rychlosti bloku jinak pásku označí jako Modified).
 * Bloky s výchozí rychlostí, jejichž výchozí
 * rychlost ve snapshotu je jiná než aktuální volba uživatele, dostanou
 * rychlost ze snapshotu jako vlastní (SET): snapshot nepřepíše volbu
 * výchozí rychlosti, signál bloku je stejný jako při uložení, ale blok
 * pak už nesleduje pozdější změny výchozí rychlosti.
 *
 * Načtení je tiché (cmt_quiet_begin() / cmt_quiet_end()): zastavení,
 * vysunutí a otevření pásky během něj neodpalují BP eventy cmt:mstate
 * a cmt:state_change a nevypisují "Virtual CMT is stopped" - v emulovaném
 * stroji k přechodu nedošlo. Varování (SNAP_WARN) se vypisují dál.
 *
 * Záznam vstupů (replay.c) nasazuje pásku z vlastní kopie a na dobu
 * načtení svých snapshotů zapne snap_cmt_set_keep_inserted_tape(): load
 * pak použije vloženou pásku (jinou cestu nekontroluje) a bez vložené
 * pásky soubor neotevírá, transport srovná cmt_sanitize_state().
 *
 * Vlákna: save i load běží ve vlákně emulátoru (snapshot_request_poll()
 * v paused smyčce mzarch.c, dbgapi SNAPSHOT_*, checkpoint záznamu vstupů).
 * Load mění vloženou pásku pod device_state_lock(), protože okna kazety
 * v UI vlákně ji čtou pod týmž zámkem.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "device_request.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_record.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mztape/mztape.h"
#include "libs/tmz/tmz_player.h"
#include "hw-generic/cmt/cmthack.h"
#include "replay_media.h"


/**
 * @brief Je cesta síťová nebo zařízení (UNC, \\?\, \\.\, //server)?
 *
 * Takovou cestu loader neotevírá automaticky: snapshot je soubor od
 * uživatele nebo cizí, otevření síťové cesty by mohlo kontaktovat cizí
 * server (rozhodnutí kontroléru; místní cesty a namapovaná písmena disků
 * se otevírají). Kontrola je čistě textová, bez přístupu k souboru.
 *
 * @param path Cesta (UTF-8).
 * @return true pro cestu začínající dvěma lomítky (\\ nebo //).
 */
static bool snap_cmt_is_network_path(const char *path)
{
    return (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
}


/** @brief Nejvyšší počet bloků, které loader ze snapshotu přijme (ochrana proti poškozenému XML). */
#define SNAP_CMT_MAX_BLOCKS 100000


/**
 * @brief true = load použije vloženou pásku a soubor z cesty ve snapshotu neotevírá.
 *
 * Nastavuje záznam vstupů (snap_cmt_set_keep_inserted_tape()) na dobu
 * načtení svých snapshotů: pásku nasazuje z kopie v záznamu (dočasný
 * soubor s jinou cestou) a soubor na disku se číst nesmí.
 */
static bool s_keep_inserted_tape = false;


void snap_cmt_set_keep_inserted_tape(bool keep)
{
    s_keep_inserted_tape = keep;
}


/**
 * @brief Nejvyšší velikost obrazu pásky vloženého do snapshotu (bajty před kódováním base64).
 *
 * Mez chrání velikost snapshotu (base64 je o třetinu větší, archiv má mez
 * SNAPSHOT_MAX_ARCHIVE_SIZE) i loader (alokace úměrná vstupu).
 */
#define SNAP_CMT_EMBED_MAX (16u * 1024u * 1024u)

/** @brief Účinná mez vloženého obrazu (SNAP_CMT_EMBED_MAX, v testech menší). */
static size_t s_embed_max = SNAP_CMT_EMBED_MAX;

/** @brief false = save obraz pásky nevkládá (snapshoty záznamu vstupů). */
static bool s_embed_tape = true;

/** @brief Kopie nahrávání, kterou load obnoví místo zahození (checkpoint záznamu vstupů), jinak NULL. */
static const st_CMT_RECORD_CKPT *s_record_restore = NULL;


void snap_cmt_set_embed_tape(bool embed)
{
    s_embed_tape = embed;
}


void snap_cmt_set_record_restore(const st_CMT_RECORD_CKPT *rc)
{
    s_record_restore = rc;
}


#ifdef MZTEST_HEADLESS
void snap_cmt_test_set_embed_limit(size_t limit)
{
    s_embed_max = limit ? limit : SNAP_CMT_EMBED_MAX;
}


/** @brief Jen pro testy: poškození vloženého obrazu při save (0 = žádné). */
static int s_test_corrupt_image = 0;


void snap_cmt_test_corrupt_image(int mode)
{
    s_test_corrupt_image = mode;
}


/** @brief Jen pro testy: true = save element <tape> nezapíše (snapshot jako z verze 2.0.6). */
static bool s_test_omit_tape = false;


void snap_cmt_test_omit_tape(bool omit)
{
    s_test_omit_tape = omit;
}


/** @brief Jen pro testy: true = save nezapíše příznaky neuloženo bloků (snapshot jako ze staršího emulátoru). */
static bool s_test_omit_unsaved = false;


void snap_cmt_test_omit_unsaved(bool omit)
{
    s_test_omit_unsaved = omit;
}


/** @brief Jen pro testy: varování od posledního vyprázdnění (snap_cmt_test_warnings()). */

static GString *s_test_warnings = NULL;


const char *snap_cmt_test_warnings(void)
{
    return s_test_warnings ? s_test_warnings->str : "";
}


void snap_cmt_test_clear_warnings(void)
{
    if (s_test_warnings)
        g_string_truncate(s_test_warnings, 0);
}


/**
 * @brief Jen pro testy: zapamatuje text varování (printf).
 * @param fmt Formát. @param ... Argumenty.
 */
static void snap_cmt_test_note_warning(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void snap_cmt_test_note_warning(const char *fmt, ...)
{
    if (!s_test_warnings)
        s_test_warnings = g_string_new("");
    if (s_test_warnings->len)
        g_string_append_c(s_test_warnings, '\n');
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(s_test_warnings, fmt, ap);
    va_end(ap);
}

/** @brief Varování komponenty "cmt" (SNAP_WARN), v testech navíc zapamatované. */
#define SNAP_CMT_WARN(fmt, ...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        SNAP_WARN("cmt", fmt, ##__VA_ARGS__);                                                                          \
        snap_cmt_test_note_warning(fmt, ##__VA_ARGS__);                                                                \
    } while (0)
#else
/** @brief Varování komponenty "cmt" (SNAP_WARN). */
#define SNAP_CMT_WARN(fmt, ...) SNAP_WARN("cmt", fmt, ##__VA_ARGS__)
#endif


/**
 * @brief Vloží obraz vložené pásky do <tape> (element <image> base64, nebo <image_omitted>).
 * @param w Writer uvnitř <tape>.
 */
static void snap_cmt_write_image(snapshot_xml_writer_t *w)
{
    uint8_t *img = NULL;
    size_t size = 0;
    if (EXIT_SUCCESS != cmttmz_save_image(&img, &size))
    {
        SNAP_CMT_WARN("Snapshot: unsaved tape cannot be embedded");
        snapshot_xml_write_bool(w, "image_omitted", true);
        return;
    };
    if (size > s_embed_max)
    {
        SNAP_CMT_WARN("Snapshot: unsaved tape is too large to embed");
        snapshot_xml_write_bool(w, "image_omitted", true);
        g_free(img);
        return;
    };
    bool modified = cmttmz_is_modified() != 0;
#ifdef MZTEST_HEADLESS
    if (s_test_corrupt_image == 3)
        modified = false; /* snapshot z cizího zdroje: load musí Modified vynutit */
#endif
    snapshot_xml_write_bool(w, "modified", modified);
    gchar *b64 = g_base64_encode(img, size);
#ifdef MZTEST_HEADLESS
    if (s_test_corrupt_image == 1)
    {
        b64[0] = '!'; /* znak mimo abecedu base64 */
    }
    else if (s_test_corrupt_image == 2)
    {
        g_free(b64);
        b64 = g_base64_encode((const guchar *)"not a tape image", 16); /* platný base64, vadný obraz */
    };
#endif
    snapshot_xml_write_string(w, "image", b64);
    g_free(b64);
    g_free(img);
}


/**
 * @brief Dekóduje obraz pásky z base64 se všemi kontrolami nedůvěryhodného vstupu.
 *
 * Délka textu nejvýš 4 * ceil(s_embed_max / 3), násobek 4, jen znaky
 * abecedy base64 (A-Z a-z 0-9 + /) a nejvýš dvě '=' na konci; dekódovaná
 * délka nejvýš s_embed_max a nenulová.
 *
 * @param b64 Text ze snapshotu. @param[out] size Délka obrazu.
 * @return Obraz (g_free) nebo NULL, když text kontrolou neprojde.
 */
static uint8_t *snap_cmt_decode_image(const char *b64, size_t *size)
{
    *size = 0;
    size_t len = strlen(b64);
    size_t max_len = ((s_embed_max + 2u) / 3u) * 4u;
    if (len == 0 || len > max_len || (len % 4u) != 0)
        return NULL;
    for (size_t i = 0; i < len; i++)
    {
        char ch = b64[i];
        bool alpha = g_ascii_isalnum(ch) || ch == '+' || ch == '/';
        bool pad = (ch == '=') && (i >= len - 2u) && ((i == len - 1u) || (b64[len - 1u] == '='));
        if (!alpha && !pad)
            return NULL;
    };
    gsize out = 0;
    guchar *data = g_base64_decode(b64, &out);
    if (!data || out == 0 || out > s_embed_max)
    {
        g_free(data);
        return NULL;
    };
    *size = out;
    return data;
}


/**
 * @brief Převede délky pulzů v sekundách na celé µs (LONG H, LONG L, SHORT H, SHORT L).
 * @param p Délky pulzů. @param[out] us Délky v µs.
 */
static void snap_cmt_pulses_to_us(const st_MZTAPE_PULSES_LENGTH *p, unsigned us[4])
{
    const double s[4] = {p->long_pulse.high, p->long_pulse.low, p->short_pulse.high, p->short_pulse.low};
    for (int i = 0; i < 4; i++)
        us[i] = (s[i] > 0.0) ? (unsigned)lround(s[i] * 1e6) : 0;
}


/**
 * @brief Zapíše element se 4 délkami pulzů v µs (lh, ll, sh, sl).
 * @param w Writer. @param name Jméno elementu. @param p Délky pulzů.
 */
static void snap_cmt_write_pulses(snapshot_xml_writer_t *w, const char *name, const st_MZTAPE_PULSES_LENGTH *p)
{
    unsigned us[4];
    snap_cmt_pulses_to_us(p, us);
    snapshot_xml_open_element(w, name);
    snapshot_xml_write_uint(w, "lh", us[0]);
    snapshot_xml_write_uint(w, "ll", us[1]);
    snapshot_xml_write_uint(w, "sh", us[2]);
    snapshot_xml_write_uint(w, "sl", us[3]);
    snapshot_xml_close_element(w);
}


/**
 * @brief Přečte element se 4 délkami pulzů v µs.
 * @param r Reader. @param name Jméno elementu. @param[out] us Délky v µs.
 * @return true, pokud element a všechny 4 hodnoty existují.
 */
static bool snap_cmt_read_pulses(snapshot_xml_reader_t *r, const char *name, unsigned us[4])
{
    if (!snapshot_xml_enter_element(r, name))
        return false;
    bool ok = snapshot_xml_read_uint(r, "lh", &us[0]) && snapshot_xml_read_uint(r, "ll", &us[1])
              && snapshot_xml_read_uint(r, "sh", &us[2]) && snapshot_xml_read_uint(r, "sl", &us[3]);
    snapshot_xml_leave_element(r);
    return ok;
}


/**
 * @brief Zapíše element <tape>: blok, pozice, rychlosti bloků, stav playeru.
 *
 * Volá se jen s vloženou páskou rozšíření cmt_tmz a s neprázdnou cestou
 * nebo s @p embed. S obrazem pásky nese každý <block_N> navíc příznak
 * neuloženo (<unsaved>, cmttmz_block_is_unsaved(); starší
 * emulátor ho nečte).
 *
 * @param w Writer (uvnitř <cmt_state>).
 * @param embed true = vložit obraz pásky (páska Modified nebo bez cesty).
 */
static void snap_cmt_save_tape(snapshot_xml_writer_t *w, bool embed)
{
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    int count = cmtext_container_get_count_blocks(c);
    int block_id = 0;
    uint64_t ticks = 0;
    if (EXIT_SUCCESS != cmt_tape_get_position(&block_id, &ticks))
    {
        block_id = -1; /* páska bez otevřeného bloku */
        ticks = 0;
    };

    snapshot_xml_open_element(w, "tape");
    snapshot_xml_write_int(w, "block_count", count);
    snapshot_xml_write_int(w, "block_id", block_id);
    snapshot_xml_write_uint64(w, "ticks_in_block", ticks);
    snapshot_xml_write_int(w, "default_speed", (int)g_cmt.mz_cmtspeed);
    snap_cmt_write_pulses(w, "default_pulses_us", &g_cmt.mz_custom_pulses);

    bool write_unsaved = embed;
#ifdef MZTEST_HEADLESS
    if (s_test_omit_unsaved)
        write_unsaved = false;
#endif

    for (int i = 0; i < count; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "block_%d", i);
        snapshot_xml_open_element(w, name);
        snapshot_xml_write_int(w, "type", (int)cmtext_container_get_block_type(c, i));
        snapshot_xml_write_int(w, "blspeed", (int)cmtext_container_get_block_speed(c, i));
        snapshot_xml_write_int(w, "speed", (int)cmtext_container_get_block_cmt_speed(c, i));
        st_MZTAPE_PULSES_LENGTH p;
        if (EXIT_SUCCESS == cmtext_container_get_block_pulses(c, i, &p))
            snap_cmt_write_pulses(w, "pulses_us", &p);
        /* příznak neuloženo jen s obrazem pásky (páska z cesty je celá uložená) */
        if (write_unsaved)
            snapshot_xml_write_bool(w, "unsaved", cmttmz_block_is_unsaved(i) != 0);
        snapshot_xml_close_element(w);
    };

    st_TMZ_PLAYER_STATE st;
    if (EXIT_SUCCESS == cmttmz_get_player_state(&st))
    {
        snapshot_xml_open_element(w, "player");
        snapshot_xml_write_uint(w, "current_block", st.current_block);
        snapshot_xml_write_uint(w, "last_played_block", st.last_played_block);
        snapshot_xml_write_int(w, "loop_depth", st.loop_depth);
        for (int i = 0; i < st.loop_depth && i < TMZ_PLAYER_MAX_LOOP_DEPTH; i++)
        {
            char name[32];
            snprintf(name, sizeof(name), "loop_%d", i);
            snapshot_xml_open_element(w, name);
            snapshot_xml_write_uint(w, "start_block", st.loop_stack[i].start_block);
            snapshot_xml_write_uint(w, "remaining", st.loop_stack[i].remaining);
            snapshot_xml_close_element(w);
        };
        snapshot_xml_write_int(w, "call_depth", st.call_depth);
        for (int i = 0; i < st.call_depth && i < TMZ_PLAYER_MAX_CALL_DEPTH; i++)
        {
            char name[32];
            snprintf(name, sizeof(name), "call_%d", i);
            snapshot_xml_open_element(w, name);
            snapshot_xml_write_uint(w, "call_block", st.call_stack[i].call_block);
            snapshot_xml_write_uint(w, "current_offset_idx", st.call_stack[i].current_offset_idx);
            snapshot_xml_write_uint(w, "total_offsets", st.call_stack[i].total_offsets);
            snapshot_xml_close_element(w);
        };
        snapshot_xml_write_bool(w, "stopped", st.stopped);
        snapshot_xml_write_bool(w, "finished", st.finished);
        snapshot_xml_write_uint(w, "signal_level", st.signal_level);
        snapshot_xml_write_uint(w, "mz_cpu_clock", st.config.mz_cpu_clock);
        snapshot_xml_close_element(w); /* player */
    };

    /* stav CMT hacku mezi pastmi ROM, vůči bloku pásky */
    st_CMTHACK_TAPE_STATE hs;
    if (cmthack_tape_state_get(&hs))
    {
        snapshot_xml_open_element(w, "cmthack");
        snapshot_xml_write_bool(w, "pending", hs.pending);
        snapshot_xml_write_int(w, "pending_block", hs.pending_block);
        snapshot_xml_write_bool(w, "pending_moved", hs.pending_moved);
        snapshot_xml_write_bool(w, "end", hs.end);
        snapshot_xml_close_element(w);
    };

    if (embed)
        snap_cmt_write_image(w);

    snapshot_xml_close_element(w); /* tape */
}


/**
 * @brief Uloží stav CMT (g_cmt) do devices/cmt.xml.
 *
 * @param ctx Kontext snapshotu (otevřený pro zápis).
 * @return SNAPSHOT_OK, nebo chyba zápisu z snapshot_io_write_xml.
 *
 * @pre Emulace je pozastavená; vlákno emulátoru.
 */
static en_SNAPSHOT_RESULT snap_cmt_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "cmt_state");

    /* Základní stavové proměnné */
    snapshot_xml_write_int(w, "state", (int)g_cmt.state);
    snapshot_xml_write_int(w, "paused", g_cmt.paused);
    /* Pauza uživatele a motor za ní: jen když je pauza uživatele zapnutá
     * (jinak motor stojí == paused); snapshot bez pauzy uživatele je tak
     * stejný jako dřív a starší snapshot se načte s user_paused = 0. */
    if (g_cmt.user_paused)
    {
        snapshot_xml_write_int(w, "user_paused", 1);
        snapshot_xml_write_int(w, "motor_off", g_cmt.motor_off ? 1 : 0);
    };
    snapshot_xml_write_int(w, "polarity", (int)g_cmt.polarity);
    snapshot_xml_write_int(w, "mz_cmtspeed", (int)g_cmt.mz_cmtspeed);
    snapshot_xml_write_int(w, "output", g_cmt.output);
    /* Stav přehrávání bloku (BODY/PAUSE/STOP). Bez něj by po načtení
     * snapshotu do procesu ve stavu STOP cmt_update_output nedetekoval
     * konec bloku (porovnává nový playsts s uloženým). */
    snapshot_xml_write_int(w, "playsts", (int)g_cmt.playsts);

    /* Časové údaje */
    snapshot_xml_write_uint64(w, "start_time", g_cmt.start_time);
    snapshot_xml_write_uint64(w, "paused_time", g_cmt.paused_time);
    snapshot_xml_write_uint64(w, "recording_last_event", g_cmt.recording_last_event);

    /* Nastavení. cpu_boost a mzfsize_check (a výše polarity, mz_cmtspeed)
     * se zapisují jen kvůli kompatibilitě se staršími verzemi emulátoru,
     * které je při načtení obnovují; snap_cmt_load je ignoruje (uživatelské
     * preference). */
    snapshot_xml_write_int(w, "cpu_boost", (int)g_cmt.cpu_boost);
    snapshot_xml_write_int(w, "mzfsize_check", (int)g_cmt.mzfsize_check);
    snapshot_xml_write_int(w, "recording_to_stream", g_cmt.recording_to_stream);

    /* Cesta vložené pásky ("" bez pásky); při přehrání záznamu vstupů cesta
     * původního souboru místo dočasné kopie (replay_media_cmt_snapshot_path) */
    const char *tape_path = replay_media_cmt_snapshot_path(g_cmt.last_filename);
    snapshot_xml_write_string(w, "last_filename", tape_path ? tape_path : "");

    /* Páska rozšíření cmt_tmz: blok, pozice, rychlosti, stav playeru vždy
     * (i u pásky bez cesty: checkpointy záznamu vstupů pásku nevkládají,
     * load použije vloženou); páska Modified nebo bez cesty navíc s obrazem
     * (mimo snapshoty záznamu vstupů) */
    bool write_tape = true;
#ifdef MZTEST_HEADLESS
    write_tape = !s_test_omit_tape;
#endif
    if (write_tape && CMT_TEST_FILLED && (g_cmt.ext == g_cmt_tmz_extension) && cmtext_get_container(g_cmt.ext))
    {
        bool has_path = tape_path && tape_path[0];
        bool embed = s_embed_tape && (cmttmz_is_modified() || !has_path);
        snap_cmt_save_tape(w, embed);
    };

    snapshot_xml_close_element(w); /* cmt_state */

    char *xml = snapshot_xml_writer_finish(w);
#ifdef MZTEST_HEADLESS
    if (s_test_corrupt_image == 5)
    {
        /* snapshot bez komponenty CMT (devices/cmt.xml chybí) */
        g_free(xml);
        return SNAPSHOT_OK;
    };
    if (s_test_corrupt_image == 4)
    {
        /* platné XML bez cmt_state: load ho odmítne (SNAPSHOT_ERR_XML_PARSE) */
        g_free(xml);
        xml = g_strdup("<?xml version=\"1.0\" encoding=\"UTF-8\"?>" "<other_state/>");
    };
#endif
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/cmt.xml", xml);
    g_free(xml);

    return res;
}


/**
 * @brief Stav transportu přečtený ze snapshotu (nastaví se do g_cmt až po obnově pásky).
 */
typedef struct st_SNAP_CMT_TRANSPORT {
    bool has_state;          /**< element state existuje */
    int state;               /**< en_CMT_STATE */
    bool has_paused;         /**< element paused existuje */
    int paused;              /**< g_cmt.paused */
    int user_paused;         /**< g_cmt.user_paused (chybí-li element, 0) */
    int motor_off;           /**< g_cmt.motor_off při user_paused (chybí-li element, 0) */
    bool has_output;         /**< element output existuje */
    int output;              /**< g_cmt.output */
    int playsts;             /**< en_CMTEXT_BLOCK_PLAYSTS (odvozený, když chybí) */
    bool has_start_time;     /**< element start_time existuje */
    uint64_t start_time;     /**< g_cmt.start_time */
    bool has_paused_time;    /**< element paused_time existuje */
    uint64_t paused_time;    /**< g_cmt.paused_time */
    bool has_rec_last;       /**< element recording_last_event existuje */
    uint64_t rec_last;       /**< g_cmt.recording_last_event */
    bool has_rec_stream;     /**< element recording_to_stream existuje */
    int rec_stream;          /**< g_cmt.recording_to_stream */
} st_SNAP_CMT_TRANSPORT;


/**
 * @brief Zapíše přečtený stav transportu do g_cmt.
 *
 * Pole s příznakem has_* jen tehdy, když v XML byla; zdroje pauzy
 * (user_paused, motor_off) vždy: bez elementů je pauza uživatele 0 a stav
 * motoru se odvodí z paused (starší snapshot).
 *
 * @param t Stav transportu.
 */
static void snap_cmt_apply_transport(const st_SNAP_CMT_TRANSPORT *t)
{
    if (t->has_state)
        g_cmt.state = (en_CMT_STATE)t->state;
    if (t->has_paused)
        g_cmt.paused = t->paused;
    /* pauza uživatele jen s paused (invariant st_CMT); bez ní motor_off
     * odvodí cmt_sanitize_state() z paused */
    g_cmt.user_paused = (t->user_paused && g_cmt.paused) ? 1 : 0;
    g_cmt.motor_off = g_cmt.user_paused ? (t->motor_off ? 1 : 0) : (g_cmt.paused ? 1 : 0);
    if (t->has_output)
        g_cmt.output = t->output;
    g_cmt.playsts = (en_CMTEXT_BLOCK_PLAYSTS)t->playsts;
    if (t->has_start_time)
        g_cmt.start_time = t->start_time;
    if (t->has_paused_time)
        g_cmt.paused_time = t->paused_time;
    if (t->has_rec_last)
        g_cmt.recording_last_event = t->rec_last;
    if (t->has_rec_stream)
        g_cmt.recording_to_stream = t->rec_stream;
}


/**
 * @brief Přepne přečtený stav transportu na klidový STOP (páska nejde obnovit celá).
 * @param[in,out] t Stav transportu.
 */
static void snap_cmt_transport_to_stop(st_SNAP_CMT_TRANSPORT *t)
{
    t->has_state = t->has_paused = t->has_output = t->has_start_time = t->has_paused_time = true;
    t->state = (int)CMT_STATE_STOP;
    t->paused = 0;
    t->user_paused = 0;
    t->motor_off = 0;
    t->output = 0;
    t->playsts = (int)CMTEXT_BLOCK_PLAYSTS_STOP;
    t->start_time = 0;
    t->paused_time = 0;
}


/**
 * @brief Obnoví rychlost jednoho bloku přes funkce rychlosti Virtual CMT.
 *
 * Blok s výchozí rychlostí zůstane DEFAULT, pokud výchozí rychlost ve
 * snapshotu odpovídá aktuální volbě uživatele (g_cmt.mz_cmtspeed a pulzy
 * v celých µs); jinak dostane rychlost ze snapshotu jako vlastní (SET).
 * Snapshot tak nepřepíše volbu výchozí rychlosti uživatele (g_cmt.mz_cmtspeed
 * a mz_custom_pulses zůstávají), signál bloku je stejný jako při uložení,
 * blok ale pak už nesleduje pozdější změny výchozí rychlosti.
 * Blok, jehož rychlost změnit nejde, se přeskočí.
 *
 * @param i Index bloku (typ už ověřený proti indexu).
 * @param blspeed en_CMTEXT_BLOCK_SPEED ze snapshotu.
 * @param speed en_CMTSPEED ze snapshotu.
 * @param has_pulses true = pulses_us platí (blok s vlastními pulzy).
 * @param pulses_us Vlastní pulzy bloku v µs.
 * @param def_speed Výchozí rychlost ve snapshotu.
 * @param def_us Výchozí vlastní pulzy ve snapshotu v µs.
 */
static void snap_cmt_restore_block_speed(int i, int blspeed, int speed, bool has_pulses, const unsigned pulses_us[4],
                                         int def_speed, const unsigned def_us[4])
{
    if (blspeed == (int)CMTEXT_BLOCK_SPEED_NONE)
        return;
    if (blspeed == (int)CMTEXT_BLOCK_SPEED_DEFAULT)
    {
        unsigned cur_us[4];
        snap_cmt_pulses_to_us(&g_cmt.mz_custom_pulses, cur_us);
        bool same = (def_speed == (int)g_cmt.mz_cmtspeed)
                    && (!cmtspeed_is_custom((en_CMTSPEED)def_speed) || (0 == memcmp(cur_us, def_us, sizeof(cur_us))));
        if (same)
        {
            st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
            if (cmtext_container_get_block_speed(c, i) != CMTEXT_BLOCK_SPEED_DEFAULT)
                (void)cmt_tape_change_block_speed(i, CMTSPEED_NONE, true, NULL);
            return;
        };
        /* jiná výchozí rychlost než teď: hrát rychlostí ze snapshotu, bez změny volby uživatele */
        speed = def_speed;
        pulses_us = def_us;
        has_pulses = cmtspeed_is_custom((en_CMTSPEED)def_speed);
    };
    if (cmtspeed_is_custom((en_CMTSPEED)speed))
    {
        st_MZTAPE_PULSES_LENGTH p;
        if (has_pulses && (EXIT_SUCCESS == mztape_pulses_set_us(&p, pulses_us[0], pulses_us[1], pulses_us[2], pulses_us[3])))
            (void)cmt_tape_set_block_pulses(i, &p);
        return;
    };
    if (cmtspeed_is_valid((en_CMTSPEED)speed))
        (void)cmt_tape_set_block_cmt_speed(i, (en_CMTSPEED)speed);
}


/** @brief Výsledek čtení elementu <player>. */
typedef enum en_SNAP_CMT_PLAYER {
    SNAP_CMT_PLAYER_ABSENT = 0, /**< element chybí (stav playeru se neobnovuje) */
    SNAP_CMT_PLAYER_INVALID,    /**< element je, ale hodnotu nejde přečíst nebo je mimo meze */
    SNAP_CMT_PLAYER_OK,         /**< přečteno; zbytek ověří cmttmz_set_player_state() */
} en_SNAP_CMT_PLAYER;


/**
 * @brief Přečte stav playeru z elementu <player>.
 * @param r Reader (uvnitř <tape>). @param[out] st Stav (vynulovaný a vyplněný).
 * @return SNAP_CMT_PLAYER_ABSENT bez elementu; SNAP_CMT_PLAYER_INVALID, chybí-li
 *         povinná hodnota, hloubka je mimo meze knihovny nebo hodnota
 *         přesahuje svůj typ; jinak SNAP_CMT_PLAYER_OK (ostatní hodnoty
 *         ověří cmttmz_set_player_state()).
 */
static en_SNAP_CMT_PLAYER snap_cmt_read_player(snapshot_xml_reader_t *r, st_TMZ_PLAYER_STATE *st)
{
    memset(st, 0, sizeof(*st));
    if (!snapshot_xml_enter_element(r, "player"))
        return SNAP_CMT_PLAYER_ABSENT;
    unsigned u;
    bool b;
    bool ok = true;
    ok = ok && snapshot_xml_read_uint(r, "current_block", &st->current_block);
    ok = ok && snapshot_xml_read_uint(r, "last_played_block", &st->last_played_block);
    ok = ok && snapshot_xml_read_int(r, "loop_depth", &st->loop_depth);
    ok = ok && (st->loop_depth >= 0) && (st->loop_depth <= TMZ_PLAYER_MAX_LOOP_DEPTH);
    for (int i = 0; ok && i < st->loop_depth; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "loop_%d", i);
        ok = snapshot_xml_enter_element(r, name);
        if (!ok)
            break;
        ok = snapshot_xml_read_uint(r, "start_block", &st->loop_stack[i].start_block)
             && snapshot_xml_read_uint(r, "remaining", &st->loop_stack[i].remaining);
        snapshot_xml_leave_element(r);
    };
    ok = ok && snapshot_xml_read_int(r, "call_depth", &st->call_depth);
    ok = ok && (st->call_depth >= 0) && (st->call_depth <= TMZ_PLAYER_MAX_CALL_DEPTH);
    for (int i = 0; ok && i < st->call_depth; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "call_%d", i);
        ok = snapshot_xml_enter_element(r, name);
        if (!ok)
            break;
        unsigned idx = 0, total = 0;
        ok = snapshot_xml_read_uint(r, "call_block", &st->call_stack[i].call_block)
             && snapshot_xml_read_uint(r, "current_offset_idx", &idx) && snapshot_xml_read_uint(r, "total_offsets", &total)
             && (idx <= 0xFFFF) && (total <= 0xFFFF);
        st->call_stack[i].current_offset_idx = (uint16_t)idx;
        st->call_stack[i].total_offsets = (uint16_t)total;
        snapshot_xml_leave_element(r);
    };
    if (ok && snapshot_xml_read_bool(r, "stopped", &b))
        st->stopped = b;
    if (ok && snapshot_xml_read_bool(r, "finished", &b))
        st->finished = b;
    ok = ok && snapshot_xml_read_uint(r, "signal_level", &u) && (u <= 0xFF);
    st->signal_level = (uint8_t)u;
    if (ok && snapshot_xml_read_uint(r, "mz_cpu_clock", &u))
        st->config.mz_cpu_clock = u;
    snapshot_xml_leave_element(r);
    return ok ? SNAP_CMT_PLAYER_OK : SNAP_CMT_PLAYER_INVALID;
}


/**
 * @brief Přečte element <cmthack> (stav CMT hacku nad páskou) uvnitř <tape>.
 *
 * @param r Reader uvnitř <tape>.
 * @param[out] hs Stav (vynulovaný, chybí-li element).
 * @return true = element je a všechny hodnoty jdou přečíst; false = element
 *         chybí (hs je prázdný stav) nebo je neúplný (pak varování a hs prázdný).
 */
static bool snap_cmt_read_cmthack(snapshot_xml_reader_t *r, st_CMTHACK_TAPE_STATE *hs)
{
    memset(hs, 0, sizeof(*hs));
    if (!snapshot_xml_enter_element(r, "cmthack"))
        return false;
    bool ok = snapshot_xml_read_bool(r, "pending", &hs->pending)
              && snapshot_xml_read_int(r, "pending_block", &hs->pending_block)
              && snapshot_xml_read_bool(r, "pending_moved", &hs->pending_moved)
              && snapshot_xml_read_bool(r, "end", &hs->end);
    snapshot_xml_leave_element(r);
    if (!ok)
    {
        SNAP_CMT_WARN("Snapshot CMT hack tape state is invalid, ignored");
        memset(hs, 0, sizeof(*hs));
    };
    return ok;
}


/**
 * @brief Přečte příznaky původu WAV bloků vloženého obrazu z elementů <block_N>/<type>.
 *
 * Obraz pásky (tzx_save()) nenese, že blok 0x18 vznikl importem WAV;
 * nese to typ bloku v indexu (CMTEXT_BLOCK_TYPE_WAV), který snapshot
 * ukládá v <block_N>/<type>. Bez elementu (starší snapshot) blok WAV není.
 * Hodnoty jsou nedůvěryhodné: příznak mění jen typ bloku 0x18 (polarita).
 *
 * @param r Reader uvnitř <tape>.
 * @param count Počet bloků ze snapshotu (0 .. SNAP_CMT_MAX_BLOCKS).
 * @return Pole @p count příznaků (g_malloc0, uvolní volající g_free()), NULL
 *         bez bloku WAV.
 */
static uint8_t *snap_cmt_read_wav_blocks(snapshot_xml_reader_t *r, int count)
{
    uint8_t *flags = NULL;
    for (int i = 0; i < count; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "block_%d", i);
        if (!snapshot_xml_enter_element(r, name))
            continue;
        int type = -1;
        bool ok = snapshot_xml_read_int(r, "type", &type);
        snapshot_xml_leave_element(r);
        if (!ok || type != (int)CMTEXT_BLOCK_TYPE_WAV)
            continue;
        if (!flags)
            flags = (uint8_t *)g_malloc0((gsize)count);
        flags[i] = 1;
    };
    return flags;
}


/**
 * @brief Obnoví příznaky neuloženo bloků pásky z obrazu podle <block_N>/<unsaved>.
 *
 * Volá se jen u pásky z obrazu, která si ponechala cestu (snapshot téže
 * relace): páska bez cesty je celá neuložená (cmttmz_open_image()). Blok
 * bez elementu (snapshot ze staršího emulátoru) příznak nemění - platí dosavadní
 * pravidlo obrazu s cestou (všechny bloky uložené). Hodnoty jsou
 * nedůvěryhodné, mění jen příznak zobrazený v okně a dotaz na smazání.
 *
 * @param r Reader uvnitř <tape>.
 * @param count Počet bloků ze snapshotu (0 .. SNAP_CMT_MAX_BLOCKS); bloky
 *        mimo vloženou pásku se přeskočí (cmttmz_set_block_unsaved() je odmítne).
 * @pre Vlákno emulátoru, drží device_state_lock().
 */
static void snap_cmt_restore_unsaved(snapshot_xml_reader_t *r, int count)
{
    for (int i = 0; i < count; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "block_%d", i);
        if (!snapshot_xml_enter_element(r, name))
            continue;
        bool unsaved = false;
        bool ok = snapshot_xml_read_bool(r, "unsaved", &unsaved);
        snapshot_xml_leave_element(r);
        if (ok)
            (void)cmttmz_set_block_unsaved(i, unsaved ? 1 : 0);
    };
}


/**
 * @brief Obnoví vloženou pásku podle elementu <tape> (reader stojí uvnitř <tape>).
 *
 * S s_keep_inserted_tape se použije vložená páska bez ohledu na cestu
 * (soubor se neotevírá); volající to volá jen s vloženou páskou cmt_tmz.
 * Cesta pochází ze snapshotu a otevírá se tak, jak je (bez kanonizace);
 * síťová cesta a cesta zařízení (snap_cmt_is_network_path()) se neotevře
 * - kontrola je před jakýmkoli testem souboru, takže se síť nekontaktuje.
 *
 * Pozice: ve STOP a v pauze přes cmt_tape_set_position() (v pauze se před
 * tím nastaví g_cmt.state = PLAY a paused = 1, aby funkce pozici přijala;
 * nastaví paused_time a výstup signálu v daném místě). Při PLAY bez pauzy
 * cmt_tape_set_position() nastavení odmítá, start_time se proto obnoví
 * přímo z XML (snap_cmt_apply_transport()).
 *
 * Stav CMT hacku (<cmthack>) se jen přečte do @p hs, a to jen když páska
 * odpovídá snapshotu a má otevřený blok; nastaví ho volající až po
 * obnovení transportu (cmthack_tape_state_set()).
 *
 * Obsahuje-li <tape> obraz (<image>), má přednost před cestou: páska se
 * vloží z obrazu (cmt_open_image(), Modified) bez cesty: cesta ze snapshotu
 * se jen ukáže ve zprávě "Unsaved tape restored from the snapshot (was:
 * <jméno souboru>)" (Save tape by na cestu z nedůvěryhodného snapshotu
 * jinak zapsal). Cestu si ponechá, je-li při načtení vložená páska se
 * stejnou cestou (snapshot téže relace); bloky 0x18 s <type> WAV dostanou typ WAV
 * (snap_cmt_read_wav_blocks()).
 * Soubor se nečte; vadný obraz nebo <image_omitted> = varování a postup
 * jako u chybějícího souboru.
 *
 * @param r Reader uvnitř <tape>.
 * @param path Cesta pásky (last_filename ze snapshotu; prázdná jen s obrazem).
 * @param[in,out] t Stav transportu ze snapshotu; při neúspěchu se přepne na STOP.
 * @param[out] hs Stav CMT hacku ze snapshotu (prázdný, pokud páska
 *        neodpovídá nebo element chybí).
 *
 * @pre Vlákno emulátoru, drží device_state_lock().
 * @post Páska je vložená a odpovídá snapshotu, nebo vložená ve STOP na
 *       prvním bloku (soubor neodpovídá, nebo páska při uložení neměla
 *       otevřený blok), nebo není vložená (soubor chybí či nejde otevřít;
 *       t je STOP).
 */
static void snap_cmt_restore_tape(snapshot_xml_reader_t *r, const char *path, st_SNAP_CMT_TRANSPORT *t,
                                  st_CMTHACK_TAPE_STATE *hs)
{
    memset(hs, 0, sizeof(*hs));
    int count = 0, block_id = -1, def_speed = (int)CMTSPEED_1_1;
    uint64_t ticks = 0;
    unsigned def_us[4] = {0, 0, 0, 0};
    snapshot_xml_read_int(r, "block_count", &count);
    snapshot_xml_read_int(r, "block_id", &block_id);
    snapshot_xml_read_uint64(r, "ticks_in_block", &ticks);
    snapshot_xml_read_int(r, "default_speed", &def_speed);
    (void)snap_cmt_read_pulses(r, "default_pulses_us", def_us);
    if (count < 0 || count > SNAP_CMT_MAX_BLOCKS)
        count = 0;

    /* transport do STOP (probíhající nahrávání zahodil už začátek načtení,
     * cmt_stop() ho nedokončí), páska se mění jen ve STOP */
    if (!CMT_TEST_STOP)
        cmt_stop();

    /* obraz vložené pásky (páska Modified nebo bez cesty) */
    char *image = NULL;
    bool omitted = false;
    (void)snapshot_xml_read_string(r, "image", &image);
    (void)snapshot_xml_read_bool(r, "image_omitted", &omitted);
    /* element modified (zápis kvůli starším verzím) se při načtení nepoužívá */
    bool embedded = image || omitted;
    const char *orig_path = path;
    /* páska z obrazu nesmí vlastnit síťovou cestu: cestu (z nedůvěryhodného
     * snapshotu) by pak četl záznam vstupů, otisk stroje i další load */
    if (embedded && snap_cmt_is_network_path(path))
    {
        SNAP_CMT_WARN("Snapshot tape path on a network location was dropped");
        path = "";
    };
    const char *shown = path[0] ? path : "(new tape)";

    /* stejná páska už vložená (načtení ve stejném procesu, checkpoint záznamu
     * vstupů); vložená páska z obrazu se vždy otevře znovu z obrazu */
    st_CMTEXT_CONTAINER *c = (CMT_TEST_FILLED && (g_cmt.ext == g_cmt_tmz_extension)) ? cmtext_get_container(g_cmt.ext) : NULL;
    bool reuse = c && (s_keep_inserted_tape || (!embedded && (0 == strcmp(cmtext_container_get_filepath(c), path))
                                                && (cmtext_container_get_count_blocks(c) == count)));
    if (!reuse && embedded)
    {
        size_t size = 0;
        uint8_t *data = image ? snap_cmt_decode_image(image, &size) : NULL;
        g_free(image);
        image = NULL;
        /* původ WAV bloků 0x18 (obraz typ bloku nenese): element type bloku */
        uint8_t *wav = snap_cmt_read_wav_blocks(r, count);
        /* cestu z nedůvěryhodného snapshotu páska z obrazu nedostane (Save
         * tape by na ni zapsal): je bez jména a Save se na jméno zeptá.
         * Výjimka: v kazetofonu je páska s přesně touto cestou - snapshot
         * patří této relaci (Quick Save -> Quick Load), cesta zůstane.
         * (Snapshot záznamu vstupů, keep-inserted, obraz nevkládá a používá
         * vloženou pásku, sem nedojde.) */
        const bool same_tape = c && path[0] && (0 == strcmp(cmtext_container_get_filepath(c), path));
        const char *img_path = (s_keep_inserted_tape || same_tape) ? path : "";
        bool opened = false;
        if (omitted)
            SNAP_CMT_WARN("Snapshot tape was too large to embed: %s", shown);
        else if (!data || (EXIT_SUCCESS != cmt_open_image(data, size, img_path, wav, wav ? (size_t)count : 0)))
            SNAP_CMT_WARN("Snapshot embedded tape is invalid: %s", shown);
        else
        {
            opened = true;
            /* varování importu obrazu patří k varováním snapshotu */
            if (cmttmz_get_open_warning()[0])
                SNAP_CMT_WARN("Snapshot embedded tape: %s", cmttmz_get_open_warning());
            /* jméno souboru ze snapshotu jen jako nápověda ve zprávě */
            if (!img_path[0] && orig_path[0])
            {
                gchar *base = g_path_get_basename(orig_path);
                SNAP_CMT_WARN("Unsaved tape restored from the snapshot (was: %s)", base);
                g_free(base);
            };
        };
        g_free(wav);
        g_free(data);
        if (!opened || !CMT_TEST_FILLED || (g_cmt.ext != g_cmt_tmz_extension))
        {
            cmt_eject();
            snap_cmt_transport_to_stop(t);
            return;
        };
        /* páska z obrazu je vždy Modified bez ohledu na XML (obsah se může od
         * souboru lišit, snapshot je nedůvěryhodný vstup) */
        cmttmz_set_modified(1);
        /* páska s cestou (téže relace): které bloky soubor nese, říkají příznaky
         * neuloženo snapshotu; páska bez cesty je celá neuložená */
        if (img_path[0])
            snap_cmt_restore_unsaved(r, count);
        c = cmtext_get_container(g_cmt.ext);
    }
    else if (!reuse)
    {
        /* síťová cesta a cesta zařízení: neotevírat, ještě před jakýmkoli testem souboru */
        if (snap_cmt_is_network_path(path))
        {
            SNAP_CMT_WARN("Snapshot tape on a network path is not opened automatically: %s", path);
            cmt_eject();
            snap_cmt_transport_to_stop(t);
            return;
        };
        if (!path[0] || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
        {
            SNAP_CMT_WARN("Snapshot tape file not found: %s", shown);

            cmt_eject();
            snap_cmt_transport_to_stop(t);
            return;
        };
        if ((EXIT_SUCCESS != cmt_open_file_by_extension((char *)path)) || (g_cmt.ext != g_cmt_tmz_extension))
        {
            SNAP_CMT_WARN("Snapshot tape file cannot be opened: %s", path);
            cmt_eject();
            snap_cmt_transport_to_stop(t);
            return;
        };
        c = cmtext_get_container(g_cmt.ext);
    };
    g_free(image);

    /* rychlosti bloků, které odpovídají indexem i druhem; obnova rychlosti
     * není změna pásky uživatelem: Modified zůstane, jaký byl (změna rychlosti
     * bloku jinak pásku označí jako Modified) */
    const int modified_before = cmttmz_is_modified();
    int file_count = cmtext_container_get_count_blocks(c);
    bool match = (file_count == count);
    int n = (count < file_count) ? count : file_count;
    for (int i = 0; i < n; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "block_%d", i);
        if (!snapshot_xml_enter_element(r, name))
        {
            match = false;
            continue;
        };
        int type = -1, blspeed = 0, speed = 0;
        unsigned us[4] = {0, 0, 0, 0};
        snapshot_xml_read_int(r, "type", &type);
        snapshot_xml_read_int(r, "blspeed", &blspeed);
        snapshot_xml_read_int(r, "speed", &speed);
        bool has_pulses = snap_cmt_read_pulses(r, "pulses_us", us);
        snapshot_xml_leave_element(r);
        if (type != (int)cmtext_container_get_block_type(c, i))
        {
            match = false;
            continue;
        };
        snap_cmt_restore_block_speed(i, blspeed, speed, has_pulses, us, def_speed, def_us);
    };
    if (!modified_before && cmttmz_is_modified())
        cmttmz_set_modified(0);

    /* páska bez otevřeného bloku (dohraná bez převinutí, jen řídicí bloky):
     * shoduje-li se soubor, je jako po otevření, ve STOP, bez varování */
    if (match && block_id < 0)
    {
        (void)cmttmz_rewind();
        /* nahrávání do pásky bez otevřeného bloku (nová páska) RECORD drží:
         * obnoví ho kopie checkpointu, jinak ho srovná loader */
        if (!(t->has_state && (t->state == (int)CMT_STATE_RECORD)))
            snap_cmt_transport_to_stop(t);
        return;
    };

    if (match && block_id < file_count)
    {
        if (EXIT_SUCCESS != cmttmz_seek_block(block_id))
            match = false;
    }
    else
    {
        match = false;
    };

    if (!match)
    {
        SNAP_CMT_WARN("Snapshot tape does not match file: %s", path);
        (void)cmttmz_rewind();
        snap_cmt_transport_to_stop(t);
        return;
    };

    st_TMZ_PLAYER_STATE st;
    en_SNAP_CMT_PLAYER pr = snap_cmt_read_player(r, &st);
    if ((pr == SNAP_CMT_PLAYER_INVALID) || ((pr == SNAP_CMT_PLAYER_OK) && (EXIT_SUCCESS != cmttmz_set_player_state(&st))))
    {
        SNAP_CMT_WARN("Snapshot tape player state is invalid, playback continues from block %d", block_id);
    };

    /* pozice: ve STOP a v pauze přes cmt_tape_set_position(), při PLAY start_time z XML */
    bool stop = !t->has_state || (t->state == (int)CMT_STATE_STOP);
    bool paused = !stop && (t->state == (int)CMT_STATE_PLAY) && t->has_paused && t->paused;
    if (stop)
    {
        (void)cmt_tape_set_position(block_id, ticks);
    }
    else if (paused)
    {
        g_cmt.state = CMT_STATE_PLAY;
        g_cmt.paused = 1;
        if (EXIT_SUCCESS == cmt_tape_set_position(block_id, ticks))
        {
            /* paused_time a výstup nastavila cmt_tape_set_position() */
            t->has_paused_time = false;
            t->has_output = false;
        };
    };

    (void)snap_cmt_read_cmthack(r, hs);
}


static en_SNAPSHOT_RESULT snap_cmt_load_body(st_SNAPSHOT_CONTEXT *ctx);


/**
 * @brief Zahodí probíhající nahrávání před obnovou stavu ze snapshotu.
 *
 * Snapshoty záznamu vstupů (keep-inserted: start, checkpoint) ho zahodí
 * bez zprávy (cmt_record_drop()) - checkpoint ho buď obnoví z kopie, nebo
 * se vrací do doby před ním; jinak se zahodí jako při vysunutí
 * (cmt_record_discard("a snapshot was loaded"), zpráva, prázdný soubor
 * zkoušky WAV pryč).
 *
 * Volá se hned na začátku handleru (snap_cmt_load_body()), ještě před
 * čtením devices/cmt.xml: handlery běží až po ověření manifestu,
 * architektury a kontrolního součtu, načtení tedy v té chvíli pokračuje
 * a nahraje celý stroj (i když je devices/cmt.xml vadný - komponenta je
 * volitelná a rámec po jejím selhání pokračuje). Nahrávání z dosavadní
 * časové osy (buffer hran, start_time) do načteného stroje nepatří.
 *
 * @pre Vlákno emulátoru, volající nedrží device_state_lock()
 *      (cmt_record_discard() si ho vezme, nedrží-li ho).
 */
static void snap_cmt_drop_recording(void)
{
    if (!(CMT_TEST_RECORD && cmttmz_rec_is_active()))
        return;
    if (s_keep_inserted_tape)
        cmt_record_drop();
    else
        cmt_record_discard("a snapshot was loaded");
}


/**
 * @brief Kazetofon do klidu, když snapshot stav CMT neobnoví (devices/cmt.xml chybí nebo je vadný).
 *
 * Nahrávání zahodila už snap_cmt_drop_recording(); transport (PLAY,
 * RECORD) by běžel s časovou základnou dosavadní osy (start_time) nad
 * načteným strojem, proto se zastaví (cmt_stop(), v tichém režimu
 * načtení). Páska zůstane vložená.
 *
 * @pre Vlákno emulátoru, nahrávání neaktivní.
 * @post CMT_TEST_STOP, žádné nahrávání.
 */
static void snap_cmt_transport_rest(void)
{
    if (!CMT_TEST_STOP)
        cmt_stop();
}


/**
 * @brief Načte stav CMT z devices/cmt.xml do g_cmt.
 *
 * Chybějící entry není chyba (komponenta je volitelná). Chybějící
 * playsts (starší snapshot) se odvodí ze stavu transportu. S elementem
 * <tape> a neprázdnou cestou se obnoví páska (snap_cmt_restore_tape()).
 * Pak se volá cmt_sanitize_state(): bez vložené pásky (nebo s páskou,
 * kterou nelze v obnoveném stavu použít) skončí transport ve STOP
 * a vypíše se varování na stderr. Nakonec cmt_cpu_boost_apply() srovná MAX
 * SPEED s obnoveným transportem a aktuální volbou cpu_boost.
 *
 * Elementy cpu_boost, polarity, mz_cmtspeed a mzfsize_check v XML se
 * záměrně ignorují: jsou to uživatelské preference a jejich cfg elementy
 * v sekci [CMT] ukazují přímo na proměnné g_cmt, takže by hodnota ze
 * snapshotu při ukončení emulátoru přepsala i INI.
 *
 * Cesta pásky (last_filename) pochází ze souboru snapshotu a otevírá se
 * tak, jak je (bez kanonizace); síťovou cestu a cestu zařízení loader
 * automaticky neotevře (varování, STOP bez pásky), ledaže je vložená
 * páska s přesně touto cestou.
 *
 * Pořadí načtení: start_time a paused_time jsou v taktech
 * gdg_get_total_ticks(), loader proto spoléhá na to, že handler GDG
 * (SNAPSHOT_PRIORITY_HW_CORE, 20) obnoví g_gdg.total_elapsed dřív než
 * tento handler (SNAPSHOT_PRIORITY_DEVICE, 40).
 *
 * Stav CMT hacku nad páskou se na začátku vynuluje
 * (cmthack_tape_state_clear()) a po obnovení pásky a transportu nastaví
 * z <tape>/<cmthack> (cmthack_tape_state_set()), jen když je CMT hack ve
 * stroji zapnutý (s vypnutým zůstane vynulovaný); neplatné hodnoty =
 * varování, hack začíná od nuly.
 *
 * Celé načtení běží v tichém režimu CMT (cmt_quiet_begin()): cmt_stop(),
 * cmt_eject() a cmt_open_file_by_extension() během něj neodpalují BP
 * eventy cmt:mstate a cmt:state_change a nevypisují "Virtual CMT is
 * stopped"; cache hrany cmt:mstate se na konci srovná na obnovený stav.
 *
 * @param ctx Kontext snapshotu (otevřený pro čtení).
 * @return SNAPSHOT_OK, nebo chyba čtení / parsování XML. Problémy s páskou
 *         (chybějící nebo neodpovídající soubor) chybou nejsou, jen varováním.
 *
 * @pre Emulace je pozastavená; vlákno emulátoru. Funkce bere
 *      device_state_lock() (nesmí ho držet volající).
 * @post Platí invarianty st_CMT (bez pásky je transport ve STOP).
 * @post Patch zápisu CMT hacku (Direct Save) odpovídá obnovenému transportu
 *       (cmthack_save_patch_sync_loaded_state(): v RECORD v ROM není);
 *       PC obnovené ze snapshotu se přitom nemění.
 * @post g_cmt.cpu_boost, polarity, mz_cmtspeed a mzfsize_check mají
 *       stejnou hodnotu jako před voláním.
 * @post Hrající páska (PLAY/RECORD bez pauzy) s cpu_boost -> MAX SPEED
 *       zapnutá; jinak neběží MAX SPEED zapnutá automatikou. MAX SPEED
 *       zvolená uživatelem se nemění.
 */
static en_SNAPSHOT_RESULT snap_cmt_load(st_SNAPSHOT_CONTEXT *ctx)
{
    cmt_quiet_begin();
    en_SNAPSHOT_RESULT res = snap_cmt_load_body(ctx);
    cmt_quiet_end();
    /* obnovený transport (i RECORD): patch zápisu CMT hacku jen mimo RECORD;
     * pokrývá i předčasné návraty těla. PC je už ze snapshotu a patří ROM
     * stroje snapshotu, proto bez přesunu PC. */
    cmthack_save_patch_sync_loaded_state();
    return res;
}


/**
 * @brief Tělo snap_cmt_load() (bez tichého režimu, ten zapíná volající).
 * @param ctx Kontext snapshotu. @return Jako snap_cmt_load().
 */
static en_SNAPSHOT_RESULT snap_cmt_load_body(st_SNAPSHOT_CONTEXT *ctx)
{
    /* stav hacku z doby před načtením do načteného stroje nepatří */
    cmthack_tape_state_clear();

    /* probíhající nahrávání do načteného stroje nepatří (zahodí se jako při
     * vysunutí): handler běží až po ověření manifestu a kontrolního součtu,
     * načtení stroje tedy proběhne i při vadném devices/cmt.xml */
    snap_cmt_drop_recording();

    /* CMT entry je volitelný - pokud neexistuje, přeskočíme; transport
     * dosavadní osy do načteného stroje nepatří (STOP) */
    if (!snapshot_io_entry_exists(ctx->io, "devices/cmt.xml")) {
        snap_cmt_transport_rest();
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/cmt.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("cmt", "Cannot load devices/cmt.xml");
        snap_cmt_transport_rest();
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("cmt", "Parse error in devices/cmt.xml");
        snap_cmt_transport_rest();
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "cmt_state")) {
        SNAP_ERR("cmt", "Missing element cmt_state");
        snapshot_xml_reader_free(r);
        snap_cmt_transport_rest();
        return SNAPSHOT_ERR_XML_PARSE;
    }

    st_SNAP_CMT_TRANSPORT t;
    memset(&t, 0, sizeof(t));

    /* Základní stavové proměnné */
    t.has_state = snapshot_xml_read_int(r, "state", &t.state);
    t.has_paused = snapshot_xml_read_int(r, "paused", &t.paused);
    /* starší snapshot (a snapshot bez pauzy uživatele) elementy nemá: 0 */
    if (!snapshot_xml_read_int(r, "user_paused", &t.user_paused))
        t.user_paused = 0;
    if (!snapshot_xml_read_int(r, "motor_off", &t.motor_off))
        t.motor_off = 0;
    /* polarity a mz_cmtspeed záměrně nečteme - uživatelské preference. */
    t.has_output = snapshot_xml_read_int(r, "output", &t.output);

    /* playsts: starší snapshoty ho neobsahují - odvodíme ze stavu. */
    int ival;
    int state_now = t.has_state ? t.state : (int)g_cmt.state;
    if (snapshot_xml_read_int(r, "playsts", &ival)
        && (ival >= (int)CMTEXT_BLOCK_PLAYSTS_BODY)
        && (ival <= (int)CMTEXT_BLOCK_PLAYSTS_STOP)) {
        t.playsts = ival;
    } else {
        t.playsts = (state_now == (int)CMT_STATE_STOP)
                        ? (int)CMTEXT_BLOCK_PLAYSTS_STOP
                        : (int)CMTEXT_BLOCK_PLAYSTS_BODY;
    }

    /* Časové údaje */
    t.has_start_time = snapshot_xml_read_uint64(r, "start_time", &t.start_time);
    t.has_paused_time = snapshot_xml_read_uint64(r, "paused_time", &t.paused_time);
    t.has_rec_last = snapshot_xml_read_uint64(r, "recording_last_event", &t.rec_last);

    /* Nastavení. Elementy cpu_boost a mzfsize_check záměrně nečteme - jsou
     * to uživatelské preference (cfg [CMT]), ne stav stroje; obnovení by je
     * tiše a trvale přepsalo (i v INI). */
    t.has_rec_stream = snapshot_xml_read_int(r, "recording_to_stream", &t.rec_stream);

    /* Nahrávání: buffer hran soubor snapshotu nenese; obnoví ho jen
     * checkpoint záznamu vstupů (s_record_restore), jinak STOP. */
    bool restore_record = t.has_state && (t.state == (int)CMT_STATE_RECORD) && s_record_restore;
    if (t.has_state && (t.state == (int)CMT_STATE_RECORD) && !restore_record)
    {
        SNAP_CMT_WARN("Snapshot was taken while recording; the recording was not restored");
        snap_cmt_transport_to_stop(&t);
        t.has_rec_stream = true;
        t.rec_stream = 0;
    };

    /* Cesta pásky: s elementem <tape> se páska obnoví, jinak (starší
     * snapshot) se jako dřív neotevírá. */
    char *fname = NULL;
    (void)snapshot_xml_read_string(r, "last_filename", &fname);
    bool has_path = fname && (fname[0] != '\0');
    /* Záznam vstupů bez vložené pásky: soubor se neotevírá, transport
     * srovná cmt_sanitize_state() jako u snapshotu bez <tape>. Páska bez
     * cesty má <tape> vždy (s obrazem, nebo bez něj ve snapshotech záznamu vstupů). */
    bool keep_without_tape = s_keep_inserted_tape && !(CMT_TEST_FILLED && (g_cmt.ext == g_cmt_tmz_extension));
    if (!keep_without_tape && snapshot_xml_enter_element(r, "tape")) {
        st_CMTHACK_TAPE_STATE hs;
        device_state_lock();
        snap_cmt_restore_tape(r, fname ? fname : "", &t, &hs);
        snapshot_xml_leave_element(r); /* tape */
        snap_cmt_apply_transport(&t);
        device_state_unlock();
        /* až po obnovení pásky: stav hacku se váže na aktuální čítač otevření.
         * Jen se zapnutým hackem (snapshot stav hacku nenese, platí ten ve
         * stroji): s vypnutým by rozečtená hlavička zůstala ležet a po pozdějším
         * zapnutí hacku by první načtení blok přeskočilo. */
        if (CMTHACK_TEST_IS_INSTALLED && (hs.pending || hs.end) && (EXIT_SUCCESS != cmthack_tape_state_set(&hs)))
            SNAP_CMT_WARN("Snapshot CMT hack tape state is invalid, ignored");
    } else {
        if (has_path && !keep_without_tape) {
            SNAP_CMT_WARN("Cassette reference file '%s' recorded, but will not be auto-opened", fname);
        }
        snap_cmt_apply_transport(&t);
    }
    g_free(fname);

    snapshot_xml_leave_element(r); /* cmt_state */
    snapshot_xml_reader_free(r);

    /* nahrávání z checkpointu záznamu vstupů: buffer hran zpět */
    if (restore_record && CMT_TEST_RECORD && (EXIT_SUCCESS != cmt_record_ckpt_install(s_record_restore)))
        SNAP_CMT_WARN("Snapshot was taken while recording; the recording was not restored");

    /* Stav PLAY/RECORD bez vložené (nebo bez vhodné) pásky by vedl k práci
     * s nevloženou páskou -> transport přepneme do STOP. Pokud páska
     * vložená je (obnovená ze snapshotu, nebo načtení ve stejném procesu
     * bez <tape>), stav i pozice zůstávají ze snapshotu. */
    if (cmt_sanitize_state()) {
        SNAP_CMT_WARN("Tape transport was active in the snapshot, but no matching tape is inserted - transport set to STOP");
    }

    /* Snapshot obnovil stav transportu, rychlost emulace ale ne: hrající
     * páska s cpu_boost by běžela normální rychlostí a MAX SPEED zapnutá
     * boostem před načtením by zůstala viset i po STOP. Srovnává se podle
     * aktuální volby cpu_boost uživatele. Uživatelem zvolenou MAX SPEED
     * cmt_cpu_boost_apply nemění. */
    cmt_cpu_boost_apply();

    return SNAPSHOT_OK;
}


void snap_cmt_register(void)
{
    snapshot_register_component("cmt",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_cmt_save,
                                snap_cmt_load,
                                true);
}
