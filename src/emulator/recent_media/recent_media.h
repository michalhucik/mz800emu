/**
 * @file   recent_media.h
 * @brief  Historie naposledy vložených médií (obrazy disket, pásky, Quick Disky) bez závislosti na UI.
 *
 * Pro každý druh média se drží nejvýš RECENT_MEDIA_MAX cest od naposledy
 * vložené po nejstarší, každá s časem posledního vložení. Vložení cesty,
 * která v historii už je, položku jen přesune na začátek a obnoví její čas.
 * Cesty se porovnávají jako texty: na Windows bez ohledu na velikost písmen
 * a s oddělovači '/' a '\\' jako stejnými; jinde přesně. Relativní cesta se
 * při zápisu převede na absolutní vůči pracovnímu adresáři procesu.
 *
 * Kdo zapisuje: jádro v místě, kde vložení média uspělo, pokud ho vyvolal
 * uživatel (klasické menu, zkratka, overlay menu, příkazová řádka při běhu
 * s oknem):
 * - disketa: fdc_mount_dskfile() (ne při obnově vložení z INI po startu a
 *   ne při novém otevření téhož obrazu po změně volby mechaniky),
 * - páska: cmt_open_file() (ne v tichém režimu při načítání snapshotu),
 * - Quick Disk: qdisk_mount_path() (obraz i adresář virtuálního Quick Disku).
 * Média vkládaná přehráváním záznamu vstupů a vracená po jeho skončení se
 * nezapisují (replay_media.c drží zápis pozastavený, recent_media_suspend()).
 * Stejně se nezapisují vložení, která nedělá uživatel: příkazy MCP
 * `media_insert` a `cmt_open` (dbgapi.c) a vložení diskety emulovaným
 * správcem Unicard (unicard_fdc_mount()). Obě běží v emu vlákně, stejně
 * jako zásahy vyžádané z UI (fronta device_request), takže pozastavení
 * kolem jejich volání nezahodí souběžný zápis uživatele.
 * V běhu bez okna (`--headless`, tedy i `--mcp-pipe`) se nezapisuje nic,
 * ani média z příkazové řádky: cfgmain_init() zápis pozastaví hned po
 * načtení historie a do konce procesu ho neobnoví.
 * Zápis nikdy neběží v horké cestě emulace: stojí jeden zámek a práci
 * s nejvýš RECENT_MEDIA_MAX krátkými texty při vložení média.
 *
 * Uložení: sekce `[RECENT_MEDIA]` konfigurace emulátoru (g_cfgmain), klíče
 * `<druh>_<n>` (cesta) a `<druh>_<n>_time` (čas vložení v sekundách od
 * 1. 1. 1970 UTC) pro druhy `dsk`, `cmt`, `qd` a n = 1 až RECENT_MEDIA_MAX.
 * Načítá se jednou v recent_media_config_init(), zapisuje se s celou
 * konfigurací (cfgroot_save()).
 *
 * @par Známá omezení
 * Cesta, která začíná nebo končí mezerou, se při načtení z konfigurace
 * ořízne (položka pak ukazuje na jiný soubor) a řádek konfigurace delší
 * než 4095 znaků se při načtení ignoruje (položka po restartu chybí).
 * Stejnou slabinu má čtení INI u ostatních klíčů s cestou (cfgmodule.c);
 * historie ji neobchází.
 *
 * @par Vlákna
 * Všechny funkce smí volat libovolné vlákno; stav chrání jeden zámek.
 * Zapisuje typicky emu vlákno (vložení média) a hlavní vlákno při startu
 * (příkazová řádka); UI vlákno čte přes recent_media_dup() do vlastní kopie
 * (overlay_view_fill()). Ukazatel vrácený recent_media_get() platí jen do
 * další změny historie, proto ho smí používat jen kód, který zná všechny
 * zapisovatele (testy, emu vlákno); ostatní volají recent_media_dup().
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef RECENT_MEDIA_H
#define RECENT_MEDIA_H

#include <stdbool.h>

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Největší počet položek historie jednoho druhu média. */
#define RECENT_MEDIA_MAX 12

/** @brief Druh média v historii. */
typedef enum {
    RECENT_MEDIA_DSK,       /**< obraz diskety (FDC0 i FDC1) */
    RECENT_MEDIA_CMT,       /**< soubor pásky */
    RECENT_MEDIA_QD,        /**< Quick Disk: soubor obrazu nebo adresář virtuálního Quick Disku */
    RECENT_MEDIA_KIND_COUNT /**< počet druhů (není druh) */
} en_RECENT_MEDIA_KIND;

/**
 * @brief Zapíše vložení média do historie.
 *
 * Cesta, která v historii druhu už je (porovnání viz popis souboru), se
 * přesune na začátek a zůstane v původním zápisu; nová se vloží na začátek
 * a nejstarší položka nad RECENT_MEDIA_MAX vypadne. Čas položky se nastaví
 * na současný čas. Relativní cesta se uloží jako absolutní.
 *
 * @param kind Druh média; hodnota mimo rozsah se ignoruje.
 * @param path Cesta souboru nebo adresáře (UTF-8, zkopíruje se); NULL nebo prázdná se ignoruje.
 * @post Při pozastaveném zápisu (recent_media_suspend()) se historie nemění.
 */
void recent_media_add(en_RECENT_MEDIA_KIND kind, const char *path);

/**
 * @brief Počet položek historie druhu.
 * @param kind Druh média.
 * @return 0 až RECENT_MEDIA_MAX; 0 pro druh mimo rozsah.
 */
unsigned recent_media_count(en_RECENT_MEDIA_KIND kind);

/**
 * @brief Cesta položky historie.
 * @param kind  Druh média.
 * @param index Pořadí položky; 0 = naposledy vložená.
 * @return Cesta (UTF-8), kterou vlastní historie; NULL pro druh nebo index mimo rozsah.
 * @note Ukazatel platí jen do další změny historie (viz odstavec Vlákna); jiné vlákno než zapisovatel volá recent_media_dup().
 */
const char *recent_media_get(en_RECENT_MEDIA_KIND kind, unsigned index);

/**
 * @brief Čas posledního vložení položky historie.
 * @param kind  Druh média.
 * @param index Pořadí položky; 0 = naposledy vložená.
 * @return Sekundy od 1. 1. 1970 UTC; 0 pro druh nebo index mimo rozsah a u položky, jejíž čas v konfiguraci chyběl.
 */
gint64 recent_media_get_time(en_RECENT_MEDIA_KIND kind, unsigned index);

/**
 * @brief Odstraní položku historie; následující položky se posunou.
 * @param kind  Druh média.
 * @param index Pořadí položky; druh nebo index mimo rozsah se ignoruje.
 */
void recent_media_remove(en_RECENT_MEDIA_KIND kind, unsigned index);

/**
 * @brief Odstraní z historie položku s danou cestou.
 *
 * Pro volající, který drží jen kopii historie (UI): pořadí položek se mezi
 * pořízením kopie a voláním mohlo změnit, cesta ne.
 *
 * @param kind Druh média.
 * @param path Cesta (porovnání jako v recent_media_add()); NULL nebo prázdná = nic.
 * @return true = položka v historii byla a je odstraněná.
 */
bool recent_media_remove_path(en_RECENT_MEDIA_KIND kind, const char *path);

/**
 * @brief Kopie položky historie pro čtení z jiného vlákna.
 * @param kind  Druh média.
 * @param index Pořadí položky; 0 = naposledy vložená.
 * @param[out] time Čas posledního vložení (jako recent_media_get_time()); smí být NULL.
 * @return Nově alokovaná cesta (uvolnit g_free()); NULL pro druh nebo index mimo rozsah.
 */
char *recent_media_dup(en_RECENT_MEDIA_KIND kind, unsigned index, gint64 *time);

/**
 * @brief Číslo stavu historie: změní se při každé změně obsahu.
 *
 * Čtenář, který si drží kopii (overlay menu), podle něj pozná, že ji má
 * obnovit, aniž by historii každý snímek kopíroval.
 *
 * @return Čítač změn od startu procesu (přetéká).
 */
unsigned recent_media_serial(void);

/**
 * @brief Pozastaví zápis do historie (recent_media_add() nic nedělá).
 *
 * Pro kód, který média vkládá za uživatele a jen dočasně (přehrávání
 * záznamu vstupů a návrat médií po něm). Volání se počítají; zápis se
 * obnoví, až recent_media_resume() zazní stejněkrát. Běh bez okna
 * (`--headless`) drží jedno pozastavení po celý běh (cfgmain_init()).
 */
void recent_media_suspend(void);

/**
 * @brief Zruší jedno pozastavení zápisu (recent_media_suspend()).
 * @pre Předcházelo párové recent_media_suspend(); volání navíc se ignoruje.
 */
void recent_media_resume(void);

/**
 * @brief Vyprázdní historii všech druhů (akce uživatele "Clear recent media").
 *
 * Pozastavení zápisu se nemění. Uložená konfigurace se změní až při
 * příštím uložení.
 *
 * @post recent_media_count() je 0 pro každý druh; číslo stavu historie se změnilo.
 */
void recent_media_clear(void);

/**
 * @brief Vyprázdní historii všech druhů a zruší pozastavení zápisu.
 *
 * Pro testy; uložená konfigurace se změní až při příštím uložení.
 */
void recent_media_reset(void);

/**
 * @brief Zaregistruje sekci `[RECENT_MEDIA]` v konfiguraci emulátoru a načte z ní historii.
 *
 * Položky bez cesty se přeskočí, opakovaná cesta se bere jen jednou
 * (ruční zásah do INI), nečitelný čas je 0. Historie v paměti se před
 * načtením vyprázdní.
 *
 * @pre g_cfgmain existuje (cfgmain_init()); volá se jednou pro každý kořen konfigurace, před prvním vložením média.
 * @post Při cfgroot_save() se do sekce zapíše současná historie.
 */
void recent_media_config_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RECENT_MEDIA_H */
