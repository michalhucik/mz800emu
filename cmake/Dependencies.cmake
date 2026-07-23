# cmake/Dependencies.cmake
#
# Detekce externích závislostí přes pkg-config (SDL3, GLib, minizip-ng, libcurl)
# a platformně specifických knihoven (OpenGL, Win32 systémové).
#
# Po include() jsou dostupné importované knihovny:
#   - mz::sdl3        (SDL3 + SDL3_image)
#   - mz::glib        (glib-2.0)
#   - mz::minizip     (minizip-ng)
#   - mz::libcurl     (libcurl, na Windows static)
#   - mz::opengl      (OpenGL, GDI, IMM32 na Windows)
#
# Také propaguje compile defines USE_SDL3, USE_SDL3_VIDEO, USE_SDL_AUDIO,
# USE_SDL3_AUDIO a platformní WINDOWS/LINUX.

# Wrapper Makefile nastavuje PKG_CONFIG_PATH podle MSYSTEM (MINGW64/UCRT64).
# Pokud se cmake volá přímo bez wrapperu a PKG_CONFIG_PATH ukazuje na jiný
# toolchain než compiler, vznikají ABI mismatch (různý layout _Mbstatet apod.).
if(WIN32 AND DEFINED ENV{PKG_CONFIG_PATH} AND DEFINED CMAKE_C_COMPILER)
    # Z cesty kompilátoru extrahujeme kořen toolchainu (např. C:/msys64/mingw64)
    get_filename_component(_compiler_bin_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_compiler_root "${_compiler_bin_dir}" DIRECTORY)

    if(NOT "$ENV{PKG_CONFIG_PATH}" MATCHES "${_compiler_root}")
        message(WARNING
            "PKG_CONFIG_PATH ($ENV{PKG_CONFIG_PATH}) neodpovídá toolchainu "
            "kompilátoru (${_compiler_root}). Hrozí ABI mismatch (např. "
            "std::codecvt_utf8_utf16 linker chyby). Použij 'make' wrapper "
            "nebo nastav PKG_CONFIG_PATH na ${_compiler_root}/lib/pkgconfig."
        )
    endif()
endif()

find_package(PkgConfig REQUIRED)

# ----------------------------------------------------------------------------
# SDL3 + SDL3_image
# ----------------------------------------------------------------------------
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3 sdl3-image)

add_library(mz_sdl3 INTERFACE)
add_library(mz::sdl3 ALIAS mz_sdl3)
target_link_libraries(mz_sdl3 INTERFACE PkgConfig::SDL3)
target_compile_definitions(mz_sdl3 INTERFACE
    USE_SDL3
    USE_SDL3_VIDEO
    USE_SDL_AUDIO
    USE_SDL3_AUDIO
)

# Na Windows pkg-config typicky vrátí -mwindows v --libs sdl3.
# Pro FORCE_CONSOLE potřebujeme tuto volbu odstranit, jinak vznikne GUI subsystem
# bez konzole. CMake tu nemá hezký override - musíme manuálně přepsat link options.
if(WIN32 AND MZ_FORCE_CONSOLE)
    # Necháme include flags z pkg-config, ale přepíšeme libs (bez -mwindows)
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --libs sdl3 sdl3-image
        OUTPUT_VARIABLE _sdl3_libs_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REPLACE "-mwindows" "" _sdl3_libs_no_console "${_sdl3_libs_raw}")
    separate_arguments(_sdl3_libs_no_console UNIX_COMMAND "${_sdl3_libs_no_console}")
    # Přemažeme INTERFACE_LINK_LIBRARIES z PkgConfig::SDL3 explicitním seznamem
    set_property(TARGET mz_sdl3 PROPERTY INTERFACE_LINK_LIBRARIES ${_sdl3_libs_no_console})
endif()

# ----------------------------------------------------------------------------
# Alternativní SDL3 INTERFACE knihovna BEZ -mwindows (= console subsystem).
#
# Pro mz800emu_pipe (V0.A.4 JSONL transport) - pipe binárka potřebuje
# klasický console subsystem (stdin/stdout). Bez tohoto override by
# pkg-config protlačil -mwindows do link line a linker by selhal s
# tichou chybou (collect2 "ld returned 1 exit status" bez detailu).
#
# Vytváříme jen na Windows; na Linuxu je INTERFACE alias na mz_sdl3.
# ----------------------------------------------------------------------------
add_library(mz_sdl3_console INTERFACE)
add_library(mz::sdl3_console ALIAS mz_sdl3_console)
target_compile_definitions(mz_sdl3_console INTERFACE
    USE_SDL3
    USE_SDL3_VIDEO
    USE_SDL_AUDIO
    USE_SDL3_AUDIO
)
if(WIN32)
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --libs sdl3 sdl3-image
        OUTPUT_VARIABLE _sdl3_libs_pipe_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REPLACE "-mwindows" "" _sdl3_libs_pipe "${_sdl3_libs_pipe_raw}")
    separate_arguments(_sdl3_libs_pipe UNIX_COMMAND "${_sdl3_libs_pipe}")
    set_property(TARGET mz_sdl3_console PROPERTY
        INTERFACE_LINK_LIBRARIES ${_sdl3_libs_pipe})
    # Include directories přebíráme z PkgConfig::SDL3 (stejné jako mz_sdl3).
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags-only-I sdl3 sdl3-image
        OUTPUT_VARIABLE _sdl3_cflags_pipe
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    separate_arguments(_sdl3_cflags_pipe UNIX_COMMAND "${_sdl3_cflags_pipe}")
    foreach(_inc IN LISTS _sdl3_cflags_pipe)
        string(REGEX REPLACE "^-I" "" _inc_path "${_inc}")
        target_include_directories(mz_sdl3_console INTERFACE ${_inc_path})
    endforeach()
else()
    target_link_libraries(mz_sdl3_console INTERFACE PkgConfig::SDL3)
endif()

# ----------------------------------------------------------------------------
# GLib 2.0
# ----------------------------------------------------------------------------
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)

add_library(mz_glib INTERFACE)
add_library(mz::glib ALIAS mz_glib)
target_link_libraries(mz_glib INTERFACE PkgConfig::GLIB)

# ----------------------------------------------------------------------------
# json-glib 1.0
#
# JSON-GLib (LGPL, postavená nad GLib + GObject) je primární nástroj pro
# strukturovaný read/write JSON v emulátoru. Použít všude tam, kde se generuje
# nebo parsuje JSON soubor (.bpt, debugger meta.json, trace-suite meta).
#
# Přímý g_string_append_printf() pro JSON se NEPOUŽÍVÁ - viz devdoc/json-style.md.
#
# MSYS2 package: mingw-w64-{ucrt,mingw}-x86_64-json-glib (verze 1.10+)
# Linux:        libjson-glib-dev (Debian/Ubuntu) / json-glib-devel (Fedora)
# ----------------------------------------------------------------------------
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

add_library(mz_json_glib INTERFACE)
add_library(mz::json_glib ALIAS mz_json_glib)
target_link_libraries(mz_json_glib INTERFACE PkgConfig::JSON_GLIB)

# ----------------------------------------------------------------------------
# minizip-ng (snapshot system)
# Linux používá static + transitivní deps, Windows dynamic
# ----------------------------------------------------------------------------
if(UNIX)
    pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip-ng)
    # Pro statické linkování - Linux variant
    pkg_check_modules(MINIZIP_STATIC REQUIRED minizip-ng)
endif()
if(WIN32)
    pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip-ng)
endif()

add_library(mz_minizip INTERFACE)
add_library(mz::minizip ALIAS mz_minizip)
target_link_libraries(mz_minizip INTERFACE PkgConfig::MINIZIP)
if(UNIX AND NOT APPLE AND MINIZIP_STATIC_LIBRARIES)
    target_link_libraries(mz_minizip INTERFACE ${MINIZIP_STATIC_LIBRARIES})
endif()

# Pokud je dostupny pouze .a archiv minizip-ng (bez .so), linkuje se staticky
# a transitivni zavislost na zlib se nedoplnuje automaticky pres pkg-config
# (--libs minizip-ng nevraci -lz, jen --libs --static ji vraci, ale STATIC
# argument v pkg_check_modules vyzaduje CMake >=3.29). Reseni: explicitni
# find_package(ZLIB) + link na UNIX. Pokud je dostupna shared verze, link
# je redundantni ale neskodi.
if(UNIX)
    find_package(ZLIB REQUIRED)
    target_link_libraries(mz_minizip INTERFACE ZLIB::ZLIB)
endif()

# ----------------------------------------------------------------------------
# libcurl
# Na Windows static (s -Wl,-Bstatic / -Wl,-Bdynamic obalením)
# ----------------------------------------------------------------------------
add_library(mz_libcurl INTERFACE)
add_library(mz::libcurl ALIAS mz_libcurl)

if(UNIX)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    target_link_libraries(mz_libcurl INTERFACE PkgConfig::CURL)
endif()

if(WIN32)
    # Static link curl - potřebujeme --static cflags + libs s Bstatic obalením
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags --static libcurl
        OUTPUT_VARIABLE _curl_cflags_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --libs --static libcurl
        OUTPUT_VARIABLE _curl_libs_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    separate_arguments(_curl_cflags UNIX_COMMAND "${_curl_cflags_raw}")
    separate_arguments(_curl_libs UNIX_COMMAND "${_curl_libs_raw}")

    # Vyextrahujeme -I a -D z cflags
    set(_curl_includes "")
    set(_curl_defines "")
    foreach(_flag IN LISTS _curl_cflags)
        if(_flag MATCHES "^-I(.+)")
            list(APPEND _curl_includes "${CMAKE_MATCH_1}")
        elseif(_flag MATCHES "^-D(.+)")
            list(APPEND _curl_defines "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    target_include_directories(mz_libcurl INTERFACE ${_curl_includes})
    target_compile_definitions(mz_libcurl INTERFACE ${_curl_defines})

    # Linkování v static obalení.
    # Použijeme target_link_libraries (ne target_link_options), aby CMake
    # umístil flags ZA archivy v link line - jinak by static knihovny
    # nenašly potřebné symboly (linker order matters pro static libs).
    target_link_libraries(mz_libcurl INTERFACE
        -Wl,-Bstatic ${_curl_libs} -Wl,-Bdynamic
    )
endif()

# ----------------------------------------------------------------------------
# OpenGL + platformní GUI knihovny
# ----------------------------------------------------------------------------
add_library(mz_opengl INTERFACE)
add_library(mz::opengl ALIAS mz_opengl)

if(UNIX)
    if (APPLE)
        find_package(OpenGL REQUIRED)
        target_link_libraries(mz_opengl INTERFACE OpenGL::GL)
    else()
        pkg_check_modules(GL REQUIRED IMPORTED_TARGET gl)
        target_link_libraries(mz_opengl INTERFACE PkgConfig::GL)
    endif()
endif()

if(WIN32)
    target_link_libraries(mz_opengl INTERFACE gdi32 opengl32 imm32)
endif()

# ----------------------------------------------------------------------------
# Platformní compile defines
# ----------------------------------------------------------------------------
add_library(mz_platform INTERFACE)
add_library(mz::platform ALIAS mz_platform)

if(UNIX)
    target_compile_definitions(mz_platform INTERFACE
        LINUX
        _POSIX_C_SOURCE=200809L
    )
    target_link_libraries(mz_platform INTERFACE m)
endif()

if(WIN32)
    target_compile_definitions(mz_platform INTERFACE
        WINDOWS
        WINDOWS_X64
    )
    # mingw32 musí být v link line (specifické pro MinGW main wrapper)
    target_link_libraries(mz_platform INTERFACE mingw32)

    # WinSock 2 - vyžaduje src/emulator/mcp/tcp_server.c (V0.A.5).
    # Symbol set: WSAStartup, socket, bind, listen, accept, send, recv,
    # shutdown, closesocket, inet_ntop, WSAGetLastError, ...
    # Linkujeme bezpodmínečně - i builds s NO_MCP_TCP=1 si lib přilinkují
    # jako no-op (žádné call sites), což je acceptable trade-off proti
    # podmíněnému linkování přes per-target option.
    target_link_libraries(mz_platform INTERFACE ws2_32)
endif()

