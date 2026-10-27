# tests/devices/pcg700/CMakeLists.txt
# Testy PCG-700: čisté jádro (STANDALONE) a lepidlo devbus (EMU).
#
# Jádro nezávisí na emulátoru, proto se jeho zdroj překládá přímo do testu
# (žádná knihovna, žádné emu core). Model a rozhodnutí: devdoc/pcg700.md.

mz_add_test(pcg700
    TYPE     STANDALONE
    SOURCES  test_pcg700.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/devices/pcg700/pcg700_core.c
    LABELS   "standalone;devices;pcg700"
)

# Lepidlo PCG-700 pro devbus (pcg700_devbus.c): registrace, MMIO 0E010h-0E012h,
# volby, zdroj glyfů, reset, region, katalog I/O, snapshot. Běží v testovacím
# jádru MZ-800 s falešným adaptérem devbus; lepidlo závisí jen na
# arch-neutrálním devbus a g_rom, proto se jeho zdroj (a jádro) překládá
# přímo do testu.
mz_add_test(pcg700_devbus
    TYPE     EMU
    SOURCES  test_pcg700_devbus.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/devices/pcg700/pcg700_devbus.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/devices/pcg700/pcg700_core.c
    LABELS   "emu;devices;pcg700"
)
