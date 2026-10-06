/**
 * @file version_info.c
 * @brief Implementace výpisu --version (verze, revize, původ zdrojů, build).
 *
 * Viz version_info.h. Zdroje údajů:
 *  - verze a tag: cfgmain.h (CFGMAIN_EMULATOR_VERSION_*),
 *  - datum buildu: cfgmain_get_build_datetime() (__DATE__/__TIME__ v cfgmain.c),
 *  - revize, původ zdrojů, větev, commit, dirty, MSYSTEM: vygenerovaný
 *    build_revision.c (tools/generate_build_revision_git.sh),
 *  - platforma a varianta: per-target defines MZARCH a MZTVSYS,
 *  - zakompilované části: MZ800EMU_CFG_* z mzarch_config.h a globální
 *    defines buildu (MZ800EMU_CFG_RAM_FASTPATH, FDC_DIAG, ...),
 *  - verze knihoven: hlavičky (při překladu) a SDL_GetVersion() /
 *    glib_*_version (linkované za běhu, bez inicializace).
 */

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include "version_info.h"
#include "mzarch/mzarch_config.h"
#include "emulator/cfgmain.h"
#include "build_revision/build_revision.h"


/** @brief Jméno binárky podle per-target defines MZARCH a MZTVSYS. */
#if MZARCH == 700
#if MZTVSYS == MZTVSYS_PAL
#define VERSION_INFO_PROGRAM "mz700emu-pal"
#else
#define VERSION_INFO_PROGRAM "mz700emu-ntsc"
#endif
#elif MZARCH == 1500
#define VERSION_INFO_PROGRAM "mz1500emu"
#else
#define VERSION_INFO_PROGRAM "mz800emu"
#endif

/** @brief Emulovaný počítač podle MZARCH. */
#if MZARCH == 700
#define VERSION_INFO_MACHINE "Sharp MZ-700"
#elif MZARCH == 1500
#define VERSION_INFO_MACHINE "Sharp MZ-1500"
#else
#define VERSION_INFO_MACHINE "Sharp MZ-800"
#endif

/** @brief Popis překladače (GCC / Clang / jiný) včetně verze. */
#if defined(__clang__)
#define VERSION_INFO_COMPILER __VERSION__
#elif defined(__GNUC__)
#define VERSION_INFO_COMPILER "GCC " __VERSION__
#else
#define VERSION_INFO_COMPILER "unknown"
#endif

/** @brief Architektura CPU, pro kterou je binárka přeložená. */
#if defined(__x86_64__) || defined(_M_X64)
#define VERSION_INFO_CPU_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define VERSION_INFO_CPU_ARCH "arm64"
#elif defined(__i386__) || defined(_M_IX86)
#define VERSION_INFO_CPU_ARCH "x86"
#else
#define VERSION_INFO_CPU_ARCH "unknown"
#endif


/**
 * @brief Vrátí řetězec, nebo "unknown" když je NULL či prázdný.
 * @param s vstupní řetězec (může být NULL)
 * @return s, nebo statický literál "unknown"
 */
static const char *_or_unknown(const char *s)
{
    return (s && s[0]) ? s : "unknown";
}


/**
 * @brief Zapíše do bufferu popis hostitelského OS (za běhu).
 *
 * Windows: verze z RtlGetVersion (GetVersionEx by bez manifestu vracel
 * zkreslenou verzi) + nativní architektura z GetNativeSystemInfo.
 * Ostatní: uname() (sysname, release, machine).
 *
 * @param buf  cílový buffer
 * @param size velikost bufferu
 */
static void _host_os(char *buf, size_t size)
{
#ifdef _WIN32
    typedef LONG(WINAPI * t_RtlGetVersion)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    int have_ver = 0;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
    {
        t_RtlGetVersion fn = (t_RtlGetVersion)(void *)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn && fn(&vi) == 0)
        {
            have_ver = 1;
        }
    }

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *arch;
    switch (si.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64:
        arch = "x86_64";
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        arch = "arm64";
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        arch = "x86";
        break;
    default:
        arch = "unknown";
        break;
    }

    if (have_ver)
    {
        snprintf(buf, size, "Windows %lu.%lu build %lu (%s)",
                 (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
                 (unsigned long)vi.dwBuildNumber, arch);
    }
    else
    {
        snprintf(buf, size, "Windows (%s)", arch);
    }
#else
    struct utsname u;
    if (uname(&u) == 0)
    {
        snprintf(buf, size, "%s %s (%s)", u.sysname, u.release, u.machine);
    }
    else
    {
        snprintf(buf, size, "unknown");
    }
#endif
}


void version_info_prepare_console(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != NULL && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN)
    {
        /* stdout je platný (soubor, roura, konzole) - nic neměnit. */
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        (void)freopen("CONOUT$", "w", stdout);
        (void)freopen("CONOUT$", "w", stderr);
        /* Začít na novém řádku - prompt rodičovské konzole už mohl být
         * vypsán (cmd.exe na GUI program nečeká). */
        fputs("\n", stdout);
    }
#endif
}


void version_info_print(FILE *out)
{
    if (!out)
    {
        out = stdout;
    }

    /* ---- Program a verze ---- */
    fprintf(out, "%s - %s emulator (%s)\n", VERSION_INFO_PROGRAM, VERSION_INFO_MACHINE, MZTVSYS_NAME);
    fprintf(out, "Version:      %s %s\n", CFGMAIN_EMULATOR_VERSION_NUM_STRING, CFGMAIN_EMULATOR_VERSION_TAG);

    int rev = build_revision_get_int();
    if (rev >= 0)
    {
        fprintf(out, "Revision:     %d\n", rev);
    }
    else
    {
        fprintf(out, "Revision:     unknown (not an official upstream build)\n");
    }
    fprintf(out, "Build date:   %s\n", cfgmain_get_build_datetime());

    /* ---- Původ zdrojů ---- */
    const char *origin;
    switch (build_revision_get_repo_origin())
    {
    case BUILD_REVISION_ORIGIN_NAS1:
        origin = "NAS1 repository";
        break;
    case BUILD_REVISION_ORIGIN_GITHUB:
        origin = "GitHub (official upstream)";
        break;
    default:
        origin = "unknown";
        break;
    }
    fprintf(out, "Source:       %s\n", origin);

    if (build_revision_is_detached())
    {
        fprintf(out, "Branch:       detached HEAD\n");
    }
    else
    {
        fprintf(out, "Branch:       %s\n", _or_unknown(build_revision_get_branch()));
    }

    /* Neznámý stav (-1) se nevypisuje - nelze tvrdit čistý ani změněný strom. */
    const char *dirty_note = (build_revision_is_dirty() == 1) ? " (dirty: uncommitted changes)" : "";
    fprintf(out, "Commit:       %s%s\n", _or_unknown(build_revision_get_commit()), dirty_note);

    /* ---- Build ---- */
    fprintf(out, "Compiler:     %s\n", VERSION_INFO_COMPILER);
    fprintf(out, "Toolchain:    %s\n", _or_unknown(build_revision_get_build_msystem()));
    fprintf(out, "Target:       %s %s\n", SDL_GetPlatform(), VERSION_INFO_CPU_ARCH);
#ifdef NDEBUG
    fprintf(out, "Assertions:   disabled (NDEBUG)\n");
#else
    fprintf(out, "Assertions:   enabled\n");
#endif

    fprintf(out, "Features:    ");
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    fprintf(out, " debugger");
#else
    fprintf(out, " no-debugger");
#endif
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    fprintf(out, " mcp");
#else
    fprintf(out, " no-mcp");
#endif
#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
    fprintf(out, " mcp-tcp");
#else
    fprintf(out, " no-mcp-tcp");
#endif
#ifdef MZ800EMU_CFG_AUDIO_DISABLED
    fprintf(out, " no-audio");
#else
    fprintf(out, " audio");
#endif
#ifdef MZ800EMU_CFG_RAM_FASTPATH
    fprintf(out, " ram-fastpath");
#endif
#ifdef MZ800EMU_CFG_RAM_FASTPATH_VERIFY
    fprintf(out, " ram-fastpath-verify");
#endif
#ifdef FDC_DIAG
    fprintf(out, " fdc-diag");
#endif
    fprintf(out, "\n");

    /* ---- Knihovny ---- */
    int sdl_linked = SDL_GetVersion();
    fprintf(out, "SDL:          %d.%d.%d (compiled), %d.%d.%d (linked)\n",
            SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
            SDL_VERSIONNUM_MAJOR(sdl_linked), SDL_VERSIONNUM_MINOR(sdl_linked),
            SDL_VERSIONNUM_MICRO(sdl_linked));
    fprintf(out, "GLib:         %d.%d.%d (compiled), %u.%u.%u (linked)\n",
            GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION,
            glib_major_version, glib_minor_version, glib_micro_version);

    /* ---- Hostitel ---- */
    char host[128];
    _host_os(host, sizeof(host));
    fprintf(out, "Host OS:      %s\n", host);

    fflush(out);
}
