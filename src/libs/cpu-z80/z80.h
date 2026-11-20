/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: MIT
 * https://github.com/michalhucik/z80-mz800
 */
/**
 * @file z80.h
 * @brief cpu-z80 multi-v0.2 - Přesný a rychlý multi-instance Z80A emulátor.
 *
 * Multi-instance API: každá CPU instance nese vlastní callbacky a user_data.
 * Umožňuje provozovat více nezávislých CPU instancí současně.
 *
 * Kompletní instrukční sada včetně nedokumentovaných instrukcí.
 * Přesné počítání T-stavů, správné chování všech flagů (včetně F3/F5),
 * MEMPTR/WZ registr, přerušovací režimy IM0/1/2, NMI.
 *
 * Optimalizace:
 * - Lokální cache callback pointerů v z80_execute() (nulový overhead)
 * - Computed goto dispatch (GCC/Clang)
 * - Lokální registrová cache v z80_execute()
 * - Eliminace null-checků callbacků (default handlery)
 * - DAA lookup tabulka (2048 záznamů)
 * - Inline prefix handlery
 *
 * @version multi-v0.5
 */

#ifndef CPU_Z80_H
#define CPU_Z80_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** @name Flagy Z80
 * @{ */
#define Z80_FLAG_C  0x01  /**< Carry */
#define Z80_FLAG_N  0x02  /**< Subtract */
#define Z80_FLAG_PV 0x04  /**< Parity/Overflow */
#define Z80_FLAG_3  0x08  /**< Nedokumentovaný bit 3 */
#define Z80_FLAG_H  0x10  /**< Half Carry */
#define Z80_FLAG_5  0x20  /**< Nedokumentovaný bit 5 */
#define Z80_FLAG_Z  0x40  /**< Zero */
#define Z80_FLAG_S  0x80  /**< Sign */
/** @} */

/* Forward deklarace pro callback typy */
struct z80_s;

/** @name Typy callbacků
 *
 * Všechny callbacky dostávají ukazatel na CPU instanci a user_data.
 * @{ */

/**
 * @brief Callback pro čtení bajtu z paměti.
 * @param cpu Ukazatel na CPU instanci.
 * @param addr 16bitová adresa.
 * @param m1_state 1 = M1 fetch (čtení instrukce), 0 = normální čtení.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 * @return Přečtený bajt.
 */
typedef uint8_t (*z80_mread_cb)(struct z80_s *cpu, uint16_t addr, int m1_state, void *user_data);

/**
 * @brief Callback pro zápis bajtu do paměti.
 * @param cpu Ukazatel na CPU instanci.
 * @param addr 16bitová adresa.
 * @param value Zapisovaný bajt.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_mwrite_cb)(struct z80_s *cpu, uint16_t addr, uint8_t value, void *user_data);

/**
 * @brief Callback pro čtení z I/O portu.
 * @param cpu Ukazatel na CPU instanci.
 * @param port 16bitová adresa portu.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 * @return Přečtený bajt.
 */
typedef uint8_t (*z80_pread_cb)(struct z80_s *cpu, uint16_t port, void *user_data);

/**
 * @brief Callback pro zápis na I/O port.
 * @param cpu Ukazatel na CPU instanci.
 * @param port 16bitová adresa portu.
 * @param value Zapisovaný bajt.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_pwrite_cb)(struct z80_s *cpu, uint16_t port, uint8_t value, void *user_data);

/**
 * @brief Callback pro čtení vektoru přerušení.
 * @param cpu Ukazatel na CPU instanci.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 * @return Vektor přerušení.
 */
typedef uint8_t (*z80_intread_cb)(struct z80_s *cpu, void *user_data);

/**
 * @brief Callback pro potvrzení přerušení (INTACK signál).
 * @param cpu Ukazatel na CPU instanci.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_intack_cb)(struct z80_s *cpu, void *user_data);

/**
 * @brief Callback pro RETI instrukci - notifikace periferií (daisy chain).
 * @param cpu Ukazatel na CPU instanci.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_reti_cb)(struct z80_s *cpu, void *user_data);

/**
 * @brief Callback volaný při provedení instrukce EI.
 * @param cpu Ukazatel na CPU instanci.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_ei_cb)(struct z80_s *cpu, void *user_data);

/**
 * @brief Callback volaný při provedení instrukce DI.
 * @param cpu Ukazatel na CPU instanci.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_di_cb)(struct z80_s *cpu, void *user_data);

/**
 * @brief Callback volaný při změně IM (Interrupt Mode).
 * @param cpu Ukazatel na CPU instanci.
 * @param new_im Nová hodnota IM (0, 1 nebo 2).
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_im_cb)(struct z80_s *cpu, uint8_t new_im, void *user_data);

/**
 * @brief Callback volaný při přechodu CPU do HALT stavu.
 * @param cpu Ukazatel na CPU instanci.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_halt_cb)(struct z80_s *cpu, void *user_data);

/**
 * @brief Callback volaný při NMI assertion (z80_nmi).
 * @param cpu Ukazatel na CPU instanci.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_nmi_cb)(struct z80_s *cpu, void *user_data);

/**
 * @brief Důvod změny IFF1 / IFF2.
 *
 * Předáváno jako parametr do z80_iff_change_cb. Konzument může podle
 * důvodu rozlišit dispatch-driven clear (INT_ACK / NMI_ACK) od
 * explicit instrukční změny (EI / DI / RETI / RETN).
 *
 * @note INT_ACK pokrývá všechny IM 0/1/2 - Z80 při ack maskovaného
 *       přerušení vždy clearuje IFF1+IFF2 nezávisle na IM mode (Zilog
 *       Z80 manual; Sean Young). IM mode jen určuje co se vykoná po
 *       clearu, ne zda se IFF clearují.
 */
typedef enum {
    Z80_IFF_REASON_RESET    = 0,  /**< CPU reset - IFF1=0, IFF2=0 */
    Z80_IFF_REASON_EI       = 1,  /**< EI instrukce - IFF1=1, IFF2=1 */
    Z80_IFF_REASON_DI       = 2,  /**< DI instrukce - IFF1=0, IFF2=0 */
    Z80_IFF_REASON_INT_ACK  = 3,  /**< Maskované INT přijato (IM 0/1/2) - IFF1=0, IFF2=0 */
    Z80_IFF_REASON_NMI_ACK  = 4,  /**< NMI přijato - IFF1=0, IFF2 zachováno */
    Z80_IFF_REASON_RETI     = 5,  /**< RETI - IFF1 <- IFF2 */
    Z80_IFF_REASON_RETN     = 6   /**< RETN - IFF1 <- IFF2 */
} z80_iff_change_reason_t;

/**
 * @brief Callback volaný při změně IFF1 nebo IFF2.
 *
 * Fire vždy když CPU modifikuje IFF1 nebo IFF2 - tj. při EI, DI, RETI,
 * RETN, INT ack (IM 0/1/2), NMI ack a reset. Doplňuje existující
 * ei_cb / di_cb / nmi_cb / reti_cb (= ty zůstávají pro BC), poskytuje
 * sjednocený event source pro debugger BP eventy typu cpu:iff1_change.
 *
 * Hot path: NULL ptr check v callsite (= nulový overhead pokud nenastaven).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param new_iff1 Nová hodnota IFF1 (0 nebo 1) - již zapsaná v cpu->iff1.
 * @param new_iff2 Nová hodnota IFF2 (0 nebo 1) - již zapsaná v cpu->iff2.
 * @param reason Důvod změny (z80_iff_change_reason_t cast na uint8_t).
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_iff_change_cb)(struct z80_s *cpu,
                                  uint8_t new_iff1,
                                  uint8_t new_iff2,
                                  uint8_t reason,
                                  void *user_data);

/**
 * @brief Typ CPU control eventu pro z80_cpu_ctrl_event_cb.
 *
 * HALT_ENTER a HALT_EXIT pokrývají přechody cpu->halted 0->1 / 1->0
 * (vstup do HALT instrukcí, výstup při přijetí IRQ/NMI). RST_00..RST_38
 * jsou jednotlivé RST opcody (C7/CF/D7/DF/E7/EF/F7/FF). Konzument se
 * používá pro dispatch do eventlog kategorie CPU_CTRL v emu vrstvě
 * (event-viewer mutant, Vlna 1).
 */
typedef enum {
    Z80_CPU_CTRL_HALT_ENTER = 0,  /**< Instrukce HALT vykonána, cpu->halted 0->1 */
    Z80_CPU_CTRL_HALT_EXIT  = 1,  /**< IRQ/NMI probudilo z HALT, cpu->halted 1->0 */
    Z80_CPU_CTRL_RST_00     = 2,  /**< Opcode 0xC7 (RST 00h) dispatch */
    Z80_CPU_CTRL_RST_08     = 3,  /**< Opcode 0xCF (RST 08h) dispatch */
    Z80_CPU_CTRL_RST_10     = 4,  /**< Opcode 0xD7 (RST 10h) dispatch */
    Z80_CPU_CTRL_RST_18     = 5,  /**< Opcode 0xDF (RST 18h) dispatch */
    Z80_CPU_CTRL_RST_20     = 6,  /**< Opcode 0xE7 (RST 20h) dispatch */
    Z80_CPU_CTRL_RST_28     = 7,  /**< Opcode 0xEF (RST 28h) dispatch */
    Z80_CPU_CTRL_RST_30     = 8,  /**< Opcode 0xF7 (RST 30h) dispatch */
    Z80_CPU_CTRL_RST_38     = 9   /**< Opcode 0xFF (RST 38h) dispatch */
} z80_cpu_ctrl_event_t;

/**
 * @brief Callback pro CPU control events (HALT entry/exit, RST nn).
 *
 * Fire při:
 *   - HALT opcode (0x76) - entry, cpu->halted 0->1, před voláním
 *     existujícího halt_cb (= ten zůstává beze změny, BC).
 *   - HALT exit při NMI ack i při maskovaném INT ack v
 *     handle_interrupts_internal, cpu->halted 1->0. Fire jen pokud
 *     CPU bylo skutečně v HALT před ack (= ne při přerušení mezi
 *     běžnými instrukcemi).
 *   - RST opcode 0xC7..0xFF dispatch, fire před push PC do stacku.
 *
 * PC předávaný do callbacku:
 *   - HALT_ENTER:   adresa za HALT instrukcí (= rPC + 1 po fetch,
 *                   reflektuje cpu->pc v okamžiku eventu).
 *   - HALT_EXIT:    cpu->pc v okamžiku exit (= adresa za HALT,
 *                   kam se vrátí po obsluze IRQ/NMI).
 *   - RST_xx:       adresa za RST opcode (= rPC po fetch, před
 *                   push do stacku; konzument si může načíst
 *                   adresu RST instrukce jako pc - 1, pokud
 *                   potřebuje).
 *
 * Hot path: NULL ptr check v callsite (= nulový overhead pokud
 * nenastaven). Nezávislý na existujícím halt_cb (= ten dostane
 * vlastní notifikaci při HALT entry, callbacky se doplňují).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param event Typ control eventu (z80_cpu_ctrl_event_t cast na uint8_t).
 * @param pc PC v okamžiku eventu (viz výše mapping per event type).
 * @param user_data Uživatelská data předaná při registraci.
 */
typedef void (*z80_cpu_ctrl_event_cb)(struct z80_s *cpu,
                                      uint8_t event,
                                      uint16_t pc,
                                      void *user_data);

/**
 * @brief Callback volaný při přijetí CALL instrukce (taken větve).
 *
 * Fire bezprostředně před `PUSH(rPC); rPC = target;` v opcode handleru
 * - tedy v okamžiku, kdy už proběhl FETCH16 cíle, ale stack ještě
 * nemá uloženou návratovou adresu a PC ukazuje za CALL instrukci
 * (= shodné s tím, co se vzápětí pushne).
 *
 * Pokrývá všechny CALL opcody:
 *   - unconditional CALL nn (0xCD)
 *   - CALL cc, nn (0xC4 / 0xCC / 0xD4 / 0xDC / 0xE4 / 0xEC / 0xF4 / 0xFC)
 *
 * U podmíněných variant fire jen pokud je podmínka splněna (= CALL
 * skutečně přijat, PC bude přepsán cílem). Při condition=false se
 * callback nevolá (= jen FETCH16 a pokračování za CALL).
 *
 * Vyhrazeno pro callstack subsystem (shadow stack push). Zero overhead
 * v hot path při NULL ptr (NULL check v callsite). Nezávislé na
 * cpu_ctrl_event_cb - RST/CALL jsou rozdílné události.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param call_site Adresa CALL opcode v paměti
 *                  (= return_addr - 3 pro 3-bytové CALL).
 * @param target Cílová adresa (= operand za CALL opcode).
 * @param return_addr Adresa za CALL instrukcí (= co bude pushnuto na
 *                    stack a kam se RET vrátí; v okamžiku fire shodné
 *                    s cpu->pc).
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_call_cb)(struct z80_s *cpu,
                            uint16_t call_site,
                            uint16_t target,
                            uint16_t return_addr,
                            void *user_data);

/**
 * @brief Callback volaný při přijetí RET instrukce (taken větve).
 *
 * Fire bezprostředně před `rPC = POP();` v opcode handleru - tedy
 * v okamžiku, kdy stack ještě obsahuje návratovou adresu na vrcholu
 * a SP ukazuje na ni. Konzument může načíst pop_target přes PEEK16
 * (provedeno v callsite) a porovnat sp_before_pop se shadow stack
 * záznamem (= match podle SP, ne podle adresy).
 *
 * Pokrývá:
 *   - unconditional RET (0xC9)
 *   - RET cc (0xC0 / 0xC8 / 0xD0 / 0xD8 / 0xE0 / 0xE8 / 0xF0 / 0xF8)
 *
 * U podmíněných variant fire jen pokud je podmínka splněna. Při
 * condition=false se POP nedělá a callback se nevolá.
 *
 * NEPOKRÝVÁ RETI (ED 4D) ani RETN (ED 45). Ty mají vlastní reti_cb
 * resp. iff_change_cb s reason=RETI/RETN. Callstack si interrupt
 * návraty řadí přes iff_change_cb (= jiná dispatch cesta než běžné
 * RET pro user kód).
 *
 * Vyhrazeno pro callstack subsystem (shadow stack pop / unwind).
 * Zero overhead v hot path při NULL ptr (NULL check v callsite).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param pop_target Adresa, na kterou se CPU vrátí (= PEEK16(sp) v
 *                   okamžiku fire, vzápětí nahraje do PC).
 * @param sp_before_pop Hodnota SP před POP (= ukazatel na pop_target
 *                      na stacku). Pro match se shadow stack entry
 *                      sp_at_entry porovnává konzument.
 * @param user_data Uživatelská data předaná při registraci callbacku.
 */
typedef void (*z80_ret_cb)(struct z80_s *cpu,
                           uint16_t pop_target,
                           uint16_t sp_before_pop,
                           void *user_data);

/** @} */

/**
 * @brief Pár registrů s 16bit/8bit přístupem (little-endian).
 *
 * Umožňuje přístup k registrovému páru jako k 16bitové hodnotě (w)
 * nebo ke dvěma 8bitovým polovinám (h, l).
 *
 * @invariant Na little-endian platformě: l je na nižší adrese, h na vyšší.
 */
typedef union {
    uint16_t w;         /**< 16bitový přístup */
    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint8_t l, h;   /**< 8bitový přístup: nízký a vysoký bajt */
#else
        uint8_t h, l;
#endif
    };
} z80_pair_t;

/**
 * @brief Pointery na lokální registrovou cache uvnitř z80_execute().
 *
 * Během běhu z80_execute() jsou hlavní registry drženy v lokálních
 * proměnných pro rychlost (kompilátor je drží v CPU registrech).
 * Aby callbacky volané z průběhu instrukce mohly transparentně měnit
 * registry přes z80_set_reg(), z80_execute() vyplní tuto strukturu
 * adresami svých lokálních proměnných a zaregistruje ji v z80_t._active_cache.
 *
 * z80_set_reg() pak při zápisu nejen aktualizuje pole z80_t (cpu->af.w,
 * cpu->bc.w, ...), ale současně přes tyto pointery i lokální cache,
 * aby se změna projevila ihned v probíhající instrukci.
 *
 * @invariant Pokud je _active_cache != NULL, ukazuje na strukturu
 *            žijící po dobu jediného běhu z80_execute(). Mimo z80_execute()
 *            musí být _active_cache == NULL.
 */
typedef struct z80_local_cache_s {
    uint8_t  *A, *F;       /**< AF pár */
    uint8_t  *B, *C;       /**< BC pár */
    uint8_t  *D, *E;       /**< DE pár */
    uint8_t  *H, *L;       /**< HL pár */
    uint16_t *PC, *SP, *WZ;/**< PC, SP, WZ */
    uint8_t  *R, *Q;       /**< R refresh, Q interní */
} z80_local_cache_t;

/**
 * @brief Stav procesoru Z80 s multi-instance callbacky.
 *
 * Obsahuje všechny registry, přerušovací systém, čítače cyklů
 * a callbacky s user_data pro každý typ operace.
 * IX, IY a alternativní sada zůstávají ve struktuře (méně časté přístupy).
 * Hlavní registry (AF, BC, DE, HL, PC, SP, WZ, R) jsou v z80_execute()
 * cachovány do lokálních proměnných pro rychlejší přístup.
 *
 * @invariant im je vždy 0, 1 nebo 2.
 * @invariant mread_cb a mwrite_cb nesmějí být NULL (nastaví se default).
 * @invariant _active_cache je NULL mimo z80_execute().
 */
typedef struct z80_s {
    /* Hlavní registrová sada */
    z80_pair_t af, bc, de, hl;
    /* Alternativní registrová sada */
    z80_pair_t af2, bc2, de2, hl2;
    /* Indexové registry */
    z80_pair_t ix, iy;
    /* Interní registr MEMPTR/WZ - ovlivňuje F3/F5 u BIT n,(HL) aj. */
    z80_pair_t wz;
    /* Speciální registry */
    uint16_t sp;          /**< Stack Pointer */
    uint16_t pc;          /**< Program Counter */
    uint8_t  i;           /**< Interrupt Vector */
    uint8_t  r;           /**< Memory Refresh - +1 za každý M1 cyklus (i opakovaný fetch HALT a potvrzení INT/NMI), mění se jen bity 0-6 */
    /* Přerušovací systém */
    uint8_t  iff1, iff2;  /**< Interrupt Flip-Flops */
    uint8_t  im;          /**< Interrupt Mode (0, 1, 2) */
    /* Stavy */
    bool halted;      /**< CPU je v HALT stavu */
    bool int_pending;  /**< Čekající přerušení */
    bool nmi_pending;  /**< Čekající NMI */
    bool ei_delay;     /**< EI delay: po EI se přerušení odloží o 1 instrukci */
    bool ld_a_ir;      /**< HW bug: INT po LD A,I/R resetuje PF na 0 */
    uint8_t   int_vector;   /**< Vektor přerušení (pro IM2) */
    uint8_t   q;            /**< Interní registr Q: F po instrukci, která zapsala příznaky, jinak 0 (YF/XF u SCF/CCF) */
    /* Počítadlo cyklů */
    uint32_t cycles;        /**< Aktuální T-stavy ve frame */
    uint32_t total_cycles;  /**< Celkový počet T-stavů */
    int wait_cycles;   /**< Extra wait states vložené I/O zařízením */

    /* Callbacky s user_data - pro multi-instance */
    z80_mread_cb  mread_cb;     /**< Callback pro čtení z paměti */
    void         *mread_data;   /**< User data pro mread_cb */
    z80_mwrite_cb mwrite_cb;    /**< Callback pro zápis do paměti */
    void         *mwrite_data;  /**< User data pro mwrite_cb */
    z80_pread_cb  pread_cb;     /**< Callback pro čtení z I/O portu */
    void         *pread_data;   /**< User data pro pread_cb */
    z80_pwrite_cb pwrite_cb;    /**< Callback pro zápis na I/O port */
    void         *pwrite_data;  /**< User data pro pwrite_cb */
    z80_intread_cb intread_cb;  /**< Callback pro čtení vektoru přerušení */
    void          *intread_data;/**< User data pro intread_cb */
    z80_intack_cb  intack_cb;   /**< Callback pro INTACK signal */
    void          *intack_data; /**< User data pro intack_cb */
    z80_reti_cb    reti_cb;     /**< Callback pro RETI notifikaci */
    void          *reti_data;   /**< User data pro reti_cb */
    z80_ei_cb      ei_cb;       /**< Callback volaný při EI instrukci */
    void          *ei_data;     /**< User data pro ei_cb */
    z80_di_cb      di_cb;       /**< Callback volaný při DI instrukci */
    void          *di_data;     /**< User data pro di_cb */
    z80_im_cb      im_cb;       /**< Callback volaný při IM 0/1/2 změně */
    void          *im_data;     /**< User data pro im_cb */
    z80_halt_cb    halt_cb;     /**< Callback volaný při HALT instrukci */
    void          *halt_data;   /**< User data pro halt_cb */
    z80_nmi_cb     nmi_cb;      /**< Callback volaný při NMI assertion */
    void          *nmi_data;    /**< User data pro nmi_cb */
    z80_iff_change_cb iff_change_cb; /**< Callback volaný při změně IFF1/IFF2 (V17+ debugger BP) */
    void          *iff_change_data;  /**< User data pro iff_change_cb */
    z80_cpu_ctrl_event_cb cpu_ctrl_event_cb; /**< Callback pro CPU control eventy (HALT enter/exit, RST nn) */
    void          *cpu_ctrl_event_data;      /**< User data pro cpu_ctrl_event_cb */
    z80_call_cb    call_cb;     /**< Callback pro CALL dispatch (callstack push) */
    void          *call_data;   /**< User data pro call_cb */
    z80_ret_cb     ret_cb;      /**< Callback pro RET dispatch (callstack pop) */
    void          *ret_data;    /**< User data pro ret_cb */
    int op_tstate;          /**< T-stavy od začátku aktuální instrukce (inkrementován při FETCH/RD/WR/IO) */
    bool external_int_handling; /**< Pokud true, z80_execute() přeskočí automatické zpracování přerušení */

    /** Post-step callback - voláno po každé instrukci (MZ-700 per-line WAIT). */
    void (*post_step_cb)(struct z80_s *cpu, void *data);
    void *post_step_data;       /**< User data pro post_step_cb */

    /**
     * Pointery na lokální registrovou cache aktivní instance z80_execute().
     * Nenulové pouze během běhu z80_execute(); umožňuje z80_set_reg()
     * propsat změny i do lokálních proměnných probíhající instrukce.
     * Mimo z80_execute() musí být NULL.
     */
    z80_local_cache_t *_active_cache;

#ifdef MZ800EMU_CFG_RAM_FASTPATH
    /**
     * @name RAM-access fast-path (E1, KROK 1 návrhu D3)
     *
     * Page-table 16 záznamů (1 / 4 KiB stránka, index addr>>12). Každá stránka
     * je BUĎ přímý ukazatel na bázi 4 KiB RAM banku (přístup ptr[addr&0xFFF]),
     * NEBO NULL = "NEEDS_CALLBACK" (VRAM/CGRAM/ROM/mapped-ports/PROHIBITED) -
     * tehdy RD/WR makro spadne na původní mread_cb/mwrite_cb (přesný GDG sync,
     * regDBUS_latch, CDL logging zachovány).
     *
     * Tabulky plní a invaliduje mzarch vrstva (mz800: z80_ram_fastpath_rebuild)
     * při každém banking switchi (cold path). Jádro je arch-independent - jen
     * konzumuje pointery, nezná mapovací pravidla.
     *
     * @invariant Stránka je v ram_fp_read/write non-NULL pouze pokud čistý RAM
     *            přístup na ni je BIT-IDENTICKÝ s průchodem přes memory_*_cb
     *            (včetně všech vedlejších efektů - viz ram_fp_dbus_latch).
     * @{
     */
    uint8_t *ram_fp_read[16];   /**< Read page-table: NULL = callback, jinak báze 4 KiB banku. */
    uint8_t *ram_fp_write[16];  /**< Write page-table: NULL = callback, jinak báze 4 KiB banku. */
    /**
     * Ukazatel na arch-specifický "data bus latch" (mz800: g_mzarch_main.
     * regDBUS_latch). Čistá RAM cesta v memory_read_cb tento latch nastavuje
     * na přečtenou hodnotu - fast-path read ho proto musí replikovat, jinak
     * není bit-identický (latch čte např. čtení CTC control registru na E007).
     * NULL = arch latch nepoužívá (fast-path read ho pak neaktualizuje).
     */
    uint8_t *ram_fp_dbus_latch;
    /**
     * Gating: fast-path je aktivní jen když true. Mzarch vrstva ho nastaví
     * false kdykoliv je nainstalován logging callback (debugger/CDL) - tam
     * mají M1/read callbacky vedlejší efekty (X/R klasifikace, history) které
     * fast-path neumí replikovat, takže tam zůstává plný callback.
     */
    bool ram_fp_enabled;
    /** @} */
#endif
} z80_t;

/**
 * @brief Enum pro přístup k registrům přes z80_get_reg/z80_set_reg.
 */
typedef enum {
    Z80_REG_AF,   /**< Pár AF */
    Z80_REG_BC,   /**< Pár BC */
    Z80_REG_DE,   /**< Pár DE */
    Z80_REG_HL,   /**< Pár HL */
    Z80_REG_AF2,  /**< Alternativní AF' */
    Z80_REG_BC2,  /**< Alternativní BC' */
    Z80_REG_DE2,  /**< Alternativní DE' */
    Z80_REG_HL2,  /**< Alternativní HL' */
    Z80_REG_IX,   /**< Indexový registr IX */
    Z80_REG_IY,   /**< Indexový registr IY */
    Z80_REG_SP,   /**< Stack Pointer */
    Z80_REG_PC,   /**< Program Counter */
    Z80_REG_WZ,   /**< Interní registr MEMPTR/WZ */
    Z80_REG_IR    /**< I (vysoký bajt), R (nízký bajt) */
} z80_reg_t;

/* ========== Životní cyklus ========== */

/**
 * @brief Vytvoří novou CPU instanci s callbacky.
 *
 * Alokuje paměť pro z80_t, inicializuje lookup tabulky (při prvním volání),
 * nastaví callbacky a provede reset.
 *
 * @param mread Callback pro čtení z paměti (nesmí být NULL).
 * @param mread_data User data pro mread.
 * @param mwrite Callback pro zápis do paměti (nesmí být NULL).
 * @param mwrite_data User data pro mwrite.
 * @param pread Callback pro čtení z I/O portu (nesmí být NULL).
 * @param pread_data User data pro pread.
 * @param pwrite Callback pro zápis na I/O port (nesmí být NULL).
 * @param pwrite_data User data pro pwrite.
 * @param intread Callback pro čtení vektoru přerušení (může být NULL).
 * @param intread_data User data pro intread.
 * @return Ukazatel na novou CPU instanci, nebo NULL při chybě alokace.
 * @post Všechny registry jsou v defaultním stavu (z80_reset).
 */
z80_t *z80_create(
    z80_mread_cb mread, void *mread_data,
    z80_mwrite_cb mwrite, void *mwrite_data,
    z80_pread_cb pread, void *pread_data,
    z80_pwrite_cb pwrite, void *pwrite_data,
    z80_intread_cb intread, void *intread_data
);

/**
 * @brief Zničí CPU instanci a uvolní paměť.
 *
 * @param cpu Ukazatel na CPU instanci. Může být NULL (no-op).
 */
void z80_destroy(z80_t *cpu);

/**
 * @brief Reset CPU do výchozího stavu.
 *
 * Zachovává callbacky, resetuje registry, přerušovací systém a účty
 * T-stavů (cycles, total_cycles, wait_cycles, op_tstate).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @pre cpu != NULL.
 * @post Všechny registry jsou v defaultním stavu, callbacky zachovány.
 * @post op_tstate == 0 a wait_cycles == 0: kód, který je čte mimo instrukci
 *       (např. synchronizace času periferií před první instrukcí), nevidí
 *       délku ani čekací stavy instrukce z doby před resetem.
 */
void z80_reset(z80_t *cpu);

/* ========== Emulace ========== */

/**
 * @brief Provedení jedné instrukce.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @return Počet T-stavů spotřebovaných instrukcí.
 * @pre cpu != NULL.
 */
int z80_step(z80_t *cpu);

/**
 * @brief Provedení instrukcí po dobu daného počtu T-stavů.
 *
 * Hlavní emulační smyčka s optimalizovaným dispatch.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param target_cycles Cílový počet T-stavů k provedení.
 * @return Skutečný počet provedených T-stavů (>= target_cycles).
 * @pre cpu != NULL.
 */
int z80_execute(z80_t *cpu, int target_cycles);

/* ========== Přerušení ========== */

/**
 * @brief Vyvolání maskovaného přerušení s callbackem pro vektor.
 *
 * Vektor se čte přes intread_cb callback při zpracování.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @pre cpu != NULL.
 */
void z80_int(z80_t *cpu);

/**
 * @brief Vyvolání maskovaného přerušení s explicitním vektorem.
 *
 * Pro zpětnou kompatibilitu - vektor je uložen přímo.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param vector Vektor přerušení (použito v IM0 a IM2).
 * @pre cpu != NULL.
 */
void z80_irq(z80_t *cpu, uint8_t vector);

/**
 * @brief Vyvolání nemaskovaného přerušení (NMI).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @pre cpu != NULL.
 */
void z80_nmi(z80_t *cpu);

/* ========== Přístup k registrům ========== */

/**
 * @brief Čtení 16bitové hodnoty registru.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param reg Registr k přečtení.
 * @return 16bitová hodnota registru.
 * @pre cpu != NULL.
 */
uint16_t z80_get_reg(z80_t *cpu, z80_reg_t reg);

/**
 * @brief Zápis 16bitové hodnoty do registru.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param reg Registr k nastavení.
 * @param value 16bitová hodnota.
 * @pre cpu != NULL.
 * @post Zápis Z80_REG_AF vynuluje interní registr Q (jako POP AF), takže
 *       následující SCF/CCF vezme YF/XF z A or F.
 */
void z80_set_reg(z80_t *cpu, z80_reg_t reg, uint16_t value);

/**
 * @brief Zjištění zda je CPU v HALT stavu.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @return true pokud je CPU zastavena instrukcí HALT.
 * @pre cpu != NULL.
 */
bool z80_is_halted(z80_t *cpu);

/* ========== Dynamická změna callbacků ========== */

/** @name Settery pro callbacky
 * Umožňují dynamickou změnu callbacků za běhu.
 * @{ */
void z80_set_mread(z80_t *cpu, z80_mread_cb fn, void *data);
void z80_set_mwrite(z80_t *cpu, z80_mwrite_cb fn, void *data);
void z80_set_pread(z80_t *cpu, z80_pread_cb fn, void *data);
void z80_set_pwrite(z80_t *cpu, z80_pwrite_cb fn, void *data);
void z80_set_intread(z80_t *cpu, z80_intread_cb fn, void *data);
void z80_set_intack(z80_t *cpu, z80_intack_cb fn, void *data);
void z80_set_reti(z80_t *cpu, z80_reti_cb fn, void *data);
void z80_set_ei(z80_t *cpu, z80_ei_cb fn, void *data);
void z80_set_di(z80_t *cpu, z80_di_cb fn, void *data);
void z80_set_im_change(z80_t *cpu, z80_im_cb fn, void *data);
void z80_set_halt(z80_t *cpu, z80_halt_cb fn, void *data);
void z80_set_nmi_cb(z80_t *cpu, z80_nmi_cb fn, void *data);

#ifdef MZ800EMU_CFG_RAM_FASTPATH
/**
 * @brief Nastaví RAM-access fast-path page-table a gating (E1, KROK 1).
 *
 * Naplní ram_fp_read[]/ram_fp_write[] (16 záznamů / 4 KiB stránka), ukazatel
 * na arch data-bus latch a gating flag. Volat z mzarch vrstvy při KAŽDÉ změně
 * mapování (banking switch, DMD switch) a při změně read/write callbacků
 * (logging on/off). Jádro pak v RD/WR makrech přeskakuje mread_cb/mwrite_cb
 * pro stránky s non-NULL ukazatelem.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param read_table Pole 16 ukazatelů (NULL = callback, jinak báze 4 KiB banku). Kopíruje se.
 * @param write_table Pole 16 ukazatelů (NULL = callback, jinak báze 4 KiB banku). Kopíruje se.
 * @param dbus_latch Ukazatel na arch data-bus latch, nebo NULL. Drží se (ne kopie).
 * @param enabled true = fast-path aktivní; false = vždy callback (baseline).
 * @pre cpu != NULL, read_table != NULL, write_table != NULL.
 * @post Tabulky zkopírovány do cpu->; při enabled=false se obsah ignoruje v hot path.
 */
void z80_set_ram_fastpath(z80_t *cpu, uint8_t *const read_table[16],
                          uint8_t *const write_table[16],
                          uint8_t *dbus_latch, bool enabled);
#endif

/**
 * @brief Nastaví callback pro změnu IFF1/IFF2.
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= no overhead).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param fn Callback funkce nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 * @pre cpu != NULL.
 */
void z80_set_iff_change(z80_t *cpu, z80_iff_change_cb fn, void *data);

/**
 * @brief Nastaví callback pro CPU control eventy (HALT enter/exit, RST nn).
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= nulový overhead v hot
 * path). Fire při HALT entry/exit a RST 00..38 dispatch (viz typedef
 * z80_cpu_ctrl_event_cb pro detail).
 *
 * Nezávislý na halt_cb (= ten zůstává pro BC). Konzument event-viewer
 * mutantu používá pro kategorii CPU_CTRL v eventlog ringu.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param fn Callback funkce, nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 * @pre cpu != NULL.
 */
void z80_set_cpu_ctrl_event(z80_t *cpu, z80_cpu_ctrl_event_cb fn, void *data);

/**
 * @brief Nastaví callback pro CALL dispatch (callstack push).
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= nulový overhead v hot
 * path). Fire před `PUSH(rPC); rPC = target;` ve všech CALL opcode
 * handlerech (uncond + 8 podmíněných variant), pouze pokud je CALL
 * skutečně přijat (= condition splněna). Viz typedef @ref z80_call_cb
 * pro detail signatury a fire timing.
 *
 * Nezávislý na cpu_ctrl_event_cb. Vyhrazeno pro callstack subsystem -
 * jiní konzumenti by neměli registrovat (single-listener pattern;
 * pokud bude potřeba multi-listener, vrstva nad call_cb si fan-out
 * řeší sama).
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param fn Callback funkce, nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 * @pre cpu != NULL.
 */
void z80_set_call(z80_t *cpu, z80_call_cb fn, void *data);

/**
 * @brief Nastaví callback pro RET dispatch (callstack pop).
 *
 * Volat lze kdykoliv. NULL = callback vypnut (= nulový overhead v hot
 * path). Fire před `rPC = POP();` v RET / RET cc opcode handlerech,
 * pouze pokud je RET skutečně přijat. NEPOKRÝVÁ RETI (ED 4D) ani RETN
 * (ED 45) - viz typedef @ref z80_ret_cb pro důvod.
 *
 * Nezávislý na cpu_ctrl_event_cb. Vyhrazeno pro callstack subsystem.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param fn Callback funkce, nebo NULL pro vypnutí.
 * @param data User data předaná zpět při volání.
 * @pre cpu != NULL.
 */
void z80_set_ret(z80_t *cpu, z80_ret_cb fn, void *data);

void z80_set_post_step(z80_t *cpu, void (*fn)(z80_t *cpu, void *data), void *data);
/** @} */

/**
 * @brief Přidání wait states z callbacku.
 *
 * Voláno z memory/IO callbacku pro přidání extra čekacích stavů
 * (např. PSG READY signál). Přičítá i do op_tstate.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param wait Počet extra T-stavů.
 * @pre cpu != NULL.
 */
void z80_add_wait_states(z80_t *cpu, int wait);

/**
 * @brief Okamžité zpracování čekajícího maskovaného přerušení.
 *
 * Pro použití s external_int_handling=true. Emulátor nastaví int_pending
 * přes z80_int(), pak volá tuto funkci pro okamžité zpracování.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @return Počet T-stavů spotřebovaných obsluhou, nebo 0 pokud přerušení nebylo přijato.
 * @pre cpu != NULL.
 * @post Pokud přerušení přijato: IFF1=0, IFF2=0, PC nastaven na obslužnou rutinu.
 *       T-stavy přičteny do cpu->cycles a cpu->total_cycles.
 */
int z80_process_interrupt(z80_t *cpu);

/** Řetězec verze knihovny. */
#define CPU_Z80_VERSION "multi-v0.5"

#endif /* CPU_Z80_H */
