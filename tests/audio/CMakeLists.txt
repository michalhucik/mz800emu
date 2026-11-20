# tests/audio/CMakeLists.txt
# Zachycení zvuku do složky (audio_capture.h) nad emu core; háčky zvukové
# cesty se v testech volají přímo.

mz_add_test(audio_capture
    TYPE     EMU
    SOURCES  test_audio_capture.c
    LABELS   "emu;audio"
)

# Čisté funkce rozboru zvuku (výška tónu, noty, úseky, převzorkování, WAV,
# posun signálů) bez emu core.
mz_add_test(audio_analysis
    TYPE     STANDALONE
    SOURCES  test_audio_analysis.c
             ${CMAKE_SOURCE_DIR}/src/emulator/audio_analysis.c
    LIBS     m
    LABELS   "standalone;audio"
)
target_include_directories(test_audio_analysis PRIVATE ${CMAKE_SOURCE_DIR}/src/emulator)

# Střídavá vazba CTC0 vedená emulátorem v emulovaném čase (audio.c), echo
# kazety stejným kanálem.
mz_add_test(audio_ctc0_ac
    TYPE     EMU
    SOURCES  test_audio_ctc0_ac.c
    LABELS   "emu;audio"
)

# CTC0 v režimu 3 s děličem 2 (554 kHz): do zvukového logu jde místo hran
# stálá úroveň 0,64 maskovaná PC0 (změřeno na MZ-800); přechody na jiný dělič,
# brána, řídicí slovo, echo kazety, stav po načtení snapshotu.
mz_add_test(audio_ctc0_div2
    TYPE     EMU
    SOURCES  test_audio_ctc0_div2.c
    LABELS   "emu;audio"
)

# Hlavní hlasitost a ztlumení poslechu (iface_audio_apply_master()): výchozí
# stav nechá blok bitově shodný, ztlumení dá ticho, zisk 0,5 poloviční
# amplitudu, změna je plynulý přechod přes jeden blok; meze nastavení,
# hlasitost nad 100 % s měkkým omezovačem, rozklad do INI klíčů. Indikace
# úrovní podle špičky a dorovnání hlasitosti v mixu (iface_audio_mix_block()).
mz_add_test(audio_master
    TYPE     EMU
    SOURCES  test_audio_master.c
    LABELS   "emu;audio"
)
