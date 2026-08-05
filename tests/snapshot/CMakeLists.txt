mz_add_test(snapshot-io
    TYPE     EMU
    SOURCES  test_snapshot_io.c
    LABELS   "emu;snapshot"
)

mz_add_test(snapshot-xml
    TYPE     EMU
    SOURCES  test_snapshot_xml.c
    LABELS   "emu;snapshot"
)

mz_add_test(snapshot-mgr
    TYPE     EMU
    SOURCES  test_snapshot_mgr.c
    LABELS   "emu;snapshot"
)

# Per-arch snapshot round-trip + arch/tvsys reject matice (mzhal krok 14):
# tentýž arch-agnostický zdroják proti core každé další konfigurace.
# Pokrývá i mz700-ntsc větev legacy tvsys policy (default PAL -> reject).
mz_add_test(snapshot-mgr_mz700_pal
    TYPE     EMU
    CORE     mz700_pal
    SOURCES  test_snapshot_mgr.c
    LABELS   "emu;snapshot;arch700"
)

mz_add_test(snapshot-mgr_mz700_ntsc
    TYPE     EMU
    CORE     mz700_ntsc
    SOURCES  test_snapshot_mgr.c
    LABELS   "emu;snapshot;arch700"
)

mz_add_test(snapshot-mgr_mz1500
    TYPE     EMU
    CORE     mz1500
    SOURCES  test_snapshot_mgr.c
    LABELS   "emu;snapshot;arch1500"
)

mz_add_test(snapshot-buffer
    TYPE     EMU
    SOURCES  test_snapshot_buffer.c
    LABELS   "emu;snapshot"
)
