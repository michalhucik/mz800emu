/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file mz1p16_emu.h
 * @brief Integrační vrstva plotteru MZ-1P16 do emulátoru - veřejné API.
 *
 * Modul drží globální instanci mechaniky plotteru (@ref g_mz1p16) spolu s
 * jejím jádrem 8050 (@ref g_mz1p16_cpu) a embeddovaným firmwarem a napojuje ji
 * do životního cyklu emulátoru (init/reset/shutdown), na konfiguraci (INI
 * sekce [MZ1P16], CLI --mz1p16) a na krokování MCU v per-frame smyčce.
 *
 * Plotter se připojuje na Z80 PIO Centronics rozhraní (porty 0FCh-0FFh) -
 * proto se inicializuje jen na architekturách se Z80 PIO (MZ-800, MZ-1500),
 * stejně jako modul printer. MZ-700 Z80 PIO nemá.
 *
 * Vzor integrace = printer (printer.{c,h}).
 *
 * Handshake (data PB->P2, STROBE PA7->/INT, BUSY plotter->PA0) je napojen
 * v pioz80.c; tento modul poskytuje stavovou instanci a krokování jádra.
 */

#ifndef MZ1P16_EMU_H
#define MZ1P16_EMU_H

#include <stdbool.h>

#include "emulator/hw-generic/mz1p16/mz1p16.h"
#include "emulator/hw-generic/mz1p16/mcs48.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Globální instance mechaniky plotteru (mirror pro UI a handshake). */
extern st_MZ1P16 g_mz1p16;

/** Globální instance jádra 8050 plotteru. */
extern st_MCS48 g_mz1p16_cpu;

/**
 * @brief Uzamkne serializační mutex plotteru.
 *
 * Veškerý přístup k jádru/stavu plotteru (g_mz1p16_cpu, g_mz1p16 vč. strokes)
 * z více vláken (emu vlákno: per-frame doběh a pioz80 handshake hooky; UI
 * vlákno: render stroke bufferu) musí probíhat pod tímto zámkem, jinak vzniká
 * datový závod. Vnitřní funkce modulu (drive_poll a privátní helpery)
 * zámek NEberou - předpokládají, že ho drží volající.
 *
 * @warning Zámek se NESMÍ vnořovat. Držitel zámku nesmí volat jinou funkci,
 *          která zámek bere (UI akce mz1p16_emu_set_active / _reset /
 *          _run_drawing_selftest / _pulse_pen_change / _button_* berou zámek
 *          uvnitř - volej je BEZ vnějšího zámku).
 * @note NULL-safe: před @ref mz1p16_emu_init (mutex ještě neexistuje) je no-op.
 */
void mz1p16_emu_lock ( void );

/**
 * @brief Odemkne serializační mutex plotteru.
 *
 * Pár k @ref mz1p16_emu_lock. NULL-safe (před inicializací no-op).
 */
void mz1p16_emu_unlock ( void );

/**
 * @brief Jednorázová inicializace integrační vrstvy plotteru.
 *
 * Inicializuje jádro 8050 nad embeddovaným firmwarem (mz1p16_rom_data),
 * navže mechaniku (mz1p16_init), zaregistruje INI sekci [MZ1P16] (klíč
 * active:bool, default 0) v g_cfgmain a aplikuje případný CLI flag --mz1p16.
 * Volá se z init sekvence architektur se Z80 PIO (mz800, mz1500), po
 * @ref printer_init. Idempotentní (opakované volání je no-op).
 *
 * @post g_mz1p16 i g_mz1p16_cpu jsou v resetovaném stavu; plotter neaktivní,
 *       pokud INI/CLI neřekne jinak.
 */
void mz1p16_emu_init ( void );

/**
 * @brief Reakce na reset stroje.
 *
 * Resetuje jádro 8050 (mcs48_reset) i mechaniku (pozice, kresbu) a znovu
 * navže klidové úrovně vstupů. Pokud je plotter aktivní, naplánuje dohánění
 * homingu po snímcích (viz @ref mz1p16_emu_on_screen_done), aby se plotter
 * rychle ustálil bez zastavení emulace; rozběhnutý self-test zruší. Volá se
 * z mzarch.c (vedle printer_reset).
 */
void mz1p16_emu_reset ( void );

/**
 * @brief Ukončení modulu.
 *
 * Aktuálně bez alokovaných zdrojů - jen shodí idempotence flag. Doplněno
 * kvůli symetrii s printer_shutdown.
 */
void mz1p16_emu_shutdown ( void );

/**
 * @brief Zapne/vypne plotter.
 *
 * Při zapnutí se začne krokovat jádro 8050 (per-frame) a plotter reaguje na
 * Centronics handshake. Při (re)aktivaci se naplánuje dohánění homingu po
 * snímcích (@ref mz1p16_emu_on_screen_done), aby uživatel nečekal na realtime
 * homing (~5 s) a emulace přitom nestála. Při vypnutí se plotter chová jako
 * odpojený (žádné krokování, BUSY se nehlásí), dohánění se zahodí a
 * rozběhnutý self-test pustí PAPER FEED.
 *
 * @param active true = zapnout, false = vypnout
 */
void mz1p16_emu_set_active ( bool active );

/**
 * @brief Vrátí, zda je plotter aktivní.
 * @return true pokud aktivní
 */
bool mz1p16_emu_get_active ( void );

/**
 * @brief Per-frame doběh rozdělané práce jádra 8050.
 *
 * Volá se z per-frame eventu emulátoru (mz800_main_event_callback_screen_done,
 * ~20 ms). Pokud plotter není aktivní, okamžitě se vrací (1 atomic read +
 * branch = ~zero impact na hot path). Když je aktivní, odkrokuje pevný rozpočet
 * (MZ1P16_FRAME_CYCLES), aby rozdělaná kresba (poslední znak) doběhla i tehdy,
 * když host po posledním bajtu přestane pollovat BUSY; v klidu firmware jen
 * protočí poll smyčku.
 *
 * Dohánění: po (re)aktivaci, resetu a spuštění self-testu odkrokuje místo
 * MZ1P16_FRAME_CYCLES až MZ1P16_EMU_CATCHUP_CHUNK_CYCLES cyklů za snímek,
 * dokud nedožene naplánovaný počet (@ref mz1p16_emu_catchup_pending); po
 * dohnání self-testu uvolní PAPER FEED (T1=1). Emulace stroje tak nestojí.
 */
void mz1p16_emu_on_screen_done ( void );

/**
 * @brief Zbývající cykly jádra 8050 k dohnání po snímcích.
 *
 * @return 0 = nic se nedohání; jinak počet cyklů, které ještě odkrokuje
 *         @ref mz1p16_emu_on_screen_done (po nejvýš
 *         MZ1P16_EMU_CATCHUP_CHUNK_CYCLES za snímek).
 * @note Bere serializační zámek; libovolné vlákno (testy, okno plotteru).
 */
long mz1p16_emu_catchup_pending ( void );

/**
 * @brief Deterministicky posune jádro 8050 o pevný počet cyklů (event-driven).
 *
 * Volá se z pioz80 při host čtení stavu plotteru (BUSY/PA0 v poll smyčce ROM).
 * Posune firmware o MZ1P16_DRIVE_CYCLES_PER_POLL strojových cyklů - NEZÁVISLE
 * na uplynulém čase. Tím je kresba čistě funkcí sekvence bus událostí a
 * nezávisí na časování hostitelského vlákna. No-op, pokud plotter není aktivní.
 *
 * @pre Volá se s drženým serializačním zámkem (mz1p16_emu_lock).
 */
void mz1p16_emu_drive_poll ( void );

/**
 * @brief Synchronně doručí právě zapsaný bajt do firmwaru.
 *
 * Po nastavení dat na P2 a aktivaci STROBE (pioz80 PA write) odkrokuje firmware,
 * dokud bajt z P2 nepřečte. Zaručuje, že host nepřepíše data dřív, než si je
 * firmware stihne přečíst (jinak by se bajt ztratil nebo zpracoval špatně).
 *
 * No-op, pokud plotter není aktivní. Počet kroků shora omezen
 * (MZ1P16_SYNC_READ_MAX_STEPS).
 *
 * @pre Volá se s drženým serializačním zámkem (mz1p16_emu_lock).
 */
void mz1p16_emu_sync_read_byte ( void );

/**
 * @brief Smaže kresbu (papír). Samostatná akce nezávislá na resetu plotteru.
 *
 * RESET plotteru/stroje papír NEmaže (jako reálný HW); tahle funkce je jediná
 * cesta, jak kresbu vymazat (tlačítko Clear paper). Bere serializační zámek.
 */
void mz1p16_emu_clear_paper ( void );

/**
 * @brief Nastaví/uvolní tlačítko PEN CHANGE (vstup T0 jádra, aktivní LOW).
 *
 * Tlačítka plotteru jsou na vstupech T0/T1 jádra 8050 a jsou aktivní v
 * nule (sepnuté = drženo = 0). Tato funkce jen přepíše úroveň T0; vlastní
 * reakci firmware vyhodnotí při dalším krokování (per-frame).
 *
 * @param pressed true = drženo (T0=0), false = uvolněno (T0=1)
 */
void mz1p16_emu_button_pen_change ( bool pressed );

/**
 * @brief Provede momentální stisk tlačítka PEN CHANGE (puls T0 + doběh).
 *
 * Reálné tlačítko PEN CHANGE generuje krátký stisk: T0 sepne do nuly,
 * firmware hranu/úroveň vyhodnotí při krokování a odjede vozíkem do levé
 * pen-change zóny (kde mechanický náraz otočí buben na další barvu), pak
 * tlačítko povolí. Pouhé jednorázové nastavení T0=0 v UI nestačí - firmware
 * mezi dvěma per-frame doběhy nedostane dost cyklů na reakci a změna barvy
 * se neprovede.
 *
 * Tato funkce proto:
 *  1) sepne T0=0 a fast-forwardem odkrokuje jádro (firmware zareaguje),
 *  2) povolí T0=1 a krátce dokrokuje (firmware dokončí sekvenci).
 *
 * No-op, pokud plotter není aktivní.
 *
 * @post Pokud firmware na PEN CHANGE reaguje, @ref g_mz1p16 color/color_changes
 *       se aktualizuje. Úroveň T0 zůstává po návratu v klidu (1).
 */
void mz1p16_emu_pulse_pen_change ( void );

/**
 * @brief Nastaví/uvolní tlačítko PAPER FEED (vstup T1 jádra, aktivní LOW).
 *
 * PAPER FEED na T1; aktivní LOW (drženo = 0). Podržení PAPER FEED přes reset
 * navíc spouští kreslicí self-test (viz @ref mz1p16_emu_run_drawing_selftest).
 *
 * @param pressed true = drženo (T1=0), false = uvolněno (T1=1)
 */
void mz1p16_emu_button_paper_feed ( bool pressed );

/**
 * @brief Spustí vestavěný kreslicí self-test (znakový generátor 4 barvy).
 *
 * HW postup: podržet PAPER FEED a stisknout RESET. Model: resetuje jádro s
 * T1 drženým v nule (PAPER FEED), vynuluje kresbu a naplánuje dohánění
 * homingu a začátku kreslení znakového generátoru po snímcích
 * (@ref mz1p16_emu_on_screen_done, asi 120 snímků), aby uživatel nečekal
 * realtime a emulace přitom nestála. Po dohnání se PAPER FEED uvolní (T1=1,
 * jinak by firmware kreslil znakovou sadu donekonečna). Pokud plotter není
 * aktivní, je no-op.
 */
void mz1p16_emu_run_drawing_selftest ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ1P16_EMU_H */
