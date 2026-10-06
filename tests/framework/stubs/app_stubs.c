/*
 * app_stubs.c — stub implementace pro symboly z UI/SDL/version_check
 *
 * Emulátorové jádro referencuje funkce z UI a SDL vrstev,
 * které v headless testu nepotřebujeme. Tento soubor poskytuje
 * prázdné implementace aby linker byl spokojený.
 *
 * Licence: GPLv3
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

/* ================================================================
 * sdlapp stubs
 * ================================================================ */

/* SdlApp je typedef na struct v sdlapp.h — nemůžeme ho includovat
 * bez SDL3, ale potřebujeme jen tyto dvě funkce. Přijímají void*. */

int sdlapp_is_running(void *app)
{
    (void)app;
    return 0; /* v testu "neběží" — zabrání nekonečným smyčkám */
}

void sdlapp_quit(void *app)
{
    (void)app;
}

int sdlapp_is_quit_requested(void *app)
{
    (void)app;
    return 0; /* v testu "ukončení nevyžádáno" — neutrální default */
}

void *sdlapp_get_winmanager(void *app)
{
    (void)app;
    return NULL; /* v testu nemáme window manager */
}

void *sdlapp_winmanager_get_window_by_name(void *wm, const char *name)
{
    (void)wm;
    (void)name;
    return NULL; /* v testu nemáme okna */
}

int sdlapp_send_message_for_SDL_windowID(uint32_t win_id, uint32_t type, int32_t code, void *data1, void *data2)
{
    (void)win_id;
    (void)type;
    (void)code;
    (void)data1;
    (void)data2;
    return 0; /* v testu neposíláme SDL eventy */
}

/* ================================================================
 * version_check stubs
 * ================================================================ */

void version_check_init(void)
{
    /* nic — v testu neprovádíme HTTP kontrolu verze */
}

void version_check_exit(void)
{
    /* nic */
}

/* version_check_branch_* a version_check_report_* — používány
 * v snapshot kódu pro generování version reportu */

void *version_check_report_new(void)
{
    return NULL;
}

void version_check_report_add_branch(void *report, void *branch)
{
    (void)report;
    (void)branch;
}

void *version_check_branch_new(const char *name, const char *version, uint32_t version_int)
{
    (void)name;
    (void)version;
    (void)version_int;
    return NULL;
}

void version_check_branch_set_msg(void *branch, const char *msg)
{
    (void)branch;
    (void)msg;
}

int version_xml_document_parse(const char *xml, void *report)
{
    (void)xml;
    (void)report;
    return -1;
}

/* ================================================================
 * build_revision stubs
 * ================================================================ */

uint32_t build_revision_get_int(void)
{
    return 0;
}

/* ================================================================
 * baseui stubs (filechooser a message dialogy)
 *
 * baseui_tools_* NEJSOU stubovány — ty se linkují z baseui_tools.o
 * ================================================================ */

/* filechooser — v testech neotvíráme souborové dialogy */

void *baseui_filechooser_open_file(const char *title, const char *filter)
{
    (void)title;
    (void)filter;
    return NULL;
}

/**
 * @brief Stub: v testu není interaktivní GUI, blokující dialog by nikdo nezavřel.
 * @return false (stejně jako produkční kód v --headless).
 */
bool baseui_filechooser_can_wait(void)
{
    return false;
}

void *baseui_filechooser_open_file_wait(const char *title, const char *filter)
{
    (void)title;
    (void)filter;
    return NULL;
}

void *baseui_filechooser_save_file(const char *title, const char *filter)
{
    (void)title;
    (void)filter;
    return NULL;
}

void *baseui_filechooser_open_dir(const char *title)
{
    (void)title;
    return NULL;
}

void *baseui_filechooser_open_rw_file(const char *title, const char *filter)
{
    (void)title;
    (void)filter;
    return NULL;
}

const char *baseui_filechooser_get_extension_ptr(void *fc)
{
    (void)fc;
    return NULL;
}

void baseui_filechooser_destroy(void *fc)
{
    (void)fc;
}

/* zprávy a chyby — v testech jen logujeme na stderr */

void baseui_show_message(const char *msg)
{
    fprintf(stderr, "[stub] baseui_show_message: %s\n", msg);
}

void baseui_show_error_message(const char *msg)
{
    fprintf(stderr, "[stub] baseui_show_error_message: %s\n", msg);
}

int baseui_cmt_check_mzf_filesize(const char *filename, unsigned int expected_size, unsigned int real_size)
{
    (void)filename;
    (void)expected_size;
    (void)real_size;
    return 0; /* přijmout jak je */
}

/* ================================================================
 * imgui stubs — funkce z ImGui UI které jádro referencuje
 * ================================================================ */

void imgui_cmt_tape_update_filelist(void)
{
    /* nic */
}

void imgui_cmt_fix_mzfsize_popup_activate(void)
{
    /* nic */
}

void imgui_message_vprintf(int type, const char *fmt, va_list args)
{
    (void)type;
    /* v testech přepošleme na stderr */
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
}

void imgui_version_check_report_done(void *report)
{
    (void)report;
}

void imgui_filechooser_new(void)
{
    /* nic */
}

/* ================================================================
 * debugger UI stubs — jádro debuggeru volá UI funkce pro
 * zobrazení/skrytí okna, v headless testu jsou nepotřebné.
 * ================================================================ */

void ui_debugger_show_main_window(void)
{
    /* nic — v testu nemáme UI */
}

void ui_debugger_hide_main_window(void)
{
    /* nic */
}

void bpt_state_init(void)
{
    /* nic — v testu nemáme UI breakpointů */
}

/* ================================================================
 * iface_joy stubs (nahrazují iface_joy.o který závisí na SDL3)
 * Deklarace bez #include iface_joy.h — přetáhlo by sdlapp.h
 * s konfliktními typy vůči void* stubům výše.
 * ================================================================ */

bool iface_joy_lowlevel_init(void)
{
    return true;
}

void iface_joy_lowlevel_exit(void)
{
    /* nic */
}

void iface_joy_get_calibration(void)
{
    /* nic */
}

bool iface_joy_init(void)
{
    return true;
}

void iface_joy_exit(void)
{
    /* nic */
}

int iface_joy_open_configured_joyid(int joy_devid)
{
    (void)joy_devid;
    return -1;
}

uint8_t iface_joy_scan(int joy_devid)
{
    (void)joy_devid;
    return 0xff;
}

/* ================================================================
 * Memory Heatmap GUI okno (CDL) - stub pro headless test
 * ================================================================
 * src/emulator/debugger/debugger.c volá tuto funkci pro registraci
 * GUI persistence klíčů do cfgmodule. V testu UI nemáme, takže nic
 * neregistrujeme - testy debugger logiky tím nejsou ovlivněny.
 */
void mhmap_window_register_persistence(void *cmod_void)
{
    (void)cmod_void;
}


/* ================================================================
 *  dasm_window_register_persistence + dasm_window_apply_persisted stub
 * ================================================================
 * Stejně jako mhmap_window_register_persistence - debugger.c volá tyto
 * funkce pro UI persistenci Disassembler V1 okna (mutant
 * disassembler-window-v1). V testech UI nemáme - stuby jsou no-op.
 */
void dasm_window_register_persistence(void *cmod_void)
{
    (void)cmod_void;
}

void dasm_window_apply_persisted(void)
{
}
