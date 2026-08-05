/**
 * @file membrowser_fileio.cpp
 * @brief Load BIN / Save BIN file dialog handlery - implementace.
 *
 * Použity IGFD (ImGuiFileDialog) - standardní project file chooser.
 * Vzorem je src/ui-imgui/memext/memext_save_dialog.cpp.
 *
 * Workflow Save (2-step):
 *   1. Range modal popup: From (hex) + Size (hex) + [x] Whole region
 *      checkbox. Uživatel potvrdí range nebo zruší.
 *   2. IGFD save file chooser - po confirm se range bajtů dumpne do .bin.
 *
 * Workflow Load (3-step):
 *   1. IGFD load file chooser - uživatel vybere zdrojový soubor.
 *      (Soubor se musí znát první kvůli default Size = min(file_size,
 *       region_size - To).)
 *   2. Range modal popup: To (hex, v target regionu) + Size (hex) +
 *      Source offset (hex, v source souboru) + [x] Whole file checkbox.
 *   3. Po confirm se file [src .. src+size] zapíše do region [To .. To+size].
 *
 * Edit master toggle: Load BIN ho NEbere v potaz - je to deliberate
 * uživatelská akce (klik na tlačítko = explicit consent). Save je vždy
 * read-only operace, edit toggle neřeší.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_fileio.h"
#include "membrowser_io.h"
#include "hex_view_backend.h"

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <sys/stat.h>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
}


/* --- Save state machine. --- */

/**
 * @brief Fáze Save workflow.
 *
 * IDLE       = žádný save neaktivní.
 * RANGE      = otevřený range modal popup (From/Size/Whole).
 * IGFD       = IGFD file chooser běží, range už schválen.
 */
enum class SavePhase { IDLE, RANGE, IGFD };

static SavePhase s_save_phase = SavePhase::IDLE;
static bool s_save_need_open = false;       /**< OpenPopup() pending request. */
static int s_save_region_id = -1;
static uint64_t s_save_region_size = 0;     /**< Total size aktivního regionu (cache). */
static uint64_t s_save_from = 0;            /**< Posuv v rámci regionu. */
static uint64_t s_save_size = 0;            /**< Počet bajtů k dumpnutí. */
static bool s_save_whole = true;            /**< Whole region checkbox stav. */
static char s_save_from_text[32] = "0";     /**< InputText hex bufer From. */
static char s_save_size_text[32] = "0";     /**< InputText hex bufer Size. */
static char s_save_error[256] = "";


/* --- Load state machine. --- */

/**
 * @brief Fáze Load workflow.
 *
 * IDLE       = žádný load neaktivní.
 * IGFD       = IGFD file chooser běží, čekáme na výběr souboru.
 * RANGE      = otevřený range modal popup (To/Size/Source offset/Whole file).
 */
enum class LoadPhase { IDLE, IGFD, RANGE };

static LoadPhase s_load_phase = LoadPhase::IDLE;
static int s_load_region_id = -1;
static uint64_t s_load_region_size = 0;     /**< Total size cílového regionu. */
static std::string s_load_file_path;        /**< Vybraný source path. */
static uint64_t s_load_file_size = 0;       /**< Velikost source souboru v bajtech. */
static uint64_t s_load_to = 0;              /**< Posuv v rámci regionu. */
static uint64_t s_load_size = 0;            /**< Počet bajtů k načtení. */
static uint64_t s_load_src_off = 0;         /**< Posuv v rámci source souboru. */
static bool s_load_whole = true;            /**< Whole file checkbox stav. */
static char s_load_to_text[32] = "0";
static char s_load_size_text[32] = "0";
static char s_load_src_text[32] = "0";
static char s_load_error[256] = "";


/**
 * @brief Zjistí total_size regionu (helper pro defaulty).
 *
 * @param region_id Region ID.
 * @return Total size v bajtech, 0 při chybě (region disconnected apod.).
 */
static uint64_t fetch_region_size ( int region_id )
{
    st_HEX_VIEW_BACKEND be;
    membrowser_io_make_emu_backend ( &be, region_id, false );
    if ( !be.total_size ) return 0;
    return be.total_size ( be.ctx );
}


/**
 * @brief Parse hex string ("1234", "0x1234", "1234h") na uint64_t.
 *
 * Tolerantní: prázdný string → 0, neúspěch → 0. Sufix "h"/"H" ignoruje.
 * Prefix "0x"/"0X" ignoruje.
 *
 * @param s Vstupní text z InputText.
 * @return Parsovaná hodnota nebo 0.
 */
static uint64_t parse_hex ( const char *s )
{
    if ( !s ) return 0;
    while ( *s == ' ' || *s == '\t' ) ++s;
    if ( s[0] == '0' && ( s[1] == 'x' || s[1] == 'X' ) ) s += 2;

    uint64_t v = 0;
    while ( *s ) {
        char c = *s++;
        if ( c >= '0' && c <= '9' ) v = ( v << 4 ) | ( uint64_t ) ( c - '0' );
        else if ( c >= 'a' && c <= 'f' ) v = ( v << 4 ) | ( uint64_t ) ( c - 'a' + 10 );
        else if ( c >= 'A' && c <= 'F' ) v = ( v << 4 ) | ( uint64_t ) ( c - 'A' + 10 );
        else if ( c == 'h' || c == 'H' ) break;
        else break;
    }
    return v;
}


/**
 * @brief Naformátuje uint64_t jako hex (bez prefixu) do bufferu.
 *
 * @param[out] buf      Cílový buffer.
 * @param[in]  buf_size Velikost cílového bufferu.
 * @param[in]  v        Hodnota.
 */
static void format_hex ( char *buf, size_t buf_size, uint64_t v )
{
    std::snprintf ( buf, buf_size, "%llX", ( unsigned long long ) v );
}


/**
 * @brief Zjistí velikost souboru přes stat() (cross-platform).
 *
 * @param path UTF-8 cesta.
 * @return Velikost v bajtech nebo 0 (i pro chybu).
 */
static uint64_t file_size_bytes ( const char *path )
{
    struct stat st;
    if ( stat ( path, &st ) != 0 ) return 0;
    if ( st.st_size < 0 ) return 0;
    return ( uint64_t ) st.st_size;
}


extern "C" void membrowser_fileio_open_save ( int region_id )
{
    if ( region_id < 0 ) return;

    s_save_region_id = region_id;
    s_save_region_size = fetch_region_size ( region_id );
    if ( s_save_region_size == 0 ) return;

    /* Default: Whole region ON, From=0, Size=region_size. */
    s_save_whole = true;
    s_save_from = 0;
    s_save_size = s_save_region_size;
    format_hex ( s_save_from_text, sizeof ( s_save_from_text ), s_save_from );
    format_hex ( s_save_size_text, sizeof ( s_save_size_text ), s_save_size );

    s_save_error[0] = '\0';
    s_save_phase = SavePhase::RANGE;
    s_save_need_open = true;
}


extern "C" void membrowser_fileio_open_load ( int region_id )
{
    if ( region_id < 0 ) return;

    s_load_region_id = region_id;
    s_load_region_size = fetch_region_size ( region_id );
    if ( s_load_region_size == 0 ) return;

    s_load_file_path.clear ( );
    s_load_file_size = 0;
    s_load_to = 0;
    s_load_size = 0;
    s_load_src_off = 0;
    s_load_whole = true;
    s_load_error[0] = '\0';

    s_load_phase = LoadPhase::IGFD;

    IGFD::FileDialogConfig config;
    config.path = ".";
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                   | ImGuiFileDialogFlags_DontShowHiddenFiles
                   | ImGuiFileDialogFlags_ShowDevicesButton;
    ImGuiFileDialog::Instance ( )->OpenDialog (
        "MembrowserLoadFch",
        _( "Load .bin into region" ),
        ".bin,.dat,.*",
        config );
}


/**
 * @brief Otevře IGFD save dialog po schválení range.
 */
static void open_save_igfd ( void )
{
    IGFD::FileDialogConfig config;
    config.path = ".";
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                   | ImGuiFileDialogFlags_DontShowHiddenFiles
                   | ImGuiFileDialogFlags_ShowDevicesButton
                   | ImGuiFileDialogFlags_ConfirmOverwrite;
    ImGuiFileDialog::Instance ( )->OpenDialog (
        "MembrowserSaveFch",
        _( "Save region as .bin" ),
        ".bin,.dat,.*",
        config );
}


/**
 * @brief Vlastní I/O dump bajtů z regionu do souboru.
 *
 * @param path     Cesta k cílovému souboru.
 * @param from     Posuv v rámci regionu (offset prvního bajtu).
 * @param size     Počet bajtů k zápisu.
 * @return true při úspěchu.
 */
static bool do_save_bytes ( const std::string &path, uint64_t from, uint64_t size )
{
    st_HEX_VIEW_BACKEND be;
    membrowser_io_make_emu_backend ( &be, s_save_region_id, false );

    uint64_t total = be.total_size ( be.ctx );
    if ( from >= total ) {
        std::snprintf ( s_save_error, sizeof ( s_save_error ),
                        "%s", _( "From offset out of range" ) );
        return false;
    }
    if ( from + size > total ) size = total - from;
    if ( size == 0 ) {
        std::snprintf ( s_save_error, sizeof ( s_save_error ),
                        "%s", _( "Nothing to save (size is zero)" ) );
        return false;
    }

    FILE *fp = std::fopen ( path.c_str ( ), "wb" );
    if ( !fp ) {
        std::snprintf ( s_save_error, sizeof ( s_save_error ),
                        "%s", _( "Cannot open file for writing" ) );
        return false;
    }

    uint8_t buf[65536];
    uint64_t off = from;
    uint64_t end = from + size;
    bool ok = true;
    while ( off < end ) {
        uint32_t want = ( uint32_t )
            ( ( end - off > sizeof ( buf ) ) ? sizeof ( buf ) : end - off );
        int got = be.read_bytes ( be.ctx, off, buf, want );
        if ( got <= 0 ) { ok = false; break; }
        size_t w = std::fwrite ( buf, 1, ( size_t ) got, fp );
        if ( w != ( size_t ) got ) { ok = false; break; }
        off += ( uint64_t ) got;
    }
    std::fclose ( fp );

    if ( !ok ) {
        std::snprintf ( s_save_error, sizeof ( s_save_error ),
                        "%s", _( "Save failed (I/O error)" ) );
        return false;
    }
    return true;
}


/**
 * @brief Vlastní I/O nahrání bajtů ze souboru do regionu.
 *
 * @param path     Cesta ke zdrojovému souboru.
 * @param to       Posuv v rámci regionu (offset prvního zapisovaného bajtu).
 * @param size     Počet bajtů.
 * @param src_off  Posuv v rámci zdrojového souboru.
 * @return true při úspěchu.
 */
static bool do_load_bytes ( const std::string &path, uint64_t to,
                            uint64_t size, uint64_t src_off )
{
    st_HEX_VIEW_BACKEND be;
    membrowser_io_make_emu_backend ( &be, s_load_region_id, true );

    uint64_t total = be.total_size ( be.ctx );
    if ( !be.write_bytes ) {
        std::snprintf ( s_load_error, sizeof ( s_load_error ),
                        "%s", _( "Region is read-only" ) );
        return false;
    }
    if ( to >= total ) {
        std::snprintf ( s_load_error, sizeof ( s_load_error ),
                        "%s", _( "To offset out of range" ) );
        return false;
    }
    if ( to + size > total ) size = total - to;
    if ( size == 0 ) {
        std::snprintf ( s_load_error, sizeof ( s_load_error ),
                        "%s", _( "Nothing to load (size is zero)" ) );
        return false;
    }

    FILE *fp = std::fopen ( path.c_str ( ), "rb" );
    if ( !fp ) {
        std::snprintf ( s_load_error, sizeof ( s_load_error ),
                        "%s", _( "Cannot open file for reading" ) );
        return false;
    }
    /* fseek na source offset; ošetřeno velkými soubory přes _fseeki64
     * (MSVCRT/UCRT) případně fseeko (POSIX). MSYS2 UCRT poskytuje
     * _fseeki64 stejně jako fseeko. Použijeme fseek s long protože
     * V0 source files jsou typicky <2 GB; pro 16 MB Ramdisk je to
     * bezpečně v rozsahu. */
    if ( src_off > 0 ) {
#ifdef _WIN32
        if ( _fseeki64 ( fp, ( long long ) src_off, SEEK_SET ) != 0 ) {
            std::fclose ( fp );
            std::snprintf ( s_load_error, sizeof ( s_load_error ),
                            "%s", _( "Cannot seek source file" ) );
            return false;
        }
#else
        if ( fseeko ( fp, ( off_t ) src_off, SEEK_SET ) != 0 ) {
            std::fclose ( fp );
            std::snprintf ( s_load_error, sizeof ( s_load_error ),
                            "%s", _( "Cannot seek source file" ) );
            return false;
        }
#endif
    }

    uint8_t buf[65536];
    uint64_t off = to;
    uint64_t end = to + size;
    bool ok = true;
    while ( off < end ) {
        uint32_t want = ( uint32_t )
            ( ( end - off > sizeof ( buf ) ) ? sizeof ( buf ) : end - off );
        size_t r = std::fread ( buf, 1, want, fp );
        if ( r == 0 ) {
            std::snprintf ( s_load_error, sizeof ( s_load_error ),
                            "%s", _( "Source file ended before requested size" ) );
            ok = false;
            break;
        }
        int w = be.write_bytes ( be.ctx, off, buf, ( uint32_t ) r );
        if ( w != ( int ) r ) { ok = false; break; }
        off += ( uint64_t ) r;
    }
    std::fclose ( fp );

    if ( !ok && s_load_error[0] == '\0' ) {
        std::snprintf ( s_load_error, sizeof ( s_load_error ),
                        "%s", _( "Load failed (write error)" ) );
    }
    return ok;
}


/**
 * @brief Render Save range modal popup (krok 1 Save workflow).
 *
 * Po Save tlačítku přepne fázi na IGFD a otevře file chooser.
 */
static void render_save_range_modal ( void )
{
    if ( s_save_phase != SavePhase::RANGE ) return;

    /* OpenPopup() musí být volán ze stejného ID stacku jako
     * BeginPopupModal(); proto pending z open_save() vyřizujeme zde. */
    if ( s_save_need_open ) {
        ImGui::OpenPopup ( "###mb_save_range" );
        s_save_need_open = false;
    }

    ImGui::SetNextWindowSize ( ImVec2 ( 380, 0 ), ImGuiCond_Appearing );
    if ( ImGui::BeginPopupModal ( _L ( "Save BIN range###mb_save_range" ),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize ) ) {

        ImGui::Text ( "%s 0..%llXh", _( "Region range:" ),
                      ( unsigned long long ) ( s_save_region_size - 1 ) );
        ImGui::Separator ( );

        if ( ImGui::Checkbox ( _L ( "Whole region###mb_save_whole" ),
                                &s_save_whole ) ) {
            if ( s_save_whole ) {
                s_save_from = 0;
                s_save_size = s_save_region_size;
                format_hex ( s_save_from_text, sizeof ( s_save_from_text ),
                             s_save_from );
                format_hex ( s_save_size_text, sizeof ( s_save_size_text ),
                             s_save_size );
            }
        }

        if ( s_save_whole ) ImGui::BeginDisabled ( );

        ImGui::SetNextItemWidth ( 140 );
        if ( ImGui::InputText ( _L ( "From (hex)###mb_save_from" ),
                                 s_save_from_text, sizeof ( s_save_from_text ),
                                 ImGuiInputTextFlags_CharsHexadecimal
                                 | ImGuiInputTextFlags_AutoSelectAll ) ) {
            s_save_from = parse_hex ( s_save_from_text );
        }

        ImGui::SetNextItemWidth ( 140 );
        if ( ImGui::InputText ( _L ( "Size (hex)###mb_save_size" ),
                                 s_save_size_text, sizeof ( s_save_size_text ),
                                 ImGuiInputTextFlags_CharsHexadecimal
                                 | ImGuiInputTextFlags_AutoSelectAll ) ) {
            s_save_size = parse_hex ( s_save_size_text );
        }

        if ( s_save_whole ) ImGui::EndDisabled ( );

        /* Validace + náhled. */
        uint64_t end = s_save_from + s_save_size;
        bool valid = ( s_save_size > 0 )
                     && ( s_save_from < s_save_region_size )
                     && ( end <= s_save_region_size );

        ImGui::Separator ( );
        if ( valid ) {
            ImGui::Text ( "%s %llXh - %llXh (%llu B)", _( "Range:" ),
                          ( unsigned long long ) s_save_from,
                          ( unsigned long long ) ( end - 1 ),
                          ( unsigned long long ) s_save_size );
        } else {
            ImGui::PushStyleColor ( ImGuiCol_Text,
                                     ImVec4 ( 1.0f, 0.5f, 0.5f, 1.0f ) );
            ImGui::TextWrapped ( "%s", _( "Invalid range" ) );
            ImGui::PopStyleColor ( );
        }

        ImGui::Separator ( );

        if ( !valid ) ImGui::BeginDisabled ( );
        if ( ImGui::Button ( _L ( "Save###mb_save_range_ok" ),
                              ImVec2 ( 120, 0 ) ) ) {
            /* Přejdi na IGFD file chooser krok. */
            s_save_phase = SavePhase::IGFD;
            ImGui::CloseCurrentPopup ( );
            open_save_igfd ( );
        }
        if ( !valid ) ImGui::EndDisabled ( );

        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel###mb_save_range_cancel" ),
                              ImVec2 ( 120, 0 ) ) ) {
            s_save_phase = SavePhase::IDLE;
            ImGui::CloseCurrentPopup ( );
        }

        ImGui::EndPopup ( );
    }
}


/**
 * @brief Render Save IGFD file chooser (krok 2 Save workflow).
 *
 * Po IsOk vyvolá do_save_bytes() s dříve schváleným range.
 */
static void render_save_igfd ( void )
{
    if ( s_save_phase != SavePhase::IGFD ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "MembrowserSaveFch" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            do_save_bytes ( path, s_save_from, s_save_size );
        }
        ImGuiFileDialog::Instance ( )->Close ( );
        s_save_phase = SavePhase::IDLE;
    }
}


/**
 * @brief Render Save error popup (zobrazí se po neúspěšném I/O).
 */
static void render_save_error ( void )
{
    if ( s_save_error[0] == '\0' ) return;

    if ( ImGui::Begin ( _L ( "Save error###mb_save_err" ) ) ) {
        ImGui::PushStyleColor ( ImGuiCol_Text,
                                 ImVec4 ( 1.0f, 0.3f, 0.3f, 1.0f ) );
        ImGui::TextWrapped ( "%s", s_save_error );
        ImGui::PopStyleColor ( );
        if ( ImGui::Button ( _L ( "OK###mb_save_err_ok" ) ) ) {
            s_save_error[0] = '\0';
        }
    }
    ImGui::End ( );
}


/**
 * @brief Render Load IGFD file chooser (krok 1 Load workflow).
 *
 * Po IsOk si přečte file_size a otevře range modal.
 */
static void render_load_igfd ( void )
{
    if ( s_load_phase != LoadPhase::IGFD ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "MembrowserLoadFch" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            s_load_file_path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            s_load_file_size = file_size_bytes ( s_load_file_path.c_str ( ) );

            if ( s_load_file_size == 0 ) {
                std::snprintf ( s_load_error, sizeof ( s_load_error ),
                                "%s", _( "Source file is empty or unreadable" ) );
                s_load_phase = LoadPhase::IDLE;
                ImGuiFileDialog::Instance ( )->Close ( );
                return;
            }

            /* Default: Whole file ON, To=0, src=0, size=min(file, region). */
            s_load_whole = true;
            s_load_to = 0;
            s_load_src_off = 0;
            s_load_size = ( s_load_file_size < s_load_region_size )
                          ? s_load_file_size
                          : s_load_region_size;
            format_hex ( s_load_to_text, sizeof ( s_load_to_text ), s_load_to );
            format_hex ( s_load_size_text, sizeof ( s_load_size_text ), s_load_size );
            format_hex ( s_load_src_text, sizeof ( s_load_src_text ), s_load_src_off );

            s_load_phase = LoadPhase::RANGE;
            ImGui::OpenPopup ( "###mb_load_range" );
        } else {
            s_load_phase = LoadPhase::IDLE;
        }
        ImGuiFileDialog::Instance ( )->Close ( );
    }
}


/**
 * @brief Render Load range modal popup (krok 2 Load workflow).
 *
 * Po Load tlačítku vyvolá do_load_bytes().
 */
static void render_load_range_modal ( void )
{
    if ( s_load_phase != LoadPhase::RANGE ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 420, 0 ), ImGuiCond_Appearing );
    if ( ImGui::BeginPopupModal ( _L ( "Load BIN range###mb_load_range" ),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize ) ) {

        ImGui::Text ( "%s %s", _( "File:" ), s_load_file_path.c_str ( ) );
        ImGui::Text ( "%s %llu B (%llXh)", _( "File size:" ),
                      ( unsigned long long ) s_load_file_size,
                      ( unsigned long long ) s_load_file_size );
        ImGui::Text ( "%s 0..%llXh", _( "Region range:" ),
                      ( unsigned long long ) ( s_load_region_size - 1 ) );
        ImGui::Separator ( );

        if ( ImGui::Checkbox ( _L ( "Whole file###mb_load_whole" ),
                                &s_load_whole ) ) {
            if ( s_load_whole ) {
                s_load_src_off = 0;
                s_load_size = ( s_load_file_size < s_load_region_size - s_load_to )
                              ? s_load_file_size
                              : ( s_load_region_size - s_load_to );
                format_hex ( s_load_size_text, sizeof ( s_load_size_text ),
                             s_load_size );
                format_hex ( s_load_src_text, sizeof ( s_load_src_text ),
                             s_load_src_off );
            }
        }

        ImGui::SetNextItemWidth ( 140 );
        if ( ImGui::InputText ( _L ( "To (hex)###mb_load_to" ),
                                 s_load_to_text, sizeof ( s_load_to_text ),
                                 ImGuiInputTextFlags_CharsHexadecimal
                                 | ImGuiInputTextFlags_AutoSelectAll ) ) {
            s_load_to = parse_hex ( s_load_to_text );
            /* Pokud Whole file: dopočti Size aby se vešel do regionu. */
            if ( s_load_whole ) {
                uint64_t room = ( s_load_to < s_load_region_size )
                                ? ( s_load_region_size - s_load_to ) : 0;
                s_load_size = ( s_load_file_size < room ) ? s_load_file_size : room;
                format_hex ( s_load_size_text, sizeof ( s_load_size_text ),
                             s_load_size );
            }
        }

        if ( s_load_whole ) ImGui::BeginDisabled ( );

        ImGui::SetNextItemWidth ( 140 );
        if ( ImGui::InputText ( _L ( "Size (hex)###mb_load_size" ),
                                 s_load_size_text, sizeof ( s_load_size_text ),
                                 ImGuiInputTextFlags_CharsHexadecimal
                                 | ImGuiInputTextFlags_AutoSelectAll ) ) {
            s_load_size = parse_hex ( s_load_size_text );
        }

        ImGui::SetNextItemWidth ( 140 );
        if ( ImGui::InputText ( _L ( "Source offset (hex)###mb_load_src" ),
                                 s_load_src_text, sizeof ( s_load_src_text ),
                                 ImGuiInputTextFlags_CharsHexadecimal
                                 | ImGuiInputTextFlags_AutoSelectAll ) ) {
            s_load_src_off = parse_hex ( s_load_src_text );
        }

        if ( s_load_whole ) ImGui::EndDisabled ( );

        /* Validace. */
        uint64_t reg_end = s_load_to + s_load_size;
        uint64_t src_end = s_load_src_off + s_load_size;
        bool valid = ( s_load_size > 0 )
                     && ( s_load_to < s_load_region_size )
                     && ( reg_end <= s_load_region_size )
                     && ( s_load_src_off < s_load_file_size )
                     && ( src_end <= s_load_file_size );

        ImGui::Separator ( );
        if ( valid ) {
            ImGui::Text ( "%s src[%llXh..%llXh] -> region[%llXh..%llXh]",
                          _( "Plan:" ),
                          ( unsigned long long ) s_load_src_off,
                          ( unsigned long long ) ( src_end - 1 ),
                          ( unsigned long long ) s_load_to,
                          ( unsigned long long ) ( reg_end - 1 ) );
        } else {
            ImGui::PushStyleColor ( ImGuiCol_Text,
                                     ImVec4 ( 1.0f, 0.5f, 0.5f, 1.0f ) );
            ImGui::TextWrapped ( "%s",
                _( "Invalid range (check region/file bounds)" ) );
            ImGui::PopStyleColor ( );
        }

        ImGui::Separator ( );

        if ( !valid ) ImGui::BeginDisabled ( );
        if ( ImGui::Button ( _L ( "Load###mb_load_range_ok" ),
                              ImVec2 ( 120, 0 ) ) ) {
            do_load_bytes ( s_load_file_path, s_load_to,
                            s_load_size, s_load_src_off );
            s_load_phase = LoadPhase::IDLE;
            ImGui::CloseCurrentPopup ( );
        }
        if ( !valid ) ImGui::EndDisabled ( );

        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel###mb_load_range_cancel" ),
                              ImVec2 ( 120, 0 ) ) ) {
            s_load_phase = LoadPhase::IDLE;
            ImGui::CloseCurrentPopup ( );
        }

        ImGui::EndPopup ( );
    }
}


/**
 * @brief Render Load error popup (po neúspěšném I/O).
 */
static void render_load_error ( void )
{
    if ( s_load_error[0] == '\0' ) return;

    if ( ImGui::Begin ( _L ( "Load result###mb_load_err" ) ) ) {
        ImGui::PushStyleColor ( ImGuiCol_Text,
                                 ImVec4 ( 1.0f, 0.6f, 0.3f, 1.0f ) );
        ImGui::TextWrapped ( "%s", s_load_error );
        ImGui::PopStyleColor ( );
        if ( ImGui::Button ( _L ( "OK###mb_load_err_ok" ) ) ) {
            s_load_error[0] = '\0';
        }
    }
    ImGui::End ( );
}


extern "C" void membrowser_fileio_render_dialogs ( void )
{
    render_save_range_modal ( );
    render_save_igfd ( );
    render_save_error ( );

    render_load_igfd ( );
    render_load_range_modal ( );
    render_load_error ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
