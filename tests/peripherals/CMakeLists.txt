mz_add_test(ctc8253
    TYPE     EMU
    SOURCES  test_ctc8253.c
    LABELS   "emu;peripherals"
)

mz_add_test(pio8255
    TYPE     EMU
    SOURCES  test_pio8255.c
    LABELS   "emu;peripherals"
)

mz_add_test(keyboard_numpad_joy
    TYPE     EMU
    SOURCES  test_keyboard_numpad_joy.c
    LABELS   "emu;peripherals"
)

mz_add_test(psg
    TYPE     EMU
    SOURCES  test_psg.c
    LABELS   "emu;peripherals"
)

mz_add_test(unicard
    TYPE     EMU
    SOURCES  test_unicard.c
    LABELS   "emu;peripherals"
)

mz_add_test(wd279x
    TYPE     EMU
    SOURCES  test_wd279x.c
    LABELS   "emu;peripherals"
)

mz_add_test(gdg_mirror
    TYPE     EMU
    SOURCES  test_gdg_mirror.c
    LABELS   "emu;peripherals"
)

# HW scroll MZ-800: zápis registru uprostřed řádku nesmí přemapovat pixely,
# které paprsek už vykreslil; ladicí settery v jednotkách portu.
mz_add_test(hwscroll
    TYPE     EMU
    SOURCES  test_hwscroll.c
    LABELS   "emu;peripherals"
)

# Bootstrap --run-mzf (MZ-800 mód) nastavuje DMD cestou OUT CEh
# (GATE0 CTC0), ne přímým zápisem g_gdg.regDMD.
mz_add_test(mz800_bootstrap_dmd
    TYPE     EMU
    SOURCES  test_mz800_bootstrap_dmd.c
    LABELS   "emu;peripherals"
)

# Model WAIT u přístupu CPU do VRAM přes GDG (mz800_vramwait.h): případy
# ze simulace netlistu GDG (čtení vždy čeká, zápis přes buffer).
mz_add_test(mz800_vramwait
    TYPE     EMU
    SOURCES  test_mz800_vramwait.c
    LABELS   "emu;peripherals"
)

mz_add_test(printer
    TYPE     EMU
    SOURCES  test_printer.c
    LABELS   "emu;peripherals"
)

mz_add_test(mz1p16_emu
    TYPE     EMU
    SOURCES  test_mz1p16_emu.c
    LABELS   "emu;peripherals;mz1p16"
)

# Stav transportu CMT: invariant "bez pásky STOP", snapshot bez pásky,
# eject/stop bez pásky a přehrání nové pásky od začátku.
mz_add_test(cmt_state
    TYPE     EMU
    SOURCES  test_cmt_state.c
    LABELS   "emu;peripherals;cmt"
)

# Vlastní délky pulzů Virtual CMT a hlavičky CMTSPEED zařízení UniCMT v MZT.
mz_add_test(cmt_unicmt
    TYPE     EMU
    SOURCES  test_cmt_unicmt.c
    LABELS   "emu;peripherals;cmt"
)

# Fronta zásahů GUI do zařízení: zařazení nic neprovede, vyřídí až
# device_request_poll() (emu vlákno), při replay odmítnuto, zámek stavu
# zařízení držený UI zásahy odloží.
mz_add_test(device-request
    TYPE     EMU
    SOURCES  test_device_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do FDC (fdc_request.h): stav FDC se změní až v
# device_request_poll(), při replay se zásah neprovede.
mz_add_test(fdc-request
    TYPE     EMU
    SOURCES  test_fdc_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do Quick Disku (qdisk_request.h): stav QD se změní až
# v device_request_poll(), při replay se zásah neprovede.
mz_add_test(qdisk-request
    TYPE     EMU
    SOURCES  test_qdisk_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do RAM disků (ramdisk_request.h): realokace a odpojení
# až v device_request_poll(), při replay se zásah neprovede.
mz_add_test(ramdisk-request
    TYPE     EMU
    SOURCES  test_ramdisk_request.c
    LABELS   "emu;peripherals"
)

# Záložní soubory RAM disků: relativní cesta (výchozí rd-<platforma>.dat)
# se převádí vůči cfg_dir, ne vůči aktuálnímu adresáři procesu.
mz_add_test(ramdisk-backup-path
    TYPE     EMU
    SOURCES  test_ramdisk_backup_path.c
    LABELS   "emu;peripherals"
)

# Kořen SD karty Unicard a FLASH MemExt Luftner s relativní cestou vůči
# cfg_dir, ne vůči aktuálnímu adresáři procesu.
mz_add_test(cfg-dir-media-paths
    TYPE     EMU
    SOURCES  test_cfg_dir_media_paths.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do MemExt (memext_request.h): připojení a načtení souboru
# až v device_request_poll(), při replay se zásah neprovede.
mz_add_test(memext-request
    TYPE     EMU
    SOURCES  test_memext_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do IDE8 a Unicardu (ide8_request.h, unicard_request.h):
# změna až v device_request_poll(), při replay se zásah neprovede.
mz_add_test(ide8-unicard-request
    TYPE     EMU
    SOURCES  test_ide8_unicard_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do tiskárny (printer_request.h): standard, zachytávání
# a zavření souboru až v device_request_poll(), při replay neprovedeno.
mz_add_test(printer-request
    TYPE     EMU
    SOURCES  test_printer_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do přepínačů stroje (mzarch_request.h): SW1 a PSG1 až
# v device_request_poll(), při replay neprovedeno.
mz_add_test(mzarch-request
    TYPE     EMU
    SOURCES  test_mzarch_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do ROM (rom_request.h): výběr ROM a nastavení uživatelské
# ROM až v device_request_poll(), při replay neprovedeno.
mz_add_test(rom-request
    TYPE     EMU
    SOURCES  test_rom_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do kazety (cmt_request.h): otevření, rychlost, pulzy, rychlost
# bloku, CMT hack, Fix FNAME a polarita až v device_request_poll(), při
# replay neprovedeno.
mz_add_test(cmt-request
    TYPE     EMU
    SOURCES  test_cmt_request.c
    LABELS   "emu;peripherals"
)

# Zásahy GUI do joysticků (joy_request.h): typ zařízení až
# v device_request_poll(), při replay neprovedeno.
mz_add_test(joy-request
    TYPE     EMU
    SOURCES  test_joy_request.c
    LABELS   "emu;peripherals"
)
