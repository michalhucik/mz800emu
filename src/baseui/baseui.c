#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "baseui/baseui.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/message/message_window.h"

#ifdef USE_NFDIALOG
// #include "nfd.h"
#endif // USE_NFDIALOG

/**
 * @brief Show message to user
 * @brief If is_error is true, message is printed to stderr, otherwise to stdout
 */
void baseui_show_message(bool is_error, char *format, ...)
{
    FILE *stream = is_error ? stderr : stdout;
    va_list args, args_copy;
    va_start(args, format);
    va_copy(args_copy, args);
    vfprintf(stream, format, args);
    ui_message_type_t msg_type = is_error ? MESSAGE_TYPE_ERROR : MESSAGE_TYPE_INFO;
    imgui_message_vprintf(msg_type, format, args_copy);
    va_end(args_copy);
    va_end(args);
}

/**
 * @brief Show error message to user
 */
void baseui_show_error_message(char *format, ...)
{
    va_list args, args_copy;
    va_start(args, format);
    va_copy(args_copy, args);
    vfprintf(stderr, format, args);
    imgui_message_vprintf(MESSAGE_TYPE_ERROR, format, args_copy);
    va_end(args_copy);
    va_end(args);
}

void baseui_cmt_check_mzf_filesize(const char *filename, int valid_size)
{
    FILE *fh = baseui_tools_file_open(filename, "rb");
    if (!fh)
    {
        /* Soubor mezitím zmizel nebo nejde číst: kontrola se přeskočí,
         * chybu otevření ohlásí volající při vlastním čtení. */
        fprintf(stderr, "MZF file size check: cannot open '%s'\n", filename ? filename : "");
        return;
    };
    if (0 != fseek(fh, 0, SEEK_END))
    {
        fclose(fh);
        return;
    };
    long file_size = ftell(fh);
    fclose(fh);
    if (file_size < 0)
        return;
    if (file_size > valid_size)
    {
        imgui_cmt_fix_mzfsize_popup_activate(filename, valid_size, (int)file_size);
    };
    return;
}

#if 0
char *baseui_filechhoser_open_file(char *last_file, const char *filter)
{
    (void)last_file;

#ifdef USE_NFDIALOG
    nfdchar_t *outPath = NULL;
    nfdresult_t result = NFD_OpenDialog(filter, NULL, &outPath);
    if (result == NFD_OKAY)
    {
        puts("Success!");
        puts(outPath);
        // free(outPath);
    }
    else if (result == NFD_CANCEL)
    {
        puts("User pressed cancel.");
    }
    else
    {
        printf("Error: %s\n", NFD_GetError());
    }

    return outPath;
#else
    (void)filter;
    baseui_error("NativeFileDialog not supported!\n");
    return NULL;
#endif
}

char *baseui_filechhoser_save_file(char *last_file, const char *filter)
{
    (void)last_file;

#ifdef USE_NFDIALOG
    nfdchar_t *outPath = NULL;
    nfdresult_t result = NFD_SaveDialog(filter, NULL, &outPath);
    if (result == NFD_OKAY)
    {
        puts("Success!");
        puts(outPath);
        // free(outPath);
    }
    else if (result == NFD_CANCEL)
    {
        puts("User pressed cancel.");
    }
    else
    {
        printf("Error: %s\n", NFD_GetError());
    }

    return outPath;
#else
    (void)filter;
    baseui_error("NativeFileDialog not supported!\n");
    return NULL;
#endif
}

char *baseui_filechhoser_open_directory(char *last_directory)
{
    (void)last_directory;
#ifdef USE_NFDIALOG
    nfdchar_t *outPath = NULL;
    nfdresult_t result = NFD_PickFolder(NULL, &outPath);
    if (result == NFD_OKAY)
    {
        puts("Success!");
        puts(outPath);
        // free(outPath);
    }
    else if (result == NFD_CANCEL)
    {
        puts("User pressed cancel.");
    }
    else
    {
        printf("Error: %s\n", NFD_GetError());
    }
    return outPath;
#else
    baseui_error("NativeFileDialog not supported!\n");
    return NULL;
#endif
}
#endif