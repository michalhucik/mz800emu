# Smoke test playeru TMZ (profil ROM-800-NOMINAL -> blok -> CMT stream)
mz_add_test(tmz_player
    TYPE     LIB
    SOURCES  test_tmz_player.c
    LABELS   "lib;libs;cmt"
)
