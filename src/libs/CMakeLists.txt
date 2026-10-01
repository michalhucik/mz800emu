# src/libs/CMakeLists.txt
#
# Sdílené knihovny - arch-independent (žádné MZARCH closures).
# Každá knihovna jako samostatný STATIC target.
# Archivy (.a) se ukládají do ${CMAKE_BINARY_DIR}/build-libs/.

# ----------------------------------------------------------------------------
# Helper: přidá static knihovnu se standardními propsy.
# ----------------------------------------------------------------------------
function(mz_add_lib lib_name)
    cmake_parse_arguments(ARG "NO_WARNINGS" "" "SOURCES;INCLUDES;DEPS" ${ARGN})

    add_library(${lib_name} STATIC ${ARG_SOURCES})

    # Veřejný include path - pro konzumenty knihovny
    if(ARG_INCLUDES)
        target_include_directories(${lib_name} PUBLIC ${ARG_INCLUDES})
    endif()

    # src/ je globální include base (kvůli #include "libs/foo/bar.h" stylu)
    target_include_directories(${lib_name} PUBLIC ${CMAKE_SOURCE_DIR}/src)

    # Závislosti na ostatních libs nebo externích cílech (mz::glib apod.)
    if(ARG_DEPS)
        target_link_libraries(${lib_name} PUBLIC ${ARG_DEPS})
    endif()

    # Vendored knihovny generují varování, vypnout
    if(ARG_NO_WARNINGS)
        target_compile_options(${lib_name} PRIVATE -w)
    endif()

    # Archive (.a) jde do build-libs/
    set_target_properties(${lib_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build-libs
    )
endfunction()

# ----------------------------------------------------------------------------
# Pure C/utility libs
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_endianity        SOURCES endianity/endianity.c)
mz_add_lib(mzlib_generic_driver   SOURCES generic_driver/generic_driver.c
                                  DEPS    mz::glib)
mz_add_lib(mzlib_sharpmz_ascii    SOURCES sharpmz_ascii/sharpmz_ascii.c
                                          sharpmz_ascii/sharpmz_utf8.c)
mz_add_lib(mzlib_sharpmz_koi8cs   SOURCES sharpmz_koi8cs/sharpmz_koi8cs.c)
mz_add_lib(mzlib_vram_sim         SOURCES vram_sim/vram_sim.c
                                          vram_sim/vram_sim_modes.c
                                          vram_sim/vram_sim_render.c)
mz_add_lib(mzlib_mz_vcode         SOURCES mz_vcode/mz_vcode.c)
mz_add_lib(mzlib_mzglyphs         SOURCES mzglyphs/mzglyphs.c)
mz_add_lib(mzlib_wav              SOURCES wav/wav.c
                                  DEPS    mz::glib)
mz_add_lib(mzlib_qd_image         SOURCES qd_image/qd_image.c)

mz_add_lib(mzlib_cfgfile          SOURCES cfgfile/cfgelement.c
                                          cfgfile/cfgmodule.c
                                          cfgfile/cfgroot.c
                                          cfgfile/cfgtools.c
                                  DEPS    mz::glib)

mz_add_lib(mzlib_cmt_stream       SOURCES cmt_stream/cmt_bitstream.c
                                          cmt_stream/cmt_stream.c
                                          cmt_stream/cmt_vstream.c
                                  DEPS    mz::glib mzlib_wav)

mz_add_lib(mzlib_cmtspeed         SOURCES cmtspeed/cmtspeed.c)

# ----------------------------------------------------------------------------
# Z80 CPU + disassembler
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_cpu_z80          SOURCES cpu-z80/z80.c)

mz_add_lib(mzlib_dasm_z80         SOURCES dasm-z80/z80_dasm.c
                                          dasm-z80/z80_dasm_analysis.c
                                          dasm-z80/z80_dasm_compat.c
                                          dasm-z80/z80_dasm_format.c
                                          dasm-z80/z80_dasm_symtab.c
                                          dasm-z80/z80_dasm_tables.c)

# Z80 instrukcni metadata (description + flag effects pro debugger tooltip).
# z80_meta_table.c je auto-generated z tools/z80_meta_gen v emu-experiments.
mz_add_lib(mzlib_z80meta          SOURCES z80meta/z80_meta.c
                                          z80meta/z80_meta_table.c
                                  DEPS    mzlib_dasm_z80)

mz_add_lib(mzlib_iasm             SOURCES iasm/iasm.c)

# ----------------------------------------------------------------------------
# MCS-48 (Intel 8050) - jádro mikrokontroléru plotteru MZ-1P16
# (experiment mz1p16-plotter, fáze 1). Arch-independent, ROM-agnostic.
# Zdroj je v src/emulator/hw-generic/mz1p16/, registrován jako samostatná
# knihovna kvůli standalone unit testu (tests/libs/mcs48).
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_mcs48            SOURCES ../emulator/hw-generic/mz1p16/mcs48.c
                                  INCLUDES ${CMAKE_SOURCE_DIR}/src/emulator)

# Embeddovany firmware plotteru (4 KB ROM 8050) + mechanika MZ-1P16.
# Samostatna knihovna kvuli standalone testum mechaniky.
mz_add_lib(mzlib_mz1p16           SOURCES ../emulator/hw-generic/mz1p16/mz1p16_rom.c
                                          ../emulator/hw-generic/mz1p16/mz1p16.c
                                  DEPS    mzlib_mcs48
                                  INCLUDES ${CMAKE_SOURCE_DIR}/src/emulator)

# ----------------------------------------------------------------------------
# Souborové formáty
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_dsk              SOURCES dsk/dsk.c
                                          dsk/dsk_tools.c)

mz_add_lib(mzlib_mzf              SOURCES mzf/mzf.c
                                          mzf/mzf_tools.c)

mz_add_lib(mzlib_mztape           SOURCES mztape/mztape.c
                                  DEPS    mzlib_mzf mzlib_cmt_stream)

mz_add_lib(mzlib_zxtape           SOURCES zxtape/zxtape.c
                                  DEPS    mzlib_cmt_stream)

# ----------------------------------------------------------------------------
# sdlapp (SDL3 application framework)
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_sdlapp
    SOURCES sdlapp/sdlapp.c
            sdlapp/sdlapp_options.c
            sdlapp/sdlapp_paths.c
            sdlapp/sdlapp_window.c
            sdlapp/sdlapp_winmanager.c
            sdlapp/sdl3_backend/sdl3_backend.c
            sdlapp/sdl3_backend/sdl3_background_GetEventName.c
    DEPS    mz::sdl3 mz::glib mz::platform
)

# ----------------------------------------------------------------------------
# ImGui (vendored core + SDL3/OpenGL3 backendy)
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_imgui
    SOURCES imgui/imgui.cpp
            imgui/imgui_demo.cpp
            imgui/imgui_draw.cpp
            imgui/imgui_tables.cpp
            imgui/imgui_widgets.cpp
            imgui/backends/imgui_impl_opengl3.cpp
            imgui/backends/imgui_impl_sdl3.cpp
            imgui/backends/imgui_impl_sdlrenderer3.cpp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/imgui
             ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    DEPS    mz::sdl3 mz::opengl
)

# ----------------------------------------------------------------------------
# ImGuiFileDialog (vendored, vyžaduje custom config header z aplikace)
# Konzument musí includovat src/ui-imgui/filechooser/headers (kde je
# CustomImGuiFileDialogConfig.h).
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_igfd
    NO_WARNINGS
    SOURCES igfd/ImGuiFileDialog.cpp
    DEPS    mzlib_imgui mz::glib
)
target_compile_definitions(mzlib_igfd PUBLIC
    CUSTOM_IMGUIFILEDIALOG_CONFIG="CustomImGuiFileDialogConfig.h"
)
target_include_directories(mzlib_igfd PUBLIC
    ${CMAKE_SOURCE_DIR}/src/ui-imgui/filechooser/headers
)

# ----------------------------------------------------------------------------
# ImWidgets (vendored, vyžaduje custom config header z aplikace)
# ----------------------------------------------------------------------------
mz_add_lib(mzlib_imwidgets
    NO_WARNINGS
    SOURCES imwidgets/ImWidgets.cpp
    DEPS    mzlib_imgui
)
target_compile_definitions(mzlib_imwidgets PUBLIC
    CUSTOM_IMWIDGETS_CONFIG="CustomImWidgetsConfig.h"
)
target_include_directories(mzlib_imwidgets PUBLIC
    ${CMAKE_SOURCE_DIR}/src/ui-imgui/filechooser/headers
)

# ----------------------------------------------------------------------------
# Aggregátní INTERFACE knihovna - propojí vše najednou
# ----------------------------------------------------------------------------
add_library(mz_libs INTERFACE)
add_library(mz::libs ALIAS mz_libs)
target_link_libraries(mz_libs INTERFACE
    mzlib_endianity
    mzlib_generic_driver
    mzlib_sharpmz_ascii
    mzlib_sharpmz_koi8cs
    mzlib_vram_sim
    mzlib_mz_vcode
    mzlib_mzglyphs
    mzlib_wav
    mzlib_qd_image
    mzlib_cfgfile
    mzlib_cmt_stream
    mzlib_cmtspeed
    mzlib_cpu_z80
    mzlib_dasm_z80
    mzlib_z80meta
    mzlib_iasm
    mzlib_dsk
    mzlib_mzf
    mzlib_mztape
    mzlib_zxtape
    mzlib_sdlapp
    mzlib_imgui
    mzlib_igfd
    mzlib_imwidgets
)
