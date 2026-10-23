/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file mz1p16_emu.c
 * @brief Integrační vrstva plotteru MZ-1P16 do emulátoru - implementace.
 *
 * Drží globální instanci mechaniky + jádra 8050 a napojuje je do životního
 * cyklu emulátoru. Vzor = printer.c.
 *
 * Krokování jádra (clock domain):
 *  Plotter 8050 běží ASYNCHRONNĚ vůči Z80. Krokuje se DETERMINISTICKY podle
 *  UDÁLOSTÍ na sběrnici (ne podle uplynulého času), takže kresba je čistě
 *  funkcí sekvence handshake událostí a nezávisí na časování hostitelského
 *  vlákna:
 *   - zápis dat + STROBE (pioz80 PA write) -> mz1p16_emu_sync_read_byte:
 *     odkrokuje firmware, dokud bajt nepřijme (BUSY 0->1) = bajt přijat
 *     právě jednou,
 *   - čtení stavu/BUSY (pioz80 PA read) -> mz1p16_emu_drive_poll: posun o
 *     pevný počet cyklů; host v busy-wait smyčce čte BUSY opakovaně a tím
 *     firmware plynule dokresluje tah,
 *   - per video frame (mz1p16_emu_on_screen_done): dokrokuje pevný rozpočet,
 *     aby rozdělaná práce (poslední znak) doběhla i když host přestane
 *     pollovat.
 *
 * Fast-forward: homing/selftest trvá ~2,15 mil. cyklů (~5,4 s realtime).
 * Aby uživatel po (re)aktivaci/resetu nečekal, doženeme homing zrychleně
 * (mz1p16_emu_schedule_catchup): per-frame krok odkrokuje místo běžného
 * rozpočtu až MZ1P16_EMU_CATCHUP_CHUNK_CYCLES, dokud dluh nesplatí. Stejně
 * self-test. Dřív se doháněl najednou a emulace stroje i zvuk stály.
 */

#include <stdint.h>
#include <stdbool.h>

#include "mz1p16/mz1p16_emu.h"
#include "mz1p16/mz1p16.h"
#include "mz1p16/mcs48.h"
#include "mz1p16/mz1p16_rom.h"

#include "pioz80/pioz80.h"
#include "printer/printer.h"

#include "libs/sdlapp/sdlapp_options.h"
#include "libs/cfgfile/cfgmodule.h"
#include "cfgmain.h"

#include "app/app_thread.h"

//#define DBGLEVEL (DBGNON | DBGERR | DBGWAR | DBGINF )
#include "debug.h"

st_MZ1P16 g_mz1p16;
st_MCS48  g_mz1p16_cpu;

/** Idempotence flag pro @ref mz1p16_emu_init. */
static bool s_initialized = false;

/**
 * @brief Jediný serializační mutex pro veškerý přístup k jádru/stavu plotteru.
 *
 * Plotter (g_mz1p16_cpu, g_mz1p16 vč. strokes) se dotýká z více vláken: emu
 * vlákno (per-frame doběh, pioz80 handshake hooky) i UI vlákno (render
 * stroke bufferu, akční tlačítka). Bez serializace dochází k datovému závodu.
 * Tento mutex serializuje VŠECHNY vnější vstupní body do plotter jádra.
 *
 * @note Zámek se NEvnořuje: vnitřní práce (drive_poll a privátní helpery)
 *       zámek NEbere a předpokládá, že ho drží volající vnější vstup.
 * @note NULL-safe: dokud není vytvořen (před @ref mz1p16_emu_init), jsou
 *       @ref mz1p16_emu_lock / @ref mz1p16_emu_unlock no-op.
 */
static app_mutex_t *s_mz1p16_lock = NULL;

/**
 * @brief Strop instrukcí pro synchronní přečtení bajtu (mz1p16_emu_sync_read_byte).
 *
 * Po dodání bajtu se firmware krokuje, dokud bajt nepřečte (IN A,P2). Poll
 * smyčka + read je řádově desítky instrukcí; strop je pojistka pro případ, že
 * firmware právě kreslí (busy) a hned nečte.
 */
#define MZ1P16_SYNC_READ_MAX_STEPS 4000

/**
 * @brief Počet strojových cyklů 8050 odkrokovaných na 1 čtení stavu hostem.
 *
 * DETERMINISTICKÉ (event-driven) krokování plotteru: jádro 8050 se NEhýbe podle
 * uplynulého času, ale podle UDÁLOSTÍ na sběrnici. Při každém host čtení stavu
 * (BUSY/PA0 v poll smyčce MZ-800 ROM) se firmware posune o tento pevný počet
 * cyklů. Host v busy-wait smyčce čte BUSY opakovaně, čímž firmware plynule
 * dokresluje tah; počet iterací je dán jen stavem firmwaru (kdy shodí BUSY),
 * ne časem -> stejný proud bajtů + stejný handshake = VŽDY stejná kresba.
 *
 * Krokování nezávislé na čase je nutné pro determinismus: časově úměrný
 * catch-up by firmware krokoval pokaždé jinak (mezera mezi bajty kolísá podle
 * časování přerušení v hostitelské smyčce), takže by handshake hrany a čtení
 * souřadnic padaly do jiných okamžiků a bajt by se občas zpracoval špatně.
 *
 * @note [ladit] - vyšší = rychlejší plotter (firmware víc cyklů na poll, busy
 *       zhasne dřív), nižší = pomalejší.
 */
#define MZ1P16_DRIVE_CYCLES_PER_POLL 4

/**
 * @brief Počet strojových cyklů 8050 odkrokovaných per video frame (on_screen_done).
 *
 * Event-driven krokování (mz1p16_emu_drive_poll) posouvá firmware jen při host
 * I/O. Když ale host po posledním bajtu přestane pollovat BUSY (např. konec
 * řádku v PLOT ON režimu = psací stroj), firmware zůstane uprostřed kreslení
 * posledního znaku a "zamrzne" - znak se dotiskne až s dalším bajtem. Proto
 * per-frame firmware bezpodmínečně dokrokujeme tímto rozpočtem: rozdělaná práce
 * doběhne, a když je firmware v klidu (ready), jen neškodně protočí poll smyčku.
 *
 * Rozpočet je shora omezen, ať jediný frame neblokuje emu vlákno; běžný znak
 * (~desítky tisíc cyklů) se tak dokreslí do jednoho frame. Kresba míří na
 * absolutní souřadnicové cíle, takže navíc dokrokované cykly výsledek nemění.
 *
 * @note [ladit] Rychlost AKTIVNÍHO tisku řídí hlavně drive_poll (host poll);
 *       tenhle rozpočet má hlavně dokončit "ocas" (poslední znak). Vyšší =
 *       svižnější dokreslení i mírně rychlejší tisk.
 */
#define MZ1P16_FRAME_CYCLES 40000

/**
 * @brief Rozpočet cyklů pro fast-forward homingu/selftestu při (re)aktivaci.
 *
 * Homing + náběh kreslicího selftestu trvá ~2,15 mil. cyklů. Dáváme rezervu
 * 3 mil., aby plotter spolehlivě doběhl do ustáleného poll stavu (případně
 * rozkreslil začátek selftestu) bez čekání v realtime. Dohání se po snímcích
 * (MZ1P16_EMU_CATCHUP_CHUNK_CYCLES), ne najednou.
 */
#define MZ1P16_EMU_FASTFWD_CYCLES (3 * 1000 * 1000)

/**
 * @brief Rozpočet cyklů pro fast-forward kreslicího self-testu.
 *
 * Homing (~2,15 mil. cyklů) + vykreslení několika řádků znakového generátoru.
 * Self-test kreslí ~1 textový řádek za ~18 mil. cyklů MCU. 60 mil. cyklů dá
 * homing + ~3 řádky charsetu - dost na vizuální potvrzení bez nepřiměřeného
 * čekání (po MZ1P16_EMU_CATCHUP_CHUNK_CYCLES za snímek asi 120 snímků).
 */
#define MZ1P16_EMU_SELFTEST_CYCLES (60 * 1000 * 1000)

/**
 * @brief Nejvýš cyklů jádra 8050 za jeden snímek při dohánění.
 *
 * Dohánění homingu (aktivace, reset) a self-testu se rozloží do snímků:
 * mz1p16_emu_on_screen_done() místo MZ1P16_FRAME_CYCLES odkrokuje až tolik
 * cyklů, dokud dluh neklesne na nulu. Dřív proběhlo najednou a emulace
 * stroje (i zvuk) stála: self-test 60 mil. cyklů asi 0,9 s v debug buildu,
 * homing 3 mil. cyklů přes dva snímky (lupnutí ve zvuku). 500 tisíc cyklů je
 * v debug buildu asi 7 ms z 20 ms snímku [odhad z doby self-testu].
 */
#define MZ1P16_EMU_CATCHUP_CHUNK_CYCLES (500 * 1000)

/**
 * @brief Zbývající cykly dohánění (dluh), pod serializačním zámkem.
 *
 * Nastaví ho aktivace, reset a self-test (mz1p16_emu_schedule_catchup()),
 * splácí mz1p16_emu_on_screen_done(). 0 = plotter běží jen per-frame
 * rozpočtem MZ1P16_FRAME_CYCLES.
 */
static long s_catchup_cycles = 0;

/**
 * @brief Po splacení dluhu uvolnit PAPER FEED (T1=1) - konec self-testu.
 *
 * Self-test drží T1 v nule po celé dohánění; bez uvolnění by firmware kreslil
 * znakovou sadu donekonečna.
 */
static bool s_catchup_release_feed = false;

/**
 * @brief Rozpočet cyklů pro reakci na momentální stisk PEN CHANGE.
 *
 * Po sepnutí T0=0 musí firmware stihnout odjet vozíkem do levé pen-change
 * zóny (mechanický náraz = rotace bubnu). Vozík od běžné pozice doleva ujede
 * max šířku papíru (~530 kroků, řádově stovky tisíc cyklů). 2 mil. cyklů dá
 * firmwaru spolehlivě dost času na dojezd + rotaci, ale proběhne v rámci
 * jednoho UI clicku (fast-forward).
 */
#define MZ1P16_EMU_PENCHANGE_CYCLES (2 * 1000 * 1000)

/**
 * @brief Cykly dokrokované po uvolnění T0 (dokončení sekvence pen-change).
 */
#define MZ1P16_EMU_PENCHANGE_RELEASE_CYCLES (200 * 1000)

/**
 * @brief Naplánuje dohánění jádra 8050 po snímcích (rozložený fast-forward).
 *
 * Nic neodkrokuje: nastaví dluh, který splácí mz1p16_emu_on_screen_done()
 * po nejvýš MZ1P16_EMU_CATCHUP_CHUNK_CYCLES za snímek, takže emulace stroje
 * nestojí. Volá se po resetu a aktivaci (homing) a ze self-testu.
 *
 * @param cycles        Kolik cyklů dohnat (nahradí dosavadní dluh).
 * @param release_feed  true = po splacení uvolnit PAPER FEED (konec
 *                      self-testu); false = zrušit čekající uvolnění.
 * @pre Volající drží serializační zámek (mz1p16_emu_lock()).
 */
static void mz1p16_emu_schedule_catchup ( long cycles, bool release_feed ) {
    s_catchup_cycles = cycles;
    s_catchup_release_feed = release_feed;
}

void mz1p16_emu_drive_poll ( void ) {
    if ( !g_mz1p16.active ) return;

    /* Deterministické krokování řízené událostí (host čtení stavu/BUSY).
     * Odkrokuje pevný počet strojových cyklů 8050 - NEZÁVISLE na čase. Viz
     * MZ1P16_DRIVE_CYCLES_PER_POLL. Volá se pod serializačním zámkem (z pioz80
     * PA read). */
    long remaining = MZ1P16_DRIVE_CYCLES_PER_POLL;
    while ( remaining > 0 ) {
        remaining -= mcs48_step ( &g_mz1p16_cpu );
    };
}

void mz1p16_emu_sync_read_byte ( void ) {
    if ( !g_mz1p16.active ) return;

    /* Synchronní doručení bajtu:
     * Po nastavení dat na P2 a aktivaci STROBE (pioz80, pod zámkem) odkrokuj
     * firmware, dokud bajt SKUTEČNĚ NEPŘIJME = zvedne BUSY (P1.4: 0 -> 1).
     *
     * Proč BUSY hrana a NE pouhé IN A,P2: firmware čte P2 i spekulativně ve své
     * idle/poll smyčce; exit na první čtení P2 by vedl k předčasnému zpracování
     * a rozsypání streamu. Náběžná hrana BUSY je firmwarova vlastní reakce
     * "přijal jsem bajt", nastane na bajt právě jednou. Tím je doručení
     * deterministické bez ohledu na časování vláken:
     *   - ROM po assertu STROBE spolehlivě uvidí BUSY (nepromešká krátký puls),
     *   - bajt se přijme PRÁVĚ JEDNOU (ne ztráta ani duplikace).
     *
     * ROM asserte STROBE jen když je plotter READY (P1.4=0), takže na vstupu
     * čekáme náběžnou hranu. Dlouhý tah (pen move) se dokreslí asynchronně přes
     * drive_poll / on_screen_done; tady čekáme jen na rychlé přijetí bajtu
     * (jednotky-stovky cyklů). Strop MZ1P16_SYNC_READ_MAX_STEPS je pojistka.
     *
     * @pre Volá se s drženým serializačním zámkem (z pioz80 PA write). */
    int budget = MZ1P16_SYNC_READ_MAX_STEPS;
    /* BUSY = P1.4 výstupního latche jádra (1 = busy). */
    while ( ( ( g_mz1p16_cpu.P1 & 0x10u ) == 0u ) && budget-- > 0 ) {
        mcs48_step ( &g_mz1p16_cpu );
    };
}

void mz1p16_emu_clear_paper ( void ) {
    /* Smaže kresbu (papír) - samostatná akce, NEzávislá na resetu plotteru.
     * Bere serializační zámek (volá se z UI vlákna). */
    mz1p16_emu_lock ( );
    mz1p16_clear_drawing ( &g_mz1p16 );
    mz1p16_emu_unlock ( );
}

/**
 * @brief Propagate cb pro [MZ1P16] active - aplikuje hodnotu z INI.
 */
static void mz1p16_cfg_propagate_active ( void *e, void *data ) {
    ( void ) data;
    st_CFGELEMENT *elm = ( st_CFGELEMENT * ) e;
    int v = cfgelement_get_bool_value ( elm );
    mz1p16_emu_set_active ( v ? true : false );
}

/**
 * @brief Save cb pro [MZ1P16] active - zapíše aktuální stav do INI.
 */
static void mz1p16_cfg_save_active ( void *e, void *data ) {
    ( void ) data;
    st_CFGELEMENT *elm = ( st_CFGELEMENT * ) e;
    cfgelement_set_bool_value ( elm, g_mz1p16.active ? 1 : 0 );
}

void mz1p16_emu_lock ( void ) {
    if ( s_mz1p16_lock == NULL ) return;
    APP_MUTEX_LOCK ( s_mz1p16_lock );
}

void mz1p16_emu_unlock ( void ) {
    if ( s_mz1p16_lock == NULL ) return;
    APP_MUTEX_UNLOCK ( s_mz1p16_lock );
}

void mz1p16_emu_init ( void ) {
    if ( s_initialized ) return;
    s_initialized = true;

    /* Serializační mutex musí existovat PŘED jakýmkoliv set_active (volá ho
     * cfgmodule_propagate níže). Vytvoř ho jako úplně první krok. */
    APP_MUTEX_CREATE ( s_mz1p16_lock );

    /* Jádro 8050 nad embeddovaným firmwarem + mechanika. mz1p16_init si
     * memsetne g_mz1p16 a navže hooky portů jádra + klidové úrovně vstupů. */
    mcs48_init ( &g_mz1p16_cpu, mz1p16_rom_data ( ) );
    mz1p16_init ( &g_mz1p16, &g_mz1p16_cpu );

    /* cfgmain [MZ1P16] sekce - klíč:
     *   - active (bool, default 0) plotter ON/OFF při startu */
    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "MZ1P16" );
    if ( cmod ) {
        CFGELM *elm = cfgmodule_register_new_element ( cmod, "active",
                                                       CFGENTYPE_BOOL, 0 );
        cfgelement_set_propagate_cb ( elm, mz1p16_cfg_propagate_active, NULL );
        cfgelement_set_save_cb      ( elm, mz1p16_cfg_save_active,      NULL );
        cfgmodule_parse ( cmod );
        cfgmodule_propagate ( cmod );
    };

    /* CLI --mz1p16 je FLAG (bez hodnoty). Pokud přítomen, vynutí aktivaci
     * nad INI hodnotou. Vzor: printer_init. */
    if ( sdlapp_option_present ( "--mz1p16" ) ) {
        mz1p16_emu_set_active ( true );
    };
}

void mz1p16_emu_reset ( void ) {
    mz1p16_emu_lock ( );

    /* RESET printeru = konec aktuální tiskové úlohy: uzavři i stávající capture
     * soubor (má-li obsah). Capture i plotter sdílí jeden konektor; další bajt
     * pak založí nový soubor. (Při resetu stroje to volá i printer_reset -
     * druhé volání je no-op.) printer_close_file nebere mz1p16 zámek. */
    printer_close_file ( );

    /* Reset jádra (signál RESET 8050) + vynulování kresby. Pozice motorů
     * v g_mz1p16 NEnulujeme přímo - homing fast-forward je dožene; ale po
     * resetu chceme začít s čistou kresbou a od levého dorazu. */
    mcs48_reset ( &g_mz1p16_cpu );

    /* Po mcs48_reset se latche portů nastaví na 0FFh; klidové úrovně vstupů
     * (INT/T0/T1, P*_in) nastaví znovu (mcs48_reset je nemění konzistentně). */
    g_mz1p16_cpu.INT = 1;
    g_mz1p16_cpu.T0  = 1;
    g_mz1p16_cpu.T1  = 1;
    g_mz1p16_cpu.T1_prev = 1;
    g_mz1p16_cpu.P1_in  = 0xFFu;
    g_mz1p16_cpu.P2_in  = 0xFFu;
    g_mz1p16_cpu.BUS_in = 0xFFu;

    /* Mechaniku resetujeme: pozice, pero, barva, kresba. host_data zpět na
     * klid (0FFh = žádný platný bajt). Hooky portů zůstávají navázané. */
    g_mz1p16.sx.phase = 0; g_mz1p16.sx.have_phase = false; g_mz1p16.sx.pos = 0;
    g_mz1p16.sy.phase = 0; g_mz1p16.sy.have_phase = false; g_mz1p16.sy.pos = 0;
    g_mz1p16.pen_down = false;
    g_mz1p16.color = 0;
    g_mz1p16.busy = false;
    g_mz1p16.in_penchange = false;
    g_mz1p16.host_data = 0xFFu;
    /* POZN.: kresbu (papír) zde ZÁMĚRNĚ NEmažeme - RESET plotteru/stroje
     * nesmaže papír (jako reálný HW). Smazání papíru je samostatná akce
     * (mz1p16_emu_clear_paper / tlačítko Clear paper). */

    /* Pokud je plotter aktivní, dožeň homing po snímcích, ať se rychle
     * ustálí (jinak by uživatel viděl ~5 s "mrtvý" plotter po resetu). Reset
     * jádra ruší i rozběhnutý self-test (T1 je zase 1). */
    mz1p16_emu_schedule_catchup ( g_mz1p16.active ? MZ1P16_EMU_FASTFWD_CYCLES : 0, false );
    mz1p16_emu_unlock ( );
}

void mz1p16_emu_shutdown ( void ) {
    s_initialized = false;
    /* Uvolni serializační mutex (symetrie s @ref mz1p16_emu_init). Po tomto
     * bodě jsou lock/unlock opět no-op (NULL-safe). */
    APP_MUTEX_DESTROY ( s_mz1p16_lock );
}

void mz1p16_emu_set_active ( bool active ) {
    mz1p16_emu_lock ( );
    bool was_active = g_mz1p16.active;
    g_mz1p16.active = active;
    DBGPRINTF ( DBGINF, "mz1p16: active = %d\n", ( int ) active );

    /* Vypnutí: dohánění zahodit; rozběhnutý self-test pustí PAPER FEED. */
    if ( !active ) {
        if ( s_catchup_release_feed ) {
            g_mz1p16_cpu.T1 = 1;
            g_mz1p16_cpu.T1_prev = 1;
        };
        mz1p16_emu_schedule_catchup ( 0, false );
    };

    /* Přechod neaktivní -> aktivní: dožeň homing po snímcích, aby plotter
     * naběhl bez realtime čekání a bez zastavení emulace. */
    if ( active && !was_active ) {
        mz1p16_emu_schedule_catchup ( MZ1P16_EMU_FASTFWD_CYCLES, false );

        /* MZ-1P16 je MZ printer: při otevření/připojení plotteru automaticky
         * přepni standard tiskárny (zadní DIP SW2/SW3) na MZ, aby polarita
         * řídicích signálů odpovídala plotteru. Uživatel může v menu změnit
         * zpět. Na deaktivaci standard nevracíme (necháme volbu na uživateli). */
        pioz80_set_printer_std ( PIOZ80_PRINTER_STD_MZ );
    };
    mz1p16_emu_unlock ( );
}

bool mz1p16_emu_get_active ( void ) {
    return g_mz1p16.active;
}

void mz1p16_emu_on_screen_done ( void ) {
    /* Hot-path: neaktivní plotter = okamžitý návrat. */
    if ( !g_mz1p16.active ) return;

    /* Dokresli rozdělanou práci i bez probíhajícího host I/O. Bezpodmínečně
     * odkrokuj rozpočet MZ1P16_FRAME_CYCLES: rozdělaný znak/příkaz doběhne, a
     * když je firmware v klidu, jen protočí poll smyčku (neškodné). NEpodmiňovat
     * BUSY/P1.4 - ten během kreslení toggluje (FC<->EC), smyčka by skončila
     * předčasně. Bere serializační zámek (běží z emu vlákna, race s UI).
     * Při dohánění (aktivace, reset, self-test) odkrokuje víc, nejvýš
     * MZ1P16_EMU_CATCHUP_CHUNK_CYCLES, a sníží o to dluh. */
    mz1p16_emu_lock ( );
    long budget = MZ1P16_FRAME_CYCLES;
    if ( s_catchup_cycles > 0 ) {
        long chunk = ( s_catchup_cycles < MZ1P16_EMU_CATCHUP_CHUNK_CYCLES )
                     ? s_catchup_cycles : MZ1P16_EMU_CATCHUP_CHUNK_CYCLES;
        if ( chunk > budget ) budget = chunk;
    };
    long done = 0;
    while ( done < budget ) {
        done += mcs48_step ( &g_mz1p16_cpu );
    };
    if ( s_catchup_cycles > 0 ) {
        s_catchup_cycles = ( done >= s_catchup_cycles ) ? 0 : s_catchup_cycles - done;
        if ( s_catchup_cycles == 0 && s_catchup_release_feed ) {
            /* Konec self-testu: uvolnit PAPER FEED, firmware dokreslí a přejde
             * do klidu (kresba zůstane). */
            g_mz1p16_cpu.T1 = 1;
            g_mz1p16_cpu.T1_prev = 1;
            s_catchup_release_feed = false;
            DBGPRINTF ( DBGINF, "mz1p16: drawing self-test done (strokes=%u, dropped=%u)\n",
                        ( unsigned ) g_mz1p16.n_strokes, ( unsigned ) g_mz1p16.dropped );
        };
    };
    mz1p16_emu_unlock ( );
}

void mz1p16_emu_button_pen_change ( bool pressed ) {
    mz1p16_emu_lock ( );
    /* T0 aktivní LOW: drženo => 0, uvolněno => 1. */
    g_mz1p16_cpu.T0 = pressed ? 0 : 1;
    mz1p16_emu_unlock ( );
}

void mz1p16_emu_pulse_pen_change ( void ) {
    mz1p16_emu_lock ( );
    if ( !g_mz1p16.active ) {
        mz1p16_emu_unlock ( );
        return;
    };

    uint32_t color_before = g_mz1p16.color;
    uint64_t changes_before = g_mz1p16.color_changes;

    /* 1) Tlačítko sepnuté (T0=0) - firmware dostane fast-forward na reakci
     *    (dojezd vozíku do levé pen-change zóny = rotace bubnu). */
    g_mz1p16_cpu.T0 = 0;
    long remaining = MZ1P16_EMU_PENCHANGE_CYCLES;
    while ( remaining > 0 ) {
        remaining -= mcs48_step ( &g_mz1p16_cpu );
    };

    /* 2) Tlačítko uvolněné (T0=1) - krátký doběh na dokončení sekvence. */
    g_mz1p16_cpu.T0 = 1;
    remaining = MZ1P16_EMU_PENCHANGE_RELEASE_CYCLES;
    while ( remaining > 0 ) {
        remaining -= mcs48_step ( &g_mz1p16_cpu );
    };

    DBGPRINTF ( DBGINF,
        "mz1p16: pen change pulse (color %u->%u, changes %llu->%llu)\n",
        ( unsigned ) color_before, ( unsigned ) g_mz1p16.color,
        ( unsigned long long ) changes_before,
        ( unsigned long long ) g_mz1p16.color_changes );
    mz1p16_emu_unlock ( );
}

void mz1p16_emu_button_paper_feed ( bool pressed ) {
    mz1p16_emu_lock ( );
    /* T1 aktivní LOW: drženo => 0, uvolněno => 1. T1_prev necháváme jádru
     * (hranová detekce uvnitř firmwaru); nastavujeme jen aktuální úroveň. */
    g_mz1p16_cpu.T1 = pressed ? 0 : 1;
    mz1p16_emu_unlock ( );
}

void mz1p16_emu_run_drawing_selftest ( void ) {
    mz1p16_emu_lock ( );
    if ( !g_mz1p16.active ) {
        mz1p16_emu_unlock ( );
        return;
    };

    /* HW postup: PAPER FEED držet (T1=0) a stisknout RESET. Reset jádra +
     * vynulování kresby, T1 držíme v nule po celou dobu náběhu. */
    mcs48_reset ( &g_mz1p16_cpu );
    g_mz1p16_cpu.INT = 1;
    g_mz1p16_cpu.T0  = 1;
    g_mz1p16_cpu.T1  = 0;      /* PAPER FEED držen */
    g_mz1p16_cpu.T1_prev = 0;
    g_mz1p16_cpu.P1_in  = 0xFFu;
    g_mz1p16_cpu.P2_in  = 0xFFu;
    g_mz1p16_cpu.BUS_in = 0xFFu;

    g_mz1p16.sx.phase = 0; g_mz1p16.sx.have_phase = false; g_mz1p16.sx.pos = 0;
    g_mz1p16.sy.phase = 0; g_mz1p16.sy.have_phase = false; g_mz1p16.sy.pos = 0;
    g_mz1p16.pen_down = false;
    g_mz1p16.color = 0;
    g_mz1p16.busy = false;
    g_mz1p16.in_penchange = false;
    g_mz1p16.host_data = 0xFFu;
    mz1p16_clear_drawing ( &g_mz1p16 );

    /* Homing + vykreslení několika řádků znakového generátoru po snímcích
     * (mz1p16_emu_on_screen_done()). Po splacení se uvolní PAPER FEED (T1=1),
     * jinak by průběžné per-frame clockování pokračovalo v self-testu
     * donekonečna (firmware by pořád kreslil charset) -> přetečení bufferu
     * + chaotická rostoucí kresba. Po uvolnění firmware self-test dokončí
     * a přejde do klidu; kresba zůstane. */
    mz1p16_emu_schedule_catchup ( MZ1P16_EMU_SELFTEST_CYCLES, true );
    mz1p16_emu_unlock ( );
}


long mz1p16_emu_catchup_pending ( void ) {
    mz1p16_emu_lock ( );
    long v = s_catchup_cycles;
    mz1p16_emu_unlock ( );
    return v;
}
