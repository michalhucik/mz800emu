/**
 * @file   cmt_analyze_dialog.cpp
 * @brief  Dialog "Analyze recording - block N" okna Virtual CMT: analýza surového bloku na soubory.
 *
 * Rozvržení shora (návrh okna, SPEC,
 * koncepty cmt-analyze-v2 a cmt-analyze-empty-v2): věta výsledku, ztlumený
 * souhrn bloku, tabulka (zaškrtnutí, #, Name, Type, Format, Start, Size,
 * Checksum, Result; ImGuiListClipper, pořadí podle polohy v záznamu),
 * řádek nápovědy k vybranému řádku, volby (Leader tolerance 2-35 % s Reset
 * a Analyze again, pod nimi tři opravy), sbalený oddíl Signal details
 * (histogram délek pulzů), varování, tlačítka Add files after recording
 * (výchozí, Enter), Replace recording, Cancel (Escape).
 *
 * Stavy: kopie pulzů a analýza běží (Analyzing..., uplynulý čas, akce
 * zakázané), hotovo, nic nenalezeno (Signal details se samy rozbalí),
 * chyba (text chyby místo tabulky), zastaralé výsledky (volby jiné než
 * u výsledku: tabulka ztlumená, akce zakázané), páska se změnila (jiný
 * čítač výměn pásky: dialog zůstane, akce zakázané s textem).
 *
 * Velikost z obsahu, nejvýš 90 % pracovní plochy: šířka podle přirozené
 * šířky tabulky, voleb a tlačítek (nejvýš 90 % šířky plochy, jinak se volby
 * a tlačítka zalomí a tabulka se posouvá vodorovně). Horní část a patička
 * jsou vždy vidět, mezi nimi posouvané tělo. V dialogu je nejvýš jeden
 * svislý posuvník: vejdou-li se všechny řádky,
 * žádný; vejde-li se aspoň CAD_TABLE_MIN_ROWS řádků, posouvá se jen tabulka;
 * jinak má tabulka výšku všech řádků a posouvá se tělo. Hrany posouvaného
 * těla leží jen mezi řádky (značky cad_mark()); rozbalení Signal details
 * posune tělo na histogram s osou. Znovu doprostřed jen při otevření
 * a změně velikosti, ne po přetažení.
 *
 * Vlákna a vlastnictví: viz cmt_analyze_dialog.h. Úloha analýzy
 * (st_CAD_JOB) a kopie pulzů (st_CAD_PULSES) mají atomické čítače odkazů;
 * kdo pustí poslední odkaz, data uvolní (UI vlákno, nebo pracovní vlákno
 * po opuštění). Data zásahu uplatnění drží modul v s_cad_applies (jediné
 * místo jejich uvolnění: cad_apply_release()).
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

#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <glib.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"

#include "emulator/hw-generic/cmt/cmt_tmz_analyze.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "device_request.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "imgui_cmt.h"
#include "cmt_view.h"
#include "cmt_tokens.h"
#include "cmt_window.h"
#include "cmt_analyze_dialog.h"
#include "ui-imgui/imgui_modal.h"


/* ------------------------------------------------------------------------- */
/*  Pseudo-překlad pro testy (texty o 40 % delší, jako cmt_window.cpp)       */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
/* _(), _L() a C_() s testovacím prodloužením textů (společné cmt_view.cpp) */
#undef _
#define _(STRING) cmt_view_test_tr ( STRING )
#define _L(STRING) cmt_view_test_trl ( STRING )
#undef C_
#define C_(CTX, STR) cmt_view_test_trc ( CTX "\004" STR, STR )
#endif


/* ------------------------------------------------------------------------- */
/*  Konstanty                                                                */
/* ------------------------------------------------------------------------- */

/** @brief Stabilní ID modálu (titulek se mění s číslem bloku a jazykem, ID ne). */
#define CAD_POPUP_ID "###cmt_analyze"

/** @brief Nejmenší Leader tolerance v % (WAV_ANALYZER_MIN_TOLERANCE 0,02; SPEC 2-35 %). */
#define CAD_TOL_MIN 2
/** @brief Největší Leader tolerance v % (WAV_ANALYZER_MAX_TOLERANCE 0,35). */
#define CAD_TOL_MAX 35
/** @brief Výchozí Leader tolerance v % (cmttmz_analyze_opts_default(): 0,20). */
#define CAD_TOL_DEFAULT 20

/** @brief Počet binů histogramu na ose (navíc jeden bin pro delší pulzy, který se nekreslí). */
#define CAD_HIST_BINS 60u

/**
 * @brief Horní mez osy histogramu v µs.
 *
 * Pauzy a ticho mají pulzy až v sekundách; bez meze by roztáhly osu a vrcholy
 * signálu by splynuly. Hodnota 1 500 µs pokryje pulzy
 * Sharp ROM (asi 240 až 500 µs) s rezervou; vhodnost pro všechny
 * zapisovače [neověřeno].
 */
#define CAD_HIST_AXIS_MAX_US 1500.0

/** @brief Dolní mez osy histogramu v µs (krátký záznam s krátkými pulzy). */
#define CAD_HIST_AXIS_MIN_US 200.0

/** @brief Nejméně řádků těla tabulky, jsou-li nějaké řádky (jinak by tabulka "zmizela"). */
#define CAD_TABLE_MIN_ROWS 3


/** @brief Sloupce tabulky výsledků (SPEC: zaškrtnutí, #, Name, Type, Format, Start, Size, Checksum, Result). */
typedef enum en_CAD_COL {
    CAD_COL_USE = 0,  /**< zaškrtnutí (jen soubory) */
    CAD_COL_NUM,      /**< "#": pořadí řádku od 1 (poloha v záznamu) */
    CAD_COL_NAME,     /**< "Name": jméno souboru, "Unknown signal" u neznámého úseku */
    CAD_COL_TYPE,     /**< "Type": typ MZF slovně */
    CAD_COL_FORMAT,   /**< "Format": "Sharp standard" (NORMAL), jinak jméno formátu knihovny */
    CAD_COL_START,    /**< "Start": začátek úseku v záznamu (m:ss) */
    CAD_COL_SIZE,     /**< "Size": velikost dat v B ("-" u neznámého úseku) */
    CAD_COL_CHECKSUM, /**< "Checksum": OK / Header error / Body error / - */
    CAD_COL_RESULT,   /**< "Result": Will be added / Will be added (repaired) / Stays raw / Cannot add */
    CAD_COL_COUNT     /**< počet sloupců (sentinel) */
} en_CAD_COL;


/* ------------------------------------------------------------------------- */
/*  Úloha analýzy a kopie pulzů (sdílené s pracovním vláknem)                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Kopie pulzů surového bloku s čítačem odkazů.
 *
 * Odkaz drží dialog (histogram, uplatnění), každá úloha analýzy a každý
 * čekající zásah uplatnění. Kopie se po vzniku nemění; čtou ji současně
 * pracovní vlákno i UI vlákno. Nejdelší pulz celé kopie spočítá jednou
 * první úloha analýzy v pracovním vlákně (max_us, pak max_known = 1;
 * g_atomic_int_set je bariéra, UI vlákno čte max_us jen po max_known);
 * analýzy jedné kopie běží jen po sobě (nejvýš jedno vlákno), zápis je
 * tedy jediný.
 *
 * @invariant refs >= 1, dokud struktura existuje; p != NULL.
 */
typedef struct st_CAD_PULSES {
    st_CMTTMZ_PULSES *p; /**< kopie (cmt_request_take_pulses(), uvolní cmttmz_pulses_free()) */
    gint refs;           /**< počet odkazů (atomicky) */
    double max_us;       /**< nejdelší pulz celé kopie v µs (platí při max_known) */
    gint max_known;      /**< 1 = max_us spočítané (atomicky) */
} st_CAD_PULSES;

/**
 * @brief Jedna úloha analýzy (vstup, výsledek, příznak hotovo) s čítačem odkazů.
 *
 * Odkazy: UI vlákno (dialog) a po spuštění pracovní vlákno. Pracovní vlákno
 * zapíše result a row_max a pak nastaví done (g_atomic_int_set je bariéra);
 * UI vlákno čte result a row_max jen po done == 1. Kdo pustí poslední
 * odkaz, uvolní result, row_max, pustí kopii pulzů a strukturu.
 *
 * @invariant pulses != NULL; refs >= 1, dokud struktura existuje.
 * @invariant done == 1 => row_max má cmttmz_analysis_row_count ( result ) položek (NULL při 0 řádcích).
 */
typedef struct st_CAD_JOB {
    gint refs;                   /**< počet odkazů (atomicky) */
    st_CAD_PULSES *pulses;       /**< kopie pulzů (vlastní odkaz) */
    st_CMTTMZ_ANALYZE_OPTS opts; /**< volby analýzy */
    st_CMTTMZ_ANALYSIS *result;  /**< výsledek (zapíše pracovní vlákno před done) */
    double *row_max;             /**< nejdelší pulz rozsahu každého řádku v µs (g_new, pracovní vlákno před done) */
    gint done;                   /**< 1 = analýza skončila (atomicky) */
    bool started;                /**< úloha byla předána vláknu (nebo frontě testu) */
} st_CAD_JOB;

/** @brief Počet živých úloh (diagnostika a testy; atomicky). */
static gint s_cad_live_jobs = 0;
/** @brief Počet živých kopií pulzů (diagnostika a testy; atomicky). */
static gint s_cad_live_pulses = 0;
/** @brief Počet běžících pracovních vláken (nejvýš jedna analýza najednou; atomicky). */
static gint s_cad_workers = 0;

/**
 * @brief Vlákna GLib, která se ještě nepřipojila (g_thread_join() po doběhnutí, UI vlákno).
 *
 * Vlákno se připojí v cad_poll(), jakmile žádné neběží (s_cad_workers == 0
 * - tělo doběhlo, zbývá jen návrat), nebo při ukončení aplikace s mezí
 * CAD_EXIT_JOIN_MS; po mezi se odpojí (g_thread_unref()).
 */
static std::vector<GThread *> s_cad_threads;

/** @brief Nejdéle tolik ms čeká ukončení aplikace na běžící analýzu, pak vlákno odpojí. */
#define CAD_EXIT_JOIN_MS 2000

#ifdef MZTEST_HEADLESS
/** @brief Testy: mez čekání při ukončení v ms (cmt_analyze_dialog_test_set_exit_join_ms(); v aplikaci CAD_EXIT_JOIN_MS). */
static int s_cad_exit_join_ms = CAD_EXIT_JOIN_MS;
/** @brief Mez čekání při ukončení v ms: v aplikaci konstanta, v testech nastavitelná. */
#define CAD_EXIT_JOIN_MS_NOW s_cad_exit_join_ms

/** @brief Testy: funkce analýzy (cmt_analyze_dialog_test_set_analyzer(); v aplikaci cmttmz_analyze_pulses()); čte ji pracovní vlákno (atomicky). */
static gpointer s_cad_analyzer = ( gpointer ) cmttmz_analyze_pulses;
/** @brief Testy: úlohy se nespouští ve vlákně, čekají na cmt_analyze_dialog_test_run_jobs(). */
static bool s_cad_test_manual = false;
/** @brief Testy: fronta úloh řízeného "vlákna". */
static std::vector<st_CAD_JOB *> s_cad_test_jobs;
/** @brief Testy: volby poslední spuštěné úlohy. */
static st_CMTTMZ_ANALYZE_OPTS s_cad_test_last_opts = { 0.20, 0, 0, 0 };
/** @brief Testy: počet průchodů celou kopií pulzů pro nejdelší pulz (atomicky). */
static gint s_cad_test_max_passes = 0;
#else
/** @brief Mez čekání při ukončení v ms: v aplikaci konstanta, v testech nastavitelná. */
#define CAD_EXIT_JOIN_MS_NOW CAD_EXIT_JOIN_MS
#endif


/**
 * @brief Nový držák kopie pulzů (1 odkaz volajícího).
 * @param p Kopie (převezme ji držák). @return Držák.
 */
static st_CAD_PULSES *cad_pulses_new ( st_CMTTMZ_PULSES *p )
{
    st_CAD_PULSES *h = g_new0 ( st_CAD_PULSES, 1 );
    h->p = p;
    h->refs = 1;
    g_atomic_int_inc ( &s_cad_live_pulses );
    return h;
}

/** @brief Přidá odkaz na kopii pulzů. @param h Držák. @return Týž držák. */
static st_CAD_PULSES *cad_pulses_ref ( st_CAD_PULSES *h )
{
    g_atomic_int_inc ( &h->refs );
    return h;
}

/**
 * @brief Pustí odkaz na kopii pulzů; poslední odkaz kopii uvolní.
 * @param h Držák (NULL se ignoruje).
 * @note Libovolné vlákno (poslední odkaz může pustit pracovní vlákno).
 */
static void cad_pulses_unref ( st_CAD_PULSES *h )
{
    if ( !h ) return;
    if ( !g_atomic_int_dec_and_test ( &h->refs ) ) return;
    cmttmz_pulses_free ( h->p );
    g_free ( h );
    g_atomic_int_add ( &s_cad_live_pulses, -1 );
}

/**
 * @brief Pustí odkaz na úlohu; poslední odkaz uvolní výsledek, kopii pulzů a úlohu.
 * @param job Úloha (NULL se ignoruje).
 * @note Libovolné vlákno.
 */
static void cad_job_unref ( st_CAD_JOB *job )
{
    if ( !job ) return;
    if ( !g_atomic_int_dec_and_test ( &job->refs ) ) return;
    cmttmz_analysis_free ( job->result );
    g_free ( job->row_max );
    cad_pulses_unref ( job->pulses );
    g_free ( job );
    g_atomic_int_add ( &s_cad_live_jobs, -1 );
}

/**
 * @brief Nejdelší pulz rozsahu [first, end) kopie v µs (jeden průchod, pracovní vlákno).
 * @param p Kopie. @param first První pulz. @param end Pulz za rozsahem. @return µs (0 u prázdného rozsahu).
 */
static double cad_range_max_us ( const st_CMTTMZ_PULSES *p, uint32_t first, uint32_t end )
{
    uint32_t mx = 0;
    if ( end > p->count ) end = p->count;
    for ( uint32_t k = first; k < end; k++ )
        if ( p->lengths[k] > mx ) mx = p->lengths[k];
    return p->sample_rate ? ( double ) mx * 1e6 / ( double ) p->sample_rate : 0.0;
}

/**
 * @brief Tělo pracovního vlákna: analýza, nejdelší pulzy (kopie jednou, řádky), příznak hotovo, odkaz pryč.
 *
 * Čte jen kopii pulzů úlohy a volby (cmttmz_analyze_pulses() je čistá
 * funkce, ověřeno čtením zdrojů knihoven); na UI ani jádro nesahá.
 * Nejdelší pulz celé kopie spočítá jen první úloha nad kopií; nejdelší
 * pulz každého řádku (osa histogramu vybraného řádku) jedním průchodem
 * na řádek (řádky se mohou překrývat; jen pracovní
 * vlákno) - UI vlákno pak histogram staví jen
 * ze vzorku.
 *
 * @param job Úloha (vlákno vlastní jeden její odkaz).
 * @post Odkaz vlákna je puštěný; s_cad_workers o 1 menší.
 */
static void cad_worker_body ( st_CAD_JOB *job )
{
    const st_CMTTMZ_PULSES *p = job->pulses->p;
#ifdef MZTEST_HEADLESS
    /* testy mohou analyzátor nahradit (syntetický výsledek, pomalý analyzátor) */
    cmt_analyze_dialog_analyzer_fn fn = ( cmt_analyze_dialog_analyzer_fn ) g_atomic_pointer_get ( &s_cad_analyzer );
    job->result = fn ( p, &job->opts );
#else
    job->result = cmttmz_analyze_pulses ( p, &job->opts );
#endif
    if ( !g_atomic_int_get ( &job->pulses->max_known ) ) {
        job->pulses->max_us = cad_range_max_us ( p, 0, p->count );
        g_atomic_int_set ( &job->pulses->max_known, 1 );
#ifdef MZTEST_HEADLESS
        g_atomic_int_inc ( &s_cad_test_max_passes );
#endif
    }
    uint32_t n = cmttmz_analysis_row_count ( job->result );
    if ( n ) {
        job->row_max = g_new0 ( double, n );
        for ( uint32_t i = 0; i < n; i++ ) {
            const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row ( job->result, i );
            job->row_max[i] = cad_range_max_us ( p, r->first_pulse, r->end_pulse );
        }
    }
    g_atomic_int_set ( &job->done, 1 );
    g_atomic_int_add ( &s_cad_workers, -1 );
    cad_job_unref ( job );
}

/** @brief Vstup vlákna GLib. @param data Úloha. @return NULL. */
static gpointer cad_worker_thread ( gpointer data )
{
    cad_worker_body ( ( st_CAD_JOB * ) data );
    return NULL;
}

/**
 * @brief Nová úloha (1 odkaz volajícího, dosud nespuštěná).
 * @param pulses Kopie pulzů (úloha si vezme vlastní odkaz). @param opts Volby.
 * @return Úloha.
 */
static st_CAD_JOB *cad_job_new ( st_CAD_PULSES *pulses, const st_CMTTMZ_ANALYZE_OPTS *opts )
{
    st_CAD_JOB *job = g_new0 ( st_CAD_JOB, 1 );
    job->refs = 1;
    job->pulses = cad_pulses_ref ( pulses );
    job->opts = *opts;
    g_atomic_int_inc ( &s_cad_live_jobs );
    return job;
}

/**
 * @brief Spustí úlohu v pracovním vlákně (vlákno doběhne a odkaz pustí samo; UI ho po doběhnutí připojí, cad_threads_join()).
 * @param job Úloha (nespuštěná). @return true = spuštěná (vlákno v s_cad_threads); false = vlákno nevzniklo (úloha zůstává jen volajícímu).
 * @pre UI vlákno; s_cad_workers == 0 (jedna analýza najednou).
 */
static bool cad_job_start ( st_CAD_JOB *job )
{
    g_atomic_int_inc ( &job->refs );
    g_atomic_int_inc ( &s_cad_workers );
    job->started = true;
#ifdef MZTEST_HEADLESS
    s_cad_test_last_opts = job->opts;
    if ( s_cad_test_manual ) {
        s_cad_test_jobs.push_back ( job );
        return true;
    }
#endif
    GError *err = NULL;
    GThread *t = g_thread_try_new ( "cmt-analyze", cad_worker_thread, job, &err );
    if ( !t ) {
        if ( err ) g_error_free ( err );
        job->started = false;
        g_atomic_int_add ( &s_cad_workers, -1 );
        cad_job_unref ( job );
        return false;
    }
    /* vlákno se připojí po doběhnutí (cad_threads_join()); úlohu uvolní poslední odkaz */
    s_cad_threads.push_back ( t );
    return true;
}

/**
 * @brief Připojí doběhlá vlákna analýzy (všechna, když žádné neběží).
 *
 * s_cad_workers klesá na konci těla vlákna (po zápisu výsledku); zbývá
 * jen pustit odkaz a vrátit se, g_thread_join() tedy čeká nejvýš na tento
 * krátký zbytek.
 *
 * @pre UI vlákno.
 * @post Při s_cad_workers == 0 je s_cad_threads prázdné.
 */
static void cad_threads_join ( void )
{
    if ( s_cad_threads.empty ( ) || g_atomic_int_get ( &s_cad_workers ) != 0 ) return;
    for ( GThread *t : s_cad_threads ) g_thread_join ( t );
    s_cad_threads.clear ( );
}

/**
 * @brief Počká s mezí na doběhnutí vláken analýzy a připojí je; po mezi je odpojí (ukončení aplikace).
 *
 * Analýzu nejde zrušit (analyzátor zrušení nemá). Doběhnou-li vlákna do
 * @p max_ms, připojí se (g_thread_join()); jinak se odpojí
 * (g_thread_unref()) - doběhnou nad vlastní kopií pulzů, na UI nesahají,
 * data uvolní sama a proces je při konci ukončí.
 *
 * @param max_ms Nejdelší čekání v ms (0 = nečekat).
 * @return true = všechna vlákna připojená (nebo žádné); false = odpojená po mezi.
 * @pre UI vlákno. Řízené "vlákno" testu se nečeká (jeho úlohy vlákno nemají).
 * @post s_cad_threads je prázdné.
 */
static bool cad_threads_finish ( int max_ms )
{
    if ( s_cad_threads.empty ( ) ) return true;
    const gint64 until = g_get_monotonic_time ( ) + ( gint64 ) max_ms * 1000;
    while ( g_atomic_int_get ( &s_cad_workers ) != 0 && g_get_monotonic_time ( ) < until ) g_usleep ( 2000 );
    if ( g_atomic_int_get ( &s_cad_workers ) == 0 ) {
        cad_threads_join ( );
        return true;
    }
    for ( GThread *t : s_cad_threads ) g_thread_unref ( t );
    s_cad_threads.clear ( );
    return false;
}


/* ------------------------------------------------------------------------- */
/*  Čekající zásahy uplatnění                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Data jednoho zásahu uplatnění, která čte emu vlákno (cmt_request_analysis_apply()).
 *
 * Patří modulu, ne dialogu: zavření dialogu je neuvolní, dokud zásah může
 * běžet; dialog se po Add / Replace hned zavře a zásah
 * uživatele se nezruší. Uvolní je jen cad_apply_release() - po výsledku
 * zásahu se stejným seq, když fronta hlásí, že zásah už neprovede (ztracený
 * výsledek), nebo při ukončení aplikace (cmt_analyze_dialog_shutdown():
 * emu vlákno je už připojené a registr zásahů vymazaný).
 *
 * @invariant seq != 0, pulses, analysis a use platné až do cad_apply_release().
 */
typedef struct st_CAD_APPLY {
    uint32_t seq;                  /**< pořadové číslo zásahu */
    st_CAD_PULSES *pulses;         /**< kopie pulzů (vlastní odkaz) */
    st_CMTTMZ_ANALYSIS *analysis;  /**< analýza (vlastní ji držák) */
    uint8_t *use;                  /**< výběr řádků (g_malloc, vlastní ho držák) */
    int idle_frame;                /**< snímek, kdy byla fronta prázdná a výsledek nepřišel (-1 = ne) */
} st_CAD_APPLY;

/** @brief Čekající zásahy uplatnění (UI vlákno). */
static std::vector<st_CAD_APPLY> s_cad_applies;

/**
 * @brief Uvolní data zásahu uplatnění (jediné místo uvolnění).
 *
 * Nejdřív zásah zapomene v registru jádra (cmt_request_analysis_cancel()):
 * po výsledku tam už není (návrat false, nic se nestane), u ztraceného
 * neprovedeného zásahu tím registr přestane ukazovat na uvolňovaná data.
 *
 * @param ap Držák.
 * @pre Emu vlákno data už nečte: výsledek zásahu přišel, fronta zásah
 *      neprovede, nebo aplikace končí s připojeným emu vláknem.
 */
static void cad_apply_release ( st_CAD_APPLY *ap )
{
    ( void ) cmt_request_analysis_cancel ( ap->seq );
    cmttmz_analysis_free ( ap->analysis );
    ap->analysis = NULL;
    g_free ( ap->use );
    ap->use = NULL;
    cad_pulses_unref ( ap->pulses );
    ap->pulses = NULL;
}


/* ------------------------------------------------------------------------- */
/*  Stav dialogu                                                             */
/* ------------------------------------------------------------------------- */

/** @brief Stav dialogu. */
typedef enum en_CAD_STATE {
    CAD_STATE_NONE = 0, /**< dialog zavřený */
    CAD_STATE_COPYING,  /**< čeká na kopii pulzů (zásah fronty); ukazuje Analyzing... */
    CAD_STATE_WAITING,  /**< úloha čeká, až doběhne dřívější opuštěné vlákno; ukazuje Analyzing... */
    CAD_STATE_RUNNING,  /**< analýza běží v pracovním vlákně */
    CAD_STATE_DONE,     /**< výsledky jsou (i žádný soubor) */
    CAD_STATE_ERROR,    /**< chyba kopie nebo analýzy; text v error */
} en_CAD_STATE;

/** @brief Texty jednoho řádku výsledků pro tabulku (připravené jednou po analýze). */
typedef struct st_CAD_ROW_UI {
    std::string name;     /**< Name ("Unknown signal", "(no name)") */
    std::string type;     /**< Type */
    std::string format;   /**< Format */
    std::string start;    /**< Start (m:ss) */
    std::string size;     /**< Size ("1 024 B", "-") */
    std::string checksum; /**< Checksum */
    ImU32 checksum_color; /**< barva Checksum (chyba error, OK ztlumeně) */
    bool file;            /**< řádek souboru (má zaškrtnutí) */
    bool addable;         /**< soubor jde přidat (ADDABLE, ADDABLE_REPAIRED) */
} st_CAD_ROW_UI;

/**
 * @brief Stav dialogu (UI vlákno).
 *
 * @invariant state == NONE => job == NULL, pulses == NULL, analysis == NULL, rows prázdné.
 * @invariant state == DONE nebo ERROR s analýzou => rows.size() == use.size() == row_ui.size() == počet řádků analýzy.
 * @invariant job != NULL <=> state je WAITING nebo RUNNING.
 */
typedef struct st_CAD {
    en_CAD_STATE state;          /**< stav */
    bool open_pending;           /**< modál se má otevřít (OpenPopup v kontextu okna Virtual CMT) */
    int drawn_frame;             /**< snímek posledního cmt_analyze_dialog_draw() nebo cmt_analyze_dialog_skip_frame() (-1 = žádný) */
    bool skipped;                /**< okno kazety v některém snímku obsah nekreslilo (cmt_analyze_dialog_skip_frame()); příští kreslení ověří popup */
    int block_id;                /**< analyzovaný blok */
    unsigned tape_serial;        /**< čítač výměn pásky při otevření */
    bool unsaved;                /**< blok je neuložený (varování u Replace) */
    std::string block_name;      /**< druh bloku pro souhrn ("Raw recording") */
    double block_len_s;          /**< délka bloku v s (-1 neznámá) */
    uint32_t pulse_count;        /**< počet pulzů bloku */
    uint32_t pulses_seq;         /**< zásah kopie pulzů, na jehož výsledek se čeká (0 = žádný) */
    st_CAD_PULSES *pulses;       /**< kopie pulzů (odkaz dialogu) */
    st_CAD_JOB *job;             /**< úloha analýzy (odkaz dialogu) */
    double run_start;            /**< ImGui::GetTime() začátku analýzy */
    int tol_pct;                 /**< Leader tolerance v % (volba v dialogu) */
    bool rep_header;             /**< Repair damaged header */
    bool rep_body;               /**< Repair damaged body */
    bool rep_bsd;                /**< Repair incomplete BSD file */
    st_CMTTMZ_ANALYZE_OPTS opts_done; /**< volby, se kterými vznikl zobrazený výsledek */
    st_CMTTMZ_ANALYSIS *analysis; /**< výsledek (vlastní ho dialog) */
    std::vector<st_CMTTMZ_ANALYZE_ROW> rows; /**< kopie řádků analýzy */
    std::vector<st_CAD_ROW_UI> row_ui; /**< texty řádků */
    std::vector<uint8_t> use;    /**< zaškrtnutí řádků (1 = přidat) */
    int files;                   /**< počet řádků souborů */
    int files_bad;               /**< počet souborů s chybou součtu (hlavička nebo tělo) */
    int sel_row;                 /**< vybraný řádek tabulky (-1 = žádný) */
    ImGuiID nav_last;            /**< řádek, který měl naposledy fokus navigace (výběr šipkami) */
    std::string error;           /**< text chyby (stav ERROR) */
    bool details_open;           /**< oddíl Signal details rozbalený */
    bool details_drawn_open;     /**< oddíl Signal details byl rozbalený v minulém nakresleném snímku (rozbalení = posun těla na histogram) */
    float table_extra_h;         /**< výška vodorovného posuvníku tabulky z minulého snímku (0 = bez něj); tabulka je o ni vyšší */
    std::vector<double> row_max; /**< nejdelší pulz rozsahu každého řádku v µs (z pracovního vlákna, osa histogramu řádku) */
    int focus_add_frames;        /**< ještě tolik snímků dát fokus navigace tlačítku Add (otevření, hotová analýza); klesá každý snímek */
    int recenter_frames;         /**< ještě tolik snímků umístit modál doprostřed okna (otevření, změna velikosti; ne tažení) */
    int scroll_hist_frames;      /**< ještě tolik snímků posunout tělo tak, aby byl histogram s osou celý vidět (rozbalení Signal details) */
    bool body_scroll_reset;      /**< příští kreslení posune tělo nahoru (nový výsledek bez rozbaleného histogramu) */
    std::vector<float> body_marks; /**< horní okraje řádkových prvků těla (místní y v obsahu těla) z minulého snímku: hrany posouvané oblasti jen mezi řádky */
    std::vector<float> body_marks_cur; /**< horní okraje řádkových prvků rozpracovaného snímku */
    bool marking;                /**< kreslí se posouvané tělo (cad_mark() zapisuje do body_marks_cur) */
    ImGuiWindow *body_win;       /**< okno posouvaného těla v rozpracovaném snímku (platné jen při marking) */
    bool table_own_scroll;       /**< tabulka v tomto snímku má vlastní svislý posun (tělo se pak neposouvá) */
    ImGuiID popup_id;            /**< ID popupu modálu (kontext okna Virtual CMT), 0 = ještě neotevřený; zavření mimo modál (imgui_cmt_close_popup()) */
    float nontable_h;            /**< výška obsahu posouvaného těla mimo tabulku z minulého snímku (0 = neznámá) */
    float body_content_h;        /**< výška obsahu posouvaného těla z minulého snímku (0 = neznámá) */
    float footer_h;              /**< výška patičky (varování a tlačítka) z minulého snímku (0 = neznámá) */
    float top_h;                 /**< výška horní části (věta výsledku a souhrn) z minulého snímku (0 = neznámá) */
    float plot_h_used;           /**< výška plochy histogramu nakreslená v minulém snímku (0 = nekreslila se); je v nontable_h */
    float plot_h;                /**< výška plochy histogramu pro tento snímek (2 až 4 řádky textu podle místa) */
    ImVec2 last_size;            /**< velikost modálu z minulého snímku */
    int result_serial;           /**< čítač výsledků (klíč mezipaměti šířek a histogramu) */
    /* mezipaměť šířek sloupců */
    float col_w[CAD_COL_COUNT];  /**< šířky obsahu sloupců */
    int col_key_serial;          /**< result_serial šířek (-1 = neplatné) */
    bool table_scroll_reset;     /**< příští kreslení tabulky ji posune na první řádek (nový výsledek) */
    float col_key_font;          /**< velikost písma šířek */
    bool col_key_stretch;        /**< prodloužení textů (testy) při výpočtu šířek */
    /* mezipaměť histogramu */
    std::vector<uint32_t> hist_bins; /**< CAD_HIST_BINS binů na ose + 1 bin delších pulzů */
    double hist_axis_us;         /**< horní mez osy v µs */
    bool hist_overflow;          /**< některé pulzy jsou delší než osa */
    bool hist_valid;             /**< histogram spočítaný */
    int hist_key_row;            /**< řádek histogramu (-1 = celý záznam) */
    int hist_key_serial;         /**< result_serial histogramu */
    st_CAD_PULSES *hist_key_pulses; /**< kopie pulzů histogramu */
} st_CAD;

/** @brief Stav dialogu (UI vlákno). */
static st_CAD s_cad;

/** @brief Inicializace stavu při prvním použití (statické pole s std::vector se inicializuje konstruktorem). */
static bool s_cad_init = false;


/* ------------------------------------------------------------------------- */
/*  Analýza na pozadí při hraní surového bloku                               */
/* ------------------------------------------------------------------------- */

/** @brief Stav analýzy na pozadí (cmt_analyze_dialog_auto()). */
typedef enum en_CAD_AUTO_STATE {
    CAD_AUTO_IDLE = 0, /**< nic (klíč neplatí) */
    CAD_AUTO_COPYING,  /**< čeká na kopii pulzů (zásah fronty) */
    CAD_AUTO_WAITING,  /**< úloha čeká na volné pracovní vlákno */
    CAD_AUTO_RUNNING,  /**< analýza běží */
    CAD_AUTO_READY,    /**< výsledek je připravený (result != NULL) */
    CAD_AUTO_SPENT,    /**< blok klíče už vyřízen: výsledek převzal dialog, kopie nebo analýza selhala; znovu se nespouští */
} en_CAD_AUTO_STATE;

/**
 * @brief Analýza surového bloku na pozadí, zatímco hraje (UI vlákno).
 *
 * Klíč (tape_serial, block_id, pulse_count) určuje blok; jiná páska nebo
 * jiný hrající surový blok stav zahodí a začne znovu. Výsledek čeká na
 * oznámení (blok dohrál nebo se páska zastavila) a na otevření dialogu
 * analýzy téhož bloku, který ho převezme bez nové analýzy.
 *
 * @invariant state == IDLE => pulses, job i result jsou NULL.
 * @invariant job != NULL <=> state je WAITING nebo RUNNING; result != NULL <=> state == READY.
 * @invariant pulses != NULL <=> state je WAITING, RUNNING nebo READY.
 */
typedef struct st_CAD_AUTO {
    en_CAD_AUTO_STATE state;     /**< stav */
    unsigned tape_serial;        /**< klíč: čítač výměn pásky */
    int block_id;                /**< klíč: blok */
    uint32_t pulse_count;        /**< klíč: počet pulzů bloku z kopie stavu okna */
    uint32_t pulses_seq;         /**< zásah kopie pulzů, na jehož výsledek se čeká (0 = žádný) */
    st_CAD_PULSES *pulses;       /**< kopie pulzů (vlastní odkaz) */
    st_CAD_JOB *job;             /**< úloha (vlastní odkaz) */
    st_CMTTMZ_ANALYSIS *result;  /**< hotová analýza (vlastní ji tento stav) */
    std::vector<double> row_max; /**< nejdelší pulz rozsahu každého řádku v µs */
    int addable;                 /**< počet řádků výsledku, které jdou přidat */
    bool announced;              /**< nález už byl ohlášen v řádku zpráv */
} st_CAD_AUTO;

/** @brief Stav analýzy na pozadí (UI vlákno). */
static st_CAD_AUTO s_cad_auto;

/**
 * @brief Zahodí data analýzy na pozadí (běžící úloha doběhne a uvolní se sama) a nastaví stav.
 * @param state Nový stav: CAD_AUTO_IDLE (klíč neplatí) nebo CAD_AUTO_SPENT (klíč zůstává, blok se znovu neanalyzuje).
 */
static void cad_auto_drop ( en_CAD_AUTO_STATE state )
{
    cad_job_unref ( s_cad_auto.job );
    s_cad_auto.job = NULL;
    cmttmz_analysis_free ( s_cad_auto.result );
    s_cad_auto.result = NULL;
    cad_pulses_unref ( s_cad_auto.pulses );
    s_cad_auto.pulses = NULL;
    s_cad_auto.row_max.clear ( );
    s_cad_auto.pulses_seq = 0;
    s_cad_auto.addable = 0;
    s_cad_auto.state = state;
}

/**
 * @brief Patří stav analýzy na pozadí danému bloku?
 * @param tape_serial Čítač výměn pásky. @param block_id Blok. @param pulse_count Počet pulzů bloku.
 * @return true = klíč souhlasí a stav není IDLE.
 */
static bool cad_auto_is ( unsigned tape_serial, int block_id, uint32_t pulse_count )
{
    return s_cad_auto.state != CAD_AUTO_IDLE && s_cad_auto.tape_serial == tape_serial && s_cad_auto.block_id == block_id
        && s_cad_auto.pulse_count == pulse_count;
}

/**
 * @brief Nastaví výchozí hodnoty polí, která nemají konstruktor (jednou).
 */
static void cad_init_once ( void )
{
    if ( s_cad_init ) return;
    s_cad_init = true;
    s_cad.state = CAD_STATE_NONE;
    s_cad.drawn_frame = -1;
    s_cad.block_id = -1;
    s_cad.sel_row = -1;
    s_cad.col_key_serial = -1;
    s_cad.hist_key_serial = -1;
    s_cad.tol_pct = CAD_TOL_DEFAULT;
}


#ifdef MZTEST_HEADLESS
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cad_items;      /**< prvky rozpracovaného snímku */
static std::vector<st_CMT_WINDOW_TEST_ITEM> s_cad_items_done; /**< prvky posledního nakresleného snímku */
static ImVec2 s_cad_wmin, s_cad_wmax, s_cad_imin, s_cad_imax; /**< obdélník modálu a vnitřku z posledního snímku */
static bool s_cad_drawn = false;                               /**< modál se v posledním snímku kreslil */
static std::string s_cad_title;                                /**< titulek z posledního snímku */
static bool s_cad_in_body = false;                             /**< záznam prvků posouvaného těla dialogu (svisle se smí posunout mimo ořez) */
static ImVec2 s_cad_body_min, s_cad_body_max;                  /**< ořez posouvaného těla z posledního snímku */
static bool s_cad_body_scroll = false;                         /**< tělo mělo v posledním snímku svislý posuvník */
static bool s_cad_table_scroll = false;                        /**< tabulka měla v posledním snímku vlastní svislý posuvník */
static ImGuiID s_cad_table_id = 0;                             /**< ID tabulky výsledků z posledního snímku (0 = nekreslila se) */
#endif

/**
 * @brief Zapamatuje horní okraj řádkového prvku posouvaného těla (místní y v obsahu těla).
 *
 * Výška těla a posun na histogram se pak volí tak, aby hrana posouvané
 * oblasti ležela mezi řádky, ne uprostřed textu.
 *
 * @param y_screen Horní okraj prvku na obrazovce (výchozí: poloha kurzoru).
 * @pre Uvnitř okna těla; jen při s_cad.marking (jinak nic).
 */
static void cad_mark ( float y_screen = -FLT_MAX )
{
    if ( !s_cad.marking || !s_cad.body_win ) return;
    ImGuiWindow *w = s_cad.body_win;
    if ( y_screen == -FLT_MAX ) y_screen = ImGui::GetCursorScreenPos ( ).y;
    s_cad.body_marks_cur.push_back ( y_screen - w->Pos.y + w->Scroll.y );
}

/**
 * @brief Zaznamená prvek dialogu pro testy (v aplikaci nic).
 *
 * Prvky posouvaného těla (věta, tabulka, volby, Signal details) se
 * zaznamenají jako posouvané svisle (scroll_y), i když @p scroll je false.
 *
 * @param name Jméno. @param text Text. @param min Levý horní roh. @param max Pravý dolní roh.
 * @param container Obal jiných prvků (z kontroly překryvu se vynechá). @param scroll Prvek v posouvané oblasti (řádek tabulky).
 * @param disabled Zakázaný. @param reason Důvod zákazu nebo tooltip (NULL = žádný).
 * @pre Uvnitř modálu (ořez = GetCurrentWindow()->ClipRect).
 */
static void cad_record ( const char *name, const std::string &text, ImVec2 min, ImVec2 max, bool container, bool scroll, bool disabled,
                         const char *reason )
{
#ifdef MZTEST_HEADLESS
    st_CMT_WINDOW_TEST_ITEM it;
    it.name = name;
    it.text = text;
    it.disabled = disabled;
    it.min = min;
    it.max = max;
    ImGuiWindow *w = ImGui::GetCurrentWindow ( );
    it.clip_min = w->ClipRect.Min;
    it.clip_max = w->ClipRect.Max;
    it.container = container;
    it.deck = false;
    it.right = false;
    it.scroll_y = scroll || s_cad_in_body;
    it.popup = true;
    it.reason = reason ? reason : "";
    s_cad_items.push_back ( it );
#else
    ( void ) name;
    ( void ) text;
    ( void ) min;
    ( void ) max;
    ( void ) container;
    ( void ) scroll;
    ( void ) disabled;
    ( void ) reason;
#endif
}

/** @brief Zaznamená poslední položku ImGui (testy). @param name Jméno. @param text Text. @param disabled Zakázaná. @param reason Důvod. */
static void cad_record_item ( const char *name, const std::string &text, bool disabled, const char *reason )
{
    cad_record ( name, text, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false, false, disabled, reason );
}

/** @brief Zaznamená pomocný údaj bez obdélníku (stav, ztlumení, fokus; testy). @param name Jméno. @param text Hodnota. */
static void cad_record_value ( const char *name, const std::string &text )
{
    cad_record ( name, text, ImVec2 ( 0.0f, 0.0f ), ImVec2 ( 0.0f, 0.0f ), false, false, false, NULL );
}

/**
 * @brief Jméno prvku s indexem pro testy ("ad_name_2"); v aplikaci jen základ (bez formátování).
 * @param buf Pole znaků pro jméno (jen testy; sizeof musí dát jeho velikost).
 * @param base Základ jména. @param i Index (v aplikaci se jen vyhodnotí).
 * @return Jméno prvku (const char *).
 */
#ifdef MZTEST_HEADLESS
#define CAD_NAME( buf, base, i ) ( snprintf ( buf, sizeof ( buf ), "%s_%d", base, ( int ) ( i ) ), buf )
#else
#define CAD_NAME( buf, base, i ) ( ( void ) ( i ), base )
#endif


/* ------------------------------------------------------------------------- */
/*  Texty                                                                    */
/* ------------------------------------------------------------------------- */

/** @brief Číslo s tisíci oddělenými mezerou ("60 000", jako okno, SPEC K27). @param value Číslo. @return Text. */
static std::string cad_thousands ( uint64_t value )
{
    char raw[32];
    snprintf ( raw, sizeof ( raw ), "%llu", ( unsigned long long ) value );
    std::string out;
    size_t len = strlen ( raw );
    for ( size_t i = 0; i < len; i++ ) {
        if ( i > 0 && ( len - i ) % 3 == 0 ) out += ' ';
        out += raw[i];
    }
    return out;
}

/** @brief Čas m:ss (zaokrouhlený na sekundy). @param s Čas v s (záporný = "-"). @return Text. */
static std::string cad_time_text ( double s )
{
    if ( s < 0.0 ) return std::string ( "-" );
    long long t = ( long long ) floor ( s + 0.5 );
    char buf[32];
    snprintf ( buf, sizeof ( buf ), "%lld:%02lld", t / 60, t % 60 );
    return std::string ( buf );
}

/** @brief Text "páska se změnila" (SPEC): důvod zákazu akcí i varování dialogu. @return Přeložený text. */
static const char *cad_text_tape_changed ( void )
{
    return _ ( "The tape changed. Close this dialog and analyze again." );
}

/** @brief Text zastaralých výsledků (SPEC): tooltip akcí a řádek nápovědy. @return Přeložený text. */
static const char *cad_text_stale ( void )
{
    return _ ( "Analyze again to apply the new options." );
}

static bool cad_row_addable ( const st_CMTTMZ_ANALYZE_ROW *r );

/**
 * @brief Text sloupce Result podle výsledku řádku a zaškrtnutí.
 * @param r Řádek. @param use Zaškrtnutí. @param[out] color Barva textu.
 * @return Přeložený text.
 */
static const char *cad_result_text ( const st_CMTTMZ_ANALYZE_ROW *r, bool use, ImU32 *color )
{
    *color = CMT_TOK_TEXT_DIM;
    if ( r->kind != CMTTMZ_ANALYZE_FILE ) return _ ( "Stays raw" );
    switch ( r->result ) {
        case CMTTMZ_ANALYZE_ADDABLE:
            if ( !use ) return _ ( "Stays raw" );
            *color = CMT_TOK_TEXT;
            return _ ( "Will be added" );
        case CMTTMZ_ANALYZE_ADDABLE_REPAIRED:
            if ( !use ) return _ ( "Stays raw" );
            *color = CMT_TOK_TEXT;
            return _ ( "Will be added (repaired)" );
        case CMTTMZ_ANALYZE_CANNOT_ADD:
            /* warn jen u Cannot add (barva varování jen pro varování) */
            *color = CMT_TOK_WARN;
            return _ ( "Cannot add" );
        case CMTTMZ_ANALYZE_STAYS_RAW:
        default:
            return _ ( "Stays raw" );
    }
}

/**
 * @brief Vysvětlení řádku pro tooltip výsledku a řádek nápovědy (bez jména).
 *
 * Důvod jádra (cmttmz_analyze_reason_text(), anglicky, nepřekládá se),
 * u opraveného souboru s popisem opravy; u standardního souboru, který se
 * přidá bez opravy, věta o rychlosti nového bloku; u souboru jiného formátu
 * (TURBO, FASTIPL, BSD, CP/M), který se přidá, věta o profilu zapisovače
 * (st_CMTTMZ_ANALYZE_ROW::writer_profile).
 *
 * @param r Řádek. @param speed_text Výchozí rychlost decku ("1:1").
 * @return Text (bez tečky na konci u textu jádra).
 */
static std::string cad_row_detail ( const st_CMTTMZ_ANALYZE_ROW *r, const std::string &speed_text )
{
    const bool standard = r->format_name && 0 == strcmp ( r->format_name, "NORMAL" );
    if ( r->kind == CMTTMZ_ANALYZE_FILE && r->result == CMTTMZ_ANALYZE_ADDABLE && standard ) {
        char buf[160];
        snprintf ( buf, sizeof ( buf ), _ ( "Stored as a standard tape block at speed %s." ), speed_text.c_str ( ) );
        return std::string ( buf );
    }
    if ( r->kind == CMTTMZ_ANALYZE_FILE && cad_row_addable ( r ) && !standard ) {
        /* jiný formát: blok vyrobí profil zapisovače znovu, časování nahrávky se nezachová */
        char buf[320];
        snprintf ( buf, sizeof ( buf ), _ ( "Stored as a %s tape block built by the writer profile %s; the signal is generated again, the timing of the recording is not kept." ),
                   r->format_name ? r->format_name : "", ( r->writer_profile && r->writer_profile[0] ) ? r->writer_profile : "-" );
        std::string t ( buf );
        if ( r->result == CMTTMZ_ANALYZE_ADDABLE_REPAIRED && r->recovery_text && r->recovery_text[0] ) {
            t += " (";
            t += r->recovery_text;
            t += ")";
        }
        return t;
    }
    std::string t = cmttmz_analyze_reason_text ( r->reason );
    if ( r->result == CMTTMZ_ANALYZE_ADDABLE_REPAIRED && r->recovery_text && r->recovery_text[0] ) {
        t += " (";
        t += r->recovery_text;
        t += ")";
    }
    return t;
}

/**
 * @brief Věta řádku nápovědy u souboru, který zůstane surový: že ho přidat nejde a co zkusit.
 *
 * Jen soubor s výsledkem Stays raw (Cannot add má důvod formátu, který
 * žádná volba nezmění). Chyba součtu těla nebo neúplné tělo: pokus
 * s opravou Repair damaged body, není-li ve voleb výsledku zapnutá (věta
 * jako pokus - oprava nemusí pomoci); data, která potřebují jinou
 * vypnutou opravu: pokus s volbami oprav; jinak jen "Cannot be added.".
 *
 * @param r Řádek. @return Přeložená věta ("" = žádná).
 */
static std::string cad_row_remedy ( const st_CMTTMZ_ANALYZE_ROW *r )
{
    if ( r->kind != CMTTMZ_ANALYZE_FILE || r->result != CMTTMZ_ANALYZE_STAYS_RAW ) return std::string ( );
    char buf[256];
    if ( ( r->reason == CMTTMZ_ANALYZE_REASON_BODY_CHECKSUM || r->reason == CMTTMZ_ANALYZE_REASON_INCOMPLETE ) && !s_cad.opts_done.repair_body ) {
        snprintf ( buf, sizeof ( buf ), _ ( "Cannot be added. You can try \"%s\" and Analyze again." ), _ ( "Repair damaged body" ) );
        return std::string ( buf );
    }
    if ( r->reason == CMTTMZ_ANALYZE_REASON_REPAIR_DISABLED ) return std::string ( _ ( "Cannot be added. You can try the repair options and Analyze again." ) );
    return std::string ( _ ( "Cannot be added." ) );
}

/** @brief Je soubor řádku přidatelný (zaškrtnutí povolené)? @param r Řádek. @return true = ano. */
static bool cad_row_addable ( const st_CMTTMZ_ANALYZE_ROW *r )
{
    return r->kind == CMTTMZ_ANALYZE_FILE && ( r->result == CMTTMZ_ANALYZE_ADDABLE || r->result == CMTTMZ_ANALYZE_ADDABLE_REPAIRED );
}

/**
 * @brief Připraví texty řádku tabulky.
 * @param r Řádek analýzy. @param[out] u Texty.
 */
static void cad_row_ui ( const st_CMTTMZ_ANALYZE_ROW *r, st_CAD_ROW_UI *u )
{
    u->file = ( r->kind == CMTTMZ_ANALYZE_FILE );
    u->addable = cad_row_addable ( r );
    const bool mzf = u->file && r->reason != CMTTMZ_ANALYZE_REASON_NO_MZF;
    if ( !u->file ) u->name = _ ( "Unknown signal" );
    else if ( r->name[0] ) u->name = r->name;
    else u->name = _ ( "(no name)" );
    u->type = mzf ? cmt_view_mzf_type_text ( r->ftype ) : std::string ( "-" );
    if ( !u->file ) u->format = "-";
    else if ( r->format_name && 0 == strcmp ( r->format_name, "NORMAL" ) ) u->format = _ ( "Sharp standard" );
    else u->format = r->format_name ? r->format_name : "-";
    u->start = cad_time_text ( ( double ) r->start_us / 1e6 );
    u->size = u->file ? cad_thousands ( r->size ) + " B" : std::string ( "-" );
    u->checksum_color = CMT_TOK_TEXT_DIM;
    if ( !u->file ) {
        u->checksum = "-";
    } else if ( r->header_checksum == CMTTMZ_ANALYZE_CHECKSUM_ERROR ) {
        u->checksum = _ ( "Header error" );
        u->checksum_color = CMT_TOK_ERROR;
    } else if ( r->body_checksum == CMTTMZ_ANALYZE_CHECKSUM_ERROR ) {
        u->checksum = _ ( "Body error" );
        u->checksum_color = CMT_TOK_ERROR;
    } else if ( r->header_checksum == CMTTMZ_ANALYZE_CHECKSUM_OK && r->body_checksum == CMTTMZ_ANALYZE_CHECKSUM_OK ) {
        u->checksum = _ ( "OK" );
    } else {
        /* součet některé části není k dispozici (tělo chybí, formát ho nemá): OK by tvrdilo víc, než víme */
        u->checksum = "-";
    }
}

/** @brief Popisek sloupce tabulky (přeložený; sloupec zaškrtnutí bez textu). @param c Sloupec. @return Text. */
static const char *cad_col_label ( int c )
{
    switch ( c ) {
        case CAD_COL_USE: return "##use";
        case CAD_COL_NUM: return "#";
        case CAD_COL_NAME: return _ ( "Name" );
        case CAD_COL_TYPE: return _ ( "Type" );
        /* kontext: sloupce tabulky analýzy ("Format", "Start" jsou v katalogu i jinde, jiný význam) */
        case CAD_COL_FORMAT: return C_ ( "Analysis column", "Format" );
        case CAD_COL_START: return C_ ( "Analysis column", "Start" );
        case CAD_COL_SIZE: return _ ( "Size" );
        case CAD_COL_CHECKSUM: return _ ( "Checksum" );
        case CAD_COL_RESULT:
        default: return _ ( "Result" );
    }
}

std::string cmt_analyze_dialog_added_message ( int count )
{
    if ( count < 0 ) return std::string ( _ ( "Files added from the recording." ) );
    char buf[128];
    snprintf ( buf, sizeof ( buf ), _ ( "Files added from the recording: %d" ), count );
    return std::string ( buf );
}


/* ------------------------------------------------------------------------- */
/*  Životní cyklus dialogu                                                   */
/* ------------------------------------------------------------------------- */

/** @brief Volby analýzy z ovládacích prvků dialogu. @return Volby. */
static st_CMTTMZ_ANALYZE_OPTS cad_opts_ui ( void )
{
    st_CMTTMZ_ANALYZE_OPTS o;
    cmttmz_analyze_opts_default ( &o );
    o.tolerance = ( double ) s_cad.tol_pct / 100.0;
    o.repair_header = s_cad.rep_header ? 1 : 0;
    o.repair_body = s_cad.rep_body ? 1 : 0;
    o.repair_bsd = s_cad.rep_bsd ? 1 : 0;
    return o;
}

/** @brief Liší se volby dialogu od voleb zobrazeného výsledku (zastaralé výsledky)? @return true = ano. */
static bool cad_stale ( void )
{
    if ( s_cad.state != CAD_STATE_DONE ) return false;
    st_CMTTMZ_ANALYZE_OPTS o = cad_opts_ui ( );
    return fabs ( o.tolerance - s_cad.opts_done.tolerance ) > 1e-9 || o.repair_header != s_cad.opts_done.repair_header
        || o.repair_body != s_cad.opts_done.repair_body || o.repair_bsd != s_cad.opts_done.repair_bsd;
}

/** @brief Zahodí výsledek analýzy dialogu (řádky, výběr, analýzu). */
static void cad_clear_result ( void )
{
    cmttmz_analysis_free ( s_cad.analysis );
    s_cad.analysis = NULL;
    s_cad.rows.clear ( );
    s_cad.row_max.clear ( );
    s_cad.row_ui.clear ( );
    s_cad.use.clear ( );
    s_cad.files = s_cad.files_bad = 0;
    s_cad.sel_row = -1;
    s_cad.error.clear ( );
    s_cad.result_serial++;
}

/**
 * @brief Opustí dialog: úloha se pustí (spuštěná doběhne a uvolní se sama), kopie pulzů a výsledek se uvolní.
 * @post s_cad.state == CAD_STATE_NONE; čekající zásahy uplatnění se nemění; popup modálu se zavře (imgui_cmt_close_popup()).
 */
static void cad_reset ( void )
{
    cad_init_once ( );
    /* opuštění mimo modál (zavřené okno, hlídač, test) nesmí nechat modál v zásobníku popupů */
    imgui_cmt_close_popup ( &s_cad.popup_id );
    cad_job_unref ( s_cad.job );
    s_cad.job = NULL;
    cad_clear_result ( );
    cad_pulses_unref ( s_cad.pulses );
    s_cad.pulses = NULL;
    s_cad.hist_key_pulses = NULL;
    s_cad.hist_valid = false;
    s_cad.state = CAD_STATE_NONE;
    s_cad.open_pending = false;
    s_cad.pulses_seq = 0;
    s_cad.block_id = -1;
}

/**
 * @brief Spustí analýzu nad kopií pulzů dialogu s volbami dialogu.
 *
 * Běží-li ještě dřívější (opuštěné) vlákno, úloha počká (stav WAITING)
 * a spustí ji cad_poll() - nejvýš jedna analýza najednou.
 *
 * @pre s_cad.pulses != NULL, s_cad.job == NULL.
 */
static void cad_start_analysis ( void )
{
    cad_clear_result ( );
    st_CMTTMZ_ANALYZE_OPTS o = cad_opts_ui ( );
    s_cad.job = cad_job_new ( s_cad.pulses, &o );
    s_cad.state = CAD_STATE_WAITING;
    s_cad.run_start = ImGui::GetCurrentContext ( ) ? ImGui::GetTime ( ) : 0.0;
}

/**
 * @brief Převezme hotovou analýzu: řádky, zaškrtnutí, texty, nejdelší pulzy řádků, stav.
 * @param a Analýza (převezme ji dialog; nesmí být NULL). @param opts Volby, se kterými vznikla.
 * @param row_max Nejdelší pulz rozsahu každého řádku v µs (z úlohy; NULL při 0 řádcích); kopíruje se.
 */
static void cad_take_result ( st_CMTTMZ_ANALYSIS *a, const st_CMTTMZ_ANALYZE_OPTS *opts, const double *row_max )
{
    cad_clear_result ( );
    s_cad.analysis = a;
    s_cad.opts_done = *opts;
    s_cad.table_scroll_reset = true;
    uint32_t n = cmttmz_analysis_row_count ( a );
    for ( uint32_t i = 0; i < n; i++ ) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row ( a, i );
        s_cad.rows.push_back ( *r );
        s_cad.row_max.push_back ( row_max ? row_max[i] : 0.0 );
        st_CAD_ROW_UI u;
        cad_row_ui ( r, &u );
        s_cad.row_ui.push_back ( u );
        /* výchozí: přidá se všechno, co jde přidat */
        s_cad.use.push_back ( u.addable ? 1 : 0 );
        if ( u.file ) {
            s_cad.files++;
            if ( r->header_checksum == CMTTMZ_ANALYZE_CHECKSUM_ERROR || r->body_checksum == CMTTMZ_ANALYZE_CHECKSUM_ERROR ) s_cad.files_bad++;
        }
    }
    /* text chyby analyzátoru (anglicky, jádro; čísla bloků v něm nejsou) */
    s_cad.error = cmttmz_analysis_error ( a );
    s_cad.state = s_cad.error.empty ( ) ? CAD_STATE_DONE : CAD_STATE_ERROR;
    /* nic nenalezeno: Signal details se rozbalí samy (koncept empty-v2) */
    if ( s_cad.state == CAD_STATE_DONE && s_cad.files == 0 ) s_cad.details_open = true;
    /* rozbalený histogram: tělo se posune na něj (osa celá vidět); jinak tělo od začátku */
    if ( s_cad.details_open ) s_cad.scroll_hist_frames = 3;
    else s_cad.body_scroll_reset = true;
    s_cad.focus_add_frames = 3;
}

/**
 * @brief Posun úlohy: připojí doběhlá vlákna, spustí čekající úlohu, převezme hotovou (každý snímek).
 *
 * Čekající úloha (WAITING) se spustí, jen když neběží žádné pracovní
 * vlákno (ani dřívější opuštěné) - nejvýš jedna analýza najednou.
 *
 * @pre UI vlákno.
 */
static void cad_poll ( void )
{
    cad_threads_join ( );
    if ( s_cad.state == CAD_STATE_WAITING && s_cad.job && g_atomic_int_get ( &s_cad_workers ) == 0 ) {
        if ( cad_job_start ( s_cad.job ) ) {
            s_cad.state = CAD_STATE_RUNNING;
        } else {
            cad_job_unref ( s_cad.job );
            s_cad.job = NULL;
            s_cad.state = CAD_STATE_ERROR;
            s_cad.error = _ ( "The analysis could not be started." );
        }
    }
    if ( s_cad.state == CAD_STATE_RUNNING && s_cad.job && g_atomic_int_get ( &s_cad.job->done ) ) {
        st_CAD_JOB *job = s_cad.job;
        st_CMTTMZ_ANALYSIS *a = job->result;
        job->result = NULL;
        st_CMTTMZ_ANALYZE_OPTS o = job->opts;
        s_cad.job = NULL;
        if ( a ) cad_take_result ( a, &o, job->row_max );
        cad_job_unref ( job );
        if ( !a ) {
            cad_clear_result ( );
            s_cad.state = CAD_STATE_ERROR;
            s_cad.error = _ ( "The analysis failed: not enough memory." );
        }
    }
}


void cmt_analyze_dialog_open ( const st_CMT_VIEW *v, int block_id )
{
    cad_init_once ( );
    cad_reset ( );
    if ( !v || block_id < 0 || block_id >= v->block_count ) return;
    const st_CMT_VIEW_BLOCK *b = &v->blocks[( size_t ) block_id];
    s_cad.block_id = block_id;
    s_cad.tape_serial = v->tape_serial;
    s_cad.unsaved = b->unsaved;
    s_cad.block_name = b->name;
    s_cad.block_len_s = b->length_s;
    s_cad.pulse_count = b->pulse_count;
    s_cad.tol_pct = CAD_TOL_DEFAULT;
    s_cad.rep_header = s_cad.rep_body = s_cad.rep_bsd = false;
    s_cad.details_open = false;
    s_cad.details_drawn_open = false;
    s_cad.table_extra_h = 0.0f;
    s_cad.nontable_h = 0.0f;
    s_cad.plot_h_used = 0.0f;
    s_cad.body_content_h = s_cad.footer_h = s_cad.top_h = 0.0f;
    s_cad.last_size = ImVec2 ( 0.0f, 0.0f );
    s_cad.nav_last = 0;
    s_cad.run_start = ImGui::GetCurrentContext ( ) ? ImGui::GetTime ( ) : 0.0;
    const bool from_auto = cad_auto_is ( v->tape_serial, block_id, b->pulse_count )
        && ( s_cad_auto.state == CAD_AUTO_WAITING || s_cad_auto.state == CAD_AUTO_RUNNING || s_cad_auto.state == CAD_AUTO_READY );
    if ( !from_auto ) {
        s_cad.pulses_seq = cmt_request_analysis_pulses ( v->tape_serial, block_id );
        s_cad.state = CAD_STATE_COPYING;
    }
    s_cad.open_pending = true;
    s_cad.skipped = false;
    s_cad.focus_add_frames = 3;
    s_cad.scroll_hist_frames = 0;
    s_cad.body_scroll_reset = false;
    s_cad.body_marks.clear ( );
    /* otevření = objevení modálu: vystředit (dál jen při změně velikosti) */
    s_cad.recenter_frames = 3;
    if ( from_auto ) {
        /* analýza téhož bloku na pozadí (výchozí volby): dialog převezme kopii pulzů a hotový výsledek
         * nebo rozběhnutou úlohu, nová kopie ani analýza se nedělá */
        st_CMTTMZ_ANALYZE_OPTS o;
        cmttmz_analyze_opts_default ( &o );
        s_cad.pulses = s_cad_auto.pulses;
        s_cad_auto.pulses = NULL;
        s_cad.pulse_count = s_cad.pulses->p->count;
        if ( s_cad_auto.state == CAD_AUTO_READY ) {
            st_CMTTMZ_ANALYSIS *a = s_cad_auto.result;
            s_cad_auto.result = NULL;
            cad_take_result ( a, &o, s_cad_auto.row_max.empty ( ) ? NULL : s_cad_auto.row_max.data ( ) );
        } else {
            s_cad.job = s_cad_auto.job;
            s_cad_auto.job = NULL;
            s_cad.state = ( s_cad_auto.state == CAD_AUTO_RUNNING ) ? CAD_STATE_RUNNING : CAD_STATE_WAITING;
        }
        cad_auto_drop ( CAD_AUTO_SPENT );
        return;
    }
    if ( !s_cad.pulses_seq ) {
        s_cad.state = CAD_STATE_ERROR;
        s_cad.error = _ ( "The recording could not be read." );
    }
}


void cmt_analyze_dialog_auto ( const st_CMT_VIEW *v, bool enabled )
{
    cad_init_once ( );
    if ( !v ) return;
    /* jiná páska nebo vypnutá volba: nic se nedrží */
    if ( s_cad_auto.state != CAD_AUTO_IDLE && ( !enabled || !v->has_tape || v->tape_serial != s_cad_auto.tape_serial ) ) {
        cad_auto_drop ( CAD_AUTO_IDLE );
    }
    if ( !enabled ) return;

    /* hraje surový blok (v každém režimu načítání; ne při záznamu vstupů a jeho přehrávání - kopii pulzů by fronta odmítla) */
    const st_CMT_VIEW_BLOCK *cur = ( v->has_tape && v->current_block >= 0 && v->current_block < v->block_count )
        ? &v->blocks[( size_t ) v->current_block] : NULL;
    const bool playing_raw = cur && v->state == CMT_VIEW_STATE_PLAY && cur->kind == CMT_VIEW_KIND_RAW && cur->pulse_count > 0
        && !v->replay_recording && !v->replay_playing;
    /* dialog analýzy je otevřený: uživatel analyzuje sám, na pozadí se nic nového nespouští */
    if ( playing_raw && s_cad.state == CAD_STATE_NONE && !cad_auto_is ( v->tape_serial, cur->id, cur->pulse_count ) ) {
        cad_auto_drop ( CAD_AUTO_IDLE );
        s_cad_auto.tape_serial = v->tape_serial;
        s_cad_auto.block_id = cur->id;
        s_cad_auto.pulse_count = cur->pulse_count;
        s_cad_auto.announced = false;
        s_cad_auto.pulses_seq = cmt_request_analysis_pulses ( v->tape_serial, cur->id );
        s_cad_auto.state = s_cad_auto.pulses_seq ? CAD_AUTO_COPYING : CAD_AUTO_SPENT;
    }

    /* posun úlohy: nejvýš jedna analýza najednou, dialog má přednost */
    cad_threads_join ( );
    if ( s_cad_auto.state == CAD_AUTO_WAITING && s_cad.state != CAD_STATE_WAITING && g_atomic_int_get ( &s_cad_workers ) == 0 ) {
        if ( cad_job_start ( s_cad_auto.job ) ) s_cad_auto.state = CAD_AUTO_RUNNING;
        else cad_auto_drop ( CAD_AUTO_SPENT );
    }
    if ( s_cad_auto.state == CAD_AUTO_RUNNING && g_atomic_int_get ( &s_cad_auto.job->done ) ) {
        st_CAD_JOB *job = s_cad_auto.job;
        s_cad_auto.job = NULL;
        st_CMTTMZ_ANALYSIS *a = job->result;
        job->result = NULL;
        uint32_t n = cmttmz_analysis_row_count ( a );
        s_cad_auto.row_max.assign ( job->row_max ? job->row_max : NULL, job->row_max ? job->row_max + n : NULL );
        cad_job_unref ( job );
        if ( !a || cmttmz_analysis_error ( a )[0] ) {
            cmttmz_analysis_free ( a );
            cad_auto_drop ( CAD_AUTO_SPENT );
        } else {
            s_cad_auto.result = a;
            s_cad_auto.addable = 0;
            for ( uint32_t i = 0; i < n; i++ ) {
                if ( cad_row_addable ( cmttmz_analysis_row ( a, i ) ) ) s_cad_auto.addable++;
            }
            s_cad_auto.state = CAD_AUTO_READY;
        }
    }

    /* oznámení: blok dohrál nebo se páska zastavila a v záznamu jsou soubory, které jdou přidat */
    if ( s_cad_auto.state == CAD_AUTO_READY && !s_cad_auto.announced
         && !( playing_raw && cur->id == s_cad_auto.block_id ) ) {
        s_cad_auto.announced = true;
        if ( s_cad_auto.addable > 0 ) {
            char buf[256];
            /* čísla bloků od 1 jako v okně; věta bez množného čísla (katalogy ho nemají) */
            snprintf ( buf, sizeof ( buf ), _ ( "Block %d: files found in the recording: %d. Use Analyze recording... to add them to the tape." ),
                       s_cad_auto.block_id + 1, s_cad_auto.addable );
            cmt_view_post ( buf, CMT_VIEW_MSG_INFO );
        }
    }
}

#ifdef MZTEST_HEADLESS
bool cmt_analyze_dialog_is_open ( void )
{
    cad_init_once ( );
    return s_cad.state != CAD_STATE_NONE;
}
#endif

void cmt_analyze_dialog_abandon ( void )
{
    cad_reset ( );
}


bool cmt_analyze_dialog_request_result ( const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit )
{
    cad_init_once ( );
    if ( 0 == strcmp ( res->name, CMT_REQUEST_ANALYSIS_PULSES_NAME ) ) {
        /* kopii vyzvednout vždy (i pro opuštěný dialog): jinak by ji modul držel do dalšího zásahu */
        st_CMTTMZ_PULSES *p = ( res->status == DEVICE_REQ_DONE && res->rc == 0 ) ? cmt_request_take_pulses ( res->seq ) : NULL;
        if ( s_cad_auto.state == CAD_AUTO_COPYING && res->seq == s_cad_auto.pulses_seq ) {
            /* kopie pro analýzu na pozadí: selhání (záznam vstupů, jiná páska, chyba čtení) se nehlásí */
            s_cad_auto.pulses_seq = 0;
            if ( !p ) {
                cad_auto_drop ( CAD_AUTO_SPENT );
            } else {
                st_CMTTMZ_ANALYZE_OPTS o;
                cmttmz_analyze_opts_default ( &o );
                s_cad_auto.pulses = cad_pulses_new ( p );
                s_cad_auto.job = cad_job_new ( s_cad_auto.pulses, &o );
                s_cad_auto.state = CAD_AUTO_WAITING;
            }
            return true;
        }
        if ( s_cad.state != CAD_STATE_COPYING || res->seq != s_cad.pulses_seq ) {
            cmttmz_pulses_free ( p );
            return true;
        }
        s_cad.pulses_seq = 0;
        if ( res->status != DEVICE_REQ_DONE ) {
            s_cad.state = CAD_STATE_ERROR;
            s_cad.error = _ ( "The recording cannot be analyzed while an input replay is recorded or played." );
        } else if ( res->rc == CMT_REQUEST_RC_TAPE_CHANGED ) {
            s_cad.state = CAD_STATE_ERROR;
            s_cad.error = cad_text_tape_changed ( );
        } else if ( res->rc != 0 || !p ) {
            s_cad.state = CAD_STATE_ERROR;
            /* text jádra (anglicky, nepřekládá se) */
            s_cad.error = ( edit && edit->message[0] ) ? cmt_view_core_text ( edit->message ) : std::string ( _ ( "The recording could not be read." ) );
            cmttmz_pulses_free ( p );
        } else {
            s_cad.pulses = cad_pulses_new ( p );
            s_cad.pulse_count = p->count;
            cad_start_analysis ( );
            cad_poll ( );
        }
        return true;
    }
    if ( 0 != strcmp ( res->name, CMT_REQUEST_ANALYSIS_APPLY_NAME ) ) return false;
    /* data zásahu: emu vlákno je po výsledku nečte */
    for ( size_t i = 0; i < s_cad_applies.size ( ); i++ ) {
        if ( s_cad_applies[i].seq != res->seq ) continue;
        cad_apply_release ( &s_cad_applies[i] );
        s_cad_applies.erase ( s_cad_applies.begin ( ) + ( long ) i );
        break;
    }
    if ( res->status != DEVICE_REQ_DONE ) {
        cmt_view_post ( _ ( "The tape cannot be edited while an input replay is recorded or played." ), CMT_VIEW_MSG_ERROR );
        return true;
    }
    if ( res->rc == CMT_REQUEST_RC_TAPE_CHANGED ) {
        cmt_view_post ( _ ( "The tape changed; the action was cancelled." ), CMT_VIEW_MSG_WARNING );
        return true;
    }
    if ( res->rc != 0 ) {
        cmt_view_post ( ( edit && edit->message[0] ) ? cmt_view_core_text ( edit->message ).c_str ( ) : _ ( "The tape could not be edited." ), CMT_VIEW_MSG_ERROR );
        return true;
    }
    if ( !edit ) {
        /* úspěch bez výsledku úpravy (přepsaný kruh): počet ani blok k výběru neznáme, zpráva přesto */
        cmt_view_post ( cmt_analyze_dialog_added_message ( -1 ).c_str ( ), CMT_VIEW_MSG_INFO );
        return true;
    }
    std::string text = cmt_analyze_dialog_added_message ( ( int ) edit->count );
    if ( edit->message[0] ) {
        /* varování jádra anglicky (nepřekládá se), čísla bloků od 1 jako v okně */
        text += " (";
        text += cmt_view_core_text ( edit->message );
        text += ")";
    }
    cmt_view_post ( text.c_str ( ), edit->message[0] ? CMT_VIEW_MSG_WARNING : CMT_VIEW_MSG_INFO );
    cmt_view_select_after_edit ( edit->tape_serial, edit->select_block );
    return true;
}


bool cmt_analyze_dialog_skip_frame ( void )
{
    cad_init_once ( );
    if ( s_cad.state == CAD_STATE_NONE ) return false;
    /* okno kazety obsah nekreslí: výsledek, výběr řádků, volby i popup zůstanou; analýza dál běží a hotovou
     * se převezme */
    s_cad.drawn_frame = ImGui::GetFrameCount ( );
    s_cad.skipped = true;
    cad_poll ( );
    return true;
}

void cmt_analyze_dialog_frame ( void )
{
    cad_init_once ( );
    const int frame = ImGui::GetCurrentContext ( ) ? ImGui::GetFrameCount ( ) : 0;
    /* okno Virtual CMT dialog v tomto snímku nenakreslilo (zavřené okno; okno bez obsahu ohlásí
     * cmt_analyze_dialog_skip_frame()): jako Cancel */
    if ( s_cad.state != CAD_STATE_NONE && s_cad.drawn_frame != frame ) cad_reset ( );
    /* vlákna opuštěných analýz připojit i se zavřeným dialogem */
    cad_threads_join ( );
    /* ztracený výsledek zásahu uplatnění: fronta prázdná a výsledek nepřišel ani ve vyřízení výsledků
     * minulého snímku (počet čekajících klesá až s výsledkem ve frontě výsledků, device_request.c) */
    const bool idle = ( device_request_pending ( ) == 0 );
    for ( size_t i = 0; i < s_cad_applies.size ( ); ) {
        st_CAD_APPLY *ap = &s_cad_applies[i];
        if ( !idle ) {
            ap->idle_frame = -1;
        } else if ( ap->idle_frame < 0 ) {
            ap->idle_frame = frame;
        } else if ( ap->idle_frame != frame ) {
            cad_apply_release ( ap );
            s_cad_applies.erase ( s_cad_applies.begin ( ) + ( long ) i );
            continue;
        }
        i++;
    }
}

void cmt_analyze_dialog_shutdown ( void )
{
    cad_reset ( );
    cad_auto_drop ( CAD_AUTO_IDLE );
    /* běžící analýzu nejde zrušit: počkat nejvýš CAD_EXIT_JOIN_MS (testy mez mění) a vlákno připojit, jinak
     * odpojit (doběhne nad vlastní kopií pulzů, na UI nesahá; proces ho ukončí) */
    ( void ) cad_threads_finish ( CAD_EXIT_JOIN_MS_NOW );
    /* data zásahů uplatnění: emu vlákno je už připojené a cmt_request_exit() registr vymazal (pořadí
     * main.c) - žádný zásah je nepřečte, uvolní se všechna */
    for ( st_CAD_APPLY &ap : s_cad_applies ) cad_apply_release ( &ap );
    s_cad_applies.clear ( );
}


/* ------------------------------------------------------------------------- */
/*  Kreslení                                                                 */
/* ------------------------------------------------------------------------- */

/** @brief Rozměry dialogu jednoho snímku. */
typedef struct st_CAD_METRICS {
    float F;       /**< výška rámečku */
    float Lh;      /**< výška řádku textu */
    float u;       /**< jednotka mezer (písmo / 7, při 28 px 4 px) */
    float rh;      /**< výška řádku tabulky (Lh + u) */
    float cp;      /**< vnitřní okraj buňky vodorovně (2u) */
    float sp;      /**< mezera mezi prvky (ItemSpacing.x) */
    float W;       /**< šířka obsahu dialogu */
    ImFont *mono;  /**< monospace písmo (čísla, časy) */
} st_CAD_METRICS;

/** @brief Šířka textu (bez skrývání "##"). @param s Text. @return px. */
static float cad_text_w ( const std::string &s )
{
    return ImGui::CalcTextSize ( s.c_str ( ), s.c_str ( ) + s.size ( ), false ).x;
}

/**
 * @brief Šířka tlačítka s popiskem (_L(), část za "##" se nekreslí); společné pravidlo okna kazety.
 * @param label Popisek. @return px (imgui_cmt_button_w()).
 */
static float cad_button_w ( const char *label )
{
    return imgui_cmt_button_w ( label );
}

/** @brief Šířka zaškrtávátka s popiskem. @param label Popisek. @return px. */
static float cad_checkbox_w ( const char *label )
{
    return ImGui::GetFrameHeight ( ) + ImGui::GetStyle ( ).ItemInnerSpacing.x + ImGui::CalcTextSize ( label, NULL, true ).x;
}

/**
 * @brief Zalomí text po slovech na šířku.
 * @param text Text. @param max_w Šířka (px). @return Řádky (aspoň jeden).
 */
static std::vector<std::string> cad_wrap ( const std::string &text, float max_w )
{
    std::vector<std::string> lines;
    std::string line, word;
    size_t i = 0;
    while ( i <= text.size ( ) ) {
        if ( i == text.size ( ) || text[i] == ' ' ) {
            if ( !word.empty ( ) ) {
                std::string cand = line.empty ( ) ? word : line + " " + word;
                if ( !line.empty ( ) && cad_text_w ( cand ) > max_w ) {
                    lines.push_back ( line );
                    line = word;
                } else {
                    line = cand;
                }
                word.clear ( );
            }
        } else {
            word += text[i];
        }
        i++;
    }
    if ( !line.empty ( ) || lines.empty ( ) ) lines.push_back ( line );
    return lines;
}

/**
 * @brief Zalomený text na šířku obsahu (každý řádek zaznamenaný zvlášť jako "<name>" / "<name>_<k>").
 *
 * Testy čtou celý text z prvního záznamu jména @p name (text všech řádků).
 *
 * @param name Jméno prvku. @param text Text. @param color Barva. @param m Rozměry.
 */
static void cad_wrapped ( const char *name, const std::string &text, ImU32 color, const st_CAD_METRICS *m )
{
    std::vector<std::string> lines = cad_wrap ( text, m->W );
    ImVec2 p0 = ImGui::GetCursorScreenPos ( );
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    float wmax = 0.0f;
    for ( const std::string &l : lines ) {
        cad_mark ( );
        ImGui::TextUnformatted ( l.c_str ( ), l.c_str ( ) + l.size ( ) );
        wmax = ImMax ( wmax, cad_text_w ( l ) );
    }
    ImGui::PopStyleColor ( );
    ImVec2 p1 = ImGui::GetItemRectMax ( );
    cad_record ( name, text, p0, ImVec2 ( p0.x + wmax, p1.y ), false, false, false, NULL );
}

/**
 * @brief Tlačítko se zákazem a tooltipem důvodu.
 * @param label Popisek (_L()). @param name Jméno prvku (testy). @param reason Důvod zákazu (NULL = povolené).
 * @param primary Výchozí tlačítko (plocha segment-on). @param[out] id ID tlačítka (smí být NULL).
 * @return true = stisknuté a povolené.
 * @note Tvar a barvy společné s ostatními dialogy okna kazety (imgui_cmt_button());
 *       testy dostanou u výchozího povoleného tlačítka hodnotu "ad_primary" = @p name.
 */
static bool cad_button ( const char *label, const char *name, const char *reason, bool primary, ImGuiID *id )
{
    const bool pressed = imgui_cmt_button ( label, 0.0f, reason, primary );
    if ( id ) *id = ImGui::GetItemID ( );
    std::string shown ( label, ImGui::FindRenderedTextEnd ( label ) );
    cad_record_item ( name, shown, reason != NULL, reason );
    if ( primary && !reason ) cad_record_value ( "ad_primary", name );
    return pressed;
}

/**
 * @brief Šířky obsahu sloupců (mezipaměť podle výsledku, písma a prodloužení textů).
 * @param m Rozměry.
 */
static void cad_col_widths ( const st_CAD_METRICS *m )
{
#ifdef MZTEST_HEADLESS
    bool stretch = cmt_view_test_stretch ( );
#else
    bool stretch = false;
#endif
    if ( s_cad.col_key_serial == s_cad.result_serial && s_cad.col_key_font == ImGui::GetFontSize ( ) && s_cad.col_key_stretch == stretch )
        return;
    float *w = s_cad.col_w;
    for ( int c = 0; c < CAD_COL_COUNT; c++ ) w[c] = ImGui::CalcTextSize ( cad_col_label ( c ), NULL, true ).x;
    w[CAD_COL_USE] = ImGui::GetFontSize ( );
    ImGui::PushFont ( m->mono, 0.0f );
    char buf[32];
    snprintf ( buf, sizeof ( buf ), "%d", ImMax ( ( int ) s_cad.rows.size ( ), 9 ) );
    w[CAD_COL_NUM] = ImMax ( w[CAD_COL_NUM], ImGui::CalcTextSize ( buf ).x );
    for ( const st_CAD_ROW_UI &u : s_cad.row_ui ) {
        w[CAD_COL_START] = ImMax ( w[CAD_COL_START], cad_text_w ( u.start ) );
        w[CAD_COL_SIZE] = ImMax ( w[CAD_COL_SIZE], cad_text_w ( u.size ) );
    }
    ImGui::PopFont ( );
    /* šířky z textů, které řádky skutečně mají (výsledek: oba texty, mezi kterými zaškrtnutí přepíná);
     * tabulka bez řádků má jen hlavičku, šířku dialogu pak dají volby a tlačítka */
    for ( size_t i = 0; i < s_cad.row_ui.size ( ); i++ ) {
        const st_CAD_ROW_UI &u = s_cad.row_ui[i];
        w[CAD_COL_NAME] = ImMax ( w[CAD_COL_NAME], cad_text_w ( u.name ) );
        w[CAD_COL_TYPE] = ImMax ( w[CAD_COL_TYPE], cad_text_w ( u.type ) );
        w[CAD_COL_FORMAT] = ImMax ( w[CAD_COL_FORMAT], cad_text_w ( u.format ) );
        w[CAD_COL_CHECKSUM] = ImMax ( w[CAD_COL_CHECKSUM], cad_text_w ( u.checksum ) );
        ImU32 col;
        const st_CMTTMZ_ANALYZE_ROW *r = &s_cad.rows[i];
        w[CAD_COL_RESULT] = ImMax ( w[CAD_COL_RESULT], cad_text_w ( cad_result_text ( r, true, &col ) ) );
        w[CAD_COL_RESULT] = ImMax ( w[CAD_COL_RESULT], cad_text_w ( cad_result_text ( r, false, &col ) ) );
    }
    for ( int c = 0; c < CAD_COL_COUNT; c++ ) w[c] = ceilf ( w[c] );
    s_cad.col_key_serial = s_cad.result_serial;
    s_cad.col_key_font = ImGui::GetFontSize ( );
    s_cad.col_key_stretch = stretch;
}

/**
 * @brief Přirozená šířka tabulky (sloupce, vnitřní okraje, čáry; volitelně svislý posuvník).
 * @param m Rozměry. @param scrollbar Počítat svislý posuvník. @return px.
 */
static float cad_table_natural_w ( const st_CAD_METRICS *m, bool scrollbar )
{
    float w = 0.0f;
    for ( int c = 0; c < CAD_COL_COUNT; c++ ) w += s_cad.col_w[c] + 2.0f * m->cp;
    w += ( float ) ( CAD_COL_COUNT + 1 ); /* čáry sloupců a okraje */
    if ( scrollbar ) w += ImGui::GetStyle ( ).ScrollbarSize;
    return ceilf ( w );
}

/**
 * @brief Text buňky zarovnaný vpravo (čísla, časy), zaznamenaný pro testy.
 * @param name Jméno prvku (testy). @param text Text. @param color Barva textu.
 */
static void cad_cell_right ( const char *name, const std::string &text, ImU32 color )
{
    float tw = cad_text_w ( text );
    float avail = ImGui::GetContentRegionAvail ( ).x;
    if ( avail > tw ) ImGui::SetCursorPosX ( ImGui::GetCursorPosX ( ) + avail - tw );
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    ImGui::TextUnformatted ( text.c_str ( ), text.c_str ( ) + text.size ( ) );
    ImGui::PopStyleColor ( );
    cad_record ( name, text, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false, true, false, NULL );
}

/**
 * @brief Zkrátí text zprava elipsou "..." na šířku (po celých znacích UTF-8).
 * @param s Text. @param max_w Šířka (px). @param[out] elided true = text se zkrátil.
 * @return Text, který se vejde (aspoň "...").
 */
static std::string cad_ellipsize_end ( const std::string &s, float max_w, bool *elided )
{
    *elided = false;
    if ( cad_text_w ( s ) <= max_w ) return s;
    *elided = true;
    std::string cut = s;
    while ( !cut.empty ( ) ) {
        /* ubrat jeden znak UTF-8 (pokračovací bajty 10xxxxxx) */
        size_t n = cut.size ( ) - 1;
        while ( n > 0 && ( ( unsigned char ) cut[n] & 0xC0u ) == 0x80u ) n--;
        cut.erase ( n );
        if ( cad_text_w ( cut + "..." ) <= max_w ) break;
    }
    return cut + "...";
}

/**
 * @brief Text buňky zarovnaný vlevo, zkrácený elipsou na šířku sloupce, zaznamenaný (testy).
 * @param name Jméno prvku. @param text Text. @param color Barva.
 * @param tip Tooltip (NULL = žádný; zkrácený text bez tooltipu ukáže v tooltipu celý text).
 * @return true = myš nad textem.
 */
static bool cad_cell_left ( const char *name, const std::string &text, ImU32 color, const char *tip )
{
    bool elided = false;
    std::string shown = cad_ellipsize_end ( text, ImGui::GetContentRegionAvail ( ).x, &elided );
    ImGui::PushStyleColor ( ImGuiCol_Text, color );
    ImGui::TextUnformatted ( shown.c_str ( ), shown.c_str ( ) + shown.size ( ) );
    ImGui::PopStyleColor ( );
    std::string t = ( elided && !( tip && tip[0] ) ) ? text : std::string ( tip ? tip : "" );
    cad_record ( name, elided ? text : shown, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false, true, false,
                 t.empty ( ) ? NULL : t.c_str ( ) );
    bool hov = ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip );
    if ( hov && !t.empty ( ) ) ImGui::SetTooltip ( "%s", t.c_str ( ) );
    return hov;
}

/**
 * @brief Enter na zaškrtávátku s fokusem navigace ho přepne a nic jiného neudělá.
 *
 * ImGui zaškrtávátko Enterem přepne samo jen při viditelném kurzoru
 * navigace (NavActivateId == ID položky v tomto snímku); po kliknutí myší
 * kurzor vidět není a přepne ho tato funkce. Enter na zaškrtávátku tak
 * nikdy nespustí Add (kreslení dialogu ho bere jako Add jen při fokusu na
 * Add, na řádku nebo bez fokusu).
 *
 * @param v Hodnota zaškrtávátka. @param enter Enter stisknutý v tomto snímku (mimo otevření a aktivní prvek).
 * @return true = přepnuto touto funkcí.
 * @pre Hned po ImGui::Checkbox().
 */
static bool cad_enter_toggles ( bool *v, bool enter )
{
    if ( !enter || !ImGui::IsItemFocused ( ) || ImGui::GetCurrentContext ( )->NavActivateId == ImGui::GetItemID ( ) ) return false;
    *v = !*v;
    return true;
}

/**
 * @brief Barva textu buňky ve vybraném řádku: ztlumený a chybový text světlejší, aby měl na row-selected aspoň 4,5:1.
 * @param c Barva na nevybraném řádku. @param sel Řádek je vybraný.
 * @return text-dim-selected místo text-dim, error-selected místo error; jinak @p c.
 */
static ImU32 cad_sel_color ( ImU32 c, bool sel )
{
    if ( !sel ) return c;
    if ( c == CMT_TOK_TEXT_DIM ) return CMT_TOK_TEXT_DIM_SELECTED;
    if ( c == CMT_TOK_ERROR ) return CMT_TOK_ERROR_SELECTED;
    return c;
}

/**
 * @brief Jeden řádek výsledků (jen viditelné řádky z ImGuiListClipper).
 *
 * Klik vybere řádek (nápověda, histogram), šipky přesouvají fokus
 * navigace a s ním výběr, mezerník na řádku s fokusem přepne zaškrtnutí
 * (soubor, který jde přidat), Enter na zaškrtávátku ho přepne
 * (cad_enter_toggles()). Zaškrtávátko je jen u souborů; u souboru, který
 * přidat nejde, zakázané.
 *
 * @param m Rozměry. @param i Index řádku. @param last Poslední řádek (o 1 px vyšší, jako tabulka okna).
 * @param speed_text Výchozí rychlost decku (tooltip výsledku). @param editing Některý prvek byl aktivní na začátku snímku.
 * @param enter Enter stisknutý v tomto snímku (mimo otevření a aktivní prvek).
 * @param[out] row_focused Řádek má fokus navigace.
 */
static void cad_table_row ( const st_CAD_METRICS *m, int i, bool last, const std::string &speed_text, bool editing, bool enter,
                            bool *row_focused )
{
    char nb[40];
    const st_CMTTMZ_ANALYZE_ROW *r = &s_cad.rows[( size_t ) i];
    const st_CAD_ROW_UI *u = &s_cad.row_ui[( size_t ) i];
    const bool sel = ( i == s_cad.sel_row );
    ImGui::TableNextRow ( ImGuiTableRowFlags_None, m->rh + ( last ? 1.0f : 0.0f ) );
    /* hrana posouvaného těla jen mezi řádky (tabulka bez vlastního posuvníku) */
    if ( !s_cad.table_own_scroll ) cad_mark ( ImGui::GetCurrentTable ( )->RowPosY1 );
    ImGui::PushID ( i );
    ImGui::TableSetColumnIndex ( CAD_COL_USE );
    ImVec2 cell = ImGui::GetCursorScreenPos ( );
    if ( ImGui::Selectable ( "##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2 ( 0.0f, m->Lh ) ) )
        s_cad.sel_row = i;
    cad_record ( CAD_NAME ( nb, "ad_row", i ), "", ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), true, true, false, NULL );
    ImGuiID row_id = ImGui::GetItemID ( );
    if ( ImGui::IsItemFocused ( ) ) {
        *row_focused = true;
        /* šipky: výběr následuje fokus navigace (jen nově zaostřený řádek) */
        if ( row_id != s_cad.nav_last ) {
            s_cad.nav_last = row_id;
            s_cad.sel_row = i;
        }
        /* mezerník přepne zaškrtnutí řádku s fokusem (SPEC) */
        if ( !editing && u->addable && ImGui::IsKeyPressed ( ImGuiKey_Space, false ) ) s_cad.use[( size_t ) i] ^= 1u;
    }
    if ( u->file ) {
        ImGui::SetCursorScreenPos ( cell );
        ImGui::PushStyleVar ( ImGuiStyleVar_FramePadding, ImVec2 ( 0.0f, 0.0f ) );
        ImGui::BeginDisabled ( !u->addable );
        bool c = s_cad.use[( size_t ) i] != 0;
        if ( ImGui::Checkbox ( "##use", &c ) && u->addable ) s_cad.use[( size_t ) i] = c ? 1u : 0u;
        if ( u->addable && cad_enter_toggles ( &c, enter ) ) s_cad.use[( size_t ) i] = c ? 1u : 0u;
        ImGui::EndDisabled ( );
        ImGui::PopStyleVar ( );
        std::string why = u->addable ? std::string ( ) : cad_row_detail ( r, speed_text );
        if ( !u->addable && ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip ( "%s", why.c_str ( ) );
        cad_record ( CAD_NAME ( nb, "ad_check", i ), c ? "1" : "0", ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false, true,
                     !u->addable, why.empty ( ) ? NULL : why.c_str ( ) );
    }
    char buf[24];
    if ( ImGui::TableSetColumnIndex ( CAD_COL_NUM ) ) {
        snprintf ( buf, sizeof ( buf ), "%d", i + 1 );
        ImGui::PushFont ( m->mono, 0.0f );
        cad_cell_right ( CAD_NAME ( nb, "ad_num", i ), buf, cad_sel_color ( CMT_TOK_TEXT_DIM, sel ) );
        ImGui::PopFont ( );
    }
    if ( ImGui::TableSetColumnIndex ( CAD_COL_NAME ) ) cad_cell_left ( CAD_NAME ( nb, "ad_name", i ), u->name, CMT_TOK_TEXT, NULL );
    if ( ImGui::TableSetColumnIndex ( CAD_COL_TYPE ) ) cad_cell_left ( CAD_NAME ( nb, "ad_type", i ), u->type, CMT_TOK_TEXT, NULL );
    if ( ImGui::TableSetColumnIndex ( CAD_COL_FORMAT ) ) cad_cell_left ( CAD_NAME ( nb, "ad_format", i ), u->format, CMT_TOK_TEXT, NULL );
    ImGui::PushFont ( m->mono, 0.0f );
    if ( ImGui::TableSetColumnIndex ( CAD_COL_START ) ) cad_cell_right ( CAD_NAME ( nb, "ad_start", i ), u->start, CMT_TOK_TEXT );
    if ( ImGui::TableSetColumnIndex ( CAD_COL_SIZE ) ) cad_cell_right ( CAD_NAME ( nb, "ad_size", i ), u->size, CMT_TOK_TEXT );
    ImGui::PopFont ( );
    const ImU32 ck_col = cad_sel_color ( u->checksum_color, sel );
    if ( ImGui::TableSetColumnIndex ( CAD_COL_CHECKSUM ) ) cad_cell_left ( CAD_NAME ( nb, "ad_checksum", i ), u->checksum, ck_col, NULL );
    if ( ImGui::TableSetColumnIndex ( CAD_COL_RESULT ) ) {
        ImU32 col;
        const char *t = cad_result_text ( r, s_cad.use[( size_t ) i] != 0, &col );
        col = cad_sel_color ( col, sel );
        std::string tip = cad_row_detail ( r, speed_text );
        cad_cell_left ( CAD_NAME ( nb, "ad_result", i ), t, col, tip.c_str ( ) );
        if ( sel ) {
            /* barvy vybraného řádku pro testy */
            char hex[16];
            snprintf ( hex, sizeof ( hex ), "%08X", ( unsigned ) ck_col );
            cad_record_value ( "ad_sel_checksum_color", hex );
            snprintf ( hex, sizeof ( hex ), "%08X", ( unsigned ) col );
            cad_record_value ( "ad_sel_result_color", hex );
        }
    }
    ImGui::PopID ( );
}

/**
 * @brief Tabulka výsledků; bez řádků (běží, nic nenalezeno, chyba) hlavička a pod ní věty.
 *
 * @param m Rozměry. @param size Velikost tabulky (šířka W, výška podle řádků nebo vět).
 * @param body_lines Věty pod hlavičkou (bez řádků), prázdné = řádky výsledků.
 * @param body_color Barva vět. @param progress Pod větami neurčitý průběh (analýza běží).
 * @param dim Výsledky zastaralé (tabulka ztlumená). @param speed_text Výchozí rychlost decku.
 * @param editing Prvek aktivní na začátku snímku. @param enter Enter stisknutý v tomto snímku.
 * @param own_scroll Tabulka se posouvá sama (ImGuiTableFlags_ScrollY, výška = viditelné řádky); false = výška
 *        pro všechny řádky bez vlastního posuvníku, posouvá se tělo dialogu.
 * @param[out] row_focused Řádek má fokus navigace.
 */
static void cad_draw_table ( const st_CAD_METRICS *m, ImVec2 size, const std::vector<std::string> &body_lines, ImU32 body_color,
                             bool progress, bool dim, const std::string &speed_text, bool editing, bool enter, bool own_scroll,
                             bool *row_focused )
{
    const bool rows_mode = body_lines.empty ( );
    const int n = rows_mode ? ( int ) s_cad.rows.size ( ) : 0;
    ImVec2 pos = ImGui::GetCursorScreenPos ( );
    cad_mark ( );
    s_cad.table_own_scroll = rows_mode && own_scroll;
    /* šířky sloupců: přirozené; nevejdou-li se do šířky dialogu (175 %, delší překlady), zúží se nejdřív Type
     * a Format, pak Name, ale ne pod šířku textu "Unknown signal" (říká, že úsek je nerozpoznaný),
     * pak Checksum a Result a teprve nakonec Name i pod ni - každý nejvýš na 4 široké znaky (texty se zkrátí
     * elipsou s tooltipem), teprve pak vodorovný posuv */
    const bool vbar = s_cad.table_own_scroll && n > 0 && size.y < ( float ) ( 1 + n ) * m->rh + 1.0f - 0.5f;
    const float inner = size.x - ( vbar ? ImGui::GetStyle ( ).ScrollbarSize : 0.0f );
    float w[CAD_COL_COUNT];
    for ( int c = 0; c < CAD_COL_COUNT; c++ ) w[c] = s_cad.col_w[c];
    float natural = cad_table_natural_w ( m, false );
    bool shrink = false;
    float unknown_w = 0.0f;
    for ( const st_CAD_ROW_UI &u : s_cad.row_ui )
        if ( !u.file ) unknown_w = ceilf ( cad_text_w ( u.name ) );
    /* fáze: sloupce a zda Name drží šířku textu "Unknown signal" */
    static const int phase_cols[4][2] = { { CAD_COL_TYPE, CAD_COL_FORMAT }, { CAD_COL_NAME, -1 }, { CAD_COL_CHECKSUM, CAD_COL_RESULT },
                                          { CAD_COL_NAME, -1 } };
    static const bool phase_protect[4] = { false, true, false, false };
    const float min_w = ImGui::CalcTextSize ( "MMMM" ).x;
    for ( int ph = 0; ph < 4 && natural > inner + 0.5f; ph++ ) {
        float lo_c[2] = { 0.0f, 0.0f };
        float room = 0.0f;
        for ( int k = 0; k < 2; k++ ) {
            const int c = phase_cols[ph][k];
            if ( c < 0 ) continue;
            lo_c[k] = ImMax ( min_w, ImGui::CalcTextSize ( cad_col_label ( c ), NULL, true ).x );
            if ( c == CAD_COL_NAME && phase_protect[ph] ) lo_c[k] = ImMax ( lo_c[k], unknown_w );
            room += ImMax ( 0.0f, w[c] - lo_c[k] );
        }
        const float take = ImMin ( natural - inner + 1.0f, room );
        if ( room > 0.0f ) {
            for ( int k = 0; k < 2; k++ ) {
                const int c = phase_cols[ph][k];
                if ( c < 0 ) continue;
                const float lo = lo_c[k];
                if ( w[c] > lo ) w[c] = ImMax ( lo, floorf ( w[c] - ( w[c] - lo ) * take / room ) );
            }
            shrink = true;
        }
        natural = ( float ) ( CAD_COL_COUNT + 1 );
        for ( int c = 0; c < CAD_COL_COUNT; c++ ) natural += w[c] + 2.0f * m->cp;
    }
    const bool scroll_x = natural > inner + 0.5f;
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter
        | ( s_cad.table_own_scroll ? ImGuiTableFlags_ScrollY : 0 ) | ImGuiTableFlags_NoSavedSettings
        | ( scroll_x ? ImGuiTableFlags_ScrollX : 0 );
    /* vodorovný posuvník zabere spodek tabulky: tabulka o něj vyšší, aby řádky nebyly rozříznuté */
    if ( rows_mode && scroll_x ) size.y += ImGui::GetStyle ( ).ScrollbarSize;
    s_cad.table_extra_h = ( rows_mode && scroll_x ) ? ImGui::GetStyle ( ).ScrollbarSize : 0.0f;
#ifdef MZTEST_HEADLESS
    cad_record_value ( "ad_table_scroll_x", scroll_x ? "1" : "0" );
#endif
    ImGui::PushStyleColor ( ImGuiCol_TableHeaderBg, CMT_TOK_TABLE_HEADER );
    ImGui::PushStyleColor ( ImGuiCol_TableRowBg, CMT_TOK_PANEL );
    ImGui::PushStyleColor ( ImGuiCol_TableRowBgAlt, CMT_TOK_ROW_ALT );
    ImGui::PushStyleColor ( ImGuiCol_TableBorderLight, CMT_TOK_BORDER );
    ImGui::PushStyleColor ( ImGuiCol_TableBorderStrong, CMT_TOK_BORDER );
    ImGui::PushStyleColor ( ImGuiCol_Header, CMT_TOK_ROW_SELECTED );
    ImGui::PushStyleColor ( ImGuiCol_HeaderHovered, CMT_TOK_CONTROL );
    ImGui::PushStyleColor ( ImGuiCol_HeaderActive, CMT_TOK_CONTROL_ACTIVE );
    ImGui::PushStyleColor ( ImGuiCol_ChildBg, CMT_TOK_PANEL );
    ImGui::PushStyleVar ( ImGuiStyleVar_CellPadding, ImVec2 ( m->cp, 0.5f * m->u ) );
    /* zastaralé výsledky: tabulka ztlumená (SPEC), řádky dál jdou vybrat */
    if ( dim ) ImGui::PushStyleVar ( ImGuiStyleVar_Alpha, ImGui::GetStyle ( ).Alpha * 0.45f );
    /* stálé ID tabulky (posun a fokus řádku zůstanou); šířky pevných sloupců se nastaví
     * každý snímek (ImGui bere šířku z TableSetupColumn() jen při vzniku tabulky) */
    const bool open = ImGui::BeginTable ( "##cad_table", CAD_COL_COUNT, flags, ImVec2 ( size.x, rows_mode ? size.y : m->rh + 1.0f ),
                                          scroll_x ? natural : 0.0f );
    if ( open ) {
        /* Name roztažený do zbytku šířky, jen když se tabulka vejde bez zúžení */
        const bool stretch_name = !scroll_x && !shrink;
        for ( int c = 0; c < CAD_COL_COUNT; c++ ) {
            ImGuiTableColumnFlags cf = ( c == CAD_COL_NAME && stretch_name ) ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed;
            /* bez NoResize: šířku pevného sloupce pak ImGui nepřepisuje šířkou obsahu (tabulka nemá
             * ImGuiTableFlags_Resizable, uživatel sloupce měnit nemůže) */
            ImGui::TableSetupColumn ( cad_col_label ( c ), cf | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHide,
                                      ( c == CAD_COL_NAME && stretch_name ) ? 1.0f : w[c] );
        }
        {
            ImGuiTable *t = ImGui::GetCurrentTable ( );
            for ( int c = 0; c < CAD_COL_COUNT; c++ ) {
                if ( c == CAD_COL_NAME && stretch_name ) continue;
                t->Columns[c].WidthRequest = w[c];
            }
            /* nový výsledek: tabulka od prvního řádku (jen tabulka s vlastním posunem: jinak je aktuální okno
             * tělo dialogu nebo vnitřní okno vodorovného posunu, tělo nahoru vrací body_scroll_reset) */
            if ( s_cad.table_scroll_reset ) {
                if ( s_cad.table_own_scroll ) ImGui::SetScrollY ( 0.0f );
                s_cad.table_scroll_reset = false;
            }
#ifdef MZTEST_HEADLESS
            s_cad_table_id = t->ID;
            s_cad_table_scroll = ( t->InnerWindow != t->OuterWindow ) && t->InnerWindow->ScrollbarY;
#endif
        }
        ImGui::TableSetupScrollFreeze ( 0, 1 );
        ImGui::TableNextRow ( ImGuiTableRowFlags_Headers, m->rh );
        for ( int c = 0; c < CAD_COL_COUNT; c++ ) {
            if ( !ImGui::TableSetColumnIndex ( c ) ) continue;
            const char *lbl = cad_col_label ( c );
            ImGui::TableHeader ( lbl );
            char nb[24];
            std::string shown ( lbl, ImGui::FindRenderedTextEnd ( lbl ) );
            ImVec2 imin = ImGui::GetItemRectMin ( ), imax = ImGui::GetItemRectMax ( );
            ImVec2 tsz = ImGui::CalcTextSize ( lbl, NULL, true );
            ImVec2 tmin ( imin.x, floorf ( ( imin.y + imax.y - tsz.y ) * 0.5f ) );
            cad_record ( CAD_NAME ( nb, "ad_hdr", c ), shown, tmin, ImVec2 ( tmin.x + tsz.x, tmin.y + tsz.y ), false, false, false, NULL );
        }
        if ( rows_mode ) {
            ImGuiListClipper clipper;
            clipper.Begin ( n, m->rh );
            if ( s_cad.sel_row >= 0 && s_cad.sel_row < n ) clipper.IncludeItemByIndex ( s_cad.sel_row );
            while ( clipper.Step ( ) ) {
                for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++ )
                    cad_table_row ( m, i, i == n - 1, speed_text, editing, enter, row_focused );
            }
            clipper.End ( );
        }
        ImGui::EndTable ( );
    }
    if ( dim ) ImGui::PopStyleVar ( );
    ImGui::PopStyleVar ( );
    ImGui::PopStyleColor ( 9 );
    cad_record ( "ad_table", "", pos, ImVec2 ( pos.x + size.x, pos.y + size.y ), true, false, false, NULL );
    if ( rows_mode ) return;
    /* tělo bez řádků: rámeček bez čar sloupců (koncept empty-v2), věty uprostřed */
    float body_top = pos.y + m->rh + 1.0f;
    {
        ImDrawList *dl = ImGui::GetWindowDrawList ( );
        ImVec2 b0 ( pos.x, pos.y + m->rh ), b1 ( pos.x + size.x, pos.y + size.y );
        dl->AddRectFilled ( b0, b1, CMT_TOK_PANEL );
        dl->AddRect ( b0, b1, CMT_TOK_BORDER );
        /* místo těla v rozvržení (tabulka sama má jen hlavičku) */
        ImGui::SetCursorScreenPos ( ImVec2 ( pos.x, body_top ) );
        ImGui::Dummy ( ImVec2 ( size.x, ImMax ( 0.0f, b1.y - body_top ) ) );
    }
    float block_h = ( float ) body_lines.size ( ) * m->Lh + ( progress ? m->u + 0.5f * m->Lh : 0.0f );
    float y = floorf ( body_top + ( pos.y + size.y - body_top - block_h ) * 0.5f );
    ImVec2 after = ImGui::GetCursorScreenPos ( );
    for ( size_t k = 0; k < body_lines.size ( ); k++ ) {
        const std::string &l = body_lines[k];
        float tw = cad_text_w ( l );
        ImGui::SetCursorScreenPos ( ImVec2 ( floorf ( pos.x + ( size.x - tw ) * 0.5f ), y ) );
        ImGui::PushStyleColor ( ImGuiCol_Text, body_color );
        ImGui::TextUnformatted ( l.c_str ( ), l.c_str ( ) + l.size ( ) );
        ImGui::PopStyleColor ( );
        char nb[24];
        cad_record ( CAD_NAME ( nb, "ad_body", k ), l, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), false, false, false, NULL );
        y += m->Lh;
    }
    if ( progress ) {
        /* neurčitý průběh (analyzátor průběh neumí) */
        float pw = floorf ( size.x * 0.4f );
        ImGui::SetCursorScreenPos ( ImVec2 ( floorf ( pos.x + ( size.x - pw ) * 0.5f ), y + m->u ) );
        ImGui::PushStyleColor ( ImGuiCol_PlotHistogram, CMT_TOK_ACCENT );
        ImGui::ProgressBar ( -1.0f * ( float ) ImGui::GetTime ( ), ImVec2 ( pw, 0.5f * m->Lh ), "" );
        ImGui::PopStyleColor ( );
        cad_record_item ( "ad_progress", "", false, NULL );
    }
    ImGui::SetCursorScreenPos ( after );
}

/**
 * @brief Spočítá histogram rozsahu pulzů (mezipaměť podle řádku, výsledku a kopie).
 *
 * Osa od 0 do min(nejdelší pulz zaokrouhlený nahoru na 100 µs,
 * CAD_HIST_AXIS_MAX_US), aspoň CAD_HIST_AXIS_MIN_US; pulzy delší než osa
 * padnou do posledního binu, který se nekreslí (hist_overflow). Nejdelší
 * pulz dá pracovní vlákno (celá kopie jednou za kopii, řádek s výsledkem
 * analýzy); UI vlákno volá cmttmz_analysis_histogram() bez průchodu pro
 * maximum (max_us NULL), takže projde jen vzorek nejvýš
 * CMTTMZ_ANALYZE_HIST_MAX_PULSES pulzů (obří blok). Bez známého maxima
 * (kopie bez hotové analýzy) osa
 * CAD_HIST_AXIS_MAX_US.
 *
 * @param first První pulz. @param end Pulz za rozsahem. @param row Řádek (-1 = celý záznam; klíč mezipaměti).
 */
static void cad_hist_compute ( uint32_t first, uint32_t end, int row )
{
    if ( s_cad.hist_valid && s_cad.hist_key_row == row && s_cad.hist_key_serial == s_cad.result_serial && s_cad.hist_key_pulses == s_cad.pulses )
        return;
    s_cad.hist_valid = true;
    s_cad.hist_key_row = row;
    s_cad.hist_key_serial = s_cad.result_serial;
    s_cad.hist_key_pulses = s_cad.pulses;
    s_cad.hist_bins.assign ( CAD_HIST_BINS + 1u, 0u );
    s_cad.hist_overflow = false;
    s_cad.hist_axis_us = CAD_HIST_AXIS_MAX_US;
    const st_CMTTMZ_PULSES *p = s_cad.pulses ? s_cad.pulses->p : NULL;
    if ( !p || first >= end || end > p->count ) {
        s_cad.hist_bins.clear ( );
        return;
    }
    double max_us = CAD_HIST_AXIS_MAX_US;
    if ( row >= 0 && row < ( int ) s_cad.row_max.size ( ) ) max_us = s_cad.row_max[( size_t ) row];
    else if ( row < 0 && g_atomic_int_get ( &s_cad.pulses->max_known ) ) max_us = s_cad.pulses->max_us;
    double axis = ceil ( max_us / 100.0 ) * 100.0;
    axis = ImClamp ( axis, CAD_HIST_AXIS_MIN_US, CAD_HIST_AXIS_MAX_US );
    s_cad.hist_axis_us = axis;
    if ( EXIT_SUCCESS != cmttmz_analysis_histogram ( p, first, end, axis / ( double ) CAD_HIST_BINS, s_cad.hist_bins.data ( ),
                                                     CAD_HIST_BINS + 1u, NULL ) ) {
        s_cad.hist_bins.clear ( );
        return;
    }
    s_cad.hist_overflow = s_cad.hist_bins[CAD_HIST_BINS] > 0u;
}

/**
 * @brief Obsah oddílu Signal details: popisek, sloupce histogramu, osa v µs, značky krátkého a dlouhého pulzu vybraného souboru.
 *
 * Bez vybraného řádku celý záznam; s vybraným řádkem jeho rozsah pulzů
 * a u souboru značky short_pulse_us / long_pulse_us (accent, hodnota
 * v µs). Sloupce neutrální šedé, bez čáry prahu (SPEC).
 *
 * @param m Rozměry.
 */
static void cad_draw_histogram ( const st_CAD_METRICS *m )
{
    const st_CMTTMZ_PULSES *p = s_cad.pulses->p;
    int row = ( s_cad.sel_row >= 0 && s_cad.sel_row < ( int ) s_cad.rows.size ( ) ) ? s_cad.sel_row : -1;
    uint32_t first = 0, end = p->count;
    std::string caption;
    if ( row >= 0 ) {
        first = s_cad.rows[( size_t ) row].first_pulse;
        end = s_cad.rows[( size_t ) row].end_pulse;
        char buf[160];
        snprintf ( buf, sizeof ( buf ), _ ( "Pulse lengths, %s" ), s_cad.row_ui[( size_t ) row].name.c_str ( ) );
        caption = buf;
    } else {
        caption = _ ( "Pulse lengths, whole recording" );
    }
    cad_hist_compute ( first, end, row );
    /* popisek, plocha, osa a popisky osy jsou jeden celek: hrana posouvaného těla jen nad ním */
    cad_mark ( );
    ImVec2 p0 = ImGui::GetCursorScreenPos ( );
    ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT_DIM );
    ImGui::TextUnformatted ( caption.c_str ( ), caption.c_str ( ) + caption.size ( ) );
    ImGui::PopStyleColor ( );
    cad_record_item ( "ad_hist_caption", caption, false, NULL );
    const float plot_h = s_cad.plot_h;
    ImVec2 pmin = ImGui::GetCursorScreenPos ( );
    ImVec2 pmax ( pmin.x + m->W, pmin.y + plot_h );
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    /* osa: vpravo místo pro jednotku */
    ImGui::PushFont ( m->mono, 0.0f );
    const char *unit = "µs";
    float unit_w = cad_text_w ( unit );
    float ax0 = pmin.x, ax1 = pmax.x - unit_w - 2.0f * m->u;
    float axis_us = ( float ) s_cad.hist_axis_us;
    if ( !s_cad.hist_bins.empty ( ) ) {
        uint32_t peak = 1;
        for ( uint32_t k = 0; k < CAD_HIST_BINS; k++ ) peak = ImMax ( peak, s_cad.hist_bins[k] );
        float bw = ( ax1 - ax0 ) / ( float ) CAD_HIST_BINS;
        float gap = ImMax ( 1.0f, floorf ( bw * 0.25f ) );
        for ( uint32_t k = 0; k < CAD_HIST_BINS; k++ ) {
            if ( !s_cad.hist_bins[k] ) continue;
            float h = ( float ) s_cad.hist_bins[k] / ( float ) peak * ( plot_h - m->Lh - m->u );
            h = ImMax ( h, 1.0f );
            float x = floorf ( ax0 + ( float ) k * bw );
            dl->AddRectFilled ( ImVec2 ( x, pmax.y - h ), ImVec2 ( ImMax ( x + 1.0f, floorf ( x + bw - gap ) ), pmax.y ), CMT_TOK_HIST_BAR );
        }
    }
    dl->AddLine ( ImVec2 ( ax0, pmax.y + 0.5f ), ImVec2 ( ax1, pmax.y + 0.5f ), CMT_TOK_CONTROL_BORDER, 1.0f );
    cad_record ( "ad_hist", "", pmin, pmax, true, false, false, NULL );
    /* značky krátkého a dlouhého pulzu vybraného souboru: krátký popisek vlevo od čáry, dlouhý vpravo */
    if ( row >= 0 && s_cad.rows[( size_t ) row].kind == CMTTMZ_ANALYZE_FILE ) {
        const double us[2] = { s_cad.rows[( size_t ) row].short_pulse_us, s_cad.rows[( size_t ) row].long_pulse_us };
        for ( int k = 0; k < 2; k++ ) {
            if ( !( us[k] > 0.0 ) || us[k] >= ( double ) axis_us ) continue;
            float x = floorf ( ax0 + ( float ) ( us[k] / axis_us ) * ( ax1 - ax0 ) ) + 0.5f;
            dl->AddLine ( ImVec2 ( x, pmin.y ), ImVec2 ( x, pmax.y ), CMT_TOK_ACCENT, ImMax ( 1.0f, floorf ( 0.5f * m->u ) ) );
            char lb[32];
            snprintf ( lb, sizeof ( lb ), "%.0f µs", us[k] );
            float lw = cad_text_w ( lb );
            float lx = ( k == 0 ) ? x - m->u - lw : x + m->u;
            lx = ImClamp ( lx, pmin.x, pmax.x - lw );
            ImVec2 lp ( floorf ( lx ), pmin.y );
            dl->AddText ( lp, CMT_TOK_ACCENT, lb );
            cad_record ( k == 0 ? "ad_hist_short" : "ad_hist_long", lb, lp, ImVec2 ( lp.x + lw, lp.y + m->Lh ), false, false, false, NULL );
        }
    }
    /* popisky osy: krok tak, aby se popisky nepřekrývaly */
    static const float steps[] = { 50.0f, 100.0f, 200.0f, 250.0f, 500.0f, 1000.0f };
    char lb[16];
    snprintf ( lb, sizeof ( lb ), "%d", ( int ) axis_us );
    float lab_w = cad_text_w ( lb ) + 6.0f * m->u;
    float step = steps[5];
    for ( float s : steps ) {
        if ( ( ax1 - ax0 ) * s / axis_us >= lab_w ) {
            step = s;
            break;
        }
    }
    float ty = pmax.y + 0.5f * m->u;
    int k = 0;
    float last_right = -FLT_MAX;
    for ( float t = 0.0f; t <= axis_us + 0.5f; t += step, k++ ) {
        snprintf ( lb, sizeof ( lb ), "%d", ( int ) t );
        float x = ax0 + t / axis_us * ( ax1 - ax0 );
        float lw = cad_text_w ( lb );
        float lx = ImClamp ( x - 0.5f * lw, ax0, ax1 - lw );
        /* popisek u kraje osy posunutý dovnitř by se překryl s předchozím: vynechá se */
        if ( lx < last_right + m->u ) continue;
        last_right = lx + lw;
        dl->AddLine ( ImVec2 ( floorf ( x ) + 0.5f, pmax.y ), ImVec2 ( floorf ( x ) + 0.5f, pmax.y + 0.5f * m->u ), CMT_TOK_CONTROL_BORDER, 1.0f );
        dl->AddText ( ImVec2 ( floorf ( lx ), ty ), CMT_TOK_TEXT_DIM, lb );
        char nb[32];
        cad_record ( CAD_NAME ( nb, "ad_hist_tick", k ), lb, ImVec2 ( floorf ( lx ), ty ), ImVec2 ( floorf ( lx ) + lw, ty + m->Lh ), false, false,
                     false, NULL );
    }
    ImVec2 up ( pmax.x - unit_w, ty );
    dl->AddText ( up, CMT_TOK_TEXT_DIM, unit );
    cad_record ( "ad_hist_unit", unit, up, ImVec2 ( up.x + unit_w, up.y + m->Lh ), false, false, false, NULL );
    ImGui::PopFont ( );
    ImGui::SetCursorScreenPos ( ImVec2 ( p0.x, ty + m->Lh ) );
    ImGui::Dummy ( ImVec2 ( m->W, 0.0f ) );
    if ( s_cad.hist_overflow ) {
        char buf[96];
        snprintf ( buf, sizeof ( buf ), _ ( "Pulses longer than %d µs are not shown." ), ( int ) axis_us );
        cad_wrapped ( "ad_hist_note", buf, CMT_TOK_TEXT_DIM, m );
    }
}

/**
 * @brief Položka voleb v řádku s zalomením: posune pozici, nevejde-li se do šířky obsahu.
 * @param x0 Levý okraj obsahu. @param m Rozměry. @param w Šířka položky. @param[in,out] pos Pozice další položky. @param line_h Výška řádku.
 */
static void cad_flow ( float x0, const st_CAD_METRICS *m, float w, ImVec2 *pos, float line_h )
{
    if ( pos->x > x0 + 0.5f && pos->x + w > x0 + m->W + 0.5f ) {
        pos->x = x0;
        pos->y += line_h + ImGui::GetStyle ( ).ItemSpacing.y;
        cad_mark ( pos->y );
    }
}

/**
 * @brief Volby analýzy: Leader tolerance (slider 2-35 %, hodnota, Reset), Analyze again vpravo; pod nimi tři opravy.
 *
 * Při delších textech se položky zalomí na další řádek (SPEC). Volby jde
 * měnit i během analýzy (výsledek pak bude zastaralý).
 *
 * @param m Rozměry. @param again_reason Důvod zákazu Analyze again (NULL = povolené).
 * @param enter Enter stisknutý v tomto snímku (Enter na zaškrtávátku opravy ho přepne, cad_enter_toggles()).
 * @param again_primary Analyze again je další krok (zastaralé výsledky): plocha výchozího tlačítka.
 * @param[out] again_id ID tlačítka Analyze again.
 * @return true = Analyze again stisknuté.
 */
static bool cad_draw_options ( const st_CAD_METRICS *m, const char *again_reason, bool enter, bool again_primary, ImGuiID *again_id )
{
    const ImGuiStyle &st = ImGui::GetStyle ( );
    const float x0 = ImGui::GetCursorScreenPos ( ).x;
    ImVec2 pos = ImGui::GetCursorScreenPos ( );
    cad_mark ( pos.y );
    const float F = m->F;
    /* řádek 1 */
    const char *tol_label = _ ( "Leader tolerance" );
    const char *tol_tip = _ ( "How much pulse lengths in the leader tone may vary. Raise it for worn or unstable recordings." );
    float lw = cad_text_w ( tol_label );
    cad_flow ( x0, m, lw, &pos, F );
    ImGui::SetCursorScreenPos ( ImVec2 ( pos.x, pos.y + st.FramePadding.y ) );
    ImGui::PushStyleColor ( ImGuiCol_Text, CMT_TOK_TEXT_DIM );
    ImGui::TextUnformatted ( tol_label );
    ImGui::PopStyleColor ( );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) ImGui::SetTooltip ( "%s", tol_tip );
    cad_record_item ( "ad_tol_label", tol_label, false, tol_tip );
    pos.x += lw + m->sp;
    float sw = floorf ( 7.0f * ImGui::GetFontSize ( ) );
    cad_flow ( x0, m, sw, &pos, F );
    ImGui::SetCursorScreenPos ( pos );
    ImGui::SetNextItemWidth ( sw );
    ImGui::PushStyleColor ( ImGuiCol_SliderGrab, CMT_TOK_ACCENT );
    ImGui::PushStyleColor ( ImGuiCol_SliderGrabActive, CMT_TOK_ACCENT );
    ImGui::SliderInt ( "##cad_tol", &s_cad.tol_pct, CAD_TOL_MIN, CAD_TOL_MAX, "", ImGuiSliderFlags_AlwaysClamp );
    ImGui::PopStyleColor ( 2 );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) ImGui::SetTooltip ( "%s", tol_tip );
    cad_record_item ( "ad_tol_slider", "", false, NULL );
    pos.x += sw + m->sp;
    char val[16];
    snprintf ( val, sizeof ( val ), "%d %%", s_cad.tol_pct );
    ImGui::PushFont ( m->mono, 0.0f );
    float vw = ImGui::CalcTextSize ( "35 %" ).x;
    cad_flow ( x0, m, vw, &pos, F );
    ImGui::SetCursorScreenPos ( ImVec2 ( pos.x + vw - ImGui::CalcTextSize ( val ).x, pos.y + st.FramePadding.y ) );
    ImGui::TextUnformatted ( val );
    ImGui::PopFont ( );
    cad_record_item ( "ad_tol_value", val, false, NULL );
    pos.x += vw + m->sp;
    const char *l_reset = _L ( "Reset" );
    float rw = cad_button_w ( l_reset );
    cad_flow ( x0, m, rw, &pos, F );
    ImGui::SetCursorScreenPos ( pos );
    if ( cad_button ( l_reset, "ad_tol_reset", NULL, false, NULL ) ) s_cad.tol_pct = CAD_TOL_DEFAULT;
    pos.x += rw + m->sp;
    const char *l_again = _L ( "Analyze again" );
    float aw = cad_button_w ( l_again );
    /* vpravo na řádku, jinak na dalším řádku vpravo */
    if ( pos.x + aw > x0 + m->W + 0.5f ) {
        pos.x = x0;
        pos.y += F + st.ItemSpacing.y;
        cad_mark ( pos.y );
    }
    pos.x = x0 + m->W - aw;
    ImGui::SetCursorScreenPos ( pos );
    bool again = cad_button ( l_again, "ad_again", again_reason, again_primary, again_id );
    /* řádek 2: opravy ve třech sloupcích, nevejdou-li se, pod sebou */
    pos.x = x0;
    pos.y += F + st.ItemSpacing.y;
    cad_mark ( pos.y );
    const char *labels[3] = { _L ( "Repair damaged header" ), _L ( "Repair damaged body" ), _L ( "Repair incomplete BSD file" ) };
    const char *names[3] = { "ad_rep_header", "ad_rep_body", "ad_rep_bsd" };
    const char *tips[3] = {
        _ ( "Keeps a file whose header is intact but whose body is missing. The body is filled with 00h." ),
        _ ( "Keeps a file whose body is incomplete, or joins two damaged copies. Missing bytes are filled with 00h." ),
        _ ( "Keeps an incomplete BSD data file in the results. BSD files cannot be added as blocks and stay in the recording." ),
    };
    bool *vals[3] = { &s_cad.rep_header, &s_cad.rep_body, &s_cad.rep_bsd };
    float cw[3];
    float colw = floorf ( m->W / 3.0f );
    bool grid = true;
    for ( int k = 0; k < 3; k++ ) {
        cw[k] = cad_checkbox_w ( labels[k] );
        if ( cw[k] + m->sp > colw ) grid = false;
    }
    for ( int k = 0; k < 3; k++ ) {
        if ( grid ) {
            pos.x = x0 + ( float ) k * colw;
        } else {
            cad_flow ( x0, m, cw[k], &pos, F );
        }
        ImGui::SetCursorScreenPos ( pos );
        ImGui::Checkbox ( labels[k], vals[k] );
        ( void ) cad_enter_toggles ( vals[k], enter );
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) ImGui::SetTooltip ( "%s", tips[k] );
        std::string shown ( labels[k], ImGui::FindRenderedTextEnd ( labels[k] ) );
        cad_record_item ( names[k], shown, false, tips[k] );
        if ( !grid ) pos.x += cw[k] + 2.0f * m->sp;
    }
    ImGui::SetCursorScreenPos ( ImVec2 ( x0, pos.y + F + st.ItemSpacing.y ) );
    return again;
}


void cmt_analyze_dialog_draw ( const st_CMT_VIEW *v, const char *edit_reason )
{
    cad_init_once ( );
    if ( s_cad.state == CAD_STATE_NONE ) return;
    const int frame = ImGui::GetFrameCount ( );
    s_cad.drawn_frame = frame;
    cad_poll ( );
#ifdef MZTEST_HEADLESS
    s_cad_items.clear ( );
    s_cad_drawn = false;
#endif
    /* po snímcích bez obsahu okna: popup, který mezitím zavřelo ImGui, otevřít znovu (stav dialogu zůstal) */
    if ( s_cad.skipped ) {
        s_cad.skipped = false;
        if ( !s_cad.open_pending && s_cad.popup_id && !ImGui::IsPopupOpen ( s_cad.popup_id, ImGuiPopupFlags_None ) ) s_cad.open_pending = true;
    }
    /* otevření jen při volnu (imgui_modal.h): jiný otevřený popup by OpenPopup() zavřelo */
    if ( s_cad.open_pending ) {
        if ( !imgui_modal_open_when_free ( CAD_POPUP_ID ) ) return;
        s_cad.open_pending = false;
        s_cad.popup_id = ImGui::GetID ( CAD_POPUP_ID );
    }
    char title[128];
    snprintf ( title, sizeof ( title ), _ ( "Analyze recording - block %d" ), s_cad.block_id + 1 );
    std::string full_title = std::string ( title ) + CAD_POPUP_ID;
#ifdef MZTEST_HEADLESS
    s_cad_title = full_title;
#endif

    st_CAD_METRICS m;
    const ImGuiStyle &st = ImGui::GetStyle ( );
    m.F = ImGui::GetFrameHeight ( );
    m.Lh = ImGui::GetTextLineHeight ( );
    m.u = ImGui::GetFontSize ( ) / 7.0f;
    m.rh = m.Lh + m.u;
    m.cp = 2.0f * m.u;
    m.sp = st.ItemSpacing.x;
    m.mono = myimgui_get_monospace_font ( );
    if ( !m.mono ) m.mono = ImGui::GetFont ( );

    /* --- rozvržení: horní část (věta, souhrn), posouvané tělo (tabulka, nápověda, volby, Signal details),
     *     patička (varování, tlačítka) vždy vidět; celé nejvýš 90 % pracovní plochy --- */
    const ImVec2 work = imgui_cmt_window_work_size ( );
    const float max_w = floorf ( 0.9f * work.x ) - 2.0f * st.WindowPadding.x - 2.0f * st.WindowBorderSize;
    const float max_h = floorf ( 0.9f * work.y );
    cad_col_widths ( &m );
    const char *l_add = _L ( "Add files after recording" );
    const char *l_replace = _L ( "Replace recording" );
    const char *l_cancel = _L ( "Cancel" );
    const float bw[3] = { cad_button_w ( l_add ), cad_button_w ( l_replace ), cad_button_w ( l_cancel ) };
    const float buttons_w = bw[0] + bw[1] + bw[2] + 2.0f * m.sp;
    float opts_w = cad_text_w ( _ ( "Leader tolerance" ) ) + floorf ( 7.0f * ImGui::GetFontSize ( ) ) + cad_button_w ( _L ( "Reset" ) )
        + cad_button_w ( _L ( "Analyze again" ) ) + ImGui::CalcTextSize ( "35 %" ).x + 5.0f * m.sp;
    const int nrows = ( int ) s_cad.rows.size ( );
    const float rh = m.rh;
    const float deco = m.F + 2.0f * st.WindowPadding.y + 2.0f * st.WindowBorderSize;
    const float top_h = ( s_cad.top_h > 0.0f ) ? s_cad.top_h : 3.0f * m.Lh;
    const float footer_h = ( s_cad.footer_h > 0.0f ) ? s_cad.footer_h : m.F + m.Lh + 2.0f * st.ItemSpacing.y;
    /* místo pro posouvané tělo (mezi částmi ItemSpacing) */
    const float body_max = ImMax ( 2.0f * rh, max_h - deco - top_h - footer_h - 2.0f * st.ItemSpacing.y - 2.0f );
    /* výška tabulky: řádky (aspoň CAD_TABLE_MIN_ROWS), omezená zbytkem těla z minulého snímku; s rozbaleným
     * histogramem se dělí místo: tabulka aspoň CAD_TABLE_MIN_ROWS řádků, histogram 2 až 4 řádky textu */
    const float nontable_base = ( s_cad.nontable_h > 0.0f ) ? ImMax ( 0.0f, s_cad.nontable_h - s_cad.plot_h_used ) : 8.0f * m.F;
    const float avail = body_max - nontable_base;
    const bool hist = s_cad.details_open && s_cad.pulses && s_cad.pulses->p->count > 0;
    s_cad.plot_h = 0.0f;
    if ( hist ) {
        const float table_min_h = ( float ) ( 1 + CAD_TABLE_MIN_ROWS ) * rh + 1.0f;
        s_cad.plot_h = floorf ( ImClamp ( avail - table_min_h, 2.0f * m.Lh, 4.0f * m.Lh ) );
    }
    const float table_budget = avail - s_cad.plot_h;
    /* místo pro vodorovný posuvník tabulky se drží, kdykoli může nastat (přirozená šířka se svislým posuvníkem
     * přes šířku dialogu) - nezávisle na tom, zda ho měl minulý snímek, jinak by se tabulka přepínala mezi
     * vlastním posunem a posunem těla */
    const float sx_reserve = ( cad_table_natural_w ( &m, true ) > max_w - 0.5f ) ? st.ScrollbarSize : 0.0f;
    const int fit_rows = ( int ) floorf ( ( table_budget - sx_reserve - rh - 1.0f ) / rh );
    const int all_rows = ImMax ( nrows, CAD_TABLE_MIN_ROWS );
    const bool show_rows = ( s_cad.state == CAD_STATE_DONE && s_cad.files > 0 );
    /* jen jeden svislý posuvník: (A) všechny řádky se vejdou - tabulka i tělo bez posuvníku;
     * (B) vejde se aspoň CAD_TABLE_MIN_ROWS řádků - tabulka se posouvá sama, tělo se vejde; (C) jinak má tabulka
     * výšku všech řádků bez vlastního posuvníku a posouvá se tělo (ImGuiListClipper kreslí jen viditelné řádky) */
    int vis_rows = all_rows;
    bool table_own = false;
    if ( show_rows && all_rows > fit_rows && fit_rows >= CAD_TABLE_MIN_ROWS ) {
        vis_rows = fit_rows;
        table_own = true;
    }
    /* případ (C) s rozbaleným histogramem: tělo se posouvá tak jako tak, histogram dostane plnou výšku */
    if ( hist && show_rows && !table_own && all_rows > fit_rows ) s_cad.plot_h = floorf ( 4.0f * m.Lh );
    const bool vscroll = table_own && vis_rows < nrows;
    /* tělo se bude posouvat (obsah minulého snímku větší než místo; stejné zaokrouhlení jako výška těla):
     * šířka obsahu těla bez posuvníku */
    const float body_lim = floorf ( body_max );
    const bool body_scroll = s_cad.body_content_h > body_lim;
    const float sbw = body_scroll ? st.ScrollbarSize : 0.0f;
    float W = ImMax ( cad_table_natural_w ( &m, vscroll ) + sbw, ImMax ( buttons_w, opts_w + sbw ) );
    W = ImMax ( W, floorf ( 20.0f * ImGui::GetFontSize ( ) ) );
    W = floorf ( ImMin ( W, max_w ) );
    m.W = W - sbw;
    st_CAD_METRICS mf = m; /* patička a horní část: celá šířka */
    mf.W = W;
    /* výška těla: obsah, nejvýš body_lim; posouvá-li se, hrany jen mezi řádky (značky minulého snímku):
     * s rozbaleným histogramem tak, aby po posunu na konec (histogram s osou) horní hrana ležela na
     * začátku řádku, jinak spodní hrana na začátku prvního řádku, který se celý nevejde */
    float body_h = floorf ( ImMin ( ( s_cad.body_content_h > 0.0f ) ? s_cad.body_content_h : body_max, body_lim ) );
    if ( body_scroll && !s_cad.body_marks.empty ( ) ) {
        const float end = s_cad.body_content_h;
        float best = -1.0f;
        for ( float mk : s_cad.body_marks ) {
            const float h = hist ? end - mk : mk;
            if ( h <= body_lim + 0.5f && h > best ) best = h;
        }
        if ( best >= 2.0f * rh ) body_h = floorf ( best + 0.5f );
    }

    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 0.0f, 0.0f ), ImVec2 ( floorf ( 0.9f * work.x ), max_h ) );
    imgui_cmt_window_modal_center ( s_cad.last_size, s_cad.recenter_frames > 0 ? ImGuiCond_Always : ImGuiCond_Appearing );
    if ( s_cad.recenter_frames > 0 ) s_cad.recenter_frames--;
    if ( !ImGui::BeginPopupModal ( full_title.c_str ( ), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings ) ) {
        /* modál zavřel někdo jiný (jiný modál, reset): jako Cancel */
        s_cad.popup_id = 0;
        cad_reset ( );
        return;
    }
    const bool appearing = ImGui::IsWindowAppearing ( );
    /* prvek aktivní na začátku snímku (slider, zadávání textu): Escape a mezerník patří jemu */
    const bool editing = ( ImGui::GetActiveID ( ) != 0 );
    const bool enter = !appearing && !editing && ( ImGui::IsKeyPressed ( ImGuiKey_Enter, false ) || ImGui::IsKeyPressed ( ImGuiKey_KeypadEnter, false ) );
    ImGuiWindow *win = ImGui::GetCurrentWindow ( );
    const float y_start = ImGui::GetCursorScreenPos ( ).y;
    /* šířka obsahu dialogu (CollapsingHeader a zalomení se k ní vztahují) */
    ImGui::Dummy ( ImVec2 ( W, 0.0f ) );

    const bool tape_changed = !v->has_tape || v->tape_serial != s_cad.tape_serial;
    const bool busy = ( s_cad.state == CAD_STATE_COPYING || s_cad.state == CAD_STATE_WAITING || s_cad.state == CAD_STATE_RUNNING );
    const bool stale = cad_stale ( );
    int checked = 0;
    for ( size_t i = 0; i < s_cad.use.size ( ); i++ )
        if ( s_cad.use[i] && cad_row_addable ( &s_cad.rows[i] ) ) checked++;

    /* --- věta výsledku --- */
    std::string headline;
    if ( busy ) {
        headline = _ ( "Analyzing..." );
    } else if ( s_cad.state == CAD_STATE_ERROR ) {
        headline = _ ( "The recording could not be analyzed." );
    } else if ( s_cad.files == 0 ) {
        headline = _ ( "No files found." );
    } else {
        char buf[160];
        if ( s_cad.files_bad == 0 ) {
            if ( s_cad.files == 1 ) snprintf ( buf, sizeof ( buf ), "%s", _ ( "1 file found." ) );
            else snprintf ( buf, sizeof ( buf ), _ ( "%d files found." ), s_cad.files );
        } else if ( s_cad.files == 1 ) {
            snprintf ( buf, sizeof ( buf ), "%s", _ ( "1 file found, 1 with a checksum error." ) );
        } else {
            snprintf ( buf, sizeof ( buf ), _ ( "%d files found, %d with a checksum error." ), s_cad.files, s_cad.files_bad );
        }
        headline = buf;
    }
    ImGui::PushFont ( NULL, ImGui::GetCurrentContext ( )->FontSizeBase * 1.2f );
    {
        std::vector<std::string> lines = cad_wrap ( headline, W );
        ImVec2 p0 = ImGui::GetCursorScreenPos ( );
        float wmax = 0.0f;
        for ( const std::string &l : lines ) {
            ImGui::TextUnformatted ( l.c_str ( ), l.c_str ( ) + l.size ( ) );
            wmax = ImMax ( wmax, cad_text_w ( l ) );
        }
        cad_record ( "ad_headline", headline, p0, ImVec2 ( p0.x + wmax, ImGui::GetItemRectMax ( ).y ), false, false, false, NULL );
    }
    ImGui::PopFont ( );

    /* --- ztlumený souhrn bloku --- */
    {
        char buf[200];
        double len_s = s_cad.block_len_s;
        if ( len_s < 0.0 && s_cad.pulses ) len_s = ( double ) s_cad.pulses->p->total_us / 1e6;
        snprintf ( buf, sizeof ( buf ), _ ( "%s, %s, %s pulses" ), s_cad.block_name.c_str ( ), cad_time_text ( len_s ).c_str ( ),
                   cad_thousands ( s_cad.pulse_count ).c_str ( ) );
        cad_wrapped ( "ad_summary", buf, CMT_TOK_TEXT_DIM, &mf );
    }
    s_cad.top_h = ImGui::GetCursorScreenPos ( ).y - y_start;

    /* --- posouvané tělo: tabulka, nápověda, volby, Signal details --- */
    ImGui::BeginChild ( "##cad_body", ImVec2 ( W, body_h ), ImGuiChildFlags_NavFlattened, 0 );
#ifdef MZTEST_HEADLESS
    s_cad_in_body = true;
    s_cad_body_min = ImGui::GetCurrentWindow ( )->ClipRect.Min;
    s_cad_body_max = ImGui::GetCurrentWindow ( )->ClipRect.Max;
    s_cad_body_scroll = ImGui::GetCurrentWindow ( )->ScrollbarY;
    s_cad_table_id = 0;
    s_cad_table_scroll = false;
#endif
    s_cad.body_win = ImGui::GetCurrentWindow ( );
    s_cad.marking = true;
    s_cad.body_marks_cur.clear ( );
    /* nový výsledek bez rozbaleného histogramu: tělo od začátku */
    if ( s_cad.body_scroll_reset ) {
        ImGui::SetScrollY ( 0.0f );
        s_cad.body_scroll_reset = false;
    }
    std::vector<std::string> body;
    ImU32 body_color = CMT_TOK_TEXT_DIM;
    bool progress = false;
    if ( busy ) {
        char buf[96];
        snprintf ( buf, sizeof ( buf ), _ ( "Elapsed time: %s" ), cad_time_text ( ImGui::GetTime ( ) - s_cad.run_start ).c_str ( ) );
        body = cad_wrap ( buf, m.W - 2.0f * m.cp );
        progress = true;
    } else if ( s_cad.state == CAD_STATE_ERROR ) {
        /* text chyby jádra nebo analyzátoru (anglicky, nepřekládá se; čísla bloků od 1, cmt_view_core_text()) */
        body = cad_wrap ( s_cad.error, m.W - 2.0f * m.cp );
        body_color = CMT_TOK_ERROR;
    } else if ( s_cad.files == 0 ) {
        for ( const std::string &l : cad_wrap ( _ ( "No files found in this recording." ), m.W - 2.0f * m.cp ) ) body.push_back ( l );
        for ( const std::string &l : cad_wrap ( _ ( "Try a different leader tolerance, or check the signal details below." ), m.W - 2.0f * m.cp ) )
            body.push_back ( l );
    }
    float table_h;
    if ( body.empty ( ) ) {
        table_h = ( float ) ( 1 + vis_rows ) * rh + 1.0f;
    } else {
        /* jen hlavička a věty (nic nenalezeno: malá plocha) */
        table_h = rh + 1.0f + ( float ) body.size ( ) * m.Lh + ( progress ? m.u + 0.5f * m.Lh : 0.0f ) + 4.0f * m.u;
    }
    bool row_focused = false;
    cad_draw_table ( &m, ImVec2 ( m.W, table_h ), body, body_color, progress, stale, v->default_speed_text, editing, enter, table_own,
                     &row_focused );
    cad_record_value ( "ad_table_dim", stale ? "1" : "0" );

    /* --- řádek nápovědy k vybranému řádku --- */
    if ( s_cad.state == CAD_STATE_DONE && s_cad.files > 0 ) {
        if ( stale ) {
            cad_wrapped ( "ad_hint", cad_text_stale ( ), CMT_TOK_WARN, &m );
        } else if ( s_cad.sel_row >= 0 && s_cad.sel_row < nrows ) {
            const st_CMTTMZ_ANALYZE_ROW *sr = &s_cad.rows[( size_t ) s_cad.sel_row];
            std::string d = cad_row_detail ( sr, v->default_speed_text );
            if ( !d.empty ( ) && d.back ( ) != '.' ) d += ".";
            const std::string remedy = cad_row_remedy ( sr );
            if ( !remedy.empty ( ) ) d += " " + remedy;
            cad_wrapped ( "ad_hint", s_cad.row_ui[( size_t ) s_cad.sel_row].name + ": " + d, CMT_TOK_TEXT, &m );
        } else {
            cad_wrapped ( "ad_hint", _ ( "Checked files are added as tape blocks. Everything else stays in the recording." ), CMT_TOK_TEXT_DIM, &m );
        }
    }
    ImGui::Dummy ( ImVec2 ( 0.0f, m.u ) );

    /* --- volby --- */
    const char *again_reason = tape_changed ? cad_text_tape_changed ( )
        : ( busy ? _ ( "Wait until the analysis finishes." ) : ( !s_cad.pulses ? _ ( "The recording could not be read." ) : NULL ) );
    /* zastaralé výsledky: Analyze again je další krok - plocha výchozího tlačítka a Enter (bez fokusu na volbě,
     * Enter na zaškrtávátku ho přepne) ho spustí; po doběhnutí analýzy je výchozí zase Add.
     * Fokus navigace se volbě, kterou uživatel právě změnil, nebere. */
    ImGuiID again_id = 0;
    const bool again_primary = stale && !again_reason;
    const bool do_again = cad_draw_options ( &m, again_reason, enter, again_primary, &again_id ) && !again_reason;

    /* --- Signal details (sbalené; nic nenalezeno = rozbalené): hlavička jako vlastnosti bloku okna --- */
    if ( s_cad.pulses && s_cad.pulses->p->count > 0 ) {
        const char *l_det = _ ( "Signal details" );
        cad_mark ( );
        s_cad.details_open = imgui_cmt_section_header ( "##cad_details", l_det, s_cad.details_open, m.W );
        cad_record ( "ad_details", l_det, ImGui::GetItemRectMin ( ), ImGui::GetItemRectMax ( ), true, false, false, NULL );
        /* rozbalení (klik, rozbalení samo při "nic nenalezeno"): tělo na histogram s osou; vystředění
         * dá změna velikosti */
        if ( s_cad.details_open && !s_cad.details_drawn_open ) s_cad.scroll_hist_frames = 3;
        if ( s_cad.details_open ) {
            /* rozbaleno právě v tomto snímku: výšku dá až příští snímek, zatím nejmenší */
            if ( s_cad.plot_h <= 0.0f ) s_cad.plot_h = floorf ( 2.0f * m.Lh );
            cad_draw_histogram ( &m );
        }
    }
    s_cad.plot_h_used = ( s_cad.details_open && s_cad.pulses && s_cad.pulses->p->count > 0 ) ? s_cad.plot_h : 0.0f;
    /* poslední pozice kurzoru (SetCursorScreenPos() voleb) potvrzená položkou: ImGui jinak hlásí rozšíření okna bez položky */
    ImGui::Dummy ( ImVec2 ( 0.0f, 0.0f ) );
    {
        /* výška obsahu těla a obsahu mimo tabulku pro příští snímek */
        ImGuiWindow *bw_win = ImGui::GetCurrentWindow ( );
        s_cad.body_content_h = ceilf ( bw_win->DC.CursorMaxPos.y - bw_win->DC.CursorStartPos.y );
        s_cad.nontable_h = ImMax ( 0.0f, s_cad.body_content_h - table_h - s_cad.table_extra_h );
        /* posun na rozbalený histogram: konec obsahu (histogram s osou) na spodní hranu těla; výška těla je zvolená tak,
         * aby horní hrana pak ležela na začátku řádku */
        if ( s_cad.scroll_hist_frames > 0 ) {
            if ( s_cad.details_open ) ImGui::SetScrollY ( ImMax ( 0.0f, s_cad.body_content_h - bw_win->Size.y ) );
            s_cad.scroll_hist_frames--;
        }
    }
    s_cad.details_drawn_open = s_cad.details_open;
    s_cad.marking = false;
    s_cad.body_win = NULL;
    s_cad.body_marks.swap ( s_cad.body_marks_cur );
#ifdef MZTEST_HEADLESS
    s_cad_in_body = false;
#endif
    ImGui::EndChild ( );
    if ( do_again ) cad_start_analysis ( );

    /* --- patička: varování a tlačítka (vždy vidět) --- */
    const float y_footer = ImGui::GetCursorScreenPos ( ).y;
    if ( tape_changed ) cad_wrapped ( "ad_warning", cad_text_tape_changed ( ), CMT_TOK_WARN, &mf );
    else if ( s_cad.unsaved ) cad_wrapped ( "ad_warning", _ ( "This recording is not saved yet. Replacing it cannot be undone." ), CMT_TOK_WARN, &mf );

    /* --- tlačítka: vpravo; nevejdou-li se, zalomí se (každý řádek vpravo) --- */
    const char *r_act = NULL;
    if ( tape_changed ) r_act = cad_text_tape_changed ( );
    else if ( busy ) r_act = _ ( "Wait until the analysis finishes." );
    else if ( s_cad.state == CAD_STATE_ERROR ) r_act = _ ( "The recording could not be analyzed." );
    else if ( stale ) r_act = cad_text_stale ( );
    else if ( edit_reason ) r_act = edit_reason;
    else if ( s_cad.files == 0 ) r_act = _ ( "There are no files to add." );
    else if ( checked == 0 ) r_act = _ ( "Check at least one file to add it." );
    ImGuiID add_id = 0, replace_id = 0, cancel_id = 0;
    bool do_add = false, do_replace = false, do_cancel = false;
    {
        const float x0 = ImGui::GetCursorScreenPos ( ).x;
        float y = ImGui::GetCursorScreenPos ( ).y;
        int first = 0;
        while ( first < 3 ) {
            int last = first;
            float line_w = bw[first];
            while ( last + 1 < 3 && line_w + m.sp + bw[last + 1] <= W + 0.5f ) {
                last++;
                line_w += m.sp + bw[last];
            }
            float x = x0 + W - line_w;
            for ( int k = first; k <= last; k++ ) {
                ImGui::SetCursorScreenPos ( ImVec2 ( x, y ) );
                if ( k == 0 ) do_add = cad_button ( l_add, "ad_add", r_act, true, &add_id );
                else if ( k == 1 ) do_replace = cad_button ( l_replace, "ad_replace", r_act, false, &replace_id );
                else do_cancel = cad_button ( l_cancel, "ad_cancel", NULL, false, &cancel_id );
                if ( k == 0 ) {
                    /* výchozí tlačítko: počáteční fokus navigace i zakázané (Enter pak nic) a viditelný kurzor
                     * navigace; počitadlo klesá každý snímek, fokus později nepřeskočí */
                    ImGui::SetItemDefaultFocus ( );
                    if ( s_cad.focus_add_frames > 0 ) {
                        ImGui::SetFocusID ( add_id, win );
                        /* rámeček fokusu jen u povoleného Add (zakázané s rámečkem by mátlo) */
                        if ( !r_act ) ImGui::SetNavCursorVisible ( true );
                        s_cad.focus_add_frames--;
                    }
                }
                x += bw[k] + m.sp;
            }
            y += m.F + st.ItemSpacing.y;
            first = last + 1;
        }
    }
    s_cad.footer_h = ImGui::GetCursorScreenPos ( ).y - y_footer;
    /* klávesy: Enter = Add (fokus na Add, na řádku tabulky nebo nikde), Escape = Cancel */
    const ImGuiID focus = ImGui::GetFocusID ( );
    if ( enter && ( focus == 0 || focus == add_id || row_focused ) && !r_act ) do_add = true;
    /* zastaralé výsledky: Enter = Analyze again (výchozí tlačítko); s fokusem na Analyze again ho
     * stiskne ImGui samo (do_again), proto jen jiný fokus a jen jednou */
    if ( enter && !do_again && !again_reason && cad_stale ( ) && ( focus == 0 || focus == add_id || row_focused ) ) cad_start_analysis ( );
    /* Escape ze snímku, kdy se modál otevřel (zavřel jiný popup), se nepočítá */
    if ( !appearing && !editing && ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) ) do_cancel = true;
#ifdef MZTEST_HEADLESS
    {
        const char *fs = ( focus == 0 ) ? "none" : ( focus == add_id ? "add" : ( focus == replace_id ? "replace"
                                                                                    : ( focus == cancel_id ? "cancel" : ( row_focused ? "row" : "other" ) ) ) );
        cad_record_value ( "ad_focus", fs );
        static const char *states[] = { "none", "copying", "waiting", "running", "done", "error" };
        cad_record_value ( "ad_state", states[s_cad.state] );
    }
#endif

    /* znovu doprostřed (a do plochy) jen při objevení a když se změní velikost dialogu (stav, rozbalení, delší
     * nápověda); poloha, kam ho uživatel přetáhl za titulek, se jinak nemění */
    if ( !appearing && s_cad.last_size.x > 0.0f
         && ( fabsf ( win->Size.x - s_cad.last_size.x ) > 0.5f || fabsf ( win->Size.y - s_cad.last_size.y ) > 0.5f ) )
        s_cad.recenter_frames = ImMax ( s_cad.recenter_frames, 2 );
    s_cad.last_size = win->Size;
#ifdef MZTEST_HEADLESS
    s_cad_wmin = win->Pos;
    s_cad_wmax = ImVec2 ( win->Pos.x + win->Size.x, win->Pos.y + win->Size.y );
    s_cad_imin = win->InnerRect.Min;
    s_cad_imax = win->InnerRect.Max;
    s_cad_drawn = true;
#endif

    if ( ( do_add || do_replace ) && !r_act && s_cad.analysis && s_cad.pulses ) {
        /* výběr se kopíruje pro zásah: data drží modul do výsledku (st_CAD_APPLY) */
        const size_t n = s_cad.use.size ( );
        uint8_t *use = ( uint8_t * ) g_malloc0 ( n ? n : 1 );
        for ( size_t i = 0; i < n; i++ ) use[i] = ( s_cad.use[i] && cad_row_addable ( &s_cad.rows[i] ) ) ? 1u : 0u;
        uint32_t seq = cmt_request_analysis_apply ( s_cad.pulses->p, s_cad.analysis, use, do_replace ? 1 : 0 );
        if ( !seq ) {
            g_free ( use );
            cmt_view_post ( _ ( "The tape could not be edited." ), CMT_VIEW_MSG_ERROR );
        } else {
            st_CAD_APPLY ap;
            ap.seq = seq;
            ap.pulses = cad_pulses_ref ( s_cad.pulses );
            ap.analysis = s_cad.analysis;
            s_cad.analysis = NULL;
            ap.use = use;
            ap.idle_frame = -1;
            s_cad_applies.push_back ( ap );
        }
        do_cancel = true;
    }
    if ( do_cancel ) {
        ImGui::CloseCurrentPopup ( );
        cad_reset ( );
    }
    ImGui::EndPopup ( );
#ifdef MZTEST_HEADLESS
    s_cad_items_done = s_cad_items;
#endif
}


/* ------------------------------------------------------------------------- */
/*  Testovací rozhraní                                                       */
/* ------------------------------------------------------------------------- */

#ifdef MZTEST_HEADLESS
void cmt_analyze_dialog_test_set_manual_runner ( bool manual )
{
    s_cad_test_manual = manual;
}

int cmt_analyze_dialog_test_run_jobs ( void )
{
    std::vector<st_CAD_JOB *> jobs;
    jobs.swap ( s_cad_test_jobs );
    for ( st_CAD_JOB *j : jobs ) cad_worker_body ( j );
    return ( int ) jobs.size ( );
}

int cmt_analyze_dialog_test_queued_jobs ( void )
{
    return ( int ) s_cad_test_jobs.size ( );
}

void cmt_analyze_dialog_test_set_analyzer ( cmt_analyze_dialog_analyzer_fn fn )
{
    g_atomic_pointer_set ( &s_cad_analyzer, ( gpointer ) ( fn ? fn : cmttmz_analyze_pulses ) );
}

int cmt_analyze_dialog_test_live_jobs ( void )
{
    return g_atomic_int_get ( &s_cad_live_jobs );
}

int cmt_analyze_dialog_test_live_pulses ( void )
{
    return g_atomic_int_get ( &s_cad_live_pulses );
}

int cmt_analyze_dialog_test_pending_applies ( void )
{
    return ( int ) s_cad_applies.size ( );
}

const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_analyze_dialog_test_items ( void )
{
    return s_cad_items_done;
}

bool cmt_analyze_dialog_test_rect ( ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax )
{
    *wmin = s_cad_wmin;
    *wmax = s_cad_wmax;
    *imin = s_cad_imin;
    *imax = s_cad_imax;
    return s_cad_drawn && cmt_analyze_dialog_is_open ( );
}

const char *cmt_analyze_dialog_test_title ( void )
{
    return s_cad_title.c_str ( );
}

void cmt_analyze_dialog_test_select_row ( int row )
{
    s_cad.sel_row = row;
}

void cmt_analyze_dialog_test_set_details ( bool open )
{
    s_cad.details_open = open;
}

st_CMTTMZ_ANALYZE_OPTS cmt_analyze_dialog_test_last_opts ( void )
{
    return s_cad_test_last_opts;
}

bool cmt_analyze_dialog_test_body_rect ( ImVec2 *min, ImVec2 *max, bool *scroll )
{
    *min = s_cad_body_min;
    *max = s_cad_body_max;
    *scroll = s_cad_body_scroll;
    return s_cad_drawn && cmt_analyze_dialog_is_open ( );
}

ImGuiID cmt_analyze_dialog_test_table ( bool *scrollbar )
{
    if ( scrollbar ) *scrollbar = s_cad_table_scroll;
    return s_cad_table_id;
}

void cmt_analyze_dialog_test_set_exit_join_ms ( int ms )
{
    s_cad_exit_join_ms = ms;
}

int cmt_analyze_dialog_test_max_passes ( void )
{
    return g_atomic_int_get ( &s_cad_test_max_passes );
}

int cmt_analyze_dialog_test_threads ( void )
{
    return ( int ) s_cad_threads.size ( );
}

void cmt_analyze_dialog_test_open_with_pulses ( const st_CMT_VIEW *v, int block_id, st_CMTTMZ_PULSES *p )
{
    cmt_analyze_dialog_open ( v, block_id );
    /* zásah kopie pulzů zařazený otevřením se nepoužije: jeho výsledek dialog zahodí (jiné seq) */
    s_cad.pulses_seq = 0;
    s_cad.state = CAD_STATE_COPYING;
    s_cad.error.clear ( );
    s_cad.pulses = cad_pulses_new ( p );
    s_cad.pulse_count = p->count;
    s_cad.block_len_s = ( double ) p->total_us / 1e6;
    cad_start_analysis ( );
}

void cmt_analyze_dialog_test_reset ( void )
{
    cad_reset ( );
    cad_auto_drop ( CAD_AUTO_IDLE );
    s_cad_auto.announced = false;
    /* nespuštěné "vlákno" testu: úlohy provést (pustí odkazy a uvolní data) */
    ( void ) cmt_analyze_dialog_test_run_jobs ( );
    if ( device_request_pending ( ) == 0 ) {
        for ( st_CAD_APPLY &ap : s_cad_applies ) cad_apply_release ( &ap );
        s_cad_applies.clear ( );
    }
    s_cad_test_manual = false;
    ( void ) cad_threads_finish ( 5000 );
    g_atomic_pointer_set ( &s_cad_analyzer, ( gpointer ) cmttmz_analyze_pulses );
    s_cad_exit_join_ms = CAD_EXIT_JOIN_MS;
    s_cad_items.clear ( );
    s_cad_items_done.clear ( );
    s_cad_drawn = false;
    if ( ImGui::GetCurrentContext ( ) && ImGui::IsPopupOpen ( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) )
        ImGui::ClosePopupToLevel ( 0, false );
}
#endif
