# cmake/AddMzTest.cmake
#
# Test framework pro Sharp MZ-800/700/1500 emulátor.
#
# Používá Unity (C unit test framework) + vlastní mztest wrapper a poskytuje
# 4 typy test executables:
#   1. STANDALONE - jen testovaná knihovna + Unity (např. iasm, cmtspeed)
#   2. LIB        - knihovna + emulator core + headless stuby (cfgfile, dsk, ...)
#   3. EMU        - full emulator core + stuby (sanity, snapshot, peripherals)
#   4. UI         - full + ImGui + SDL3 (smoke, functional, visual)
#
# Po include() je dostupné:
#   - target mz_test_framework (STATIC, Unity + mztest)
#   - target mz_test_stubs (STATIC, headless stuby pro video/audio/UI)
#   - target mz_test_core_mz800 (OBJECT, emulator core s MZARCH=800 + MZTEST_HEADLESS)
#   - function mz_add_test(<name> TYPE <type> SOURCES <files>)
#
# Volání:
#   enable_testing() v top-level CMakeLists.txt PŘED include AddMzTest.cmake
#   add_subdirectory(tests) PO include
#
# Per-arch: zatím jen MZ800 (TEST_MZARCH=800). Rozšíření na 700/1500 by vyžadovalo
# samostatné mz_test_core_mz<arch> OBJECT libraries (per arch -DMZARCH).

if(NOT BUILD_TESTING)
    return()
endif()

set(MZ_TEST_DIR ${CMAKE_SOURCE_DIR}/tests)
set(MZ_TEST_FW_DIR ${MZ_TEST_DIR}/framework)

# ----------------------------------------------------------------------------
# mz_test_framework - Unity + mztest (sdílené přes všechny testy)
# ----------------------------------------------------------------------------
add_library(mz_test_framework STATIC
    ${MZ_TEST_FW_DIR}/unity.c
    ${MZ_TEST_FW_DIR}/mztest.c
)
target_compile_definitions(mz_test_framework PUBLIC
    MZTEST_HEADLESS
    UNITY_INCLUDE_DOUBLE
    MZARCH=800
    MZARCH_NAME="mz800"
)
target_include_directories(mz_test_framework PUBLIC
    ${MZ_TEST_FW_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/emulator
    ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic
)
# Headers adresáře (src/**/headers) - main.h transitivně includuje hodně věcí
# POZN: GLOB_RECURSE s LIST_DIRECTORIES true ignoruje pattern a vraci vsechny
# podadresare pod src/, proto musime filtrovat (jinak by se zaradil i Windows
# shim src/libs/igfd/dirent ktery na Linuxu pada).
file(GLOB_RECURSE _fw_header_dirs LIST_DIRECTORIES true
    "${CMAKE_SOURCE_DIR}/src/*/headers"
)
list(FILTER _fw_header_dirs INCLUDE REGEX "/headers$")
foreach(_hd IN LISTS _fw_header_dirs)
    if(IS_DIRECTORY ${_hd})
        target_include_directories(mz_test_framework PUBLIC ${_hd})
    endif()
endforeach()
# main.h → app.h → <glib.h>; audio.h → SDL3 headers - tranzitivně potřeba
target_link_libraries(mz_test_framework PUBLIC mz::glib mz::sdl3 mz::platform)
set_target_properties(mz_test_framework PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build-tests
)

# ----------------------------------------------------------------------------
# mz_test_stubs - headless stuby pro video/audio/UI symboly
# ----------------------------------------------------------------------------
add_library(mz_test_stubs STATIC
    ${MZ_TEST_FW_DIR}/stubs/iface_video_stub.c
    ${MZ_TEST_FW_DIR}/stubs/iface_audio_stub.c
    ${MZ_TEST_FW_DIR}/stubs/app_stubs.c
)
target_compile_definitions(mz_test_stubs PUBLIC
    MZTEST_HEADLESS
    MZARCH=800
    MZARCH_NAME="mz800"
)
target_include_directories(mz_test_stubs PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/emulator
    ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic
)
# Headers adresáře (src/**/headers) - viz pozn. u mz_test_framework vyse.
file(GLOB_RECURSE _stubs_header_dirs LIST_DIRECTORIES true
    "${CMAKE_SOURCE_DIR}/src/*/headers"
)
list(FILTER _stubs_header_dirs INCLUDE REGEX "/headers$")
foreach(_hd IN LISTS _stubs_header_dirs)
    if(IS_DIRECTORY ${_hd})
        target_include_directories(mz_test_stubs PUBLIC ${_hd})
    endif()
endforeach()
target_link_libraries(mz_test_stubs PUBLIC mz::glib mz::sdl3 mz::platform)
set_target_properties(mz_test_stubs PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build-tests
)

# ----------------------------------------------------------------------------
# mz_test_core_mz800 - emulator core OBJECT library (MZARCH=800, MZTEST_HEADLESS)
#
# Sběr zdrojů odpovídá mz_add_emulator() v AddMzEmu.cmake, ale BEZ:
#   - main.c (testy mají vlastní main z Unity)
#   - src/iface-video/, src/iface-audio/ (stubuje mz_test_stubs)
#   - src/ui-imgui/, src/ui/, src/baseui/ (stubuje mz_test_stubs)
#   - src/version_check/ (stubuje mz_test_stubs)
#   - src/iface/iface_joy.c (stubuje mz_test_stubs)
#   - windows_rc/ (žádné GUI resource)
#   - build_revision.c (test nepoužívá)
# ----------------------------------------------------------------------------
mz_glob_sources(_test_emu_sources
    src/emulator/hw-generic
    src/emulator/debugger
    src/emulator/snapshot
    src/emulator/videorec
    src/emulator/mzarch/mz800
    src/emulator/mcp
)
# main_pipe.c (V0.A.4) má vlastní main() - kolize s Unity testovacím
# main, ani v EMU testovacím prostředí ho nebuildíme.
list(FILTER _test_emu_sources EXCLUDE REGEX "/src/emulator/mcp/main_pipe\\.c$")
mz_glob_flat(_test_emu_flat_emu      src/emulator)
mz_glob_flat(_test_emu_flat_mzarch   src/emulator/mzarch)

# fs_layer.c, time_profiler.c z root src/ - core potřebuje
list(APPEND _test_emu_sources
    ${CMAKE_SOURCE_DIR}/src/fs_layer.c
    ${CMAKE_SOURCE_DIR}/src/time_profiler.c
)

# Generic driver subset z iface/ (bez iface_joy.c, iface_video, iface_audio)
list(APPEND _test_emu_sources
    ${CMAKE_SOURCE_DIR}/src/iface/iface.c
    ${CMAKE_SOURCE_DIR}/src/iface/iface_audio.c
    ${CMAKE_SOURCE_DIR}/src/iface/iface_audio_resampler.c
    ${CMAKE_SOURCE_DIR}/src/iface/iface_keyboard.c
    ${CMAKE_SOURCE_DIR}/src/iface/iface_keyboard_event.c
    ${CMAKE_SOURCE_DIR}/src/iface/iface_video.c
)

# baseui/baseui_tools.c - obsahuje pomocné funkce které emu potřebuje
# (baseui.c a baseui_filechooser.c jsou GUI - stubujeme).
list(APPEND _test_emu_sources
    ${CMAKE_SOURCE_DIR}/src/baseui/baseui_tools.c
)

# generic_driver/ - file/memory driver
list(APPEND _test_emu_sources
    ${CMAKE_SOURCE_DIR}/src/generic_driver/file_driver.c
    ${CMAKE_SOURCE_DIR}/src/generic_driver/memory_driver.c
)

# app/ je headers-only (app.h, app_main.h, app_thread.h) - žádné .c

add_library(mz_test_core_mz800 OBJECT
    ${_test_emu_sources}
    ${_test_emu_flat_emu}
    ${_test_emu_flat_mzarch}
)
target_compile_definitions(mz_test_core_mz800 PUBLIC
    MZARCH=800
    MZARCH_NAME="mz800"
    MZTVSYS_PAL=50
    MZTVSYS_NTSC=60
    MZTVSYS=MZTVSYS_PAL
    MZTVSYS_NAME="PAL"
    MZTEST_HEADLESS
)
target_include_directories(mz_test_core_mz800 PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/emulator
    ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic
)

# Headers adresáře z src/**/headers (jako v AddMzEmu.cmake) - viz pozn. vyse
file(GLOB_RECURSE _test_header_dirs LIST_DIRECTORIES true
    "${CMAKE_SOURCE_DIR}/src/*/headers"
)
list(FILTER _test_header_dirs INCLUDE REGEX "/headers$")
foreach(_hd IN LISTS _test_header_dirs)
    if(IS_DIRECTORY ${_hd})
        target_include_directories(mz_test_core_mz800 PUBLIC ${_hd})
    endif()
endforeach()

# Závislosti - core volá GLib funkce přímo, takže jejich headers musí být dostupné.
# mz::platform poskytuje WINDOWS/LINUX defines, které ovlivňují conditional kompilaci
# (např. wd279x.c bez WINDOWS by zapnul FS_LAYER_FATFS s ff.h dependence).
target_link_libraries(mz_test_core_mz800 PUBLIC
    mz::glib mz::json_glib mz::minizip mz::sdl3 mz::platform
)

# ----------------------------------------------------------------------------
# mz_add_test(<name>
#     TYPE <STANDALONE|LIB|EMU|UI>
#     SOURCES <files...>
#     [LIBS <library_targets...>]    # pro STANDALONE/LIB - explicit lib targets
#     [ARGS <test_args>]              # CLI args testovaného programu
#     [LABELS <ctest_labels>]         # CTest labels pro filtrování
# )
#
# Vytvoří test executable + add_test() registraci do CTest.
# Binárka jde do build-tests/, pojmenování test_<name>.
# ----------------------------------------------------------------------------
function(mz_add_test name)
    cmake_parse_arguments(ARG ""
        "TYPE"
        "SOURCES;LIBS;ARGS;LABELS"
        ${ARGN})

    if(NOT ARG_TYPE)
        message(FATAL_ERROR "mz_add_test(${name}): TYPE je povinný (STANDALONE|LIB|EMU|UI)")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "mz_add_test(${name}): SOURCES jsou povinné")
    endif()

    # Debugger-závislé testy (label "debugger") nelze přeložit bez debugger
    # subsystému: jejich API (watch_cache, sym_db, freeze, dasm_export, ...) je
    # schované za #ifdef MZ800EMU_CFG_DEBUGGER_ENABLED v hlavičkách. Při
    # MZ_NO_DEBUGGER registraci přeskočíme (stejný princip jako MZ_NO_MCP guard
    # v tests/mcp/CMakeLists.txt), takže `make test` prochází i v NO_DEBUGGER
    # buildu - debugger testy se v něm jen vynechají, neběží jako fail.
    if(MZ_NO_DEBUGGER AND ARG_LABELS MATCHES "(^|;)debugger(;|$)")
        message(STATUS "  test ${name}: skipped (MZ_NO_DEBUGGER=ON, debugger-dependent)")
        return()
    endif()

    set(target test_${name})
    add_executable(${target} ${ARG_SOURCES})

    target_compile_definitions(${target} PRIVATE
        MZTEST_HEADLESS
        MZARCH=800
        MZARCH_NAME="mz800"
    )

    # Framework (Unity + mztest) je vždy potřeba
    target_link_libraries(${target} PRIVATE mz_test_framework)

    # Per-typ link sada
    if(ARG_TYPE STREQUAL "STANDALONE")
        # Jen testovaná knihovna - ARG_LIBS musí být explicitní
        if(ARG_LIBS)
            target_link_libraries(${target} PRIVATE ${ARG_LIBS})
        endif()

    elseif(ARG_TYPE STREQUAL "LIB" OR ARG_TYPE STREQUAL "EMU")
        # Knihovní/emu testy potřebují celý emulator core + stuby
        target_link_libraries(${target} PRIVATE
            mz_test_core_mz800
            mz_test_stubs
            mz::libs
            mz::sdl3   # snapshot_screenshot používá SDL_Surface
            mz::glib
            mz::json_glib
            mz::minizip
            mz::platform
            stdc++
        )
        if(ARG_LIBS)
            target_link_libraries(${target} PRIVATE ${ARG_LIBS})
        endif()

    elseif(ARG_TYPE STREQUAL "UI")
        # UI testy: emu core + ImGui + SDL3 + OpenGL (žádné stuby pro UI)
        target_link_libraries(${target} PRIVATE
            mz_test_core_mz800
            mz_test_stubs   # iface_video/audio stuby pořád potřeba
            mz::libs
            mz::sdl3
            mz::glib
            mz::json_glib
            mz::minizip
            mz::opengl
            mz::platform
            stdc++
        )
        if(ARG_LIBS)
            target_link_libraries(${target} PRIVATE ${ARG_LIBS})
        endif()

    else()
        message(FATAL_ERROR "mz_add_test(${name}): neznámý TYPE=${ARG_TYPE}")
    endif()

    # Output do build-tests/
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build-tests
    )

    # CTest registrace
    add_test(NAME ${name}
        COMMAND ${target} ${ARG_ARGS}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/build-tests
    )

    # Na Windows test binárky potřebují toolchain/bin v PATH pro DLL load
    # (SDL3.dll, glib-2.0.dll, libstdc++ atd.). Bereme z CMAKE_C_COMPILER cesty.
    if(WIN32)
        get_filename_component(_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "PATH=${_cc_dir};$ENV{PATH}"
        )
    endif()

    if(ARG_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}")
    endif()
endfunction()
