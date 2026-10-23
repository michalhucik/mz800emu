#ifndef BASEUI_H
#define BASEUI_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

void baseui_show_message(bool is_error, char *format, ...);
void baseui_show_error_message(char *format, ...);

// TODO: Tohle makro by melo postupne nahradit vsechny fprintf(stderr, ...) volani
#define baseui_error(format, ...) \
    baseui_show_error_message("%s():%d ERROR - " format, __func__, __LINE__, ##__VA_ARGS__)

/**
 * @brief Kontrola velikosti souboru MZF proti délce z hlavičky.
 *
 * Když je soubor větší, než udává hlavička (hlavička + tělo), otevře
 * popup MZF Size Warning (imgui_cmt_fix_mzfsize_popup_activate()).
 * Soubor, který nejde otevřít nebo změřit, kontrolu přeskočí (výpis na
 * stderr), popup se neotevře.
 *
 * @param filename Cesta k souboru MZF (UTF-8).
 * @param valid_size Očekávaná velikost souboru podle hlavičky [B].
 * @note Volá ji jádro kazety (otevření pásky, CMT hack), i z emu vlákna;
 *       popup má vlastní zámek (cmt_fix_mzfsize.cpp).
 */
void baseui_cmt_check_mzf_filesize(const char *filename, int valid_size);

//char *baseui_filechhoser_open_file(char *last_file, const char *filter);
// char *baseui_filechhoser_save_file(char *last_file, const char *filter);
// char *baseui_filechhoser_open_directory(char *last_directory);

#include "baseui_tools.h"

#endif