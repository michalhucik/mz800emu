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

# Kód zařízení v BC při přímém spuštění MZF ([CMT] run_mzf_device): zápis
# volby (jména, čísla, neplatné), tabulka pro 700 / 800 / 1500, nastavení,
# klíč INI a BC po bootstrapu.
mz_add_test(bootstrap_device
    TYPE     EMU
    SOURCES  test_bootstrap_device.c
    LABELS   "emu;peripherals;cmt"
)

# Stav registrů GDG MZ-800 po resetu jako na HW (paleta černá, DMD 0,
# okraj 0; simulace netlistu GDG).
mz_add_test(mz800_gdg_reset
    TYPE     EMU
    SOURCES  test_mz800_gdg_reset.c
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

# Rozšíření cmt_tmz: index pásky z TMZ (řídicí bloky CONTROL), otevření bloku
# přes tmz_player, páska bez signálu, vysunutí během pauzy a nové otevření.
mz_add_test(cmt_tmz
    TYPE     EMU
    SOURCES  test_cmt_tmz.c
    LABELS   "emu;peripherals;cmt"
)

# Úpravy vložené pásky: mazání, přesun a vložení bloků, invarianty
# indexu, kurzor, příznak neuloženo, pauza na švu, řídicí bloky, značka
# checkpointu a snapshot po úpravách.
mz_add_test(cmt_tmz_edit
    TYPE     EMU
    SOURCES  test_cmt_tmz_edit.c
    LABELS   "emu;peripherals;cmt"
)

# Analýza surového bloku 0x18: kopie pulzů a její chyby, řádky
# (krátké neznámé úseky vynechané, ZX a překryv souborů), soubory ADDABLE /
# STAYS_RAW / opravené, uplatnění Add a Replace (pulzy surových řádků
# v původním pořadí, pauzy jako záznam, mez playeru), odmítnutí zastaralého
# výsledku, prázdný blok, histogram.
mz_add_test(cmt_tmz_analyze
    TYPE     EMU
    SOURCES  test_cmt_tmz_analyze.c
    LABELS   "emu;peripherals;cmt"
)

# Nástroje nad páskou: informace o pásce (bloky 0x32 a 0x43,
# cizí položky, zápis beze změny), kontrola pásky (nálezy s číslem bloku),
# převod bloku 0x46 na jiný profil zapisovače (MZF bajtově shodné).
mz_add_test(cmt_tmz_meta
    TYPE     EMU
    SOURCES  test_cmt_tmz_meta.c
    LABELS   "emu;peripherals;cmt"
)

# CMT hack nad vloženou páskou: MZF z aktuálního bloku, posun po těle,
# jen hlavička, posun pásky mezi pastmi, bloky bez MZF a s jiným fsize.
mz_add_test(cmthack_tape
    TYPE     EMU
    SOURCES  test_cmthack_tape.c
    LABELS   "emu;peripherals;cmt"
)

# Direct Save: patch zápisových rutin ROM nad všemi vestavěnými obrazy dolní
# ROM (signatura, XOR beze změny, odinstalace vrátí obraz), patch podle
# hacku a nastavení, výměna ROM, zásah menu jen ve STOP, pasti OUT 03h
# (WHEAD / WDATA) a OUT 04h (VERFY). Obrazy ROM MZ-700 a MZ-1500 jsou
# přilinkované jako data (jádro testů je MZ-800).
mz_add_test(cmthack_save
    TYPE     EMU
    SOURCES  test_cmthack_save.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/memory/ROM/ROM_MZ700_13A_0000.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz700/memory/ROM/ROM_MZ700_9B_0000.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz1500/memory/ROM/ROM_MZ1500_0000.c
             ${CMAKE_SOURCE_DIR}/src/emulator/mzarch/mz1500/memory/ROM/ROM_MZNEWMON_0000.c
    LABELS   "emu;peripherals;cmt"
)

# Testovací pásky pro tests/mcp/test_cmt_tmz.py (tests/data/cmt/): generátor
# z malých syntetických MZF; porovná uložené soubory, s MZTEST_REGEN_CMT_DATA=1
# je zapíše.
mz_add_test(cmt_testdata
    TYPE     EMU
    SOURCES  test_cmt_testdata.c
    LABELS   "emu;peripherals;cmt"
)
target_compile_definitions(test_cmt_testdata PRIVATE
    MZTEST_CMT_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/cmt"
)

# Import pásky do TMZ v paměti (cmt_tmz_import.h): zdroj podle přípony,
# MZF a MZT jako bloky 0x46 MZ800EMU-CMT, shoda signálu s mztape.
mz_add_test(cmt_tmz_import
    TYPE     EMU
    SOURCES  test_cmt_tmz_import.c
    LABELS   "emu;peripherals;cmt"
)

# Buffer hran záznamu (cmt_tmz_record.h): úseky mezi hranami, ignorovaná
# stejná úroveň, odmítnutý nerostoucí čas, meze položek a délky (zmenšené
# testovacím setterem), dělení úseku nad UINT32_MAX, begin po discard.
mz_add_test(cmt_tmz_record
    TYPE     EMU
    SOURCES  test_cmt_tmz_record.c
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

# Tentýž soubor obrazu diskety ve více mechanikách (fdc.h): druhé vložení
# je chráněné proti zápisu, zápis do souboru smí jen jedna mechanika.
mz_add_test(fdc-shared-image
    TYPE     EMU
    SOURCES  test_fdc_shared_image.c
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

# Zásahy úprav pásky (cmt_request.h): smazání, přesun, import a export
# celé pásky až v device_request_poll(), kódování seznamu bloků do rozsahů,
# kontrola čítače výměn pásky, odmítnutí při replay, výsledek úpravy pro okno.
mz_add_test(cmt-request-edit
    TYPE     EMU
    SOURCES  test_cmt_request_edit.c
    LABELS   "emu;peripherals;cmt"
)

# Zásahy nástrojů nad páskou (cmt_request.h): kopie pulzů surového
# bloku a její vyzvednutí, uplatnění analýzy (zásah drží odkazy do provedení),
# zápis informací o pásce (kopie formuláře), převod profilu bloku, kontrola
# čítače výměn pásky, odmítnutí při replay, výsledek úpravy pro okno.
mz_add_test(cmt-request-analyze
    TYPE     EMU
    SOURCES  test_cmt_request_analyze.c
    LABELS   "emu;peripherals;cmt"
)

# Zásahy GUI do joysticků (joy_request.h): typ zařízení až
# v device_request_poll(), při replay neprovedeno.
mz_add_test(joy-request
    TYPE     EMU
    SOURCES  test_joy_request.c
    LABELS   "emu;peripherals"
)

# Jádro devbus (sběrnice volitelných zařízení, plán D1): registr, připojení,
# kolize, dispečerské tabulky portů a MMIO, volby a INI, události životního
# cyklu, sekundární video výstup. Adaptér nahrazuje falešný st_DEVBUS_ARCH_OPS,
# zařízení devbus_dummy.c existuje jen v testech.
mz_add_test(devbus
    TYPE     EMU
    SOURCES  test_devbus.c devbus_dummy.c
    LABELS   "emu;peripherals"
)

# devbus: čistá rezoluce čtení sdílené sběrnice (devbus_resolve.c): pravidla
# 1-5 (nebudící, jediný budič + idle, PARTIAL, otevřený kolektor, kolize
# s politikou AND / FIRST / REFUSE), jména a parsování. Bez závislostí.
mz_add_test(devbus_resolve
    TYPE     STANDALONE
    SOURCES  test_devbus_resolve.c
             ${CMAKE_SOURCE_DIR}/src/emulator/hw-generic/devbus/devbus_resolve.c
    LABELS   "standalone;peripherals;devbus"
)

# devbus: komponenta snapshotu "devbus" (round-trip, starší snapshot odpojí,
# neznámé id a vyšší verze stavu = varování, pin INI), hash periph beze změny
# bez připojeného zařízení, zásahy GUI přes device_request (replay odmítne).
mz_add_test(devbus_snapshot
    TYPE     EMU
    SOURCES  test_devbus_snapshot.c devbus_dummy.c
    LABELS   "emu;peripherals;snapshot"
)
