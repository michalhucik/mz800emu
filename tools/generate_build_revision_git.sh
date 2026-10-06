#!/bin/bash
#
# generate_build_revision_git.sh - generátor src/build_revision/build_revision.c
#
# Volá se při každém buildu (cmake/BuildRevision.cmake, target
# mz_build_revision). Z git metadat zdrojového stromu zjistí:
#
#   - číslo revize (jen oficiální upstream klon github.com/michalhucik/mz800emu,
#     REV = 250 + počet commitů na HEAD; jinak -1 / "???"),
#   - původ zdrojů (GitHub upstream / repozitář NAS1 / neznámý),
#   - větev (nebo příznak detached HEAD), plný a zkrácený hash commitu,
#   - příznak neuložených změn (dirty) ve sledovaných souborech,
#   - MSYSTEM prostředí, ve kterém build běží (MSYS2 toolchain).
#
# Robustnost: chybějící git, strom bez .git (zdrojový archiv), strom uvnitř
# cizího repozitáře, repozitář bez remotů, detached HEAD, shallow klon nebo
# selhání libovolného git příkazu NIKDY nezpůsobí chybu - do výstupu se zapíší
# hodnoty "unknown" a skript skončí s kódem 0. Nenulový kód vrací jen při
# chybném volání (chybí argument) nebo když nelze zapsat výstupní soubor.
#
# Skript nikdy nekontaktuje síť: čte jen lokální konfiguraci remotů
# (git remote get-url), nikdy nevolá fetch/ls-remote.
#
# Výstupní soubor se přepíše jen tehdy, když se jeho obsah změnil (jinak by
# každý build zbytečně překládal a linkoval všechny binárky).
#
# Všechny hlášky skriptu jsou anglicky (výstup buildu).

# Print help function
print_help() {
    echo "Usage: $0 <output_file>"
    echo ""
    echo "This script creates a C source file containing build information"
    echo "derived from git: revision number, source origin, branch, commit"
    echo "and the dirty flag of the working tree."
    echo ""
    echo "The revision is only generated for the official upstream repository"
    echo "github.com/michalhucik/mz800emu (HTTPS, SSH or gh CLI form). For any"
    echo "other clone (fork, mirror, local-only repo) the revision is set to -1."
    echo ""
    echo "Calculation:"
    echo "  - upstream:   REV_INT = 250 + total commit count on HEAD"
    echo "                (offset 250 preserves revision-number continuity"
    echo "                after the project's migration from SourceForge"
    echo "                (SVN) to GitHub (git); historical SVN revisions"
    echo "                ended below 250, so the new git numbering starts"
    echo "                strictly above the last released SF revision)"
    echo "  - otherwise:  REV_INT = -1, REV_TEXT = \"???\""
    echo ""
    echo "Source origin:"
    echo "  - GitHub:     a remote points to github.com/michalhucik/mz800emu"
    echo "  - NAS1:       a remote host is 'nas1' (or 'nas1.<domain>'); remotes"
    echo "                that are local repository paths are followed up to"
    echo "                $MAX_LOCAL_DEPTH levels deep (cycle-safe)"
    echo "  - unknown:    anything else (fork, no remotes, no git)"
    echo ""
    echo "Missing git information never fails: \"unknown\" values are written"
    echo "and the script exits with 0."
    echo ""
    echo "Intended for use during the build process (Makefile or CMake)."
    echo ""
    echo "Arguments:"
    echo "  <output_file>   The path to the output C source file."
}

# Maximální hloubka sledování remotů, které jsou lokální cestou k jinému
# repozitáři (mutant klon -> hlavní klon -> ...). Hloubka 0 = vlastní
# repozitář zdrojového stromu.
MAX_LOCAL_DEPTH=4

# Check number of arguments
if [ "$#" -ne 1 ]; then
    echo "Error: Missing output file argument."
    print_help
    exit 1
fi

OUTPUT_FILE="$1"
SCRIPT_PATH="$(realpath "$0" 2>/dev/null || echo "$0")"

# Výstupní cestu ukotvíme absolutně dřív, než se změní pracovní adresář.
case "$OUTPUT_FILE" in
    /*|[A-Za-z]:[/\\]*) ;;
    *) OUTPUT_FILE="$(pwd)/$OUTPUT_FILE" ;;
esac

# Kořen zdrojového stromu = rodič adresáře tools/, ve kterém skript leží.
# Všechny git dotazy se vztahují k němu, ne k náhodnému pracovnímu adresáři.
SOURCE_ROOT="$(cd "$(dirname "$SCRIPT_PATH")/.." 2>/dev/null && pwd)"
if [ -n "$SOURCE_ROOT" ]; then
    cd "$SOURCE_ROOT" 2>/dev/null || true
fi

# Offset zachovává kontinuitu číslování revizí po přechodu projektu
# ze SourceForge (SVN) na GitHub (git). Historické SVN revize na
# SourceForge skončily pod hodnotou 250; první git revize tedy
# pokračuje od 250 + 1 a žádné dříve vydané číslo revize se nikdy
# znovu nepoužije. Díky tomu zůstanou všechna stará čísla revizí
# (uvedená v binárkách distribuovaných ze SourceForge, ve zprávách
# o chybách, screenshotech atd.) jednoznačně identifikovatelná.
REV_OFFSET=250

# Regex, který identifikuje oficiální upstream repozitář. Pokrývá všechny
# obvyklé varianty URL, které git/gh produkují:
#   https://github.com/michalhucik/mz800emu(.git)?
#   git@github.com:michalhucik/mz800emu(.git)?
#   ssh://git@github.com/michalhucik/mz800emu(.git)?
# Porovnání je case-insensitive (GitHub URL jsou v praxi case-insensitive
# v host části i v path části jména repozitáře).
UPSTREAM_REGEX='github\.com[:/]michalhucik/mz800emu(\.git)?/?$'

# Regex pro jméno hostitele repozitáře NAS1 (case-insensitive): přesně
# "nas1" nebo plně kvalifikované "nas1.<doména>".
NAS1_HOST_REGEX='^nas1(\..+)?$'

echo "Generating build revision file: $OUTPUT_FILE"

# Výchozí hodnoty (= "neznámý / nepodporovaný repozitář")
REV_TEXT="???"
REV_INT="-1"
ORIGIN_ENUM="BUILD_REVISION_ORIGIN_UNKNOWN"
ORIGIN_NAME="unknown"
BRANCH="unknown"
DETACHED=0
COMMIT="unknown"
COMMIT_SHORT="unknown"
DIRTY=-1
BUILD_MSYSTEM="${MSYSTEM:-}"


# ---------------------------------------------------------------------------
# Pomocné funkce
# ---------------------------------------------------------------------------

# c_escape <text>
# Vypíše text upravený pro bezpečné vložení do C řetězcového literálu:
# zpětné lomítko, uvozovky a otazník (trigrafy) escapuje, řídicí znaky
# a bajty mimo tisknutelné ASCII zapíše jako oktalové escape \ooo.
c_escape() {
    local LC_ALL=C
    local s="$1" out="" c code i
    for (( i = 0; i < ${#s}; i++ )); do
        c="${s:i:1}"
        case "$c" in
            '\') out+='\\' ;;
            '"') out+='\"' ;;
            '?') out+='\?' ;;
            *)
                printf -v code '%d' "'$c"
                if (( code < 32 || code >= 127 )); then
                    printf -v c '\\%03o' $(( code & 255 ))
                fi
                out+="$c"
                ;;
        esac
    done
    printf '%s' "$out"
}

# remote_local_path <url>
# Pokud URL remotu označuje lokální cestu (absolutní/relativní cesta,
# Windows cesta s písmenem disku, file://), vypíše ji a vrátí 0.
# Pro síťovou URL (scheme://..., scp tvar host:cesta) vrátí 1.
remote_local_path() {
    local url="$1"
    case "$url" in
        file://*) printf '%s\n' "${url#file://}"; return 0 ;;
    esac
    if [[ "$url" =~ ^[A-Za-z]:[/\\] ]]; then
        printf '%s\n' "$url"; return 0
    fi
    if [[ "$url" =~ ^[A-Za-z][A-Za-z0-9+.-]*:// ]]; then
        return 1
    fi
    # scp tvar ([user@]host:cesta): dvojtečka dřív než první lomítko.
    if [[ "$url" =~ ^[^/]*: ]]; then
        return 1
    fi
    printf '%s\n' "$url"
    return 0
}

# remote_host <url>
# Vypíše jméno hostitele ze síťové URL (scheme://[user@]host[:port]/...
# nebo scp tvar [user@]host:cesta). Vrátí 1, pokud host nelze určit.
remote_host() {
    local url="$1" rest
    if [[ "$url" =~ ^[A-Za-z][A-Za-z0-9+.-]*://([^/]*) ]]; then
        rest="${BASH_REMATCH[1]}"
        rest="${rest##*@}"
        rest="${rest%%:*}"
        [ -n "$rest" ] || return 1
        printf '%s\n' "$rest"
        return 0
    fi
    if [[ "$url" =~ ^([^/@:]+@)?([^/:]+): ]]; then
        printf '%s\n' "${BASH_REMATCH[2]}"
        return 0
    fi
    return 1
}

# repo_remote_urls <dir>
# Vypíše (po řádcích) všechny fetch i push URL všech remotů repozitáře
# v adresáři <dir>. Při jakékoliv chybě nevypíše nic.
repo_remote_urls() {
    local dir="$1" remote kind url
    local -a remotes=()
    mapfile -t remotes < <(git -C "$dir" remote 2>/dev/null </dev/null)
    for remote in "${remotes[@]}"; do
        [ -z "$remote" ] && continue
        for kind in fetch push; do
            url=$(git -C "$dir" remote get-url --"$kind" "$remote" 2>/dev/null </dev/null || true)
            [ -n "$url" ] && printf '%s\n' "$url"
        done
    done
}

# Množina již navštívených repozitářů (absolutní git dir) - ochrana proti cyklu.
VISITED="|"
NAS1_URL=""

# repo_reaches_nas1 <dir> <depth>
# Vrátí 0, pokud některý remote repozitáře <dir> ukazuje na hostitele NAS1,
# nebo pokud remote je lokální cesta k repozitáři, který (rekurzivně, nejvýše
# do hloubky MAX_LOCAL_DEPTH) na NAS1 ukazuje. Nalezenou URL uloží do NAS1_URL.
repo_reaches_nas1() {
    local dir="$1" depth="$2" key base url lp abs host
    local -a urls=()

    key=$(git -C "$dir" rev-parse --absolute-git-dir 2>/dev/null </dev/null) || return 1
    [ -n "$key" ] || return 1
    case "$VISITED" in
        *"|$key|"*) return 1 ;;
    esac
    VISITED+="$key|"

    # Relativní lokální cesty remotů se vztahují ke kořeni pracovního stromu
    # (u bare repozitáře ke git adresáři).
    base=$(git -C "$dir" rev-parse --show-toplevel 2>/dev/null </dev/null) || base=""
    [ -n "$base" ] || base="$key"

    mapfile -t urls < <(repo_remote_urls "$dir")
    for url in "${urls[@]}"; do
        [ -z "$url" ] && continue
        if lp=$(remote_local_path "$url"); then
            [ "$depth" -lt "$MAX_LOCAL_DEPTH" ] || continue
            case "$lp" in
                /*|[A-Za-z]:[/\\]*) abs="$lp" ;;
                *) abs="$base/$lp" ;;
            esac
            [ -d "$abs" ] || continue
            if repo_reaches_nas1 "$abs" $(( depth + 1 )); then
                return 0
            fi
        else
            host=$(remote_host "$url") || continue
            shopt -s nocasematch
            if [[ "$host" =~ $NAS1_HOST_REGEX ]]; then
                shopt -u nocasematch
                NAS1_URL="$url"
                return 0
            fi
            shopt -u nocasematch
        fi
    done
    return 1
}


# ---------------------------------------------------------------------------
# Sběr informací
# ---------------------------------------------------------------------------

# Detekce git a ověření, že kořen zdrojového stromu je zároveň kořenem
# pracovního stromu git. Zdrojový archiv rozbalený uvnitř cizího repozitáře
# (--show-prefix by byl neprázdný) se tak nesplete s tímto projektem.
IN_GIT=0
if ! command -v git >/dev/null 2>&1; then
    echo "Warning: git command not found. Using default values."
elif [ "$(git rev-parse --is-inside-work-tree 2>/dev/null </dev/null)" != "true" ]; then
    echo "Warning: not inside a git working tree. Using default values."
elif [ -n "$(git rev-parse --show-prefix 2>/dev/null </dev/null)" ]; then
    echo "Warning: source tree is not the root of its git working tree. Using default values."
else
    IN_GIT=1
fi

if [ "$IN_GIT" -eq 1 ]; then
    # Načti všechny remote URL (každý remote může mít fetch i push URL).
    REMOTE_URLS="$(repo_remote_urls .)"

    # Zkontroluj, jestli alespoň jeden remote ukazuje na oficiální upstream.
    IS_UPSTREAM=0
    if [ -n "$REMOTE_URLS" ]; then
        # shopt nocasematch zajistí case-insensitive porovnání bez závislosti
        # na grep flagách.
        shopt -s nocasematch
        while IFS= read -r url; do
            [ -z "$url" ] && continue
            if [[ "$url" =~ $UPSTREAM_REGEX ]]; then
                IS_UPSTREAM=1
                MATCHED_URL="$url"
                break
            fi
        done <<< "$REMOTE_URLS"
        shopt -u nocasematch
    fi

    if [ "$IS_UPSTREAM" -eq 1 ]; then
        ORIGIN_ENUM="BUILD_REVISION_ORIGIN_GITHUB"
        ORIGIN_NAME="GitHub"
        if REV_COUNT=$(git rev-list --count HEAD 2>/dev/null </dev/null); then
            REV_INT=$(( REV_OFFSET + REV_COUNT ))
            REV_TEXT="$REV_INT"
            echo "Upstream remote matched: $MATCHED_URL"
            echo "Git commits on HEAD: $REV_COUNT, offset: $REV_OFFSET, revision: $REV_INT"
            if [ "$(git rev-parse --is-shallow-repository 2>/dev/null </dev/null)" = "true" ]; then
                echo "Warning: shallow clone - the commit count (and revision) is incomplete."
            fi
        else
            echo "Warning: upstream remote matched but git rev-list failed. Using default values."
        fi
    else
        echo "Warning: no remote points to github.com/michalhucik/mz800emu."
        echo "         Build revision set to -1 (this is not the official upstream clone)."
        if repo_reaches_nas1 . 0; then
            ORIGIN_ENUM="BUILD_REVISION_ORIGIN_NAS1"
            ORIGIN_NAME="NAS1"
            echo "NAS1 repository remote found: $NAS1_URL"
        fi
    fi

    # Commit (HEAD). V prázdném repozitáři bez commitů zůstane "unknown".
    if c=$(git rev-parse --verify -q HEAD 2>/dev/null </dev/null) && [ -n "$c" ]; then
        COMMIT="$c"
        COMMIT_SHORT=$(git rev-parse --short HEAD 2>/dev/null </dev/null) || COMMIT_SHORT=""
        [ -n "$COMMIT_SHORT" ] || COMMIT_SHORT="${COMMIT:0:8}"
    fi

    # Větev; detached HEAD se pozná podle selhání symbolic-ref.
    if b=$(git symbolic-ref -q --short HEAD 2>/dev/null </dev/null) && [ -n "$b" ]; then
        BRANCH="$b"
    elif [ "$COMMIT" != "unknown" ]; then
        BRANCH="detached"
        DETACHED=1
    fi

    # Neuložené změny ve sledovaných souborech (neverzované soubory se
    # nepočítají - stejně jako u git describe --dirty). --no-optional-locks
    # brání zápisu indexu souběžně s jinými git operacemi.
    if [ "$COMMIT" != "unknown" ]; then
        if st=$(git --no-optional-locks status --porcelain --untracked-files=no 2>/dev/null </dev/null); then
            if [ -n "$st" ]; then DIRTY=1; else DIRTY=0; fi
        fi
    fi

    echo "Source origin: $ORIGIN_NAME, branch: $BRANCH, commit: $COMMIT, dirty: $DIRTY"
fi


# ---------------------------------------------------------------------------
# Zápis výstupu (jen při změně obsahu)
# ---------------------------------------------------------------------------

E_REV_TEXT=$(c_escape "$REV_TEXT")
E_ORIGIN_NAME=$(c_escape "$ORIGIN_NAME")
E_BRANCH=$(c_escape "$BRANCH")
E_COMMIT=$(c_escape "$COMMIT")
E_COMMIT_SHORT=$(c_escape "$COMMIT_SHORT")
E_MSYSTEM=$(c_escape "$BUILD_MSYSTEM")

TMP_FILE="$OUTPUT_FILE.tmp.$$"

echo "Writing output file..."
if ! cat <<EOF > "$TMP_FILE"
/* This file is automatically created by $(basename "$SCRIPT_PATH") */
/* Do not edit! */
#include "build_revision.h"

const char *build_revision_get_const_char(void) { return "Revision: $E_REV_TEXT"; }
int build_revision_get_int(void) { return $REV_INT; }
en_BUILD_REVISION_ORIGIN build_revision_get_repo_origin(void) { return $ORIGIN_ENUM; }
const char *build_revision_get_repo_origin_name(void) { return "$E_ORIGIN_NAME"; }
const char *build_revision_get_branch(void) { return "$E_BRANCH"; }
int build_revision_is_detached(void) { return $DETACHED; }
const char *build_revision_get_commit(void) { return "$E_COMMIT"; }
const char *build_revision_get_commit_short(void) { return "$E_COMMIT_SHORT"; }
int build_revision_is_dirty(void) { return $DIRTY; }
const char *build_revision_get_build_msystem(void) { return "$E_MSYSTEM"; }
EOF
then
    echo "Error: cannot write $TMP_FILE"
    rm -f "$TMP_FILE"
    exit 1
fi

if [ -f "$OUTPUT_FILE" ] && command -v cmp >/dev/null 2>&1 && cmp -s "$TMP_FILE" "$OUTPUT_FILE"; then
    rm -f "$TMP_FILE"
    echo "Output unchanged."
else
    if ! mv -f "$TMP_FILE" "$OUTPUT_FILE"; then
        echo "Error: cannot write $OUTPUT_FILE"
        rm -f "$TMP_FILE"
        exit 1
    fi
fi

echo "Done."
exit 0
