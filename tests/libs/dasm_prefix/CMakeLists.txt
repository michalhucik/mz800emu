# tests/libs/dasm_prefix/CMakeLists.txt
# Standalone test - zobrazeni ignorovaneho prefixu DD/FD (DD*, FD*)
# a nedefinovane ED (NOP*) v dasm-z80 vcetne metadat z80meta.

mz_add_test(dasm_prefix
    TYPE     STANDALONE
    SOURCES  test_dasm_prefix.c
    LIBS     mzlib_dasm_z80 mzlib_z80meta
    LABELS   "standalone;libs;debugger"
)
