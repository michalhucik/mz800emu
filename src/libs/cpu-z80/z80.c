/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: MIT
 * https://github.com/michalhucik/z80-mz800
 */
/**
 * @file z80.c
 * @brief cpu-z80 multi-v0.2 - Přesný a rychlý multi-instance Z80A emulátor.
 *
 * Callbacky jsou uloženy v z80_t struktuře - každá instance má vlastní.
 * V z80_execute() se callback pointery cachují do lokálních proměnných
 * pro eliminaci opakované dereference cpu-> (nulový overhead).
 *
 * Optimalizace:
 * - Lokální cache callback pointerů v z80_execute()
 * - Computed goto dispatch (GCC/Clang)
 * - Lokální registrová cache
 * - GCC atributy: hot, likely/unlikely
 * - Eliminace null-checků callbacků (default handlery)
 * - DAA lookup tabulka (2048 záznamů)
 * - Inline prefix handlery (CB/ED/DD/FD/DDCB/FDCB)
 *
 * @version multi-v0.5
 */

#include "z80.h"
#include <string.h>
#include <stdlib.h>

/* Compiler hints pro agresivní optimalizaci */
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define HOT         __attribute__((hot))
#define NOINLINE    __attribute__((noinline))
#define ALWAYS_INLINE __attribute__((always_inline)) static inline
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#define HOT
#define NOINLINE
#define ALWAYS_INLINE static inline
#endif

/* ========== Default callbacky ========== */

/**
 * @brief Default callback pro čtení z paměti - vrací 0xFF (odpojený bus).
 * @param cpu Ukazatel na CPU instanci (ignorován).
 * @param addr Adresa (ignorovaná).
 * @param m1_state M1 stav (ignorován).
 * @param user_data User data (ignorována).
 * @return Vždy 0xFF.
 */
static uint8_t default_mread(z80_t *cpu, uint16_t addr, int m1_state, void *user_data) {
    (void)cpu; (void)addr; (void)m1_state; (void)user_data; return 0xFF;
}

/**
 * @brief Default callback pro zápis do paměti - no-op.
 * @param cpu Ukazatel na CPU instanci (ignorován).
 * @param addr Adresa (ignorovaná).
 * @param value Data (ignorována).
 * @param user_data User data (ignorována).
 */
static void default_mwrite(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data) {
    (void)cpu; (void)addr; (void)value; (void)user_data;
}

/**
 * @brief Default callback pro čtení z I/O portu - vrací 0xFF.
 * @param cpu Ukazatel na CPU instanci (ignorován).
 * @param port Adresa portu (ignorovaná).
 * @param user_data User data (ignorována).
 * @return Vždy 0xFF.
 */
static uint8_t default_pread(z80_t *cpu, uint16_t port, void *user_data) {
    (void)cpu; (void)port; (void)user_data; return 0xFF;
}

/**
 * @brief Default callback pro zápis na I/O port - no-op.
 * @param cpu Ukazatel na CPU instanci (ignorován).
 * @param port Adresa portu (ignorovaná).
 * @param value Data (ignorována).
 * @param user_data User data (ignorována).
 */
static void default_pwrite(z80_t *cpu, uint16_t port, uint8_t value, void *user_data) {
    (void)cpu; (void)port; (void)value; (void)user_data;
}

/* ========== Lookup tabulky ========== */

/** Paritní tabulka: PF pokud sudý počet jedničkových bitů */
static uint8_t parity_table[256];

/** SZ53P tabulka: flagy S, Z, F5, F3, P pro danou hodnotu */
static uint8_t sz53p_table[256];

/** SZ53 tabulka: flagy S, Z, F5, F3 (bez parity) */
static uint8_t sz53_table[256];

/**
 * @brief DAA lookup tabulka.
 *
 * 2048 záznamů indexovaných: A | (carry << 8) | (half << 9) | (sub << 10).
 * Každý záznam obsahuje novou hodnotu A (dolních 8 bitů) a nový F (horních 8 bitů).
 */
static uint16_t daa_table[2048];

/* Flagové konstanty - krátké aliasy */
#define CF  Z80_FLAG_C
#define NF  Z80_FLAG_N
#define PF  Z80_FLAG_PV
#define F3  Z80_FLAG_3
#define HF  Z80_FLAG_H
#define F5  Z80_FLAG_5
#define ZF  Z80_FLAG_Z
#define SF  Z80_FLAG_S

/**
 * @brief Inicializace všech lookup tabulek.
 *
 * Generuje parity_table, sz53_table, sz53p_table a daa_table.
 * Voláno jednou při prvním z80_init().
 */
static void init_tables(void) {
    /* Parita, SZ53, SZ53P */
    for (int i = 0; i < 256; i++) {
        int bits = 0;
        for (int b = 0; b < 8; b++) {
            if (i & (1 << b)) bits++;
        }
        parity_table[i] = (bits % 2 == 0) ? PF : 0;

        sz53_table[i] = (i & SF) | (i & F5) | (i & F3);
        if (i == 0) sz53_table[i] |= ZF;

        sz53p_table[i] = sz53_table[i] | parity_table[i];
    }

    /* DAA tabulka */
    for (int idx = 0; idx < 2048; idx++) {
        uint8_t a = (uint8_t)(idx & 0xFF);
        int carry_in = (idx >> 8) & 1;
        int half_in = (idx >> 9) & 1;
        int sub = (idx >> 10) & 1;

        uint8_t correction = 0;
        uint8_t carry_out = 0;
        uint8_t hf_out = 0;

        if ((a & 0x0F) > 9 || half_in) correction = 0x06;
        if (a > 0x99 || carry_in) { correction |= 0x60; carry_out = CF; }

        uint8_t new_a;
        if (sub) {
            hf_out = ((correction & 0x0F) > (a & 0x0F)) ? HF : 0;
            new_a = a - correction;
        } else {
            hf_out = ((a & 0x0F) + (correction & 0x0F)) >= 0x10 ? HF : 0;
            new_a = a + correction;
        }

        uint8_t new_f = sz53p_table[new_a] | carry_out | hf_out | (sub ? NF : 0);
        daa_table[idx] = (uint16_t)new_a | ((uint16_t)new_f << 8);
    }
}

static bool tables_initialized = false;

/*
 * Inline přístup k paměti používaný MIMO z80_execute()
 * (handle_interrupts_internal). Uvnitř z80_execute() se používají
 * makra RD/WR/IO_RD/IO_WR s lokální cache callback pointerů.
 */

/** Čtení bajtu z paměti (pro použití mimo execute loop). */
static inline uint8_t rd_slow(z80_t *cpu, uint16_t addr) {
    return cpu->mread_cb(cpu, addr, 0, cpu->mread_data);
}

/** Zápis bajtu do paměti (pro použití mimo execute loop). */
static inline void wr_slow(z80_t *cpu, uint16_t addr, uint8_t data) {
    cpu->mwrite_cb(cpu, addr, data, cpu->mwrite_data);
}

/** Čtení 16bitové hodnoty (pro použití mimo execute loop). */
static inline uint16_t rd16_slow(z80_t *cpu, uint16_t addr) {
    uint8_t lo = rd_slow(cpu, addr);
    uint8_t hi = rd_slow(cpu, addr + 1);
    return (uint16_t)(lo | (hi << 8));
}

/**
 * @brief Uložení 16bitové hodnoty na zásobník (mimo execute loop: přijetí INT a NMI).
 *
 * Pořadí zápisů podle Zilog UM0080 (PUSH qq, CALL, RST, NMI): nejdřív
 * vyšší bajt na @p addr + 1 (= SP před přijetím - 1), pak nižší bajt na
 * @p addr. Obsah paměti je little endian jako u ostatních zápisů; pořadí
 * vidí callback zápisu do paměti (záznam zápisů, breakpointy MEM_W).
 *
 * @param cpu  CPU.
 * @param addr Nový vrchol zásobníku (SP už snížený o 2).
 * @param val  Ukládaná hodnota (návratová adresa).
 */
static inline void wr16_stack_slow(z80_t *cpu, uint16_t addr, uint16_t val) {
    wr_slow(cpu, (uint16_t)(addr + 1), (uint8_t)(val >> 8));
    wr_slow(cpu, addr, (uint8_t)(val & 0xFF));
}

/* Forward deklarace - použita v NEXT makru uvnitř z80_execute() */
static int handle_interrupts_internal(z80_t *cpu);

#ifdef MZ800EMU_CFG_RAM_FASTPATH
/*
 * RAM-access fast-path helpery (E1, KROK 1 návrhu D3). Definovány JEDNOU na
 * úrovni TU (ne v z80_execute.inc, který se includuje 2x) - sdíleny batch i
 * step variantou. Volané z RD/WR/FETCH/M1_FETCH maker; op_tstate inkrementuje
 * volající makro (timing zachován), tady jen samotný přístup + side efekty.
 */

/**
 * @brief Fast-path RAM read (bez op_tstate tiku - ten dělá volající makro).
 *
 * @param cpu CPU instance.
 * @param addr 16bit adresa.
 * @return Přečtená hodnota, je-li stránka čistá RAM; jinak fall-back na
 *         cpu->mread_cb (= bit-identický s baseline, včetně sync/latch/CDL).
 * @post Je-li cesta čistá RAM a ram_fp_dbus_latch != NULL, latch = retval
 *       (replikace memory_read_cb side-efektu regDBUS_latch).
 * @note m1_state se ignoruje stejně jako v memory_read_cb (logging cb, který
 *       m1_state čte, je v debug režimu = ram_fp_enabled false -> sem nevejde).
 */
#ifdef MZ800EMU_CFG_RAM_FASTPATH_VERIFY
/*
 * Diff-verify mód (E1 důkaz přesnosti): globální čítače + assert.
 * z80_fp_read/write spustí fast-path I kanonický callback a porovná výsledky
 * (hodnota + regDBUS_latch). Při neshodě abort s diagnostikou. Pomalé - jen
 * pro důkaz ekvivalence na reálném běhu, ne pro perf.
 */
#include <stdio.h>
#include <stdlib.h>
unsigned long long g_z80_fp_verify_reads  = 0;  /**< Počet ověřených RAM čtení. */
unsigned long long g_z80_fp_verify_writes = 0;  /**< Počet ověřených RAM zápisů. */
unsigned long long g_z80_fp_verify_fastpath = 0; /**< Z toho kolik šlo fast-path větví. */

#include <signal.h>

/**
 * @brief Report verify čítačů (jen verify build).
 *
 * Vypíše na stderr počet ověřených přístupů. Pokud došlo k neshodě, proces
 * už dávno abortoval - tento report tedy běží jen při ukončení bez neshody =
 * implicitní důkaz "0 neshod na N přístupech". Volán z atexit i ze SIGINT/
 * SIGTERM handleru (headless emu se ukončuje signálem, atexit pak neběží).
 */
void z80_fp_verify_report(void) {
    fprintf(stderr,
        "[RAM_FASTPATH_VERIFY] clean (no mismatch): reads=%llu writes=%llu "
        "fastpath_hits=%llu\n",
        g_z80_fp_verify_reads, g_z80_fp_verify_writes,
        g_z80_fp_verify_fastpath);
    fflush(stderr);
}

/** Signal handler: vypíše verify report a ukončí proces (headless SIGINT/TERM). */
static void z80_fp_verify_sigreport(int sig) {
    z80_fp_verify_report();
    _exit(sig == SIGINT ? 130 : 143);
}
#endif

ALWAYS_INLINE uint8_t z80_fp_read(z80_t *cpu, uint16_t addr, int m1_state) {
#ifdef MZ800EMU_CFG_RAM_FASTPATH_VERIFY
    /*
     * Verify: spočti fast-path hodnotu (bez latch update), pak zavolej
     * kanonický callback (= nastaví latch sám), porovnej. Latch po callbacku
     * MUSÍ být roven fast-path hodnotě (memory_read_cb nastavuje latch =
     * retval pro RAM), jinak fast-path latch replikace není bit-identická.
     */
    g_z80_fp_verify_reads++;
    /* Periodický report (1. přístup + pak každých 10M) - důkaz "běží + 0
     * mismatchů" i když se proces ukončí signálem (headless) bez atexit. */
    if (g_z80_fp_verify_reads == 1
        || (g_z80_fp_verify_reads % 1000000ULL) == 0) z80_fp_verify_report();
    if (cpu->ram_fp_enabled) {
        uint8_t *page = cpu->ram_fp_read[addr >> 12];
        if (page) {
            uint8_t fp_v = page[addr & 0x0FFF];
            uint8_t cb_v = cpu->mread_cb(cpu, addr, m1_state, cpu->mread_data);
            uint8_t cb_latch = cpu->ram_fp_dbus_latch ? *cpu->ram_fp_dbus_latch : cb_v;
            g_z80_fp_verify_fastpath++;
            if (fp_v != cb_v || (cpu->ram_fp_dbus_latch && cb_latch != fp_v)) {
                fprintf(stderr,
                    "[RAM_FASTPATH_VERIFY] READ MISMATCH addr=%04X m1=%d "
                    "fp=%02X cb=%02X latch=%02X\n",
                    addr, m1_state, fp_v, cb_v, cb_latch);
                abort();
            }
            return cb_v; /* kanonický výsledek (latch už nastaven callbackem) */
        }
    }
    return cpu->mread_cb(cpu, addr, m1_state, cpu->mread_data);
#else
    if (cpu->ram_fp_enabled) {
        uint8_t *page = cpu->ram_fp_read[addr >> 12];
        if (page) {
            uint8_t v = page[addr & 0x0FFF];
            if (cpu->ram_fp_dbus_latch) *cpu->ram_fp_dbus_latch = v;
            return v;
        }
    }
    return cpu->mread_cb(cpu, addr, m1_state, cpu->mread_data);
#endif
}

/**
 * @brief Fast-path RAM write (bez op_tstate tiku - ten dělá volající makro).
 *
 * @param cpu CPU instance.
 * @param addr 16bit adresa.
 * @param val Zapisovaná hodnota.
 * @post Je-li stránka čistá RAM, zapíše přímo; jinak fall-back na
 *       cpu->mwrite_cb (bit-identický s baseline). Čistá RAM write nemá
 *       žádný vedlejší efekt (memory_write_cb pro RAM jen uloží byte).
 */
ALWAYS_INLINE void z80_fp_write(z80_t *cpu, uint16_t addr, uint8_t val) {
#ifdef MZ800EMU_CFG_RAM_FASTPATH_VERIFY
    /*
     * Verify: proveď kanonický callback zápis, pak ověř že fast-path cílová
     * buňka (ram_fp_write[page]+off) obsahuje zapsanou hodnotu = fast-path by
     * zapsal na TOTÉŽ místo. Chytí záměnu banku / stale tabulku po bankingu.
     * Pozn: tento test je validní jen když callback zapsal do RAM (KIND_RAM
     * stránka); pro non-RAM (write potlačený ROM/VRAM jinou cestou) fast-path
     * page == NULL, takže sem nevejde.
     */
    g_z80_fp_verify_writes++;
    if (cpu->ram_fp_enabled) {
        uint8_t *page = cpu->ram_fp_write[addr >> 12];
        if (page) {
            g_z80_fp_verify_fastpath++;
            cpu->mwrite_cb(cpu, addr, val, cpu->mwrite_data);
            uint8_t after = page[addr & 0x0FFF];
            if (after != val) {
                fprintf(stderr,
                    "[RAM_FASTPATH_VERIFY] WRITE MISMATCH addr=%04X "
                    "val=%02X fp_cell_after_cb=%02X\n",
                    addr, val, after);
                abort();
            }
            return;
        }
    }
    cpu->mwrite_cb(cpu, addr, val, cpu->mwrite_data);
#else
    if (cpu->ram_fp_enabled) {
        uint8_t *page = cpu->ram_fp_write[addr >> 12];
        if (page) {
            page[addr & 0x0FFF] = val;
            return;
        }
    }
    cpu->mwrite_cb(cpu, addr, val, cpu->mwrite_data);
#endif
}
#endif /* MZ800EMU_CFG_RAM_FASTPATH */

/*
 * Dvě varianty jádra generované z z80_execute.inc:
 *   z80_execute_batch - registrová cache (původní chování, z80_execute API)
 *   z80_execute_step  - registry přímo v cpu-> (per-step, volaná z z80_step)
 */
HOT int z80_execute_batch(z80_t *cpu, int target_cycles);
HOT int z80_execute_step(z80_t *cpu, int target_cycles);

/* ========== Computed goto podpora ========== */

#if defined(__GNUC__) || defined(__clang__)
#define USE_COMPUTED_GOTO 1
#else
#define USE_COMPUTED_GOTO 0
#endif

/* ========== Hlavní emulační smyčka ========== */


/*
 * Tělo emulační smyčky je vyčleněné do z80_execute.inc a includované DVAKRÁT
 * (varianta A návrhu A4): jednou jako batch jádro (registrová cache) a jednou
 * jako per-step jádro bez cache (registry přímo v cpu->). Sdílený zdroj =
 * žádná textová duplikace handlerů, žádná diverze při budoucích opravách.
 * Sdílené tabulky (sz53_table aj.) a helpery (handle_interrupts_internal,
 * rd_slow) jsou static na úrovni tohoto TU -> jedna kopie, žádný ODR problém.
 */

/* 1) Batch varianta (původní chování) - volaná z veřejného z80_execute(). */
#define Z80_EXEC_FN     z80_execute_batch
#define Z80_DIRECT_REGS 0
#include "z80_execute.inc"
#undef Z80_EXEC_FN
#undef Z80_DIRECT_REGS

/* 2) Per-step varianta bez registrové cache - volaná z z80_step(). */
#define Z80_EXEC_FN     z80_execute_step
#define Z80_DIRECT_REGS 1
#include "z80_execute.inc"
#undef Z80_EXEC_FN
#undef Z80_DIRECT_REGS

/**
 * @brief Provedení instrukcí po dobu daného počtu T-stavů (veřejné API).
 *
 * Tenký wrapper na batch variantu jádra (z80_execute_batch). Zachovává
 * původní veřejný kontrakt; volající mimo per-1 hot loop (např. ne-cycle-
 * accurate dávkové provádění) používají tuto entry.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param target_cycles Cílový počet T-stavů.
 * @return Skutečný počet provedených T-stavů.
 * @pre cpu != NULL, z80_init() bylo voláno.
 */
HOT int z80_execute(z80_t *cpu, int target_cycles) {
    return z80_execute_batch(cpu, target_cycles);
}

/* ========== Zpracování přerušení ========== */

/**
 * @brief Inkrement registru R za M1 cyklus potvrzení přerušení (INT i NMI).
 *
 * Přijetí INT (IM 0/1/2) i NMI začíná M1 cyklem s refreshem, proto R vzroste
 * o 1 (báze mz800-knowledge cpu/z80/09-r-register.md, tabulka "Přijetí INT
 * a NMI"; z80ex dělá totéž). Mění se jen dolních 7 bitů, bit 7 zůstává.
 * Chybějící inkrement byl regresí v2.0.2 (loader hry Interkarate odvozuje
 * dešifrovací klíč z R po HALT čekajícím na přerušení).
 *
 * @param cpu Ukazatel na CPU instanci (pracuje se strukturou, ne s locals).
 * @pre Voláno mimo běh z80_execute() nebo po WRITEBACK() (cpu->r je aktuální).
 * @post cpu->r = (r & 0x80) | ((r + 1) & 0x7F).
 */
static inline void inc_r_on_int_ack(z80_t *cpu) {
    cpu->r = (uint8_t)((cpu->r & 0x80) | ((cpu->r + 1) & 0x7F));
}

/**
 * @brief Interní handler přerušení.
 *
 * Zpracovává NMI a maskované přerušení (IM0/1/2).
 * Volán z execute smyčky - pracuje přímo se strukturou (ne s locals).
 * Při přijetí NMI i INT zvýší R o 1 (inc_r_on_int_ack) a vynuluje
 * registr Q (cpu->q).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @return Počet T-stavů spotřebovaných obsluhou, nebo 0.
 */
static int handle_interrupts_internal(z80_t *cpu) {
    int int_cycles = 0;

#ifdef Z80_NO_EI_DELAY
    bool ei_blocked = false;
    cpu->ei_delay = false;
#else
    bool ei_blocked = cpu->ei_delay;
    cpu->ei_delay = false;
#endif

    /* NMI má vyšší prioritu.
     *
     * NMI ack: IFF2 si zachová kopii původního IFF1 (-> RETN ho obnoví),
     * IFF1 -> 0. Pozn: IFF2 se "mění" jen pokud je původní iff1 != iff2,
     * což na běžně použité konfiguraci nenastane (EI/DI mění všechno
     * spolu). Fire iff_change vždy - reason NMI_ACK signalizuje
     * sémantiku, ne nutně reálnou změnu IFF2.
     *
     * Knowledge base reference: ~/projects/mz800-knowledge/public/reference/agent/
     * cpu/z80/07-interrupts.md řádek 24-25, 127.
     */
    if (cpu->nmi_pending) {
        bool was_halted = cpu->halted;
        inc_r_on_int_ack(cpu);
        cpu->nmi_pending = false;
        cpu->halted = false;
        cpu->iff2 = cpu->iff1;
        cpu->iff1 = 0;
        /* BUG1 fix: během HALT je cpu->pc = X (adresa instrukce HALT, viz
         * op_76). Před uložením návratové adresy ho inkrementujeme na X+1
         * (= adresa za HALT) - reálný Z80 inkrementuje PC před skokem do
         * rutiny. Tím push i HALT_EXIT event vidí X+1 (sémantika beze změny). */
        if (was_halted) cpu->pc++;
        cpu->sp -= 2;
        wr16_stack_slow(cpu, cpu->sp, cpu->pc);
        /*
         * Fire HALT_EXIT před změnou cpu->pc (= předáme adresu za HALT,
         * kam by se CPU vrátilo bez NMI). Až pak update cpu->pc na vektor.
         */
        if (was_halted && cpu->cpu_ctrl_event_cb) {
            cpu->cpu_ctrl_event_cb(cpu, (uint8_t)Z80_CPU_CTRL_HALT_EXIT,
                                   cpu->pc, cpu->cpu_ctrl_event_data);
        }
        cpu->pc = 0x0066;
        cpu->wz.w = cpu->pc;
        int_cycles = 11;
        if (cpu->iff_change_cb) {
            cpu->iff_change_cb(cpu, cpu->iff1, cpu->iff2,
                               (uint8_t)Z80_IFF_REASON_NMI_ACK,
                               cpu->iff_change_data);
        }
    }
    /* Maskované přerušení - blokováno po EI.
     *
     * INT ack pokrývá všechny IM 0/1/2 - Z80 při ack INT vždy clearuje
     * IFF1+IFF2 nezávisle na IM mode (Zilog Z80 manual, Sean Young Z80
     * doc). IM mode jen ovlivňuje co se vykoná po clearu.
     *
     * Knowledge base reference: ~/projects/mz800-knowledge/public/reference/agent/
     * cpu/z80/07-interrupts.md řádek 42-43, 105.
     */
    else if (cpu->iff1 && !ei_blocked) {
        if (!cpu->int_pending) {
            return 0;
        }
        bool was_halted = cpu->halted;
        inc_r_on_int_ack(cpu);
        cpu->int_pending = false;
        cpu->halted = false;
        cpu->iff1 = 0;
        cpu->iff2 = 0;

        /* BUG1 fix: během HALT je cpu->pc = X (adresa instrukce HALT, viz
         * op_76). Před HALT_EXIT eventem i pushem v switch(im) níže ho
         * inkrementujeme na X+1 (= adresa za HALT, návratová adresa) -
         * reálný Z80 inkrementuje PC před skokem do rutiny. */
        if (was_halted) cpu->pc++;

        /* HALT exit fire jen při skutečném přechodu 1->0 (= IRQ před INT
         * mezi běžnými instrukcemi nesmí fire HALT_EXIT). PC je za HALT
         * instrukcí, tedy adresa kam by se CPU vrátilo. */
        if (was_halted && cpu->cpu_ctrl_event_cb) {
            cpu->cpu_ctrl_event_cb(cpu, (uint8_t)Z80_CPU_CTRL_HALT_EXIT,
                                   cpu->pc, cpu->cpu_ctrl_event_data);
        }

        if (cpu->intack_cb) cpu->intack_cb(cpu, cpu->intack_data);
        if (cpu->iff_change_cb) {
            cpu->iff_change_cb(cpu, cpu->iff1, cpu->iff2,
                               (uint8_t)Z80_IFF_REASON_INT_ACK,
                               cpu->iff_change_data);
        }

        /* Čtení vektoru přes intread callback (pokud existuje a není nastaven přímo) */
        if (cpu->intread_cb && cpu->int_vector == 0) {
            cpu->int_vector = cpu->intread_cb(cpu, cpu->intread_data);
        }

        switch (cpu->im) {
            case 0:
                cpu->sp -= 2;
                wr16_stack_slow(cpu, cpu->sp, cpu->pc);
                cpu->pc = cpu->int_vector & 0x38;
                cpu->wz.w = cpu->pc;
                int_cycles = 13;
                break;
            case 1:
                cpu->sp -= 2;
                wr16_stack_slow(cpu, cpu->sp, cpu->pc);
                cpu->pc = 0x0038;
                cpu->wz.w = cpu->pc;
                int_cycles = 13;
                break;
            case 2:
                cpu->sp -= 2;
                wr16_stack_slow(cpu, cpu->sp, cpu->pc);
                {
                    uint16_t vec_addr = ((uint16_t)cpu->i << 8) | (cpu->int_vector & 0xFE);
                    cpu->pc = rd16_slow(cpu, vec_addr);
                }
                cpu->wz.w = cpu->pc;
                int_cycles = 19;
                break;
        }
    }

    /* Přijetí přerušení příznaky nezapisuje: instrukce za ním vidí Q = 0. */
    if (int_cycles > 0) cpu->q = 0;

    return int_cycles;
}

int z80_process_interrupt(z80_t *cpu) {
    int cyc = handle_interrupts_internal(cpu);
    if (cyc > 0) {
        cpu->cycles += cyc;
        cpu->total_cycles += cyc;
    }
    return cyc;
}

/* ========== Veřejné API ========== */

/**
 * @brief Inicializace lookup tabulek (při prvním volání).
 *
 * Bezpečné pro volání z více vláken - tabulky jsou sdílené a read-only.
 */
static void ensure_tables(void) {
    if (!tables_initialized) {
        init_tables();
        tables_initialized = true;
    }
}

z80_t *z80_create(
    z80_mread_cb mread, void *mread_data,
    z80_mwrite_cb mwrite, void *mwrite_data,
    z80_pread_cb pread, void *pread_data,
    z80_pwrite_cb pwrite, void *pwrite_data,
    z80_intread_cb intread, void *intread_data
) {
    ensure_tables();

    z80_t *cpu = (z80_t *)malloc(sizeof(z80_t));
    if (!cpu) return NULL;
    memset(cpu, 0, sizeof(z80_t));

#ifdef MZ800EMU_CFG_RAM_FASTPATH_VERIFY
    /* Verify režim: registruj atexit report čítačů (jednou). */
    {
        static bool _verify_atexit_registered = false;
        if (!_verify_atexit_registered) {
            _verify_atexit_registered = true;
            atexit(z80_fp_verify_report);
            signal(SIGINT, z80_fp_verify_sigreport);
            signal(SIGTERM, z80_fp_verify_sigreport);
        }
    }
#endif

    /* Nastavení callbacků */
    cpu->mread_cb   = mread  ? mread  : default_mread;
    cpu->mread_data  = mread_data;
    cpu->mwrite_cb  = mwrite ? mwrite : default_mwrite;
    cpu->mwrite_data = mwrite_data;
    cpu->pread_cb   = pread  ? pread  : default_pread;
    cpu->pread_data  = pread_data;
    cpu->pwrite_cb  = pwrite ? pwrite : default_pwrite;
    cpu->pwrite_data = pwrite_data;
    cpu->intread_cb  = intread;
    cpu->intread_data = intread_data;

    z80_reset(cpu);
    return cpu;
}

void z80_destroy(z80_t *cpu) {
    free(cpu);
}

void z80_reset(z80_t *cpu) {
    /* Zachováme callbacky */
    z80_mread_cb  save_mread  = cpu->mread_cb;
    void         *save_mrd    = cpu->mread_data;
    z80_mwrite_cb save_mwrite = cpu->mwrite_cb;
    void         *save_mwd    = cpu->mwrite_data;
    z80_pread_cb  save_pread  = cpu->pread_cb;
    void         *save_prd    = cpu->pread_data;
    z80_pwrite_cb save_pwrite = cpu->pwrite_cb;
    void         *save_pwd    = cpu->pwrite_data;
    z80_intread_cb save_intread = cpu->intread_cb;
    void          *save_ird    = cpu->intread_data;
    z80_intack_cb  save_intack = cpu->intack_cb;
    void          *save_iad    = cpu->intack_data;
    z80_reti_cb    save_reti   = cpu->reti_cb;
    void          *save_rtd    = cpu->reti_data;
    z80_ei_cb      save_ei     = cpu->ei_cb;
    void          *save_eid    = cpu->ei_data;
    z80_di_cb      save_di     = cpu->di_cb;
    void          *save_did    = cpu->di_data;
    z80_im_cb      save_im     = cpu->im_cb;
    void          *save_imd    = cpu->im_data;
    z80_halt_cb    save_halt   = cpu->halt_cb;
    void          *save_haltd  = cpu->halt_data;
    z80_nmi_cb     save_nmi    = cpu->nmi_cb;
    void          *save_nmid   = cpu->nmi_data;
    z80_iff_change_cb save_iff_change = cpu->iff_change_cb;
    void          *save_iff_changed = cpu->iff_change_data;
    z80_cpu_ctrl_event_cb save_cpu_ctrl = cpu->cpu_ctrl_event_cb;
    void          *save_cpu_ctrld     = cpu->cpu_ctrl_event_data;
    z80_call_cb    save_call   = cpu->call_cb;
    void          *save_calld  = cpu->call_data;
    z80_ret_cb     save_ret    = cpu->ret_cb;
    void          *save_retd   = cpu->ret_data;
    bool save_ext_int = cpu->external_int_handling;
    void (*save_ps)(z80_t *, void *) = cpu->post_step_cb;
    void *save_psd = cpu->post_step_data;

    cpu->af.w = 0xFFFF;
    cpu->bc.w = 0x0000;
    cpu->de.w = 0x0000;
    cpu->hl.w = 0x0000;
    cpu->af2.w = 0x0000;
    cpu->bc2.w = 0x0000;
    cpu->de2.w = 0x0000;
    cpu->hl2.w = 0x0000;
    cpu->ix.w = 0x0000;
    cpu->iy.w = 0x0000;
    cpu->wz.w = 0x0000;
    cpu->sp = 0xFFFF;
    cpu->pc = 0x0000;
    cpu->i = 0x00;
    cpu->r = 0x00;
    cpu->iff1 = 0;
    cpu->iff2 = 0;
    cpu->im = 0;
    cpu->halted = false;
    cpu->int_pending = false;
    cpu->nmi_pending = false;
    cpu->ei_delay = false;
    cpu->ld_a_ir = false;
    cpu->cycles = 0;
    cpu->total_cycles = 0;
    cpu->wait_cycles = 0;
    cpu->op_tstate = 0;
    cpu->q = 0;

    /* Obnovíme callbacky */
    cpu->mread_cb   = save_mread;   cpu->mread_data  = save_mrd;
    cpu->mwrite_cb  = save_mwrite;  cpu->mwrite_data = save_mwd;
    cpu->pread_cb   = save_pread;   cpu->pread_data  = save_prd;
    cpu->pwrite_cb  = save_pwrite;  cpu->pwrite_data = save_pwd;
    cpu->intread_cb = save_intread; cpu->intread_data = save_ird;
    cpu->intack_cb  = save_intack;  cpu->intack_data = save_iad;
    cpu->reti_cb    = save_reti;    cpu->reti_data   = save_rtd;
    cpu->ei_cb      = save_ei;     cpu->ei_data     = save_eid;
    cpu->di_cb      = save_di;     cpu->di_data     = save_did;
    cpu->im_cb      = save_im;     cpu->im_data     = save_imd;
    cpu->halt_cb    = save_halt;   cpu->halt_data   = save_haltd;
    cpu->nmi_cb     = save_nmi;    cpu->nmi_data    = save_nmid;
    cpu->iff_change_cb   = save_iff_change;
    cpu->iff_change_data = save_iff_changed;
    cpu->cpu_ctrl_event_cb   = save_cpu_ctrl;
    cpu->cpu_ctrl_event_data = save_cpu_ctrld;
    cpu->call_cb   = save_call;    cpu->call_data  = save_calld;
    cpu->ret_cb    = save_ret;     cpu->ret_data   = save_retd;
    cpu->external_int_handling = save_ext_int;
    cpu->post_step_cb = save_ps;    cpu->post_step_data = save_psd;

    /*
     * Fire iff_change reset event - konzument může sledovat reset jako
     * konkrétní IFF source. Voláme až po obnovení callbacků (= save/restore
     * zachoval iff_change_cb přes reset cyklus).
     */
    if (cpu->iff_change_cb) {
        cpu->iff_change_cb(cpu, cpu->iff1, cpu->iff2,
                           (uint8_t)Z80_IFF_REASON_RESET,
                           cpu->iff_change_data);
    }
}

/* ========== Dynamická změna callbacků ========== */

void z80_set_mread(z80_t *cpu, z80_mread_cb fn, void *data) {
    cpu->mread_cb = fn ? fn : default_mread;
    cpu->mread_data = data;
}

void z80_set_mwrite(z80_t *cpu, z80_mwrite_cb fn, void *data) {
    cpu->mwrite_cb = fn ? fn : default_mwrite;
    cpu->mwrite_data = data;
}

void z80_set_pread(z80_t *cpu, z80_pread_cb fn, void *data) {
    cpu->pread_cb = fn ? fn : default_pread;
    cpu->pread_data = data;
}

void z80_set_pwrite(z80_t *cpu, z80_pwrite_cb fn, void *data) {
    cpu->pwrite_cb = fn ? fn : default_pwrite;
    cpu->pwrite_data = data;
}

void z80_set_intread(z80_t *cpu, z80_intread_cb fn, void *data) {
    cpu->intread_cb = fn;
    cpu->intread_data = data;
}

void z80_set_intack(z80_t *cpu, z80_intack_cb fn, void *data) {
    cpu->intack_cb = fn;
    cpu->intack_data = data;
}

void z80_set_reti(z80_t *cpu, z80_reti_cb fn, void *data) {
    cpu->reti_cb = fn;
    cpu->reti_data = data;
}

void z80_set_ei(z80_t *cpu, z80_ei_cb fn, void *data) {
    cpu->ei_cb = fn;
    cpu->ei_data = data;
}

void z80_set_di(z80_t *cpu, z80_di_cb fn, void *data) {
    cpu->di_cb = fn;
    cpu->di_data = data;
}

void z80_set_im_change(z80_t *cpu, z80_im_cb fn, void *data) {
    cpu->im_cb = fn;
    cpu->im_data = data;
}

void z80_set_halt(z80_t *cpu, z80_halt_cb fn, void *data) {
    cpu->halt_cb = fn;
    cpu->halt_data = data;
}

void z80_set_nmi_cb(z80_t *cpu, z80_nmi_cb fn, void *data) {
    cpu->nmi_cb = fn;
    cpu->nmi_data = data;
}

#ifdef MZ800EMU_CFG_RAM_FASTPATH
void z80_set_ram_fastpath(z80_t *cpu, uint8_t *const read_table[16],
                          uint8_t *const write_table[16],
                          uint8_t *dbus_latch, bool enabled) {
    for (int i = 0; i < 16; i++) {
        cpu->ram_fp_read[i]  = read_table[i];
        cpu->ram_fp_write[i] = write_table[i];
    }
    cpu->ram_fp_dbus_latch = dbus_latch;
    cpu->ram_fp_enabled    = enabled;
}
#endif

/**
 * @brief Nastaví callback pro změnu IFF1/IFF2.
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= no overhead v hot path).
 * Fire při EI, DI, RETI, RETN, INT ack (IM 0/1/2), NMI ack a reset.
 *
 * @param cpu Ukazatel na CPU instanci. Pre cpu != NULL.
 * @param fn Callback funkce, nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 */
void z80_set_iff_change(z80_t *cpu, z80_iff_change_cb fn, void *data) {
    cpu->iff_change_cb = fn;
    cpu->iff_change_data = data;
}

/**
 * @brief Nastaví callback pro CPU control eventy (HALT enter/exit, RST nn).
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= nulový overhead v hot path).
 * Fire při HALT entry (op_76), HALT exit při IRQ/NMI ack a při každém
 * RST opcode dispatch (0xC7..0xFF). Pattern shodný s iff_change_cb.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param fn Callback funkce, nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 * @pre cpu != NULL.
 */
void z80_set_cpu_ctrl_event(z80_t *cpu, z80_cpu_ctrl_event_cb fn, void *data) {
    cpu->cpu_ctrl_event_cb = fn;
    cpu->cpu_ctrl_event_data = data;
}

/**
 * @brief Nastaví callback pro CALL dispatch (callstack push).
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= nulový overhead v hot
 * path). Fire pattern shodný s cpu_ctrl_event_cb - NULL check
 * v callsite každého CALL opcode handleru.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param fn Callback funkce, nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 * @pre cpu != NULL.
 */
void z80_set_call(z80_t *cpu, z80_call_cb fn, void *data) {
    cpu->call_cb = fn;
    cpu->call_data = data;
}

/**
 * @brief Nastaví callback pro RET dispatch (callstack pop).
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= nulový overhead v hot
 * path). Fire pattern shodný s call_cb. NEPOKRÝVÁ RETI / RETN -
 * callstack subsystem si je napojuje přes iff_change_cb.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param fn Callback funkce, nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 * @pre cpu != NULL.
 */
void z80_set_ret(z80_t *cpu, z80_ret_cb fn, void *data) {
    cpu->ret_cb = fn;
    cpu->ret_data = data;
}

void z80_set_post_step(z80_t *cpu, void (*fn)(z80_t *cpu, void *data), void *data) {
    cpu->post_step_cb = fn;
    cpu->post_step_data = data;
}

void z80_add_wait_states(z80_t *cpu, int wait) {
    cpu->wait_cycles += wait;
    cpu->op_tstate += wait;
}

/**
 * @brief Provedení jedné instrukce (per-step jádro bez registrové cache).
 *
 * Volá z80_execute_step() (Z80_DIRECT_REGS=1) místo batch varianty. Per-step
 * jádro provede PRÁVĚ 1 instrukci a vrátí se (target_cycles=1), takže KDY se
 * vyhodnocuje event/interrupt boundary se NEMĚNÍ oproti původnímu per-1
 * volání z80_execute(cpu, 1) - mzarch smyčka testuje boundary po každé
 * instrukci. Rozdíl je jen v eliminaci prologu/epilogu (RELOAD/WRITEBACK/
 * _lcache) - registry ŽIJÍ přímo v cpu->.
 *
 * Rozdíl total_cycles se počítá v uint32_t (modulo 2^32), na int se převádí
 * až malý výsledek. Odečet absolutních hodnot převedených na int by na
 * hranici 2^31 přetekl (nedefinované chování v C).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @return Počet T-stavů spotřebovaných instrukcí (správně i přes přetečení
 *         total_cycles na hranici 2^32).
 */
int z80_step(z80_t *cpu) {
    uint32_t before = cpu->total_cycles;
    z80_execute_step(cpu, 1);
    return (int)(uint32_t)(cpu->total_cycles - before);
}

void z80_int(z80_t *cpu) {
    cpu->int_pending = true;
    cpu->int_vector = 0;
}

void z80_irq(z80_t *cpu, uint8_t vector) {
    cpu->int_pending = true;
    cpu->int_vector = vector;
}

void z80_nmi(z80_t *cpu) {
    cpu->nmi_pending = true;
    if (cpu->nmi_cb) cpu->nmi_cb(cpu, cpu->nmi_data);
}

/* ========== Přístup k registrům ========== */

uint16_t z80_get_reg(z80_t *cpu, z80_reg_t reg) {
    switch (reg) {
        case Z80_REG_AF:  return cpu->af.w;
        case Z80_REG_BC:  return cpu->bc.w;
        case Z80_REG_DE:  return cpu->de.w;
        case Z80_REG_HL:  return cpu->hl.w;
        case Z80_REG_AF2: return cpu->af2.w;
        case Z80_REG_BC2: return cpu->bc2.w;
        case Z80_REG_DE2: return cpu->de2.w;
        case Z80_REG_HL2: return cpu->hl2.w;
        case Z80_REG_IX:  return cpu->ix.w;
        case Z80_REG_IY:  return cpu->iy.w;
        case Z80_REG_SP:  return cpu->sp;
        case Z80_REG_PC:  return cpu->pc;
        case Z80_REG_WZ:  return cpu->wz.w;
        case Z80_REG_IR:  return (uint16_t)((cpu->i << 8) | cpu->r);
        default:          return 0;
    }
}

void z80_set_reg(z80_t *cpu, z80_reg_t reg, uint16_t value) {
    /*
     * Pokud probíhá z80_execute() (cpu->_active_cache != NULL), modifikujeme
     * kromě pole z80_t i lokální cache, aby změna byla vidět v probíhající
     * instrukci. Bez toho by WRITEBACK na konci instrukce přepsal cpu->...w
     * zpět starou hodnotou ze stále neaktualizované lokální cache.
     */
    z80_local_cache_t *lc = cpu->_active_cache;
    switch (reg) {
        case Z80_REG_AF:
            cpu->af.w = value;
            /* Zápis AF zvenčí se pro registr Q chová jako POP AF: Q = 0.
             * Při volání z callbacku uprostřed instrukce Q stejně přepíše
             * konec té instrukce (Q_UPDATE). */
            cpu->q = 0;
            if (lc) {
                *lc->A = (uint8_t)(value >> 8);
                *lc->F = (uint8_t)value;
                *lc->Q = 0;
            }
            break;
        case Z80_REG_BC:
            cpu->bc.w = value;
            if (lc) { *lc->B = (uint8_t)(value >> 8); *lc->C = (uint8_t)value; }
            break;
        case Z80_REG_DE:
            cpu->de.w = value;
            if (lc) { *lc->D = (uint8_t)(value >> 8); *lc->E = (uint8_t)value; }
            break;
        case Z80_REG_HL:
            cpu->hl.w = value;
            if (lc) { *lc->H = (uint8_t)(value >> 8); *lc->L = (uint8_t)value; }
            break;
        case Z80_REG_AF2: cpu->af2.w = value; break;
        case Z80_REG_BC2: cpu->bc2.w = value; break;
        case Z80_REG_DE2: cpu->de2.w = value; break;
        case Z80_REG_HL2: cpu->hl2.w = value; break;
        case Z80_REG_IX:  cpu->ix.w  = value; break;
        case Z80_REG_IY:  cpu->iy.w  = value; break;
        case Z80_REG_SP:
            cpu->sp = value;
            if (lc) *lc->SP = value;
            break;
        case Z80_REG_PC:
            cpu->pc = value;
            if (lc) *lc->PC = value;
            break;
        case Z80_REG_WZ:
            cpu->wz.w = value;
            if (lc) *lc->WZ = value;
            break;
        case Z80_REG_IR:
            cpu->i = (uint8_t)(value >> 8);
            cpu->r = (uint8_t)value;
            if (lc) *lc->R = (uint8_t)value;
            break;
        default: break;
    }
}

bool z80_is_halted(z80_t *cpu) {
    return cpu->halted;
}
