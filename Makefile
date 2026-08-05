# Makefile - tenký wrapper kolem CMake
#
# Cross-platform: Windows (MSYS2 - MINGW64 nebo UCRT64) i Linux.
# Originální (1330 řádkový) Makefile je zachován jako OLD-Makefile.
# Tento wrapper poskytuje známé příkazy "make", "make mz800emu", "make clean"
# atd. a deleguje vše na CMake.
#
# Build adresář: build/
#   build/build-libs/             - .a archivy
#   build/build-mz800emu/         - mz800emu(.exe)        (MZ-800 PAL)
#   build/build-mz1500emu/        - mz1500emu(.exe)       (MZ-1500 NTSC)
#   build/build-mz700emu-pal/     - mz700emu-pal(.exe)    (MZ-700 EU)
#   build/build-mz700emu-ntsc/    - mz700emu-ntsc(.exe)   (MZ-700 JP)
#   build/CMakeFiles/             - interní (.o files spravované CMake)
#
# Volby:
#   DEBUG=1                 debug build s -g -O0 (default)
#   DEBUG=0                 release build s -O2 -DNDEBUG
#   FORCE_CONSOLE=1         Windows: zachovat console subsystem
#   MSYSTEM=MINGW64|UCRT64  Windows: vybrat MSYS2 toolchain (default UCRT64)
#   QUIET=                  podrobný výpis (default tichý)
#   GENERATOR=...           CMake generator (auto: Ninja > Make)

# ----------------------------------------------------------------------------
# Detekce platformy
# ----------------------------------------------------------------------------
# Wrapper běží na Windows (MSYS2 prostředí) i Linuxu. Detekce přes built-in
# proměnnou OS - na Windows ji nastavuje OS jako "Windows_NT", na Linuxu
# neexistuje a $(OS) je prázdné.
ifeq ($(OS),Windows_NT)
    MZ_PLATFORM := Windows
else
    MZ_PLATFORM := Linux
endif

# ============================================================================
# Windows (MSYS2) konfigurace
# ============================================================================
ifeq ($(MZ_PLATFORM),Windows)

# MSYS2/MinGW: GCC nemůže psát temp soubory do C:\Windows
export TEMP := /tmp
export TMP  := /tmp

# Wrapper podporuje MINGW64 i UCRT64. Volba:
#   1. Pokud je nastavená MSYSTEM (typicky shell-startup z MSYS2), použije se ta
#   2. Pokud uživatel vynutí přes `make MSYSTEM=MINGW64`, použije se ta
#   3. Fallback na UCRT64 (preferovaný toolchain - moderní CRT, lepší C99/C11)
MSYSTEM ?= UCRT64

ifeq ($(MSYSTEM),UCRT64)
    MZ_TOOLCHAIN_DIR := C:/msys64/ucrt64
else ifeq ($(MSYSTEM),MINGW64)
    MZ_TOOLCHAIN_DIR := C:/msys64/mingw64
else
    $(error Unsupported MSYSTEM=$(MSYSTEM); use MINGW64 or UCRT64)
endif

MZ_CC    := $(MZ_TOOLCHAIN_DIR)/bin/gcc.exe
MZ_CXX   := $(MZ_TOOLCHAIN_DIR)/bin/g++.exe
MZ_CMAKE := $(MZ_TOOLCHAIN_DIR)/bin/cmake.exe
MZ_NINJA := $(MZ_TOOLCHAIN_DIR)/bin/ninja.exe

# CC/CXX env variables - cmake je respektuje. Nepoužíváme command-line -D,
# protože MSYS2 bash si v argumentech rozkládá `C:/path` na `C` (dvojtečka
# jako separator), zatímco env variables jsou inertní vůči parsing.
export CC  := $(MZ_CC)
export CXX := $(MZ_CXX)

# PATH separator detekce přes built-in $(MAKE_HOST):
#   x86_64-w64-mingw32 (mingw32-make.exe) → Windows-style `;`
#   *-msys, *-pc-cygwin (MSYS make)        → POSIX `:`
# PATH musí být v Makefile přebíjený - gcc potřebuje toolchain/bin pro
# cc1.exe, as.exe, atd. (relativně k jeho cestě nedohledá vše).
ifneq (,$(filter %-w64-mingw32 %-mingw32, $(MAKE_HOST)))
    MZ_PATH_SEP := ;
else
    MZ_PATH_SEP := :
endif

export PATH            := $(MZ_TOOLCHAIN_DIR)/bin$(MZ_PATH_SEP)$(PATH)
export PKG_CONFIG_PATH := $(MZ_TOOLCHAIN_DIR)/lib/pkgconfig$(MZ_PATH_SEP)$(MZ_TOOLCHAIN_DIR)/share/pkgconfig

endif # Windows

# ============================================================================
# Linux konfigurace
# ============================================================================
ifeq ($(MZ_PLATFORM),Linux)

# Na Linuxu používáme systémové cmake/ninja/gcc z PATH (typicky /usr/bin/).
# Žádný toolchain magic, žádný MSYSTEM, žádný PATH override.
MZ_CMAKE := cmake
MZ_NINJA := $(shell command -v ninja 2>/dev/null)

endif # Linux

# ----------------------------------------------------------------------------
# Volby
# ----------------------------------------------------------------------------
DEBUG ?= 1
FORCE_CONSOLE ?= 0
QUIET ?= @

# Generator: Ninja pokud je $(MZ_NINJA) k dispozici, jinak Make-based
# (Windows: "MSYS Makefiles", Linux: "Unix Makefiles").
ifeq ($(MZ_NINJA),)
    ifeq ($(MZ_PLATFORM),Windows)
        DEFAULT_GENERATOR := MSYS Makefiles
    else
        DEFAULT_GENERATOR := Unix Makefiles
    endif
else
    DEFAULT_GENERATOR := Ninja
endif
GENERATOR ?= $(DEFAULT_GENERATOR)

ifeq ($(DEBUG),1)
    BUILD_TYPE := Debug
else
    BUILD_TYPE := Release
endif

ifeq ($(FORCE_CONSOLE),1)
    CMAKE_FORCE_CONSOLE := -DMZ_FORCE_CONSOLE=ON
else
    CMAKE_FORCE_CONSOLE := -DMZ_FORCE_CONSOLE=OFF
endif

BUILD_DIR := build
CMAKE_CACHE := $(BUILD_DIR)/CMakeCache.txt

# Display jméno projektu (= co cmake configure vytiskne). Override-able
# z command line: `make MZ_PROJECT_NAME=cokoli`. Pokud tuto definici
# zakomentuješ, CMakeLists.txt udělá fallback na jméno adresáře projektu.
MZ_PROJECT_NAME ?= mz800emu

CMAKE_CONFIGURE_FLAGS := \
    -S . -B $(BUILD_DIR) \
    -G "$(GENERATOR)" \
    -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
    -DMZ_PROJECT_NAME=$(MZ_PROJECT_NAME) \
    $(CMAKE_FORCE_CONSOLE)

# FDC_DIAG propagace: `make FDC_DIAG=1` zapne verbose trace v
# wd279x.c a fdc.c (LOADED/FLUSHED sektory, WRITE TRACK state
# machine, DSK header repair). Bez parametru zůstává cache hodnota
# (default OFF).
ifneq ($(FDC_DIAG),)
    CMAKE_CONFIGURE_FLAGS += -DFDC_DIAG=$(FDC_DIAG)
endif

# RAM_FASTPATH propagace (E1, KROK 1): `make RAM_FASTPATH=1` zapne inline
# page-table fast-path pro cista RAM cteni/zapisy v Z80 jadre. OFF = A5 baseline.
# `make RAM_FASTPATH_VERIFY=1` navic zapne diff-verify (DEBUG, pomale).
ifneq ($(RAM_FASTPATH),)
    CMAKE_CONFIGURE_FLAGS += -DMZ_RAM_FASTPATH=$(RAM_FASTPATH)
endif
ifneq ($(RAM_FASTPATH_VERIFY),)
    CMAKE_CONFIGURE_FLAGS += -DMZ_RAM_FASTPATH_VERIFY=$(RAM_FASTPATH_VERIFY)
endif

# REPRO propagace: `make REPRO=1` zapne deterministický build pro
# repro-diff srovnávání binárek (experiment mzhal). Volající skript musí
# navíc nastavit SOURCE_DATE_EPOCH v prostředí. Viz MZ_REPRO_BUILD
# v CMakeLists.txt.
ifneq ($(REPRO),)
    CMAKE_CONFIGURE_FLAGS += -DMZ_REPRO_BUILD=$(REPRO)
endif

# NO_DEBUGGER propagace: `make NO_DEBUGGER=1` vypne debugger subsystém
# (vypne MZ800EMU_CFG_DEBUGGER_ENABLED v mzarch_config.h přes globální
# define MZ800EMU_NO_DEBUGGER). Bez debuggeru je binárka cca o 15 %
# menší (-6.7 MB) a emulace by měla být rychlejší.
ifneq ($(NO_DEBUGGER),)
    CMAKE_CONFIGURE_FLAGS += -DMZ_NO_DEBUGGER=$(NO_DEBUGGER)
endif

# NO_MCP / NO_MCP_TCP propagace: vypíná MCP server backend, respektive
# jen jeho TCP listener. MCP je ve standardním buildu VŽDY zapnutý;
# vypíná se explicitně přes NO_MCP=1 / NO_MCP_TCP=1. Negativní toggle ->
# CMake -DMZ_NO_MCP=ON -> add_compile_definitions MZ800EMU_NO_MCP ->
# #ifndef guard v mzarch_config.h.
#
# Sémantika (cascade):
#   NO_DEBUGGER=1   implicitně vyžaduje NO_MCP=1 (MCP vyžaduje debugger)
#   NO_MCP=1        implicitně vyžaduje NO_MCP_TCP=1 (TCP vyžaduje MCP)
# CMakeLists.txt cascade vynutí automaticky a vypíše message(STATUS).
ifneq ($(NO_MCP),)
    CMAKE_CONFIGURE_FLAGS += -DMZ_NO_MCP=$(NO_MCP)
endif
ifneq ($(NO_MCP_TCP),)
    CMAKE_CONFIGURE_FLAGS += -DMZ_NO_MCP_TCP=$(NO_MCP_TCP)
endif

# CMAKE_MAKE_PROGRAM se přidává jen pokud máme explicitní cestu k ninja
# (Windows toolchain). Na Linuxu se ninja najde z PATH automaticky.
ifneq ($(MZ_NINJA),)
ifeq ($(MZ_PLATFORM),Windows)
    CMAKE_CONFIGURE_FLAGS += -DCMAKE_MAKE_PROGRAM=$(MZ_NINJA)
endif
endif

# Detekce nproc pro paralelní build
JOBS := $(shell nproc 2>/dev/null || echo 4)

# ----------------------------------------------------------------------------
# Default target
# ----------------------------------------------------------------------------
.PHONY: all
all: mz800emu mz1500emu mz700emu-pal mz700emu-ntsc i18n-mo-auto

# ----------------------------------------------------------------------------
# Configure (jen pokud chybí cache)
# ----------------------------------------------------------------------------
ifeq ($(MZ_PLATFORM),Windows)
    MZ_PLATFORM_LABEL := $(MZ_PLATFORM)/$(MSYSTEM)
else
    MZ_PLATFORM_LABEL := $(MZ_PLATFORM)
endif

$(CMAKE_CACHE):
	@echo "Configuring CMake ($(BUILD_TYPE), $(GENERATOR), $(MZ_PLATFORM_LABEL)) ..."
	$(QUIET)$(MZ_CMAKE) $(CMAKE_CONFIGURE_FLAGS)

.PHONY: configure
configure:
	@echo "Configuring CMake ($(BUILD_TYPE), $(GENERATOR), $(MZ_PLATFORM_LABEL)) ..."
	$(QUIET)$(MZ_CMAKE) $(CMAKE_CONFIGURE_FLAGS)

# ----------------------------------------------------------------------------
# Build targets
# ----------------------------------------------------------------------------
.PHONY: mz800emu mz1500emu mz700emu-pal mz700emu-ntsc
mz800emu: $(CMAKE_CACHE) i18n-mo-auto
	$(QUIET)$(MZ_CMAKE) --build $(BUILD_DIR) --target mz800emu -j$(JOBS)
	@cp -f $(BUILD_DIR)/build-mz800emu/mz800emu* . 2>/dev/null || true

mz1500emu: $(CMAKE_CACHE) i18n-mo-auto
	$(QUIET)$(MZ_CMAKE) --build $(BUILD_DIR) --target mz1500emu -j$(JOBS)
	@cp -f $(BUILD_DIR)/build-mz1500emu/mz1500emu* . 2>/dev/null || true

mz700emu-pal: $(CMAKE_CACHE) i18n-mo-auto
	$(QUIET)$(MZ_CMAKE) --build $(BUILD_DIR) --target mz700emu-pal -j$(JOBS)
	@cp -f $(BUILD_DIR)/build-mz700emu-pal/mz700emu-pal* . 2>/dev/null || true

mz700emu-ntsc: $(CMAKE_CACHE) i18n-mo-auto
	$(QUIET)$(MZ_CMAKE) --build $(BUILD_DIR) --target mz700emu-ntsc -j$(JOBS)
	@cp -f $(BUILD_DIR)/build-mz700emu-ntsc/mz700emu-ntsc* . 2>/dev/null || true

# ----------------------------------------------------------------------------
# Build/debug aliases (kompatibilita s OLD-Makefile)
# ----------------------------------------------------------------------------
.PHONY: debug build
debug:
	$(QUIET)$(MAKE) --no-print-directory DEBUG=1 mz800emu

build:
	$(QUIET)$(MAKE) --no-print-directory DEBUG=0 mz800emu

# ----------------------------------------------------------------------------
# Run
# ----------------------------------------------------------------------------
.PHONY: run
run: mz800emu
	$(QUIET)./mz800emu

# ----------------------------------------------------------------------------
# Test suite (CTest přes CMake)
# Build executables a spuštění registrovaných testů. Filtrování přes
# CTEST_LABEL (např. "lib", "emu", "ui") nebo CTEST_REGEX.
# ----------------------------------------------------------------------------
CTEST_LABEL ?=
CTEST_REGEX ?=
CTEST_ARGS  := --test-dir $(BUILD_DIR) --output-on-failure
ifneq ($(CTEST_LABEL),)
    CTEST_ARGS += -L $(CTEST_LABEL)
endif
ifneq ($(CTEST_REGEX),)
    CTEST_ARGS += -R $(CTEST_REGEX)
endif

# Helper - postavi vsechny testy a spusti CTest.
# MCP e2e testy (tests/mcp/test_pipe.py, test_mcp_server_stdio.py) spouštějí
# mz800emu z rootu repozitáře. cmake --build ho staví jen do build/; kopii do
# rootu dělá jinak až target `mz800emu`, takže bez následujícího cp by tyto e2e
# testy padaly po `make mrproper` (binárka v rootu chybí).
.PHONY: test
test: $(CMAKE_CACHE)
	$(QUIET)$(MZ_CMAKE) --build $(BUILD_DIR) -j$(JOBS)
	$(QUIET)cp -f $(BUILD_DIR)/build-mz800emu/mz800emu* . 2>/dev/null || true
	$(QUIET)ctest $(CTEST_ARGS)

# Per-skupina shortcuts
.PHONY: test-standalone test-lib test-emu test-ui test-snapshot test-peripherals
test-standalone:
	$(QUIET)$(MAKE) --no-print-directory test CTEST_LABEL=standalone
test-lib:
	$(QUIET)$(MAKE) --no-print-directory test CTEST_LABEL=lib
test-emu:
	$(QUIET)$(MAKE) --no-print-directory test CTEST_LABEL=emu
test-ui:
	$(QUIET)$(MAKE) --no-print-directory test CTEST_LABEL=ui
test-snapshot:
	$(QUIET)$(MAKE) --no-print-directory test CTEST_LABEL=snapshot
test-peripherals:
	$(QUIET)$(MAKE) --no-print-directory test CTEST_LABEL=peripherals

# i18n meta testy (kontrolní shell skripty - bez emulátorového kódu)
.PHONY: test-i18n test-i18n-core test-i18n-coverage test-i18n-completeness
test-i18n: test-i18n-core test-i18n-coverage test-i18n-completeness

test-i18n-core:
	@bash tools/check_i18n_core.sh

test-i18n-coverage:
	@bash tools/check_i18n_coverage.sh

test-i18n-completeness:
	@echo ""
	@echo "=== Translation completeness check ==="
	@found=0; \
	for arch in 800 1500; do \
	    domain="mz$${arch}emu"; \
	    for po in $(I18N_LOCALE_DIR)/*/LC_MESSAGES/$${domain}.po; do \
	        if [ -f "$$po" ]; then \
	            found=1; \
	            lang=$$(echo "$$po" | sed 's|.*/\([a-z]*\)/LC_MESSAGES/.*|\1|'); \
	            stats=$$(msgfmt --statistics -o /dev/null "$$po" 2>&1); \
	            echo "  $${domain} [$$lang]: $$stats"; \
	        fi; \
	    done; \
	done; \
	if [ $$found -eq 0 ]; then echo "  (no .po files found)"; fi

# Per-test shortcuts (vybírá podle test name přes regex)
# Příklad: 'make test-iasm' = ctest -R '^iasm$$'
test-%: $(CMAKE_CACHE)
	$(QUIET)$(MZ_CMAKE) --build $(BUILD_DIR) --target test_$* -j$(JOBS)
	$(QUIET)ctest --test-dir $(BUILD_DIR) --output-on-failure -R "^$*$$"

# ----------------------------------------------------------------------------
# Doxygen dokumentace
# ----------------------------------------------------------------------------
.PHONY: docs
docs:
	$(QUIET)doxygen Doxyfile

# ----------------------------------------------------------------------------
# compile_commands.json - symlink na CMake-generovaný v build/
# IDE/clangd ho čekají v root adresáři projektu.
# ----------------------------------------------------------------------------
.PHONY: compile_commands.json
compile_commands.json: $(CMAKE_CACHE)
	$(QUIET)cp -f $(BUILD_DIR)/compile_commands.json . 2>/dev/null || \
	    echo "Warning: $(BUILD_DIR)/compile_commands.json does not exist (run 'make' first)"

# ============================================================================
# i18n - gettext .pot/.po/.mo pipeline
# ============================================================================
I18N_LOCALE_DIR := src/locale
I18N_LANGS      := cs sk ja de it es nl fr pl uk
I18N_DOMAIN     := mz$(MZARCH)emu
MZARCH          ?= 800

# Všechny existující .po → odpovídající .mo (auto-build při make all)
I18N_ALL_PO := $(wildcard $(I18N_LOCALE_DIR)/*/LC_MESSAGES/*.po)
I18N_ALL_MO := $(I18N_ALL_PO:.po=.mo)

# Pattern rule: kompilovat .po → .mo (msgfmt)
%.mo: %.po
	@echo "  msgfmt: $<"
	$(QUIET)msgfmt -c -o $@ $<

# i18n-mo-auto: kompiluje pouze zastaralé .mo (závislost na .po timestamp)
.PHONY: i18n-mo-auto
i18n-mo-auto: $(I18N_ALL_MO)

# i18n-update-pot: extract translatable strings ze zdrojáků (pro daný MZARCH)
.PHONY: i18n-update-pot
i18n-update-pot:
	@echo "=== Generating $(I18N_DOMAIN).pot (MZARCH=$(MZARCH)) ==="
	$(QUIET)bash tools/i18n_find_sources.sh $(MZARCH) | sort | \
	    xgettext -k_ -k_L -kN_ -kC_:1c,2 \
	        --language=C++ --from-code=UTF-8 \
	        --package-name=$(I18N_DOMAIN) \
	        --package-version=1.0 \
	        --copyright-holder="Michal Hucik" \
	        -o $(I18N_LOCALE_DIR)/$(I18N_DOMAIN).pot \
	        -f -

# i18n-update-pot-all: pot pro všechny architektury
.PHONY: i18n-update-pot-all
i18n-update-pot-all:
	@for arch in 700 800 1500; do \
	    $(MAKE) --no-print-directory i18n-update-pot MZARCH=$$arch; \
	done

# i18n-update-po: msgmerge nových řetězců do existujících .po (per arch)
#
# POZN: --no-fuzzy-matching je povinné, aby msgmerge nepřiřadil existující
# msgstr novým podobným msgid. Bez něj fuzzy match propagoval "FDC State"
# jako překlad pro "CTC State"/"PPI State"/"PSG State" napříč všemi jazyky
# (regrese z per-chip-panels mutantu, viz Task 3 gdg-panel).
.PHONY: i18n-update-po
i18n-update-po:
	@for lang in $(I18N_LANGS); do \
	    po="$(I18N_LOCALE_DIR)/$$lang/LC_MESSAGES/$(I18N_DOMAIN).po"; \
	    if [ -f "$$po" ]; then \
	        echo "  msgmerge: $$lang"; \
	        msgmerge --update --quiet --no-fuzzy-matching "$$po" $(I18N_LOCALE_DIR)/$(I18N_DOMAIN).pot; \
	    fi; \
	done

# i18n-compile: vynucená kompilace .po → .mo (per arch, na rozdíl od auto)
.PHONY: i18n-compile
i18n-compile:
	@for lang in $(I18N_LANGS); do \
	    po="$(I18N_LOCALE_DIR)/$$lang/LC_MESSAGES/$(I18N_DOMAIN).po"; \
	    if [ -f "$$po" ]; then \
	        echo "  msgfmt: $$lang"; \
	        msgfmt -c -o "$${po%.po}.mo" "$$po"; \
	    fi; \
	done

# i18n-compile-all: kompilace pro všechny architektury
.PHONY: i18n-compile-all
i18n-compile-all:
	@for arch in 700 800 1500; do \
	    $(MAKE) --no-print-directory i18n-compile MZARCH=$$arch; \
	done

# i18n-translate: auto-překlad jednoho jazyka přes Claude API (interaktivní)
.PHONY: i18n-translate
i18n-translate:
	$(QUIET)python3 tools/i18n_translate.py $(I18N_LOCALE_DIR)/$(I18N_DOMAIN).pot $(LANG)

# i18n-translate-all: postupný překlad všech jazyků (každý vyžaduje schválení)
.PHONY: i18n-translate-all
i18n-translate-all:
	@for lang in $(I18N_LANGS); do \
	    echo ""; \
	    echo "=== Translating: $$lang ==="; \
	    python3 tools/i18n_translate.py $(I18N_LOCALE_DIR)/$(I18N_DOMAIN).pot $$lang; \
	done

# i18n-propagate: kopie překladů z mz800emu do mz700emu/mz1500emu (společné
# řetězce přes msgmerge --compendium, arch-specifické zůstanou nepřeložené)
.PHONY: i18n-propagate
i18n-propagate:
	@for lang in $(I18N_LANGS); do \
	    src800="$(I18N_LOCALE_DIR)/$$lang/LC_MESSAGES/mz800emu.po"; \
	    if [ -f "$$src800" ]; then \
	        for arch in 700 1500; do \
	            pot="$(I18N_LOCALE_DIR)/mz$${arch}emu.pot"; \
	            po="$(I18N_LOCALE_DIR)/$$lang/LC_MESSAGES/mz$${arch}emu.po"; \
	            if [ -f "$$pot" ]; then \
	                echo "  propagate: $$lang -> mz$${arch}emu"; \
	                msgmerge --compendium="$$src800" --no-fuzzy-matching \
	                    "$$po" "$$pot" -o "$$po" 2>/dev/null || \
	                msgmerge --compendium="$$src800" --no-fuzzy-matching \
	                    /dev/null "$$pot" -o "$$po"; \
	            fi; \
	        done; \
	    fi; \
	done

# ----------------------------------------------------------------------------
# Distribuce - production build s DLL, locale, docs do dist/
# ----------------------------------------------------------------------------
DIST_DIR := dist

.PHONY: dist
dist:
	@echo "=== Building release version ==="
	@# Vynuť fresh CMake configure: configure běží jen když chybí
	@# CMakeCache.txt ($(CMAKE_CACHE) file target). Bez tohohle by dist
	@# na již nakonfigurovaném dev build dir (typicky Debug) NEpřekonfiguroval
	@# a vyrobil by Debug binárku (-O0 -g) navzdory DEBUG=0. Smazání cache
	@# zaručí, že se aplikuje release (-O2 -DNDEBUG).
	@# Pozn.: po dist zůstane build/ v release konfiguraci; pro návrat
	@# k debug vývoji spusť `make configure` (přepíše BUILD_TYPE zpět).
	@echo "Forcing fresh CMake configure (release build)"
	$(QUIET)rm -f $(CMAKE_CACHE)
	$(QUIET)$(MAKE) --no-print-directory DEBUG=0 mz800emu mz1500emu mz700emu-pal mz700emu-ntsc i18n-mo-auto
	@echo ""
	@echo "=== Preparing $(DIST_DIR)/ ==="
	$(QUIET)rm -rf $(DIST_DIR)
	$(QUIET)mkdir -p $(DIST_DIR)
	@echo "Copying ui_resources/"
	$(QUIET)cp -r ui_resources $(DIST_DIR)/
	@echo "Copying docs/"
	$(QUIET)mkdir -p $(DIST_DIR)/docs
	$(QUIET)find ./docs -type f ! -path "./docs/unimportant_notes/*" -exec \
	    sh -c 'for f; do d="$(DIST_DIR)/docs/$${f#./docs/}"; \
	        mkdir -p "$$(dirname "$$d")"; cp "$$f" "$$d"; done' _ {} +
	@echo "Copying binaries"
	$(QUIET)cp -f mz800emu* mz1500emu* mz700emu-pal* mz700emu-ntsc* $(DIST_DIR)/ 2>/dev/null || true
	@echo "Copying locale .mo files"
	$(QUIET)for target in mz800emu mz1500emu mz700emu-pal mz700emu-ntsc; do \
	    for lang in $(I18N_LANGS); do \
	        mo="$(I18N_LOCALE_DIR)/$$lang/LC_MESSAGES/$${target}.mo"; \
	        if [ -f "$$mo" ]; then \
	            mkdir -p "$(DIST_DIR)/locale/$$lang/LC_MESSAGES"; \
	            cp -f "$$mo" "$(DIST_DIR)/locale/$$lang/LC_MESSAGES/"; \
	        fi; \
	    done; \
	done
	@echo "Copying mcp-server/ (Python MCP wrapper)"
	$(QUIET)mkdir -p $(DIST_DIR)/mcp-server
	$(QUIET)cp -f mcp-server/mcp_server.py $(DIST_DIR)/mcp-server/
	$(QUIET)cp -f mcp-server/mcpinit.sh $(DIST_DIR)/mcp-server/
	$(QUIET)chmod +x $(DIST_DIR)/mcp-server/mcpinit.sh
	$(QUIET)cp -f mcp-server/requirements.txt $(DIST_DIR)/mcp-server/
	$(QUIET)cp -f mcp-server/README.md $(DIST_DIR)/mcp-server/
	$(QUIET)cp -f mcp-server/README.cs.md $(DIST_DIR)/mcp-server/
	@# .mcp.json se v dist nepřikládá - obsahuje absolutní cesty
	@# specifické pro install location. Uživatel ho vyrobí jedním
	@# ./mcp-server/mcpinit.sh po rozbalení dist.
ifeq ($(MZ_PLATFORM),Windows)
	@echo "Copying Windows extras (cacert.pem, libpng DLLs)"
	$(QUIET)mkdir -p $(DIST_DIR)/certs
	$(QUIET)cp -f docs/certs/cacert.pem $(DIST_DIR)/certs/ 2>/dev/null || true
	$(QUIET)cp -f $(MZ_TOOLCHAIN_DIR)/bin/libpng*.dll $(DIST_DIR)/ 2>/dev/null || true
	@echo "Collecting DLL dependencies (tools/collect-dlls.sh)"
	$(QUIET)find $(DIST_DIR) -maxdepth 1 -type f \( -iname "*.exe" -o -iname "*.dll" \) | \
	    while read bin; do \
	        echo "  $$bin"; \
	        bash tools/collect-dlls.sh "$$bin" "$(DIST_DIR)"; \
	    done
endif
	@echo ""
	@echo "Distribution created in $(DIST_DIR)/"

# ----------------------------------------------------------------------------
# Clean
# ----------------------------------------------------------------------------
.PHONY: clean
clean:
	$(QUIET)rm -rf $(BUILD_DIR)
	$(QUIET)rm -f mz800emu mz800emu.exe mz1500emu mz1500emu.exe mz700emu-pal mz700emu-pal.exe mz700emu-ntsc mz700emu-ntsc.exe
	$(QUIET)rm -f compile_commands.json

.PHONY: mrproper
mrproper: clean
	$(QUIET)rm -rf $(DIST_DIR)
	$(QUIET)rm -f src/build_revision/build_revision.c

.PHONY: rmruntime
rmruntime:
	$(QUIET)rm -f mz[7-8]00*.ini mz[7-8]00*.bpt mz[7-8]00*.vars mz[7-8]00.bookmarks rd-mz[7-8]00.dat mz[7-8]00*.lbl mz[7-8]00*.watch mz1500*.ini mz1500*.bpt mz1500*.vars mz1500.bookmarks rd-mz1500.dat mz1500.lbl mz1500.watch

# ----------------------------------------------------------------------------
# Help
# ----------------------------------------------------------------------------
.PHONY: help
help:
	@echo ""
	@echo "Sharp MZ-800/700/1500 Emulator - Build System (CMake wrapper)"
	@echo "=============================================================="
	@echo ""
	@echo "BUILD TARGETS"
	@echo "  make                         Build all (mz800emu + mz1500emu + mz700emu-pal + mz700emu-ntsc + i18n .mo)"
	@echo "  make all                     Same as 'make' (default target)"
	@echo "  make mz800emu                Build MZ-800 emulator (PAL)"
	@echo "  make mz1500emu               Build MZ-1500 emulator (NTSC)"
	@echo "  make mz700emu-pal            Build MZ-700 emulator (PAL = EU)"
	@echo "  make mz700emu-ntsc           Build MZ-700 emulator (NTSC = JP)"
	@echo "  make debug                   Alias for 'make DEBUG=1 mz800emu'"
	@echo "  make build                   Alias for 'make DEBUG=0 mz800emu'"
	@echo "  make run                     Build and run MZ-800 emulator"
	@echo "  make configure               Re-run cmake configure"
	@echo ""
	@echo "BUILD OPTIONS"
	@echo "  DEBUG=1                      Debug build with -g -O0 (default)"
	@echo "  DEBUG=0                      Release build with -O2 -DNDEBUG"
	@echo "  FORCE_CONSOLE=1              Windows: keep console window open"
	@echo "  MSYSTEM=MINGW64|UCRT64       Windows: select MSYS2 toolchain (default UCRT64)"
	@echo "  GENERATOR=Ninja|...          CMake generator (auto-detected)"
	@echo "  FDC_DIAG=1                   Enable FDC verbose trace (wd279x, fdc)"
	@echo "  NO_DEBUGGER=1                Build without debugger subsystem (~15 % smaller binary)"
	@echo "  NO_MCP=1                     Build without MCP server backend (cascade: implies NO_MCP_TCP=1)"
	@echo "  NO_MCP_TCP=1                 Build without MCP TCP listener (pipe transport only)"
	@echo "  QUIET=                       Show full build commands"
	@echo ""
	@echo "TESTING (CTest)"
	@echo "  make test                    Run all tests (build is performed automatically)"
	@echo "  make test-<name>             Run a specific test (e.g. test-iasm, test-snapshot-io)"
	@echo "  make test-standalone         Standalone tests (without emulator core)"
	@echo "  make test-lib                Library tests (lib + emulator core)"
	@echo "  make test-emu                Emulator core tests (sanity, breakpoints, peripherals, snapshot)"
	@echo "  make test-snapshot           Snapshot tests only (io, xml, mgr)"
	@echo "  make test-peripherals        Peripherals tests only (ctc, pio, psg)"
	@echo "  make test-ui                 UI tests (smoke)"
	@echo "  make test-i18n               i18n meta checks (core + coverage + completeness)"
	@echo "  make test-i18n-core          i18n core check only"
	@echo "  make test-i18n-coverage      i18n coverage check only"
	@echo "  make test-i18n-completeness  i18n translation completeness statistics"
	@echo "  CTEST_LABEL=<label>          Filter by label (lib, emu, ui, libs, peripherals, ...)"
	@echo "  CTEST_REGEX=<regex>          Filter by test name regex"
	@echo ""
	@echo "DISTRIBUTION"
	@echo "  make dist                    Release build + DLL + locale + docs into dist/"
	@echo ""
	@echo "LOCALIZATION (i18n)"
	@echo "  make i18n-mo-auto                  Compile .po -> .mo (auto-called from 'make all')"
	@echo "  make i18n-update-pot MZARCH=800    Extract translatable strings into .pot"
	@echo "  make i18n-update-pot-all           Extract .pot for all architectures"
	@echo "  make i18n-update-po                msgmerge .pot -> .po"
	@echo "  make i18n-compile MZARCH=800       Force compile .po -> .mo"
	@echo "  make i18n-compile-all              Force compile for all architectures"
	@echo "  make i18n-translate LANG=cs        Auto-translate one language (Claude API)"
	@echo "  make i18n-translate-all            Auto-translate all languages"
	@echo "  make i18n-propagate                Copy translations mz800emu -> mz700/1500emu"
	@echo ""
	@echo "DOCUMENTATION"
	@echo "  make docs                    Generate Doxygen documentation"
	@echo "  make compile_commands.json   IDE/clangd compilation database"
	@echo ""
	@echo "CLEANING"
	@echo "  make clean                   Remove build/ and binaries"
	@echo "  make mrproper                clean + dist/ + build_revision.c"
	@echo "  make rmruntime               Remove emulator runtime files (.ini, .bpt, .vars, ...)"
	@echo ""
	@echo "CURRENT SETTINGS"
	@echo "  Platform:      $(MZ_PLATFORM_LABEL)"
	@echo "  Build type:    $(BUILD_TYPE)"
	@echo "  Generator:     $(GENERATOR)"
	@echo "  Build dir:     $(BUILD_DIR)"
	@echo "  Parallel jobs: $(JOBS)"
	@echo ""
