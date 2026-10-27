/**
 * @file   mz700_devbus.h
 * @brief  Adaptér sběrnice volitelných zařízení (devbus) pro jádro MZ-700.
 *
 * Adaptér drží jediné místo, odkud se berou "čisté" callbacky CPU MZ-700
 * (g_mz700_cpu_ops), a jediné místo volání z80_set_* (mz700_install_cpu_callbacks()).
 * Bez připojeného zařízení jsou v g_mz700_cpu_ops přímo dnešní funkce
 * (memory_write_cb, port_read_cb, port_write_cb, memory_write_byte_raw),
 * takže CPU volá stejnou strojovou funkci jako dřív. Zařízení s porty nebo
 * MMIO přepne příslušné callbacky na obálky (*_devbus); obálky volají
 * původní funkci beze změny a teprve pak dispečink devbus.
 *
 * Logovací varianty callbacků (debugger) a záznam zápisů (ramlog) volají
 * g_mz700_cpu_ops.* místo jména funkce, takže zařízení vidí i zápisy
 * a porty v ladicím režimu.
 *
 * @par Licence: GPLv3
 */

#ifndef MZ700_DEVBUS_H
#define MZ700_DEVBUS_H

#include <stdint.h>
#include <stdbool.h>

#include "libs/cpu-z80/z80.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Aktuální čisté callbacky CPU MZ-700.
 *
 * @invariant Žádný ukazatel není NULL. Zapisuje jen mz700_devbus_recompute()
 *            (emu vlákno mezi instrukcemi, nebo zastavená emulace).
 */
typedef struct st_MZ700_CPU_OPS {
    z80_mwrite_cb mwrite;                         /**< memory_write_cb | memory_write_cb_devbus */
    z80_pread_cb  pread;                          /**< port_read_cb | port_read_cb_devbus */
    z80_pwrite_cb pwrite;                         /**< port_write_cb | port_write_cb_devbus */
    void (*raw_write)(uint16_t addr, uint8_t v);  /**< memory_write_byte_raw | memory_write_byte_raw_devbus */
} st_MZ700_CPU_OPS;

/** @brief Aktuální čisté callbacky (viz st_MZ700_CPU_OPS). */
extern st_MZ700_CPU_OPS g_mz700_cpu_ops;

/**
 * @brief Nainstaluje callbacky CPU podle stavu debuggeru a g_mz700_cpu_ops.
 *
 * Jediné místo volání z80_set_mread/mwrite/pread/pwrite na MZ-700. Volá ho
 * změna stavu debuggeru (mzarch_platform_fn_debugger_state_changed()) a
 * adaptér devbus po každé změně háčků. Varianty: logovací (debugger
 * potřebuje ladicí callbacky), záznam zápisů (ramlog), čisté
 * (g_mz700_cpu_ops). V buildu bez debuggeru jen čisté.
 *
 * @pre Emu vlákno mezi instrukcemi, nebo zastavená emulace (load snapshotu);
 *      g_mzarch_main.cpu existuje.
 * @post CPU volá callbacky odpovídající aktuálnímu stavu.
 */
void mz700_install_cpu_callbacks(void);

/**
 * @brief Obálka port_read_cb() pro porty zařízení devbus (mz700_iorq.c).
 *
 * Port bez zařízení (g_devbus_ports[].count == 0) předá port_read_cb()
 * beze změny. Port zařízení: nejdřív statický switch port_read_cb()
 * (insideop; zda deska budila, říká příznak UNCONNECTED po návratu), pak
 * rezoluce čtení devbus_port_read_resolve() přes všechny vlastníky portu
 * s hodnotou nebuzené sběrnice platformy. Značka pro trace: HANDLED, když
 * někdo budil, jinak UNCONNECTED; kolize značí adaptér (CONFLICT).
 *
 * @param cpu CPU.
 * @param addr Adresa portu (dekód podle dolního bajtu).
 * @param user_data Nepoužito.
 * @return Přečtená hodnota.
 * @pre Emu vlákno uvnitř instrukce (callback CPU) nebo dbgapi.
 */
uint8_t port_read_cb_devbus(z80_t *cpu, uint16_t addr, void *user_data);

/**
 * @brief Obálka port_write_cb() pro porty zařízení devbus (mz700_iorq.c).
 *
 * Vždy nejdřív statický switch port_write_cb() (deska první, insideop),
 * pak u portu se zařízením broadcast devbus_port_write_all() všem
 * vlastníkům v pořadí připojení (i těm, kteří sběrnici nebudí) a značka
 * "obslouženo".
 *
 * @param cpu CPU.
 * @param addr Adresa portu.
 * @param value Zapisovaný bajt.
 * @param user_data Nepoužito.
 * @pre Jako port_read_cb_devbus().
 */
void port_write_cb_devbus(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data);

/**
 * @brief Obálka memory_write_cb() se snoopem MMIO stránky 0E000h (mz700_memory.c).
 *
 * Nejdřív memory_write_cb() beze změny (deska), pak u adres 0E000h-0EFFFh
 * devbus_mmio_dispatch() se stavem mapy (MAPPED / DRAM), příznakem
 * Prohibited a DEVBUS_ACC_CPU. Mapu stránky mění jen OUT, ne zápis do
 * paměti, takže stav zjištěný po zápisu platí i pro okamžik zápisu.
 *
 * @param cpu CPU.
 * @param addr Adresa.
 * @param value Zapisovaný bajt.
 * @param user_data Nepoužito.
 * @pre Emu vlákno uvnitř instrukce.
 */
void memory_write_cb_devbus(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data);

/**
 * @brief Obálka memory_write_byte_raw() (zápis debuggeru) se snoopem MMIO.
 *
 * Jako memory_write_cb_devbus(), ale s DEVBUS_ACC_DEBUGGER (bez
 * synchronizace času).
 *
 * @param addr Adresa.
 * @param value Zapisovaný bajt.
 * @pre Emu vlákno.
 */
void memory_write_byte_raw_devbus(uint16_t addr, uint8_t value);

/**
 * @brief Inicializuje devbus s adaptérem MZ-700 (prázdný registr).
 * @pre Emu vlákno v mzarch_platform_fn_init() po vytvoření CPU, před
 *      registrací zařízení.
 */
void mz700_devbus_init(void);

/**
 * @brief Zaregistruje zařízení MZ-700 (PCG-700, SUC 80 sloupců, karta K&P 80)
 *        a ukončí registraci: devbus_start() (připojení podle INI).
 *
 * Registruje v tomto pořadí: PCG-700 (pcg700_devbus_register()),
 * SUC 80 sloupců (suc80_devbus_register()), karta K&P 80
 * (kp80_devbus_register()).
 * @pre Emu vlákno na konci mzarch_platform_fn_init(), po memory_init()
 *      (CG-ROM je nainstalovaná, připojení z INI z ní staví generátor).
 */
void mz700_devbus_start(void);

/**
 * @brief Ukončí devbus (devbus_exit()) bez překreslování obrazu.
 *
 * Odpojení zařízení s video háčkem by v pauze překreslilo plátno; při
 * ukončení emulátoru už video rozhraní nemusí existovat.
 *
 * @pre Emu vlákno v mzarch_platform_fn_exit() (po cfgmain_exit(), INI je uložené).
 * @post Kreslení řádku je výchozí, registr devbus prázdný.
 */
void mz700_devbus_exit(void);

/**
 * @brief Je statický vlastník druhu @p kind (en_MZ700_BUS_PRESENT) právě přítomný?
 *
 * Lepidlo tabulky mz700_bus_owners.c pro vrstvu bus_map: čte stav
 * periferií (Unicard, FDC, IDE8, RAM disk, Quick Disk, MemExt, CMT hack).
 *
 * @param kind Druh přítomnosti (en_MZ700_BUS_PRESENT).
 * @return true = na sběrnici je; neznámý druh = false.
 * @pre Emu vlákno, nebo UI pod device_state_lock() (jen čtení příznaků).
 */
bool mz700_bus_owner_present(unsigned kind);

/**
 * @brief Je adresa sledovaná oknem I/O a Event Viewerem jako MMIO zápis?
 *
 * 0E000h-0E008h vždy (8255, 8253, GDG), navíc MMIO rozsahy připojených
 * zařízení v okně 0E000h-0E0FFh (bitmapa, přepočítá ji změna háčků).
 * Jen ladicí cesta.
 */
bool mz700_devbus_mmio_tracked(uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* MZ700_DEVBUS_H */
