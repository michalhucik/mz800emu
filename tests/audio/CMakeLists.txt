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

# Převod snímků na skutečný čas podle krystalu ve zvukové cestě: akumulátor
# délky bloku SDL cesty bez driftu (PAL i NTSC), termíny tempa bez zvukového
# zařízení, násobitel CTC0 pro skutečnou délku bloku.
mz_add_test(audio_exact_clock
    TYPE     EMU
    SOURCES  test_audio_exact_clock.c
    LABELS   "emu;audio"
)

# Převod CTC0 SDL cesty (iface_audio_resampler_ctc0_run()): vnitřní buňky
# intervalu se jen počítají, krajní se váží; výstup musí být bitově stejný
# jako u dřívějšího vážení každé buňky (referenční kopie v testu), PAL i NTSC.
mz_add_test(audio_ctc0_resample_equiv
    TYPE     EMU
    SOURCES  test_audio_ctc0_resample_equiv.c
    LABELS   "emu;audio"
)

# Průměr výchylky PSG přes interval výstupního vzorku SDL cesty
# (iface_audio_resampler_psg_average()): bitová shoda s integrálem po taktech
# na globální ose, přenos kusu vzorku přes hranici bloku, PAL i NTSC,
# 100 %, 50 % a 200 %, navazující i nenavazující log.
mz_add_test(audio_psg_average
    TYPE     EMU
    SOURCES  test_audio_psg_average.c
    LABELS   "emu;audio"
)

# Zrychlené cesty převodu PSG (kanál beze změny v logu) jsou bitově shodné
# s dřívější implementací (doslovná kopie z f9da3035 v testu): průměry,
# hodnoty na konci intervalu, stav přenosu i výstup kanálu, PAL i NTSC,
# 100 %, 50 % a 200 %.
mz_add_test(audio_psg_average_equiv
    TYPE     EMU
    SOURCES  test_audio_psg_average_equiv.c
    LABELS   "emu;audio"
)
