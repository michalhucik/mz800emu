mz_add_test(memory
    TYPE     EMU
    SOURCES  test_memory.c
    LABELS   "emu;memory"
)

mz_add_test(memext
    TYPE     EMU
    SOURCES  test_memext.c
    LABELS   "emu;memory"
)

mz_add_test(regions
    TYPE     EMU
    SOURCES  test_regions.c
    LABELS   "emu;memory;debugger"
)

# Okno MemExt Map Settings: přemapování (memext_map_pwrite + memory_reconnect_ram)
# provádí emu vlákno přes memext_map_request / _poll, ne přímo UI vlákno.
mz_add_test(memext_map_request
    TYPE     EMU
    SOURCES  test_memext_map_request.c
    LABELS   "emu;memory"
)
