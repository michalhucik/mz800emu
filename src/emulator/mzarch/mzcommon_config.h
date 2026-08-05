/**
 * @file mzcommon_config.h
 * @brief Platformně NEZÁVISLÉ konfigurační toggly emulátoru.
 *
 * Vyčleněno z mzarch_config.h (mzhal krok 5): tyto volby jsou společné
 * všem platformám a nesmí záviset na MZARCH. Hlavičky, jejichž obsah
 * (layout struktur, číslování enumů) na těchto togglech závisí
 * (mzevent.h, ctc8253.h), tento soubor includují PŘÍMO - zapomenutý
 * include cesty přes mzarch_config.h tak nemůže tiše změnit layout.
 *
 * V mzarch_config.h zůstává: dispatch na per-arch mz*_config.h +
 * fallbacky capability maker (CFG_HWEXT_*, HAVE_*).
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

#ifndef MZCOMMON_CONFIG_H
#define MZCOMMON_CONFIG_H

/*
 * Konfiguracni vypnuti modulu MZ-800 debugger
 * ===========================================
 *
 * Debugger lze vypnout dvema zpusoby:
 *
 *  1. Build-time prepinac z prikazove radky:
 *     make NO_DEBUGGER=1   (Makefile to predava CMake jako
 *                           -DMZ_NO_DEBUGGER=ON, ktery definuje
 *                           MZ800EMU_NO_DEBUGGER pres globalni
 *                           add_compile_definitions)
 *
 *  2. Lokalni edit tohoto souboru - zakomentovat radek `#define
 *     MZ800EMU_CFG_DEBUGGER_ENABLED` nize.
 *
 * Bez debuggeru je binarka cca o 15 % mensi a emulace by mela
 * byt rychlejsi (presne hodnoty neoverene).
 */

#ifndef MZ800EMU_NO_DEBUGGER
#define MZ800EMU_CFG_DEBUGGER_ENABLED
#endif

/*
 * Konfigurační vypnutí MCP serveru
 * ================================
 *
 * MCP (Model Context Protocol) server zpřístupňuje debugger emulátoru
 * externím AI agentům (Claude Code, FastMCP klienti). Backend je
 * rozdělen do dvou nezávislých togglů:
 *
 *  - MZ800EMU_CFG_MCP_SERVER_ENABLED řídí MCP backend jako celek
 *    (pipe transport, JSONL parser, frontu příkazů). Sleduje
 *    MZ800EMU_CFG_DEBUGGER_ENABLED - bez debuggeru nemá MCP smysl.
 *
 *  - MZ800EMU_CFG_MCP_TCP_ENABLED řídí volitelný TCP listener
 *    (loopback 127.0.0.1). Vždy vyžaduje MCP_SERVER_ENABLED.
 *    Lze ho vypnout zvlášť pro "pipe-only" build.
 *
 * Build-time přepínače z příkazové řádky:
 *   make NO_MCP=1       -> vypne celý MCP (+ implicitně i TCP)
 *   make NO_MCP_TCP=1   -> vypne jen TCP listener (pipe zůstane)
 *
 * Lokální edit této sekce: zakomentuj `#define` řádky níže.
 *
 * Pozn.: Pattern kopíruje MZ800EMU_NO_DEBUGGER (negativní toggle,
 * pozitivní _ENABLED define bez hodnoty, `#ifdef` v hostujícím
 * kódu). NIKDY nepoužívej `#if MZ800EMU_CFG_MCP_..._ENABLED` -
 * bezhodnotový define se zde vyhodnotí jako 0.
 */

#ifndef MZ800EMU_NO_MCP
#define MZ800EMU_CFG_MCP_SERVER_ENABLED
#endif

#ifndef MZ800EMU_NO_MCP_TCP
#define MZ800EMU_CFG_MCP_TCP_ENABLED
#endif

/*
 * Validační guardy zachytí nesmyslné kombinace při compile-time.
 * Bez DEBUGGER nemá MCP smysl; TCP listener vyžaduje funkční MCP
 * backend (sdílí JSONL parser, frontu, transport vrstvu).
 */
#if defined(MZ800EMU_CFG_MCP_SERVER_ENABLED) && !defined(MZ800EMU_CFG_DEBUGGER_ENABLED)
#error "MZ800EMU_CFG_MCP_SERVER_ENABLED requires MZ800EMU_CFG_DEBUGGER_ENABLED (use NO_MCP=1 when NO_DEBUGGER=1)"
#endif

#if defined(MZ800EMU_CFG_MCP_TCP_ENABLED) && !defined(MZ800EMU_CFG_MCP_SERVER_ENABLED)
#error "MZ800EMU_CFG_MCP_TCP_ENABLED requires MZ800EMU_CFG_MCP_SERVER_ENABLED (use NO_MCP_TCP=1 when NO_MCP=1)"
#endif

/*
 * Emulace CLK 1.1 MHz (CTC8253-CTC0 a CMT)
 * ========================================
 *
 * CLK1M1 (CTC0/CMT) se emuluje výhradně event-driven - step se volá
 * jen když má nastat konkrétní event. Žádný build toggle neexistuje.
 */

/*
 * Vypnuti audio modulu
 * ====================
 *
 * Pokud neni definovano pouziti SDL nebo Gstreamer Audio, tak se vypne audio modul.
 * Sznchronizace 20ms bude nahrazena synchronizaci podle get_ticks_ns().
 *
 */

// #define MZ800EMU_CFG_AUDIO_DISABLED

// Vypnuti audio modulu, pokud neni definovano pouziti SDL nebo Gstreamer Audio
#ifndef MZ800EMU_CFG_AUDIO_DISABLED
#if !defined(USE_SDL_AUDIO) && !defined(USE_SDL2_AUDIO) && !defined(USE_SDL3_AUDIO) && !defined(USE_GSTREAMER_AUDIO)
#define MZ800EMU_CFG_AUDIO_DISABLED
#endif /* !USE_SDL_AUDIO && !USE_GSTREAM_AUDIO */
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */

/*
 * Memory statistics
 * =================
 *
 * Chci zkusit zjistit, jak nejlepe optimalizovat umisteni podminek pro cteni a zapis do pameti.
 * Tato volba vytvori aditivni soubor se statistikou pro cteni a zapis pameti.
 *
 */
// #define MEMORY_MAKE_STATISTICS

#endif /* MZCOMMON_CONFIG_H */
