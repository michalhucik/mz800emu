/*
 * File:   memext.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 17. července 2018, 20:02
 *
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

#ifndef MEMEXT_H
#define MEMEXT_H

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#define MEMEXT_RAM_SIZE 0x80000
#define MEMEXT_FLASH_SIZE 0x80000

#define MEMEXT_LUFTNER_BANKS 0x80
#define MEMEXT_LUFTNER_BANK_MASK 0xff
#define MEMEXT_LUFTNER_BANK_SIZE 0x1000
#define MEMEXT_LUFTNER_ADDR_MASK 0x0fff

#define MEMEXT_PEHU_BANKS 0x40
#define MEMEXT_PEHU_MASK 0x3f
#define MEMEXT_PEHU_BANK_SIZE 0x2000
#define MEMEXT_PEHU_ADDR_MASK 0x1fff

#define MEMEXT_RAW_MAP_SIZE 0x10
#define MEMEXT_RAW_BANK_SIZE 0x1000

#define MEMEXT_DEFAULT_FLASH_FNAME "flash.dat"

typedef enum en_MEMEXT_CONNECTION
{
    MEMEXT_CONNECTION_NO = 0,
    MEMEXT_CONNECTION_YES = 1,
} en_MEMEXT_CONNECTION;

typedef enum en_MEMEXT_TYPE
{
    MEMEXT_TYPE_LUFTNER = 0,
    MEMEXT_TYPE_PEHU,
} en_MEMEXT_TYPE;

typedef enum en_MEMEXT_INIT_MEM
{
    MEMEXT_INIT_MEM_NULL = 0,
    MEMEXT_INIT_MEM_RANDOM,
    MEMEXT_INIT_MEM_SHARP,
} en_MEMEXT_INIT_MEM;

typedef enum en_MEMEXT_INIT_LUFTNER
{
    MEMEXT_INIT_LUFTNER_NONE = 0,
    MEMEXT_INIT_LUFTNER_RESET,
} en_MEMEXT_INIT_LUFTNER;

typedef struct st_MEMEXT
{
    en_MEMEXT_CONNECTION connection;
    en_MEMEXT_TYPE type;
    en_MEMEXT_INIT_MEM init_mem;
    en_MEMEXT_INIT_LUFTNER init_luftner;
    char *flash_filepath;
    uint32_t map[MEMEXT_RAW_MAP_SIZE];
    uint16_t addr_mask;
    uint8_t RAM[MEMEXT_RAM_SIZE];
    uint8_t FLASH[MEMEXT_FLASH_SIZE];
    uint8_t WOM[MEMEXT_RAW_BANK_SIZE]; // Write Only Memory :-)
} st_MEMEXT;

extern st_MEMEXT g_memext;

#define MEMEXT_TEST_TYPE_LUFTNER (g_memext.type == MEMEXT_TYPE_LUFTNER)
#define MEMEXT_TEST_TYPE_PEHU (g_memext.type == MEMEXT_TYPE_PEHU)

#define MEMEXT_TEST_CONNECTED (g_memext.connection == MEMEXT_CONNECTION_YES)
#define MEMEXT_TEST_CONNECTED_PEHU (MEMEXT_TEST_CONNECTED && MEMEXT_TEST_TYPE_PEHU)
#define MEMEXT_TEST_CONNECTED_LUFTNER (MEMEXT_TEST_CONNECTED && MEMEXT_TEST_TYPE_LUFTNER)

#define MEMEXT_TEST_INIT_FILL_NULL (g_memext.init_mem == MEMEXT_INIT_MEM_NULL)
#define MEMEXT_TEST_INIT_FILL_RANDOM (g_memext.init_mem == MEMEXT_INIT_MEM_RANDOM)
#define MEMEXT_TEST_INIT_FILL_SHARP (g_memext.init_mem == MEMEXT_INIT_MEM_SHARP)

#define MEMEXT_TEST_LUFTNER_AUTO_INIT (g_memext.init_luftner == MEMEXT_INIT_LUFTNER_RESET)

#define MEMEXT_SET_INIT_FILL(type) g_memext.init_mem = type
#define MEMEXT_SET_INIT_LUFTNER(type) g_memext.init_luftner = type

#define MEMEXT_LUFTNER_GET_FLASH_FILEPATH() g_memext.flash_filepath

#ifdef __cplusplus
extern "C"
{
#endif

    void memext_init(void);
    void memext_reset(void);

    void memext_connect(en_MEMEXT_TYPE type);
    void memext_disconnect(void);

    void memext_flash_reload(void);

    void memext_map_pwrite(int addr_point, uint8_t value);

    /**
     * @brief Převede aktuální g_memext.map[] na hodnoty zápisu OUT E7h.
     *
     * Výsledek je inverzí memext_map_pwrite() pro aktuální g_memext.type:
     * zapsání @p values zpět (memext_map_request() nebo OUT E7h na
     * address pointy) mapu nezmění. Používá okno MemExt Map Settings
     * k naplnění editačních polí.
     *
     *  - LUFTNER: values[i] = map[i] & 0xFF (4 KB raw banka, bit 7 =
     *    FLASH) - beze změny proti dřívějšímu chování okna.
     *  - PEHU: values[i] = číslo 8 KB banky páru, do kterého address
     *    point i patří, tj. (map[i & ~1] >> 1) & MEMEXT_PEHU_MASK; sudý
     *    i lichý point páru dostane stejnou hodnotu. (map[] drží 4 KB
     *    raw banky 2n a 2n+1, OUT E7h ale bere číslo 8 KB banky n.)
     *
     * @param values Výstupní pole MEMEXT_RAW_MAP_SIZE hodnot; vlastní ho
     *               volající. NULL = no-op.
     *
     * @pre g_memext.map[] odpovídá g_memext.type (mapu zapisuje
     *      memext_map_pwrite(), memext_reset() nebo snapshot).
     *
     * Vedlejší efekty: žádné (jen čte g_memext.map[] a g_memext.type).
     * Volá se z UI vlákna bez synchronizace; souběžný OUT E7h může dát
     * směs starých a nových hodnot (jen zobrazení, stejné jako dřív).
     */
    void memext_map_get_out_values(uint8_t *values);

    /**
     * @brief Příznak čekajícího požadavku na přemapování MemExt z UI vlákna.
     *
     * Nastavuje memext_map_request() (UI vlákno), nuluje
     * memext_map_request_process() (emu vlákno). Čte se jen atomicky
     * (g_atomic_int_get) v memext_map_request_poll(). Mimo tyto funkce
     * nezapisovat.
     */
    extern gint g_memext_map_request_pending;

    /**
     * @brief Požádá emu vlákno o zápis celé mapovací tabulky MemExt.
     *
     * Okno MemExt Map Settings dřív volalo memext_map_pwrite() přímo z UI
     * vlákna: zápis g_memext.map[] a memory_reconnect_ram() (přepojení
     * memram_read/write, na MZ-800 přepočet RAM fast-path tabulky CPU)
     * souběžně s přístupy CPU do paměti a s OUT E7h. Funkce jen uloží kopii
     * hodnot a nastaví příznak; vlastní zápis provede emu vlákno
     * v memext_map_request_process(). Mechanismus nepoužívá frontu dbgapi,
     * takže funguje i v buildu bez debuggeru.
     *
     * Asynchronní: návrat neznamená provedení. Emu vlákno požadavek vyzvedne
     * při nejbližší emulační události (memext_map_request_poll() v hlavní
     * smyčce mzarch), v pauze do ~20 ms (iterace paused smyčky). Další
     * požadavek před vyzvednutím předchozí nahradí (vždy jde o celou
     * tabulku).
     *
     * @param values Hodnoty pro address pointy 0..MEMEXT_RAW_MAP_SIZE-1 ve
     *               stejném významu jako @p value u memext_map_pwrite()
     *               (OUT E7h s A12-A15 = address point; naplnit lze
     *               memext_map_get_out_values()). U PEHU se použijí jen
     *               hodnoty sudých pointů (viz memext_map_request_process()).
     *               Pole se kopíruje, volající ho vlastní. NULL = no-op.
     *
     * @pre Volat z UI vlákna (ne z emu vlákna - tam rovnou memext_map_pwrite()).
     * @post g_memext_map_request_pending != 0, dokud emu vlákno požadavek
     *       nevyzvedne.
     */
    void memext_map_request(const uint8_t *values);

    /**
     * @brief Provede čekající požadavek z memext_map_request() (emu vlákno).
     *
     * Pod zámkem převezme kopii hodnot a vynuluje příznak, potom mimo
     * zámek zavolá memext_map_pwrite() ve vzestupném pořadí address
     * pointů (každé volání včetně záznamu hwlog a memory_reconnect_ram(),
     * jako OUT E7h):
     *  - LUFTNER: pointy 0..15,
     *  - PEHU: jen sudé pointy 0, 2, ..., 14 - jeden zápis na 8 KB pár.
     *    OUT E7h na lichý point HW zarovná na sudý, takže zápis hodnoty
     *    lichého pointu by přepsal celý pár; hodnoty lichých pointů se
     *    proto ignorují.
     *
     * Typ se vyhodnocuje až zde (emu vlákno), podle aktuálního
     * g_memext.type.
     *
     * @return true pokud byl požadavek čekající a provedl se, jinak false.
     *
     * @pre Volat jen z emu vlákna v bodě mezi instrukcemi (hlavní smyčka
     *      po mzarch_main_process_events() nebo paused smyčka), nebo
     *      v testu bez běžícího emu vlákna.
     */
    bool memext_map_request_process(void);

    /**
     * @brief Levná kontrola + provedení požadavku z memext_map_request().
     *
     * Bez požadavku jen jedno atomické čtení a větev. Volá hlavní smyčka
     * mzarch v per-event bloku a v paused smyčce (oba buildy, s debuggerem
     * i bez něj).
     *
     * @pre Stejné jako memext_map_request_process().
     */
    static inline void memext_map_request_poll(void)
    {
        if (g_atomic_int_get(&g_memext_map_request_pending))
            (void)memext_map_request_process();
    }

    uint8_t *memext_get_ram_read_pointer_by_addr_point(int addr_point);
    uint8_t *memext_get_ram_write_pointer_by_addr_point(int addr_point);

    /**
     * Vrátí read pointer pro raw bank index (= absolutní index v Memext map
     * tabulce, bez ohledu na aktuální banking stav).
     *
     * Bank index 0..0x7F (= MEMEXT_LUFTNER_BANKS - 1) ukazuje do g_memext.RAM,
     * index 0x80..0xFF do g_memext.FLASH (Luftner only - PEHU používá jen
     * dolní polovinu). Velikost banky = MEMEXT_RAW_BANK_SIZE (4 KB).
     *
     * Používá se pro debug API (Memory Browser, REGION_LIST) k iteraci přes
     * všechny banky bez ohledu na aktuální mapování. Direct array access,
     * žádný side-effect.
     *
     * @param rawbank   Raw bank index 0..0xFF.
     * @return          Pointer na začátek 4 KB banky v g_memext.RAM/FLASH.
     */
    uint8_t *memext_get_ram_read_pointer_by_rawbank(int rawbank);

    /**
     * Vrátí write pointer pro raw bank index. Pro FLASH banky (rawbank
     * & 0x80) vrací g_memext.WOM (write-only scratch, zápis se zahodí).
     * Pro RAM banky stejný jako read pointer.
     *
     * Pro debug API: zápis do FLASH banky cestou get_*_write_pointer skončí
     * ve WOM scratch a uživatel by viděl jen "co naposledy spadlo do
     * ztracena". Proto Memory Browser používá `writable=0` pro
     * REGION_KIND_MEMEXT_FLASH a tuto funkci nezná na write API.
     *
     * @param rawbank   Raw bank index 0..0xFF.
     * @return          Pointer na 4 KB write target (RAM bank nebo WOM).
     */
    uint8_t *memext_get_ram_write_pointer_by_rawbank(int rawbank);

    void memext_luftner_set_flash_filepath ( const char *filepath );

    /**
     * Vrátí offset v g_memext.RAM odpovídající danému pointeru, nebo -1
     * pokud pointer leží mimo Memext RAM (např. ukazuje do WOM, do
     * g_memory.RAM nebo je NULL).
     *
     * Používá se pro Memory Heatmap recording - hot-path získá pointer
     * z g_memory.memram_read/write[addr_point], a tato funkce ho převede
     * na flat offset 0..MEMEXT_RAM_SIZE-1.
     *
     * @param ptr  Pointer, typicky g_memory.memram_read[addr >> 12].
     * @return     Offset 0..MEMEXT_RAM_SIZE-1, nebo -1 mimo rozsah.
     */
    int32_t memext_get_ram_offset_from_pointer ( const uint8_t *ptr );

#ifdef __cplusplus
}
#endif

#endif /* MEMEXT_H */
