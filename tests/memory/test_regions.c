/*
 * test_regions.c - unit testy pro REGION_LIST API (dbgapi_regions)
 *
 * Testuje:
 *   - basic enumerate: vrátí ne-prázdný seznam s LOGICAL + RAM + ROM_LOWER
 *     + ROM_UPPER (= minimum invariant na MZ-800)
 *   - read_ram_no_se: zápis do g_memory.RAM[] viditelný přes API
 *   - read_rom_no_se: ROM_LOWER region vrací data z g_memory.ROM[0..]
 *   - memext_not_connected: Memext disconnected = žádné MEMEXT_RAM/FLASH
 *     regiony v enumerate
 *   - memext_pehu_bank_count: PEHU connected = 128 raw 4 KB kusů (64 bank
 *     po 8 KB), 0 FLASH; horní půlka banky 3Fh = raw kus 7Fh
 *   - memext_connected_luftner: LUFTNER connected = 128 RAM + 128 FLASH
 *   - prohibited_shadow: aktivace flagu = region s 0x1A bajty
 *   - ramdisk_pezik_e8: PEZIK E8 connected = jeden RAMDISK_PEZIK region
 *     se sub_id=1, 512 KB
 *   - ramdisk_std_shrink_stale: po zmenšení RAMdisku STD starý deskriptor
 *     nesahá mimo buffer, staré ID s generací = DBGAPI_REGIONS_ERR_STALE
 *   - vram_plane_read: plane I/II R/W přes API
 *   - write_respects_flags: FLASH writable=0 → write vrátí -1
 *
 * Test framework je MZARCH=800 only (viz cmake/AddMzTest.cmake), takže
 * testy se kompilují jen pro MZ-800 platformu.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/ramdisk/ramdisk.h"
#include "mzarch/mz800/memory/mz800_memory.h"
#include "debugger/dbgapi_regions.h"


/* setUp: deterministický stav. Memext odpojen, Ramdisk odpojen,
 * PROHIBITED flag clear. */
void setUp(void)
{
    g_memext.init_luftner = MEMEXT_INIT_LUFTNER_RESET;
    memext_disconnect();
    /* Ramdisk - odpojení všech 3 instancí. */
    g_ramdisk.std.connected = RAMDISK_DISCONNECTED;
    g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected = RAMDISK_DISCONNECTED;
    g_ramdisk.pezik[RAMDISK_PEZIK_68].connected = RAMDISK_DISCONNECTED;
    /* Vyčisti PROHIBITED flag (může zůstat z předchozího testu). */
    g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
}

void tearDown(void)
{
    memext_disconnect();
    g_ramdisk.std.connected = RAMDISK_DISCONNECTED;
    g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected = RAMDISK_DISCONNECTED;
    g_ramdisk.pezik[RAMDISK_PEZIK_68].connected = RAMDISK_DISCONNECTED;
    g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
}


/* Helper: najde první region daného druhu. Vrací index nebo -1. */
static int find_region(const st_REGION_DESC *regs, int count, en_REGION_KIND kind)
{
    for (int i = 0; i < count; i++) {
        if (regs[i].kind == kind) return i;
    }
    return -1;
}


/* Helper: najde region kind+sub_id. Vrací index nebo -1. */
static int find_region_sub(const st_REGION_DESC *regs, int count,
    en_REGION_KIND kind, int sub_id)
{
    for (int i = 0; i < count; i++) {
        if (regs[i].kind == kind && regs[i].sub_id == sub_id) return i;
    }
    return -1;
}


/* Helper: spočítá kolik regionů daného druhu je v seznamu. */
static int count_regions(const st_REGION_DESC *regs, int count, en_REGION_KIND kind)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (regs[i].kind == kind) n++;
    }
    return n;
}


/* ================================================================
 * Test 0: read BEZ předchozího enumerate musí fungovat (mzdos 0012)
 *
 * region_id se překládá přes interní cache s_region_cache, kterou plní
 * dbgapi_regions_enumerate(). Bez lazy auto-fill by read na čerstvém
 * stavu (= klient nezavolal regions_list / neotevřel Memory Browser)
 * vrátil -1 ("REGIONS_READ failed"). Lazy-fill v lookup_region() to
 * řeší - read/write je self-sufficient.
 *
 * POZOR: tento test MUSÍ běžet jako PRVNÍ (před jakýmkoliv enumerate),
 * protože s_region_cache_count je static init 0 jen na začátku procesu.
 * Nemáme veřejné API na reset cache, takže prázdný stav získáme jen
 * pořadím (první RUN_TEST v main()).
 * ================================================================ */

void test_regions_read_without_enumerate_autofills(void)
{
    /* Žádné volání dbgapi_regions_enumerate() před readem. Region 0
     * vždy existuje (LOGICAL) a má 64 KB, takže read 4 bajtů z offsetu 0
     * musí vrátit přesně 4 (= cache se sama naplnila). Před fixem: -1. */
    uint8_t buf[4];
    int n = dbgapi_regions_read(0, 0x0000, buf, 4);
    TEST_ASSERT_EQUAL_INT(4, n);
}


/* ================================================================
 * Test 1: basic enumerate vrátí ne-prázdný seznam s povinnými regiony
 * ================================================================ */

void test_regions_enumerate_basic(void)
{
    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);

    /* MZ-800 minimum: LOGICAL + RAM + ROM_LOWER + ROM_UPPER + CGROM
     * + CGRAM_700 + 2× VRAM_700 + 4× VRAM_PHYS_PLANE = 11. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(11, count);

    /* Povinné regiony. */
    TEST_ASSERT_NOT_EQUAL_INT(-1, find_region(regs, count, REGION_KIND_LOGICAL));
    TEST_ASSERT_NOT_EQUAL_INT(-1, find_region(regs, count, REGION_KIND_RAM));
    TEST_ASSERT_NOT_EQUAL_INT(-1, find_region(regs, count, REGION_KIND_ROM_LOWER));
    TEST_ASSERT_NOT_EQUAL_INT(-1, find_region(regs, count, REGION_KIND_ROM_UPPER));
    TEST_ASSERT_NOT_EQUAL_INT(-1, find_region(regs, count, REGION_KIND_CGROM));

    /* ID stabilita per session: id[i] == i. */
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(i, regs[i].id);
    }
}


/* ================================================================
 * Test 2: read z REGION_KIND_RAM vrací data z g_memory.RAM[]
 *         (no-side-effect, žádný banking aliasing)
 * ================================================================ */

void test_regions_read_ram_no_se(void)
{
    /* Zapiš známý vzor přímo do g_memory.RAM[] (= "raw" 64 KB). */
    g_memory.RAM[0x1000] = 0xAB;
    g_memory.RAM[0x1001] = 0xCD;
    g_memory.RAM[0x1002] = 0xEF;

    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);
    int ram_id = find_region(regs, count, REGION_KIND_RAM);
    TEST_ASSERT_NOT_EQUAL_INT(-1, ram_id);

    /* Velikost 64 KB. */
    TEST_ASSERT_EQUAL_UINT32(MEMORY_SIZE_RAM, regs[ram_id].size);
    TEST_ASSERT_EQUAL_INT(1, regs[ram_id].writable);
    TEST_ASSERT_EQUAL_INT(1, regs[ram_id].connected);

    uint8_t buf[4];
    int n = dbgapi_regions_read(ram_id, 0x1000, buf, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0xAB, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, buf[2]);
}


/* ================================================================
 * Test 3: read z REGION_KIND_ROM_LOWER vrací data z g_memory.ROM[0..]
 * ================================================================ */

void test_regions_read_rom_lower(void)
{
    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);
    int rom_id = find_region(regs, count, REGION_KIND_ROM_LOWER);
    TEST_ASSERT_NOT_EQUAL_INT(-1, rom_id);

    /* ROM_LOWER má 4 KB, writable=0. */
    TEST_ASSERT_EQUAL_UINT32(ROM_SIZE_0000, regs[rom_id].size);
    TEST_ASSERT_EQUAL_INT(0, regs[rom_id].writable);

    /* První 4 bajty ROM musí odpovídat g_memory.ROM[0..3]. */
    uint8_t buf[4];
    int n = dbgapi_regions_read(rom_id, 0, buf, 4);
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_HEX8(g_memory.ROM[0], buf[0]);
    TEST_ASSERT_EQUAL_HEX8(g_memory.ROM[1], buf[1]);
    TEST_ASSERT_EQUAL_HEX8(g_memory.ROM[2], buf[2]);
    TEST_ASSERT_EQUAL_HEX8(g_memory.ROM[3], buf[3]);

    /* Write do ROM_LOWER musí selhat (writable=0). */
    uint8_t wd[1] = { 0x42 };
    int wn = dbgapi_regions_write(rom_id, 0, wd, 1);
    TEST_ASSERT_EQUAL_INT(-1, wn);
}


/* ================================================================
 * Test 4: Memext disconnected = žádný MEMEXT_RAM/FLASH region
 * ================================================================ */

void test_regions_memext_not_connected(void)
{
    /* setUp už odpojil Memext. */
    TEST_ASSERT_FALSE(MEMEXT_TEST_CONNECTED);

    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);

    /* Žádné MEMEXT_RAM ani MEMEXT_FLASH. */
    TEST_ASSERT_EQUAL_INT(0, count_regions(regs, count, REGION_KIND_MEMEXT_RAM));
    TEST_ASSERT_EQUAL_INT(0, count_regions(regs, count, REGION_KIND_MEMEXT_FLASH));
}


/* ================================================================
 * Test 5: Memext LUFTNER connected = 128 RAM + 128 FLASH banks
 * ================================================================ */

void test_regions_memext_luftner_connected(void)
{
    memext_connect(MEMEXT_TYPE_LUFTNER);
    TEST_ASSERT_TRUE(MEMEXT_TEST_CONNECTED);
    TEST_ASSERT_TRUE(MEMEXT_TEST_TYPE_LUFTNER);

    /* Pole musí být dostatečně velké (alespoň ~11 + 256 = ~267). */
    static st_REGION_DESC regs[512];
    int count = dbgapi_regions_enumerate(regs, 512);

    int ram_n = count_regions(regs, count, REGION_KIND_MEMEXT_RAM);
    int flash_n = count_regions(regs, count, REGION_KIND_MEMEXT_FLASH);
    TEST_ASSERT_EQUAL_INT(MEMEXT_LUFTNER_BANKS, ram_n);     /* 128 */
    TEST_ASSERT_EQUAL_INT(MEMEXT_LUFTNER_BANKS, flash_n);   /* 128 */

    /* Read z RAM bank 0x05: zapiš známý vzor přes get_*_by_rawbank a
     * ověř že API vrátí stejné. */
    uint8_t *p = memext_get_ram_read_pointer_by_rawbank(0x05);
    TEST_ASSERT_NOT_NULL(p);
    p[0x100] = 0x55;
    p[0x101] = 0xAA;

    int idx = find_region_sub(regs, count, REGION_KIND_MEMEXT_RAM, 0x05);
    TEST_ASSERT_NOT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_INT(1, regs[idx].writable);
    TEST_ASSERT_EQUAL_UINT32(MEMEXT_RAW_BANK_SIZE, regs[idx].size);

    uint8_t buf[2];
    int n = dbgapi_regions_read(regs[idx].id, 0x100, buf, 2);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x55, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, buf[1]);

    /* FLASH bank 0x80 (= první FLASH): writable=0, write vrátí -1. */
    int fidx = find_region_sub(regs, count, REGION_KIND_MEMEXT_FLASH, 0x80);
    TEST_ASSERT_NOT_EQUAL_INT(-1, fidx);
    TEST_ASSERT_EQUAL_INT(0, regs[fidx].writable);

    uint8_t wd[1] = { 0x42 };
    int wn = dbgapi_regions_write(regs[fidx].id, 0, wd, 1);
    TEST_ASSERT_EQUAL_INT(-1, wn);
}


/* ================================================================
 * Test 6: PROHIBITED shadow region (vrací 0x1A na MZ-800)
 * ================================================================ */

void test_regions_prohibited_shadow(void)
{
    /* Bez flagu: žádný PROHIBITED_SHADOW region. */
    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);
    TEST_ASSERT_EQUAL_INT(-1,
        find_region(regs, count, REGION_KIND_PROHIBITED_SHADOW));

    /* Set flagu = region se objeví. */
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    count = dbgapi_regions_enumerate(regs, 64);
    int pid = find_region(regs, count, REGION_KIND_PROHIBITED_SHADOW);
    TEST_ASSERT_NOT_EQUAL_INT(-1, pid);

    TEST_ASSERT_EQUAL_INT(0, regs[pid].writable);
    TEST_ASSERT_EQUAL_INT(1, regs[pid].mapped_now);

    /* Read vrací 0x1A. */
    uint8_t buf[8];
    int n = dbgapi_regions_read(regs[pid].id, 0, buf, 8);
    TEST_ASSERT_EQUAL_INT(8, n);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x1A, buf[i]);
    }
}


/* ================================================================
 * Test 7: VRAM plane I/II R/W přes API
 * ================================================================ */

void test_regions_vram_plane(void)
{
    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);

    /* Plane I (sub_id=0). */
    int p1 = find_region_sub(regs, count, REGION_KIND_VRAM_PHYS_PLANE, 0);
    TEST_ASSERT_NOT_EQUAL_INT(-1, p1);
    TEST_ASSERT_EQUAL_UINT32(MEMORY_SIZE_VRAM_BANK, regs[p1].size);
    TEST_ASSERT_EQUAL_INT(1, regs[p1].writable);

    /* Write a read-back. */
    uint8_t wd[3] = { 0x11, 0x22, 0x33 };
    int wn = dbgapi_regions_write(regs[p1].id, 0x500, wd, 3);
    TEST_ASSERT_EQUAL_INT(3, wn);
    TEST_ASSERT_EQUAL_HEX8(0x11, g_memory.VRAM[0x500]);
    TEST_ASSERT_EQUAL_HEX8(0x22, g_memory.VRAM[0x501]);
    TEST_ASSERT_EQUAL_HEX8(0x33, g_memory.VRAM[0x502]);

    uint8_t rbuf[3];
    int rn = dbgapi_regions_read(regs[p1].id, 0x500, rbuf, 3);
    TEST_ASSERT_EQUAL_INT(3, rn);
    TEST_ASSERT_EQUAL_HEX8(0x11, rbuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, rbuf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x33, rbuf[2]);

    /* Plane III (sub_id=2) - cílí na g_memory.EXVRAM[0]. */
    int p3 = find_region_sub(regs, count, REGION_KIND_VRAM_PHYS_PLANE, 2);
    TEST_ASSERT_NOT_EQUAL_INT(-1, p3);
    int wn3 = dbgapi_regions_write(regs[p3].id, 0x10, wd, 3);
    TEST_ASSERT_EQUAL_INT(3, wn3);
    TEST_ASSERT_EQUAL_HEX8(0x11, g_memory.EXVRAM[0x10]);
    TEST_ASSERT_EQUAL_HEX8(0x22, g_memory.EXVRAM[0x11]);
    TEST_ASSERT_EQUAL_HEX8(0x33, g_memory.EXVRAM[0x12]);
}


/* ================================================================
 * Test 7b: VRAM_700_CHAR + VRAM_700_ATTR offset mapping (MZ-800)
 *
 * Ověřuje že VRAM 700 char/attr regiony čtou z Plane I offset 0x1000 / 0x1800
 * (= správně, podle mz800_memory.c:1099/1105 dispatch). Bug pre-fix četl
 * offset 0 / 0x800 = CG-RAM oblast.
 * ================================================================ */

/* ================================================================
 * Test 7c: VRAM_700_CHAR + VRAM_700_ATTR pokrývají celé 2 KB (N-D5-4)
 *
 * Fyzická znaková VRAM v režimu 700 je 2 KB (0D000h-0D7FFh = Plane I
 * 0x1000-0x17FF), atributová 2 KB (0D800h-0DFFFh = 0x1800-0x1FFF), viz
 * mz800_memory.c (0x1000 | (addr & 0x0FFF)). Dřív regiony měly 1 KB
 * a horní polovina (blok B kitu SUC) přes region_read nešla.
 * ================================================================ */

void test_regions_vram_700_full_2k(void)
{
    g_memory.VRAM[0x1400] = 0x5A;
    g_memory.VRAM[0x17FF] = 0x5B;
    g_memory.VRAM[0x1C00] = 0x6A;
    g_memory.VRAM[0x1FFF] = 0x6B;

    st_REGION_DESC regs[128];
    int count = dbgapi_regions_enumerate(regs, 128);
    int char_id = find_region(regs, count, REGION_KIND_VRAM_700_CHAR);
    int attr_id = find_region(regs, count, REGION_KIND_VRAM_700_ATTR);
    TEST_ASSERT_NOT_EQUAL_INT(-1, char_id);
    TEST_ASSERT_NOT_EQUAL_INT(-1, attr_id);
    TEST_ASSERT_EQUAL_HEX32(0x0800, regs[char_id].size);
    TEST_ASSERT_EQUAL_HEX32(0x0800, regs[attr_id].size);
    TEST_ASSERT_EQUAL_HEX32(0xD000, regs[char_id].logical_base);
    TEST_ASSERT_EQUAL_HEX32(0xD800, regs[attr_id].logical_base);

    uint8_t b = 0;
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_read(regs[char_id].id, 0x400, &b, 1));
    TEST_ASSERT_EQUAL_HEX8(0x5A, b);
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_read(regs[char_id].id, 0x7FF, &b, 1));
    TEST_ASSERT_EQUAL_HEX8(0x5B, b);
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_read(regs[attr_id].id, 0x400, &b, 1));
    TEST_ASSERT_EQUAL_HEX8(0x6A, b);
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_read(regs[attr_id].id, 0x7FF, &b, 1));
    TEST_ASSERT_EQUAL_HEX8(0x6B, b);
    /* za koncem regionu nic */
    TEST_ASSERT_EQUAL_INT(0, dbgapi_regions_read(regs[char_id].id, 0x800, &b, 1));

    uint8_t wd = 0x99;
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_write(regs[attr_id].id, 0x7FE, &wd, 1));
    TEST_ASSERT_EQUAL_HEX8(0x99, g_memory.VRAM[0x1FFE]);
}

void test_regions_vram_700_offsets(void)
{
    /* Zapiš známé vzory přímo do Plane I (= g_memory.VRAM):
     *   CG-RAM   (offset 0x0000) = 0xC0 ... 0xCF
     *   Char     (offset 0x1000) = 0xCA ... 0xCF (znaková VRAM)
     *   Attr     (offset 0x1800) = 0xA0 ... 0xA3 (atribut VRAM)
     */
    g_memory.VRAM[0x0000] = 0xC0;
    g_memory.VRAM[0x0001] = 0xC1;
    g_memory.VRAM[0x1000] = 0xCA;
    g_memory.VRAM[0x1001] = 0xCB;
    g_memory.VRAM[0x1002] = 0xCC;
    g_memory.VRAM[0x1800] = 0xA0;
    g_memory.VRAM[0x1801] = 0xA1;
    g_memory.VRAM[0x1802] = 0xA2;

    st_REGION_DESC regs[128];
    int count = dbgapi_regions_enumerate(regs, 128);

    /* VRAM_700_CHAR: musí přečíst Plane I offset 0x1000 = 0xCA, 0xCB, 0xCC. */
    int char_id = find_region(regs, count, REGION_KIND_VRAM_700_CHAR);
    TEST_ASSERT_NOT_EQUAL_INT(-1, char_id);

    uint8_t cbuf[3];
    int cn = dbgapi_regions_read(regs[char_id].id, 0, cbuf, 3);
    TEST_ASSERT_EQUAL_INT(3, cn);
    TEST_ASSERT_EQUAL_HEX8(0xCA, cbuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCB, cbuf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xCC, cbuf[2]);

    /* VRAM_700_ATTR: musí přečíst Plane I offset 0x1800 = 0xA0, 0xA1, 0xA2. */
    int attr_id = find_region(regs, count, REGION_KIND_VRAM_700_ATTR);
    TEST_ASSERT_NOT_EQUAL_INT(-1, attr_id);

    uint8_t abuf[3];
    int an = dbgapi_regions_read(regs[attr_id].id, 0, abuf, 3);
    TEST_ASSERT_EQUAL_INT(3, an);
    TEST_ASSERT_EQUAL_HEX8(0xA0, abuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA1, abuf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xA2, abuf[2]);

    /* CG-RAM (CGRAM_700): musí přečíst Plane I offset 0 = 0xC0, 0xC1. */
    int cgram_id = find_region(regs, count, REGION_KIND_CGRAM_700);
    TEST_ASSERT_NOT_EQUAL_INT(-1, cgram_id);

    uint8_t gbuf[2];
    int gn = dbgapi_regions_read(regs[cgram_id].id, 0, gbuf, 2);
    TEST_ASSERT_EQUAL_INT(2, gn);
    TEST_ASSERT_EQUAL_HEX8(0xC0, gbuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC1, gbuf[1]);

    /* Write do VRAM_700_CHAR offset 0x20: musí jít na g_memory.VRAM[0x1020]. */
    uint8_t wd[2] = { 0x77, 0x88 };
    int wn = dbgapi_regions_write(regs[char_id].id, 0x20, wd, 2);
    TEST_ASSERT_EQUAL_INT(2, wn);
    TEST_ASSERT_EQUAL_HEX8(0x77, g_memory.VRAM[0x1020]);
    TEST_ASSERT_EQUAL_HEX8(0x88, g_memory.VRAM[0x1021]);

    /* Write do VRAM_700_ATTR offset 0x30: musí jít na g_memory.VRAM[0x1830]. */
    int wn2 = dbgapi_regions_write(regs[attr_id].id, 0x30, wd, 2);
    TEST_ASSERT_EQUAL_INT(2, wn2);
    TEST_ASSERT_EQUAL_HEX8(0x77, g_memory.VRAM[0x1830]);
    TEST_ASSERT_EQUAL_HEX8(0x88, g_memory.VRAM[0x1831]);
}


/* ================================================================
 * Test 8: PEZIK E8 connected = 8 regionů per bank, každý 64 KB
 *
 * Per-bank semantic: jedna instance PEZIK emit 8 regionů, sub_id =
 * pezik_instance * 8 + bank (= 0..15).
 * PEZIK E8 (instance index 1) → sub_id 8..15.
 * PEZIK 68 (instance index 0) → sub_id 0..7.
 * ================================================================ */

void test_regions_ramdisk_pezik_e8(void)
{
    /* Init Pezik E8 přes API. Empty filepath (init API volá strlen, NULL by
     * spadl - viz ramdisk.c:286). */
    char empty_path[1] = "";
    ramdisk_pezik_init(RAMDISK_PEZIK_E8, RAMDISK_CONNECTED, 0xff,
        PEZIK_BACKUPED_NO, empty_path);
    TEST_ASSERT_TRUE(g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected);
    TEST_ASSERT_NOT_NULL(g_ramdisk.pezik[RAMDISK_PEZIK_E8].memory);

    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);

    /* PEZIK E8 (= instance 1) musí mít všech 8 bank regionů (sub_id 8..15). */
    int pezik_n = count_regions(regs, count, REGION_KIND_RAMDISK_PEZIK);
    TEST_ASSERT_EQUAL_INT(8, pezik_n);

    /* Bank 0 (sub_id 8) - první 64 KB instance E8. */
    int idx0 = find_region_sub(regs, count, REGION_KIND_RAMDISK_PEZIK,
        RAMDISK_PEZIK_E8 * 8 + 0);
    TEST_ASSERT_NOT_EQUAL_INT(-1, idx0);
    TEST_ASSERT_EQUAL_UINT32(0x10000u, regs[idx0].size);  /* 64 KB */
    TEST_ASSERT_EQUAL_INT(1, regs[idx0].writable);

    /* Bank 7 (sub_id 15) - poslední 64 KB instance E8. */
    int idx7 = find_region_sub(regs, count, REGION_KIND_RAMDISK_PEZIK,
        RAMDISK_PEZIK_E8 * 8 + 7);
    TEST_ASSERT_NOT_EQUAL_INT(-1, idx7);

    /* PEZIK 0x68 (instance 0) není connected - žádný region sub_id 0..7. */
    TEST_ASSERT_EQUAL_INT(-1,
        find_region_sub(regs, count, REGION_KIND_RAMDISK_PEZIK, 0));
    TEST_ASSERT_EQUAL_INT(-1,
        find_region_sub(regs, count, REGION_KIND_RAMDISK_PEZIK, 7));

    /* Write + read-back přes API - bank 0, offset 0x2345. */
    uint8_t wd[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    int wn = dbgapi_regions_write(regs[idx0].id, 0x2345, wd, 4);
    TEST_ASSERT_EQUAL_INT(4, wn);

    uint8_t rbuf[4];
    int rn = dbgapi_regions_read(regs[idx0].id, 0x2345, rbuf, 4);
    TEST_ASSERT_EQUAL_INT(4, rn);
    TEST_ASSERT_EQUAL_HEX8(0xDE, rbuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, rbuf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, rbuf[2]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, rbuf[3]);

    /* Verify bank isolation: bank 1 offset 0x2345 = jiný fyzický offset
     * v pezik.memory (bank * 64 KB + offset = 0x10000 + 0x2345 = 0x12345).
     * Bank 0 zápis na offset 0x2345 → pezik.memory[0x2345] = DE.
     * Read bank 1 offset 0x2345 → pezik.memory[0x12345] - musí být jiná data. */
    g_ramdisk.pezik[RAMDISK_PEZIK_E8].memory[0x12345] = 0x77;
    int idx_b1 = find_region_sub(regs, count, REGION_KIND_RAMDISK_PEZIK,
        RAMDISK_PEZIK_E8 * 8 + 1);
    TEST_ASSERT_NOT_EQUAL_INT(-1, idx_b1);
    uint8_t b1_byte;
    rn = dbgapi_regions_read(regs[idx_b1].id, 0x2345, &b1_byte, 1);
    TEST_ASSERT_EQUAL_INT(1, rn);
    TEST_ASSERT_EQUAL_HEX8(0x77, b1_byte);  /* = pezik.memory[0x12345] */

    /* Cleanup - disconnect. */
    ramdisk_pezik_disconnect(RAMDISK_PEZIK_E8);
}


/* ================================================================
 * Test 8a: zmenšení RAMdisku STD - starý deskriptor ani staré ID
 * nesahají mimo buffer (N44)
 *
 * Výčet s RAMdiskem 1 MB (16 bank), pak ramdisk_std_init() na 64 KB
 * (realloc na jednu banku) bez nového výčtu. Cache pořád drží banku 15:
 *   - dbgapi_regions_read/write (bez generace, cesta Memory Browseru)
 *     musí podle živé velikosti vrátit -1, ne číst/psát za koncem bufferu,
 *   - dbgapi_regions_read_gen/write_gen s generací starého výčtu vrátí
 *     DBGAPI_REGIONS_ERR_STALE, i pro banku 0, která dál existuje,
 *   - s generací nového výčtu banka 0 jde číst.
 * ================================================================ */

void test_regions_ramdisk_std_shrink_stale(void)
{
    char empty_path[1] = "";
    ramdisk_std_init(RAMDISK_CONNECTED, RAMDISK_TYPE_STD, RAMDISK_SIZE_1M, empty_path);
    TEST_ASSERT_NOT_NULL(g_ramdisk.std.memory);

    int count = 0;
    uint32_t gen_old = 0;
    st_REGION_DESC *regs = dbgapi_regions_enumerate_dup_gen(&count, &gen_old);
    TEST_ASSERT_NOT_NULL(regs);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, gen_old);
    int i_last = find_region_sub(regs, count, REGION_KIND_RAMDISK_STD, 15);
    int i_first = find_region_sub(regs, count, REGION_KIND_RAMDISK_STD, 0);
    TEST_ASSERT_NOT_EQUAL_INT(-1, i_last);
    TEST_ASSERT_NOT_EQUAL_INT(-1, i_first);
    int id_last = regs[i_last].id;
    int id_first = regs[i_first].id;
    g_free(regs);

    /* Zmenšení na 64 KB bez nového výčtu (jako snapshot_load). */
    ramdisk_std_init(RAMDISK_CONNECTED, RAMDISK_TYPE_STD, RAMDISK_SIZE_64, empty_path);
    TEST_ASSERT_EQUAL_INT(RAMDISK_SIZE_64, g_ramdisk.std.size);

    uint8_t buf[16];
    memset(buf, 0x5A, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_read(id_last, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_write(id_last, 0, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_INT(DBGAPI_REGIONS_ERR_STALE,
        dbgapi_regions_read_gen(id_last, gen_old, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(DBGAPI_REGIONS_ERR_STALE,
        dbgapi_regions_write_gen(id_last, gen_old, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(DBGAPI_REGIONS_ERR_STALE,
        dbgapi_regions_read_gen(id_first, gen_old, 0, buf, sizeof(buf)));

    uint32_t gen_new = dbgapi_regions_layout_gen();
    TEST_ASSERT_NOT_EQUAL_UINT32(gen_old, gen_new);
    regs = dbgapi_regions_enumerate_dup_gen(&count, &gen_new);
    TEST_ASSERT_EQUAL_UINT32(dbgapi_regions_layout_gen(), gen_new);
    i_first = find_region_sub(regs, count, REGION_KIND_RAMDISK_STD, 0);
    TEST_ASSERT_NOT_EQUAL_INT(-1, i_first);
    TEST_ASSERT_EQUAL_INT(-1, find_region_sub(regs, count, REGION_KIND_RAMDISK_STD, 1));
    TEST_ASSERT_EQUAL_INT((int)sizeof(buf),
        dbgapi_regions_read_gen(regs[i_first].id, gen_new, 0, buf, sizeof(buf)));
    g_free(regs);

    ramdisk_std_disconnect();
}


/* ================================================================
 * Test 8b: Memext PEHU emit 128 raw 4 KB kusů = celých 512 KB
 *
 * PEHU má 64 bank po 8 KB (MEMEXT_PEHU_BANKS x MEMEXT_PEHU_BANK_SIZE
 * = 512 KB). Region MEMEXT_RAM je ale raw 4 KB kus (sub_id = raw
 * index, čtení přes memext_get_ram_read_pointer_by_rawbank), takže
 * PEHU potřebuje 2 * MEMEXT_PEHU_BANKS regionů. Dřív se emitovalo jen
 * MEMEXT_PEHU_BANKS = 64 kusů a PEHU banky 20h-3Fh nebyly vidět (N34).
 * ================================================================ */

void test_regions_memext_pehu_bank_count(void)
{
    memext_connect(MEMEXT_TYPE_PEHU);
    TEST_ASSERT_TRUE(MEMEXT_TEST_CONNECTED);
    TEST_ASSERT_TRUE(MEMEXT_TEST_TYPE_PEHU);

    static st_REGION_DESC regs[512];
    int count = dbgapi_regions_enumerate(regs, 512);

    /* 64 PEHU bank po 8 KB = 128 raw kusů po 4 KB. */
    int ram_n = count_regions(regs, count, REGION_KIND_MEMEXT_RAM);
    TEST_ASSERT_EQUAL_INT(MEMEXT_PEHU_BANKS * 2, ram_n);

    /* PEHU NEMÁ FLASH. */
    int flash_n = count_regions(regs, count, REGION_KIND_MEMEXT_FLASH);
    TEST_ASSERT_EQUAL_INT(0, flash_n);

    /* Poslední PEHU banka 3Fh namapovaná na 0x2000-0x3FFF (addr point
     * 2 a 3): horní půlka je raw kus 7Fh a region se sub_id 7Fh musí
     * číst tutéž buňku, kterou vidí CPU. */
    memext_map_pwrite(2, MEMEXT_PEHU_MASK);
    uint8_t *cpu_upper = memext_get_ram_read_pointer_by_addr_point(3);
    TEST_ASSERT_NOT_NULL(cpu_upper);
    cpu_upper[0x123] = 0xA5;

    int idx = find_region_sub(regs, count, REGION_KIND_MEMEXT_RAM, 0x7F);
    TEST_ASSERT_NOT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_UINT32(MEMEXT_RAW_BANK_SIZE, regs[idx].size);

    uint8_t b = 0;
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_read(regs[idx].id, 0x123, &b, 1));
    TEST_ASSERT_EQUAL_HEX8(0xA5, b);
}


/* ================================================================
 * Test 9: enumerate respektuje max_count (clip + ID stabilita)
 * ================================================================ */

void test_regions_enumerate_clip(void)
{
    /* Malé pole - dostane jen prvních N regionů. */
    st_REGION_DESC small[3];
    int n = dbgapi_regions_enumerate(small, 3);
    TEST_ASSERT_EQUAL_INT(3, n);

    /* ID 0, 1, 2. */
    TEST_ASSERT_EQUAL_INT(0, small[0].id);
    TEST_ASSERT_EQUAL_INT(1, small[1].id);
    TEST_ASSERT_EQUAL_INT(2, small[2].id);

    /* Velké pole - vrátí všechny. */
    st_REGION_DESC large[64];
    int m = dbgapi_regions_enumerate(large, 64);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, m);

    /* První 3 záznamy se musí shodovat (deterministické pořadí). */
    TEST_ASSERT_EQUAL_INT(small[0].kind, large[0].kind);
    TEST_ASSERT_EQUAL_INT(small[1].kind, large[1].kind);
    TEST_ASSERT_EQUAL_INT(small[2].kind, large[2].kind);

    /* Chybné parametry: NULL nebo max_count <= 0 vrací -1. */
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_enumerate(NULL, 64));
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_enumerate(large, 0));
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_enumerate(large, -1));
}


/* ================================================================
 * Test 10: read/write s neplatným region_id vrací -1
 * ================================================================ */

void test_regions_read_write_invalid_id(void)
{
    st_REGION_DESC regs[64];
    int count = dbgapi_regions_enumerate(regs, 64);
    TEST_ASSERT_GREATER_THAN_INT(0, count);

    uint8_t buf[4] = { 0 };

    /* Záporné ID. */
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_read(-1, 0, buf, 4));
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_write(-1, 0, buf, 4));

    /* ID nad count. */
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_read(count + 100, 0, buf, 4));
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_write(count + 100, 0, buf, 4));

    /* len=0. */
    int valid_id = find_region(regs, count, REGION_KIND_RAM);
    TEST_ASSERT_EQUAL_INT(-1, dbgapi_regions_read(valid_id, 0, buf, 0));

    /* offset >= size = clamp na 0 přečtených bajtů (success ale 0). */
    TEST_ASSERT_EQUAL_INT(0,
        dbgapi_regions_read(valid_id, MEMORY_SIZE_RAM, buf, 4));

    /* offset + len > size = clamp na zbývající (vrací < len ale > 0). */
    int n = dbgapi_regions_read(valid_id, MEMORY_SIZE_RAM - 2, buf, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
}


/* ================================================================
 * Test: REGION_KIND_RAM s pripojenym Memextem musi vracet skutecna
 *       data viditelna CPU (= pres memram_read[] indirection), nikoliv
 *       prazdne g_memory.RAM[] pole.
 *
 * Regrese pro RAM-zero bug: do Memory Browser User RAM regionu se
 * misto kodu zobrazovaly samé 0x00. Pricina: stary kod cetl primo
 * &g_memory.RAM[offset] memcpy, ale MZ-800 (a 700/1500) s Memextem
 * zapisuje CPU writy do memext bank pole, g_memory.RAM[] zustane
 * prazdne (bss zeros).
 *
 * Test:
 *   1. Pripoj Luftner (memext_connect)
 *   2. Memext volá memory_reconnect_ram() ktere prepoji memram_*
 *      pointery na memext bank pole
 *   3. Zapis znamy vzor pres memram_write (= simulace CPU write)
 *   4. Read pres dbgapi_regions_read MUSÍ vratit tytéž bajty
 *   5. g_memory.RAM[] zustane prazdne (= dukaz ze pre-fix kod by
 *      vratil 0)
 * ================================================================ */

void test_regions_read_ram_via_memext_banking(void)
{
    /* Pripoj Memext - memory_reconnect_ram() prebudovat memram_* na
     * memext bank pointery. */
    memext_connect(MEMEXT_TYPE_LUFTNER);
    TEST_ASSERT_TRUE(MEMEXT_TEST_CONNECTED);

    /* Sanity: memram_read[1] (= bank pro adresy 0x1000-0x1FFF) uz
     * neukazuje na &g_memory.RAM[0x1000] (nyni ma ukazovat do memext
     * bank pole). Bez teto indirekce by pre-fix bug nenastal. */
    TEST_ASSERT_TRUE(g_memory.memram_read[1] != &g_memory.RAM[0x1000]);
    TEST_ASSERT_TRUE(g_memory.memram_write[1] != &g_memory.RAM[0x1000]);

    /* Predtim vynuluj cilove pole g_memory.RAM (predchozí testy nebo
     * memext init mohly nechat neco). */
    g_memory.RAM[0x1234] = 0x00;
    g_memory.RAM[0x1235] = 0x00;
    g_memory.RAM[0x1236] = 0x00;
    g_memory.RAM[0x1237] = 0x00;

    /* Zapis znamy vzor pres CPU-equivalent path (memram_write[]) - to
     * je presne co dela MZ-800 CPU pri OUT/POKE/program load. Toto pisi
     * do memext bank pole (NE do g_memory.RAM[]). */
    g_memory.memram_write[0x1][0x234] = 0xDE;
    g_memory.memram_write[0x1][0x235] = 0xAD;
    g_memory.memram_write[0x1][0x236] = 0xBE;
    g_memory.memram_write[0x1][0x237] = 0xEF;

    /* Dukaz ze g_memory.RAM[] zustane prazdne (pre-fix bug pricina:
     * stary kod cetl odsud, vracel zeros). */
    TEST_ASSERT_EQUAL_HEX8(0x00, g_memory.RAM[0x1234]);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_memory.RAM[0x1235]);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_memory.RAM[0x1236]);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_memory.RAM[0x1237]);

    /* Read pres REGION_KIND_RAM musi vratit skutecna data, ne nuly. */
    static st_REGION_DESC regs[512];
    int count = dbgapi_regions_enumerate(regs, 512);
    int ram_id = find_region(regs, count, REGION_KIND_RAM);
    TEST_ASSERT_NOT_EQUAL_INT(-1, ram_id);

    uint8_t buf[4];
    int n = dbgapi_regions_read(ram_id, 0x1234, buf, 4);
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_HEX8(0xDE, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, buf[3]);

    /* Write pres REGION_KIND_RAM musi taky pouzit memram_write[] - jinak
     * Memory Browser edit by tise nezapsal nic (zapsal do g_memory.RAM[]
     * coz CPU nikdy nepouzije). */
    uint8_t wd[2] = { 0x42, 0x43 };
    int wn = dbgapi_regions_write(ram_id, 0x2000, wd, 2);
    TEST_ASSERT_EQUAL_INT(2, wn);

    /* Overit ze write se projevil na CPU-viditelne strane (memram_read[]),
     * nikoliv jen v g_memory.RAM[]. */
    TEST_ASSERT_EQUAL_HEX8(0x42, g_memory.memram_read[0x2][0x000]);
    TEST_ASSERT_EQUAL_HEX8(0x43, g_memory.memram_read[0x2][0x001]);
}


/* ================================================================
 * Test: User RAM region je transparentní vůči ROM swap (= žádné
 *       Memextu, ale jednotlivé bankovní stránky simulujeme změnou
 *       memram_read[bank] na ROM zatímco memram_write[bank] zůstává
 *       na User RAM. User RAM region MUSÍ vracet User RAM, ne ROM.
 *
 * Per Michalovu specifikaci: "User RAM je 0000-FFFF paměť RAM... je to
 * úplně stejné, jako Logical Z80 view v případě, že by bylo zrovna
 * všude RAM". Tj. ROM swap je v tomto pohledu transparentní.
 *
 * Bez reálného banking (= memory_reconnect_ram je voláno pouze přes
 * memext_connect/disconnect; běžný MZ-800 ROM swap je řízen inline
 * makry MEMORY_MZ800_MAP_TEST_ROM_*, ne přes memram_read[] pointer)
 * stačí simulovat divergenci read/write pointerů ručně.
 * ================================================================ */

void test_regions_read_ram_user_ram_transparent_to_rom_swap(void)
{
    /* Bez Memextu: memram_read[i] == memram_write[i] == &RAM[i<<12].
     * Simulujeme stav "stránka 0 má ROM swap" tak, že read pointer
     * přepíšeme na ROM, zatímco write pointer ponecháme na User RAM. */
    uint8_t *saved_read_0 = g_memory.memram_read[0];

    /* Zapiš známá data do User RAM stránky 0. */
    g_memory.RAM[0x0000] = 0x11;
    g_memory.RAM[0x0001] = 0x22;
    g_memory.RAM[0x0002] = 0x33;

    /* Zapiš odlišná data do ROM (= druhý zdroj na čtení). */
    g_memory.ROM[0x0000] = 0xAA;
    g_memory.ROM[0x0001] = 0xBB;
    g_memory.ROM[0x0002] = 0xCC;

    /* Simuluj ROM swap: read jde do ROM, write pořád do User RAM
     * (= write-through pattern MZ-800 ROM swap). */
    g_memory.memram_read[0] = &g_memory.ROM[0];

    static st_REGION_DESC regs[512];
    int count = dbgapi_regions_enumerate(regs, 512);
    int ram_id = find_region(regs, count, REGION_KIND_RAM);
    TEST_ASSERT_NOT_EQUAL_INT(-1, ram_id);

    /* User RAM region MUSÍ vracet User RAM (0x11/22/33), ne ROM
     * (0xAA/BB/CC). Implementace přes memram_write[] to garantuje. */
    uint8_t buf[3];
    int n = dbgapi_regions_read(ram_id, 0x0000, buf, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x33, buf[2]);

    /* Cleanup: vratit read pointer (jinak ovlivnime dalsi testy). */
    g_memory.memram_read[0] = saved_read_0;
}


/* ================================================================
 * Test: User RAM s Memext Luftner + FLASH bank namapovaná na stránku.
 *       Read pres User RAM region MUSI vratit WOM (= write target),
 *       ne FLASH content. Semanticky: FLASH bank je "ROM-like" cil,
 *       neni soucasti User RAM image.
 * ================================================================ */

void test_regions_read_ram_user_ram_memext_flash_bank(void)
{
    memext_connect(MEMEXT_TYPE_LUFTNER);
    TEST_ASSERT_TRUE(MEMEXT_TEST_CONNECTED);

    /* Namapuj FLASH bank 0 (rawbank 0x80) do address pointu 2
     * (= adresy 0x2000-0x2FFF). */
    memext_map_pwrite(2, 0x80);

    /* Sanity: memram_read[2] ukazuje do FLASH, memram_write[2] do WOM. */
    TEST_ASSERT_TRUE(g_memory.memram_read[2] != g_memory.memram_write[2]);

    /* Naplnime FLASH bank 0 znamym vzorem (primy zapis do FLASH
     * pole - simuluje predchozi flash programming). */
    g_memext.FLASH[0x000] = 0x55;
    g_memext.FLASH[0x001] = 0x66;
    g_memext.FLASH[0x002] = 0x77;

    /* A do WOM napiseme jiny vzor (= "kdyby CPU zapsalo do RAM"
     * write-only ztratil do WOM scratch). */
    g_memext.WOM[0x000] = 0xE0;
    g_memext.WOM[0x001] = 0xE1;
    g_memext.WOM[0x002] = 0xE2;

    /* Sanity: read pres memram_read by vratil FLASH (= co CPU vidi
     * pri RD), read pres memram_write WOM (= kam CPU pise). */
    TEST_ASSERT_EQUAL_HEX8(0x55, g_memory.memram_read[2][0x000]);
    TEST_ASSERT_EQUAL_HEX8(0xE0, g_memory.memram_write[2][0x000]);

    static st_REGION_DESC regs[512];
    int count = dbgapi_regions_enumerate(regs, 512);
    int ram_id = find_region(regs, count, REGION_KIND_RAM);
    TEST_ASSERT_NOT_EQUAL_INT(-1, ram_id);

    /* User RAM region MUSI vratit WOM (= write target), ne FLASH
     * (= read target). Per Michalovu specifikaci: "User RAM je
     * paměť RAM" - FLASH bank ROM-like cil sem nepatri, sledujeme
     * write-side image (= memram_write). */
    uint8_t buf[3];
    int n = dbgapi_regions_read(ram_id, 0x2000, buf, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0xE0, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xE1, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xE2, buf[2]);
}


/* ================================================================
 * Main
 * ================================================================ */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* MUSÍ být první - testuje prázdnou cache (viz komentář u testu). */
    RUN_TEST(test_regions_read_without_enumerate_autofills);

    /* smoke */
    RUN_TEST(test_regions_enumerate_basic);
    RUN_TEST(test_regions_read_ram_no_se);
    RUN_TEST(test_regions_read_rom_lower);

    /* unit */
    RUN_TEST(test_regions_memext_not_connected);
    RUN_TEST(test_regions_memext_luftner_connected);
    RUN_TEST(test_regions_prohibited_shadow);
    RUN_TEST(test_regions_vram_plane);
    RUN_TEST(test_regions_vram_700_offsets);
    RUN_TEST(test_regions_vram_700_full_2k);
    RUN_TEST(test_regions_ramdisk_pezik_e8);
    RUN_TEST(test_regions_ramdisk_std_shrink_stale);
    RUN_TEST(test_regions_memext_pehu_bank_count);
    RUN_TEST(test_regions_enumerate_clip);
    RUN_TEST(test_regions_read_write_invalid_id);
    RUN_TEST(test_regions_read_ram_via_memext_banking);
    RUN_TEST(test_regions_read_ram_user_ram_transparent_to_rom_swap);
    RUN_TEST(test_regions_read_ram_user_ram_memext_flash_bank);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
