/**
 * @file   devbus_resolve.h
 * @brief  Čistá rezoluce čtení sdílené datové sběrnice z příspěvků N budičů.
 *
 * Model sběrnice devbus: každý rozsah portů nebo MMIO deklaruje výstupní
 * stupeň (en_DEVBUS_DRIVE) a při čtení vrací hodnotu spolu s maskou bitů,
 * které právě budí. Tento modul z takových příspěvků (včetně desky
 * platformy jako dalšího třístavového budiče) složí bajt na sběrnici:
 *
 * 1. kdo nebudí (NONE nebo prázdná maska), se ignoruje;
 * 2. budiče s otevřeným kolektorem tvoří wired-AND s klidovou 1;
 * 3. třístavový nebo částečný budič dodá své bity, ostatní bity dostanou
 *    hodnotu nebuzené sběrnice platformy (@p idle);
 * 4. dva budiče na témže bitu jsou kolize: podle politiky AND hodnot, nebo
 *    vyhrává dříve uvedený příspěvek; kolize se vždy ohlásí maskou;
 * 5. výsledná maska buzených bitů říká, zda vůbec někdo odpověděl.
 *
 * Soubor nemá žádné závislosti kromě <stdint.h>, aby šel testovat
 * STANDALONE a používat i mimo jádro devbus (Bus Map, testy).
 *
 * Hodnota dvou souběžných třístavových budičů na skutečném HW doložena
 * není (jde o zkrat výstupních stupňů); politiky AND a FIRST jsou
 * aproximace emulátoru a volající má každou kolizi hlásit.
 *
 * @par Licence: GPLv3
 */

#ifndef DEVBUS_RESOLVE_H
#define DEVBUS_RESOLVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Výstupní stupeň rozsahu (portů nebo MMIO) vůči datové sběrnici při čtení.
 *
 * Popisuje, co má zařízení na schématu, ne politiku emulátoru: jádro z něj
 * při připojení pozná kolizi dvou budičů a při čtení složí hodnotu
 * sběrnice z příspěvků více vlastníků (devbus_resolve()). Deklarovat jen
 * z doloženého zapojení (schéma, datasheet), nikdy odhadem.
 */
typedef enum en_DEVBUS_DRIVE {
    DEVBUS_DRIVE_NONE = 0,       /**< Sběrnici nebudí nikdy: jen pozoruje zápis (PCG-700, SUC 80). */
    DEVBUS_DRIVE_TRISTATE,       /**< Třístavový budič: při vlastním dekódu budí všech 8 bitů (74LS245 za /BS u K&P). */
    DEVBUS_DRIVE_PARTIAL,        /**< Budí jen bity z drive_mask, ostatní nechává plavat (joystick přes 74LS365: 3Fh). */
    DEVBUS_DRIVE_OPEN_COLLECTOR, /**< Otevřený kolektor (wired-AND, klid 1); na datech MZ nedoloženo, jen pro úplnost. */
    DEVBUS_DRIVE_UNKNOWN,        /**< Jen statická tabulka vlastníků (bus_map): v bázi nedoloženo. Deskriptor zařízení ho mít nesmí. */
} en_DEVBUS_DRIVE;

/**
 * @brief Politika dvou budičů na témže bitu téže adresy.
 *
 * REFUSE je výchozí a odpovídá dřívějšímu chování (druhé zařízení se
 * nepřipojí, DEVBUS_ERR_CONFLICT). AND a FIRST připojení dovolí a při
 * každém čtení s kolizí vyvolají událost bus_conflict; výsledek na
 * skutečném HW není doložen (souběh třístavových budičů je zkrat).
 */
typedef enum en_DEVBUS_CONFLICT {
    DEVBUS_CONFLICT_REFUSE = 0, /**< Druhý budič se nepřipojí; při rezoluci se chová jako FIRST. */
    DEVBUS_CONFLICT_AND,        /**< Překrývající se bity = AND hodnot (aproximace "0 vyhrává"). */
    DEVBUS_CONFLICT_FIRST,      /**< Překrývající se bity od dříve uvedeného (dříve připojeného) budiče. */
} en_DEVBUS_CONFLICT;

/**
 * @brief Hodnota bitů, které při čtení nikdo nebudí (vlastnost platformy, ne zařízení).
 *
 * LATCH = poslední bajt přečtený z paměti (regDBUS_latch, dosavadní model
 * mz800new); CONST = pevná hodnota (např. 0FFh). Na skutečném HW
 * neměřeno; výchozí je LATCH, CONST je volba INI pro porovnání s dobovým
 * software.
 */
typedef enum en_DEVBUS_IDLE {
    DEVBUS_IDLE_LATCH = 0,  /**< regDBUS_latch platformy (st_DEVBUS_ARCH_OPS::bus_latch). */
    DEVBUS_IDLE_CONST,      /**< Konstanta devbus_bus_idle_value(). */
} en_DEVBUS_IDLE;

/**
 * @brief Jeden příspěvek do čtení sběrnice.
 *
 * @invariant driven je podmnožina deklarované masky budiče (volající ji
 *            ořízne přes devbus_drive_mask()); u DEVBUS_DRIVE_NONE se
 *            hodnota i maska ignorují.
 */
typedef struct st_DEVBUS_CONTRIB {
    en_DEVBUS_DRIVE drive;  /**< Výstupní stupeň přispěvatele. */
    uint8_t value;          /**< Hodnota, kterou přispěvatel vrátil (platí jen bity v driven). */
    uint8_t driven;         /**< Bity, které přispěvatel při tomto čtení skutečně budí. */
} st_DEVBUS_CONTRIB;

/**
 * @brief Výsledek rezoluce čtení.
 */
typedef struct st_DEVBUS_RESOLVED {
    uint8_t value;     /**< Bajt na sběrnici (buzené bity od budičů, zbytek z idle). */
    uint8_t driven;    /**< Sjednocení buzených bitů; 0 = nikdo neodpověděl (UNCONNECTED). */
    uint8_t conflict;  /**< Bity, které budily dva a více budičů (0 = bez kolize). */
} st_DEVBUS_RESOLVED;

/**
 * @brief Efektivní maska bitů, které rozsah podle deklarace budí.
 * @param drive      Výstupní stupeň.
 * @param drive_mask Maska z deskriptoru (má význam jen u PARTIAL).
 * @return 0 u NONE a UNKNOWN, drive_mask u PARTIAL, 0FFh jinak.
 */
uint8_t devbus_drive_mask(en_DEVBUS_DRIVE drive, uint8_t drive_mask);

/**
 * @brief Složí bajt na sběrnici z příspěvků podle pravidel 1-5 (viz hlavička souboru).
 *
 * Příspěvky se zpracují v daném pořadí; u politiky FIRST (a REFUSE)
 * vyhrává na sporném bitu dříve uvedený třístavový budič, u AND se sporné
 * bity sloučí logickým AND. Otevřené kolektory se slučují mezi sebou
 * wired-AND s klidovou 1; jejich souběh s třístavovým budičem je také
 * kolize (AND podle politiky, jinak vyhrává třístavový budič).
 *
 * @param c      Pole příspěvků (smí být NULL při n == 0).
 * @param n      Počet příspěvků.
 * @param idle   Hodnota nebuzených bitů (platforma: latch nebo konstanta).
 * @param policy Politika kolize.
 * @param out    Výsledek (nesmí být NULL).
 * @post out->driven == 0 právě tehdy, když žádný příspěvek nebudí; pak
 *       out->value == idle.
 */
void devbus_resolve(const st_DEVBUS_CONTRIB *c, unsigned n, uint8_t idle,
                    en_DEVBUS_CONFLICT policy, st_DEVBUS_RESOLVED *out);

/**
 * @brief Překrývají se dva budiče (kolize při připojení)?
 *
 * Dva rozsahy jsou v kolizi, když oba budí (maska po devbus_drive_mask()
 * nenulová), masky mají společný bit a nejsou oba otevřený kolektor.
 *
 * @return 1 = kolize, 0 = smějí sdílet adresu.
 */
int devbus_drive_overlaps(en_DEVBUS_DRIVE da, uint8_t ma, en_DEVBUS_DRIVE db, uint8_t mb);

/**
 * @brief Jméno výstupního stupně pro INI, MCP a UI ("none", "tristate", "partial", "open_collector", "unknown").
 * @return Statický řetězec; "unknown" i pro hodnotu mimo výčet.
 */
const char *devbus_drive_name(en_DEVBUS_DRIVE drive);

/**
 * @brief Jméno politiky kolize ("refuse", "and", "first").
 * @return Statický řetězec; "refuse" pro hodnotu mimo výčet.
 */
const char *devbus_conflict_name(en_DEVBUS_CONFLICT policy);

/**
 * @brief Převede jméno politiky na hodnotu (bez ohledu na velikost písmen).
 * @param text Text (smí být NULL).
 * @param out  Výstup (nesmí být NULL).
 * @return 1 = rozpoznáno (*out zapsán), 0 = neznámé jméno (*out beze změny).
 */
int devbus_conflict_parse(const char *text, en_DEVBUS_CONFLICT *out);

/**
 * @brief Jméno modelu nebuzené sběrnice ("latch", "const").
 * @return Statický řetězec; "latch" pro hodnotu mimo výčet.
 */
const char *devbus_idle_name(en_DEVBUS_IDLE mode);

/**
 * @brief Převede jméno modelu nebuzené sběrnice na hodnotu (bez ohledu na velikost písmen).
 * @param text Text (smí být NULL).
 * @param out  Výstup (nesmí být NULL).
 * @return 1 = rozpoznáno (*out zapsán), 0 = neznámé jméno.
 */
int devbus_idle_parse(const char *text, en_DEVBUS_IDLE *out);

#ifdef __cplusplus
}
#endif

#endif /* DEVBUS_RESOLVE_H */
