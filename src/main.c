#include "main.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <glib.h>
#include "i18n.h"
#ifdef _WIN32
#include <windows.h>
#endif

#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/dbgapi_dispatcher.h"
#include "ui-imgui/spdfd/spdfd_main.h"
#include "iface/iface.h"
#include "emulator/cfgmain.h"
#include "emulator/debugger/dbgapi_emu.h"
#include "emulator/i18n_lang.h"
#include "emulator.h"
#include "emulator/customspeed.h"
#include "emulator/mzarch/mzarch.h"
#include "emulator/hw-generic/cmt/cmt_cli.h"
#include "emulator/hw-generic/fdc/fdc_cli.h"
#include "emulator/automation.h"
#include "emulator/replay_cli.h"
#include "version_info.h"

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include "emulator/mcp/main_pipe.h"
#include "emulator/mcp/dispatch_runner.h"
#endif

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
#include "emulator/mcp/tcp_server.h"
#include "emulator/mcp/mcp_config.h"
#endif

SdlApp *g_sdlapp = NULL;

/**
 * @brief Nejdelší čekání hlavní smyčky na událost SDL v režimu --headless [ms].
 *
 * Headless běží smyčka v režimu SDLAPP_LOOP_WAIT_TIMEOUT místo aktivního
 * pollingu. Události (časovače sdlapp, ukončení) čekání probudí hned; hodnota
 * určuje jen nejdelší prodlevu, než si smyčka všimne sdlapp_quit() z jiného
 * vlákna (ten událost nevloží).
 */
#define MAIN_HEADLESS_LOOP_WAIT_MS 20


/**
 * @brief Whitelist všech známých CLI options.
 *
 * Používá se pro:
 *  - validaci v main() (neznámé options = error a exit)
 *  - generování --help výstupu
 *
 * Vyhodnocování hodnot dělají jednotlivé moduly (mzarch_main, debugger,
 * cfgmain) pomocí @c sdlapp_option_value / @c sdlapp_option_present.
 * Tabulka tedy slouží jen jako registr "co je validní".
 */
/* Sdílené allowed-values seznamy pro ENUM options. NULL-terminated. */
static const char *const MODE_VALUES[] = { "off", "window", "always", NULL };

/* Režim ringu záznamu zápisů pro --ramlog-ring. */
static const char *const RAMLOG_RING_VALUES[] = { "ring", "fill", NULL };

/* Úroveň pokrytí kódu pro --coverage. */
static const char *const COVERAGE_VALUES[] = { "off", "branch", "detail", NULL };

/* Klíč verze kódu pro --coverage-key. */
static const char *const COVERAGE_KEY_VALUES[] = { "opcode", "full_bytes", NULL };

/* FDC voličí pro --fdc-bus-xlate. */
static const char *const FDC_BUS_XLATE_VALUES[] = { "invert", "passthrough", NULL };

/* MCP volby pro --mcp-bind (V0.B.4 - dvě dovolené hodnoty). */
static const char *const MCP_BIND_VALUES[] = { "127.0.0.1", "0.0.0.0", NULL };

/* MCP volby pro --mcp-profile (V0.B.4 - 4 dovolené hodnoty;
 * v V0 ukládáme jen vybranou hodnotu, plné vynucení přijde v V1.A). */
static const char *const MCP_PROFILE_VALUES[] = {
    "wild", "confined", "sandboxed", "observer", NULL
};

/* Sloupce: name, kind, value_type, allowed_values, value_placeholder, description.
 * Pro FLAG: value_type=NONE, allowed_values=NULL, value_placeholder=NULL.
 * Pro VALUE: value_type podle očekávaného obsahu (STRING/ENUM/UINT/BOOL_ON_OFF).
 * ENUM vyžaduje neprázdný allowed_values seznam (jinak se validace přeskočí). */
static const st_SDLAPP_OPTION_DEF g_known_options[] = {
    { "--help",             SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Print this option list and exit." },
    { "--version",          SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Print version, revision, source origin and build details, then exit." },
#ifdef _WIN32
    { "--console",          SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Windows: allocate a console window for stdout/stderr." },
#endif
    { "--run-mzf",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Automatically load and run the given MZF file after the emulator boots." },
    { "--cmt",              SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Insert a tape (.mzf, .m12, .mzt, .tap, .wav, .wave) into the virtual "
      "cassette recorder at startup and open the Virtual CMT window. If the "
      "CMT hack is on, it is turned off for this session only (the INI value "
      "is kept). Missing file or unknown extension = error and exit." },
    { "--cmt-play",         SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "With --cmt: press PLAY after inserting the tape. A normal ROM boot "
      "stops the cassette motor, so the tape then waits in PLAY + PAUSE; it "
      "plays at once only together with --run-mzf." },
    { "--cmt-play-pause",   SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "With --cmt: press PLAY and PAUSE after inserting the tape; the tape "
      "starts moving when the ROM turns the cassette motor on (e.g. after "
      "LOAD). Cannot be combined with --cmt-play." },
    { "--cmt-boot",         SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "With --cmt: like --cmt-play-pause, and hold the C key from reset until "
      "the ROM turns the cassette motor on, so the MZ-800 boot menu loads the "
      "program from the tape. Only available on MZ-800. Cannot be combined "
      "with --run-mzf or --cmt-play." },
    { "--record",           SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<file.avi>",
      "Start video recording (lossless AVI + .cuts.json sidecar) right after boot." },
    { "--record-frames",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<count>",
      "Stop video recording after <count> frames; in --headless mode then quit the emulator." },
    { "--audio-capture",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dir>",
      "Start an audio capture into <dir> right after boot: channel level changes (events.csv), "
      "PSG writes (psg_writes.csv), the exact sound (reference.wav) and the sound of the audio "
      "output (sdl.wav); which files are written is set in Tools -> Audio Capture -> Settings." },
    { "--audio-capture-frames", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,    NULL,        "<count>",
      "Stop the audio capture after <count> audio frames; in --headless mode then quit the emulator." },
    { "--cdl-mode",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Set the CDL (Memory Heatmap) recording mode." },
    { "--cdl-dir",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Set the target directory for CDL export. Created at export time if missing." },
    { "--cdl-name",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<basename>",
      "Set the CDL export basename (without .json). Region files are saved as "
      "<basename>_<region>.cdl, meta as <basename>.json." },
    { "--cdl-save-on-exit", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Enable/disable automatic CDL export when the emulator exits." },
    /* trace-suite cputrack options. */
    { "--cputrack-mode",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Set the CPU tracking log recording mode (trace-suite cputrack)." },
    { "--cputrack-dir",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Set the target directory for cputrack recording. Created at start time if missing." },
    { "--cputrack-name",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<basename>",
      "Set the cputrack basename (without extension). Files: <basename>.json, "
      "<basename>.NNN.bin, <basename>_initial_*.bin." },
    { "--cputrack-chunk-mb",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Per-chunk RAM buffer size in MB before sync flush to disk (default 64)." },
    { "--cputrack-max-total-mb", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Hard limit on total recording size in MB (0 = unlimited, default 0)." },
    { "--cputrack-save-on-exit", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Flush cputrack buffer when the emulator exits (default on)." },
    { "--cputrack-regs",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Record CPU registers after each instruction (format 3, 32 bytes per record "
      "instead of 12; default off)." },
    { "--cputrack-pc-lo",        SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<addr>",
      "Lower PC bound (inclusive) for range-scoped tracking. Dec or 0x hex. "
      "Default 0. Use with --cputrack-pc-hi to record only PC in [lo,hi]." },
    { "--cputrack-pc-hi",        SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<addr>",
      "Upper PC bound (inclusive) for range-scoped tracking. Dec or 0x hex. "
      "Default 0xFFFF (= whole address space, no filter)." },
    /* Záznam zápisů do paměti (ramlog). */
    { "--ramlog-mode",           SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Record writes into memory (RAM write log): off, while the debugger window is open, or from start. "
      "Read the log over MCP (ramlog_who_wrote, trace_read channel ramlog) or in the RAM Write Log window." },
    { "--ramlog-ring",           SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        RAMLOG_RING_VALUES, "<ring|fill>",
      "RAM write log when full: ring keeps the newest records, fill keeps the first ones (default ring)." },
    { "--ramlog-capacity",       SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "RAM write log size in records, 32 bytes each (16-16777216, default 1048576)." },
    { "--ramlog-lo",             SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<addr>",
      "First CPU address recorded by the RAM write log. Dec or 0x hex. Default 0." },
    { "--ramlog-hi",             SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<addr>",
      "Last CPU address recorded by the RAM write log. Dec or 0x hex. Default 0xFFFF "
      "(with --ramlog-lo 0 = all memory, no filter)." },
    /* Pokrytí kódu (coverage). */
    { "--coverage",              SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        COVERAGE_VALUES, "<off|branch|detail>",
      "Collect code coverage from start: branch = executed addresses and branch results by PC, "
      "detail = also code versions by physical cell with counters (slower). Read it over MCP "
      "(coverage_read) or save it with --coverage-save-on-exit." },
    { "--coverage-key",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        COVERAGE_KEY_VALUES, "<opcode|full_bytes>",
      "Code version key of the detail level: opcode bytes (default) or the first 4 bytes including operands." },
    { "--coverage-capacity",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Detail level size in units, 32 bytes each (1-1048576, default 65536)." },
    { "--coverage-save-on-exit", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<file>",
      "Write all coverage rows into <file> when the emulator exits (.csv = CSV, otherwise JSON)." },
    /* trace-suite iorqlog options. */
    { "--iorqlog-mode",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Set the IORQ log recording mode (trace-suite iorqlog)." },
    { "--iorqlog-dir",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Set the target directory for iorqlog recording." },
    { "--iorqlog-name",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<basename>",
      "Set the iorqlog basename." },
    { "--iorqlog-chunk-mb",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Per-chunk RAM buffer size in MB (default 64)." },
    { "--iorqlog-max-total-mb", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Hard limit on total iorqlog size in MB (0 = unlimited)." },
    { "--iorqlog-save-on-exit", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Flush iorqlog buffer at exit (default on)." },
    /* trace-suite intlog options. */
    { "--intlog-mode",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Set the interrupt log recording mode (trace-suite intlog)." },
    { "--intlog-dir",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Set the target directory for intlog recording." },
    { "--intlog-name",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<basename>",
      "Set the intlog basename." },
    { "--intlog-chunk-mb",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Per-chunk RAM buffer size in MB (default 64)." },
    { "--intlog-max-total-mb", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Hard limit on total intlog size in MB (0 = unlimited)." },
    { "--intlog-save-on-exit", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Flush intlog buffer at exit (default on)." },
    /* trace-suite hwlog options. */
    { "--hwlog-mode",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Set the hardware log recording mode (trace-suite hwlog)." },
    { "--hwlog-dir",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Set the target directory for hwlog recording." },
    { "--hwlog-name",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<basename>",
      "Set the hwlog basename." },
    { "--hwlog-chunk-mb",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Per-chunk RAM buffer size in MB (default 64)." },
    { "--hwlog-max-total-mb", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Hard limit on total hwlog size in MB (0 = unlimited)." },
    { "--hwlog-save-on-exit", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Flush hwlog buffer at exit (default on)." },
    { "--hwlog-hs-decimation", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Decimate hwlog GDG_VIDEO HS/HBLN edges: emit every N-th edge "
      "(0 = OFF, default; ~31000 events/sec at full rate)." },
    /* trace-suite marklog options. Bez těchto definic by sdlapp_options_validate
     * odmítl jakýkoliv --marklog-* (marklog_apply_cli_options je četl, ale validace
     * je zhodila ještě před aplikací -> EXIT_FAILURE). */
    { "--marklog-mode",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Set the marker log recording mode (trace-suite marklog)." },
    { "--marklog-dir",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Set the target directory for marklog recording." },
    { "--marklog-name",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<basename>",
      "Set the marklog basename." },
    { "--marklog-chunk-mb",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "RAM buffer size in MB before a marklog chunk is flushed (0 = default)." },
    { "--marklog-max-total-mb", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Hard limit on total marklog size in MB (0 = unlimited)." },
    { "--marklog-save-on-exit", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Flush marklog buffer at exit (default on)." },
    { "--marklog-stdout",       SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Echo each emitted marker to stdout (default off)." },
    /* trace-suite shorthand options - apply to all 4 subsystems (cputrack,
     * iorqlog, intlog, hwlog). Per-subsystem options take precedence; these
     * shorthand values fill in only where no per-subsystem option was given. */
    { "--all-traces-mode",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Set the recording mode for ALL trace-suite subsystems "
      "(cputrack, iorqlog, intlog, hwlog). Per-subsystem --<sys>-mode "
      "takes precedence." },
    { "--eventlog-replay",   SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Replay an Event Viewer ring dump (.evlog) at startup: import "
      "the file into the in-memory ring so the Events window opens with "
      "pre-loaded post-mortem events." },
    { "--all-traces-dir",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Set the target directory for ALL trace-suite subsystems. "
      "Per-subsystem --<sys>-dir takes precedence." },
    /* Callstack subsystem (debugger shadow stack). */
    { "--callstack",          SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Activate the Callstack subsystem on startup (shadow stack of CALL/RST/IRQ/NMI). "
      "Overrides the [CALLSTACK] active value from the INI." },
    /* Memory Browser window auto-open. */
    { "--memory-browser",     SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Open the Memory Browser window automatically at startup "
      "(equivalent to pressing Alt+E once the emulator GUI is up)." },
    /* Profiler subsystem (per-function CPU profiler, built on Callstack listener). */
    { "--profiler",           SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Activate the CPU Profiler subsystem on startup (per-function profilation on top of Callstack). "
      "Implies --callstack (Profiler will force Callstack ON if needed). "
      "Overrides the [PROFILER] active value from the INI." },
    /* Virtual printer capture (capture bytes sent to the printer into a file). */
    { "--printer",            SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Enable the virtual printer capture at startup. When active, the printer "
      "reports ready (BUSY=0) on the Z80 PIO and bytes strobed to it are captured "
      "into a per-session file named printer-<timestamp>.bin in the working "
      "directory. Overrides the [PRINTER] capture value from the INI." },
    /* MZ-1P16 plotter (virtual color X-Y pen plotter on the Z80 PIO Centronics). */
    { "--mz1p16",             SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Enable the virtual MZ-1P16 plotter at startup. When active, the plotter's "
      "internal 8050 microcontroller is stepped and connected to the Z80 PIO "
      "Centronics interface (data, STROBE, BUSY). Overrides the [MZ1P16] active "
      "value from the INI. Only available on architectures with a Z80 PIO "
      "(MZ-800, MZ-1500)." },
    { "--mode-switch",      SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<700|800>",
      "Set the MZ-800 rear mode switch (SW1): 700 = MZ-700 mode, 800 = MZ-800 "
      "mode. The ROM reads it when starting a program. Overrides the [MZ800] "
      "mode_switch value from the INI. Invalid value = error and exit. Only "
      "available on MZ-800." },
    { "--no-save-ini",      SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Do not write the .ini file at exit (CLI overrides become session-only)." },
    { "--no-first-run-windows", SDLAPP_OPTION_FLAG, SDLAPP_OPTVAL_NONE,     NULL,        NULL,
      "Suppress About + Version Check Setup windows shown automatically on first run "
      "(when no .ini file exists). Useful for headless / scripted launches." },
    { "--headless",         SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Run the emulator without GUI window and audio output. SDL3 video/audio "
      "subsystems run in no-op mode (no SDL window, no audio device opened). "
      "Framebuffer is still rendered into memory (usable later via MCP frame "
      "Resources). Intended for CI / batch / subprocess scenarios where no "
      "display or audio device is available. Implies --no-first-run-windows. "
      "The process keeps running until SIGINT (Ctrl+C), SDL quit event or an "
      "exit condition (--exit-after-frames, --timeout-*, --exit-on-*, breakpoint "
      "action 'exit')." },
    { "--exit-after-frames", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,     NULL,        "<N>",
      "Unattended runs: end the run after N emulated frames (exit code 0). "
      "Frames are counted from the start of the run (after --run-mzf)." },
    { "--timeout-frames",   SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<N>",
      "Unattended runs: end the run with exit code 2 (timeout) after N emulated "
      "frames, unless another exit condition came first." },
    { "--timeout-seconds",  SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<S>",
      "Same as --timeout-frames, in emulated seconds (rounded up to whole frames)." },
    { "--exit-on-pc",       SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<addr>[,skip=N][,code=C]",
      "Unattended runs: end the run when the CPU executes <addr> (after skipping "
      "the first N hits), exit code C (default 0). Can be repeated." },
    { "--fail-on-pc",       SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<addr>[,skip=N]",
      "Unattended runs: end the run with exit code 1 (test failed) when the CPU "
      "executes <addr>. Can be repeated." },
    { "--exit-on-write",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<addr>[=VAL][,skip=N][,code=C]",
      "Unattended runs: end the run when the program writes to memory address "
      "<addr> (only value VAL if given). The exit code is the written value "
      "unless code=C is given. Can be repeated." },
    { "--exit-on-out",      SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<port>[,skip=N][,code=C]",
      "Unattended runs: end the run when the program writes to I/O port <port> "
      "(0-255). The exit code is the written value unless code=C is given. "
      "Can be repeated." },
    { "--bpt",              SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Unattended run: load breakpoints from the given .bpt file at start. In "
      "unattended runs the auto-loaded set is not used and nothing is saved at exit." },
    { "--result-json",      SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Unattended runs: write the result of the run (reason, exit code, time, "
      "registers, hashes, artifacts) as JSON to <filepath> at the end." },
    { "--automation-dir",   SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Unattended runs: write result.json and screen.png (final screen) into "
      "<dirpath> (created if missing)." },
    { "--exit-screenshot",  SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Unattended runs: save a PNG screenshot of the final screen at the end." },
    { "--exit-snapshot",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Unattended runs: save a snapshot (.mzs) of the final state at the end." },
    { "--replay-record",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Record the inputs of this run into a replay file (.mzr): start snapshot, "
      "keyboard / joystick changes at frame boundaries, resets, debugger writes "
      "and periodic state hashes. Recording starts at the first frame and the "
      "file is written when the emulator exits." },
    { "--replay-hash-interval", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,  NULL,        "<N>",
      "With --replay-record: frames between state hashes (1-100000, default 50)." },
    { "--replay",           SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Play a replay file back from its start snapshot. Live input is ignored; "
      "the playback pauses at the end or at the first frame whose state hash "
      "differs from the recording." },
    { "--exit-at-replay-end", SDLAPP_OPTION_FLAG, SDLAPP_OPTVAL_NONE,       NULL,        NULL,
      "With --replay: end the run when the playback ends - exit code 0 "
      "(reason replay_end), 4 when the state differs (replay_desync), 3 when "
      "the replay cannot be started. Works with --result-json." },
    { "--replay-verify",    SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "With --replay: check the replay at MAX SPEED and exit at its end "
      "(--exit-at-replay-end). Typically with --headless." },
    { "--maxspeed-bench",   SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Start in MAX SPEED and periodically print the MAX SPEED benchmark report "
      "(efficiency %, throughput, FB-FPS, distribution) to the console. Works in "
      "both full and NO_DEBUGGER builds. Intended for headless A/B efficiency "
      "measurement - combine with --headless and --run-mzf." },
    { "--speed",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<percent|max>",
      "Set the emulation speed at start: 'max' = MAX SPEED (no frame pacing), "
      "an integer 1..4000 = custom speed in percent (100 = normal speed). "
      "Works in both GUI and --headless (where emulation otherwise runs in "
      "real time). Invalid value = error and exit. If combined with "
      "--maxspeed-bench, MAX SPEED wins." },
    { "--home-dir",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Override the home directory (where the binary's read-only assets live: "
      "ui_resources/, locale/, certs/). Default: auto-detected from the binary "
      "location." },
    { "--cfg-dir",          SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Override the config directory (where mz<arch>emu.ini, "
      "mz<arch>emu-imgui.ini, breakpoints.ini, persistent ramdisk images, "
      "the Unicard SD card root and the MemExt FLASH file are stored when "
      "given as relative paths). Default: same as home-dir." },
    { "--work-dir",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Override the work directory (default open location for file dialogs). "
      "Default: current working directory at startup." },
    { "--config",           SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Path to the main config file (mz<arch>emu.ini). Default: cfg-dir/mz<arch>emu.ini." },
    { "--imgui-ini",        SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Path to the ImGui state file (mz<arch>emu-imgui.ini). Default: "
      "cfg-dir/mz<arch>emu-imgui.ini." },
    { "--spdfd",            SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Reserved internal launch mode." },
    { "--mcp-pipe",         SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Run in MCP pipe transport mode (headless emu, JSONL on stdin/stdout). "
      "Intended for spawning by mcp_server.py as a subprocess. Implies "
      "--headless and switches the main thread to mcp_pipe_main(). "
      "Only effective if the binary was built with MCP backend enabled "
      "(no NO_MCP=1)." },
    { "--mcp-tcp-port",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Automatically start the MCP TCP listener on the given port after "
      "emulator initialization. Useful for headless / scripted workflows. "
      "Overrides INI [MCP] tcp_port. Implies auto-start (overrides INI "
      "[MCP] auto_start_tcp=false). Default: server is OFF on startup, "
      "the user enables it via Tools -> MCP TCP Server -> Start. Only "
      "effective if the binary was built with MCP TCP backend enabled "
      "(no NO_MCP_TCP=1)." },
    { "--mcp-bind",         SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MCP_BIND_VALUES, "<127.0.0.1|0.0.0.0>",
      "Bind address for the MCP TCP server. '127.0.0.1' = loopback only "
      "(default, safe); '0.0.0.0' = all interfaces (RISKY without auth - "
      "V0 has no authentication). Overrides INI [MCP] bind_address." },
    { "--mcp-profile",      SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MCP_PROFILE_VALUES, "<wild|confined|sandboxed|observer>",
      "MCP security profile. 'wild' (default) = no restrictions, dev "
      "mode; others are persisted but not yet enforced (full enforcement "
      "lands in V1.A). Overrides INI [MCP] profile." },
    /* FDC volby (viz src/emulator/hw-generic/fdc/). */
    { "--fdc-bus-xlate",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        FDC_BUS_XLATE_VALUES, "<invert|passthrough>",
      "FDC BUS translation polarity. 'invert' = Sharp default "
      "(XOR 0xFF between CPU bus and WD279x chip), 'passthrough' = experimental "
      "no-inversion mode for future 'true bus' ROMs." },
    { "--fdc-hd-patch",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "Enable/disable the HD Patch circuit (port 0xDF EINT logic) on the FDC." },
    { "--fdd",              SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<N>=<filepath>",
      "Insert a DSK image into floppy drive N at startup (repeatable). "
      "N 0-3 = drives 0-3 of the FD controller, 4-7 = drives 0-3 of the "
      "secondary FD controller (FDC1, must be connected). Replaces the disk "
      "from the INI; the path is saved to the INI like a disk inserted from "
      "the menu (unless --no-save-ini). Missing file or a duplicate drive = "
      "error and exit; an image that cannot be opened = error." },
    { "--fdd0",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 0=<filepath> (FDC0 drive 0)." },
    { "--fdd1",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 1=<filepath> (FDC0 drive 1)." },
    { "--fdd2",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 2=<filepath> (FDC0 drive 2)." },
    { "--fdd3",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 3=<filepath> (FDC0 drive 3)." },
    { "--fdd4",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 4=<filepath> (FDC1 drive 0)." },
    { "--fdd5",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 5=<filepath> (FDC1 drive 1)." },
    { "--fdd6",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 6=<filepath> (FDC1 drive 2)." },
    { "--fdd7",            SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<filepath>",
      "Same as --fdd 7=<filepath> (FDC1 drive 3)." },
    { "--fdd-readonly",     SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<N>",
      "Write-protect floppy drive N (0-7) for this session only (repeatable); "
      "the INI setting is kept." },
    { NULL, SDLAPP_OPTION_FLAG, SDLAPP_OPTVAL_NONE, NULL, NULL, NULL }   /* sentinel */
};


int main(int argc, char *argv[])
{
    /* Inicializace options moduly před čímkoliv jiným, aby si moduly mohly
     * své options vytáhnout v dalších fázích startu. */
    sdlapp_options_init(argc, argv);

    /* --help: vypsat usage a skončit. Na Windows se GUI binárka nejdřív
     * připojí ke konzoli rodiče, jinak by se výpis z cmd.exe ztratil. */
    if (sdlapp_option_present("--help"))
    {
        version_info_prepare_console();
        sdlapp_options_print_help(argv[0], g_known_options);
        return EXIT_SUCCESS;
    };

    /* --version: vypsat informace o verzi a buildu a skončit. Stejně jako
     * --help se vyhodnocuje před validací ostatních voleb a před jakoukoliv
     * inicializací (SDL video/audio, cfgmain, emulátor); má proto přednost
     * i před --mcp-pipe. */
    if (sdlapp_option_present("--version"))
    {
        version_info_prepare_console();
        version_info_print(stdout);
        return EXIT_SUCCESS;
    };

    /* Validace - neznámé options nebo VALUE option bez hodnoty = chyba. */
    if (!sdlapp_options_validate(g_known_options))
    {
        /* Automatizační režim: kód 3 + result.json (automation.h). */
        return automation_cli_error(EXIT_FAILURE, "invalid command line options");
    };

    /* --speed: hodnota se ověří hned (neplatná = chyba a konec), aplikuje ji
     * až emulátorové vlákno při startu (emulator.c). */
    if (sdlapp_option_present("--speed"))
    {
        bool speed_max = false;
        int speed_percent = 100;
        if (!customspeed_parse_cli_value(sdlapp_option_value("--speed"), &speed_max, &speed_percent))
        {
            return automation_cli_error(EXIT_FAILURE, "invalid --speed value");
        };
    };

    /* --mode-switch: hodnota se ověří hned (neplatná = chyba a konec),
     * aplikuje ji mz800_main při registraci konfigurace. Jinde než na MZ-800
     * se přepínač z příkazové řádky nenastavuje. */
    if (sdlapp_option_present("--mode-switch"))
    {
        en_MZ800_MODE_SW mode;
        if (!mzarch_mode_sw_parse_cli(sdlapp_option_value("--mode-switch"), &mode))
        {
            return automation_cli_error(EXIT_FAILURE, "invalid --mode-switch value");
        };
#if MZARCH != 800
        fprintf(stderr, "Warning: --mode-switch is only supported on MZ-800, ignoring\n");
#endif
    };

    /* --cmt, --cmt-play, --cmt-play-pause, --cmt-boot: kombinace voleb
     * a soubor pásky se ověří hned (chyba = konec), pásku vloží až
     * emulátorové vlákno po resetu (mzarch_main -> cmt_cli_apply). */
    if (!cmt_cli_validate())
    {
        return automation_cli_error(EXIT_FAILURE, "invalid tape options");
    };

    /* --fdd, --fdd<N>, --fdd-readonly: čísla mechanik a soubory se ověří
     * hned (chyba = konec), disky vloží fdc_init() při inicializaci
     * emulátoru; chyby zjištěné až tam ohlásí emu vlákno (fdc_cli_report). */
    if (!fdc_cli_validate())
    {
        return automation_cli_error(EXIT_FAILURE, "invalid floppy disk options");
    };

    /* Automatizace (--exit-*, --timeout-*, --result-json, --bpt, ...):
     * volby se ověří hned, běh řídí emulátorové vlákno (automation_start). */
    if (!automation_cli_validate())
    {
        return automation_cli_error(EXIT_FAILURE, "invalid automation options");
    };

    /* Záznam a přehrání vstupů (--replay-record, --replay, ...): volby se
     * ověří hned, záznam / přehrání zahájí emulátorové vlákno
     * (replay_cli_start). */
    if (!replay_cli_validate())
    {
        return automation_cli_error(EXIT_FAILURE, "invalid replay options");
    };

    /* --spdfd: alternativní launch mód. Větvíme dřív, než se inicializuje
     * jakákoliv emulátorová infrastruktura - modul si vytváří vlastní SDL
     * kontext, vlastní okno a vlastní ImGui kontext. Pokud bychom prošli
     * sdlapp_new() + iface_init(), dva SDL_Init / dva ImGui::CreateContext
     * by se nemusely snášet.
     */
    if (sdlapp_option_present("--spdfd"))
    {
        int spdfd_rc = spdfd_main(argc, argv);
        if (spdfd_rc != SPDFD_FALLBACK_TO_EMU)
        {
            return spdfd_rc;
        };
    };

    /* --pipe: deleguj na MCP pipe entry-point. Funkce sama injectuje
     * --headless, spawnuje emu vlákno, posílá HELLO message a blokuje
     * až do shutdown signálu. Po návratu rovnou exit, nepokračujeme
     * v GUI inicializaci. Když build je s NO_MCP=1, mcp_pipe_main
     * v jeho TU neexistuje - whitelist entry zůstává viditelný v
     * --help kvůli stabilnímu CLI, ale runtime cesta selže linkrem
     * (build time error). Proto guard zde:
     */
    if (sdlapp_option_present("--mcp-pipe"))
    {
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
        return mcp_pipe_main(argc, argv);
#else
        fprintf(stderr,
                "[mz800emu] --mcp-pipe requires MCP backend (rebuild without NO_MCP=1)\n");
        return EXIT_FAILURE;
#endif
    }

#ifdef _WIN32
    if (sdlapp_option_present("--console"))
    {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
    };
#endif

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    /* Detekce --headless módu - SDL3 video/audio backendy běží v no-op
     * režimu (žádné SDL okno, žádný audio device). Framebuffer pipeline
     * pokračuje renderovat do paměti. Smyčka sdlapp_run drží proces běžet
     * do SDL_EVENT_QUIT (SIGINT / Ctrl+C). */
    if (sdlapp_option_present("--headless"))
    {
        g_print("[INFO] Running in headless mode (SDL3 video/audio: no-op)\n");
    };

    // printf ("Hello, World!\n"); // prints Hello, World! :-)
    g_print("\n");

    // g_print("Working dir: %s\n", g_get_current_dir());
    // g_print("Exe path: %s\n", g_get_prgname());

    g_sdlapp = sdlapp_new();
    if (!g_sdlapp)
    {
        SDLAPP_ERROR("Failed to create SdlApp");
        return automation_cli_error(EXIT_FAILURE, "emulator initialization failed");
    };

    /* i18n_init musí být po sdlapp_new — používá g_sdlapp->paths->home_dir
     * pro lokalizaci adresáře s .mo soubory. */
    i18n_init();

    // cfgmain musi byt prvni inicializovany modul !!!
    cfgmain_init();

    /* Aplikace jazyka z konfigurace (po cfgmain_init, kde se načte INI) */
    i18n_lang_apply();

    if (!iface_init())
    {
        SDLAPP_ERROR("Failed to initialize interface");
        return automation_cli_error(EXIT_FAILURE, "emulator initialization failed");
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Inicializace dbgapi CMDRQ fronty - musí být před dispatcherem
     * a před spuštěním EMU vlákna. EMU vlákno frontu drainuje v
     * mzarch hot loop / paused loop, UI vlákno do ní submituje. */
    dbgapi_init(&g_dbgapi_cmdrq_queue);

    /* Registrace dispatcheru pro doručení dbgapi MSG z EMU vlákna do UI.
     * Musí být zaregistrovaný před spuštěním EMU vlákna, jinak by raně
     * odeslané MSGy přišly o doručení. */
    dbgapi_dispatcher_init();
#endif

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
    /* V0.B.4: aplikujeme CLI overrides na g_mcp_config (INI hodnoty
     * už byly načteny v cfgmain_init -> mcp_config_init). Priorita:
     *
     *   default <- INI [MCP] <- CLI flags
     *
     * Auto-start TCP serveru se spustí pokud:
     *   - uživatel zadal --mcp-tcp-port=N (zpětně kompatibilní s V0.A.5),
     *   - NEBO g_mcp_config.auto_start_tcp == true (= INI persist V0.B.4).
     *
     * Server se startuje před emu thread spawnem, aby případný klient
     * mohl zachytit i první instrukce po reset. Pro GUI manual Start
     * cestu zůstává g_mcp_tcp_server NULL až do prvního kliknutí v
     * Tools menu.
     */

    /* CLI override: --mcp-tcp-port=N */
    if (sdlapp_option_present("--mcp-tcp-port"))
    {
        const char *port_str = sdlapp_option_value("--mcp-tcp-port");
        gint64 port_val = (port_str) ? g_ascii_strtoll(port_str, NULL, 10) : 0;
        if (port_str && port_val >= 1024 && port_val <= 65535)
        {
            g_mcp_config.tcp_port = (int)port_val;
        }
        else
        {
            fprintf(stderr,
                "[MCP] --mcp-tcp-port requires an integer in 1024..65535 (got: %s) - ignoring\n",
                port_str ? port_str : "<none>");
        }
    }

    /* CLI override: --mcp-bind=127.0.0.1|0.0.0.0 */
    if (sdlapp_option_present("--mcp-bind"))
    {
        const char *bind_str = sdlapp_option_value("--mcp-bind");
        g_mcp_config.bind_addr = mcp_config_bind_addr_from_str(bind_str);
    }

    /* CLI override: --mcp-profile=wild|confined|sandboxed|observer */
    if (sdlapp_option_present("--mcp-profile"))
    {
        const char *prof_str = sdlapp_option_value("--mcp-profile");
        g_mcp_config.profile = mcp_config_profile_from_str(prof_str);
    }

    /* Auto-start TCP server: implicitně pokud uživatel zadal
     * --mcp-tcp-port=N (V0.A.5 zpětná kompatibilita), nebo pokud
     * INI [MCP] auto_start_tcp=true. */
    bool mcp_should_auto_start =
        sdlapp_option_present("--mcp-tcp-port") ||
        g_mcp_config.auto_start_tcp;

    if (mcp_should_auto_start)
    {
        g_mcp_tcp_server = mcp_tcp_server_create();
        if (g_mcp_tcp_server)
        {
            const char *bind = mcp_config_bind_addr_str(g_mcp_config.bind_addr);
            en_MCP_TCP_START_RESULT r = mcp_tcp_server_start(
                g_mcp_tcp_server, g_mcp_config.tcp_port, bind);
            if (r != MCP_TCP_START_OK)
            {
                fprintf(stderr,
                    "[MCP] auto-start of TCP server on %s:%d failed (rc=%d)\n",
                    bind, g_mcp_config.tcp_port, (int)r);
            }
        }
    }
#endif

    // nastartujeme emulator
    GThread *emu_thread = g_thread_new("mz-800-emulator", emulator_thread, NULL);
    if (!emu_thread)
    {
        SDLAPP_ERROR("Failed to start emulator thread");
        return automation_cli_error(EXIT_FAILURE, "emulator initialization failed");
    };

    /* Headless: hlavní vlákno nemá okno, které by smyčku brzdilo vykreslením
     * (vsync), a ve výchozím režimu SDLAPP_LOOP_POLL by se točilo naprázdno
     * na celém jádru. Práce pro hlavní vlákno chodí jako události SDL
     * (časovače sdlapp, SDL_EVENT_QUIT), které čekání hned probudí; timeout
     * jen omezuje, jak dlouho po sdlapp_quit() z jiného vlákna smyčka ještě
     * běží. Emulace, MCP TCP server a zvuk mají vlastní vlákna. */
    if (sdlapp_option_present("--headless"))
    {
        sdlapp_set_loop_mode(g_sdlapp, SDLAPP_LOOP_WAIT_TIMEOUT, MAIN_HEADLESS_LOOP_WAIT_MS);
    };

    sdlapp_run(g_sdlapp);

    // pockej na ukonceni emulatoru (g_sdlapp i SDL audio jsou stale aktivni,
    // takze emulatorove vlakno se muze korektne ukoncit pres sdlapp_is_running() check)
    gpointer emuret = g_thread_join(emu_thread);
    int emuretval = (emuret) ? *(int *)emuret : EXIT_FAILURE;

    // az po ukonceni emulatoroveho vlakna muzeme uklidit
#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
    /* V0.A.5: shutdown TCP listeneru. Musí být před iface_exit / SDL_Quit,
     * aby případné aktivní per-conn vlákno mělo platný emu state pro
     * poslední dispatch + clean close. mcp_tcp_server_shutdown_global je
     * idempotentní (NULL guard) - bezpečné volat i bez explicitního Start.
     */
    mcp_tcp_server_shutdown_global();
#endif
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* Opuštěný (zaseknutý) MCP požadavek mohl ještě dokončovat handler na
     * pracovním vlákně (dispatch_runner.c); zámky dbgapi se smí zrušit až
     * po něm. */
    bool mcp_runner_idle = mcp_dispatch_runner_wait_idle(2000);
    if (!mcp_runner_idle)
        fprintf(stderr, "[MCP] abandoned MCP request still running at exit, "
                "skipping dbgapi teardown\n");
#else
    bool mcp_runner_idle = true;
#endif
    (void) mcp_runner_idle;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Odregistrace dispatcheru - po této funkci se MSGy zahodí. */
    dbgapi_dispatcher_shutdown();

    /* Destrukce dbgapi fronty - po ukončení EMU vlákna a dispatcheru
     * (= žádný producer ani konzument už nebude přistupovat). Pokud
     * opuštěný MCP požadavek ještě běží, zámky nerušíme (únik při exitu
     * je neškodný, zrušený zámek pod běžícím vláknem ne). */
    if (mcp_runner_idle)
        dbgapi_destroy(&g_dbgapi_cmdrq_queue);
#endif

    iface_exit();
    sdlapp_destroy(g_sdlapp);
    SDL_Quit();

    g_print("Emulator exit value: %d\n", emuretval);
    return emuretval;
}
