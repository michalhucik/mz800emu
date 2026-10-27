# tests/devices/suc80/CMakeLists.txt
# Standalone test - čisté jádro 80sloupcového kitu SUC pro MZ-700
# (src/emulator/mzarch/mz700/devices/suc80/suc80_core.c) + Unity.
#
# Jádro nezávisí na emulátoru, proto se jeho zdroj přidává přímo do testu
# (vzor tests/libs/dasm_export, tests/devices/<id>). Viz devdoc/suc80.md.

mz_add_test(suc80
    TYPE     STANDALONE
    SOURCES  test_suc80.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/devices/suc80/suc80_core.c
    LABELS   "standalone;devices;suc80"
)
