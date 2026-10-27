/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file kp80_devbus.h
 * @brief Lepidlo devbus 80-Zeichen-Karte K&P pro MZ-700 - veřejné API.
 *
 * Připojuje čisté jádro karty (kp80_core.h) na sběrnici volitelných
 * zařízení devbus: porty 70h - 73h, čas (takty GDG přepočtené na znakový
 * takt 2 MHz), volitelný WAIT na 71h - 73h, sekundární video výstup
 * "kp80" (druhý monitor 640 x 250 bodů), reset a zapnutí, volby, snapshot,
 * regiony debuggeru a katalog I/O. Obraz MZ-700 karta nemění (deskriptor
 * nemá video háčky).
 *
 * Výstup se kreslí v konci snímku (frame_done), jen když se od posledního
 * kreslení změnil stav karty nebo fáze blikání kurzoru. V pauze (zásah
 * debuggeru nebo MCP do portů či VRAM) se výstup překreslí hned.
 *
 * Model a rozhodnutí: `devdoc/kp80.md`.
 *
 * Vlákna: všechny funkce kromě kp80_devbus_vout() volá emu vlákno.
 */

#ifndef KP80_DEVBUS_H
#define KP80_DEVBUS_H

#include "hw-generic/devbus/devbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Id zařízení v devbus (klíč snapshotu, MCP, INI menu); neměnit. */
#define KP80_DEVBUS_ID "kp80"

/** @brief Id sekundárního video výstupu karty (MCP `device_screenshot`, okno GUI). */
#define KP80_DEVBUS_VOUT_ID "kp80"

/** @brief Nejvyšší hodnota volby WAIT v T-stavech. */
#define KP80_DEVBUS_WAIT_MAX 16u

/** @brief Nejvyšší hodnota volby délky update cyklu ve znakových taktech. */
#define KP80_DEVBUS_UPDATE_DELAY_MAX 255u

/**
 * @brief Indexy voleb zařízení (pořadí v deskriptoru, devbus_option_get()).
 */
typedef enum en_KP80_DEVBUS_OPT {
    KP80_OPT_READ_AUTOINC = 0, /**< BOOL `read_autoinc`: IN 70h zvýší update adresu; výchozí 1. */
    KP80_OPT_DATA_WRITE_TO_REG,/**< BOOL `data_to_reg`: OUT 71h zapíše i vybraný registr; výchozí 1. */
    KP80_OPT_ADDR_ON_R31,      /**< BOOL `addr_on_r31`: R18/R19 platí až po přístupu k R31; výchozí 1. */
    KP80_OPT_UPDATE_DELAY,     /**< UNSIGNED `update_delay`: délka update cyklu ve znakových taktech; výchozí 0. */
    KP80_OPT_WAIT,             /**< UNSIGNED `wait`: WAIT na 71h - 73h v T-stavech; výchozí 0. */
    KP80_OPT_PHOSPHOR,         /**< KEYWORD `phosphor`: barva fosforu druhého monitoru; výchozí bílá. */
    KP80_OPT_COUNT             /**< Počet voleb (není volba). */
} en_KP80_DEVBUS_OPT;

/**
 * @brief Barva fosforu druhého monitoru (hodnota volby `phosphor`).
 *
 * Hodnota + 1 je zároveň index svítícího bodu v paletě výstupu (0 = tma).
 */
typedef enum en_KP80_PHOSPHOR {
    KP80_PHOSPHOR_WHITE = 0, /**< Bílý fosfor (výchozí). */
    KP80_PHOSPHOR_GREEN,     /**< Zelený fosfor. */
    KP80_PHOSPHOR_AMBER,     /**< Jantarový fosfor. */
    KP80_PHOSPHOR_COUNT      /**< Počet barev (není barva). */
} en_KP80_PHOSPHOR;

/**
 * @brief Zaregistruje kartu K&P v devbus.
 *
 * Volá adaptér MZ-700 v mz700_devbus_start() před devbus_start(). Karta
 * zůstává odpojená, dokud ji nepřipojí INI (`[KP80] connected = 1`), menu,
 * MCP (`periph_attach kind=kp80`) nebo snapshot.
 *
 * @pre Emu vlákno při startu, devbus inicializovaný a ještě nespuštěný.
 * @post Při úspěchu je karta ve slotu devbus_find(KP80_DEVBUS_ID) a má
 *       výstup KP80_DEVBUS_VOUT_ID; při chybě registrace (plný registr,
 *       chybný deskriptor) se jen vypíše varování a karta v emulátoru není.
 */
void kp80_devbus_register(void);

/**
 * @brief Sekundární video výstup karty.
 *
 * @return Výstup, nebo NULL, pokud karta není registrovaná.
 * @note Ukazatel platí od registrace do devbus_exit(); smí ho číst i UI
 *       vlákno (výstup sám je bez zámku, viz devbus_vout_fetch()).
 */
st_DEVBUS_VOUT *kp80_devbus_vout(void);

#ifdef __cplusplus
}
#endif

#endif /* KP80_DEVBUS_H */
