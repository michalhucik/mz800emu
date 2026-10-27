/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file   suc80_ui_strings.cpp
 * @brief  Řetězce deskriptoru zařízení SUC 80 sloupců pro katalog překladů.
 *
 * Jádro emulátoru (src/emulator/) lokalizační makra nemá, proto název
 * zařízení, popisek volby a popisky jejích hodnot (keyword_labels)
 * z deskriptoru (suc80_devbus.c) značí N_() tady,
 * aby je xgettext dostal do .pot. Za běhu je překládá generické menu
 * zařízení (menu_devbus.cpp, _L()). Texty musí být shodné s deskriptorem.
 *
 * @par Licence: GPLv3
 */

#include "i18n.h"

/** @brief Msgid z deskriptoru suc80 (jen pro xgettext, za běhu se nečte). */
__attribute__((unused)) static const char *const s_suc80_msgids[] = {
    N_("SUC 80-column kit"),
    N_("Attributes in 80 columns"),
    N_("Per character"),
    N_("Per pair of characters"),
};
