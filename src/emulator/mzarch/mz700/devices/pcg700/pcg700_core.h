/**
 * @file   pcg700_core.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Čisté jádro modelu PCG-700 (HAL Laboratory) pro Sharp MZ-700.
 *
 * PCG-700 je externí programovatelný generátor znaků. Jeho 2 kB RAM nahrazuje
 * glyfy kódů 80h - 0FFh obou znakových sad CG-ROM; kódy 00h - 7Fh jsou vždy
 * z CG-ROM. Řídí se třemi zápisovými registry v paměťovém prostoru:
 *
 * | Registr | Adresa | Funkce                                              |
 * |---------|--------|-----------------------------------------------------|
 * | 0       | 0E010h | datový bajt vzoru, bit 7 = levý bod                 |
 * | 1       | 0E011h | adresa v PCG RAM, bity 7 - 0                        |
 * | 2       | 0E012h | řízení: bity 1 - 0 adresa 9 - 8, bit 2 sada (adresa |
 * |         |        | bit 10), bit 3 vypnutí, bit 4 strobe, bit 5 COPY    |
 *
 * Jádro nezávisí na emulátoru (jen `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`,
 * v implementaci `<string.h>`),
 * neví nic o dekódu adres, bankování ani o vykreslování. Napojení na device
 * API (MMIO háček, zdroj glyfů, snapshot, volby) dělá lepidlo `pcg700_devbus.c`.
 *
 * Formát RAM i efektivního generátoru znaků je formát CG-ROM emulátoru:
 * index `sada * 800h + kód * 8 + řádek`, levý bod v bitu 0 (viz devdoc/pcg700.md).
 * Bajt zapsaný přes 0E010h se proto při uložení do RAM bitově převrací, bajt
 * kopírovaný z CG-ROM (COPY) ne.
 *
 * Zdroje: báze mz800-knowledge `public/reference/agent/hw/27-mz700-pcg-700.md`,
 * překlad manuálu HAL (`pcg700_03`, `pcg700_05`), rozhodnutí v devdoc/pcg700.md.
 * Reálný HW nebyl měřen; místa, kde se prameny rozcházejí, jsou volbami.
 *
 * @par Vlákna:
 * Jádro nemá žádný globální stav ani zámky. Všechny funkce pracují jen
 * s předanou instancí; synchronizaci (emu vlákno vs. UI) zajišťuje volající.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PCG700_CORE_H
#define PCG700_CORE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


/** @brief Velikost PCG RAM v bajtech (2 sady x 128 znaků x 8 řádků). */
#define PCG700_RAM_SIZE         0x0800

/** @brief Velikost CG-ROM a efektivního generátoru znaků (2 sady x 256 znaků x 8 řádků). */
#define PCG700_CGROM_SIZE       0x1000

/** @brief Počet zápisových registrů (0E010h - 0E012h). */
#define PCG700_REG_COUNT        3

/** @brief Index registru dat (0E010h). */
#define PCG700_REG_DATA         0
/** @brief Index registru dolní adresy (0E011h). */
#define PCG700_REG_ADDR_LO      1
/** @brief Index řídicího registru (0E012h). */
#define PCG700_REG_CTRL         2

/** @brief Řídicí registr: bity 1 - 0 = adresa PCG RAM bity 9 - 8. */
#define PCG700_CTRL_ADDR_HI     0x03
/** @brief Řídicí registr: bit 2 = znaková sada (0 = sada 1, 1 = sada 2), adresa bit 10. */
#define PCG700_CTRL_SET         0x04
/** @brief Řídicí registr: bit 3 = vypnutí PCG (1 = vypnuto; platí jen se SSW = ON). */
#define PCG700_CTRL_OFF         0x08
/** @brief Řídicí registr: bit 4 = strobe zápisu (1 = data platná). */
#define PCG700_CTRL_STROBE      0x10
/** @brief Řídicí registr: bit 5 = zdroj zápisu (0 = bajt z 0E010h, 1 = kopie z CG-ROM). */
#define PCG700_CTRL_COPY        0x20

/**
 * @brief Hodnota řídicího registru po zapnutí a po RESET.
 *
 * Bit 3 = 1 (PCG vypnuto), ostatní bity 0. Shodují se na tom všechny známé
 * implementace (báze 27, tabulka rozporů); manuál stav neuvádí.
 */
#define PCG700_CTRL_RESET_VALUE PCG700_CTRL_OFF


/**
 * @brief Události hlášené funkcemi, které mění stav (bitová maska).
 *
 * Lepidlo podle nich ví, kdy přepnout zdroj glyfů a invalidovat obraz
 * (viz pcg700_core_event_needs_redraw()).
 */
typedef enum en_PCG700_EVENT
{
    PCG700_EVT_NONE = 0,                  /**< Nic se nestalo. */
    PCG700_EVT_RAM_WRITTEN = ( 1u << 0 ), /**< Proběhl zápis do PCG RAM (i se stejnou hodnotou). */
    PCG700_EVT_GLYPH_CHANGED = ( 1u << 1 ), /**< Obsah efektivního generátoru znaků se změnil. */
    PCG700_EVT_DISPLAY_CHANGED = ( 1u << 2 ), /**< Změnila se hodnota pcg700_core_display_enabled(). */
} en_PCG700_EVENT;


/**
 * @brief Okamžik zápisu do PCG RAM vzhledem ke strobe (bit 4 v 0E012h).
 *
 * Prameny se rozcházejí (báze 27, tabulka rozporů), na HW neměřeno.
 */
typedef enum en_PCG700_STROBE
{
    /**
     * Nástupná hrana: zápis do 0E012h, který mění bit 4 z 0 na 1. Adresa
     * (bity 2 - 0 a 0E011h) a COPY se berou z téhož zápisu. Výchozí volba.
     */
    PCG700_STROBE_RISING = 0,
    /**
     * Úroveň: každý zápis do kteréhokoli registru, po kterém je bit 4 = 1,
     * zapíše bajt s aktuálními latchi (tedy i opakovaný zápis 0E012h s bitem 4
     * a zápisy 0E010h / 0E011h při trvale nastaveném bitu 4).
     */
    PCG700_STROBE_LEVEL,
    /**
     * Sestupná hrana: zápis do 0E012h, který mění bit 4 z 1 na 0. Adresa bity
     * 10 - 8 a COPY se berou z hodnoty 0E012h PŘED tímto zápisem (poslední stav
     * s nastaveným strobe), adresa bity 7 - 0 a data z aktuálních latchů
     * 0E011h a 0E010h. Experimentální volba.
     */
    PCG700_STROBE_FALLING,
    PCG700_STROBE_COUNT /**< Počet hodnot (hranice pro validaci). */
} en_PCG700_STROBE;


/** @brief Přepínač HSW na desce PCG-700 (manuál `pcg700_03`). */
typedef enum en_PCG700_HSW
{
    PCG700_HSW_NOM = 0, /**< NOM: PCG vypnuto, kreslí se z CG-ROM. */
    PCG700_HSW_PCG,     /**< PCG: PCG zapnuto (dál rozhoduje SSW a bit 3). */
    PCG700_HSW_COUNT    /**< Počet hodnot (hranice pro validaci). */
} en_PCG700_HSW;


/** @brief Jumper SSW (manuál `pcg700_03`): povolení softwarového řízení bitem 3. */
typedef enum en_PCG700_SSW
{
    PCG700_SSW_OFF = 0, /**< OFF: bit 3 v 0E012h zobrazení neovlivňuje (viz en_PCG700_SSWOFF). */
    PCG700_SSW_ON,      /**< ON: bit 3 = 0 PCG zapíná, bit 3 = 1 vypíná. */
    PCG700_SSW_COUNT    /**< Počet hodnot (hranice pro validaci). */
} en_PCG700_SSW;


/** @brief Jumper GSW (manuál `pcg700_03`): připojení registrů ke sběrnici. */
typedef enum en_PCG700_GSW
{
    PCG700_GSW_OFF = 0, /**< OFF: registry odpojené, všechny zápisy se ignorují. */
    PCG700_GSW_ON,      /**< ON: registry dekódují zápisy. */
    PCG700_GSW_COUNT    /**< Počet hodnot (hranice pro validaci). */
} en_PCG700_GSW;


/**
 * @brief Výklad stavu HSW = PCG a SSW = OFF.
 *
 * Manuál (`pcg700_03`) je nejednoznačný, viz devdoc/pcg700.md, oddíl Přepínače.
 */
typedef enum en_PCG700_SSWOFF
{
    PCG700_SSWOFF_HSW_DECIDES = 0, /**< Bit 3 se ignoruje, PCG je zapnuto (rozhoduje jen HSW). Výchozí. */
    PCG700_SSWOFF_FORCE_OFF,       /**< Software PCG nezapne, PCG je vypnuto. */
    PCG700_SSWOFF_COUNT            /**< Počet hodnot (hranice pro validaci). */
} en_PCG700_SSWOFF;


/** @brief Obsah PCG RAM po zapnutí napájení (na HW neověřeno, báze 27). */
typedef enum en_PCG700_POWERON_RAM
{
    PCG700_POWERON_RAM_ZERO = 0, /**< Samé nuly (deterministické). Výchozí. */
    PCG700_POWERON_RAM_CGROM,    /**< Kopie glyfů 80h - 0FFh obou sad z CG-ROM. */
    PCG700_POWERON_RAM_COUNT     /**< Počet hodnot (hranice pro validaci). */
} en_PCG700_POWERON_RAM;


/**
 * @brief Konfigurace zařízení (polohy přepínačů a volby rozporů).
 *
 * Do snapshotu stavu (pcg700_core_state_save()) nepatří; volby ukládá
 * lepidlo jako volby zařízení devbus.
 *
 * Invariant: každá položka je v rozsahu `0 .. <ENUM>_COUNT - 1`.
 */
typedef struct st_PCG700_CONFIG
{
    en_PCG700_STROBE strobe;           /**< Okamžik zápisu vzhledem ke strobe. */
    en_PCG700_HSW hsw;                 /**< Přepínač HSW. */
    en_PCG700_SSW ssw;                 /**< Jumper SSW. */
    en_PCG700_GSW gsw;                 /**< Jumper GSW. */
    en_PCG700_SSWOFF sswoff;           /**< Výklad HSW = PCG se SSW = OFF. */
    en_PCG700_POWERON_RAM poweron_ram; /**< Obsah RAM po pcg700_core_power_on(). */
} st_PCG700_CONFIG;


/**
 * @brief Celý stav jedné instance PCG-700.
 *
 * Všechna pole mají pevnou velikost, struktura nic nevlastní a neobsahuje
 * ukazatele; lze ji kopírovat přes memcpy.
 *
 * Invarianty (platí po každé veřejné funkci):
 * - `effective` = CG-ROM předaná poslednímu volání pcg700_core_rebuild()
 *   (nebo pcg700_core_power_on() / pcg700_core_state_load()), v níž jsou
 *   bajty kódů 80h - 0FFh obou sad nahrazeny obsahem `ram`
 *   (`effective[pcg700_core_effective_index(a)] == ram[a]` pro každé `a`).
 * - `ram` i `effective` jsou ve formátu CG-ROM emulátoru (levý bod = bit 0).
 * - `cfg` splňuje invariant st_PCG700_CONFIG.
 */
typedef struct st_PCG700
{
    st_PCG700_CONFIG cfg;                 /**< Přepínače a volby. */
    uint8_t data;                         /**< Latch 0E010h (bajt tak, jak ho zapsal CPU). */
    uint8_t addr_lo;                      /**< Latch 0E011h. */
    uint8_t ctrl;                         /**< Latch 0E012h (všech 8 bitů, bity 6 - 7 bez významu). */
    uint8_t ram[PCG700_RAM_SIZE];         /**< PCG RAM ve formátu CG-ROM (levý bod = bit 0). */
    uint8_t effective[PCG700_CGROM_SIZE]; /**< Efektivní generátor znaků ve formátu CG-ROM. */
} st_PCG700;


/** @brief Verze binárního formátu stavu (pcg700_core_state_save()). */
#define PCG700_STATE_VERSION    1

/**
 * @brief Velikost binárního stavu v bajtech.
 *
 * Hlavička 8 B (magic "PCG7", verze u16 LE, délka u16 LE), latche 4 B
 * (data, addr_lo, ctrl, rezerva 0), RAM 2048 B.
 */
#define PCG700_STATE_SIZE       ( 8 + 4 + PCG700_RAM_SIZE )


/** @brief Chybové kódy funkcí vracejících int (0 = úspěch). */
typedef enum en_PCG700_RESULT
{
    PCG700_OK = 0,            /**< Úspěch. */
    PCG700_ERR_INVALID = -1,  /**< Chybný argument (NULL, malý buffer, hodnota mimo rozsah). */
    PCG700_ERR_FORMAT = -2,   /**< Binární stav nemá platnou hlavičku nebo délku. */
    PCG700_ERR_VERSION = -3,  /**< Binární stav má neznámou (novější) verzi. */
} en_PCG700_RESULT;


/**
 * @brief Naplní konfiguraci výchozími hodnotami.
 *
 * Výchozí: strobe na nástupnou hranu, HSW = PCG, SSW = ON, GSW = ON,
 * SSW OFF = rozhoduje HSW, RAM po zapnutí nulová. Polohy přepínačů odpovídají
 * nastavení, které manuál (`pcg700_05`) požaduje pro softwarové řízení.
 *
 * @param cfg Cílová konfigurace; nesmí být NULL.
 * @post `*cfg` splňuje invariant st_PCG700_CONFIG.
 */
extern void pcg700_core_config_default ( st_PCG700_CONFIG *cfg );


/**
 * @brief Ověří, že všechny položky konfigurace jsou v rozsahu.
 *
 * @param cfg Konfigurace; NULL vrací false.
 * @return true, pokud konfigurace splňuje invariant st_PCG700_CONFIG.
 */
extern bool pcg700_core_config_valid ( const st_PCG700_CONFIG *cfg );


/**
 * @brief Zapnutí napájení: konfigurace, latche, RAM a efektivní generátor.
 *
 * Latche nastaví jako pcg700_core_reset(), RAM podle `cfg->poweron_ram`
 * (nuly, nebo kopie bajtů CG-ROM kódů 80h - 0FFh obou sad) a efektivní
 * generátor přestaví z `cgrom` a RAM.
 *
 * @param pcg   Instance; nesmí být NULL. Předchozí obsah se celý přepíše.
 * @param cfg   Konfigurace; NULL = výchozí (pcg700_core_config_default()).
 * @param cgrom CG-ROM 4 kB ve formátu emulátoru; NULL = CG-ROM ze samých nul.
 *              Jádro si ukazatel neukládá.
 * @return PCG700_OK, nebo PCG700_ERR_INVALID (pcg NULL nebo neplatná cfg;
 *         instance pak zůstává beze změny).
 */
extern int pcg700_core_power_on ( st_PCG700 *pcg, const st_PCG700_CONFIG *cfg, const uint8_t *cgrom );


/**
 * @brief Tlačítko RESET: latche na výchozí hodnoty, RAM beze změny.
 *
 * data = 0, addr_lo = 0, ctrl = PCG700_CTRL_RESET_VALUE (PCG vypnuto bitem 3).
 * Reset sám nic nezapisuje do RAM (ani při volbě sestupné hrany).
 *
 * @param pcg Instance; nesmí být NULL.
 * @return Maska en_PCG700_EVENT; PCG700_EVT_DISPLAY_CHANGED, pokud reset
 *         vypnul zobrazení z PCG.
 */
extern unsigned pcg700_core_reset ( st_PCG700 *pcg );


/**
 * @brief Zápis CPU do registru PCG-700.
 *
 * Při GSW = OFF se zápis ignoruje (latche se nemění). Jinak se uloží do
 * latche `reg` a podle `cfg.strobe` se případně zapíše jeden bajt PCG RAM:
 * - COPY = 0: `ram[a] = pcg700_core_bitrev(data)`,
 * - COPY = 1: `ram[a] = cgrom[pcg700_core_effective_index(a)]` (sada podle
 *   bitu 2, bez převracení),
 * kde `a = ((ctrl & 07h) << 8) | addr_lo`. Zapsaný bajt se promítne
 * i do efektivního generátoru.
 *
 * @param pcg   Instance; nesmí být NULL.
 * @param reg   Index registru 0 - 2 (adresa - 0E010h); jiná hodnota se ignoruje.
 * @param value Zapisovaný bajt.
 * @param cgrom CG-ROM 4 kB pro COPY; NULL = kopíruje se 00h. Neukládá se.
 * @return Maska en_PCG700_EVENT (zápis do RAM, změna glyfu, změna zobrazení
 *         bitem 3).
 */
extern unsigned pcg700_core_write ( st_PCG700 *pcg, unsigned reg, uint8_t value, const uint8_t *cgrom );


/**
 * @brief Plné přestavění efektivního generátoru z CG-ROM a PCG RAM.
 *
 * Volá se po změně CG-ROM (`rom_changed`) a kdykoli je potřeba obnovit
 * invariant `effective`.
 *
 * @param pcg   Instance; nesmí být NULL.
 * @param cgrom CG-ROM 4 kB ve formátu emulátoru; NULL = samé nuly. Neukládá se.
 * @post `effective` splňuje invariant st_PCG700 vůči `cgrom`.
 */
extern void pcg700_core_rebuild ( st_PCG700 *pcg, const uint8_t *cgrom );


/**
 * @brief Má se kreslit z efektivního generátoru?
 *
 * - HSW = NOM: ne.
 * - HSW = PCG, SSW = ON: ano právě při bitu 3 v 0E012h = 0.
 * - HSW = PCG, SSW = OFF: podle `cfg.sswoff` (HSW_DECIDES = ano, FORCE_OFF = ne).
 *
 * @param pcg Instance; nesmí být NULL.
 * @return true = kreslit z `effective`, false = z CG-ROM.
 */
extern bool pcg700_core_display_enabled ( const st_PCG700 *pcg );


/**
 * @brief Vrátí generátor znaků, ze kterého se má právě kreslit.
 *
 * @param pcg   Instance; nesmí být NULL.
 * @param cgrom Aktuální CG-ROM emulátoru (vrací se při vypnutém PCG).
 * @return `pcg->effective` při pcg700_core_display_enabled(), jinak `cgrom`.
 *         Ukazatel na `effective` platí po dobu života instance.
 */
extern const uint8_t* pcg700_core_glyphs ( const st_PCG700 *pcg, const uint8_t *cgrom );


/**
 * @brief Je po dané události potřeba překreslit obraz?
 *
 * @param pcg    Instance (stav po události); nesmí být NULL.
 * @param events Maska en_PCG700_EVENT vrácená některou funkcí jádra.
 * @return true při změně zobrazení, nebo při změně glyfu se zapnutým PCG.
 */
extern bool pcg700_core_event_needs_redraw ( const st_PCG700 *pcg, unsigned events );


/**
 * @brief Změna konfigurace za běhu (přepínače, volby).
 *
 * RAM ani latche se nemění. `poweron_ram` se uplatní až při příštím
 * pcg700_core_power_on().
 *
 * @param pcg Instance; nesmí být NULL.
 * @param cfg Nová konfigurace; neplatná (nebo NULL) se odmítne beze změny stavu.
 * @return Maska en_PCG700_EVENT (PCG700_EVT_DISPLAY_CHANGED při přepnutí
 *         zobrazení); při odmítnutí PCG700_EVT_NONE.
 */
extern unsigned pcg700_core_set_config ( st_PCG700 *pcg, const st_PCG700_CONFIG *cfg );


/**
 * @brief Přečte bajt PCG RAM (debugger), ve formátu CG-ROM (levý bod = bit 0).
 *
 * @param pcg  Instance; nesmí být NULL.
 * @param addr Adresa v PCG RAM; použije se `addr & 7FFh`.
 * @return Bajt RAM.
 */
extern uint8_t pcg700_core_ram_peek ( const st_PCG700 *pcg, unsigned addr );


/**
 * @brief Zapíše bajt PCG RAM mimo registry (debugger), ve formátu CG-ROM.
 *
 * Nepřevrací bity a nemění latche; promítne bajt do efektivního generátoru.
 *
 * @param pcg   Instance; nesmí být NULL.
 * @param addr  Adresa v PCG RAM; použije se `addr & 7FFh`.
 * @param value Bajt ve formátu CG-ROM.
 * @return Maska en_PCG700_EVENT (RAM_WRITTEN, případně GLYPH_CHANGED).
 */
extern unsigned pcg700_core_ram_poke ( st_PCG700 *pcg, unsigned addr, uint8_t value );


/**
 * @brief Převede adresu PCG RAM na index v CG-ROM / efektivním generátoru.
 *
 * Adresa `a` (11 bitů: bit 10 sada, bity 9 - 3 kód - 80h, bity 2 - 0 řádek)
 * odpovídá indexu `sada * 800h + (80h + ((a >> 3) & 7Fh)) * 8 + (a & 7)`.
 *
 * @param addr Adresa v PCG RAM; použije se `addr & 7FFh`.
 * @return Index 0 - 0FFFh (vždy v horní polovině některé sady).
 */
extern unsigned pcg700_core_effective_index ( unsigned addr );


/**
 * @brief Bitové převrácení bajtu (bit 7 <-> bit 0, ...).
 *
 * Převádí mezi formátem PCG-700 (bit 7 = levý bod) a CG-ROM (bit 0 = levý bod).
 *
 * @param value Vstupní bajt.
 * @return Převrácený bajt.
 */
extern uint8_t pcg700_core_bitrev ( uint8_t value );


/**
 * @brief Uloží stav (latche a RAM) do bajtového bufferu.
 *
 * Formát verze PCG700_STATE_VERSION, little-endian:
 *
 * | Offset | Délka | Obsah                                        |
 * |--------|-------|----------------------------------------------|
 * | 0      | 4     | magic 'P', 'C', 'G', '7'                     |
 * | 4      | 2     | verze (u16 LE)                               |
 * | 6      | 2     | celková délka bloku v bajtech (u16 LE)       |
 * | 8      | 1     | latch data (0E010h)                          |
 * | 9      | 1     | latch addr_lo (0E011h)                       |
 * | 10     | 1     | latch ctrl (0E012h)                          |
 * | 11     | 1     | rezerva, 0                                   |
 * | 12     | 2048  | PCG RAM ve formátu CG-ROM                    |
 *
 * Konfigurace ani efektivní generátor se neukládají (generátor je odvozený).
 *
 * @param pcg      Instance; nesmí být NULL.
 * @param buf      Cílový buffer; nesmí být NULL.
 * @param buf_size Velikost bufferu; musí být alespoň PCG700_STATE_SIZE.
 * @return Počet zapsaných bajtů (PCG700_STATE_SIZE), nebo 0 při chybě
 *         (NULL, malý buffer; buffer pak zůstává beze změny).
 */
extern size_t pcg700_core_state_save ( const st_PCG700 *pcg, uint8_t *buf, size_t buf_size );


/**
 * @brief Načte stav (latche a RAM) z bajtového bufferu a přestaví generátor.
 *
 * Konfigurace instance zůstává beze změny. Po úspěchu se efektivní generátor
 * přestaví z `cgrom` (jako pcg700_core_rebuild()).
 *
 * @param pcg   Instance; nesmí být NULL.
 * @param buf   Zdrojový buffer; nesmí být NULL.
 * @param size  Velikost dat v bufferu.
 * @param cgrom CG-ROM 4 kB pro přestavění generátoru; NULL = samé nuly.
 * @param[out] events Volitelně (může být NULL) maska en_PCG700_EVENT:
 *              GLYPH_CHANGED vždy po úspěchu, DISPLAY_CHANGED při změně
 *              zobrazení; při chybě PCG700_EVT_NONE.
 * @return PCG700_OK; PCG700_ERR_INVALID (NULL), PCG700_ERR_FORMAT (magic,
 *         délka), PCG700_ERR_VERSION (verze 0 nebo vyšší než
 *         PCG700_STATE_VERSION). Při chybě je instance beze změny.
 */
extern int pcg700_core_state_load ( st_PCG700 *pcg, const uint8_t *buf, size_t size, const uint8_t *cgrom, unsigned *events );


#ifdef __cplusplus
}
#endif

#endif /* PCG700_CORE_H */
