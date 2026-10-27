# tests/mcp/CMakeLists.txt - MCP backend testy
#
# V0.A.2: standalone test JSONL parseru.
# V0.A.3: standalone test dispatch vrstvy s mock dbgapi (stub).
#
# Oba testy jsou STANDALONE (= Unity bez emu core), linkují přímo
# zdrojové soubory z src/emulator/mcp/ s definicí MZ800EMU_MCP_TEST_BUILD
# která vypne include `mzarch_config.h`.
#
# Pokud MZ_NO_MCP=ON, testy nemají smysl (zdroje by se v guard-blockovaném
# režimu zkompilovaly jako prázdné TU) - guard navíc výslovně přeskočíme
# registraci.

if(MZ_NO_MCP)
    message(STATUS "  tests/mcp: skipped (MZ_NO_MCP=ON)")
    return()
endif()

# ---------------------------------------------------------------
# Test 1: JSONL parser (V0.A.2)
# ---------------------------------------------------------------
mz_add_test(mcp_jsonl_parser
    TYPE     STANDALONE
    SOURCES
        test_jsonl_parser.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/jsonl_io.c
    LABELS   "standalone;mcp"
)

# Override compile definitions na samotný test target - potřebuje:
#   MZ800EMU_MCP_TEST_BUILD - vypne include mzarch_config.h v jsonl_io.c
#   MZ800EMU_CFG_MCP_SERVER_ENABLED - aktivuje tělo jsonl_io.c
target_compile_definitions(test_mcp_jsonl_parser PRIVATE
    MZ800EMU_MCP_TEST_BUILD
    MZ800EMU_CFG_MCP_SERVER_ENABLED
)

# Linkneme json-glib + glib (jsonl_io.c je na nich závislý).
target_link_libraries(test_mcp_jsonl_parser PRIVATE
    mz::glib
    mz::json_glib
)

# ---------------------------------------------------------------
# Test 2: Dispatch vrstva (V0.A.3)
# ---------------------------------------------------------------
# Linkuje dispatch.c + jsonl_io.c + stub pro dbgapi
# (test_dispatch_dbgapi_stub.c poskytuje g_dbgapi_cmdrq_queue symbol
# a mock implementaci dbgapi_ui_submit_cmd_sync_with_origin).
mz_add_test(mcp_dispatch
    TYPE     STANDALONE
    SOURCES
        test_dispatch.c
        test_dispatch_dbgapi_stub.c
        test_hid_keymap_stub.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/dispatch.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/hid_keymap.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/jsonl_io.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/cooperation.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/event_bus.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/trap_manager.c
        ${CMAKE_SOURCE_DIR}/src/emulator/screenshot/screenshot.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/trace/trace_reader.c
        ${CMAKE_SOURCE_DIR}/src/emulator/audio_analysis.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/trace/ramlog.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/trace/bplog.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/coverage/coverage.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/coverage/coverage_decode.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/coverage/coverage_export.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/memsearch/memsearch.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/run_control/run_log.c
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/dbgapi_cmd_props.c
        # device_screenshot (kp80 D4) kóduje PNG
        ${CMAKE_SOURCE_DIR}/src/emulator/debugger/png_encode.c
    LABELS   "standalone;mcp"
)

# trace_reader (trace_read) dekóduje cputrack disassemblerem; audio_analysis
# (Hz a noty audio_psg_writes) potřebuje libm.
target_link_libraries(test_mcp_dispatch PRIVATE mzlib_dasm_z80 m)

target_compile_definitions(test_mcp_dispatch PRIVATE
    MZ800EMU_MCP_TEST_BUILD
    MZ800EMU_CFG_MCP_SERVER_ENABLED
)

target_link_libraries(test_mcp_dispatch PRIVATE
    mz::glib
    mz::json_glib
)


# ---------------------------------------------------------------
# Test 3: MCP konfigurace - INI persist + validace (V0.B.4)
# ---------------------------------------------------------------
# LIB test - linkuje cfgmain + cfgfile + mcp_config pres emu core.
# Vyzaduje MCP TCP backend (NO_MCP_TCP=0).
if(NOT MZ_NO_MCP_TCP)
    mz_add_test(mcp_config
        TYPE     LIB
        SOURCES  test_mcp_config.c
        LABELS   "lib;mcp;config"
    )
else()
    message(STATUS "  tests/mcp: test_mcp_config skipped (MZ_NO_MCP_TCP=ON)")
endif()


# ---------------------------------------------------------------
# Test 4: Pipe transport E2E (V0.A.4)
# ---------------------------------------------------------------
# Python E2E test - spawns the built mz800emu(.exe) with --mcp-pipe and
# exercises the dispatch over JSONL stdin/stdout. Requires the mz800emu
# target to be built and present in CMAKE_SOURCE_DIR (= repo root, kam
# AddMzEmu.cmake kopíruje binárku via cp -f).
#
# Test sám má 10 podtestů (hello, ping, pause, get_state, get_registers,
# mem_read, reset, bp_add, unknown_cmd, shutdown). Pokud kterýkoliv
# selže, Python skript vrátí exit 1 a ctest test FAIL.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME    mcp_pipe_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_pipe.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_pipe_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 60
    )
    # Pojistka pro paralelní ctest - test pipe sám spawnuje mz800emu
    # subprocess, takže by neměl běžet souběžně se sám sebou. Žádný
    # RESOURCE_LOCK protože různé MZARCH targets nesdílíme.
    message(STATUS "  tests/mcp: registered mcp_pipe_e2e "
                   "(Python ${Python3_VERSION})")

    # Krok a rozběh z pauzy bez zbytečného 20ms čekání smyčky pauzy
    # (klient čeká jen na událost step_done / paused přes event_poll).
    add_test(
        NAME    mcp_pipe_resume_latency
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_pipe_resume_latency.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_pipe_resume_latency PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 60
    )

    # Hlavní vlákno v --headless se netočí naprázdno (spotřeba CPU procesu).
    add_test(
        NAME    mcp_headless_cpu
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_headless_cpu.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_headless_cpu PROPERTIES
        LABELS  "e2e;mcp"
        TIMEOUT 60
        SKIP_RETURN_CODE 77
    )

    # -----------------------------------------------------------
    # Test 4: MCP Python wrapper E2E přes JSON-RPC stdio (V0.B.3)
    # -----------------------------------------------------------
    # Spawne mcp_server.py jako subprocess, simuluje MCP klienta
    # (initialize + tools/list + tools/call), tím end-to-end ověří
    # celý stack Python wrapper -> JSONL -> emu backend. Vyžaduje
    # mcp[cli] v ``mcp-server/.venv`` (auto-detekce, fallback na
    # systémový Python).
    #
    # Test sám spustí mcp_server.py přes jeho vlastní venv interpret,
    # který má FastMCP. Ctest jen použije systémový Python jako
    # launcher (cesty venv si test resolvuje sám).
    add_test(
        NAME    mcp_server_stdio_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_server_stdio.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_server_stdio_e2e PROPERTIES
        LABELS  "e2e;mcp;stdio"
        TIMEOUT 90
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_server_stdio_e2e "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 5: TCP transport region wedge regrese (bug 0011)
    # -----------------------------------------------------------
    # Mock TCP server (BEZ emu) ověří, že velký region_read response
    # (>64 KiB JSONL řádek) nezasekne bridge, že error response se
    # surfacuje místo holého {}, a že _send_request self-healuje
    # off-by-one desync. Test importuje mcp_server in-process, takže
    # potřebuje FastMCP venv - sám se re-execne pod venv interpretem,
    # nebo skipne (exit 77) když venv chybí.
    add_test(
        NAME    mcp_tcp_region_wedge
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_tcp_region_wedge.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_tcp_region_wedge PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 60
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_tcp_region_wedge "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 5b: wrapper pozná konec procesu emulátoru
    # -----------------------------------------------------------
    # Falešný emulátor (Python skript, stejný JSONL protokol) místo
    # mz800emu: pád během požadavku musí _send_request ohlásit hned
    # ("emulator process exited (exit code N)"), ne po timeoutu; emu_status
    # hlásí last_exit; automatický restart dalším voláním nese
    # restarted: true; emu_stop se jako restart nehlásí; konec před hello
    # selže hned. Potřebuje FastMCP venv (jinak 77 = SKIP).
    add_test(
        NAME    mcp_wrapper_emu_exit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_wrapper_emu_exit.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_wrapper_emu_exit PROPERTIES
        LABELS  "mcp;wrapper"
        TIMEOUT 120
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_wrapper_emu_exit "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Wrapper: emu_event_poll s timeout_ms nad limitem odpovědi
    # -----------------------------------------------------------
    # Falešný emulátor odpoví na event_poll až po timeout_ms; wrapper
    # nesmí skončit timeoutem dřív (limit = SEND_TIMEOUT_S + timeout_ms).
    # Bez FastMCP (.venv) 77 (SKIP).
    add_test(
        NAME    mcp_wrapper_event_poll_timeout
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_wrapper_event_poll_timeout.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_wrapper_event_poll_timeout PROPERTIES
        LABELS  "mcp;wrapper"
        TIMEOUT 60
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_wrapper_event_poll_timeout "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 5c: oznámení restartu přes skutečný MCP protokol (stdio)
    # -----------------------------------------------------------
    # Spustí mcp_server.py pod venv (FastMCP) jako MCP server, místo
    # mz800emu podstrčí falešný emulátor (dávka/skript + Python) a jako
    # MCP klient ověří: pád emulátoru během tools/call -> hned isError
    # s exit kódem, emu_status last_exit, další volání nese restarted.
    add_test(
        NAME    mcp_server_restart_stdio
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_server_restart_stdio.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_server_restart_stdio PROPERTIES
        LABELS  "e2e;mcp;stdio;wrapper"
        TIMEOUT 120
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_server_restart_stdio "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 6: video záznam přes MCP (video-capture Task 15)
    # -----------------------------------------------------------
    # Spawne mz800emu v pipe módu (izolovaný --cfg-dir + INI v dočasném
    # adresáři) a projde postup agenta: videorec_start, emu run frames,
    # marker, snapshot retake, record-pause, stop -> ověří AVI (počet snímků
    # přes ffprobe, jinak hlavička AVI) a sidecar; plus auto-stop po N
    # snímcích s vygenerovaným jménem. Video záznam je jen na MZ-800, test
    # spouští binárku mz800emu z kořene repa (nebo MZ_EMU).
    add_test(
        NAME    mcp_videorec_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_videorec_e2e.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_videorec_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe;videorec"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_videorec_e2e "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7: tempo emulace v headless režimu (video-capture Task 16)
    # -----------------------------------------------------------
    # Spawne mz800emu v pipe módu bez programu (headless = bez audio
    # zařízení) a ověří, že při 100 % jde emulace reálným časem (cca 50
    # snímků/s, ne cca 1 snímek/s ani MAX SPEED), 200 % je dvojnásobné,
    # MAX SPEED není brzděný a návrat z MAX SPEED nedohání zameškaný čas.
    # RUN_SERIAL: měří reálný čas, souběh s jinými testy by ho zkresloval.
    add_test(
        NAME    mcp_headless_pacing
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_headless_pacing.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_headless_pacing PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
        RUN_SERIAL TRUE
    )
    message(STATUS "  tests/mcp: registered mcp_headless_pacing "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7b: CLI volba --speed <procenta|max> (headless + MCP TCP)
    # -----------------------------------------------------------
    # Spustí mz800emu --headless s --speed max / 200 / 50 (+ souběh s
    # --maxspeed-bench) a z počítadla snímků ověří tempo emulace; neplatné
    # hodnoty musí skončit chybou. MCP TCP port volný od systému (ne 23800).
    # RUN_SERIAL: měří reálný čas.
    add_test(
        NAME    mcp_cli_speed
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_speed.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cli_speed PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 120
        RUN_SERIAL TRUE
    )
    message(STATUS "  tests/mcp: registered mcp_cli_speed "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7b2: CLI volby --audio-capture, --audio-capture-frames (headless)
    # -----------------------------------------------------------
    # Spustí mz800emu --headless se zachycením zvuku na 60 snímků; ověří,
    # že emulátor sám skončí, soubory a capture.json (frames, sdl_blocks)
    # a výběr souborů z INI [AUDIO_CAPTURE]. MCP TCP port volný od systému.
    add_test(
        NAME    mcp_cli_audio_capture
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_audio_capture.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cli_audio_capture PROPERTIES
        LABELS  "e2e;mcp;cli"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_cli_audio_capture "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7c: stav stroje po CLI --run-mzf (headless + MCP TCP)
    # -----------------------------------------------------------
    # Pro mz700emu-pal, mz700emu-ntsc, mz800emu a mz1500emu (chybí-li jen
    # některá, test selže) spustí malé MZF přes --run-mzf, zastaví exec
    # breakpointem z .bpt a ověří stav, v jakém program spouští ROM
    # (IM 1, SP, 1038h, TEMPO, BPFLG, VRAM, CTC vč. latche CTC2 a předvolby
    # CTC0 po BEEP, PC0, PSG, paleta MZ-1500, BC/HL/IX) a pořadí inicializace
    # před nahráním MZF. MCP TCP port volný (ne 23800). Bez binárek 77 (SKIP).
    add_test(
        NAME    mcp_run_mzf_state
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_run_mzf_state.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_run_mzf_state PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 120
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_run_mzf_state "
                   "(Python ${Python3_VERSION})")

    # MCP media_run_mzf = stejný stav jako --run-mzf (všechny 4 binárky):
    # reference --run-mzf zastavená exec breakpointem proti media_run_mzf
    # v pauze nad "špinavým" strojem (IM 2, I, vektor Z80 PIO); chybný
    # soubor stroj nezresetuje; spuštění za běhu. MCP TCP port volný (ne
    # 23800). Bez binárek 77 (SKIP).
    add_test(
        NAME    mcp_media_run_mzf
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_media_run_mzf.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_media_run_mzf PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 200
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_media_run_mzf "
                   "(Python ${Python3_VERSION})")

    # HID nástroje počítají snímky emulace (input_send_key ze stavu pauzy
    # posune čítač snímků přesně o frames, při normal i MAX SPEED; za běhu
    # emu_frames = požadavek), run_until_addr a snapshot_load za běhu
    # vrací chybu s důvodem a emulaci nepauznou. Pipe, bez TCP. Bez
    # binárek 77 (SKIP).
    add_test(
        NAME    mcp_hid_frames
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_hid_frames.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_hid_frames PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 200
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_hid_frames "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7d: CMT + snapshot pořízený během přehrávání (pipe)
    # -----------------------------------------------------------
    # Čtyři procesy mz800emu v pipe módu (izolovaný --cfg-dir, INI s
    # vypnutým CMT hackem, vlastní vygenerované MZF): snapshot během
    # přehrávání, načtení bez pásky (transport STOP, run nepadá, stop ok),
    # načtení + cmt_open play_immediately (páska hraje od začátku, signál
    # se mění, odpověď cmt_open hlásí skutečný stav); navíc
    # playable/recordable ve výpisu pásky a MAX SPEED po načtení snapshotu
    # s cpu_boost (uživatelská MAX SPEED zůstává).
    add_test(
        NAME    mcp_cmt_snapshot
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cmt_snapshot.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cmt_snapshot PROPERTIES
        LABELS  "e2e;mcp;pipe;cmt"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_cmt_snapshot "
                   "(Python ${Python3_VERSION})")

    # CLI volby virtuální kazety --cmt, --cmt-play, --cmt-play-pause,
    # --cmt-boot (všechny 4 binárky): vložení pásky, vypnutí CMT hacku jen
    # pro běh (INI zůstane), PLAY / PLAY + PAUSE vůči hraně PC3 z inicializace
    # ROM, na MZ-800 nahrání a spuštění MZF z pásky přes IPL (klávesa C),
    # chybné kombinace. Pipe, varianty s --run-mzf přes MCP TCP (volný port
    # od systému). Bez binárek 77 (SKIP).
    add_test(
        NAME    mcp_cli_cmt
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_cmt.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cli_cmt PROPERTIES
        LABELS  "e2e;mcp;pipe;tcp;cmt"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_cli_cmt "
                   "(Python ${Python3_VERSION})")

    # CLI volby disketových mechanik --fdd, --fdd<N>, --fdd-readonly:
    # vložení do FDC0 i FDC1, nahrazení disku z INI, zápis cesty do INI,
    # read-only jen pro běh; chyby voleb (kód 1) a chyby při inicializaci
    # (odpojený FDC1, vadný obraz: headless kód 3). Pipe a headless.
    add_test(
        NAME    mcp_cli_fdd
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_fdd.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cli_fdd PROPERTIES
        LABELS  "e2e;mcp;pipe;fdc"
        TIMEOUT 300
    )
    message(STATUS "  tests/mcp: registered mcp_cli_fdd "
                   "(Python ${Python3_VERSION})")

    # Snapshot obnoví i čekající GDG událost (GitHub issue #6): stav po
    # snapshot_load přes "špinavý" stroj + run se shoduje s během bez
    # loadu (všechny 4 binárky, fáze s každou čekající GDG událostí);
    # dřív MZ-800 spadl (0xC0000005) a jinde ujel raster nebo CTC1.
    # Pipe, bez TCP. Bez binárek 77 (SKIP).
    add_test(
        NAME    mcp_snapshot_gdg_event
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_snapshot_gdg_event.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_snapshot_gdg_event PROPERTIES
        LABELS  "e2e;mcp;pipe;snapshot"
        TIMEOUT 180
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_snapshot_gdg_event "
                   "(Python ${Python3_VERSION})")

    # Stránkované čtení Trace Suite (trace_status / trace_read): krátký běh
    # jen v RAM čitelný za běhu i po stopu, raw_b64 = chunk soubory, čas
    # cputrack = kotvy chunků i s přijetím přerušení, segment_replaced po
    # restartu, marklog se jménem značky. MZ-800, pipe.
    add_test(
        NAME    mcp_trace_read
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_trace_read.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_trace_read PROPERTIES
        LABELS  "e2e;mcp;pipe;trace"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_trace_read "
                   "(Python ${Python3_VERSION})")

    # Paměť zachycení zvuku přes MCP: trace_read audio_* = CSV, PCM výřez
    # 44100 Hz stereo = WAV bajt po bajtu, souhrn (nota PSG, posun SDL cesty
    # o snímek), zachycení jen do paměti a segment_replaced. Pipe, MZ-800.
    add_test(
        NAME    mcp_audio_read
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_audio_read.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_audio_read PROPERTIES
        LABELS  "e2e;mcp;pipe;audio"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_audio_read "
                   "(Python ${Python3_VERSION})")

    # Záznam zápisů do paměti (ramlog) přes MCP: insn_pc, old/new, PUSH,
    # LDIR, dotazy kdo zapsal, malý ring, přijetí přerušení, banking RAM/VRAM
    # a plány WF REPLACE, reset a snapshot, bitová shoda se zapnutým záznamem,
    # MZ-700 a MZ-1500 (pokud binárky existují). Pipe.
    add_test(
        NAME    mcp_ramlog
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_ramlog.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_ramlog PROPERTIES
        LABELS  "e2e;mcp;pipe;ramlog"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_ramlog "
                   "(Python ${Python3_VERSION})")

    # Pokrytí kódu (coverage) přes MCP: stavy větví všech druhů podmíněných
    # instrukcí, úroveň detail (samomodifikace, DD 00, kapacita), vstup do
    # obsluhy přerušení, stránkování a kurzor, export JSON a CSV, bitová
    # shoda se zapnutým pokrytím, MZ-700 a MZ-1500 (pokud binárky existují).
    add_test(
        NAME    mcp_coverage
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_coverage.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_coverage PROPERTIES
        LABELS  "e2e;mcp;pipe;coverage"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_coverage "
                   "(Python ${Python3_VERSION})")

    # Klasifikace čtení paměti: datové čtení hned za instrukcí (POP ze
    # SP = PC, LD A,(nn) s nn = PC) je data - CDL r a zastavení MEM_R
    # breakpointu; MZ-800, MZ-700 a MZ-1500 (pokud binárky existují).
    add_test(
        NAME    mcp_mem_read_kind
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_mem_read_kind.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_mem_read_kind PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_mem_read_kind "
                   "(Python ${Python3_VERSION})")

    # Sběrnice volitelných zařízení (devbus, plán D1) bez zařízení:
    # get_devices, periph_* s id zařízení, settings_set, regions_list a zápis
    # na 0E010h / 0E00Ch na MZ-700 PAL a NTSC; get_devices na MZ-800.
    add_test(
        NAME    mcp_devbus_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_devbus_e2e.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_devbus_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 240
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_devbus_e2e "
                   "(Python ${Python3_VERSION})")

    # PCG-700 (HAL) jako zařízení devbus: program v RAM zapíše glyf kódu 80h
    # přes 0E010h-0E012h, kontrola bodů snímku, HSW NOM, SSW OFF, COPY
    # z CG-ROM, snapshot round-trip; MZ-700 PAL a NTSC, MZ-800 bez zařízení.
    add_test(
        NAME    mcp_pcg700_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_pcg700_e2e.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_pcg700_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe;pcg700"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_pcg700_e2e "
                   "(Python ${Python3_VERSION})")

    # 80sloupcový kit SUC (zařízení devbus suc80) na MZ-700 PAL a NTSC:
    # latch 0E00Ch, obraz 80 sloupců z get_frame_screenshot_raw, volba
    # atributů, reset, snapshot round-trip, chování bez zařízení.
    add_test(
        NAME    mcp_suc80_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_suc80_e2e.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_suc80_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe;suc80"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_suc80_e2e "
                   "(Python ${Python3_VERSION})")

    # 80sloupcová karta K&P (zařízení devbus kp80, plán D4): porty 70h-73h,
    # program v RAM, VRAM karty, device_screenshot druhého monitoru, fosfor,
    # překreslení v pauze, snapshot, reset; MZ-700 PAL a NTSC. Navíc všechna
    # tři zařízení MZ-700 naráz (PCG-700 + SUC + K&P) se snapshot round-trip.
    add_test(
        NAME    mcp_kp80_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_kp80_e2e.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_kp80_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe;kp80"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_kp80_e2e "
                   "(Python ${Python3_VERSION})")

    # Přístup debuggeru k portu zařízení devbus bez WAIT (plán D5, N-D5-5):
    # BP s podmínkou port[0x71]! a karta K&P s wait = 8 nemění časování
    # programu, io_read / io_write v pauze nemění čas.
    add_test(
        NAME    mcp_devbus_dbg_wait
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_devbus_dbg_wait.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_devbus_dbg_wait PROPERTIES
        LABELS  "e2e;mcp;pipe;kp80;devbus"
        TIMEOUT 150
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_devbus_dbg_wait "
                   "(Python ${Python3_VERSION})")

    # Regiony VRAM 700 po 2 KB (plán D5, N-D5-4): velikost, jméno, čtení
    # a zápis horní poloviny na MZ-700 PAL a MZ-1500.
    add_test(
        NAME    mcp_vram700_regions
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_vram700_regions.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_vram700_regions PROPERTIES
        LABELS  "e2e;mcp;pipe;regions"
        TIMEOUT 150
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_vram700_regions "
                   "(Python ${Python3_VERSION})")

    # Wrapper čte čísla [MCP] z INI šestnáctkově jako cfgfile (plán D5,
    # N-D5-1): rotace logu podle size_mb / keep z GUI. Bez emulátoru
    # a bez balíku mcp (funkce se berou ze zdroje přes ast).
    add_test(
        NAME    mcp_wrapper_ini_unsigned
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_wrapper_ini_unsigned.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_wrapper_ini_unsigned PROPERTIES
        LABELS  "mcp;wrapper"
        TIMEOUT 60
    )
    message(STATUS "  tests/mcp: registered mcp_wrapper_ini_unsigned "
                   "(Python ${Python3_VERSION})")

    # Znak se SHIFT v sekvenci HID: SHIFT o snímek dřív než klávesa (plán
    # D5, N-D5-2). Program v RAM čte SHIFT a klávesu v různých okamžicích
    # skenu; MZ-700 PAL.
    add_test(
        NAME    mcp_hid_shift
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_hid_shift.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_hid_shift PROPERTIES
        LABELS  "e2e;mcp;pipe;hid"
        TIMEOUT 150
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_hid_shift "
                   "(Python ${Python3_VERSION})")

    # Port I/O debuggeru v pauze neposune čas emulace (plán D5, N-D5-6):
    # io_read / io_write portu 74h na MZ-700 PAL, MZ-800 a MZ-1500.
    add_test(
        NAME    mcp_dbgapi_io_paused_time
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dbgapi_io_paused_time.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_dbgapi_io_paused_time PROPERTIES
        LABELS  "e2e;mcp;pipe;io"
        TIMEOUT 200
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_dbgapi_io_paused_time "
                   "(Python ${Python3_VERSION})")

    # Postupné hledání proměnných (memsearch) a Freeze Bytes přes MCP:
    # pauza i běžící program, undo/redo, ruční vyřazení, MemExt PEHU (128
    # kusů, bp_hint MMEXT_BANK), VRAM plane bez adresy, BCD, MZ-700/1500.
    add_test(
        NAME    mcp_memsearch
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_memsearch.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_memsearch PROPERTIES
        LABELS  "e2e;mcp;pipe;memsearch"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_memsearch "
                   "(Python ${Python3_VERSION})")

    # Běh ukončený podmínkou (headless): návratové kódy, result.json,
    # --exit-after-frames, --timeout-*, --exit-on-pc/--fail-on-pc/
    # --exit-on-write/--exit-on-out, --bpt, chyby voleb (kód 3), dopsání
    # artefaktů při nenulovém kódu, determinismus, všechny 4 binárky,
    # akce exit / stop v pipe a přes MCP TCP (jen pauza).
    add_test(
        NAME    mcp_headless_exit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_headless_exit.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_headless_exit PROPERTIES
        LABELS  "e2e;mcp;cli;automation"
        TIMEOUT 300
    )
    message(STATUS "  tests/mcp: registered mcp_headless_exit "
                   "(Python ${Python3_VERSION})")

    # Snapshot uloží a obnoví skrytý stav Z80 (WZ, čekající INT/NMI,
    # total_cycles); starý snapshot bez hidden_state nastaví výchozí
    # hodnoty resetu; snapshot hned po EI s aktivní linkou INT dá stejný
    # běh jako souvislý běh. Všechny 4 binárky, pipe. Bez binárek 77.
    add_test(
        NAME    mcp_snapshot_z80_hidden
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_snapshot_z80_hidden.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_snapshot_z80_hidden PROPERTIES
        LABELS  "e2e;mcp;pipe;snapshot"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_snapshot_z80_hidden "
                   "(Python ${Python3_VERSION})")

    # Snapshot z jiné HW konfigurace se načte bezpečně: RAMdisk 1 MB do
    # procesu s 64 kB (dřív přetečení bufferu, load se zasekl), IDE8
    # připojený bez otevřeného obrazu zůstane odpojený. MZ-800, pipe.
    add_test(
        NAME    mcp_snapshot_load_safety
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_snapshot_load_safety.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_snapshot_load_safety PROPERTIES
        LABELS  "e2e;mcp;pipe;snapshot"
        TIMEOUT 180
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_snapshot_load_safety "
                   "(Python ${Python3_VERSION})")

    # Snapshot obnoví latch WAIT VRAM režimu MZ-700: druhý zápis do VRAM
    # ve viditelné části řádku po načtení má stejný WAIT jako v souvislém
    # běhu (dřív build MZ-700 latch neukládal). Všechny 4 binárky, pipe.
    add_test(
        NAME    mcp_snapshot_vram_latch
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_snapshot_vram_latch.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_snapshot_vram_latch PROPERTIES
        LABELS  "e2e;mcp;pipe;snapshot"
        TIMEOUT 180
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_snapshot_vram_latch "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # MZT s hlavičkou CMTSPEED (UniCMT) a vlastní délky pulzů (pipe)
    # -----------------------------------------------------------
    # Páska LOADER + CMTSPEED + DATA: ROM z boot menu nahraje LOADER,
    # ten přes RHEAD/RDATA díl za hlavičkou. Hlavička 1x -> nahraje,
    # 3x -> ROM nenahraje, 3x s per-blok pulzy 1x přes MCP -> nahraje;
    # výchozí vlastní pulzy přes cmt_set_property custom_pulses.
    add_test(
        NAME    mcp_cmt_unicmt_mzt
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cmt_unicmt_mzt.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cmt_unicmt_mzt PROPERTIES
        LABELS  "e2e;mcp;pipe;cmt"
        TIMEOUT 300
    )
    message(STATUS "  tests/mcp: registered mcp_cmt_unicmt_mzt "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7d1: snapshot nepřepíše uživatelské INI (pipe)
    # -----------------------------------------------------------
    # Tři procesy mz800emu s ukládaným INI: A pořídí snapshot
    # s nevýchozími nastaveními, B0 je referenční běh bez snapshotu,
    # B1 snapshot načte. Preference (CMT, plnění MEMEXT, záloha Pezik)
    # se neobnoví, stav stroje (připojení a typy periferií, FW Unicardu,
    # druhý PSG) se obnoví jen pro běh; INI B1 po ukončení = INI B0.
    add_test(
        NAME    mcp_snapshot_ini
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_snapshot_ini.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_snapshot_ini PROPERTIES
        LABELS  "e2e;mcp;pipe;snapshot"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_snapshot_ini "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7d1b: zadní přepínač SW1 MZ-800 (INI + CLI, TCP)
    # -----------------------------------------------------------
    # --run-mzf s výchozím INI, [MZ800] mode_switch = MZ800,
    # --mode-switch=800 a INI MZ800 + --mode-switch=700: DMD a paleta
    # na exec adrese odpovídají módu, který ROM podle bitu 1 statusu
    # nastaví; --mode-switch=900 skončí chybou.
    add_test(
        NAME    mcp_mode_switch
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_mode_switch.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_mode_switch PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 120
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_mode_switch "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7d1c: get_periph_gdg - řadič VRAM (WF/RF) a HW scroll MZ-800
    # -----------------------------------------------------------
    # Program zapíše WF, RF a registry scrollu; odpověď nese objekty
    # vramctrl a hwscroll (scroll v jednotkách portu).
    add_test(
        NAME    mcp_periph_gdg_vramctrl
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_periph_gdg_vramctrl.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_periph_gdg_vramctrl PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 60
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_periph_gdg_vramctrl "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7d1d: screenshot_save_to_file - area, scale, crop
    # -----------------------------------------------------------
    # Rozměry PNG pro výchozí, canvas+native, scale 2 a ořez;
    # ořez mimo obraz je chyba parametrů.
    add_test(
        NAME    mcp_screenshot_options
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_screenshot_options.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_screenshot_options PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 60
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_screenshot_options "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7d1e: série screenshotů (screenshot_series_*)
    # -----------------------------------------------------------
    # Statický obraz + only_changed -> 1 soubor, všechny snímky -> N souborů,
    # druhý start a stop bez série jsou chyby.
    add_test(
        NAME    mcp_screenshot_series
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_screenshot_series_e2e.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_screenshot_series PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 90
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_screenshot_series "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7d2: rozsahové breakpointy (addr_end / addr_match_mode, pipe)
    # -----------------------------------------------------------
    # bp_create_with_init s addr_end bez RANGE vrátí warning (BP vznikne,
    # hlídá jen addr), s RANGE ne; bp_list ukazuje addr_end,
    # addr_match_mode a addr_mask.
    add_test(
        NAME    mcp_bp_addr_range
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_bp_addr_range.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_bp_addr_range PROPERTIES
        LABELS  "e2e;mcp;pipe;breakpoints"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_bp_addr_range "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Událost paused po zastavení na dočasném breakpointu (pipe)
    # -----------------------------------------------------------
    # run_until_addr, step_over přes CALL a step_out vyšlou paused
    # s reason "run_to"; uživatelský BP s nesplněnou podmínkou na cíli
    # dočasný BP nezakryje; uživatelský BP bez podmínky dá jedinou
    # událost (reason "breakpoint").
    add_test(
        NAME    mcp_run_to_paused_event
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_run_to_paused_event.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_run_to_paused_event PROPERTIES
        LABELS  "e2e;mcp;pipe;breakpoints"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_run_to_paused_event "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Krokování přes hranici snímku neztratí krok (pipe)
    # -----------------------------------------------------------
    # Blok konce snímku dřív vybíral frontu dbgapi i během kroku a další
    # STEP_INTO se ztratil: step_n posunul PC o krok méně na každé hranici
    # snímku. 20x step_n 1000 nad NOPy + jednotlivé step_into přes hranici.
    add_test(
        NAME    mcp_step_frame_boundary
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_step_frame_boundary.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_step_frame_boundary PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger"
        TIMEOUT 300
    )
    message(STATUS "  tests/mcp: registered mcp_step_frame_boundary "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Řízení běhu: čísla běhů, wait_stop, deadline, pause(if_run_seq) (pipe)
    # -----------------------------------------------------------
    # deadline v emulovaném čase (snímky, ms), determinismus délky běhu,
    # resumed + jediný paused, step_over přes CALL = běh, wait_stop timeout,
    # podmíněná pauza, přerušený run(frames) nezastaví další běh, historie.
    add_test(
        NAME    mcp_run_control
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_run_control.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_run_control PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_run_control "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # SHA-256 u čtení paměti, regionů a obrazu (pipe)
    # -----------------------------------------------------------
    # mem_read / region_read sha256 proti hashlib, want_data false,
    # frame_hash stabilní v pauze a shodný se screenshotem, změna po
    # zápisu do VRAM.
    add_test(
        NAME    mcp_read_sha256
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_read_sha256.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_read_sha256 PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_read_sha256 "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # state_observe - registry a okna paměti z jednoho okamžiku (pipe)
    # -----------------------------------------------------------
    # Za běhu konzistence registru a slova v paměti (kontrola, že dvě
    # samostatná volání neshodu zachytí), v pauze run_seq a hash okna
    # proti mem_read, okna regionů podle druhu, limity, sekce a hashe.
    add_test(
        NAME    mcp_state_observe
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_state_observe.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_state_observe PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_state_observe "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Podmíněné zápisy - expected_sha256 a expected (pipe + CLI)
    # -----------------------------------------------------------
    # mem_write, mem_write_force, region_write, set_register a
    # set_cpu_flags s podmínkou v pauze i za běhu; odmítnutý zápis se
    # nezaznamená do záznamu vstupů a --replay-verify skončí kódem 0.
    add_test(
        NAME    mcp_conditional_writes
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_conditional_writes.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_conditional_writes PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_conditional_writes "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Záznam a přehrání vstupů (pipe + headless CLI)
    # -----------------------------------------------------------
    # Záznam kláves a zápisů debuggeru, odmítnutí zásahu mimo hranici
    # snímku, přehrání v novém procesu se shodným stavem, --replay-verify
    # (kód 0, upravený záznam kód 4 na správném snímku), odchýlení.
    add_test(
        NAME    mcp_replay
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_replay.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_replay PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger;replay"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_replay "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Média v záznamu vstupů (kazeta, disk) přes MCP (pipe)
    # -----------------------------------------------------------
    # Záznam začne uprostřed hrající kazety (boot z CMT) s DSK v mechanice,
    # ovládání kazety přes MCP = události cmt; --replay-verify v novém
    # procesu bez pásky a disku kód 0, záznam bez pásky kód 4, originální
    # soubory beze změny.
    add_test(
        NAME    mcp_replay_media
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_replay_media.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_replay_media PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger;replay"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_replay_media "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Checkpointy záznamu vstupů: rewind a seek přes MCP (pipe)
    # -----------------------------------------------------------
    # Záznam v ROM monitoru s rewindem: zápis po checkpointu zmizí, soubor
    # má rerecord_count 1; --replay-verify v novém procesu kód 0; seek
    # dopředu / zpět / znovu dopředu dá stejný stav, rewind při přehrávání,
    # přehrávání doběhne bez rozjetí.
    add_test(
        NAME    mcp_replay_rewind
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_replay_rewind.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_replay_rewind PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger;replay"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_replay_rewind "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Záznam vstupů ukončený --exit-after-frames (CLI)
    # -----------------------------------------------------------
    # Smyčka IN = konec snímku uvnitř instrukce; pauza z callbacku konce
    # snímku nesmí posunout poslední bod hranice snímku: --replay-verify
    # záznamů různé délky v novém procesu kód 0.
    add_test(
        NAME    mcp_replay_exit_boundary
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_replay_exit_boundary.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_replay_exit_boundary PROPERTIES
        LABELS  "e2e;replay"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_replay_exit_boundary "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Regiony plné sestavy MZ-800 přes MCP (pipe)
    # -----------------------------------------------------------
    # MemExt Luftner + RAMdisk STD 16 MB + PEZIK 68h = 532 regionů:
    # regions_list vrátí všechny (dřív useknuto na 128), region_write
    # a region_read na ID nad 512 fungují, ID = count je chyba.
    add_test(
        NAME    mcp_regions_full_config
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_regions_full_config.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_regions_full_config PROPERTIES
        LABELS  "e2e;mcp;pipe;regions"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_regions_full_config "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Staré region_id po změně HW (pipe, dva procesy)
    # -----------------------------------------------------------
    # Snapshot s RAMdiskem 64K načtený do procesu s 16M: region_read a
    # region_write se starým ID skončí chybou "stale region id", ne
    # čtením za koncem bufferu RAMdisku (dřív access violation).
    add_test(
        NAME    mcp_regions_stale
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_regions_stale.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_regions_stale PROPERTIES
        LABELS  "e2e;mcp;pipe;regions"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_regions_stale "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Drain na hranici akce BP jen pro řídicí příkazy (pipe)
    # -----------------------------------------------------------
    # MEM_W BP s akcí continue: get_raster_pos za běhu se vykoná vždy na
    # hranici snímku (ne uprostřed instrukce), pauza projde, stav v pauze
    # je konzistentní.
    add_test(
        NAME    mcp_bp_action_drain
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_bp_action_drain.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_bp_action_drain PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_bp_action_drain "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Byte backstop akcí BP nezastaví samotný trace (pipe)
    # -----------------------------------------------------------
    # Práh 1 MB: trace přes MCP bez BP doběhne run 60 snímků, trace
    # spuštěný akcí BP backstop zastaví (stopped_by bp_action_saturation).
    add_test(
        NAME    mcp_trace_backstop
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_trace_backstop.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_trace_backstop PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger;trace"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_trace_backstop "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Seek zpět přes checkpoint s PSG v běhu (pipe)
    # -----------------------------------------------------------
    # Snapshot ukládá fázi krokování PSG: přehrání po seeku zpět přes
    # checkpoint se nerozjede na části periph (čítače kanálů PSG).
    add_test(
        NAME    mcp_replay_seek_psg
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_replay_seek_psg.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_replay_seek_psg PROPERTIES
        LABELS  "e2e;mcp;pipe;debugger;replay"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_replay_seek_psg "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Čtení paměti debuggerem nespouští MEM_R breakpoint (pipe)
    # -----------------------------------------------------------
    # mem_read a dasm přes adresu s MEM_R BP nezmění hits ani nevyšlou
    # breakpoint_hit; čtení CPU (LD A,(nn)) BP dál spustí.
    add_test(
        NAME    mcp_debugger_read_no_mem_r_bp
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_debugger_read_no_mem_r_bp.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_debugger_read_no_mem_r_bp PROPERTIES
        LABELS  "e2e;mcp;pipe;breakpoints"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_debugger_read_no_mem_r_bp "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # IRQ / NMI inject bez HW linky INT (pipe)
    # -----------------------------------------------------------
    # irq_inject při EI skočí do obsluhy IM 2, nmi_inject při IFF1 = 0 na
    # 0066h, irq_inject při DI čeká do EI.
    add_test(
        NAME    mcp_irq_nmi_inject
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_irq_nmi_inject.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_irq_nmi_inject PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_irq_nmi_inject "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # InsnPC a FrameCycle ve výrazech breakpointů (pipe)
    # -----------------------------------------------------------
    # MEM_W BP s InsnPC zastaví na zápisu dané instrukce, PC je u MEM_W
    # za instrukcí; FrameCycle = Cycle - Frame * takty snímku.
    add_test(
        NAME    mcp_bp_expr_insnpc
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_bp_expr_insnpc.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_bp_expr_insnpc PROPERTIES
        LABELS  "e2e;mcp;pipe;breakpoints"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_bp_expr_insnpc "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Výstup akcí BP log a stop přes MCP (kanál bplog, pipe)
    # -----------------------------------------------------------
    # MEM_W BP s log ve smyčce, PC_EXEC BP se stop, filtry, přetečení
    # ringu (gap, lost) a bplog_clear (nový segment).
    add_test(
        NAME    mcp_bplog
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_bplog.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_bplog PROPERTIES
        LABELS  "e2e;mcp;pipe;breakpoints"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_bplog "
                   "(Python ${Python3_VERSION})")

    # Jméno breakpointu přes MCP vydrží bez auto_name (N66): vytvoření,
    # změna adresy, bp_update jména, auto_name true; čte se z bplog (pipe).
    add_test(
        NAME    mcp_bp_name
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_bp_name.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_bp_name PROPERTIES
        LABELS  "e2e;mcp;pipe;breakpoints"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_bp_name "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Zápis paměti debuggerem nespouští MEM_W breakpoint (pipe)
    # -----------------------------------------------------------
    # mem_write a region_write (Logical) přes adresu s MEM_W BP nezmění
    # hits ani nevyšlou breakpoint_hit; zápis CPU (LD (nn),A) BP spustí.
    add_test(
        NAME    mcp_debugger_write_no_mem_w_bp
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_debugger_write_no_mem_w_bp.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_debugger_write_no_mem_w_bp PROPERTIES
        LABELS  "e2e;mcp;pipe;breakpoints"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_debugger_write_no_mem_w_bp "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Kanál marklog přes trace_* (pipe)
    # -----------------------------------------------------------
    # trace_start/reset/save/stop přijmou channel "marklog"; BP s akcí
    # mark zapíše manifest a neprázdný chunk do adresáře z INI.
    add_test(
        NAME    mcp_trace_marklog
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_trace_marklog.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_trace_marklog PROPERTIES
        LABELS  "e2e;mcp;pipe;trace"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_trace_marklog "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Cputrack: bajty instrukce z fetch, ne z paměti po vykonání (pipe)
    # -----------------------------------------------------------
    # LD (3001h),A na 3001h přepíše vlastní opkód; záznam musí mít
    # délku 3 a bajty 32 01 30 (dřív 1 bajt 00h = NOP).
    add_test(
        NAME    mcp_cputrack_insn_bytes
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cputrack_insn_bytes.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cputrack_insn_bytes PROPERTIES
        LABELS  "e2e;mcp;pipe;trace"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_cputrack_insn_bytes "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Cputrack formát v3: registry po / před instrukcí, synchronizace
    # po zápisu registru, filtr podle registru, trace_status (pipe)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_cputrack_regs
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cputrack_regs.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cputrack_regs PROPERTIES
        LABELS  "e2e;mcp;pipe;trace"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_cputrack_regs "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Cputrack: čas záznamů přes hranice chunků a po skoku času
    # (načtení snapshotu), formát v2 i v3 (pipe)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_cputrack_time
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cputrack_time.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cputrack_time PROPERTIES
        LABELS  "e2e;mcp;pipe;trace"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_cputrack_time "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # run_until_tstate s target_ticks: zastavení na čase GDG v emu
    # vlákně, přesný začátek instrukce, breakpoint před cílem (pipe)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_run_until_ticks
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_run_until_ticks.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_run_until_ticks PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_run_until_ticks "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Pauza a rozběh nemění okamžik přijetí přerušení (zastavení na
    # HALT po EI při trvalém INT, porovnání s během bez pauzy, pipe)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_pause_int_timing
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_pause_int_timing.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_pause_int_timing PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_pause_int_timing "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # replay_goto: stav v čase přehrávání, křížová kontrola s trace
    # cputrack v3 (přerušení, LDIR), breakpointy, checkpoint (pipe)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_replay_goto
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_replay_goto.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_replay_goto PROPERTIES
        LABELS  "e2e;mcp;pipe;replay"
        TIMEOUT 240
    )
    message(STATUS "  tests/mcp: registered mcp_replay_goto "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Watch nemění stav stroje ani s `!` (watch_eval, watch_list, pipe)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_watch_no_side_effects
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_watch_no_side_effects.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_watch_no_side_effects PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_watch_no_side_effects "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Revize stavu state_rev: čtení nemění, zápisy a běh zvednou,
    # expected_state_rev, if_state_rev_differs, záznam vstupů (pipe)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_state_rev
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_state_rev.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_state_rev PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 180
    )
    message(STATUS "  tests/mcp: registered mcp_state_rev "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Otisk stroje machine_hash a úplnost revize stavu (pipe): u všech
    # nástrojů druhu MUTATES "změnil se otisk => změnila se state_rev",
    # čtecí nástroje nemění otisk ani revizi, ABA.
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_machine_hash
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_machine_hash.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_machine_hash PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 200
    )
    message(STATUS "  tests/mcp: registered mcp_machine_hash "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Quick Disk přes media_insert (pipe): neexistující obraz beze změny,
    # přepnutí na MZ-1F11 se ohlásí, ro, eject.
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_media_qd
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_media_qd.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_media_qd PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_media_qd "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Periferie a nastavení za běhu (M21): media_state qd ro efektivní,
    # periph_attach/detach za běhu, settings_set do proměnné a do INI
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_periph_settings_live
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_periph_settings_live.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_periph_settings_live PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_periph_settings_live "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # get_memory_map a memory_map ve state_observe podle mapování (N94)
    # -----------------------------------------------------------
    add_test(
        NAME    mcp_memory_map_kind
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_memory_map_kind.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_memory_map_kind PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 90
    )
    message(STATUS "  tests/mcp: registered mcp_memory_map_kind "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7e: port I/O a načtení MZF přes MCP za běhu emulace (pipe)
    # -----------------------------------------------------------
    # Regrese: io_write/io_read za běhu zastavil CPU a další port I/O
    # zablokoval MCP na ~240 s (drain dbgapi v callbacku konce snímku,
    # konec snímku zpracovaný podruhé). Pro mz700emu-pal, mz700emu-ntsc,
    # mz800emu a mz1500emu (chybí-li jen některá, test selže): io za běhu,
    # media_load_mzf + 0E0h/0E1h + PC za běhu, io v pauze na hranici
    # snímku, io za běhu programu s I/O ve smyčce. Bez binárek 77 (SKIP).
    add_test(
        NAME    mcp_dbgapi_io_running
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_dbgapi_io_running.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_dbgapi_io_running PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_dbgapi_io_running "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7f: CMT hack bez GUI nezablokuje emulační vlákno (pipe)
    # -----------------------------------------------------------
    # Regrese: se zapnutým CMT hackem OUT (01h) z ROM (boot menu MZ-800,
    # klávesa C) otevřel v headless blokující dialog výběru MZF, EMU vlákno
    # čekalo navždy a MCP vracelo "Emulator busy". Pro mz700emu-pal,
    # mz700emu-ntsc, mz800emu a mz1500emu (chybí-li jen některá, test
    # selže): OUT (01h) z programu v RAM, na MZ-800 navíc přesné posloupnosti
    # z bugreportu (klávesa C, cmt_open + play). Bez binárek 77 (SKIP).
    add_test(
        NAME    mcp_cmthack_headless
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cmthack_headless.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_cmthack_headless PROPERTIES
        LABELS  "e2e;mcp;pipe;cmt"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_cmthack_headless "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7b: omezená odpověď MCP při zaseknutém emu vlákně
    # -----------------------------------------------------------
    # Testovací háček (proměnné prostředí MZ800EMU_TEST_STALL_*) zasekne
    # emu vlákno uprostřed get_registers; MCP musí odpovědět
    # "Emulator busy" do ~20 s, ping musí projít hned a emulátor se po
    # zaseknutí zotaví. Pipe, žádný TCP port.
    add_test(
        NAME    mcp_busy_stall
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_busy_stall.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_busy_stall PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 150
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_busy_stall "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 8: GUI bez audio zařízení (video-capture Task 23)
    # -----------------------------------------------------------
    # Spustí GUI mz800emu (otevře okno) s neexistujícím SDL_AUDIO_DRIVER
    # a volným MCP TCP portem; ověří start, varování v logu, tempo cca 50
    # snímků/s, MAX SPEED a zvuk video záznamu podle reality bez zařízení.
    # Kvůli oknu jen na vyžádání: bez MZ_GUI_TESTS=1 vrací 77 (SKIP).
    add_test(
        NAME    mcp_gui_no_audio
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_gui_no_audio.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_gui_no_audio PROPERTIES
        LABELS  "e2e;mcp;tcp;gui"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_gui_no_audio "
                   "(Python ${Python3_VERSION}, needs MZ_GUI_TESTS=1)")
else()
    message(STATUS "  tests/mcp: mcp_pipe_e2e SKIPPED "
                   "(Python3 interpreter not found)")
    message(STATUS "  tests/mcp: mcp_server_stdio_e2e SKIPPED "
                   "(Python3 interpreter not found)")
endif()
