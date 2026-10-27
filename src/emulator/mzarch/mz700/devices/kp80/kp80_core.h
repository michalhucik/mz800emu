/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file kp80_core.h
 * @brief Čisté jádro 80-Zeichen-Karte K&P (Kersten & Partner) pro MZ-700 - veřejné API.
 *
 * Model karty: CRTC Synertek SY6845E v transparentním režimu (R8 = 78h),
 * vlastní VRAM 2 048 B (SRAM 6116), znakový generátor EPROM 2716 (font
 * `80Z.ROM`, 128 znaků po 16 B) a vykreslení snímku druhého monitoru
 * 640 x 250 bodů. Obraz MZ-700 karta nemění.
 *
 * Jádro nezávisí na emulátoru (jen `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`),
 * testuje se jako STANDALONE. Lepidlo devbus (porty 70h - 73h, WAIT, čas,
 * sekundární video výstup, snapshot, volby) je mimo tento modul.
 *
 * Čas: všechny funkce s parametrem `now` čekají absolutní čas ve **znakových
 * taktech karty** (2 MHz, @ref KP80_CHAR_CLOCK_HZ). Čas musí být neklesající;
 * hodnota menší než poslední použitá se bere jako "žádný posun". Přepočet
 * z taktů emulátoru dělá lepidlo.
 *
 * Model, rozhodnutí a zjednodušení: `devdoc/kp80.md`. Fakta o kartě: báze
 * mz800-knowledge `public/reference/agent/hw/29-mz700-kp-80-zeichen.md`,
 * datasheet SY6545-1 (Synertek, Figure 3 Internal Register Summary).
 *
 * Vlákna: jádro nemá vlastní synchronizaci; všechny funkce volá jedno vlákno
 * (emulační), nebo volající zajistí vzájemné vyloučení.
 */

#ifndef KP80_CORE_H
#define KP80_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Velikost VRAM karty v bajtech (SRAM 6116); adresa CRTC se bere modulo tato velikost. */
#define KP80_VRAM_SIZE 2048

/** @brief Velikost znakového generátoru v bajtech (EPROM 2716, 128 znaků po 16 B). */
#define KP80_FONT_SIZE 2048

/** @brief Počet bajtů jednoho znaku ve fontu (řádky 0 - 15, adresované RA0 - RA3). */
#define KP80_FONT_CHAR_BYTES 16

/** @brief Šířka znakové buňky v bodech (posuvný registr 74LS166). */
#define KP80_CHAR_WIDTH 8

/** @brief Šířka výstupního bufferu snímku v bodech (80 znaků po 8 bodech). */
#define KP80_FB_WIDTH 640

/** @brief Výška výstupního bufferu snímku v bodech (25 řádků po 10 bodech). */
#define KP80_FB_HEIGHT 250

/** @brief Velikost výstupního bufferu snímku v bajtech (1 bajt na bod). */
#define KP80_FB_SIZE (KP80_FB_WIDTH * KP80_FB_HEIGHT)

/** @brief Znakový takt karty v Hz (krystal 16 MHz / 8, schéma Weidenbrück: "phi2 = 2 MHz"). */
#define KP80_CHAR_CLOCK_HZ 2000000u

/** @brief Počet modelovaných registrů CRTC R0 - R19 (R20 - R30 neexistují, R31 nic neukládá). */
#define KP80_NUM_REGS 20

/** @brief Číslo pomocného registru R31 (Dummy Location) transparentního režimu. */
#define KP80_REG_DUMMY 31

/**
 * @name Offsety portů karty (port AND 3)
 * Karta leží na 70h - 73h; jádro z čísla portu používá jen dolní dva bity.
 * @{
 */
#define KP80_PORT_VRAM   0 /**< 70h: IN bajt z VRAM; OUT bez účinku. */
#define KP80_PORT_DATA   1 /**< 71h: OUT bajt do VRAM (a volbou i do vybraného registru), IN vybraný registr. */
#define KP80_PORT_ADDR   2 /**< 72h: OUT výběr registru (5 bitů), IN status. */
#define KP80_PORT_REG    3 /**< 73h: IN/OUT vybraný registr. */
/** @} */

/**
 * @name Bity status registru (IN 72h)
 * Ostatní bity se čtou jako 0 (datasheet SY6545-1, Figure 3).
 * @{
 */
#define KP80_STATUS_UPDATE_READY 0x80u /**< Bit 7: update cyklus dokončen (model: viz devdoc). */
#define KP80_STATUS_LPEN_FULL    0x40u /**< Bit 6: LPEN registr plný; karta světelné pero nemá, vždy 0. */
#define KP80_STATUS_VBLANK       0x20u /**< Bit 5: paprsek je ve vertikálním zatemnění. */
/** @} */

/** @brief Verze binárního formátu stavu (kp80_state_save()); zvýšit při každé změně formátu. */
#define KP80_STATE_VERSION 1u

/**
 * @brief Velikost uloženého stavu v bajtech (verze @ref KP80_STATE_VERSION).
 *
 * Hlavička 8 B ("KP80", verze u16, velikost u16), registry 20 B, AR 1 B,
 * příznaky 1 B, update adresa 2 B, čas update ready 8 B, časování 8 + 4 + 8 B,
 * VRAM 2 048 B.
 */
#define KP80_STATE_SIZE (8u + KP80_NUM_REGS + 1u + 1u + 2u + 8u + 8u + 4u + 8u + KP80_VRAM_SIZE)

/**
 * @brief Volby modelu pro sporné chování karty (tabulka rozporů báze 29).
 *
 * Volby nejsou součástí uloženého stavu: patří do konfigurace zařízení
 * (devbus je ukládá do snapshotu jménem). Lze je měnit kdykoliv, účinek je
 * od příštího přístupu.
 */
typedef struct st_KP80_OPTIONS {
    bool read_autoinc;       /**< IN 70h po čtení zvýší update adresu (datasheet "Whenever a read/update occurs"); výchozí true. */
    bool data_write_to_reg;  /**< OUT 71h zapíše bajt i do vybraného registru CRTC (interpretace schématu); výchozí true. */
    bool addr_on_r31;        /**< Zápis R18/R19 platí až po přístupu k R31 (true, datasheet a BKK), jinak ihned; výchozí true. */
    uint32_t update_delay;   /**< Doba update cyklu ve znakových taktech: po spuštění je status bit 7 = 0 tak dlouho; výchozí 0. */
} st_KP80_OPTIONS;

/**
 * @brief Úplný stav karty K&P 80 (pevná pole) + volby a font.
 *
 * Invarianty (udržuje je jádro, kp80_state_load() je vynutí):
 *  - `reg[i]` je maskovaný šířkou registru podle datasheetu (R4 7 bitů,
 *    R5 5 bitů, R12 6 bitů ...); `reg[16]`, `reg[17]` (LPEN) jsou 0;
 *  - `ar` < 32;
 *  - `upd_addr` < 4000h (14 bitů);
 *  - `tm_phase` < kp80_frame_period() pro aktuální registry.
 *
 * Vlastnictví: struktura patří volajícímu (lepidlu), jádro nic nealokuje.
 * `font` ukazuje na paměť volajícího nebo na @ref g_kp80_font_rom a musí žít
 * po celou dobu použití struktury.
 */
typedef struct st_KP80 {
    uint8_t reg[KP80_NUM_REGS];  /**< Registry R0 - R19; R18/R19 = naposledy zapsaná (latchovaná) update adresa. */
    uint8_t ar;                  /**< Adresní registr (vybraný registr), 5 bitů. */
    bool upd_pending;            /**< R18/R19 zapsány, nová adresa čeká na přístup k R31 (jen addr_on_r31). */
    uint16_t upd_addr;           /**< Aktivní update adresa (14 bitů); VRAM se adresuje upd_addr AND 7FFh. */
    uint64_t upd_ready_at;       /**< Čas (znakové takty), od kterého je status bit 7 = 1. */
    uint64_t tm_last;            /**< Čas posledního přepočtu časování (znakové takty). */
    uint32_t tm_phase;           /**< Pozice paprsku ve snímku v čase tm_last (znakové takty od začátku snímku). */
    uint64_t tm_fields;          /**< Počet celých snímků od zapnutí / resetu do tm_last (pro blikání kurzoru). */
    uint8_t vram[KP80_VRAM_SIZE];/**< VRAM karty (jen kódy znaků, bez atributů). */
    st_KP80_OPTIONS opt;         /**< Volby modelu; nejsou v uloženém stavu. */
    const uint8_t *font;         /**< Znakový generátor KP80_FONT_SIZE B; není v uloženém stavu; NULL = vestavěný. */
} st_KP80;

/**
 * @brief Zakompilovaný font karty (`80Z.ROM`, MD5 b6bc43cd4127eff1f511123fbf98d1ed).
 *
 * Adresa = (kód AND 7Fh) * 16 + řádek znaku, bit 7 = levý bod. Pole je
 * neměnné po celou dobu běhu programu. Definice v kp80_font_rom.c.
 */
extern const uint8_t g_kp80_font_rom[KP80_FONT_SIZE];

/**
 * @brief Naplní volby výchozími hodnotami (plán D4: autoinkrement čtení ano,
 *        zápis 71h do registru ano, R18/R19 po R31, update_delay 0).
 * @param opt Cílová struktura; nesmí být NULL.
 */
void kp80_options_default(st_KP80_OPTIONS *opt);

/**
 * @brief Inicializuje celou strukturu: výchozí volby, vestavěný font a stav po zapnutí.
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech.
 * @post Stav jako po kp80_power_on(), `opt` výchozí, `font` = @ref g_kp80_font_rom.
 */
void kp80_init(st_KP80 *kp, uint64_t now);

/**
 * @brief Stav po zapnutí napájení: registry 0, VRAM 00h (zobrazí "@"), AR 0,
 *        update adresa 0, update ready = 1, časování od začátku snímku.
 *
 * Skutečný 6845 má po zapnutí registry nedefinované; nuly jsou volba modelu
 * (plán D4). Volby a font zůstávají.
 *
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech.
 */
void kp80_power_on(st_KP80 *kp, uint64_t now);

/**
 * @brief Reset sběrnice MZ-700 (tlačítko RESET).
 *
 * Volba modelu (báze 29, otázka 7 - zapojení RES CRTC neznámé): registry,
 * AR, update adresa a VRAM se **nemění**; restartují se jen čítače rozkladu
 * (pozice ve snímku a počítadlo snímků) a případný probíhající update cyklus
 * se považuje za dokončený. Odpovídá datasheetu SY6545-1 (RES: "internal
 * counters are stopped and cleared ... control registers are unaffected")
 * za předpokladu, že RES je spojen s resetem sběrnice. Zdůvodnění v devdoc.
 *
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech.
 */
void kp80_reset(st_KP80 *kp, uint64_t now);

/**
 * @brief Nastaví znakový generátor.
 * @param kp   Struktura karty; nesmí být NULL.
 * @param font Ukazatel na KP80_FONT_SIZE bajtů, nebo NULL = vestavěný @ref g_kp80_font_rom.
 *             Paměť zůstává ve vlastnictví volajícího a musí žít, dokud ji karta používá.
 */
void kp80_set_font(st_KP80 *kp, const uint8_t *font);

/**
 * @brief Čtení portu karty CPU (IN), s vedlejšími účinky.
 *
 * - 70h: bajt VRAM na update adrese; s `read_autoinc` adresa + 1.
 * - 71h, 73h: vybraný registr: R14 - R17 hodnota, R31 přístup k R31
 *   (potvrdí adresu, spustí update cyklus) a vrací @p bus (R31 nebudí
 *   sběrnici); ostatní registry jsou jen pro zápis nebo neexistují, vrací 0
 *   (volba modelu, viz devdoc).
 * - 72h: status (@ref KP80_STATUS_UPDATE_READY, @ref KP80_STATUS_VBLANK).
 *
 * @param kp   Struktura karty; nesmí být NULL.
 * @param port Číslo portu; použijí se jen bity 0 - 1.
 * @param now  Aktuální čas ve znakových taktech.
 * @param bus  Hodnota nebuzené datové sběrnice (lepidlo předá latch sběrnice).
 * @return Přečtený bajt.
 */
uint8_t kp80_read(st_KP80 *kp, unsigned port, uint64_t now, uint8_t bus);

/**
 * @brief Čtení portu bez vedlejších účinků (debugger, I/O katalog).
 *
 * Vrací totéž co kp80_read() ve stejném okamžiku, ale nezvýší update adresu,
 * nepotvrdí R18/R19 a nespustí update cyklus.
 *
 * @param kp   Struktura karty; nesmí být NULL.
 * @param port Číslo portu; použijí se jen bity 0 - 1.
 * @param now  Aktuální čas ve znakových taktech.
 * @param bus  Hodnota nebuzené datové sběrnice.
 * @return Bajt, který by vrátilo čtení.
 */
uint8_t kp80_peek(const st_KP80 *kp, unsigned port, uint64_t now, uint8_t bus);

/**
 * @brief Zápis na port karty CPU (OUT).
 *
 * - 70h: bez účinku.
 * - 71h: s `data_write_to_reg` nejdřív zápis do vybraného registru (je-li
 *   vybrán R31, jde o přístup k R31); pak bajt do VRAM na update adresu,
 *   adresa + 1 (14 bitů) a spuštění update cyklu.
 * - 72h: AR = hodnota AND 1Fh; výběr R31 je přístup k R31 (postup BKK).
 * - 73h: zápis do vybraného registru.
 *
 * @param kp    Struktura karty; nesmí být NULL.
 * @param port  Číslo portu; použijí se jen bity 0 - 1.
 * @param value Zapisovaný bajt.
 * @param now   Aktuální čas ve znakových taktech.
 */
void kp80_write(st_KP80 *kp, unsigned port, uint8_t value, uint64_t now);

/**
 * @brief Zjistí, zda přístup na port karty aktivuje WAIT CPU.
 *
 * Weidenbrück: signál "6845" (porty 71h - 73h) krátce pozdrží CPU přes WAIT.
 * Délka v T-stavech není známá, určuje ji lepidlo (volba, výchozí 0).
 *
 * @param port Číslo portu; použijí se jen bity 0 - 1.
 * @return true pro 71h - 73h, false pro 70h.
 */
bool kp80_port_has_wait(unsigned port);

/**
 * @brief Hodnota status registru v čase @p now (jako IN 72h, bez vedlejších účinků).
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech.
 * @return Bity @ref KP80_STATUS_UPDATE_READY a @ref KP80_STATUS_VBLANK, ostatní 0.
 */
uint8_t kp80_status(const st_KP80 *kp, uint64_t now);

/**
 * @brief Délka rozkladového řádku: R0 + 1 znakových taktů (1 - 256).
 * @param kp Struktura karty; nesmí být NULL.
 * @return Perioda řádku ve znakových taktech.
 */
uint32_t kp80_line_period(const st_KP80 *kp);

/**
 * @brief Počet rozkladových řádků snímku: (R4 + 1) * (R9 + 1) + R5.
 * @param kp Struktura karty; nesmí být NULL.
 * @return Počet řádků snímku (1 - 4 127).
 */
uint32_t kp80_frame_lines(const st_KP80 *kp);

/**
 * @brief Délka snímku ve znakových taktech: kp80_line_period() * kp80_frame_lines().
 * @param kp Struktura karty; nesmí být NULL.
 * @return Perioda snímku (s výchozími registry BKK 128 * 312 = 39 936, tj. 50,08 Hz).
 */
uint32_t kp80_frame_period(const st_KP80 *kp);

/**
 * @brief Je paprsek v čase @p now ve vertikálním zatemnění?
 *
 * Status bit 5 přechází do 1 na konci posledního zobrazeného rozkladového
 * řádku (R6 * (R9 + 1)) a do 0 na začátku snímku (datasheet Figure 11).
 *
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech.
 * @return true = V-blank.
 */
bool kp80_vblank(const st_KP80 *kp, uint64_t now);

/**
 * @brief Počet celých snímků od zapnutí / resetu do času @p now.
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech.
 * @return Počítadlo snímků (řídí blikání kurzoru).
 */
uint64_t kp80_field_count(const st_KP80 *kp, uint64_t now);

/**
 * @brief Je kurzor v čase @p now v rozsvícené fázi blikání?
 *
 * R10 bity 6 - 5: 00 svítí, 01 bez kurzoru, 10 bliká 1/16 snímkové
 * frekvence, 11 bliká 1/32 (datasheet SY6545-1). První polovina periody
 * blikání svítí (volba modelu).
 *
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech.
 * @return true = kurzor se v tomto snímku kreslí.
 */
bool kp80_cursor_visible(const st_KP80 *kp, uint64_t now);

/**
 * @brief Vykreslí snímek druhého monitoru do monochromatického bufferu.
 *
 * Buffer @ref KP80_FB_WIDTH x @ref KP80_FB_HEIGHT, 1 bajt na bod, hodnota
 * 0 (tma) nebo 1 (svit), po řádcích shora. Kreslí se R6 znakových řádků po
 * R9 + 1 rozkladových řádcích a R1 znaků po 8 bodech od adresy R12/R13
 * (přímé binární adresování); co se do bufferu nevejde, se ořízne, zbytek
 * bufferu je 0. Bod = bit (7 - x) bajtu fontu `(kód AND 7Fh) * 16 +
 * (řádek AND 0Fh)`, XOR bit 7 kódu (inverze), XOR kurzor (R14/R15,
 * řádky R10 bity 0 - 4 až R11, blikání podle kp80_cursor_visible()).
 *
 * @param kp  Struktura karty; nesmí být NULL.
 * @param now Aktuální čas ve znakových taktech (fáze blikání kurzoru).
 * @param fb  Cílový buffer @ref KP80_FB_SIZE bajtů; nesmí být NULL. Celý se přepíše.
 */
void kp80_render(const st_KP80 *kp, uint64_t now, uint8_t *fb);

/**
 * @brief Uloží stav karty do bajtového bufferu (little-endian, s hlavičkou a verzí).
 *
 * Ukládá registry, AR, update adresu a stav, časování a VRAM; volby a font ne.
 * Časy jsou absolutní ve znakových taktech, platí tedy jen spolu s časem
 * emulátoru ze stejného snapshotu.
 *
 * @param kp   Struktura karty; nesmí být NULL.
 * @param buf  Cílový buffer.
 * @param size Velikost bufferu v bajtech.
 * @return Počet zapsaných bajtů (@ref KP80_STATE_SIZE), nebo 0, je-li buffer
 *         NULL nebo menší; pak se nezapíše nic.
 */
size_t kp80_state_save(const st_KP80 *kp, uint8_t *buf, size_t size);

/**
 * @brief Načte stav uložený kp80_state_save().
 *
 * Ověří hlavičku ("KP80"), verzi (1 - @ref KP80_STATE_VERSION) a velikost;
 * načtené hodnoty zamaskuje podle invariantů. Volby a font se nemění.
 *
 * @param kp   Struktura karty; nesmí být NULL.
 * @param buf  Zdrojový buffer.
 * @param size Velikost dat v bajtech.
 * @return true = načteno; false = neplatná data, stav karty zůstal beze změny.
 */
bool kp80_state_load(st_KP80 *kp, const uint8_t *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* KP80_CORE_H */
