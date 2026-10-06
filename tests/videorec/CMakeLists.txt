# tests/videorec/CMakeLists.txt
# Standalone testy čistých částí video záznamu (bez emu core).

mz_add_test(videorec_audio
    TYPE     STANDALONE
    SOURCES  test_videorec_audio.c ${CMAKE_SOURCE_DIR}/src/emulator/videorec/videorec_audio.c
    LIBS     m
    LABELS   "standalone;videorec"
)

# Parita zvuku se SDL cestou emulátoru (referenční kopie matematiky iface_audio_resampler.c).
mz_add_test(videorec_audio_sdl_parity
    TYPE     STANDALONE
    SOURCES  test_videorec_audio_sdl_parity.c ${CMAKE_SOURCE_DIR}/src/emulator/videorec/videorec_audio.c
    LIBS     m
    LABELS   "standalone;videorec"
)

mz_add_test(videorec_sidecar
    TYPE     STANDALONE
    SOURCES  test_videorec_sidecar.c ${CMAKE_SOURCE_DIR}/src/emulator/videorec/videorec_sidecar.c
    LIBS     mz::glib
    LABELS   "standalone;videorec"
)

mz_add_test(videorec_pipe
    TYPE     STANDALONE
    SOURCES  test_videorec_pipe.c ${CMAKE_SOURCE_DIR}/src/emulator/videorec/videorec_pipe.c ${CMAKE_SOURCE_DIR}/src/emulator/videorec/videorec_audio.c
    LIBS     mz::glib m
    LABELS   "standalone;videorec"
)

# Integrační test retake / švu: lepidlo + snapshot manager + writer vlákno (emu core bez emu vlákna).
mz_add_test(videorec_snapshot
    TYPE     EMU
    SOURCES  test_videorec_snapshot.c
    LABELS   "emu;videorec;snapshot"
)

# Události nahrávání (pro UI), stop v pauze emulace, výstupní adresář a INI modul VIDEOREC.
mz_add_test(videorec_events
    TYPE     EMU
    SOURCES  test_videorec_events.c
    LABELS   "emu;videorec"
)

# Režim "podle reality": hodiny vzorkovače, převzorkování SDL výstupu a korekce driftu.
mz_add_test(videorec_rt
    TYPE     STANDALONE
    SOURCES  test_videorec_rt.c ${CMAKE_SOURCE_DIR}/src/emulator/videorec/videorec_rt.c
    LIBS     mz::glib m
    LABELS   "standalone;videorec"
)

# Režim "podle reality" v lepidle: vzorkovač v ručním režimu, přepínání časové základny,
# pauza / krokování / rychlost, události stavu v sidecaru (od verze 3).
mz_add_test(videorec_realtime
    TYPE     EMU
    SOURCES  test_videorec_realtime.c
    LABELS   "emu;videorec"
)

# Parametry platformy video záznamu pro všechny čtyři exe. Sonda
# videorec_platform_probe.c se překládá pro každou platformu zvlášť (vlastní
# MZARCH / MZTVSYS, bez defines test frameworku, který je napevno MZ-800),
# test pak porovná skutečné hodnoty per-arch maker s očekáváním.
set(_vr_probe_targets)
foreach(_probe IN ITEMS "mz800;800;PAL" "mz1500;1500;NTSC" "mz700pal;700;PAL" "mz700ntsc;700;NTSC")
    list(GET _probe 0 _id)
    list(GET _probe 1 _arch)
    list(GET _probe 2 _tv)
    add_library(videorec_platform_probe_${_id} OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/videorec_platform_probe.c)
    # Stejné per-arch defines jako exe (jediný zdroj v cmake/AddMzEmu.cmake).
    mz_arch_compile_definitions(videorec_platform_probe_${_id} ${_arch} ${_tv})
    target_compile_definitions(videorec_platform_probe_${_id} PRIVATE VR_PROBE_FN=vr_probe_${_id})
    target_include_directories(videorec_platform_probe_${_id} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/emulator
        ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic
    )
    list(APPEND _vr_probe_targets videorec_platform_probe_${_id})
endforeach()

mz_add_test(videorec_platform
    TYPE     STANDALONE
    SOURCES  test_videorec_platform.c ${CMAKE_SOURCE_DIR}/src/emulator/videorec/videorec_platform.c
    LIBS     ${_vr_probe_targets}
    LABELS   "standalone;videorec"
)
