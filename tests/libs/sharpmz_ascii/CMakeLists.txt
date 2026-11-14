# Test knihovny sharpmz_ascii (2.2.1): verze, regresní snímek tabulky KOI8-CS
# (expected_koi8cs.txt, vygenerováno 2.2.0 přes --dump) a známé hodnoty Sharp MZ ASCII (EU).
# STANDALONE (ne LIB): test potřebuje jen knihovnu sharpmz_ascii, ne jádro emulátoru.
mz_add_test(sharpmz_ascii
    TYPE     STANDALONE
    SOURCES  test_sharpmz_ascii.c
    LIBS     mzlib_sharpmz_ascii
    LABELS   "standalone;lib;libs"
)
target_compile_definitions(test_sharpmz_ascii PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
