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

mz_add_test(snapshot-buffer
    TYPE     EMU
    SOURCES  test_snapshot_buffer.c
    LABELS   "emu;snapshot"
)

# Fronta požadavků na snapshot z GUI: zařazení nic neprovede, vyřídí až
# snapshot_request_poll() (emu vlákno v pauze), výsledky v pořadí zařazení.
mz_add_test(snapshot-request
    TYPE     EMU
    SOURCES  test_snapshot_request.c
    LABELS   "emu;snapshot"
)

# Quick Save a Quick Load: prázdný default_directory = pracovní adresář,
# relativní vůči němu, absolutní beze změny (N96).
mz_add_test(snapshot-quicksave-dir
    TYPE     EMU
    SOURCES  test_snapshot_quicksave_dir.c
    LABELS   "emu;snapshot"
)

# INI sekce [SNAPSHOT] se načte a propaguje do g_snapshot_settings.
mz_add_test(snapshot-config
    TYPE     EMU
    SOURCES  test_snapshot_config.c
    LABELS   "emu;snapshot"
)

# Snapshot uloží a obnoví skrytý stav Z80 (WZ, Q, EI delay, čekající
# INT/NMI, vektor, LD A,I/R, total_cycles) - round-trip přes buffer API.
mz_add_test(snapshot-z80-hidden
    TYPE     EMU
    SOURCES  test_snapshot_z80_hidden.c
    LABELS   "emu;snapshot"
)

# Snapshot uloží a obnoví stav periferií, který dřív vynechával: CTC0
# rychlá cesta (událost + báze), HW scroll MZ-800, FDC pending_rnf_end
# a rt_sinfo - round-trip přes buffer API.
mz_add_test(snapshot-periph-hidden
    TYPE     EMU
    SOURCES  test_snapshot_periph_hidden.c
    LABELS   "emu;snapshot"
)
