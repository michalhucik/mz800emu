/*
 * dbgapi_regions.c - společné jádro REGION_LIST API
 *
 * Implementuje veřejné funkce z dbgapi_regions.h:
 *   - dbgapi_regions_enumerate(): dispatch na per-arch <arch>_regions_collect()
 *     a cache snapshot pro následující read/write.
 *   - dbgapi_regions_read(): no-side-effect read; per-kind dispatch.
 *   - dbgapi_regions_write(): safe write; respekt writable/connected flagů.
 *
 * Cache snapshot:
 *   - s_region_cache[] drží poslední enumerate výsledek
 *   - s_region_cache_count = počet validních záznamů
 *   - read/write hledá podle id v cache (= O(1) lookup, id == index v cache)
 *
 * Thread safety:
 *   - CMDRQ serializace kryje jen emu vlákno (dbgapi handlery, freeze).
 *     Membrowser okna (membrowser_io.cpp, membrowser_diff_window.cpp) ale
 *     volají dbgapi_regions_read/enumerate PŘÍMO z render smyčky UI
 *     vlákna - souběh s re-enumerate na emu vlákně je tedy možný.
 *   - s_region_cache[] proto chrání s_region_cache_mutex; read/write si
 *     pod zámkem pořídí by-value kopii deskriptoru (struktura nemá
 *     pointery) a vlastní přístup k datům běží už bez zámku, aby se
 *     nedržel přes per-kind dispatch (žádné lock-order riziko s device
 *     zámky). Souběžné čtení živé paměti vs. běžící emulace je u live
 *     views záměrné (no-side-effect snapshot bez pauzy).
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
#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <glib.h>

#include "dbgapi_regions.h"
#include "dbgapi_regions_arch.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/ramdisk/ramdisk.h"

#if MZARCH == 800
#include "mzarch/mz800/memory/mz800_memory.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/memory/mz1500_memory.h"
#elif MZARCH == 700
#include "mzarch/mz700/memory/mz700_memory.h"
#endif


/* ============================================================================
 * Cache poslední enumerate výsledek
 *
 * Maximální počet regionů = LOGICAL + RAM + 2× ROM + CGROM + 2× CGRAM/700 +
 * 2× VRAM_700 + 4× VRAM_PLANE + 3× PCG + 2× 128 Memext + 256 RAMDISK_STD +
 * 2× 8 PEZIK + PROHIBITED. Worst case (MZ-800 Luftner + STD 16M + 2× PEZIK):
 *   1 + 1 + 2 + 1 + 1 + 2 + 4 + 256 + 256 + 16 + 1 = ~541 regionů.
 * Zaokrouhlujeme na 1024 pro bezpečnost.
 * ============================================================================ */

#define REGION_CACHE_MAX 1024

static st_REGION_DESC s_region_cache[REGION_CACHE_MAX];
static int s_region_cache_count = 0;

/**
 * @brief Zámek nad s_region_cache[] a s_region_cache_count.
 *
 * Statická GMutex (zero-init je dle GLib validní stav). Drží se jen po
 * dobu plnění cache / kopie deskriptoru - NIKDY přes per-kind přístup
 * k datům (viz Thread safety v hlavičce souboru).
 */
static GMutex s_region_cache_mutex;


/* ============================================================================
 * Per-arch dispatch
 * ============================================================================ */

static int arch_regions_collect(st_REGION_DESC *out, int max_count)
{
#if MZARCH == 800
    return mz800_regions_collect(out, max_count);
#elif MZARCH == 1500
    return mz1500_regions_collect(out, max_count);
#elif MZARCH == 700
    return mz700_regions_collect(out, max_count);
#else
#error "Unsupported MZARCH for dbgapi_regions"
#endif
}


/* ============================================================================
 * Veřejné API
 * ============================================================================ */

/* Helper: naplní cache z per-arch collect. Volat POUZE pod
 * s_region_cache_mutex. */
static void region_cache_fill_locked(void)
{
    s_region_cache_count = arch_regions_collect(s_region_cache, REGION_CACHE_MAX);
    if (s_region_cache_count > REGION_CACHE_MAX) {
        s_region_cache_count = REGION_CACHE_MAX;
    }
}


int dbgapi_regions_enumerate(st_REGION_DESC *out, int max_count)
{
    if (!out || max_count <= 0) return -1;

    /* Naplň cache (= internal buffer). Z cache pak zkopíruj do `out`
     * limit max_count. Tím vyhneme race kdyby caller poslal menší out
     * než výsledný count - cache si pamatuje VŠECHNY. */
    g_mutex_lock(&s_region_cache_mutex);
    region_cache_fill_locked();

    int n = (s_region_cache_count < max_count) ? s_region_cache_count : max_count;
    memcpy(out, s_region_cache, n * sizeof(st_REGION_DESC));
    g_mutex_unlock(&s_region_cache_mutex);
    return n;
}


/* Helper: pod zámkem pořídí by-value kopii deskriptoru z cache podle id
 * do @p out_desc. Vrací false = invalid id. Struktura nemá pointery,
 * kopie je tedy plnohodnotný snapshot a volající s ní pracuje už bez
 * zámku (viz Thread safety v hlavičce souboru).
 *
 * Lazy auto-enumerate: pokud cache ještě nebyla naplněná (= klient zavolal
 * region_read/write bez předchozího regions_list a bez otevřeného Memory
 * Browseru, který enumerate spustí), naplň ji teď. region_id je index do
 * enumerate výsledku, takže read/write je tím self-sufficient a nevyžaduje
 * explicitní enumerate-first (mzdos 0012). Re-enumerace při už naplněné
 * cache se NEdělá - ID zůstávají stabilní v rámci session (re-enumerate je
 * na dbgapi_regions_enumerate / regions_list). */
static bool lookup_region_copy(int region_id, st_REGION_DESC *out_desc)
{
    bool found = false;
    g_mutex_lock(&s_region_cache_mutex);
    if (s_region_cache_count <= 0) {
        region_cache_fill_locked();
    }
    if (region_id >= 0 && region_id < s_region_cache_count) {
        *out_desc = s_region_cache[region_id];
        found = true;
    }
    g_mutex_unlock(&s_region_cache_mutex);
    return found;
}


/* Helper: clamp offset/len proti velikosti regionu. Vrací použitelný len. */
static uint32_t clamp_len(const st_REGION_DESC *desc, uint32_t offset, uint32_t len)
{
    if (offset >= desc->size) return 0;
    uint32_t avail = desc->size - offset;
    return (len < avail) ? len : avail;
}


/* ============================================================================
 * READ per kind
 * ============================================================================ */

#if MZARCH == 800
/* Pomocný: získá raw pointer na VRAM plane (sub_id 0..3). Pro plane III/IV
 * vrací EXVRAM pointer. */
static uint8_t *vram_plane_pointer(int sub_id)
{
    switch (sub_id) {
        case 0: return g_memory.VRAM;                          /* I */
        case 1: return &g_memory.VRAM[MEMORY_SIZE_VRAM_BANK];  /* II */
        case 2: return g_memory.EXVRAM;                        /* III */
        case 3: return &g_memory.EXVRAM[MEMORY_SIZE_VRAM_BANK];/* IV */
        default: return NULL;
    }
}
#endif


int dbgapi_regions_read(int region_id, uint32_t offset, uint8_t *out, uint32_t len)
{
    if (!out || len == 0) return -1;
    st_REGION_DESC desc_copy;
    if (!lookup_region_copy(region_id, &desc_copy)) return -1;
    const st_REGION_DESC *desc = &desc_copy;
    if (!desc->connected) return -1;

    uint32_t n = clamp_len(desc, offset, len);
    if (n == 0) return 0;

    switch (desc->kind) {

    case REGION_KIND_LOGICAL: {
        /* Per-arch banking-aware no-se read přes memory_read_byte_no_se. */
        for (uint32_t i = 0; i < n; i++) {
            out[i] = memory_read_byte_no_se((uint16_t)(offset + i));
        }
        return (int)n;
    }

    case REGION_KIND_RAM: {
        /* User RAM region = plná 64 KB uživatelská RAM jak by ji viděl
         * CPU, kdyby všude byla namapovaná RAM (žádné ROM, žádné VRAM).
         * Banking ROM/VRAM je v tomto pohledu transparentní - region
         * vrací podkladovou User RAM, ne to, co aktuálně leží na sběrnici.
         *
         * S Memextem se mění jen to, která memextí buňka je vidět ve
         * které části RAM. View je jinak ekvivalentní s "Logical Z80
         * pohledem za předpokladu, že by všude byla RAM".
         *
         * Implementace přes g_memory.memram_write[] indirekci:
         *
         *   - Bez Memext: memram_write[i] = &g_memory.RAM[i << 12]
         *                 (identita - User RAM = fyzická DRAM).
         *   - S Memextem (RAM bank): memram_write[i] = pointer do
         *                 memext_get_ram_write_pointer_by_addr_point(i)
         *                 = stejná RAM bank do které by CPU zapisoval.
         *   - S Memextem (FLASH bank u Luftneru): memram_write[i] = WOM
         *                 scratch. Read pak vrátí WOM obsah, což je
         *                 konzistentní s "co by tam bylo, kdyby CPU psal
         *                 jako do RAM" - FLASH read přes memram_read[]
         *                 by vrátil obsah FLASH, ale to je ROM-like cíl,
         *                 který sem nepatří (transparentní vůči User RAM
         *                 view).
         *
         * Rozdíl proti memram_read[]: u ROM-mapnutých stránek v
         * MZ-800 (např. 0x0000-0x0FFF Monitor ROM nebo 0xE000-0xFFFF
         * upper ROM) by memram_read mohl ukazovat na ROM - ale ve
         * skutečnosti to neukazuje, protože ROM swap je řízený inline
         * makry v memory_read_byte (memory_internal_read_*), ne přes
         * memram_read[] pointer. Banking init nastavuje
         * memram_read[i] = memram_write[i] = &g_memory.RAM[i<<12].
         * Reálný rozdíl read vs write tabulka se projeví jen u Memext
         * FLASH bank (read = FLASH, write = WOM). Pro User RAM
         * semantiku tedy chceme memram_write.
         *
         * Side-effect free (= raw pointer, žádný BP/CDL/watch fire).
         *
         * Per-byte mapping: bank = offset >> 12, in_bank = offset & 0xFFF.
         */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = offset + i;
            out[i] = g_memory.memram_write[(a >> 12) & 0xF][a & 0x0FFF];
        }
        return (int)n;
    }

    case REGION_KIND_ROM_LOWER:
        memcpy(out, &g_memory.ROM[offset], n);
        return (int)n;

    case REGION_KIND_ROM_UPPER:
        /* ROM layout v g_memory.ROM: 0x0000 lower (4 KB), 0x1000 cgrom (4 KB),
         * 0x2000 upper (8 KB). */
        memcpy(out, &g_memory.ROM[ROM_SIZE_0000 + ROM_SIZE_CGROM + offset], n);
        return (int)n;

    case REGION_KIND_CGROM:
        memcpy(out, &g_memory.ROM[ROM_SIZE_0000 + offset], n);
        return (int)n;

#if MZARCH == 800
    case REGION_KIND_CGRAM_700:
        /* MZ-800 CG-RAM (4 KB) v MZ-700 compat módu = první 4 KB Plane I
         * (= g_memoryVRAM_I[0x0000..0x0FFF]). Ověřeno proti
         * mz800_memory.c:1099 kde write z 0xC000 jde na
         * g_memoryVRAM_I[addr & 0x0FFF]. Plane I je g_memory.VRAM[0..0x1FFF],
         * tj. offset 0 v VRAM = začátek CG-RAM. */
        memcpy(out, &g_memory.VRAM[offset], n);
        return (int)n;

    case REGION_KIND_VRAM_700_CHAR:
        /* MZ-800 v 700 compat módu: char VRAM = Plane I offset 0x1000..0x17FF.
         * Ověřeno proti mz800_memory.c:1105 kde write z 0xD000 jde na
         * g_memoryVRAM_I[0x1000 | (addr & 0x0FFF)]. Bug pre-fix četl offset
         * 0 = začátek CG-RAM (= chybný obsah). */
        memcpy(out, &g_memory.VRAM[0x1000 + offset], n);
        return (int)n;

    case REGION_KIND_VRAM_700_ATTR:
        /* MZ-800 v 700 compat módu: attr VRAM = Plane I offset 0x1800..0x1BFF.
         * Stejná logika jako CHAR + posun o 0x0800 (znaková VRAM má 1 KB
         * data + 1 KB padding do 0x800-0xFFF; attr leží za nimi).
         * Bug pre-fix četl offset 0x0800 = uvnitř CG-RAM (= chybný obsah). */
        memcpy(out, &g_memory.VRAM[0x1800 + offset], n);
        return (int)n;

    case REGION_KIND_VRAM_PHYS_PLANE: {
        uint8_t *p = vram_plane_pointer(desc->sub_id);
        if (!p) return -1;
        memcpy(out, &p[offset], n);
        return (int)n;
    }
#endif /* MZARCH == 800 */

#if MZARCH == 700
    case REGION_KIND_VRAM_700_CHAR:
        memcpy(out, &g_memory.VRAM[offset], n);
        return (int)n;

    case REGION_KIND_VRAM_700_ATTR:
        memcpy(out, &g_memory.VRAM[0x0800 + offset], n);
        return (int)n;
#endif /* MZARCH == 700 */

#if MZARCH == 1500
    case REGION_KIND_VRAM_700_CHAR:
        memcpy(out, &g_memory.VRAM[offset], n);
        return (int)n;

    case REGION_KIND_VRAM_700_ATTR:
        memcpy(out, &g_memory.VRAM[0x0800 + offset], n);
        return (int)n;

    case REGION_KIND_PCG_1500: {
        /* sub_id 0..2 = bank 1..3, každá 8 KB. */
        if (desc->sub_id < 0 || desc->sub_id > 2) return -1;
        memcpy(out,
            &g_memory.PCG[desc->sub_id * MEMORY_SIZE_PCG_BANK + offset], n);
        return (int)n;
    }
#endif /* MZARCH == 1500 */

    case REGION_KIND_MEMEXT_RAM:
    case REGION_KIND_MEMEXT_FLASH: {
        /* Direct array access přes get_*_by_rawbank helper. No side-effect. */
        if (!MEMEXT_TEST_CONNECTED) return -1;
        uint8_t *p = memext_get_ram_read_pointer_by_rawbank(desc->sub_id);
        if (!p) return -1;
        memcpy(out, &p[offset], n);
        return (int)n;
    }

    case REGION_KIND_RAMDISK_STD: {
        /* Direct array access do g_ramdisk.std.memory.
         * Nelze použít ramdisk_std_read_byte() - má side-effect na offset
         * cursor (auto-inc). */
        if (!g_ramdisk.std.connected || !g_ramdisk.std.memory) return -1;
        uint32_t abs_off = (uint32_t)desc->sub_id * 0x10000u + offset;
        memcpy(out, &g_ramdisk.std.memory[abs_off], n);
        return (int)n;
    }

    case REGION_KIND_RAMDISK_PEZIK: {
        /* sub_id encoding: pezik_instance * 8 + bank_idx (0..15).
         *   instance 0 = RAMDISK_PEZIK_68 (sub_id 0..7)
         *   instance 1 = RAMDISK_PEZIK_E8 (sub_id 8..15)
         * Na MZ-700 a MZ-1500 enumerace dodává jen sub_id 0..7 (PEZIK 0xE8
         * není v IORQ namapován). Velikost region = 64 KB (jedna bank z 8). */
        if (desc->sub_id < 0 || desc->sub_id > 15) return -1;
        int pi = desc->sub_id / 8;
        int bank = desc->sub_id % 8;
        if (!g_ramdisk.pezik[pi].connected) return -1;
        if (!g_ramdisk.pezik[pi].memory) return -1;
        uint32_t abs_off = (uint32_t)bank * 0x10000u + offset;
        memcpy(out, &g_ramdisk.pezik[pi].memory[abs_off], n);
        return (int)n;
    }

    case REGION_KIND_PROHIBITED_SHADOW:
        /* Virtuální region - vždy vrací konstantní shadow byte. MZ-800 = 0x1A,
         * MZ-700 = 0xFF. Dispatch dle MZARCH. */
#if MZARCH == 800
        memset(out, 0x1A, n);
#elif MZARCH == 700
        memset(out, 0xFF, n);
#else
        memset(out, 0xFF, n);
#endif
        return (int)n;

    default:
        return -1;
    }
}


/* ============================================================================
 * WRITE per kind
 * ============================================================================ */

int dbgapi_regions_write(int region_id, uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return -1;
    st_REGION_DESC desc_copy;
    if (!lookup_region_copy(region_id, &desc_copy)) return -1;
    const st_REGION_DESC *desc = &desc_copy;
    if (!desc->connected) return -1;
    if (!desc->writable) return -1;

    uint32_t n = clamp_len(desc, offset, len);
    if (n == 0) return 0;

    switch (desc->kind) {

    case REGION_KIND_LOGICAL: {
        /* Banking-aware write přes memory_write_byte. POZOR: může mít
         * side-effecty (např. VRAM write hits GDG WF dispatch). Pro VRAM
         * editaci v pause modu preferovat fyzický plane region. */
        for (uint32_t i = 0; i < n; i++) {
            memory_write_byte((uint16_t)(offset + i), data[i]);
        }
        return (int)n;
    }

    case REGION_KIND_RAM: {
        /* Write pres memram_write[] indirection - symetricky s READ.
         * Bez Memext rovny memcpy do g_memory.RAM[]; s Memext jde do
         * skutecne aktivni RAM bank ktera je viditelna CPU. */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = offset + i;
            g_memory.memram_write[(a >> 12) & 0xF][a & 0x0FFF] = data[i];
        }
        return (int)n;
    }

#if MZARCH == 800
    case REGION_KIND_CGRAM_700:
        /* CG-RAM (4 KB) v MZ-700 compat módu = Plane I offset 0..0x0FFF. */
        memcpy(&g_memory.VRAM[offset], data, n);
        return (int)n;

    case REGION_KIND_VRAM_700_CHAR:
        /* Char VRAM v MZ-700 compat módu = Plane I offset 0x1000..0x17FF.
         * Viz read branch. */
        memcpy(&g_memory.VRAM[0x1000 + offset], data, n);
        return (int)n;

    case REGION_KIND_VRAM_700_ATTR:
        /* Attr VRAM v MZ-700 compat módu = Plane I offset 0x1800..0x1BFF. */
        memcpy(&g_memory.VRAM[0x1800 + offset], data, n);
        return (int)n;

    case REGION_KIND_VRAM_PHYS_PLANE: {
        uint8_t *p = vram_plane_pointer(desc->sub_id);
        if (!p) return -1;
        memcpy(&p[offset], data, n);
        return (int)n;
    }
#endif

#if MZARCH == 700
    case REGION_KIND_VRAM_700_CHAR:
        memcpy(&g_memory.VRAM[offset], data, n);
        return (int)n;

    case REGION_KIND_VRAM_700_ATTR:
        memcpy(&g_memory.VRAM[0x0800 + offset], data, n);
        return (int)n;
#endif

#if MZARCH == 1500
    case REGION_KIND_VRAM_700_CHAR:
        memcpy(&g_memory.VRAM[offset], data, n);
        return (int)n;

    case REGION_KIND_VRAM_700_ATTR:
        memcpy(&g_memory.VRAM[0x0800 + offset], data, n);
        return (int)n;

    case REGION_KIND_PCG_1500:
        if (desc->sub_id < 0 || desc->sub_id > 2) return -1;
        memcpy(&g_memory.PCG[desc->sub_id * MEMORY_SIZE_PCG_BANK + offset],
               data, n);
        return (int)n;
#endif

    case REGION_KIND_MEMEXT_RAM: {
        /* RAM bank: write přes raw write pointer (vrátí přímo do RAM banky).
         * FLASH ji nikdy nevolá - REGION_KIND_MEMEXT_FLASH má writable=0. */
        if (!MEMEXT_TEST_CONNECTED) return -1;
        uint8_t *p = memext_get_ram_write_pointer_by_rawbank(desc->sub_id);
        if (!p) return -1;
        /* Pro RAM bank pointer ukazuje do RAM[]; pro FLASH bank by ukazoval
         * do WOM scratch (ale tam se nedostaneme - writable=0). */
        memcpy(&p[offset], data, n);
        return (int)n;
    }

    case REGION_KIND_RAMDISK_STD: {
        if (!g_ramdisk.std.connected || !g_ramdisk.std.memory) return -1;
        if (g_ramdisk.std.type & RAMDISK_IS_READONLY) return -1;
        uint32_t abs_off = (uint32_t)desc->sub_id * 0x10000u + offset;
        memcpy(&g_ramdisk.std.memory[abs_off], data, n);
        return (int)n;
    }

    case REGION_KIND_RAMDISK_PEZIK: {
        /* Per-bank: sub_id = pezik_instance * 8 + bank_idx (0..15). Na
         * MZ-700 a MZ-1500 enumerace dodává jen sub_id 0..7. */
        if (desc->sub_id < 0 || desc->sub_id > 15) return -1;
        int pi = desc->sub_id / 8;
        int bank = desc->sub_id % 8;
        if (!g_ramdisk.pezik[pi].connected) return -1;
        if (!g_ramdisk.pezik[pi].memory) return -1;
        uint32_t abs_off = (uint32_t)bank * 0x10000u + offset;
        memcpy(&g_ramdisk.pezik[pi].memory[abs_off], data, n);
        return (int)n;
    }

    /* ROM_LOWER / ROM_UPPER / CGROM / MEMEXT_FLASH / PROHIBITED_SHADOW
     * mají writable=0; sem se nedostanou (early return výše). default
     * pro neznámé kindy. */
    default:
        return -1;
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
