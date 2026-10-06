# tests/libs/avirec/CMakeLists.txt
# Standalone testy knihovny mzlib_avirec (ZMBV enkodér, AVI zapisovač).

mz_add_test(zmbv_enc
    TYPE     STANDALONE
    SOURCES  test_zmbv_enc.c
    LIBS     mzlib_avirec ZLIB::ZLIB
    LABELS   "standalone;libs"
)

mz_add_test(avi_writer
    TYPE     STANDALONE
    SOURCES  test_avi_writer.c
    LIBS     mzlib_avirec
    LABELS   "standalone;libs"
)

find_program(MZ_FFMPEG_EXE ffmpeg)
if(NOT MZ_FFMPEG_EXE)
    set(MZ_FFMPEG_EXE "")
endif()
mz_add_test(avirec_ffmpeg
    TYPE     STANDALONE
    SOURCES  test_avirec_ffmpeg.c
    LIBS     mzlib_avirec mz::glib
    ARGS     "${MZ_FFMPEG_EXE}"
    LABELS   "standalone;libs"
)
# Test spouští externí ffmpeg (desítky DLL). Občasné selhání v plném běhu
# (nenulový status, prázdný stderr; příčina neověřena - hypotéza: selhání
# startu procesu pod zátěží) -> nikdy souběžně s jinými testy, ani při
# ctest -j / CTEST_PARALLEL_LEVEL.
set_tests_properties(avirec_ffmpeg PROPERTIES RUN_SERIAL TRUE)
