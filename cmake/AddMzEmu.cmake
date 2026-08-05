# cmake/AddMzEmu.cmake
#
# Helper funkce mz_add_emulator() která vytvoří per-arch executable (mz800emu,
# mz700emu, mz1500emu).
#
# Sources jsou společné pro všechny architektury (DIRS_MAIN, DIRS_EMULATOR,
# flat src/*.c, ...) - liší se pouze MZARCH define a per-arch podadresáře
# (src/emulator/mzarch/mz<N>/, src/ui-imgui/mz<N>/).
#
# Knihovny v src/libs/ jsou arch-independent a linkují se jen jednou (sdílené
# přes všechny arch executable).

# ----------------------------------------------------------------------------
# Pomocná funkce: rekurzivně sebere .c/.cpp z adresáře.
# (CMake nemá globbing v add_executable jako Makefile $(shell find), tak ho
# tady simulujeme přes file(GLOB_RECURSE).
#
# POZN: GLOB_RECURSE nezachycuje nově přidané soubory bez re-configure!
# Pokud přidáš .c/.cpp soubor, musíš ručně spustit `cmake .` (nebo smazat
# CMakeCache).
# ----------------------------------------------------------------------------
function(mz_glob_sources out_var)
    set(_all "")
    foreach(_dir IN LISTS ARGN)
        file(GLOB_RECURSE _src CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/${_dir}/*.c"
            "${CMAKE_SOURCE_DIR}/${_dir}/*.cpp"
        )
        list(APPEND _all ${_src})
    endforeach()
    set(${out_var} ${_all} PARENT_SCOPE)
endfunction()

# ----------------------------------------------------------------------------
# Pomocná funkce: flat scan adresáře (bez rekurze do podadresářů).
# ----------------------------------------------------------------------------
function(mz_glob_flat out_var)
    set(_all "")
    foreach(_dir IN LISTS ARGN)
        file(GLOB _src CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/${_dir}/*.c"
            "${CMAKE_SOURCE_DIR}/${_dir}/*.cpp"
        )
        list(APPEND _all ${_src})
    endforeach()
    set(${out_var} ${_all} PARENT_SCOPE)
endfunction()

# ----------------------------------------------------------------------------
# mz_add_emulator(<target> <mzarch> <tvsys>)
#
# Vytvoří executable s per-arch sources, definemi a linknutými knihovnami.
# tvsys: PAL nebo NTSC. Definuje MZTVSYS=MZTVSYS_<tvsys> (= 50 nebo 60 dle
# nominal frame rate). MZ-700 má obě varianty (mz700emu-pal, mz700emu-ntsc),
# MZ-800 = vždy PAL, MZ-1500 = vždy NTSC.
#
# Příklad:
#   mz_add_emulator(mz800emu       800  PAL)
#   mz_add_emulator(mz1500emu     1500  NTSC)
#   mz_add_emulator(mz700emu-pal   700  PAL)
#   mz_add_emulator(mz700emu-ntsc  700  NTSC)
# ----------------------------------------------------------------------------
# ----------------------------------------------------------------------------
# mz_add_emucore_lib() - compile-once knihovna sdílených TU (mzhal krok 11)
#
# TU ze seznamu cmake/emucore_sources.txt se kompilují JEDNOU (bez
# -DMZARCH/-DMZTVSYS/-D capability maker) do statické knihovny
# mz_emucore, kterou linkují všechny 4 EXE. Bezpečnost: -Werror=undef
# (chytá #if formy) + force-included cmake/emucore_poison.h
# (#pragma GCC poison chytá i #ifdef a použití v kódu).
#
# Seznam zdrojů je explicitní (žádný glob) - do knihovny smí jen TU,
# jehož celý include řetěz je bez per-arch podmínek (ověřeno přes
# ninja deps, viz mzhal/HLAVICKY-INVENTURA.md). Rozšiřování seznamu =
# další dávky kroku 11.
# ----------------------------------------------------------------------------
function(mz_add_emucore_lib)
    # Změna seznamu musí vyvolat re-configure (file(STRINGS) sama o sobě
    # závislost nezakládá - bez tohoto by build používal starý seznam).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/cmake/emucore_sources.txt)
    file(STRINGS ${CMAKE_SOURCE_DIR}/cmake/emucore_sources.txt _emucore_rel)
    set(_emucore_abs "")
    foreach(_src IN LISTS _emucore_rel)
        list(APPEND _emucore_abs ${CMAKE_SOURCE_DIR}/${_src})
    endforeach()
    set_property(GLOBAL PROPERTY EMUCORE_SOURCES ${_emucore_abs})

    add_library(mz_emucore STATIC ${_emucore_abs})

    target_compile_options(mz_emucore PRIVATE
        -Wundef -Werror=undef
        -include ${CMAKE_SOURCE_DIR}/cmake/emucore_poison.h
    )

    target_include_directories(mz_emucore PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/emulator
        ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic
    )
    file(GLOB_RECURSE _header_dirs LIST_DIRECTORIES true
        "${CMAKE_SOURCE_DIR}/src/*/headers"
    )
    list(FILTER _header_dirs INCLUDE REGEX "/headers$")
    foreach(_hd IN LISTS _header_dirs)
        if(IS_DIRECTORY ${_hd})
            target_include_directories(mz_emucore PRIVATE ${_hd})
        endif()
    endforeach()

    target_link_libraries(mz_emucore PRIVATE
        mz::libs
        mz::sdl3
        mz::glib
        mz::json_glib
        mz::minizip
        mz::libcurl
        mz::opengl
        mz::platform
    )

    list(LENGTH _emucore_rel _n)
    message(STATUS "  mz_emucore: ${_n} compile-once TU")
endfunction()


function(mz_add_emulator target mzarch tvsys)
    set(arch_name "mz${mzarch}")

    # ---- Společné zdroje (DIRS_MAIN + DIRS_EMULATOR + flat) ----------------

    # Recursive sběr DIRS_MAIN, ale BEZ per-arch ui-imgui podadresářů
    mz_glob_sources(_sources_main
        src/app
        src/baseui
        src/generic_driver
        src/iface
        src/iface-audio
        src/iface-video
        src/ui
        src/version_check
    )

    # ui-imgui rekurzivně, ale exclude všech ostatních arch podadresářů
    mz_glob_sources(_sources_uiimgui src/ui-imgui)
    list(FILTER _sources_uiimgui EXCLUDE REGEX "/src/ui-imgui/mz[0-9]+/")

    # DIRS_EMULATOR rekurzivně
    #
    # src/emulator/mcp/ obsahuje JSONL transport + dispatch (V0.A.2+) +
    # main_pipe.c (V0.A.4 pipe transport, vstupní bod `mcp_pipe_main`,
    # volaný z src/main.c při detekci --pipe flagu).
    # Soubory uvnitř jsou obaleny `#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED`,
    # takže při buildu s MZ_NO_MCP=ON se zkompilují jako prázdné translation
    # units (= žádné nedefinované symboly, žádná závislost na json-glib
    # pokud uživatel MCP vypnul build-time).
    mz_glob_sources(_sources_emu
        src/emulator/hw-generic
        src/emulator/debugger
        src/emulator/snapshot
        src/emulator/mcp
    )

    # Flat src soubory
    mz_glob_flat(_sources_flat_src      src)
    mz_glob_flat(_sources_flat_emu      src/emulator)
    mz_glob_flat(_sources_flat_mzarch   src/emulator/mzarch)

    # Per-arch zdroje - rekurzivně z mz<N>
    mz_glob_sources(_sources_arch
        src/emulator/mzarch/${arch_name}
        src/ui-imgui/${arch_name}
    )

    set(_all_sources
        ${_sources_main}
        ${_sources_uiimgui}
        ${_sources_emu}
        ${_sources_flat_src}
        ${_sources_flat_emu}
        ${_sources_flat_mzarch}
        ${_sources_arch}
        ${MZ_BUILD_REVISION_C}
    )

    # Compile-once TU jdou z knihovny mz_emucore (mzhal krok 11) -
    # z per-EXE seznamu je odebereme, jinak by symboly existovaly 2x.
    if(TARGET mz_emucore)
        get_property(_emucore_sources GLOBAL PROPERTY EMUCORE_SOURCES)
        list(REMOVE_ITEM _all_sources ${_emucore_sources})
    endif()

    # ---- Vytvoření executable ----------------------------------------------

    add_executable(${target} ${_all_sources})

    # build_revision.c se generuje custom targetem - musí proběhnout dřív
    add_dependencies(${target} mz_build_revision)

    # Windows .rc → .o se přidá přes helper. RC je shared per mzarch
    # (mz700emu-pal a mz700emu-ntsc sdílí mz700emu.rc) - per-tvsys ikony
    # by nedávaly smysl.
    if(WIN32)
        mz_add_windows_rc(${target} ${CMAKE_SOURCE_DIR}/src/windows_rc/${arch_name}emu.rc)
    endif()

    # ---- Per-arch a per-tvsys defines --------------------------------------

    # Hodnoty MZTVSYS_PAL/NTSC uz NEdefinuje build - jsou v hlavicce
    # src/emulator/mzarch/mztvsys.h (jediny zdroj pravdy). Zde zustava jen
    # vyber TV systemu targetu (MZTVSYS) + jmenne stringy.
    target_compile_definitions(${target} PRIVATE
        MZARCH=${mzarch}
        MZARCH_NAME="${arch_name}"
        MZTVSYS=MZTVSYS_${tvsys}
        MZTVSYS_NAME="${tvsys}"
    )

    # Preprocesorova bezpecnostni sit (mzhal krok 4): nedefinovane makro
    # v #if se tise vyhodnoti jako 0 - pri migraci na compile-once kod je
    # to killer trida chyb. -Werror=undef ji meni na compile error.
    # Vendorovane knihovny (src/libs targety) flag nedostavaji.
    target_compile_options(${target} PRIVATE -Wundef -Werror=undef)

    # ---- Include paths -----------------------------------------------------

    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/emulator
        ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic
    )

    # Headers adresáře (src/**/headers) - z Makefile HEADER_DIRS
    # POZN: file(GLOB_RECURSE ... LIST_DIRECTORIES true ...) ignoruje pattern
    # a vrací VSECHNY podadresare pod src/. To by zaradilo i src/libs/igfd/dirent
    # (Windows shim s #include <windows.h>) do include path - na Linuxu to padne
    # na chybejici windows.h, na MSYS2/MinGW to projde, ale shim neni potreba
    # (mingw64/glibc maji systemovy <dirent.h>). Proto rucne filtrujeme.
    file(GLOB_RECURSE _header_dirs LIST_DIRECTORIES true
        "${CMAKE_SOURCE_DIR}/src/*/headers"
    )
    list(FILTER _header_dirs INCLUDE REGEX "/headers$")
    foreach(_hd IN LISTS _header_dirs)
        if(IS_DIRECTORY ${_hd})
            target_include_directories(${target} PRIVATE ${_hd})
        endif()
    endforeach()

    # ---- Linkování ---------------------------------------------------------

    target_link_libraries(${target} PRIVATE
        mz_emucore
        mz::libs
        mz::sdl3
        mz::glib
        mz::json_glib
        mz::minizip
        mz::libcurl
        mz::opengl
        mz::platform
    )

    # POZN: dříve jsme tu měli -static-libstdc++ -static-libgcc jako workaround
    # pro `std::codecvt_utf8_utf16` linker chyby. Zjistilo se ale, že skutečnou
    # příčinou byl ABI mismatch UCRT64 headers (z pkg-config) + MINGW64 libstdc++.
    # Vynucení PKG_CONFIG_PATH na MINGW64 v Makefile wrapperu problém vyřešilo.
    # Static C++ runtime tedy nepotřebujeme - viz Makefile wrapper PKG_CONFIG_PATH.

    # ---- Output directory --------------------------------------------------
    # Binárka jde do build-${target}/ (per target name, ne arch_name) - aby
    # se mz700emu-pal a mz700emu-ntsc nepřepsaly navzájem ve sdíleném dir.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build-${target}
    )

    # ---- FORCE_CONSOLE: na Windows GUI subsystem nahradíme za CONSOLE -----
    # Pokud MZ_FORCE_CONSOLE=ON, mz::sdl3 už má upravené libs bez -mwindows,
    # ale linker by mohl ještě default mít WIN32_EXECUTABLE. Necháme default
    # (CMake na Windows defaultně linkuje pro console subsystem; SDL3 svým
    # -mwindows přepíná na GUI - když ho odebereme, vrátíme se ke konzoli).
endfunction()


# ----------------------------------------------------------------------------
# mz_add_pipe_emulator() - V0.A.4 zakomentováno, deferred do V0.A.4.x
#
# Původní plán V0.A.4 byl vytvořit separátní binárku mz800emu_pipe(.exe)
# se swapnutým entry-pointem (main_pipe.c místo main.c). V MSYS2/UCRT64
# se ale link mz800emu_pipe.exe nepodařilo zprovoznit - linker (ld přes
# collect2) selhával tichou chybou "ld returned 5 exit status" bez
# diagnostiky undefined / multi-defined symbolů; identifikovat root
# cause se nepodařilo (viz plans/VYSLEDEK-faze-V0.A.4-main-pipe.md).
#
# Aktuální cesta V0.A.4: pipe transport je dostupný jako `--pipe`
# flag v existujícím mz800emu.exe (= delegace na mcp_pipe_main z
# src/main.c). Tím se obejde problém s linkováním a zachová se
# funkční pipe transport pro V0.A.5 / V0.B navazující práce.
#
# Pokud bude později separátní binárka vyžadovaná, funkce se obnoví.
# ----------------------------------------------------------------------------
function(mz_add_pipe_emulator_disabled target mzarch tvsys)
    set(arch_name "mz${mzarch}")

    # Stejný source mix jako mz_add_emulator() - jen swap main souboru.
    mz_glob_sources(_sources_main
        src/app
        src/baseui
        src/generic_driver
        src/iface
        src/iface-audio
        src/iface-video
        src/ui
        src/version_check
    )

    mz_glob_sources(_sources_uiimgui src/ui-imgui)
    list(FILTER _sources_uiimgui EXCLUDE REGEX "/src/ui-imgui/mz[0-9]+/")

    mz_glob_sources(_sources_emu
        src/emulator/hw-generic
        src/emulator/debugger
        src/emulator/snapshot
        src/emulator/mcp
    )
    # Pro pipe target VŠECHNY src/emulator/mcp/ soubory zůstávají (vč.
    # main_pipe.c) - žádný filter.

    mz_glob_flat(_sources_flat_src      src)
    mz_glob_flat(_sources_flat_emu      src/emulator)
    mz_glob_flat(_sources_flat_mzarch   src/emulator/mzarch)

    # Klíčový rozdíl: odstraníme src/main.c (GUI entry) z flat src.
    list(FILTER _sources_flat_src EXCLUDE REGEX "/src/main\\.c$")

    mz_glob_sources(_sources_arch
        src/emulator/mzarch/${arch_name}
        src/ui-imgui/${arch_name}
    )

    set(_all_sources
        ${_sources_main}
        ${_sources_uiimgui}
        ${_sources_emu}
        ${_sources_flat_src}
        ${_sources_flat_emu}
        ${_sources_flat_mzarch}
        ${_sources_arch}
        ${MZ_BUILD_REVISION_C}
    )

    add_executable(${target} ${_all_sources})
    add_dependencies(${target} mz_build_revision)

    # Windows .rc - shared s GUI variantou.
    if(WIN32)
        mz_add_windows_rc(${target} ${CMAKE_SOURCE_DIR}/src/windows_rc/${arch_name}emu.rc)
    endif()

    target_compile_definitions(${target} PRIVATE
        MZARCH=${mzarch}
        MZARCH_NAME="${arch_name}"
        MZTVSYS=MZTVSYS_${tvsys}
        MZTVSYS_NAME="${tvsys}"
    )

    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/emulator
        ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic
    )

    file(GLOB_RECURSE _header_dirs LIST_DIRECTORIES true
        "${CMAKE_SOURCE_DIR}/src/*/headers"
    )
    list(FILTER _header_dirs INCLUDE REGEX "/headers$")
    foreach(_hd IN LISTS _header_dirs)
        if(IS_DIRECTORY ${_hd})
            target_include_directories(${target} PRIVATE ${_hd})
        endif()
    endforeach()

    target_link_libraries(${target} PRIVATE
        mz::libs
        mz::sdl3_console   # bez -mwindows, viz cmake/Dependencies.cmake
        mz::glib
        mz::json_glib
        mz::minizip
        mz::libcurl
        mz::opengl
        mz::platform
    )

    # Pipe binárka jde do vlastního build-${target}/ pro snadné kopírování.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build-${target}
    )

    # Vynutíme CONSOLE subsystem na Windows. Použít mz::sdl3_console
    # bohužel nestačí - mz::libs tahá mzlib_sdlapp, který má DEPS
    # mz::sdl3, takže -mwindows se nakonec stejně dostane do command line.
    # Triky s -mconsole nepomáhají, protože LINK_FLAGS jdou PŘED LINK_LIBRARIES
    # a linker bere poslední --subsystem specifikaci.
    #
    # Řešení: explicit -Wl,--subsystem,console+exe_format na úrovni ld,
    # který jde do LINK_LIBRARIES (= za -mwindows), takže vyhraje.
    if(WIN32)
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE OFF)
        # PE/COFF: subsystem 3 = WINDOWS_CUI (= console), 2 = WINDOWS (= GUI).
        # Jdeme přes target_link_libraries jako "linker option string" na konci
        # link line, aby vyhrál nad -mwindows uprostřed.
        target_link_libraries(${target} PRIVATE -Wl,--subsystem,console)
    endif()
endfunction()
