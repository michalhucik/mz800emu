/**
 * @file fdc_fileid.c
 * @brief Identita souboru obrazu diskety (kontrakt v fdc_fileid.h).
 *
 * Samostatný soubor kvůli hlavičce <windows.h>, která se do fdc.c netahá.
 *
 * License: GPLv3.
 */

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#include "fdc_fileid.h"


/**
 * @brief Vyrobí klíč cesty: absolutní cesta bez `.` a `..`, na Windows s `/` a bez ohledu na velikost písmen.
 * @param path Neprázdná cesta.
 * @return Nový řetězec (g_free), nikdy NULL.
 */
static char *fdc_fileid_make_path_key(const char *path)
{
    char *canon = g_canonicalize_filename(path, NULL);
#ifdef G_OS_WIN32
    for (char *p = canon; *p; p++)
        if (*p == '\\') *p = '/';
    if (g_utf8_validate(canon, -1, NULL))
    {
        char *folded = g_utf8_casefold(canon, -1);
        g_free(canon);
        canon = folded;
    }
    else
    {
        /* cesta mimo UTF-8: aspoň písmena ASCII */
        char *lower = g_ascii_strdown(canon, -1);
        g_free(canon);
        canon = lower;
    }
#endif
    return canon;
}


void fdc_fileid_init(st_FDC_FILEID *id, const char *path)
{
    if (!id)
        return;
    memset(id, 0, sizeof(*id));
    if (!path || !path[0])
        return;

    id->path_key = fdc_fileid_make_path_key(path);

#ifdef G_OS_WIN32
    wchar_t *wpath = (wchar_t *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    if (!wpath)
        return;
    /* přístup 0 = jen dotaz na metadata; symbolický odkaz se následuje */
    HANDLE h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    g_free(wpath);
    if (h == INVALID_HANDLE_VALUE)
        return;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h, &info))
    {
        uint64_t file = ((uint64_t)info.nFileIndexHigh << 32) | (uint64_t)info.nFileIndexLow;
        /* nulový index = souborový systém číslo souboru nevydává */
        if (file != 0)
        {
            id->volume = (uint64_t)info.dwVolumeSerialNumber;
            id->file = file;
            id->has_id = 1;
        }
    }
    CloseHandle(h);
#else
    GStatBuf st;
    if (g_stat(path, &st) == 0 && st.st_ino != 0)
    {
        id->volume = (uint64_t)st.st_dev;
        id->file = (uint64_t)st.st_ino;
        id->has_id = 1;
    }
#endif
}


void fdc_fileid_clear(st_FDC_FILEID *id)
{
    if (!id)
        return;
    g_free(id->path_key);
    memset(id, 0, sizeof(*id));
}


bool fdc_fileid_same(const st_FDC_FILEID *a, const st_FDC_FILEID *b)
{
    if (!a || !b || !a->path_key || !b->path_key)
        return false;
    if (strcmp(a->path_key, b->path_key) == 0)
        return true;
    return a->has_id && b->has_id && a->volume == b->volume && a->file == b->file;
}
