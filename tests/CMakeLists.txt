# tests/CMakeLists.txt
#
# Root testovacího adresáře. Definice individuálních testů jsou rozdělené
# podle skupin do per-folder CMakeLists.txt.
#
# Test typy (viz cmake/AddMzTest.cmake pro mz_add_test()):
#   STANDALONE - jen lib + Unity (žádné emu core)
#   LIB        - lib + emu core + headless stuby
#   EMU        - full emu core + stuby
#   UI         - full + ImGui + SDL3 + OpenGL
#
# Po build je dostupné:
#   ctest --test-dir build               (všechny testy)
#   ctest --test-dir build -L lib         (jen LIB testy)
#   ctest --test-dir build -R cfgfile    (regex match)
#   make test                            (= ctest)

# Standalone (lib + Unity)
add_subdirectory(libs/iasm)
add_subdirectory(libs/cmtspeed)
add_subdirectory(libs/cpu_z80)
add_subdirectory(libs/sdlapp)
add_subdirectory(libs/z80meta)
add_subdirectory(libs/dasm_sym)
add_subdirectory(libs/dasm_h_suffix_sym)
add_subdirectory(libs/dasm_export)
add_subdirectory(libs/dasm_motorola_ix)
add_subdirectory(libs/psg_scope)
add_subdirectory(libs/mzglyphs)
add_subdirectory(libs/mcs48)
add_subdirectory(libs/mz1p16)

# Lib (lib + emu core + stuby)
add_subdirectory(libs/cfgfile)
add_subdirectory(libs/cmt_stream)
add_subdirectory(libs/dsk)
add_subdirectory(libs/generic_driver)
add_subdirectory(libs/mzf)
add_subdirectory(libs/mztape)
add_subdirectory(libs/vram_sim)
add_subdirectory(libs/wav)
add_subdirectory(libs/zxtape)

# Emu core
add_subdirectory(sanity)
add_subdirectory(debugger)
add_subdirectory(memory)
add_subdirectory(peripherals)
add_subdirectory(snapshot)

# UI
add_subdirectory(ui)

# MCP backend (V0.A.2+) - standalone testy; tests/mcp/CMakeLists.txt
# sám rozhoduje (MZ_NO_MCP guard) jestli se zaregistrují.
add_subdirectory(mcp)

# ---------------------------------------------------------------------------
# CI gate: zamrazeny preprocessor stav mzhal konverze (krok 13)
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME    preproc_state_gate
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/ci/check_preproc_state.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(preproc_state_gate PROPERTIES
        LABELS  "ci;preproc"
        TIMEOUT 60
    )
    message(STATUS "  tests: registered preproc_state_gate")
endif()
