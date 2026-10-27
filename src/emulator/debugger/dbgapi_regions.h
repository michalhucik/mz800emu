/*
 * dbgapi_regions.h - inventář paměťových regionů pro Memory Browser
 *
 * Poskytuje banking-aware enumeraci všech paměťových regionů aktuální
 * architektury (MZ-800/MZ-700/MZ-1500) plus no-side-effect read a safe
 * write per region.
 *
 * Architektonický kontext:
 *   - Per-arch implementace v src/emulator/mzarch/<arch>/debugger/<arch>_regions.c
 *   - Společné jádro v src/emulator/debugger/dbgapi_regions.c dispatchuje
 *     přes compile-time MZARCH a sdílí kód pro read/write (Memext, Ramdisk,
 *     VRAM plane, PROHIBITED shadow).
 *   - API je určeno pro Memory Browser, Watch okno a Cheat Search; volá se
 *     z debug vlákna (UI side) - není v emulátor hot path.
 *
 * ID stabilita:
 *   region.id je index do výčtu a platí, dokud se nezmění sestava HW
 *   (RAMdisk, MemExt, PEZIK, PROHIBITED, načtení snapshotu s jinou
 *   sestavou). Změnu rozložení výčtu hlásí generace
 *   (dbgapi_regions_layout_gen()); dbgapi_regions_read_gen() a
 *   dbgapi_regions_write_gen() ID ze starší generace odmítnou. Klient nesmí
 *   persistovat raw `id` mezi spuštěními. Pro persistenci je doporučeno
 *   persistovat dvojici (kind, sub_id) a při restartu znovu resolvovat na
 *   aktuální id přes enumerate.
 *
 * Paměťová bezpečnost:
 *   read/write kontrolují v každé větvi živou velikost cílového bufferu
 *   (RAMdisk STD podle g_ramdisk.std.size, banky MemExt podle pole), takže
 *   ani deskriptor ze starší generace nečte ani nezapisuje mimo buffer.
 *
 * No-side-effect contract:
 *   - read: nikdy nesmí měnit chip state (auto-inc latch, GDG RF dispatch,
 *     IRQ trigger, atd.). Přímý array access nebo speciální *_no_se varianty.
 *   - enumerate: čte g_memory.map, HW connection flagy a vrací snapshot;
 *     bez side-effectu na běžící HW.
 *
 * Ownership:
 *   - st_REGION_DESC pole alokuje caller a předá pres out parametr; modul
 *     pouze zapisuje a nealokuje.
 *   - Vrácená data jsou snapshot - po návratu z funkce může klient držet
 *     bez další synchronizace.
 *
 * Licence: GPLv3
 *
 * ----------------------------- License -------------------------------------
 *
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
 *
 * ---------------------------------------------------------------------------
 */

#ifndef DBGAPI_REGIONS_H
#define DBGAPI_REGIONS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Druh paměťového regionu.
 *
 * Per arch je viditelná pouze podmnožina (např. PCG_1500 jen na MZ-1500,
 * VRAM_PHYS_PLANE jen na MZ-800). Caller musí umět zpracovat jakoukoliv
 * hodnotu, kterou enumerate vrátí.
 *
 * sub_id v st_REGION_DESC disambiguuje regiony stejného druhu (plane index,
 * bank index, PEZIK instance).
 */
typedef enum en_REGION_KIND
{
    REGION_KIND_LOGICAL = 0,            /**< Současný Z80 view (banking-aware). */
    REGION_KIND_RAM,                    /**< Plná 64 KB user DRAM jak ji vidí CPU pro RAM read/write (čte/zapisuje přes memram_read/write[] indirection, ne přímo do g_memory.RAM[] pole). S připojeným Memextem se data čtou/zapisují do aktivní memext bank; bez Memextu identické s g_memory.RAM[]. */
    REGION_KIND_ROM_LOWER,              /**< Monitor ROM 0x0000-0x0FFF (4 KB). */
    REGION_KIND_ROM_UPPER,              /**< Monitor ROM 0xE000-0xFFFF (8 KB). */
    REGION_KIND_CGROM,                  /**< CG-ROM (4 KB). MZ-800 native v 0x1000-0x1FFF, MZ-1500 přes SPEC=1. */
    REGION_KIND_CGRAM_700,              /**< CG-RAM MZ-700 (4 KB). MZ-800 v 700 compat módu. */
    REGION_KIND_VRAM_700_CHAR,          /**< Znaková VRAM MZ-700 (2 KB v 0xD000-0xD7FF; obraz používá prvních 1000 B). */
    REGION_KIND_VRAM_700_ATTR,          /**< Atributová VRAM MZ-700 (2 KB v 0xD800-0xDFFF; obraz používá prvních 1000 B). */
    REGION_KIND_VRAM_PHYS_PLANE,        /**< MZ-800 fyzická plane I-IV (8 KB raw), sub_id = 0..3. */
    REGION_KIND_PCG_1500,               /**< MZ-1500 PCG bank 1..3 (8 KB), sub_id = 0..2. */
    REGION_KIND_MEMEXT_RAM,             /**< Memext RAM bank (4 KB), sub_id = raw 4 KB index 0..0x7F (Luftner i PEHU; PEHU 8 KB banka b = raw 2*b a 2*b + 1). */
    REGION_KIND_MEMEXT_FLASH,           /**< Memext Luftner FLASH bank (4 KB), sub_id = raw bank index 0x80..0xFF. */
    REGION_KIND_RAMDISK_STD,            /**< Ramdisk STD bank (64 KB), sub_id = bank index 0..255. */
    REGION_KIND_RAMDISK_PEZIK,          /**< Ramdisk PEZIK bank (64 KB), sub_id = pezik_instance * 8 + bank_idx (0..7 = port 0x68 banks 0..7, 8..15 = port 0xE8 banks 0..7). */
    REGION_KIND_PROHIBITED_SHADOW,      /**< Virtuální shadow region (vrací 0x1A) když PROHIBITED mode aktivní. */
    REGION_KIND_DEVICE,                 /**< Paměť volitelného zařízení devbus (st_DEVBUS_REGION), sub_id = slot * 16 + index regionu; jen připojená zařízení. */
    REGION_KIND_COUNT                   /**< Sentinel - počet kindů. */
} en_REGION_KIND;

/**
 * @brief Popis jednoho paměťového regionu.
 *
 * Vrací se z dbgapi_regions_enumerate() jako pole snapshotů. ID se používá
 * pro následující read/write volání ve stejné session.
 *
 * Invariant:
 *   - id je unikátní 0..N-1 v jedné enumerate iteraci
 *   - name je '\0'-terminated ASCII (anglicky, pro i18n přes _() v UI vrstvě)
 *   - size > 0 vždy
 *   - logical_base = 0xFFFFFFFF znamená "region není mapovaný do Z80 prostoru
 *     ve výchozím stavu" (např. fyzická plane, off-bank, PROHIBITED shadow)
 */
typedef struct st_REGION_DESC
{
    int             id;             /**< Stable ID 0..N-1 v rámci session. */
    en_REGION_KIND  kind;           /**< Druh regionu. */
    int             sub_id;         /**< Disambiguator (plane/bank index, PEZIK instance). */
    char            name[64];       /**< ASCII jméno (anglicky, klíč pro _() v UI). */
    uint32_t        logical_base;   /**< Z80 adresa kde region "obvykle" sedí, nebo 0xFFFFFFFF. */
    uint32_t        size;           /**< Velikost regionu v bajtech. */
    int             writable;       /**< 1 = lze do něj psát přes dbgapi_regions_write, 0 = R-only. */
    int             connected;      /**< 1 = HW připojen / data validní, 0 = volitelný HW odpojen. */
    int             mapped_now;     /**< 1 = aktuálně viditelný v Z80 prostoru (banking match), 0 = off-bank. */
} st_REGION_DESC;

/**
 * @brief Enumeruje všechny paměťové regiony aktuální architektury.
 *
 * Vrací snapshot dle aktuálního banking stavu (`g_memory.map`, GDG DMD) a
 * connection flagů volitelného HW (Memext, Ramdisk STD, PEZIK 68/E8).
 *
 * Per arch je seznam regionů ohraničený:
 *   - MZ-800: LOGICAL, RAM, ROM_LOWER, ROM_UPPER, CGROM, CGRAM_700,
 *             VRAM_700_CHAR/ATTR (jen v 700 compat módu), 4× VRAM_PHYS_PLANE
 *             (plane III/IV vždy enumerované - EXVRAM pole je vždy alokované
 *             v st_MEMORY), MEMEXT_RAM/FLASH banks (pokud connected),
 *             RAMDISK_STD banks (pokud connected), RAMDISK_PEZIK 0/1 (pokud
 *             příslušná instance connected), PROHIBITED_SHADOW (pokud aktivní).
 *   - MZ-700: LOGICAL, RAM, ROM_LOWER, ROM_UPPER, CGRAM_700, VRAM_700_CHAR/ATTR,
 *             PROHIBITED_SHADOW (pokud aktivní). Bez Memext/Ramdisk/VRAM plane.
 *   - MZ-1500: LOGICAL, RAM, ROM_LOWER, ROM_UPPER, CGROM, VRAM_700_CHAR/ATTR,
 *              3× PCG_1500, MEMEXT_RAM/FLASH banks (pokud connected; MZ-1500
 *              má port 0xE7 jako MZ-800). PROHIBITED není MZ-1500 koncept;
 *              nepřidává se.
 *
 * No-side-effect: čte pouze stav HW (banking, connection), nezasahuje do
 * chip state. Vhodné pro UI thread polling.
 *
 * @param[out] out         Pole st_REGION_DESC alokované callerem.
 * @param[in]  max_count   Maximální počet zapsaných položek (= velikost pole).
 * @return                 Skutečný počet zapsaných položek (0..max_count).
 *                         Při out == NULL nebo max_count <= 0 vrací -1.
 *
 * @note ID v out[i].id = i. Caller může spoléhat na 0-based pořadí v rámci
 *       jednoho volání. Mezi voláními (nebo po HW reconfigure - např. po
 *       Memext connect/disconnect) se pořadí může změnit; klient si nesmí
 *       cache-ovat ID mezi enumerate calls.
 */
int dbgapi_regions_enumerate(st_REGION_DESC *out, int max_count);

/**
 * @brief Obnoví vnitřní cache výčtu regionů podle aktuálního stavu HW.
 *
 * Stejné jako dbgapi_regions_enumerate() bez kopie ven. Použití: volající,
 * který pak hledá v cache přes dbgapi_regions_find_kind() nebo
 * dbgapi_regions_get() a potřebuje čerstvé příznaky connected/mapped_now
 * (freeze jednou za snímek).
 *
 * @post Cache obsahuje všechny regiony aktuální sestavy (bez stropu); ID se
 *       mohla po HW reconfigure posunout.
 * @note Thread-safe (zámek cache), bez vedlejších efektů na stav čipů.
 *       Může alokovat (cache roste podle počtu regionů).
 */
void dbgapi_regions_refresh(void);

/**
 * @brief Vrátí kopii celého výčtu regionů (bez stropu na počet).
 *
 * Obnoví cache jako dbgapi_regions_enumerate() a vrátí nově alokovanou kopii
 * všech položek. Pro volající, kteří nesmí výčet useknout (Memory Browser,
 * MCP `regions_list`): plná sestava MZ-800 (MemExt Luftner, RAMdisk STD 16 MB,
 * oba PEZIKy) má přes 500 regionů.
 *
 * @param[out] out_count Počet položek ve vrácené kopii (0 = žádné); smí být NULL.
 * @return Kopie pole (out[i].id == i), nebo NULL, když je výčet prázdný.
 *         Vlastní volající, uvolní přes g_free().
 * @note Thread-safe (zámek cache), bez vedlejších efektů na stav čipů.
 */
st_REGION_DESC *dbgapi_regions_enumerate_dup(int *out_count);

/**
 * @brief Deskriptor regionu podle ID z posledního výčtu.
 *
 * Bez nového výčtu (cache se plní jen tehdy, když je prázdná). ID platí
 * do příští změny sestavy HW, viz dbgapi_regions_enumerate().
 *
 * @param[in]  region_id ID z dbgapi_regions_enumerate() a spol.
 * @param[out] out       Kopie deskriptoru (zapsaná jen při úspěchu).
 * @return true = ID existuje, false = neplatné ID nebo @p out == NULL.
 * @note Thread-safe (zámek cache).
 */
bool dbgapi_regions_get(int region_id, st_REGION_DESC *out);

/**
 * @brief Najde region podle (kind, sub_id) v posledním výčtu.
 *
 * Bez nového výčtu (cache se plní jen tehdy, když je prázdná); čerstvý stav
 * dá předchozí dbgapi_regions_refresh(). Vrací i odpojený nebo jen pro čtení
 * region, rozhoduje volající podle @c connected a @c writable.
 *
 * @param[in]  kind   Druh regionu.
 * @param[in]  sub_id Disambiguator (banka, plane, instance PEZIK).
 * @param[out] out    Kopie deskriptoru (zapsaná jen při úspěchu).
 * @return true = nalezeno, false = region v sestavě není nebo @p out == NULL.
 * @note Thread-safe (zámek cache).
 */
bool dbgapi_regions_find_kind(en_REGION_KIND kind, int sub_id, st_REGION_DESC *out);

/**
 * @brief No-side-effect read z regionu.
 *
 * Čte len bajtů z (region_id, offset). Žádný auto-inc latch, žádný GDG RF
 * dispatch, žádný IRQ trigger. Pro REGION_KIND_LOGICAL volá per-arch
 * memory_read_byte_no_se(), pro fyzické regiony přímý array access.
 *
 * Pokud `offset + len` přesáhne velikost regionu, čte se jen do konce
 * regionu (clamp) a vrací se zkrácená délka.
 *
 * @param[in]  region_id   ID získané z dbgapi_regions_enumerate(). Musí
 *                         odpovídat poslední enumerate volání (stabilita
 *                         per session).
 * @param[in]  offset      Offset v rámci regionu (0..size-1).
 * @param[out] out         Buffer pro výsledek; min `len` bajtů.
 * @param[in]  len         Počet bajtů k přečtení.
 * @return                 Počet skutečně přečtených bajtů (0..len), -1 při
 *                         chybě: out=NULL, len=0, region_id mimo rozsah,
 *                         offset >= size, region disconnected, region mimo
 *                         živou velikost bufferu (deskriptor ze starší
 *                         sestavy HW).
 *
 * @pre Caller musel zavolat dbgapi_regions_enumerate() a získat platné ID.
 *      Po změně sestavy HW může ID ukazovat na jiný region; kontrolu
 *      generace dělá dbgapi_regions_read_gen().
 * @post Žádný chip state se nezměnil.
 */
int dbgapi_regions_read(int region_id, uint32_t offset, uint8_t *out, uint32_t len);

/**
 * @brief Zápis do regionu (respekt writable flagů).
 *
 * Pro REGION_KIND_MEMEXT_FLASH vrací -1 (FLASH je R-only z pohledu Memory
 * Browseru - CPU-level WOM redirect je interní HW emulace a do debug API
 * se nepromítá).
 * Pro REGION_KIND_PROHIBITED_SHADOW vrací -1 (virtual region).
 * Pro VRAM regiony nedělá automatický screen refresh - to je UI zodpovědnost
 * při edit v pause modu.
 *
 * @param[in] region_id   ID získané z dbgapi_regions_enumerate().
 * @param[in] offset      Offset v rámci regionu.
 * @param[in] data        Buffer s daty k zapsání.
 * @param[in] len         Počet bajtů k zapsání.
 * @return                Počet skutečně zapsaných bajtů (0..len), -1 při
 *                        chybě: data=NULL, len=0, region_id mimo rozsah,
 *                        offset >= size, region disconnected, region R-only,
 *                        region mimo živou velikost bufferu (deskriptor ze
 *                        starší sestavy HW).
 *
 * @pre Region musí být writable a connected.
 * @post Při úspěchu jsou data zapsaná do paměti; ostatní state nedotčen
 *       (banking, latch, atd.).
 */
int dbgapi_regions_write(int region_id, uint32_t offset, const uint8_t *data, uint32_t len);

/**
 * @brief Návratový kód dbgapi_regions_read_gen() / dbgapi_regions_write_gen():
 *        ID pochází ze staršího rozložení výčtu.
 *
 * Sestava HW se od výčtu, ze kterého volající ID má, změnila (RAMdisk,
 * MemExt, PEZIK, PROHIBITED, načtení snapshotu s jinou sestavou). ID se
 * mohla posunout nebo region zmizel; volající musí udělat nový výčet.
 */
#define DBGAPI_REGIONS_ERR_STALE (-2)

/**
 * @brief Generace rozložení výčtu regionů.
 *
 * Číslo se zvýší, kdykoli plnění cache (výčet, refresh, kontrola v
 * dbgapi_regions_read_gen()) dá jiné rozložení než předchozí plnění:
 * jiný počet regionů nebo jiný druh, sub_id, velikost, `writable` či
 * `connected` některého z nich (`mapped_now` se mění s bankováním a do
 * rozložení nepatří). Stejné rozložení generaci nemění, takže ID z výčtu
 * platí, dokud se generace nezmění.
 *
 * Bez nového výčtu: vrací generaci posledního plnění cache.
 *
 * @return Generace >= 1, nebo 0, dokud se cache ještě nenaplnila.
 * @note Thread-safe (zámek cache).
 */
uint32_t dbgapi_regions_layout_gen(void);

/**
 * @brief Jako dbgapi_regions_enumerate_dup(), navíc vrátí generaci rozložení.
 *
 * Kopie výčtu a generace pocházejí ze stejného plnění cache (pod jedním
 * zámkem), takže generace přesně odpovídá vráceným ID.
 *
 * @param[out] out_count Počet položek ve vrácené kopii; smí být NULL.
 * @param[out] out_gen   Generace rozložení (>= 1); smí být NULL.
 * @return Viz dbgapi_regions_enumerate_dup(). Vlastní volající (g_free()).
 * @note Thread-safe (zámek cache), bez vedlejších efektů na stav čipů.
 */
st_REGION_DESC *dbgapi_regions_enumerate_dup_gen(int *out_count, uint32_t *out_gen);

/**
 * @brief No-side-effect read s kontrolou, že ID platí pro aktuální sestavu HW.
 *
 * Při @p layout_gen != 0 nejdřív obnoví cache podle aktuálního stavu HW
 * a porovná generaci rozložení s @p layout_gen (generace výčtu, ze kterého
 * volající ID má). Při neshodě nic nečte a vrátí DBGAPI_REGIONS_ERR_STALE.
 * Při @p layout_gen == 0 se chová přesně jako dbgapi_regions_read().
 *
 * @param[in]  region_id  ID z výčtu generace @p layout_gen.
 * @param[in]  layout_gen Generace výčtu (dbgapi_regions_enumerate_dup_gen()),
 *                        0 = bez kontroly.
 * @param[in]  offset     Offset v rámci regionu.
 * @param[out] out        Buffer pro výsledek; min @p len bajtů.
 * @param[in]  len        Počet bajtů k přečtení.
 * @return Počet přečtených bajtů, -1 při chybě jako dbgapi_regions_read(),
 *         DBGAPI_REGIONS_ERR_STALE při změně rozložení.
 * @note Obnova cache stojí jeden výčet (stovky regionů v plné sestavě);
 *       určeno pro jednotlivé dotazy zvenčí (MCP), ne pro čtení po řádcích.
 */
int dbgapi_regions_read_gen(int region_id, uint32_t layout_gen,
                            uint32_t offset, uint8_t *out, uint32_t len);

/**
 * @brief Zápis do regionu s kontrolou, že ID platí pro aktuální sestavu HW.
 *
 * Kontrola generace jako dbgapi_regions_read_gen(), jinak stejné jako
 * dbgapi_regions_write() (včetně záznamu zápisů).
 *
 * @param[in] region_id  ID z výčtu generace @p layout_gen.
 * @param[in] layout_gen Generace výčtu, 0 = bez kontroly.
 * @param[in] offset     Offset v rámci regionu.
 * @param[in] data       Data k zapsání.
 * @param[in] len        Počet bajtů.
 * @return Počet zapsaných bajtů, -1 při chybě jako dbgapi_regions_write(),
 *         DBGAPI_REGIONS_ERR_STALE při změně rozložení (nic nezapsáno).
 */
int dbgapi_regions_write_gen(int region_id, uint32_t layout_gen,
                             uint32_t offset, const uint8_t *data, uint32_t len);

/**
 * @brief Buňka záznamu zápisů (ramlog) pro bajt regionu.
 *
 * Stejný převod, jakým dbgapi_regions_write() zaznamená zápis. RAM regionu
 * REGION_KIND_RAM se určí podle aktuálního mapování MemExt.
 *
 * @param kind       en_REGION_KIND.
 * @param sub_id     sub_id regionu.
 * @param offset     Offset v regionu.
 * @param out_space  Výstup: en_RAMLOG_SPACE.
 * @param out_offset Výstup: offset v prostoru.
 * @return false = region neexistuje, offset je mimo region, nebo region
 *         záznam zápisů nepokrývá (ROM, RAMdisk, Logical - ten se ptá
 *         podle CPU adresy).
 * @pre Volá se v emu vlákně (čte mapování paměti).
 */
bool dbgapi_regions_ramlog_cell(int kind, int sub_id, uint32_t offset,
                                uint8_t *out_space, uint32_t *out_offset);

#ifdef __cplusplus
}
#endif

#endif /* DBGAPI_REGIONS_H */
