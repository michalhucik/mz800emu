#!/bin/bash
# check_i18n_core.sh — ověří, že emulátorové jádro neobsahuje _() gettext volání
#
# Emulátorové jádro (src/emulator/) nesmí záviset na lokalizaci.
# Veškeré _() patří do UI vrstvy (src/ui-imgui/, src/ui/).
#
# Použití: ./tools/check_i18n_core.sh [--strict]
#   --strict: selhání = nenulový exit kód (pro budoucí CI)
#   bez --strict: jen warning (výchozí)
#
# Licence: GPLv3

STRICT=0
if [ "$1" = "--strict" ]; then
    STRICT=1
fi

SEARCH_DIR="src/emulator"

echo "=== i18n: kontrola jádra emulátoru ==="
echo ""

# Známé výjimky — funkce v jádře volané z UI vrstvy, kde _() je legitimní:
#   audio.c:audio_print_volume_info()      — konzolový výpis při init
#   emulator.c:emulator_get_speed_status_as_text() — text pro window title
#   cmthack.c:cmthack_load_file()          - file chooser z pasti hlavičky CMT hacku
#   fdc.c:fdc_ui_mount()                   — file chooser volaný z menu_fdcontroller.cpp
#   qdisk.c:qdisk_ui_mount()               — file chooser volaný z menu_qdisk.cpp
#   unicard.c:unicard_ui_select_sd_root_directory() — file chooser volaný z menu_unicard.cpp
KNOWN_EXCEPTIONS=(
    "src/emulator/audio.c:"
    "src/emulator/emulator.c:"
    "src/emulator/hw-generic/cmt/cmt.c:"
    "src/emulator/hw-generic/cmt/cmthack.c:"
    "src/emulator/hw-generic/fdc/fdc.c:"
    "src/emulator/hw-generic/qdisk/qdisk.c:"
    "src/emulator/hw-generic/unicard/unicard.c:"
)

# Hledáme \b_( — skutečné gettext volání
# Filtrujeme:
#   - komentáře (//)
#   - #define (definice maker)
#   - IM_() — Z80 opcode makra
#   - opcodes/ — Z80 opcode soubory (obsahují IM_ makro)
#   - __attribute__ a podobné GCC interní makra
RESULTS=$(grep -rn '\b_(' "$SEARCH_DIR" \
    --include="*.c" --include="*.h" \
    | grep -v '//' \
    | grep -v '#define' \
    | grep -v 'IM_(' \
    | grep -v 'opcodes/' \
    | grep -v '__attribute__' \
    | grep -v '_Bool' \
    | grep -v '_Pragma' \
    | grep -v '_Static_assert' \
    | grep -v '_Alignof' \
    | grep -v '_Generic' \
    | grep -v '_Noreturn' \
    | grep -v '_Thread_local' \
    | grep -v '_Atomic')

# odfiltrovat známé výjimky
FILTERED="$RESULTS"
for pattern in "${KNOWN_EXCEPTIONS[@]}"; do
    FILTERED=$(echo "$FILTERED" | grep -v "$pattern")
done

KNOWN_COUNT=$(echo "$RESULTS" | grep -c . 2>/dev/null || echo 0)
if [ -z "$RESULTS" ]; then
    KNOWN_COUNT=0
fi

UNKNOWN_COUNT=$(echo "$FILTERED" | grep -c . 2>/dev/null || echo 0)
if [ -z "$FILTERED" ]; then
    UNKNOWN_COUNT=0
fi

EXCEPTION_COUNT=$((KNOWN_COUNT - UNKNOWN_COUNT))

if [ "$UNKNOWN_COUNT" -gt 0 ]; then
    if [ "$STRICT" -eq 1 ]; then
        echo "FAIL: jádro obsahuje $UNKNOWN_COUNT NOVÝCH výskytů _() gettext volání:"
    else
        echo "WARNING: jádro obsahuje $UNKNOWN_COUNT NOVÝCH výskytů _() gettext volání:"
    fi
    echo "$FILTERED"
    echo ""
    echo "Tyto _() by měly být přesunuty do UI vrstvy nebo přidány do výjimek."
    echo "(Známých výjimek: $EXCEPTION_COUNT — funkce v jádře volané z UI vrstvy)"
    if [ "$STRICT" -eq 1 ]; then
        exit 1
    fi
    exit 0
else
    echo "PASS: jádro emulátoru je čisté (známých výjimek: $EXCEPTION_COUNT)"
    exit 0
fi
