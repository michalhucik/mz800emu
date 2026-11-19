# Historie naposledy vložených médií (src/emulator/recent_media/): pořadí,
# strop, opakované vložení, odstranění, čas, uložení do INI a nové načtení
# (cesta s diakritikou a přes 260 znaků) a zápis z míst, kde vložení
# diskety, pásky a Quick Disku uspěje (fdc_mount_dskfile(), cmt_open_file(),
# qdisk_mount_path()).
mz_add_test(recent_media
    TYPE     EMU
    SOURCES  test_recent_media.c
    LABELS   "emu;recent_media"
)
