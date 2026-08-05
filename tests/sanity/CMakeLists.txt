mz_add_test(sanity
    TYPE     EMU
    SOURCES  test_sanity.c
    LABELS   "emu;sanity"
)

# Per-arch sanity boot (mzhal krok 4b): tentýž arch-agnostický zdroják
# proti core každé další konfigurace - ověřuje, že se emulator core dané
# platformy dá zkompilovat, slinkovat a inicializovat headless.
mz_add_test(sanity_mz700_pal
    TYPE     EMU
    CORE     mz700_pal
    SOURCES  test_sanity.c
    LABELS   "emu;sanity;arch700"
)

mz_add_test(sanity_mz700_ntsc
    TYPE     EMU
    CORE     mz700_ntsc
    SOURCES  test_sanity.c
    LABELS   "emu;sanity;arch700"
)

mz_add_test(sanity_mz1500
    TYPE     EMU
    CORE     mz1500
    SOURCES  test_sanity.c
    LABELS   "emu;sanity;arch1500"
)
