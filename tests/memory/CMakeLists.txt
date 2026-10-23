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

# Čtení mapovaných portů 0E000h-0E008h (režim MZ-700) debuggerem bez
# vedlejšího efektu: autotype, vkbd probe, klopný obvod LSB/MSB 8253.
mz_add_test(mapped_io_peek
    TYPE     EMU
    SOURCES  test_mapped_io_peek.c
    LABELS   "emu;memory;debugger"
)

# Sdílený hash SHA-256 částí stavu (state_hash.c): testovací vektory a co
# patří do hashe RAM / MemExt / VRAM / PCG / obrazu (result.json automatizace).
mz_add_test(state_hash
    TYPE     EMU
    SOURCES  test_state_hash.c
    LABELS   "emu;memory"
)

# Obsahový otisk stroje po částech (machine_hash.c): skladba celkového
# otisku a co vidí části memmap, rom, inputs, cmt, media a ramdisk.
mz_add_test(machine_hash
    TYPE     EMU
    SOURCES  test_machine_hash.c
    LABELS   "emu;memory"
)
