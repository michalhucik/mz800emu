# CMakeLists.txt - Sharp MZ-800/MZ-700/MZ-1500 emulátor
#
# Top-level konfigurace. Buildovací logika je rozdělená do:
#   cmake/Dependencies.cmake   - externí knihovny (SDL3, GLib, ...)
#   cmake/BuildRevision.cmake  - generování build_revision.c
#   cmake/WindowsRC.cmake      - kompilace .rc souborů na Windows
#   cmake/AddMzEmu.cmake       - mz_add_emulator() function pro per-arch exec
#   src/libs/CMakeLists.txt    - per-knihovna add_library(STATIC)
#
# Build adresáře v ${CMAKE_BINARY_DIR}:
#   build-libs/                - .a archivy ze src/libs/
#   build-mz800emu/            - mz800emu.exe        (MZ-800 PAL)
#   build-mz1500emu/           - mz1500emu.exe       (MZ-1500 NTSC)
#   build-mz700emu-pal/        - mz700emu-pal.exe    (MZ-700 PAL = EU)
#   build-mz700emu-ntsc/       - mz700emu-ntsc.exe   (MZ-700 NTSC = JP)
#
# Volby:
#   -DCMAKE_BUILD_TYPE=Debug|Release  (default: Debug)
#   -DMZ_FORCE_CONSOLE=ON             (Windows: zachová console subsystem)

cmake_minimum_required(VERSION 3.20)

project(mz800new
    VERSION 1.0.0
    DESCRIPTION "Sharp MZ-800/MZ-700/MZ-1500 emulator"
    LANGUAGES C CXX
)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

# ----------------------------------------------------------------------------
# Display jméno + verze projektu (= co se vytiskne v configure status reportu)
#
# Jméno: primárně z -DMZ_PROJECT_NAME (předané z Makefile), jinak fallback
# na jméno adresáře projektu. Tento string nesouvisí s CMake PROJECT_NAME,
# který zůstává "mz800new" (= internal repository name).
#
# Verze: parsuje se ze src/emulator/cfgmain.h (single source of truth pro
# verzi emulátoru). Display string je "<NUM_STRING> <TAG>", např.
# "2.0.1 preview". Regex odmítá zakomentované varianty "//#define ...".
# ----------------------------------------------------------------------------
if(NOT DEFINED MZ_PROJECT_NAME OR MZ_PROJECT_NAME STREQUAL "")
    get_filename_component(MZ_PROJECT_NAME "${CMAKE_SOURCE_DIR}" NAME)
endif()

file(STRINGS "${CMAKE_SOURCE_DIR}/src/emulator/cfgmain.h" _mz_ver_num_line
    REGEX "^[ \t]*#define[ \t]+CFGMAIN_EMULATOR_VERSION_NUM_STRING[ \t]+\"[^\"]+\"")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1"
    MZ_DISPLAY_VERSION_NUM "${_mz_ver_num_line}")

file(STRINGS "${CMAKE_SOURCE_DIR}/src/emulator/cfgmain.h" _mz_ver_tag_line
    REGEX "^[ \t]*#define[ \t]+CFGMAIN_EMULATOR_VERSION_TAG[ \t]+\"[^\"]+\"")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1"
    MZ_DISPLAY_VERSION_TAG "${_mz_ver_tag_line}")

set(MZ_DISPLAY_VERSION "${MZ_DISPLAY_VERSION_NUM} ${MZ_DISPLAY_VERSION_TAG}")

# ----------------------------------------------------------------------------
# Volby
# ----------------------------------------------------------------------------
option(MZ_FORCE_CONSOLE "Windows: zachovat console subsystem (vypne -mwindows v SDL3 link line)" OFF)
option(MZ_BUILD_MZ800       "Build mz800emu (MZ-800 PAL)" ON)
option(MZ_BUILD_MZ1500      "Build mz1500emu (MZ-1500 NTSC)" ON)
option(MZ_BUILD_MZ700_PAL   "Build mz700emu-pal (MZ-700 PAL = EU)" ON)
option(MZ_BUILD_MZ700_NTSC  "Build mz700emu-ntsc (MZ-700 NTSC = JP)" ON)
option(BUILD_TESTING    "Build test suite (Unity + CTest)" ON)

# ----------------------------------------------------------------------------
# FDC diagnostic trace (volitelné)
#
# FDC_DIAG - povolí diagnostické fprintf v chip emulaci.
# Zapni přes `make FDC_DIAG=1` při ladění (LOADED/FLUSHED sektory,
# WRITE TRACK state machine trace, DSK header repair).
# ----------------------------------------------------------------------------
option(FDC_DIAG "Enable diagnostic fprintf in FDC implementation" OFF)
if(FDC_DIAG)
    add_compile_definitions(FDC_DIAG=1)
    message(STATUS "  FDC_DIAG:      ON (verbose chip emulation trace)")
endif()

# ----------------------------------------------------------------------------
# RAM-access fast-path (E1, KROK 1 navrhu D3) - volitelne
#
# MZ_RAM_FASTPATH - zapne inline page-table fast-path pro cista RAM cteni/
#   zapisy v Z80 jadre (definuje MZ800EMU_CFG_RAM_FASTPATH globalne, takze ho
#   vidi i sdilena knihovna cpu-z80). Defaultne ON; OFF = A5 baseline (vzdy callback).
#
# MZ_RAM_FASTPATH_VERIFY - DEBUG-only: kazdy RAM pristup provede ZAROVEN
#   fast-path I puvodni callback a ASSERTuje shodu (hodnota + regDBUS_latch).
#   Dokazuje ekvivalenci fast-path == callback na realnem behu. Implikuje
#   MZ_RAM_FASTPATH. Velmi pomale - jen pro dukaz presnosti, ne pro perf.
#
# Defaultne ON (zabankovany +11.6-15.6% zisk). Vypnout pres
# `cmake -DMZ_RAM_FASTPATH=OFF` (A5 baseline), VERIFY pres -DMZ_RAM_FASTPATH_VERIFY=ON.
# ----------------------------------------------------------------------------
option(MZ_RAM_FASTPATH        "Enable RAM-access fast-path in Z80 core (E1)" ON)
option(MZ_RAM_FASTPATH_VERIFY "Diff-verify fast-path vs callback (DEBUG, slow)" OFF)

if(MZ_RAM_FASTPATH_VERIFY AND NOT MZ_RAM_FASTPATH)
    message(STATUS "  MZ_RAM_FASTPATH: forced ON (cascade from VERIFY)")
    set(MZ_RAM_FASTPATH ON CACHE BOOL "Enable RAM fast-path (forced by VERIFY)" FORCE)
endif()

if(MZ_RAM_FASTPATH)
    add_compile_definitions(MZ800EMU_CFG_RAM_FASTPATH)
    message(STATUS "  MZ_RAM_FASTPATH: ON (E1 KROK 1 - inline RAM page-table)")
endif()

if(MZ_RAM_FASTPATH_VERIFY)
    add_compile_definitions(MZ800EMU_CFG_RAM_FASTPATH_VERIFY)
    message(STATUS "  MZ_RAM_FASTPATH_VERIFY: ON (diff-verify, slow, DEBUG only)")
endif()

# ----------------------------------------------------------------------------
# Debugger subsystem opt-out (volitelne)
#
# MZ_NO_DEBUGGER - vypne MZ800EMU_CFG_DEBUGGER_ENABLED direktivu pres
# globalni define MZ800EMU_NO_DEBUGGER. Aplikuje se v `mzarch_config.h`
# pres `#ifndef MZ800EMU_NO_DEBUGGER`. Bez debuggeru je binarka cca
# o 15 % mensi (-6.7 MB) a emulace by mela byt rychlejsi.
#
# Zapni pres `make NO_DEBUGGER=1`.
# ----------------------------------------------------------------------------
option(MZ_NO_DEBUGGER "Build without the debugger subsystem (smaller, faster binary)" OFF)
if(MZ_NO_DEBUGGER)
    add_compile_definitions(MZ800EMU_NO_DEBUGGER)
    message(STATUS "  MZ_NO_DEBUGGER: ON (debugger subsystem disabled)")
endif()

# ----------------------------------------------------------------------------
# MCP server toggles (volitelne)
#
# MZ_NO_MCP      - vypne cely MCP backend (pipe + JSONL + TCP).
# MZ_NO_MCP_TCP  - vypne jen TCP listener; pipe transport zustane.
#
# Cascade rules (auto-vynucene zde):
#   MZ_NO_DEBUGGER=ON  =>  MZ_NO_MCP=ON     (MCP vyzaduje debugger)
#   MZ_NO_MCP=ON       =>  MZ_NO_MCP_TCP=ON (TCP vyzaduje MCP backend)
#
# Bez cascade by hostujici kod resetoval pres validacni #error v
# mzarch_config.h. Cascade dela uzivatelsky priznivejsi UX (jeden flag
# misto rucniho vypsani vsech). Validacni #error zustava jako sit pro
# rucni edit config souboru.
# ----------------------------------------------------------------------------
option(MZ_NO_MCP     "Build without the MCP server backend" OFF)
option(MZ_NO_MCP_TCP "Build without the MCP TCP listener (pipe-only)" OFF)

if(MZ_NO_DEBUGGER AND NOT MZ_NO_MCP)
    message(STATUS "  MZ_NO_MCP: forced ON (cascade from MZ_NO_DEBUGGER)")
    set(MZ_NO_MCP ON CACHE BOOL "Build without MCP server backend (forced by NO_DEBUGGER)" FORCE)
endif()

if(MZ_NO_MCP AND NOT MZ_NO_MCP_TCP)
    message(STATUS "  MZ_NO_MCP_TCP: forced ON (cascade from MZ_NO_MCP)")
    set(MZ_NO_MCP_TCP ON CACHE BOOL "Build without MCP TCP listener (forced by NO_MCP)" FORCE)
endif()

if(MZ_NO_MCP)
    add_compile_definitions(MZ800EMU_NO_MCP)
    message(STATUS "  MZ_NO_MCP: ON (MCP server backend disabled)")
endif()

if(MZ_NO_MCP_TCP)
    add_compile_definitions(MZ800EMU_NO_MCP_TCP)
    message(STATUS "  MZ_NO_MCP_TCP: ON (MCP TCP listener disabled)")
endif()

# Default Debug pokud build type není specifikován
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ----------------------------------------------------------------------------
# Globální C/C++ standard a flags
# ----------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Position independent code - obvykle vyžadované u libů
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Auto-generate compile_commands.json (clangd, IDE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug = -g -O0, Release = -O2 -DNDEBUG (CMake to nastavuje defaultně)
# Můžeme to upřesnit:
set(CMAKE_C_FLAGS_DEBUG    "-g -O0")
set(CMAKE_C_FLAGS_RELEASE  "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG  "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Auto-tracking závislostí na headerech (-MMD -MP) - GCC to dělá defaultně,
# ale potvrdíme
add_compile_options(-MMD -MP)

# ----------------------------------------------------------------------------
# CMake module path
# ----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ----------------------------------------------------------------------------
# Externí závislosti
# ----------------------------------------------------------------------------
include(cmake/Dependencies.cmake)

# ----------------------------------------------------------------------------
# build_revision.c generátor
# ----------------------------------------------------------------------------
include(cmake/BuildRevision.cmake)

# ----------------------------------------------------------------------------
# Windows resource compiler helper
# ----------------------------------------------------------------------------
include(cmake/WindowsRC.cmake)

# ----------------------------------------------------------------------------
# Per-target helper
# ----------------------------------------------------------------------------
include(cmake/AddMzEmu.cmake)

# ----------------------------------------------------------------------------
# Sdílené knihovny
# ----------------------------------------------------------------------------
add_subdirectory(src/libs)

# ----------------------------------------------------------------------------
# Per-arch executable
# ----------------------------------------------------------------------------
if(MZ_BUILD_MZ800)
    mz_add_emulator(mz800emu       800  PAL)
endif()

if(MZ_BUILD_MZ1500)
    mz_add_emulator(mz1500emu     1500  NTSC)
endif()

if(MZ_BUILD_MZ700_PAL)
    mz_add_emulator(mz700emu-pal   700  PAL)
endif()

if(MZ_BUILD_MZ700_NTSC)
    mz_add_emulator(mz700emu-ntsc  700  NTSC)
endif()

# ----------------------------------------------------------------------------
# Pipe (JSONL transport) - V0.A.4
#
# Pipe transport je dostupný jako `--pipe` flag v `mz800emu(.exe)` (=
# delegace z src/main.c na mcp_pipe_main v src/emulator/mcp/main_pipe.c).
# Separátní binárka mz800emu_pipe.exe byla původním cílem V0.A.4 ale
# linkování v MSYS2/UCRT64 dosud selhává - viz plans/VYSLEDEK V0.A.4.
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# Test suite (Unity + mztest + CTest)
# ----------------------------------------------------------------------------
if(BUILD_TESTING)
    enable_testing()
    include(cmake/AddMzTest.cmake)
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------------------
# Status report při configure
# ----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=========================================================")
message(STATUS "  ${MZ_PROJECT_NAME} ${MZ_DISPLAY_VERSION}")
message(STATUS "=========================================================")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler (C):      ${CMAKE_C_COMPILER}")
message(STATUS "  Compiler (C++):    ${CMAKE_CXX_COMPILER}")
message(STATUS "  Generator:         ${CMAKE_GENERATOR}")
message(STATUS "  Platform:          ${CMAKE_SYSTEM_NAME}")
message(STATUS "  MZ800 build:       ${MZ_BUILD_MZ800}")
message(STATUS "  MZ1500 build:      ${MZ_BUILD_MZ1500}")
message(STATUS "  MZ700 PAL build:   ${MZ_BUILD_MZ700_PAL}")
message(STATUS "  MZ700 NTSC build:  ${MZ_BUILD_MZ700_NTSC}")
message(STATUS "  Force console:     ${MZ_FORCE_CONSOLE}")
message(STATUS "  Debugger disabled: ${MZ_NO_DEBUGGER}")
message(STATUS "  MCP disabled:      ${MZ_NO_MCP}")
message(STATUS "  MCP TCP disabled:  ${MZ_NO_MCP_TCP}")
message(STATUS "  Binary dir:        ${CMAKE_BINARY_DIR}")
message(STATUS "=========================================================")
message(STATUS "")
