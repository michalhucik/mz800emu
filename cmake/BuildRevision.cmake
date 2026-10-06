# cmake/BuildRevision.cmake
#
# Generuje src/build_revision/build_revision.c při každém buildu spuštěním
# tools/generate_build_revision_git.sh.
#
# Skript ověřuje, že některý remote ukazuje na oficiální upstream
# github.com/michalhucik/mz800emu. Pokud ano, revize = 250 + git rev-list
# --count HEAD. Offset 250 zachovává kontinuitu číslování revizí po
# přechodu projektu ze SourceForge (SVN) na GitHub (git) - historické
# SVN revize skončily pod hodnotou 250, takže nové git číslování
# pokračuje vždy nad poslední vydanou SF revizí a žádné dříve
# distribuované číslo revize se nikdy znovu nepoužije.
# Pokud ne (fork, lokální klon, mirror), revize = -1.
#
# Dále zapisuje původ zdrojů (GitHub upstream / repozitář NAS1 / neznámý;
# NAS1 se pozná i přes řetěz lokálních klonů), větev, hash commitu, příznak
# neuložených změn a MSYSTEM buildu - viz src/build_revision/build_revision.h.
# Chybějící git informace build nikdy nezastaví: skript zapíše hodnoty
# "unknown" a skončí s kódem 0. Soubor přepíše jen při změně obsahu.
#
# Skript se spouští při každém volání cmake --build (custom_command s
# nezávislým add_custom_target), takže zachycuje stav repozitáře
# (revize, commit, dirty) v okamžiku buildu.
#
# Po include() je dostupný:
#   - target mz_build_revision (custom target)
#   - proměnná MZ_BUILD_REVISION_C (cesta k vygenerovanému .c souboru)

set(MZ_BUILD_REVISION_C "${CMAKE_SOURCE_DIR}/src/build_revision/build_revision.c")
set(MZ_BUILD_REVISION_SCRIPT "${CMAKE_SOURCE_DIR}/tools/generate_build_revision_git.sh")

# Custom target který vždy regeneruje build_revision.c.
# Použijeme add_custom_target (ne add_custom_command na výstupní soubor),
# protože chceme aby se to volalo při každém buildu, ne jen když chybí.
add_custom_target(mz_build_revision
    BYPRODUCTS ${MZ_BUILD_REVISION_C}
    COMMAND bash ${MZ_BUILD_REVISION_SCRIPT} ${MZ_BUILD_REVISION_C}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating build_revision.c"
    VERBATIM
)
