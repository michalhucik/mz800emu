# ============================================================================
# RuntimeAssets.cmake - zpřístupní read-only assety vedle binárky v build stromu
#
# Skript pro `cmake -P`, volaný při buildu z mz_add_emulator() (viz
# cmake/AddMzEmu.cmake, target <emu>-assets).
#
# Proč: emulátor hledá assety relativně k adresáři binárky (home_dir, viz
# src/libs/sdlapp/sdlapp_paths.c). Binárka vzniká v build/build-<target>/,
# kde assety nejsou - bez tohoto kroku jde po čistém CMake buildu spustit
# jen s --home-dir nebo po zkopírování binárky do kořene repa (to dělá
# Makefile wrapper).
#
# Vstupní proměnné (-D před -P):
#   MZ_ASSETS_SOURCE_DIR - kořen zdrojového stromu
#   MZ_ASSETS_DEST_DIR   - adresář binárky (build/build-<target>)
#
# Co vznikne v MZ_ASSETS_DEST_DIR:
#   ui_resources/ <- <src>/ui_resources   (fonty, obrázky, klávesnice)
#   certs/        <- <src>/certs          (cacert.pem pro kontrolu verze)
#   locale/       <- <src>/src/locale     (katalogy .mo, pokud jsou přeložené)
#
# Způsob:
#   - mimo Windows symbolický odkaz na zdrojový adresář (nic se nekopíruje,
#     změna assetu se projeví hned),
#   - na Windows kopie. Odkaz ani junction se tam záměrně nedělá: `rm -rf build`
#     z MSYS2 by přes něj mohl smazat zdrojový adresář. file(COPY) přeskakuje
#     soubory se shodným časem změny, opakovaný běh je proto levný. Z locale
#     se kopírují jen katalogy .mo.
#
# Omezení: soubor smazaný ze zdroje na Windows v kopii zůstane (do
# `make clean`). Existující skutečný adresář v cíli se na odkaz nepřevádí.
# ============================================================================

if(NOT MZ_ASSETS_SOURCE_DIR OR NOT MZ_ASSETS_DEST_DIR)
    message(FATAL_ERROR "RuntimeAssets.cmake: MZ_ASSETS_SOURCE_DIR and MZ_ASSETS_DEST_DIR must be set")
endif()

file(MAKE_DIRECTORY "${MZ_ASSETS_DEST_DIR}")

# mz_runtime_asset(<name> <zdroj> [<file(COPY) filtr>...])
#
# Zpřístupní adresář <zdroj> jako ${MZ_ASSETS_DEST_DIR}/<name>. Chybějící
# zdroj tiše přeskočí (assety jsou z pohledu buildu volitelné). Další
# argumenty se předají file(COPY) a uplatní se jen u kopie na Windows.
function(mz_runtime_asset name source)
    set(_dest "${MZ_ASSETS_DEST_DIR}/${name}")
    if(NOT IS_DIRECTORY "${source}")
        return()
    endif()

    if(CMAKE_HOST_WIN32)
        file(COPY "${source}/" DESTINATION "${_dest}" ${ARGN})
        return()
    endif()

    if(IS_SYMLINK "${_dest}")
        file(REMOVE "${_dest}")
    elseif(EXISTS "${_dest}")
        return()
    endif()
    file(CREATE_LINK "${source}" "${_dest}" SYMBOLIC)
endfunction()

mz_runtime_asset(ui_resources "${MZ_ASSETS_SOURCE_DIR}/ui_resources")
mz_runtime_asset(certs        "${MZ_ASSETS_SOURCE_DIR}/certs")
mz_runtime_asset(locale       "${MZ_ASSETS_SOURCE_DIR}/src/locale"
                 FILES_MATCHING PATTERN "*.mo")
