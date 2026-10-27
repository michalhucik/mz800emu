/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file suc80_devbus.h
 * @brief Zařízení devbus "suc80": 80sloupcový kit Sharp Users Club UK pro MZ-700.
 *
 * Lepidlo mezi čistým jádrem modelu (suc80_core.h) a sběrnicí volitelných
 * zařízení devbus (hw-generic/devbus/devbus.h): deskriptor, MMIO latch
 * 0E00Ch - 0E00Fh, kreslení řádku plátna (render_row), reset, volba
 * granularity atributů, snapshot a položka katalogu I/O.
 *
 * Kompiluje se jen do exe MZ-700 (PAL i NTSC). Popis: devdoc/suc80.md.
 */

#ifndef SUC80_DEVBUS_H
#define SUC80_DEVBUS_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Id zařízení v devbus (klíč snapshotu a MCP; neměnit). */
#define SUC80_DEVBUS_ID          "suc80"

/** @brief Sekce INI zařízení. */
#define SUC80_DEVBUS_INI_SECTION "SUC80"

/**
 * @brief Index volby granularity atributů v deskriptoru (klíč INI "attributes").
 *
 * Typ KEYWORD, hodnoty "per_char" (index 0, en_SUC80_ATTR_MODE
 * SUC80_ATTR_PER_CHAR, výchozí) a "per_pair" (index 1, SUC80_ATTR_PER_PAIR).
 */
#define SUC80_DEVBUS_OPT_ATTRIBUTES 0

/**
 * @brief Zaregistruje zařízení SUC do devbus.
 *
 * Volá ji adaptér MZ-700 (mz700_devbus_start()) před devbus_start().
 * Zařízení je po registraci odpojené; připojí ho INI (`[SUC80] connected`),
 * menu HW & Devices, MCP periph_attach nebo snapshot.
 *
 * @pre devbus_init() proběhl (adaptér MZ-700), devbus_start() ještě ne.
 * @post Při úspěchu je zařízení v registru devbus pod id SUC80_DEVBUS_ID.
 *       Chybu registrace (neplatný deskriptor, plný registr) zapíše do logu
 *       a zařízení pak v registru není; emulace běží dál bez něj.
 */
void suc80_devbus_register(void);

#ifdef __cplusplus
}
#endif

#endif /* SUC80_DEVBUS_H */
