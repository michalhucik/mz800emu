/*
 * io_catalog.h - Statický katalog I/O portů MZ-800 / MZ-700 / MZ-1500
 *
 * Datový model pro UI panel "I/O Ports" (D.7) - bit-by-bit popis
 * jednotlivých registrů (no$gba IO Map style). Katalog popisuje
 * adresy portů, jejich směr (R/W/RW), význam jednotlivých bitů
 * a callback pro live čtení aktuální hodnoty z g_* state.
 *
 * Data zde uvedená jsou compile-time konstanty (= žádné dynamické
 * loading). Zdroj informací: ~/projects/mz800-knowledge/public/reference/
 * agent/hw/04-io-ports.md a souvisejicí HW reference.
 *
 * UPOZORNĚNÍ: Knowledge base (= ground truth) říká:
 *   - 8255 PPI je na 0D0h-0D3h (NE na 0FCh-0FFh, jak píše některé
 *     starší materiály)
 *   - 8253 CTC je na 0D4h-0D7h
 *   - 0F4h-0F7h je Z80 SIO (Quick Disk), ne CTC ani Z80 PIO
 *   - 0FCh-0FFh je Z80 PIO (tiskárna + 2 vstupní signály)
 *
 * GDG je write-only (kromě Status na 0CEh IN). Hodnoty WF/RF/DMD/BOR
 * apod. čteme z internal mirror state v g_gdg / g_vramctrl.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef IO_CATALOG_H
#define IO_CATALOG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

    /**
     * Směr přístupu k I/O portu.
     *
     * Hodnoty určují, jak se port chová z pohledu CPU:
     *   - IO_PORT_DIR_R  - jen čtení (IORQ IN)
     *   - IO_PORT_DIR_W  - jen zápis (IORQ OUT)
     *   - IO_PORT_DIR_RW - obě operace (na téže adrese typicky různý
     *                     význam - viz GDG status vs DMD na 0CEh)
     */
    typedef enum
    {
        IO_PORT_DIR_R = 0,
        IO_PORT_DIR_W,
        IO_PORT_DIR_RW
    } en_IO_PORT_DIR;

    /**
     * Cross-window cíl pro context menu "Show in ..." akci v I/O Ports
     * Overview tabu.
     *
     * Hodnoty:
     *   - IO_CROSS_NONE        - port nemá související debug okno
     *                            (default při zero-fill initialization);
     *                            UI context menu nezobrazí "Show in ..."
     *                            položku.
     *   - IO_CROSS_MEMORY_MAP  - port souvisí s aktuálním banking /
     *                            memory mapping state (= 0xE0-0xE7);
     *                            UI nabídne "Show in Memory Map".
     *
     * Pozn.: Field je u8 a je na konci struct st_IO_PORT_DESC, tj.
     * existující inicializátory v io_catalog.c implicitně zero-fillují
     * = stávající entries dostanou IO_CROSS_NONE bez touche.
     */
    typedef enum
    {
        IO_CROSS_NONE = 0,
        IO_CROSS_MEMORY_MAP = 1
    } en_IO_CROSS_TARGET;

    /**
     * Popis jednoho bitu nebo bitového pole v registru.
     *
     * Pro single-bit zápis: bit_count == 1, bit_index = 0..7.
     * Pro bit-field: bit_count > 1, bit_index = LSB pozice. Hodnota
     * pole se získá maskou (((value >> bit_index) & ((1<<bit_count)-1))).
     *
     * label  = krátký název bitu / pole (max ~12 znaků pro UI layout)
     * desc   = jednořádkový popis (max ~80 znaků)
     */
    typedef struct st_IO_BIT_DESC
    {
        uint8_t bit_index;       /**< LSB index (0..7) */
        uint8_t bit_count;       /**< 1 = single bit, >1 = bit-field */
        const char *label;       /**< Krátký label (např. "DMD2", "VBANK") */
        const char *description; /**< Jednořádkový popis */
    } st_IO_BIT_DESC;

    /**
     * Popis jednoho I/O portu v katalogu.
     *
     * read_value:
     *   Pointer na funkci, která vrátí AKTUÁLNÍ hodnotu portu z internal
     *   state emulátoru. Implementace musí být side-effect free (= jen
     *   přečíst registr g_*.field, ne procházet I/O dispatch path -
     *   některé porty mají side-effecty na real read, např. PSG/FDC).
     *
     *   Pokud je hodnota nedostupná (write-only registr bez mirror),
     *   vrací 0 a UI zobrazí "??".
     *
     * decode:
     *   Volitelný (NULL = nepoužije se). Pro vícebitová pole vrací
     *   string s human-readable interpretací (např. DMD = 02h -> "320x200x16").
     *   Vrácený řetězec musí být statický nebo thread-local; UI ho jen
     *   přečte, neuvolňuje.
     *
     * available_for_mzarch:
     *   Bitmaska MZ_AVAIL_* (viz makra níže) - které architektury port mají.
     *   Pokud je 0, předpokládá se "vždy dostupné".
     *
     * cross_target:
     *   Volitelný (IO_CROSS_NONE = nepoužije se). Hint UI vrstvě, do
     *   kterého debug okna patří související state (viz en_IO_CROSS_TARGET).
     *   Stávající entries bez explicitní hodnoty dostanou IO_CROSS_NONE
     *   přes zero-fill (= žádná regrese).
     */
    typedef struct st_IO_PORT_DESC
    {
        uint16_t addr;                         /**< Adresa portu (typicky 8bit, ale 16bit IORQ existuje pro GDG scroll) */
        const char *name;                      /**< Krátký název (např. "GDG WMD") */
        const char *long_name;                 /**< Delší popis pro tooltip */
        en_IO_PORT_DIR direction;              /**< R / W / RW */
        const st_IO_BIT_DESC *bits;            /**< Pole bitových popisů, nebo NULL */
        size_t bit_count;                      /**< Počet záznamů v bits[] */
        uint8_t (*read_value)(void);           /**< Live reader (= side-effect free) */
        const char *(*decode)(uint8_t value);  /**< Volitelný value decoder */
        uint8_t available_for_mzarch;          /**< Bitmaska MZ_AVAIL_* */
        uint8_t cross_target;                  /**< en_IO_CROSS_TARGET hodnota (default IO_CROSS_NONE přes zero-fill) */
    } st_IO_PORT_DESC;

/* Bitmasky pro available_for_mzarch */
#define MZ_AVAIL_700   (1u << 0)
#define MZ_AVAIL_800   (1u << 1)
#define MZ_AVAIL_1500  (1u << 2)
#define MZ_AVAIL_ALL   (MZ_AVAIL_700 | MZ_AVAIL_800 | MZ_AVAIL_1500)

    /**
     * Globální katalog portů - statické pole zakončené sentinelem
     * (addr == 0xFFFF, name == NULL).
     *
     * Pořadí v poli definuje pořadí zobrazení v UI (typicky vzestupně
     * podle adresy, s výjimkami pro skupiny: GDG bloky pohromadě,
     * banking pohromadě atd.).
     */
    extern const st_IO_PORT_DESC g_io_ports[];

    /**
     * Počet záznamů v g_io_ports[] (mimo sentinel).
     */
    extern const size_t g_io_ports_count;

    /**
     * Side-effect-free probe portu přes mirror gettery z `g_io_ports[]`.
     *
     * Implementace lineárně skenuje celý katalog (io_catalog_at()) a hledá entry, který
     * splňuje VŠECHNY podmínky:
     *   0. položka je přítomná (io_catalog_present(): statická vždy, položka
     *      zařízení devbus jen s připojeným zařízením) a patří platformě, pro
     *      kterou je modul přeložen (`available_for_mzarch` == 0 nebo obsahuje
     *      bit MZ_AVAIL_* podle MZARCH; stejný filtr jako okno I/O Ports)
 *   1. `(entry->addr & 0xFF) == port_lsb` (= shoda na low byte adresy;
     *      MMIO entries s nenulovým high byte se přeskakují - ty patří do
     *      memory map space, ne IORQ portů)
     *   2. `entry->direction == IO_PORT_DIR_R || IO_PORT_DIR_RW` (= read-capable;
     *      write-only entries se přeskakují, např. GDG palette write na 0xF0)
     *   3. `entry->read_value != NULL` (= je registrovaný mirror callback;
     *      `read_value=NULL` entries se přeskakují - např. write-only registry
     *      bez mirror nebo porty kde mirror nedává smysl - 0xDB FDC data)
     *
     * Pokud entry najde, zavolá `read_value()` a uloží výsledek do `*out_value`,
     * vrátí nenulovou hodnotu. Jinak vrátí 0 (caller obvykle fallback na bus
     * latch).
     *
     * Použití: side-effect-free probe path `port_read_no_se_cb` v mzarch
     * vrstvě (per arch) volá tento helper jako prvotní krok; `regDBUS_latch`
     * fallback se použije jen pokud helper nevrátil mirror.
     *
     * Omezení: filtr je podle platformy, ne podle stavu stroje. Položka
     * přítomná na platformě vrací zrcadlo i tehdy, když ji port v aktuálním
     * režimu nebo bez připojené periferie nedekóduje (např. 8255 na 0D0h
     * MZ-800 v režimu MZ-700, FDC bez řadiče); kdo potřebuje "co by dostalo
     * CPU", kombinuje probe s bus_map_static_port_driven().
     *
     * Performance: lineární scan, není určeno do CPU hot path; volá se z BP
     * DSL `port[N]` probe, což je rate-limited debug operace.
     *
     * @param port_lsb Low byte port adresy (0..255).
     * @param out_value Out parameter pro mirror hodnotu (zapíše se jen
     *                  pokud návratová hodnota != 0).
     * @return Nenulová hodnota pokud entry s mirrorem nalezen, 0 jinak.
     */
    extern int io_catalog_probe_byte(uint8_t port_lsb, uint8_t *out_value);

    /**
     * @brief Počet položek celého katalogu: statické g_io_ports[] + položky
     *        registrovaných zařízení devbus.
     *
     * Registr devbus je po devbus_start() neměnný, takže počet se za běhu
     * nemění a smí ho číst libovolné vlákno. Položky zařízení jsou statická
     * data zařízení; zda je zařízení připojené, říká io_catalog_present().
     *
     * @return Počet položek (indexy 0 .. count - 1 pro io_catalog_at()).
     */
    extern size_t io_catalog_count(void);

    /**
     * @brief Položka katalogu podle indexu.
     *
     * Indexy 0 .. g_io_ports_count - 1 = statický katalog (beze změny pořadí),
     * dál položky zařízení devbus v pořadí registrace.
     *
     * @param i Index 0 .. io_catalog_count() - 1.
     * @return Položka (statická), nebo NULL při indexu mimo rozsah.
     */
    extern const st_IO_PORT_DESC *io_catalog_at(size_t i);

    /**
     * @brief Je položka katalogu přítomná (statická vždy, položka zařízení
     *        jen s připojeným zařízením)?
     *
     * Atomické čtení stavu připojení; UI vlákno ho volá bez zámku.
     *
     * @param i Index 0 .. io_catalog_count() - 1.
     * @return false i při indexu mimo rozsah.
     */
    extern bool io_catalog_present(size_t i);

    /**
     * Krátký anglický popis grafického režimu GDG MZ-800 podle hodnoty DMD
     * (port 0xCE, zápis). Společný pro katalog portů, Event Viewer a okno I/O.
     *
     * Bity DMD: 0 = obraz A/B (VBANK), 1 = HICOLOR, 2 = SCRW640, 3 = MZ-700.
     * Bity 4-7 GDG ignoruje. Při bitu 3 = 1 jsou bity 0-2 bez účinku
     * (09h-0Fh se chovají jako 08h, nedokumentované). 03h a 07h jsou
     * nedokumentované režimy. Zdroj: báze mz800-knowledge, `mz-800.txt`
     * (DMD) a `hw/09a-undoc-dmd-modes.md` (ověřeno simulací netlistu GDG).
     *
     * | DMD      | Popis                 |
     * |----------|-----------------------|
     * | 00h      | `320x200x4 A`         |
     * | 01h      | `320x200x4 B`         |
     * | 02h      | `320x200x16`          |
     * | 03h      | `320x200x16 undoc`    |
     * | 04h      | `640x200x2 A`         |
     * | 05h      | `640x200x2 B`         |
     * | 06h      | `640x200x4`           |
     * | 07h      | `640x200x4 undoc`     |
     * | 08h      | `MZ-700 mode`         |
     * | 09h-0Fh  | `MZ-700 mode (undoc)` |
     *
     * Bez vedlejších efektů, volatelné z libovolného vlákna.
     *
     * @param dmd Hodnota DMD (bity 4-7 se ignorují).
     * @return Statický řetězec (volající neuvolňuje), nikdy NULL.
     */
    extern const char *io_catalog_dmd_mode_name(uint8_t dmd);

#ifdef __cplusplus
}
#endif

#endif /* IO_CATALOG_H */
