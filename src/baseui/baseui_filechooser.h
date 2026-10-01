#ifndef BASEUI_FILECHOOSER_H
#define BASEUI_FILECHOOSER_H

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

typedef enum baseui_fchooser_state_t
{
    BASEUI_FCHOOSER_STATE_CREATED,
    BASEUI_FCHOOSER_STATE_OPENED,
    BASEUI_FCHOOSER_STATE_CLOSED_OK,
    BASEUI_FCHOOSER_STATE_CLOSED_CANCEL,
    BASEUI_FCHOOSER_STATE_CLOSED_ERROR
} baseui_fchooser_state_t;

typedef enum baseui_fchooser_type_t
{
    BASEUI_FCHOOSER_TYPE_OPEN_FILE, // otevrit pro cteni
    BASEUI_FCHOOSER_TYPE_SAVE_FILE, // otevrit pro zapis (vytvoreni) - pokud soubor existuje, zepta se na prepis
    BASEUI_FCHOOSER_TYPE_RW_FILE,   // otevrit pro cteni a zapis (existujici soubor, nebo novy) - pokud soubor existuje, nezepta se na prepis
    BASEUI_FCHOOSER_TYPE_OPEN_DIR   // otevrit adresar
} baseui_fchooser_type_t;

typedef struct baseui_fchooser_t baseui_fchooser_t;
typedef void (*BaseuiFchooserCb)(baseui_fchooser_t *fch);

typedef struct baseui_fchooser_t
{
    baseui_fchooser_state_t state;
    baseui_fchooser_type_t type;
    char *title;
    char *filter;
    char *path;         // implicitni cesta
    char *fileName;     // implicitni nazev souboru pro ulozeni
    char *filePathName; // cesta a nazev souboru
    char *selected_filePathName;
    char *selected_path;
    char *selected_filter; // zobrazovaný název zvoleného filtru
    BaseuiFchooserCb cb;
    gpointer user_data;
    GMutex mutex;
    GCond cond;
} baseui_fchooser_t;

#ifdef __cplusplus
extern "C"
{
#endif

    baseui_fchooser_t *baseui_filechooser_open_file(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data);
    baseui_fchooser_t *baseui_filechooser_open_dir(const char *title, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data);
    baseui_fchooser_t *baseui_filechooser_open_rw_file(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data);
    baseui_fchooser_t *baseui_filechooser_save_file(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data);
    void baseui_filechooser_destroy(baseui_fchooser_t *fch);

    char *baseui_filechooser_open_file_wait(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, char **selected_path);
    char *baseui_filechooser_open_dir_wait(const char *title, const char *path, const char *fileName, const char *filePathName);
    char *baseui_filechooser_open_rw_file_wait(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, char **selected_path);
    char *baseui_filechooser_save_file_wait(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, char **selected_path);

    const char *baseui_filechooser_get_extension_ptr(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* BASEUI_FILECHOOSER_H */
