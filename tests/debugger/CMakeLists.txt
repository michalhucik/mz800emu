mz_add_test(breakpoints
    TYPE     EMU
    SOURCES  test_breakpoints.c
    LABELS   "emu;debugger"
)

mz_add_test(breakpoints_v2
    TYPE     EMU
    SOURCES  test_breakpoints_v2.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_expr
    TYPE     EMU
    SOURCES  test_bp_expr.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_vars
    TYPE     EMU
    SOURCES  test_bp_vars.c
    LABELS   "emu;debugger"
)

mz_add_test(watch
    TYPE     EMU
    SOURCES  test_watch.c
    LABELS   "emu;debugger"
)

mz_add_test(bookmarks
    TYPE     EMU
    SOURCES  test_bookmarks.c
    LABELS   "emu;debugger"
)

mz_add_test(freeze
    TYPE     EMU
    SOURCES  test_freeze.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_action
    TYPE     EMU
    SOURCES  test_bp_action.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_enforce
    TYPE     EMU
    SOURCES  test_bp_enforce.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_match
    TYPE     EMU
    SOURCES  test_bp_match.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_ratelimit
    TYPE     EMU
    SOURCES  test_bp_ratelimit.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_persist_bc
    TYPE     EMU
    SOURCES  test_bp_persist_bc.c
    LABELS   "emu;debugger"
)
# if(TARGET): test_bp_persist_bc se při MZ_NO_DEBUGGER neregistruje (debugger
# label), pak target neexistuje a tato customizace by spadla configure chybou.
if(TARGET test_bp_persist_bc)
    target_compile_definitions(test_bp_persist_bc PRIVATE
        MZTEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
endif()

mz_add_test(bp_integration
    TYPE     EMU
    SOURCES  test_bp_integration.c
    LABELS   "emu;debugger;integration"
)

# Historie instrukcí (cpuhist): registry před instrukcí zachycené při M1.
mz_add_test(history_regs
    TYPE     EMU
    SOURCES  test_history_regs.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_hw_event
    TYPE     EMU
    SOURCES  test_bp_hw_event.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_sp_threshold
    TYPE     EMU
    SOURCES  test_bp_sp_threshold.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_zone
    TYPE     EMU
    SOURCES  test_bp_zone.c
    LABELS   "emu;debugger"
)

mz_add_test(sym_db
    TYPE     EMU
    SOURCES  test_sym_db.c
    LABELS   "emu;debugger"
)

mz_add_test(dbgapi
    TYPE     EMU
    SOURCES  test_dbgapi.c
    LABELS   "emu;debugger"
)

mz_add_test(dbgapi_cmd_origin
    TYPE     EMU
    SOURCES  test_dbgapi_cmd_origin.c
    LABELS   "emu;debugger"
)

# Každý příkaz dbgapi má výslovnou třídu záznamu vstupů (replay_dbg_classify,
# N48): nový příkaz bez zařazení test shodí.
mz_add_test(replay_dbg_classify
    TYPE     EMU
    SOURCES  test_replay_dbg_classify.c
    LABELS   "emu;debugger;replay"
)

mz_add_test(dbgapi_submit_timeout
    TYPE     EMU
    SOURCES  test_dbgapi_submit_timeout.c
    LABELS   "emu;debugger"
)

# Výběr řídicího příkazu z fronty mimo pořadí (drain na hranici akce BP).
mz_add_test(dbgapi_dequeue_matching
    TYPE     EMU
    SOURCES  test_dbgapi_dequeue_matching.c
    LABELS   "emu;debugger"
)

# Zápis Memory Browseru přes CMDRQ frontu (ne přímo z UI vlákna).
# Linkuje skutečný backend membrowser_io.cpp (bez ImGui závislostí).
mz_add_test(membrowser_io_queue
    TYPE     EMU
    SOURCES  test_membrowser_io_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/membrowser/membrowser_io.cpp
    LABELS   "emu;debugger;ui-membrowser"
)

# Okno I/O Ports: změna kapacity / clear historie a reset aktivity (všech
# portů i jednoho portu z kontextového menu) jdou přes
# CMDRQ frontu (UI helpery v dbgapi_helpers.cpp), ne přímo z UI vlákna.
mz_add_test(io_window_queue
    TYPE     EMU
    SOURCES  test_io_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ui-ioview"
)

# Okno Breakpoints: Delete All, načtení BP ze souboru a reset počítadla zásahů
# (editační panel) jdou přes CMDRQ frontu (UI helpery v dbgapi_helpers.cpp),
# ne přímo z UI vlákna.
mz_add_test(bpt_window_queue
    TYPE     EMU
    SOURCES  test_bpt_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ui-breakpoints"
)

# Test Eval v editačním panelu BP: výraz vyhodnotí emu vlákno přes CMDRQ
# frontu (dbg_ui_bp_test_eval), znak `!` se ignoruje (bez spuštění MEM_R BP).
mz_add_test(bp_test_eval_queue
    TYPE     EMU
    SOURCES  test_bp_test_eval_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ui-breakpoints"
)

# Okno Memory Heatmap a menu CDL: přepnutí režimu (swap callbacků), reset,
# reset regionu a Add / Sub importu jdou přes CMDRQ frontu (UI helpery
# v dbgapi_helpers.cpp), ne přímo z UI vlákna.
mz_add_test(mhmap_window_queue
    TYPE     EMU
    SOURCES  test_mhmap_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ui-heatmap"
)

# Okno Memory Map: banking bity a DMD (MZ-800) mění emu vlákno přes
# DBGAPI_CMD_MEMMAP_SET (UI helpery v dbgapi_helpers.cpp), ne přímo UI
# vlákno; ověřuje i konzistenci mapování stránek a RAM fast-path.
mz_add_test(memmap_window_queue
    TYPE     EMU
    SOURCES  test_memmap_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ui-memmap"
)

# Okno Events: režim záznamu, maska kategorií a import souboru do ringu
# jdou přes CMDRQ frontu (DBGAPI_CMD_EVENTLOG_SET_MODE / _SET_MASK /
# _IMPORT_FILE, UI helpery v dbgapi_helpers.cpp), ne přímo z UI vlákna.
mz_add_test(eventview_window_queue
    TYPE     EMU
    SOURCES  test_eventview_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;event-viewer"
)

# Okno Events: triggery Pause / Auto-mark on match mění emu vlákno přes
# CMDRQ frontu (DBGAPI_CMD_EVENTLOG_TRIGGER_SET / _CLEAR_MATCHES, UI helpery
# v dbgapi_helpers.cpp); UI předává nový filtr vlastnicky a starý uvolní až
# po jeho vrácení emu vláknem.
mz_add_test(eventview_trigger_queue
    TYPE     EMU
    SOURCES  test_eventview_trigger_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;event-viewer"
)

# Memory Browser: zafrození / uvolnění bajtu (Freeze Bytes) jde přes CMDRQ
# frontu (DBGAPI_CMD_FREEZE_ADD / _REMOVE, UI helpery v dbgapi_helpers.cpp),
# ne přímo z UI vlákna.
mz_add_test(freeze_window_queue
    TYPE     EMU
    SOURCES  test_freeze_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ui-membrowser"
)

# Panel Callstack: zapnutí / vypnutí (registrace Z80 CALL/RET hooků)
# a vyprázdnění shadow stacku jdou přes CMDRQ frontu
# (DBGAPI_CMD_CALLSTACK_SET_ACTIVE / _RESET, UI helpery v dbgapi_helpers.cpp),
# ne přímo z UI vlákna.
mz_add_test(callstack_window_queue
    TYPE     EMU
    SOURCES  test_callstack_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ui-callstack"
)

# Okno RAM Write Log a Memory Browser (Who wrote this byte?): záznam zápisů
# se ovládá a čte přes CMDRQ frontu (DBGAPI_CMD_RAMLOG, UI helper
# dbg_ui_ramlog v dbgapi_helpers.cpp), ne přímo z UI vlákna.
mz_add_test(ramlog_window_queue
    TYPE     EMU
    SOURCES  test_ramlog_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger;ramlog"
)

# Záložka Coverage okna CPU Profiler a značky pokrytí v Disassembly: data
# jdou přes CMDRQ frontu (DBGAPI_CMD_COVERAGE, helper dbg_ui_coverage),
# kopie v UI vlákně (dbg_coverage_data.cpp) s omezenou četností obnovy.
mz_add_test(coverage_window_queue
    TYPE     EMU
    SOURCES  test_coverage_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/sections/dbg_coverage_data.cpp
    LABELS   "emu;debugger;coverage"
)

# Okno debuggeru (Ctrl+R) a menu Emulation: vynucený refresh obrazovky jde
# přes CMDRQ frontu (DBGAPI_CMD_SCREEN_REFRESH, UI helper v dbgapi_helpers.cpp),
# ne přímo z UI vlákna.
mz_add_test(screen_refresh_queue
    TYPE     EMU
    SOURCES  test_screen_refresh_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger"
)

mz_add_test(tlog_common
    TYPE     EMU
    SOURCES  test_tlog_common.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(trace_reader
    TYPE     EMU
    SOURCES  test_trace_reader.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(cputrack
    TYPE     EMU
    SOURCES  test_cputrack.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(iorqlog
    TYPE     EMU
    SOURCES  test_iorqlog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(intlog
    TYPE     EMU
    SOURCES  test_intlog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(hwlog
    TYPE     EMU
    SOURCES  test_hwlog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(marklog
    TYPE     EMU
    SOURCES  test_marklog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(eventlog
    TYPE     EMU
    SOURCES  test_eventlog.c
    LABELS   "emu;debugger;event-viewer"
)

mz_add_test(eventlog_filter
    TYPE     EMU
    SOURCES  test_eventlog_filter.c
    LABELS   "emu;debugger;event-viewer"
)

mz_add_test(eventlog_decoder
    TYPE     EMU
    SOURCES  test_eventlog_decoder.c
    LABELS   "emu;debugger;event-viewer"
)

mz_add_test(io_catalog
    TYPE     EMU
    SOURCES  test_io_catalog.c
    LABELS   "emu;debugger"
)

mz_add_test(io_activity
    TYPE     EMU
    SOURCES  test_io_activity.c
    LABELS   "emu;debugger"
)

mz_add_test(io_history
    TYPE     EMU
    SOURCES  test_io_history.c
    LABELS   "emu;debugger"
)

# Kapacita ringu historie I/O z [IO_PORTS_PANEL] history_capacity se
# projeví po initu (klíč patří jádru, debugger_init volá
# io_history_init_from_cfg) a změna kapacity se uloží.
mz_add_test(io_history_cfg
    TYPE     EMU
    SOURCES  test_io_history_cfg.c
    LABELS   "emu;debugger"
)

mz_add_test(io_history_filter
    TYPE     EMU
    SOURCES  test_io_history_filter.c
    LABELS   "emu;debugger"
)

mz_add_test(io_filter_andor_group
    TYPE     EMU
    SOURCES  test_io_filter_andor_group.c
    LABELS   "emu;debugger"
)

mz_add_test(io_ports_cfg
    TYPE     EMU
    SOURCES  test_io_ports_cfg.c
    LABELS   "emu;debugger"
)

mz_add_test(io_tracking_e2e
    TYPE     EMU
    SOURCES  test_io_tracking_e2e.c
    LABELS   "emu;debugger;integration"
)

mz_add_test(callstack
    TYPE     EMU
    SOURCES  test_callstack.c
    LABELS   "emu;debugger"
)

mz_add_test(profiler
    TYPE     EMU
    SOURCES  test_profiler.c
    LABELS   "emu;debugger"
)

# Profiler podle PC (profiler_pc.h): součty na adresu, nečinný HALT,
# přijetí přerušení, nespojitost čítače cyklů, výběr řádků a export CSV.
mz_add_test(profiler_pc
    TYPE     EMU
    SOURCES  test_profiler_pc.c
    LABELS   "emu;debugger"
)

# Pokrytí kódu, dekodér kroku Z80 (coverage_decode.h): délky a výsledky
# podmínek proti skutečnému kroku jádra z80_step a proti z80_dasm.
mz_add_test(coverage_decode
    TYPE     STANDALONE
    SOURCES  test_coverage_decode.c
             ${CMAKE_SOURCE_DIR}/src/emulator/debugger/coverage/coverage_decode.c
    LIBS     mzlib_cpu_z80 mzlib_dasm_z80
    LABELS   "standalone;debugger"
)

# Pokrytí kódu, jádro (coverage.h): úrovně branch a detail nad syntetickými
# kroky - stavy větví, verze kódu, kapacita, nečinný HALT, epochy; snímek
# a filtr řádků (coverage_export.h) pro MCP a GUI.
mz_add_test(coverage
    TYPE     STANDALONE
    SOURCES  test_coverage.c
             ${CMAKE_SOURCE_DIR}/src/emulator/debugger/coverage/coverage_decode.c
             ${CMAKE_SOURCE_DIR}/src/emulator/debugger/coverage/coverage_export.c
    LIBS     mz::glib mz::json_glib
    LABELS   "standalone;debugger"
)

mz_add_test(mhmap_resolve
    TYPE     EMU
    SOURCES  test_mhmap_resolve.c
    LABELS   "emu;debugger"
)

# Příznak debugger_is_initialized(): okna debuggeru v UI vlákně se kreslí až
# po debugger_init() v emu vlákně (dřív pád okna Breakpoints na NULL poli).
mz_add_test(debugger_init_flag
    TYPE     EMU
    SOURCES  test_debugger_init_flag.c
    LABELS   "emu;debugger"
)

# Jádro záznamu zápisů do paměti (ramlog): ring, mapa posledního
# zapisovatele, filtr, dotazy - bez emulátoru.
mz_add_test(ramlog
    TYPE     STANDALONE
    SOURCES  test_ramlog.c
             ${CMAKE_SOURCE_DIR}/src/emulator/debugger/trace/ramlog.c
    LABELS   "standalone;debugger;ramlog"
)
# if(TARGET): při MZ_NO_DEBUGGER se test neregistruje a konfigurace by jinak
# skončila chybou "target not built by this project".
if(TARGET test_ramlog)
    target_include_directories(test_ramlog PRIVATE ${CMAKE_SOURCE_DIR}/src/emulator/debugger/trace)
endif()

# Ring výstupu akcí BP (bplog): líná alokace, segment, ořez, přetečení -
# bez emulátoru.
mz_add_test(bplog
    TYPE     STANDALONE
    SOURCES  test_bplog.c
             ${CMAKE_SOURCE_DIR}/src/emulator/debugger/trace/bplog.c
    LABELS   "standalone;debugger"
)
if(TARGET test_bplog)
    target_include_directories(test_bplog PRIVATE ${CMAKE_SOURCE_DIR}/src/emulator/debugger/trace)
endif()

# Čísla běhů a kruh záznamů zastavení (run_log, run-control): číslování,
# čekání s timeoutem a probuzením, přepsání kruhu, ukončení - bez emulátoru.
mz_add_test(run_log
    TYPE     STANDALONE
    SOURCES  test_run_log.c
             ${CMAKE_SOURCE_DIR}/src/emulator/debugger/run_control/run_log.c
    LABELS   "standalone;debugger"
)
if(TARGET test_run_log)
    target_include_directories(test_run_log PRIVATE ${CMAKE_SOURCE_DIR}/src/emulator/debugger/run_control)
endif()

# Drain fronty v pauze skončí po příkazu kroku: příkaz poslaný hned po
# kroku se zpracuje až po jeho provedení (MCP step_n ztrácel kroky).
mz_add_test(dbgapi_drain_paused
    TYPE     EMU
    SOURCES  test_dbgapi_drain_paused.c
    LABELS   "emu;debugger"
)

# Napojení záznamu zápisů (ramlog_hook) na jádro MZ-800: callback, maska
# RAM fast-path, RAM / ROM / plány VRAM, zdroje, reset.
mz_add_test(ramlog_hook
    TYPE     EMU
    SOURCES  test_ramlog_hook.c
    LABELS   "emu;debugger;ramlog"
)

# Postupné hledání proměnných (memsearch): čisté jádro nad syntetickými
# vzorky - dekódování, filtry, undo/redo s referencí, odpojený span,
# předvolby prostoru; informativně čas filtru nad 16 MiB.
mz_add_test(memsearch
    TYPE     EMU
    SOURCES  test_memsearch.c
    LABELS   "emu;debugger"
)

# Memory Search: vzorek prostoru hledání pořizuje emu vlákno přes CMDRQ
# frontu (DBGAPI_CMD_MEMSEARCH_SAMPLE, UI helper dbg_ui_memsearch_sample
# v dbgapi_helpers.cpp), odpojený span se označí.
mz_add_test(memsearch_window_queue
    TYPE     EMU
    SOURCES  test_memsearch_window_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/dbgapi_helpers.cpp
    LABELS   "emu;debugger"
)
