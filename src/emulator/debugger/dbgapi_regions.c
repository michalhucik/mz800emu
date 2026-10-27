/*
 * dbgapi_regions.c - společné jádro REGION_LIST API
 *
 * Implementuje veřejné funkce z dbgapi_regions.h:
 *   - dbgapi_regions_enumerate(): dispatch na per-arch <arch>_regions_collect()
 *     a cache snapshot pro následující read/write.
 *   - dbgapi_regions_enumerate_dup(): kopie celého výčtu bez stropu.
 *   - dbgapi_regions_refresh(): jen obnoví cache (bez kopie).
 *   - dbgapi_regions_get() / dbgapi_regions_find_kind(): deskriptor z cache
 *     podle ID nebo podle (kind, sub_id).
 *   - dbgapi_regions_read(): no-side-effect read; per-kind dispatch.
 *   - dbgapi_regions_write(): safe write; respekt writable/connected flagů.
 *   - dbgapi_regions_read_gen() / _write_gen(): totéž s kontrolou generace
 *     rozložení výčtu (staré ID po změně sestavy HW = chyba).
 *
 * Cache snapshot:
 *   - s_region_cache[] drží poslední enumerate výsledek, alokace roste podle
 *     skutečného počtu regionů (bez pevného stropu)
 *   - s_region_cache_count = počet validních záznamů
 *   - read/write hledá podle id v cache (= O(1) lookup, id == index v cache)
 *   - s_region_layout_gen se zvýší, když plnění cache dá jiné rozložení
 *     (otisk druhu, sub_id, velikosti a příznaků všech regionů)
 *   - per-kind větve kontrolují živou velikost bufferu (RAMdisk STD, banky
 *     MemExt), takže ani deskriptor ze starší sestavy nesáhne mimo buffer
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
#include "debugger/trace/ramlog_hook.h"
#include "hw-generic/devbus/devbus.h"

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
 * Počet regionů závisí na připojeném HW: MZ-800 s MemExtem Luftner, RAMdiskem
 * STD 16 MB a oběma PEZIKy má podle per-arch collect 12 + 256 + 256 + 16
 * (+ 1 PROHIBITED po OUT 0E5h) regionů. Pevný strop by výčet zase usekl,
 * proto se alokace cache zvětšuje podle počtu, který collect vrátí (collect
 * počítá i položky, které se do pole nevešly, viz dbgapi_regions_add()).
 * ============================================================================ */

/** @brief Cache výčtu; alokace s_region_cache_cap položek, NULL do 1. plnění. */
static st_REGION_DESC *s_region_cache = NULL;
/** @brief Počet alokovaných položek s_region_cache (jen roste). */
static int s_region_cache_cap = 0;
/** @brief Počet platných položek s_region_cache (<= s_region_cache_cap). */
static int s_region_cache_count = 0;

/**
 * @brief Zámek nad s_region_cache[] a s_region_cache_count.
 *
 * Statická GMutex (zero-init je dle GLib validní stav). Drží se jen po
 * dobu plnění cache / kopie deskriptoru - NIKDY přes per-kind přístup
 * k datům (viz Thread safety v hlavičce souboru).
 */
static GMutex s_region_cache_mutex;

/**
 * @brief Generace rozložení výčtu (viz dbgapi_regions_layout_gen()).
 *
 * 0 = cache se ještě nenaplnila. Zvyšuje ji region_cache_fill_locked(), když
 * otisk nového výčtu nesouhlasí s předchozím. Chráněno s_region_cache_mutex.
 */
static uint32_t s_region_layout_gen = 0;
/** @brief Otisk rozložení posledního plnění cache (chráněno s_region_cache_mutex). */
static uint64_t s_region_layout_fp = 0;


/* ============================================================================
 * Per-arch dispatch
 * ============================================================================ */

/**
 * @brief Přidá regiony připojených zařízení devbus (REGION_KIND_DEVICE).
 *
 * sub_id = slot * 16 + index regionu v deskriptoru, logical_base 0xFFFFFFFF
 * (paměť zařízení není v prostoru CPU), connected = 1. Bez inicializovaného
 * devbus (MZ-800, MZ-1500) nepřidá nic. Otisk rozložení (kind, sub_id)
 * zachytí změnu připojení sám.
 *
 * @param out       Pole výstupu.
 * @param p_count   Počet zapsaných (počítá i nevešlé, viz dbgapi_regions_add()).
 * @param max_count Kapacita pole.
 */
static void regions_collect_devbus(st_REGION_DESC *out, int *p_count, int max_count)
{
    for (unsigned slot = 0; slot < devbus_count(); slot++) {
        if (!devbus_is_connected((int)slot)) continue;
        const st_DEVBUS_DEVICE_DESC *d = devbus_desc((int)slot);
        for (unsigned i = 0; i < d->region_count; i++) {
            const st_DEVBUS_REGION *r = &d->regions[i];
            dbgapi_regions_add(out, p_count, max_count,
                REGION_KIND_DEVICE, (int)(slot * 16u + i), r->name,
                0xFFFFFFFFu, r->size, r->writable ? 1 : 0, 1, 0);
        }
    }
}


static int arch_regions_collect(st_REGION_DESC *out, int max_count)
{
#if MZARCH == 800
    int n = mz800_regions_collect(out, max_count);
#elif MZARCH == 1500
    int n = mz1500_regions_collect(out, max_count);
#elif MZARCH == 700
    int n = mz700_regions_collect(out, max_count);
#else
#error "Unsupported MZARCH for dbgapi_regions"
#endif
    regions_collect_devbus(out, &n, max_count);
    return n;
}


/* ============================================================================
 * Veřejné API
 * ============================================================================ */

/**
 * @brief FNV-1a: přimíchá 32bitovou hodnotu do 64bitového otisku.
 * @param fp Dosavadní otisk.
 * @param v  Přimíchaná hodnota.
 * @return Nový otisk.
 */
static uint64_t region_fp_mix(uint64_t fp, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        fp ^= (uint8_t)(v >> (i * 8));
        fp *= 0x100000001B3ull;
    }
    return fp;
}


/**
 * @brief Otisk rozložení výčtu v cache.
 *
 * Počet regionů a u každého druh, sub_id, velikost, writable a connected.
 * Bez mapped_now (mění se s bankováním) a jména (plyne z druhu a sub_id).
 *
 * @return 64bitový otisk.
 * @pre Drží se s_region_cache_mutex.
 */
static uint64_t region_layout_fp_locked(void)
{
    uint64_t fp = 0xCBF29CE484222325ull;
    fp = region_fp_mix(fp, (uint32_t)s_region_cache_count);
    for (int i = 0; i < s_region_cache_count; i++) {
        const st_REGION_DESC *d = &s_region_cache[i];
        fp = region_fp_mix(fp, (uint32_t)d->kind);
        fp = region_fp_mix(fp, (uint32_t)d->sub_id);
        fp = region_fp_mix(fp, d->size);
        fp = region_fp_mix(fp, (uint32_t)((d->writable ? 1u : 0u) | (d->connected ? 2u : 0u)));
    }
    return fp;
}


/* Helper: naplní cache z per-arch collect. Volat POUZE pod
 * s_region_cache_mutex. Nevejde-li se výčet do alokace, zvětší ji na
 * skutečný počet a collect zopakuje (stav HW se mezi oběma průchody nemění,
 * volání drží zámek a výčet jen čte). Když se rozložení výčtu proti
 * předchozímu plnění změnilo, zvýší generaci (s_region_layout_gen, nikdy 0). */
static void region_cache_fill_locked(void)
{
    int n = arch_regions_collect(s_region_cache, s_region_cache_cap);
    if (n > s_region_cache_cap) {
        s_region_cache = g_renew(st_REGION_DESC, s_region_cache, n);
        s_region_cache_cap = n;
        n = arch_regions_collect(s_region_cache, s_region_cache_cap);
    }
    if (n < 0) n = 0;
    s_region_cache_count = (n < s_region_cache_cap) ? n : s_region_cache_cap;

    uint64_t fp = region_layout_fp_locked();
    if (s_region_layout_gen == 0 || fp != s_region_layout_fp) {
        s_region_layout_fp = fp;
        if (++s_region_layout_gen == 0) s_region_layout_gen = 1;
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


void dbgapi_regions_refresh(void)
{
    g_mutex_lock(&s_region_cache_mutex);
    region_cache_fill_locked();
    g_mutex_unlock(&s_region_cache_mutex);
}


st_REGION_DESC *dbgapi_regions_enumerate_dup_gen(int *out_count, uint32_t *out_gen)
{
    st_REGION_DESC *copy = NULL;
    int n;
    uint32_t gen;

    g_mutex_lock(&s_region_cache_mutex);
    region_cache_fill_locked();
    n = s_region_cache_count;
    gen = s_region_layout_gen;
    if (n > 0) {
        copy = g_new(st_REGION_DESC, n);
        memcpy(copy, s_region_cache, (size_t) n * sizeof(st_REGION_DESC));
    }
    g_mutex_unlock(&s_region_cache_mutex);

    if (out_count) *out_count = n;
    if (out_gen) *out_gen = gen;
    return copy;
}


st_REGION_DESC *dbgapi_regions_enumerate_dup(int *out_count)
{
    return dbgapi_regions_enumerate_dup_gen(out_count, NULL);
}


uint32_t dbgapi_regions_layout_gen(void)
{
    g_mutex_lock(&s_region_cache_mutex);
    uint32_t gen = s_region_layout_gen;
    g_mutex_unlock(&s_region_cache_mutex);
    return gen;
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


/**
 * @brief Kopie deskriptoru podle ID s kontrolou generace rozložení.
 *
 * Při @p layout_gen != 0 obnoví cache podle aktuálního stavu HW (tím se
 * změna sestavy promítne do generace i bez otevřeného Memory Browseru)
 * a deskriptor vydá jen tehdy, když generace souhlasí. Při @p layout_gen
 * == 0 je to lookup_region_copy() (bez obnovy cache).
 *
 * @param region_id  ID z výčtu.
 * @param layout_gen Generace výčtu, ze kterého ID pochází; 0 = bez kontroly.
 * @param out_desc   Kopie deskriptoru (zapsaná jen při návratu 0).
 * @return 0 = OK, -1 = neplatné ID, DBGAPI_REGIONS_ERR_STALE = rozložení
 *         výčtu se od generace @p layout_gen změnilo.
 * @note Thread-safe (zámek cache).
 */
static int lookup_region_copy_gen(int region_id, uint32_t layout_gen, st_REGION_DESC *out_desc)
{
    if (layout_gen == 0) {
        return lookup_region_copy(region_id, out_desc) ? 0 : -1;
    }
    int rc = -1;
    g_mutex_lock(&s_region_cache_mutex);
    region_cache_fill_locked();
    if (s_region_layout_gen != layout_gen) {
        rc = DBGAPI_REGIONS_ERR_STALE;
    } else if (region_id >= 0 && region_id < s_region_cache_count) {
        *out_desc = s_region_cache[region_id];
        rc = 0;
    }
    g_mutex_unlock(&s_region_cache_mutex);
    return rc;
}


bool dbgapi_regions_get(int region_id, st_REGION_DESC *out)
{
    if (!out) return false;
    return lookup_region_copy(region_id, out);
}


bool dbgapi_regions_find_kind(en_REGION_KIND kind, int sub_id, st_REGION_DESC *out)
{
    bool found = false;
    if (!out) return false;
    g_mutex_lock(&s_region_cache_mutex);
    if (s_region_cache_count <= 0) {
        region_cache_fill_locked();
    }
    for (int i = 0; i < s_region_cache_count; i++) {
        if (s_region_cache[i].kind == kind && s_region_cache[i].sub_id == sub_id) {
            *out = s_region_cache[i];
            found = true;
            break;
        }
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


/**
 * @brief Velikost bufferu RAMdisku STD podle živého stavu.
 *
 * Buffer má (g_ramdisk.std.size + 1) bank po 64 KB (ramdisk_std_init(),
 * ramdisk_fit_buffers_after_load()). Deskriptor z výčtu před zmenšením
 * RAMdisku může mít sub_id za touto hranicí.
 *
 * @return Velikost v bajtech.
 */
static size_t ramdisk_std_live_size(void)
{
    return ((size_t)g_ramdisk.std.size + 1u) * 0x10000u;
}


/**
 * @brief Ověří, že raw banka MemExt leží v polích g_memext.RAM / FLASH.
 *
 * Raw banky 00h-7Fh jsou RAM, 80h-0FFh FLASH (memext_get_ram_read_pointer_by_rawbank()),
 * každá po MEMEXT_RAW_BANK_SIZE; obě pole mají 128 takových bank.
 *
 * @param rawbank sub_id regionu MEMEXT_RAM / MEMEXT_FLASH.
 * @return true = banka leží uvnitř pole.
 */
static bool memext_rawbank_in_range(int rawbank)
{
    return rawbank >= 0 && rawbank <= 0xFF
           && (size_t)((rawbank & 0x7F) + 1) * MEMEXT_RAW_BANK_SIZE
              <= ((rawbank & 0x80) ? (size_t)MEMEXT_FLASH_SIZE : (size_t)MEMEXT_RAM_SIZE);
}


/**
 * @brief Čtení z regionu podle deskriptoru (tělo dbgapi_regions_read()).
 *
 * @param desc   Kopie deskriptoru (z cache, případně ze starší sestavy HW).
 * @param offset Offset v regionu.
 * @param out    Výstupní buffer, min @p len bajtů.
 * @param len    Počet bajtů.
 * @return Viz dbgapi_regions_read().
 */
static int regions_read_desc(const st_REGION_DESC *desc, uint32_t offset, uint8_t *out, uint32_t len)
{
    if (!out || len == 0) return -1;
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
        /* MZ-800 v 700 compat módu: char VRAM (2 KB) = Plane I offset 0x1000..0x17FF.
         * Ověřeno proti mz800_memory.c:1105 kde write z 0xD000 jde na
         * g_memoryVRAM_I[0x1000 | (addr & 0x0FFF)]. Bug pre-fix četl offset
         * 0 = začátek CG-RAM (= chybný obsah). */
        memcpy(out, &g_memory.VRAM[0x1000 + offset], n);
        return (int)n;

    case REGION_KIND_VRAM_700_ATTR:
        /* MZ-800 v 700 compat módu: attr VRAM (2 KB) = Plane I offset 0x1800..0x1FFF.
         * Stejná logika jako CHAR + posun o 0x0800 (znaková VRAM má 2 KB,
         * attr leží za ní). Bug pre-fix četl offset 0x0800 = uvnitř CG-RAM
         * (= chybný obsah). */
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
        if (!memext_rawbank_in_range(desc->sub_id)) return -1;
        uint8_t *p = memext_get_ram_read_pointer_by_rawbank(desc->sub_id);
        if (!p) return -1;
        memcpy(out, &p[offset], n);
        return (int)n;
    }

    case REGION_KIND_RAMDISK_STD: {
        /* Direct array access do g_ramdisk.std.memory.
         * Nelze použít ramdisk_std_read_byte() - má side-effect na offset
         * cursor (auto-inc). Živá velikost: deskriptor může pocházet
         * z výčtu před zmenšením RAMdisku (načtení snapshotu). */
        if (!g_ramdisk.std.connected || !g_ramdisk.std.memory) return -1;
        if (desc->sub_id < 0) return -1;
        size_t abs_off = (size_t)desc->sub_id * 0x10000u + offset;
        if (abs_off + n > ramdisk_std_live_size()) return -1;
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

    case REGION_KIND_DEVICE: {
        /* Paměť zařízení devbus: slot = sub_id / 16, region = sub_id % 16.
         * Živá kontrola připojení a velikosti (deskriptor ze starší generace). */
        const st_DEVBUS_REGION *r = devbus_region(desc->sub_id / 16, (unsigned)(desc->sub_id % 16));
        if (!r || !devbus_is_connected(desc->sub_id / 16)) return -1;
        if (offset + n > r->size) return -1;
        void *ctx = devbus_ctx(desc->sub_id / 16);
        memcpy(out, r->data(ctx) + offset, n);
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


int dbgapi_regions_read(int region_id, uint32_t offset, uint8_t *out, uint32_t len)
{
    if (!out || len == 0) return -1;
    st_REGION_DESC desc;
    if (!lookup_region_copy(region_id, &desc)) return -1;
    return regions_read_desc(&desc, offset, out, len);
}


int dbgapi_regions_read_gen(int region_id, uint32_t layout_gen,
                            uint32_t offset, uint8_t *out, uint32_t len)
{
    if (!out || len == 0) return -1;
    st_REGION_DESC desc;
    int rc = lookup_region_copy_gen(region_id, layout_gen, &desc);
    if (rc != 0) return rc;
    return regions_read_desc(&desc, offset, out, len);
}


/* ============================================================================
 * WRITE per kind
 * ============================================================================ */

/**
 * @brief Vlastní zápis do regionu podle deskriptoru (bez záznamu zápisů).
 *
 * @param desc   Kopie deskriptoru (z cache, případně ze starší sestavy HW).
 * @param offset Offset v regionu.
 * @param data   Data k zapsání.
 * @param len    Počet bajtů.
 * @return Viz dbgapi_regions_write().
 */
static int regions_write_impl(const st_REGION_DESC *desc, uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return -1;
    if (!desc->connected) return -1;
    if (!desc->writable) return -1;

    uint32_t n = clamp_len(desc, offset, len);
    if (n == 0) return 0;

    switch (desc->kind) {

    case REGION_KIND_LOGICAL: {
        /* Banking-aware write přes memory_write_byte_no_bp (bez MEM_W
         * breakpointu - zápis debuggerem). POZOR: může mít side-effecty
         * (např. VRAM write hits GDG WF dispatch). Pro VRAM editaci
         * v pause modu preferovat fyzický plane region. */
        for (uint32_t i = 0; i < n; i++) {
            memory_write_byte_no_bp((uint16_t)(offset + i), data[i]);
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
        /* Attr VRAM v MZ-700 compat módu = Plane I offset 0x1800..0x1FFF. */
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
        if (!memext_rawbank_in_range(desc->sub_id)) return -1;
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
        /* Živá velikost jako u čtení: deskriptor může být starší než
         * zmenšení RAMdisku. */
        if (desc->sub_id < 0) return -1;
        size_t abs_off = (size_t)desc->sub_id * 0x10000u + offset;
        if (abs_off + n > ramdisk_std_live_size()) return -1;
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

    case REGION_KIND_DEVICE: {
        /* Paměť zařízení devbus; po zápisu on_write(): zařízení přepočítá
         * odvozený stav a invaliduje obraz. */
        const st_DEVBUS_REGION *r = devbus_region(desc->sub_id / 16, (unsigned)(desc->sub_id % 16));
        if (!r || !r->writable || !devbus_is_connected(desc->sub_id / 16)) return -1;
        if (offset + n > r->size) return -1;
        void *ctx = devbus_ctx(desc->sub_id / 16);
        memcpy(r->data(ctx) + offset, data, n);
        if (r->on_write) r->on_write(ctx, offset, n);
        return (int)n;
    }

    /* ROM_LOWER / ROM_UPPER / CGROM / MEMEXT_FLASH / PROHIBITED_SHADOW
     * mají writable=0; sem se nedostanou (early return výše). default
     * pro neznámé kindy. */
    default:
        return -1;
    }
}


/**
 * @brief Buňka paměti CPU pro bajt fyzického regionu (pro záznam zápisů).
 *
 * @param desc   Region.
 * @param off    Offset v regionu.
 * @param space  Výstup: prostor ramlogu.
 * @param soff   Výstup: offset v prostoru.
 * @return Ukazatel na buňku, NULL = region neleží v paměti CPU (RAMdisk,
 *         Logical - ten zaznamená memory_write_byte_no_bp()).
 */
static uint8_t *regions_ramlog_cell(const st_REGION_DESC *desc, uint32_t off,
                                    uint8_t *space, uint32_t *soff)
{
    uint8_t *p = NULL;
    switch (desc->kind) {
    case REGION_KIND_RAM:
        p = g_memory.memram_write[(off >> 12) & 0xF] + (off & 0x0FFF);
        return ramlog_platform_ram_cell(p, space, soff) ? p : NULL;
    case REGION_KIND_MEMEXT_RAM:
        if (!MEMEXT_TEST_CONNECTED) return NULL;
        p = memext_get_ram_write_pointer_by_rawbank(desc->sub_id);
        if (!p) return NULL;
        p += off;
        return ramlog_platform_ram_cell(p, space, soff) ? p : NULL;
#if MZARCH == 800
    case REGION_KIND_CGRAM_700:
        *space = RAMLOG_SPACE_VRAM; *soff = off;
        return &g_memory.VRAM[off];
    case REGION_KIND_VRAM_700_CHAR:
        *space = RAMLOG_SPACE_VRAM; *soff = 0x1000u + off;
        return &g_memory.VRAM[*soff];
    case REGION_KIND_VRAM_700_ATTR:
        *space = RAMLOG_SPACE_VRAM; *soff = 0x1800u + off;
        return &g_memory.VRAM[*soff];
    case REGION_KIND_VRAM_PHYS_PLANE:
        p = vram_plane_pointer(desc->sub_id);
        if (!p) return NULL;
        *space = RAMLOG_SPACE_VRAM; *soff = (uint32_t)desc->sub_id * MEMORY_SIZE_VRAM_BANK + off;
        return p + off;
#else
    case REGION_KIND_VRAM_700_CHAR:
        *space = RAMLOG_SPACE_VRAM; *soff = off;
        return &g_memory.VRAM[off];
    case REGION_KIND_VRAM_700_ATTR:
        *space = RAMLOG_SPACE_VRAM; *soff = 0x0800u + off;
        return &g_memory.VRAM[*soff];
#endif
#if MZARCH == 1500
    case REGION_KIND_PCG_1500:
        if (desc->sub_id < 0 || desc->sub_id > 2) return NULL;
        *space = RAMLOG_SPACE_PCG; *soff = (uint32_t)desc->sub_id * MEMORY_SIZE_PCG_BANK + off;
        return &g_memory.PCG[*soff];
#endif
    default:
        return NULL;
    }
}


/**
 * @brief Zápis podle deskriptoru včetně záznamu zápisů (tělo dbgapi_regions_write()).
 *
 * @param desc   Kopie deskriptoru.
 * @param offset Offset v regionu.
 * @param data   Data k zapsání.
 * @param len    Počet bajtů.
 * @return Viz dbgapi_regions_write().
 */
static int regions_write_logged(const st_REGION_DESC *desc, uint32_t offset, const uint8_t *data, uint32_t len)
{
    /* Záznam zápisů (ramlog): fyzické regiony v paměti CPU zaznamenat po
     * bajtech (old/new přímo z buňky); Logical zaznamená sám
     * memory_write_byte_no_bp(). */
    if (!g_ramlog_active || !data || len == 0
        || desc->kind == REGION_KIND_LOGICAL || !desc->connected || !desc->writable) {
        return regions_write_impl(desc, offset, data, len);
    }
    uint32_t n = clamp_len(desc, offset, len);
    uint8_t **cell = (uint8_t **)g_malloc0((gsize)(n ? n : 1) * sizeof(uint8_t *));
    uint8_t *old = (uint8_t *)g_malloc0(n ? n : 1);
    uint8_t *sp = (uint8_t *)g_malloc0(n ? n : 1);
    uint32_t *so = (uint32_t *)g_malloc0((gsize)(n ? n : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        cell[i] = regions_ramlog_cell(desc, offset + i, &sp[i], &so[i]);
        if (cell[i]) old[i] = *cell[i];
    }
    int rc = regions_write_impl(desc, offset, data, len);
    for (uint32_t i = 0; rc > 0 && i < n && i < (uint32_t)rc; i++) {
        if (cell[i]) ramlog_hook_cell_write((en_RAMLOG_SPACE)sp[i], so[i], old[i], *cell[i]);
    }
    g_free(cell);
    g_free(old);
    g_free(sp);
    g_free(so);
    return rc;
}


int dbgapi_regions_write(int region_id, uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return -1;
    st_REGION_DESC desc;
    if (!lookup_region_copy(region_id, &desc)) return -1;
    return regions_write_logged(&desc, offset, data, len);
}


int dbgapi_regions_write_gen(int region_id, uint32_t layout_gen,
                             uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return -1;
    st_REGION_DESC desc;
    int rc = lookup_region_copy_gen(region_id, layout_gen, &desc);
    if (rc != 0) return rc;
    return regions_write_logged(&desc, offset, data, len);
}

bool dbgapi_regions_ramlog_cell(int kind, int sub_id, uint32_t offset,
                                uint8_t *out_space, uint32_t *out_offset)
{
    if (kind < 0 || kind >= REGION_KIND_COUNT) return false;
    st_REGION_DESC desc;
    if (!dbgapi_regions_find_kind((en_REGION_KIND)kind, sub_id, &desc)) return false;
    if (offset >= desc.size) return false;
    return regions_ramlog_cell(&desc, offset, out_space, out_offset) != NULL;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
