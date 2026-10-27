# tests/devices/kp80/CMakeLists.txt
#
# Standalone test čistého jádra 80-Zeichen-Karte K&P (CRTC SY6845E
# v transparentním režimu, VRAM 2 kB, font 80Z.ROM, vykreslení snímku
# druhého monitoru). Jádro nezávisí na emulátoru, překládá se přímo ze
# zdrojů v src/emulator/mzarch/mz700/devices/kp80/. Model: devdoc/kp80.md.

mz_add_test(kp80_core
    TYPE     STANDALONE
    SOURCES  test_kp80_core.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/devices/kp80/kp80_core.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/devices/kp80/kp80_font_rom.c
    LABELS   "standalone;devices;kp80"
)
