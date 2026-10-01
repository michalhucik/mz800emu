#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <glib.h>

// Lokalizace
#include "i18n.h"

#include "baseui_filechooser.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "libs/sdlapp/sdlapp.h"

/**
 * @brief Destrukce struktury filechooseru. Vse co není NULL je uvolněno.
 * @param fch Ukazatel na strukturu filechooseru
 * @return void
 */
void baseui_filechooser_destroy(baseui_fchooser_t *fch)
{
    if (fch == NULL)
    {
        return;
    };
    g_mutex_lock(&fch->mutex);
    if (fch->selected_filePathName != NULL)
    {
        free(fch->selected_filePathName);
        fch->selected_filePathName = NULL;
    };
    if (fch->selected_path != NULL)
    {
        free(fch->selected_path);
        fch->selected_path = NULL;
    };
    if (fch->selected_filter != NULL)
    {
        free(fch->selected_filter);
        fch->selected_filter = NULL;
    };
    if (fch->title != NULL)
    {
        g_free(fch->title);
        fch->title = NULL;
    };
    if (fch->filter != NULL)
    {
        g_free(fch->filter);
        fch->filter = NULL;
    };
    if (fch->filePathName != NULL)
    {
        g_free(fch->filePathName);
        fch->filePathName = NULL;
    };
    if (fch->path != NULL)
    {
        g_free(fch->path);
        fch->path = NULL;
    };
    if (fch->fileName != NULL)
    {
        g_free(fch->fileName);
        fch->fileName = NULL;
    };
    g_mutex_unlock(&fch->mutex);
    g_mutex_clear(&fch->mutex);
    g_cond_clear(&fch->cond);
    free(fch);
}

static baseui_fchooser_t *baseui_filechooser_create_new(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data, baseui_fchooser_type_t type)
{
    baseui_fchooser_t *fch = g_new0(baseui_fchooser_t, 1);
    if (fch == NULL)
    {
        return NULL;
    };
    fch->state = BASEUI_FCHOOSER_STATE_CREATED;

    const char *default_title = NULL;
    switch (type)
    {
    case BASEUI_FCHOOSER_TYPE_OPEN_FILE:
        default_title = _("Open File");
        break;
    case BASEUI_FCHOOSER_TYPE_SAVE_FILE:
        default_title = _("Save File");
        break;
    case BASEUI_FCHOOSER_TYPE_RW_FILE:
        default_title = _("Open RW File");
        break;
    case BASEUI_FCHOOSER_TYPE_OPEN_DIR:
        default_title = _("Open Directory");
        break;
    default:
        default_title = _("Untitled File Chooser");
        break;
    }

    fch->title = (title != NULL) ? g_strdup(title) : g_strdup(default_title);
    fch->filter = (filter != NULL) ? g_strdup(filter) : NULL;

    if (filePathName != NULL)
    {
        fch->filePathName = g_strdup(filePathName);
    }
    else
    {
        fch->path = (path != NULL) ? g_strdup(path) : g_strdup(".");
        fch->fileName = (fileName != NULL) ? g_strdup(fileName) : NULL;
    };

    fch->selected_filePathName = NULL;
    fch->selected_path = NULL;
    fch->selected_filter = NULL;
    fch->cb = cb;
    fch->user_data = user_data;
    fch->type = type;
    g_mutex_init(&fch->mutex);
    g_cond_init(&fch->cond);
    return fch;
}

/**
 * @brief Otevře dialog pro výběr souboru pro čtení.
 * @param title Titulek dialogu
 * @param filter Filtr souborů
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @param cb Callback funkce
 * @param user_data Data pro callback funkci
 * @return Ukazatel na strukturu filechooseru
 */
baseui_fchooser_t *baseui_filechooser_open_file(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data)
{
    baseui_fchooser_t *fch = baseui_filechooser_create_new(title, filter, path, fileName, filePathName, cb, user_data, BASEUI_FCHOOSER_TYPE_OPEN_FILE);
    if (fch == NULL)
    {
        return NULL;
    };
    imgui_filechooser_new(fch);
    return fch;
}

/**
 * @brief Otevře dialog pro výběr adresáře.
 * @param title Titulek dialogu
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @param cb Callback funkce
 * @param user_data Data pro callback funkci
 * @return Ukazatel na strukturu filechooseru
 */
baseui_fchooser_t *baseui_filechooser_open_dir(const char *title, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data)
{
    baseui_fchooser_t *fch = baseui_filechooser_create_new(title, NULL, path, fileName, filePathName, cb, user_data, BASEUI_FCHOOSER_TYPE_OPEN_DIR);
    if (fch == NULL)
    {
        return NULL;
    };
    imgui_filechooser_new(fch);
    return fch;
}

/**
 * @brief Otevře dialog pro výběr souboru pro čtení a zápis, nebo vytvoření nového souboru. Pokud soubor existuje, nezeptá se na přepsání.
 * @param title Titulek dialogu
 * @param filter Filtr souborů
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @param cb Callback funkce
 * @param user_data Data pro callback funkci
 * @return Ukazatel na strukturu filechooseru
 */
baseui_fchooser_t *baseui_filechooser_open_rw_file(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data)
{
    baseui_fchooser_t *fch = baseui_filechooser_create_new(title, filter, path, fileName, filePathName, cb, user_data, BASEUI_FCHOOSER_TYPE_RW_FILE);
    if (fch == NULL)
    {
        return NULL;
    };
    imgui_filechooser_new(fch);
    return fch;
}

/**
 * @brief Otevře dialog pro výběr souboru pro zápis (vytvoření). Pokud soubor existuje, zeptá se na přepsání.
 * @param title Titulek dialogu
 * @param filter Filtr souborů
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @param cb Callback funkce
 * @param user_data Data pro callback funkci
 * @return Ukazatel na strukturu filechooseru
 */
baseui_fchooser_t *baseui_filechooser_save_file(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, BaseuiFchooserCb cb, gpointer user_data)
{
    baseui_fchooser_t *fch = baseui_filechooser_create_new(title, filter, path, fileName, filePathName, cb, user_data, BASEUI_FCHOOSER_TYPE_SAVE_FILE);
    if (fch == NULL)
    {
        return NULL;
    };
    imgui_filechooser_new(fch);
    return fch;
}

/**
 * @brief Čeká na dokončení dialogu a vrátí vybraný soubor, nebo adresář.
 * @param fch Ukazatel na strukturu filechooseru
 * @param selected_path Ukazatel na proměnnou, kam se uloží vybraný adresář (pouze pokud neni NULL)
 * @return Vybraný soubor, nebo NULL
 */
static char *baseui_filechooser_wait(baseui_fchooser_t *fch, char **selected_path)
{
    if (selected_path != NULL)
    {
        *selected_path = NULL;
    };

    g_mutex_lock(&fch->mutex);
    // Cekame na dokonceni dialogu, nebo na ukonceni sdlapp (1s)
    while ((fch->state == BASEUI_FCHOOSER_STATE_OPENED) && (sdlapp_is_running(g_sdlapp)))
    {
        g_cond_wait_until(&fch->cond, &fch->mutex, g_get_monotonic_time() + 1000000);
    };
    g_mutex_unlock(&fch->mutex);

    if (!sdlapp_is_running(g_sdlapp))
    {
        baseui_filechooser_destroy(fch);
        return NULL;
    };

    char *selected = NULL;

    if (fch->state == BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        switch (fch->type)
        {
        case BASEUI_FCHOOSER_TYPE_OPEN_FILE:
        case BASEUI_FCHOOSER_TYPE_SAVE_FILE:
        case BASEUI_FCHOOSER_TYPE_RW_FILE:
            if (selected_path != NULL)
            {
                *selected_path = fch->selected_path;
                fch->selected_path = NULL;
            };
            selected = fch->selected_filePathName;
            fch->selected_filePathName = NULL;
            break;

        case BASEUI_FCHOOSER_TYPE_OPEN_DIR:
            selected = fch->selected_path;
            fch->selected_path = NULL;
            break;

        default:
            break;
        };
    };

    baseui_filechooser_destroy(fch);
    return selected;
}

/**
 * @brief Otevře dialog pro výběr souboru pro čtení a vrátí vybraný soubor. Čeká na dokončení dialogu.
 * @param title Titulek dialogu
 * @param filter Filtr souborů
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @param selected_path Ukazatel na proměnnou, kam se uloží vybraný adresář (pouze pokud neni NULL)
 * @return Vybraný soubor, nebo NULL
 */
char *baseui_filechooser_open_file_wait(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, char **selected_path)
{
    baseui_fchooser_t *fch = baseui_filechooser_open_file(title, filter, path, fileName, filePathName, NULL, NULL);
    if (fch == NULL)
    {
        if (selected_path != NULL)
            *selected_path = NULL;
        return NULL;
    };
    return baseui_filechooser_wait(fch, selected_path);
}

/**
 * @brief Otevře dialog pro výběr adresáře a vrátí vybraný adresář. Čeká na dokončení dialogu.
 * @param title Titulek dialogu
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @return Vybraný adresář, nebo NULL
 */
char *baseui_filechooser_open_dir_wait(const char *title, const char *path, const char *fileName, const char *filePathName)
{
    baseui_fchooser_t *fch = baseui_filechooser_open_dir(title, path, fileName, filePathName, NULL, NULL);
    if (fch == NULL)
    {
        return NULL;
    };
    return baseui_filechooser_wait(fch, NULL);
}

/**
 * @brief Otevře dialog pro výběr souboru pro čtení a zápis, nebo vytvoření nového souboru a vrátí vybraný soubor. Čeká na dokončení dialog
 * @param title Titulek dialogu
 * @param filter Filtr souborů
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @param selected_path Ukazatel na proměnnou, kam se uloží vybraný adresář (pouze pokud neni NULL)
 * @return Vybraný soubor, nebo NULL
 */
char *baseui_filechooser_open_rw_file_wait(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, char **selected_path)
{
    baseui_fchooser_t *fch = baseui_filechooser_open_rw_file(title, filter, path, fileName, filePathName, NULL, NULL);
    if (fch == NULL)
    {
        if (selected_path != NULL)
            *selected_path = NULL;
        return NULL;
    };
    return baseui_filechooser_wait(fch, selected_path);
}

/**
 * @brief Otevře dialog pro výběr souboru pro zápis (vytvoření) a vrátí vybraný soubor. Čeká na dokončení dialogu.
 * @param title Titulek dialogu
 * @param filter Filtr souborů
 * @param path Výchozí cesta
 * @param fileName Výchozí název souboru
 * @param filePathName Plná cesta a název souboru k otevření
 * @param selected_path Ukazatel na proměnnou, kam se uloží vybraný adresář (pouze pokud neni NULL)
 * @return Vybraný soubor, nebo NULL
 */
char *baseui_filechooser_save_file_wait(const char *title, const char *filter, const char *path, const char *fileName, const char *filePathName, char **selected_path)
{
    baseui_fchooser_t *fch = baseui_filechooser_save_file(title, filter, path, fileName, filePathName, NULL, NULL);
    if (fch == NULL)
    {
        if (selected_path != NULL)
            *selected_path = NULL;
        return NULL;
    };
    return baseui_filechooser_wait(fch, selected_path);
}

/**
 * @brief Získá příponu souboru z cesty k souboru. Pozor, vrací i tečku před příponou.
 * @param filepath Cesta k souboru
 * @return Ukazatel na příponu souboru, nebo NULL pokud není nalezena
 */
const char *baseui_filechooser_get_extension_ptr(const char *filepath)
{
    if (filepath == NULL)
        return NULL;

    const char *dot = NULL;
    const char *p = filepath;

    while (*p)
    {
        if (*p == '.')
        {
            dot = p;
        }

        p = g_utf8_next_char(p);
    }

    return dot;
};
