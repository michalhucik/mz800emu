/*
 * SDÍLENÉ TĚLO EMULAČNÍ SMYČKY Z80 - dva překlady z jednoho zdroje.
 *
 * Tento soubor se v z80.c includuje DVAKRÁT (varianta A návrhu A4):
 *
 *   1) Z80_DIRECT_REGS=0 -> z80_execute_batch() (původní "batch" jádro):
 *      hlavní registry (A,F,B,C,D,E,H,L,PC,SP,WZ,R,Q) jsou lokální proměnné
 *      (registrová cache), _lcache je aktivní, WRITEBACK/RELOAD synchronizují
 *      cache <-> cpu->...  Amortizuje prolog/epilog na celou dávku T-stavů.
 *
 *   2) Z80_DIRECT_REGS=1 -> z80_execute_step() (per-step jádro BEZ cache):
 *      rA..rWZ jsou #define aliasy přímo na cpu->...  _lcache se NEVYTVÁŘÍ,
 *      cpu->_active_cache zůstává NULL po celou dobu, WRITEBACK()/RELOAD()
 *      degenerují na no-op (není co přenášet, registry už ŽIJÍ v cpu->).
 *      Odstraňuje prolog (RELOAD 14 reg) + _lcache init (14 ptr) + epilog
 *      (WRITEBACK 13 reg) + stack-spilling způsobený vzetím adres registrů
 *      do _lcache. Voláno z80_step() pro per-1 (cycle-accurate) emu smyčku.
 *
 * Parametrizace:
 *   Z80_EXEC_FN     - jméno generované funkce (z80_execute_batch / _step)
 *   Z80_DIRECT_REGS - 0 = cache, 1 = registry přímo v cpu->
 *
 * Přístupová vrstva (rA..rWZ + AF_VAL/SET_AF/... + WRITEBACK/RELOAD) je
 * jediný rozdíl mezi variantami; všech ~549 míst sahajících na registry
 * přes tyto aliasy zůstává textově identických -> žádná diverze handlerů.
 *
 * Registr Q (YF/XF u SCF a CCF):
 *   Po instrukci, která zapisuje příznaky, je Q = F (i když se hodnota F
 *   nezměnila). Po instrukci, která je nezapisuje (včetně POP AF, EX AF,AF',
 *   opakovaného fetche HALT a přijetí přerušení), je Q = 0. SCF a CCF berou
 *   YF/XF z ((Q xor F) or A): po instrukci se zápisem příznaků jen z A, jinak
 *   z A or F. Zdroj: redcode/Z80 (sources/Z80.c), MAME z80.lst; na MZ-800
 *   změřeno programem syscheck (64 shod ze 64).
 *   Instrukce značí zápis příznaků makrem F_WRITE; Q se z toho odvodí na konci
 *   instrukce v Q_UPDATE (voláno z NEXT a NEXT_SLOW) a žije v cpu->q, takže
 *   přežije hranici volání a per-step i batch jádro dávají stejný výsledek.
 *
 * @param cpu Ukazatel na CPU instanci.
 * @param target_cycles Cílový počet T-stavů (ve step variantě fakticky 1).
 * @return Skutečný počet provedených T-stavů.
 * @pre cpu != NULL, z80_init() bylo voláno.
 */
HOT int Z80_EXEC_FN(z80_t *cpu, int target_cycles) {
    /*
     * Lokální cache callback pointerů - eliminuje opakovanou dereferenci
     * cpu->mread_cb na každém rd/wr/FETCH. GCC drží tyto hodnoty
     * v callee-saved registrech po celou dobu execute smyčky.
     */
#ifndef MZ800EMU_CFG_RAM_FASTPATH
    /*
     * Při zapnutém RAM fast-path jdou memory přístupy přes z80_fp_read/write
     * (čtou cpu->mread_cb/mwrite_cb až ve fall-back větvi), takže tato lokální
     * cache by byla nepoužita. Při vypnutém fast-path = původní A5 chování.
     */
    const z80_mread_cb  _mrd  = cpu->mread_cb;
    void * const        _mrd_d = cpu->mread_data;
    const z80_mwrite_cb _mwr  = cpu->mwrite_cb;
    void * const        _mwr_d = cpu->mwrite_data;
#endif
    const z80_pread_cb  _prd  = cpu->pread_cb;
    void * const        _prd_d = cpu->pread_data;
    const z80_pwrite_cb _pwr  = cpu->pwrite_cb;
    void * const        _pwr_d = cpu->pwrite_data;

#if Z80_DIRECT_REGS
    /*
     * Per-step varianta: hlavní registry jsou aliasy přímo na pole cpu->.
     * Žádná lokální cache, žádný stack-spilling z vzetí adres. _active_cache
     * zůstává NULL po celou dobu (invariant z80.h posílen na "VŽDY NULL"
     * ve step variantě).
     */
#define rA  (cpu->af.h)
#define rF  (cpu->af.l)
#define rB  (cpu->bc.h)
#define rC  (cpu->bc.l)
#define rD  (cpu->de.h)
#define rE  (cpu->de.l)
#define rH  (cpu->hl.h)
#define rL  (cpu->hl.l)
#define rPC (cpu->pc)
#define rSP (cpu->sp)
#define rWZ (cpu->wz.w)
#define rR  (cpu->r)
#define rQ  (cpu->q)
    /* _lcache se NEVYTVÁŘÍ; _active_cache musí zůstat NULL po celou dobu. */
    cpu->_active_cache = NULL;
#else
    /* Lokální registrová cache (batch varianta) */
    uint8_t  rA  = cpu->af.h;
    uint8_t  rF  = cpu->af.l;
    uint8_t  rB  = cpu->bc.h;
    uint8_t  rC  = cpu->bc.l;
    uint8_t  rD  = cpu->de.h;
    uint8_t  rE  = cpu->de.l;
    uint8_t  rH  = cpu->hl.h;
    uint8_t  rL  = cpu->hl.l;
    uint16_t rPC = cpu->pc;
    uint16_t rSP = cpu->sp;
    uint16_t rWZ = cpu->wz.w;
    uint8_t  rR  = cpu->r;
    uint8_t  rQ  = cpu->q;

    /*
     * Registrace pointerů na lokální cache - umožňuje z80_set_reg() volané
     * z callbacku probíhající instrukce propsat změny ihned do těchto
     * lokálních proměnných (jinak by je WRITEBACK na konci instrukce
     * přepsal zpět starou hodnotou).
     */
    z80_local_cache_t _lcache = {
        .A = &rA, .F = &rF,
        .B = &rB, .C = &rC,
        .D = &rD, .E = &rE,
        .H = &rH, .L = &rL,
        .PC = &rPC, .SP = &rSP, .WZ = &rWZ,
        .R = &rR, .Q = &rQ,
    };
    cpu->_active_cache = &_lcache;
#endif

    int cycles_executed = 0;
    int last_sync = 0;  /* Poslední bod synchronizace cpu->cycles */

    /*
     * Maska "probíhající instrukce zapsala příznaky": 0xFF po F_WRITE, jinak 0.
     * Není architektonický registr; Q_UPDATE ji na konci každé instrukce
     * spotřebuje a vynuluje, takže na hranici volání je vždy 0.
     */
    uint8_t f_written = 0;

    /**
     * @brief Zápis příznaků instrukcí: F_WRITE = výraz;
     *
     * Každá instrukce, která zapisuje příznaky, musí svůj první zápis do F
     * provést přes F_WRITE; další úpravy F v téže instrukci smí jít přes rF.
     * Instrukce, které F jen přepisují z paměti nebo stínového registru
     * (POP AF, EX AF,AF'), F_WRITE nepoužívají.
     *
     * Omezení: rozvine se na čárkový výraz, proto jen jako samostatný
     * přiřazovací příkaz, ne uvnitř jiného výrazu ani v deklaraci.
     * @post f_written = 0xFF, F obsahuje přiřazenou hodnotu.
     */
#define F_WRITE f_written = 0xFF, rF

    /**
     * @brief Nastaví registr Q na konci instrukce a vynuluje f_written.
     *
     * Q = F, pokud instrukce zapsala příznaky (F_WRITE), jinak Q = 0.
     * @pre Volá se právě jednou na konci každé instrukce (NEXT, NEXT_SLOW),
     *      po posledním zápisu do F.
     */
#define Q_UPDATE() do { rQ = rF & f_written; f_written = 0; } while(0)

    /* Makra pro přístup k 16bit párům z lokálních proměnných */
#define AF_VAL  ((uint16_t)((rA << 8) | rF))
#define BC_VAL  ((uint16_t)((rB << 8) | rC))
#define DE_VAL  ((uint16_t)((rD << 8) | rE))
#define HL_VAL  ((uint16_t)((rH << 8) | rL))

    /*
     * SET_AF přepíše F bez označení zápisu příznaků: POP AF a EX AF,AF'
     * nechávají Q = 0 (viz F_WRITE).
     */
#define SET_AF(v) do { rA = (uint8_t)((v) >> 8); rF = (uint8_t)(v); } while(0)
#define SET_BC(v) do { rB = (uint8_t)((v) >> 8); rC = (uint8_t)(v); } while(0)
#define SET_DE(v) do { rD = (uint8_t)((v) >> 8); rE = (uint8_t)(v); } while(0)
#define SET_HL(v) do { rH = (uint8_t)((v) >> 8); rL = (uint8_t)(v); } while(0)

    /* WZ přístup */
#define WZH_VAL ((uint8_t)(rWZ >> 8))
#define WZL_VAL ((uint8_t)(rWZ & 0xFF))
#define SET_WZH(v) do { rWZ = (rWZ & 0x00FF) | ((uint16_t)(v) << 8); } while(0)
#define SET_WZL(v) do { rWZ = (rWZ & 0xFF00) | (uint8_t)(v); } while(0)

    /*
     * Makra pro přístup k paměti/IO přes lokální cache callback pointerů.
     * Používají _mrd/_mwr/_prd/_pwr místo cpu->mread_cb atd.
     *
     * E1 (KROK 1): při zapnutém MZ800EMU_CFG_RAM_FASTPATH jdou čtení/zápisy
     * do paměti přes z80_fp_read/z80_fp_write (inline page-table fast-path se
     * fall-backem na callback). op_tstate inkrementuje makro (timing zachován),
     * helper řeší jen přístup + replikaci vedlejších efektů (regDBUS_latch).
     * IO porty fast-path NEMÁ (zůstávají vždy přes _prd/_pwr).
     */
#ifdef MZ800EMU_CFG_RAM_FASTPATH
#define RD(addr)        (cpu->op_tstate += 3, z80_fp_read(cpu, (addr), 0))
#define WR(addr, val)   do { cpu->op_tstate += 3; z80_fp_write(cpu, (addr), (val)); } while(0)
#define M1_FETCH()      (cpu->op_tstate += 4, z80_fp_read(cpu, rPC++, 1))
#define FETCH()         (cpu->op_tstate += 3, z80_fp_read(cpu, rPC++, 0))
#else
#define RD(addr)        (cpu->op_tstate += 3, _mrd(cpu, (addr), 0, _mrd_d))
#define WR(addr, val)   do { cpu->op_tstate += 3; _mwr(cpu, (addr), (val), _mwr_d); } while(0)
    /* M1 fetch z PC (opkódový cyklus, 4 T-stavy, m1_state=1) - jen v DISPATCH a prefix handlerech */
#define M1_FETCH() (cpu->op_tstate += 4, _mrd(cpu, rPC++, 1, _mrd_d))
    /* Operandový fetch z PC (3 T-stavy, m1_state=0) - pro čtení operandu v instrukcích */
#define FETCH() (cpu->op_tstate += 3, _mrd(cpu, rPC++, 0, _mrd_d))
#endif

#define IO_RD(port)     (cpu->op_tstate += 4, _prd(cpu, (port), _prd_d))
#define IO_WR(port, val) do { cpu->op_tstate += 4; _pwr(cpu, (port), (val), _pwr_d); } while(0)
/* Čtení 16bitové hodnoty z paměti: nejdřív nižší bajt z addr, pak vyšší
 * z addr + 1, jako Z80, a každé čtení se svým T-stavem. Pořadí musí vynutit
 * operátor čárky. Dřívější výraz RD(a) | (RD(a + 1) << 8) měnil
 * cpu->op_tstate dvakrát bez určeného pořadí (nedefinované chování) a GCC
 * provedl obě přičtení před prvním čtením: adresy šly ve správném pořadí,
 * ale první (nižší) bajt viděl op_tstate o 3 T pozdější, stejný jako druhý.
 * Na tom záleží u synchronizace s GDG (čekací takty VRAM, čas čtení
 * mapovaných portů).
 * Používá tmp_lo a tmp_hi; argument se vyhodnocuje dvakrát a nesmí mít
 * vedlejší efekt ani používat FETCH16(). */
#define RD16(addr)      (tmp_lo = RD(addr), tmp_hi = RD((uint16_t)((addr) + 1)), (uint16_t)(tmp_lo | (tmp_hi << 8)))
#define WR16(addr, val) do { WR((addr), (uint8_t)((val) & 0xFF)); WR((uint16_t)((addr) + 1), (uint8_t)((val) >> 8)); } while(0)

#define FETCH16() (tmp_lo = FETCH(), tmp_hi = FETCH(), (uint16_t)(tmp_lo | (tmp_hi << 8)))

    /* Inkrementace R (dolních 7 bitů) */
#define INC_R() do { rR = (rR & 0x80) | ((rR + 1) & 0x7F); } while(0)

    /* Zápis na zásobník: vyšší bajt na SP+1 dřív, pak nižší na SP (Zilog
     * UM0080 u PUSH qq, PUSH IX/IY, CALL, CALL cc, RST). WR16 (nižší bajt
     * dřív) zůstává pro LD (nn),rr. */
#define WR16_STACK(addr, val) do { WR((uint16_t)((addr) + 1), (uint8_t)((val) >> 8)); WR((addr), (uint8_t)((val) & 0xFF)); } while(0)

    /* PUSH/POP přes lokální SP a RD16/WR16_STACK makra */
#define PUSH(val) do { rSP -= 2; WR16_STACK(rSP, val); } while(0)
#define POP() (tmp16 = RD16(rSP), rSP += 2, tmp16)

#if Z80_DIRECT_REGS
    /*
     * Per-step varianta: registry ŽIJÍ přímo v cpu->, není co přenášet.
     * WRITEBACK i RELOAD jsou úplný no-op (Q žije v cpu->q stejně jako
     * ostatní registry).
     */
#define WRITEBACK() do { } while(0)
#define RELOAD()    do { } while(0)
#else
    /* Writeback lokálních registrů do struktury */
#define WRITEBACK() do { \
    cpu->af.h = rA; cpu->af.l = rF; \
    cpu->bc.h = rB; cpu->bc.l = rC; \
    cpu->de.h = rD; cpu->de.l = rE; \
    cpu->hl.h = rH; cpu->hl.l = rL; \
    cpu->pc = rPC; cpu->sp = rSP; \
    cpu->wz.w = rWZ; cpu->r = rR; \
    cpu->q = rQ; \
} while(0)

    /* Reload lokálních registrů ze struktury */
#define RELOAD() do { \
    rA = cpu->af.h; rF = cpu->af.l; \
    rB = cpu->bc.h; rC = cpu->bc.l; \
    rD = cpu->de.h; rE = cpu->de.l; \
    rH = cpu->hl.h; rL = cpu->hl.l; \
    rPC = cpu->pc; rSP = cpu->sp; \
    rWZ = cpu->wz.w; rR = cpu->r; \
    rQ = cpu->q; \
} while(0)
#endif

    /*
     * Fire iff_change_cb pokud nastaven. Voláno po změně cpu->iff1/iff2.
     * Hot path: NULL ptr check + branch (= ~0 cycles pokud cb NULL).
     * Při volání callbacku z dispatch (op_F3, op_FB) je nutné před-zavolat
     * WRITEBACK() a po-RELOAD() (= callback může číst registry přes
     * z80_get_reg apod.). Reason = z80_iff_change_reason_t cast na uint8_t.
     */
#define FIRE_IFF_CHANGE(reason) do { \
    if (cpu->iff_change_cb) { \
        cpu->iff_change_cb(cpu, cpu->iff1, cpu->iff2, \
                           (uint8_t)(reason), cpu->iff_change_data); \
    } \
} while(0)

    /*
     * Fire call_cb (CALL přijat) - volá se v opcode handleru CALL nn /
     * CALL cc,nn před `PUSH(rPC); rPC = addr;`. V okamžiku fire platí:
     *
     *   - rPC ukazuje za CALL instrukci (= return adresa, FETCH16 už
     *     proběhl pro target operand).
     *   - target = lokální `addr` opcode handleru (= fetchnutý cíl).
     *
     * call_site dopočteme jako rPC - 3 (1 byte opcode + 2 bytes target).
     * WRITEBACK/RELOAD synchronizuje lokální registr cache s cpu->...w -
     * callback může číst registry přes z80_get_reg() aniž by viděl
     * zastaralé hodnoty.
     *
     * Hot path: jediný NULL ptr check (~0 cycles při call_cb == NULL).
     */
#define FIRE_CALL(target_addr) do { \
    if (cpu->call_cb) { \
        WRITEBACK(); \
        cpu->call_cb(cpu, (uint16_t)(rPC - 3), (target_addr), \
                     rPC, cpu->call_data); \
        RELOAD(); \
    } \
} while(0)

    /*
     * Fire ret_cb (RET přijat) - volá se v opcode handleru RET / RET cc
     * před `rPC = POP();`. V okamžiku fire platí:
     *
     *   - rSP ukazuje na slot s return adresou (= pop_target).
     *   - PEEK16 nemodifikuje SP, jen načte vrchol stacku.
     *
     * Použít přímo RD16(rSP) by inkrementovalo op_tstate u zbytečné
     * druhé čtenářské operace (PEEK je čistě read, ne stack pop). Tady
     * vyrobíme PEEK16 přímo přes rd_slow (= mread bez op_tstate tiku),
     * což odpovídá běžnému debug peek vzoru jinde v jádře.
     *
     * Hot path: jediný NULL ptr check.
     */
#define FIRE_RET() do { \
    if (cpu->ret_cb) { \
        WRITEBACK(); \
        uint8_t _ret_lo = rd_slow(cpu, rSP); \
        uint8_t _ret_hi = rd_slow(cpu, (uint16_t)(rSP + 1)); \
        uint16_t _ret_tgt = (uint16_t)(_ret_lo | (_ret_hi << 8)); \
        cpu->ret_cb(cpu, _ret_tgt, rSP, cpu->ret_data); \
        RELOAD(); \
    } \
} while(0)

    /* Aritmetické operace používající lokální cache */

    /* 8bit ADD - vrací výsledek, nastavuje rF */
#define ADD8(a, b) ({ \
    uint16_t _r16 = (a) + (b); \
    uint8_t _r8 = (uint8_t)_r16; \
    F_WRITE = sz53_table[_r8] \
       | ((_r16 >> 8) & CF) \
       | (((a) ^ (b) ^ _r8) & HF) \
       | ((((a) ^ (b) ^ 0x80) & ((a) ^ _r8) & 0x80) >> 5); \
    _r8; })

    /* 8bit ADC */
#define ADC8(a, b) ({ \
    uint16_t _r16 = (a) + (b) + (rF & CF); \
    uint8_t _r8 = (uint8_t)_r16; \
    F_WRITE = sz53_table[_r8] \
       | ((_r16 >> 8) & CF) \
       | (((a) ^ (b) ^ _r8) & HF) \
       | ((((a) ^ (b) ^ 0x80) & ((a) ^ _r8) & 0x80) >> 5); \
    _r8; })

    /* 8bit SUB */
#define SUB8(a, b) ({ \
    uint16_t _r16 = (a) - (b); \
    uint8_t _r8 = (uint8_t)_r16; \
    F_WRITE = sz53_table[_r8] \
       | ((_r16 >> 8) & CF) \
       | NF \
       | (((a) ^ (b) ^ _r8) & HF) \
       | ((((a) ^ (b)) & ((a) ^ _r8) & 0x80) >> 5); \
    _r8; })

    /* 8bit SBC */
#define SBC8(a, b) ({ \
    uint16_t _r16 = (a) - (b) - (rF & CF); \
    uint8_t _r8 = (uint8_t)_r16; \
    F_WRITE = sz53_table[_r8] \
       | ((_r16 >> 8) & CF) \
       | NF \
       | (((a) ^ (b) ^ _r8) & HF) \
       | ((((a) ^ (b)) & ((a) ^ _r8) & 0x80) >> 5); \
    _r8; })

    /* AND */
#define AND8(val) do { rA &= (val); F_WRITE = sz53p_table[rA] | HF; } while(0)

    /* XOR */
#define XOR8(val) do { rA ^= (val); F_WRITE = sz53p_table[rA]; } while(0)

    /* OR */
#define OR8(val) do { rA |= (val); F_WRITE = sz53p_table[rA]; } while(0)

    /* CP - porovnání (jako SUB, ale výsledek se neukládá, F3/F5 z operandu) */
#define CP8(val) do { \
    uint8_t _v = (val); \
    uint16_t _r16 = rA - _v; \
    uint8_t _r8 = (uint8_t)_r16; \
    F_WRITE = (sz53_table[_r8] & ~(F3 | F5)) \
       | (_v & (F3 | F5)) \
       | ((_r16 >> 8) & CF) \
       | NF \
       | ((rA ^ _v ^ _r8) & HF) \
       | (((rA ^ _v) & (rA ^ _r8) & 0x80) >> 5); \
} while(0)

    /* INC 8bit */
#define INC8(val) ({ \
    uint8_t _r = (val) + 1; \
    F_WRITE = (rF & CF) \
       | sz53_table[_r] \
       | ((_r & 0x0F) == 0 ? HF : 0) \
       | ((val) == 0x7F ? PF : 0); \
    _r; })

    /* DEC 8bit */
#define DEC8(val) ({ \
    uint8_t _r = (val) - 1; \
    F_WRITE = (rF & CF) \
       | sz53_table[_r] \
       | NF \
       | ((_r & 0x0F) == 0x0F ? HF : 0) \
       | ((val) == 0x80 ? PF : 0); \
    _r; })

    /* ADD 16bit (HL += val) - dest je pointer na registr, val je uint16_t */
#define ADD16(dest_h, dest_l, val) do { \
    uint16_t _dest = ((uint16_t)(dest_h) << 8) | (dest_l); \
    uint32_t _r32 = _dest + (val); \
    uint8_t _rh = (uint8_t)(_r32 >> 8); \
    F_WRITE = (rF & (SF | ZF | PF)) \
       | (_rh & (F3 | F5)) \
       | ((_r32 >> 16) & CF) \
       | (((_dest ^ (val) ^ _r32) >> 8) & HF); \
    (dest_h) = _rh; \
    (dest_l) = (uint8_t)_r32; \
} while(0)

    /* ADC HL, val */
#define ADC16(val) do { \
    uint16_t _hl = HL_VAL; \
    uint32_t _r32 = _hl + (val) + (rF & CF); \
    uint8_t _rh = (uint8_t)(_r32 >> 8); \
    F_WRITE = ((_r32 >> 8) & SF) \
       | ((_r32 & 0xFFFF) == 0 ? ZF : 0) \
       | (_rh & (F3 | F5)) \
       | ((_r32 >> 16) & CF) \
       | (((_hl ^ (val) ^ 0x8000) & (_hl ^ _r32) & 0x8000) >> 13) \
       | (((_hl ^ (val) ^ _r32) >> 8) & HF); \
    rH = _rh; rL = (uint8_t)_r32; \
} while(0)

    /* SBC HL, val */
#define SBC16(val) do { \
    uint16_t _hl = HL_VAL; \
    uint32_t _r32 = _hl - (val) - (rF & CF); \
    uint8_t _rh = (uint8_t)(_r32 >> 8); \
    F_WRITE = ((_r32 >> 8) & SF) \
       | ((_r32 & 0xFFFF) == 0 ? ZF : 0) \
       | (_rh & (F3 | F5)) \
       | ((_r32 >> 16) & CF) \
       | NF \
       | (((_hl ^ (val)) & (_hl ^ _r32) & 0x8000) >> 13) \
       | (((_hl ^ (val) ^ _r32) >> 8) & HF); \
    rH = _rh; rL = (uint8_t)_r32; \
} while(0)

    /* Rotace/posuvy (CB prefix) - vracejí výsledek, nastavují rF */
#define RLC(val) ({ \
    uint8_t _c = ((val) >> 7) & 1; \
    uint8_t _r = ((val) << 1) | _c; \
    F_WRITE = sz53p_table[_r] | _c; _r; })

#define RRC(val) ({ \
    uint8_t _c = (val) & 1; \
    uint8_t _r = ((val) >> 1) | (_c << 7); \
    F_WRITE = sz53p_table[_r] | _c; _r; })

#define RL(val) ({ \
    uint8_t _c = ((val) >> 7) & 1; \
    uint8_t _r = ((val) << 1) | (rF & CF); \
    F_WRITE = sz53p_table[_r] | _c; _r; })

#define RR(val) ({ \
    uint8_t _c = (val) & 1; \
    uint8_t _r = ((val) >> 1) | ((rF & CF) << 7); \
    F_WRITE = sz53p_table[_r] | _c; _r; })

#define SLA(val) ({ \
    uint8_t _c = ((val) >> 7) & 1; \
    uint8_t _r = (val) << 1; \
    F_WRITE = sz53p_table[_r] | _c; _r; })

#define SRA(val) ({ \
    uint8_t _c = (val) & 1; \
    uint8_t _r = ((val) >> 1) | ((val) & 0x80); \
    F_WRITE = sz53p_table[_r] | _c; _r; })

#define SLL(val) ({ \
    uint8_t _c = ((val) >> 7) & 1; \
    uint8_t _r = ((val) << 1) | 1; \
    F_WRITE = sz53p_table[_r] | _c; _r; })

#define SRL(val) ({ \
    uint8_t _c = (val) & 1; \
    uint8_t _r = (val) >> 1; \
    F_WRITE = sz53p_table[_r] | _c; _r; })

    /* BIT n, val */
#define BIT_OP(n, val) do { \
    uint8_t _r = (val) & (1 << (n)); \
    F_WRITE = (rF & CF) | HF | (_r & SF); \
    if (_r == 0) rF |= ZF | PF; \
    rF |= ((val) & (F3 | F5)); \
} while(0)

    /* BIT n, (HL)/(IX+d)/(IY+d) - F3/F5 z horního bajtu adresy */
#define BIT_MEM(n, val, addr_hi) do { \
    uint8_t _r = (val) & (1 << (n)); \
    F_WRITE = (rF & CF) | HF | (_r & SF); \
    if (_r == 0) rF |= ZF | PF; \
    rF |= ((addr_hi) & (F3 | F5)); \
} while(0)

    /* Dočasné proměnné pro FETCH16/POP */
    uint8_t tmp_lo, tmp_hi;
    uint16_t tmp16;
    int cycles;

    /* Strop řetězu prefixů DD/FD v jednom kroku (viz DDFD_IGNORED). */
#define DDFD_CHAIN_MAX 64
    /* Počet prefixů DD/FD řetězu za prvním; nuluje vstup op_DD/op_FD. */
    int ddfd_chain = 0;

    /* ========== Dispatch tabulky (computed goto) ========== */

#if USE_COMPUTED_GOTO

    /* Base opcode dispatch tabulka (256 záznamů) */
    static const void* const base_dispatch[256] = {
        &&op_00, &&op_01, &&op_02, &&op_03, &&op_04, &&op_05, &&op_06, &&op_07,
        &&op_08, &&op_09, &&op_0A, &&op_0B, &&op_0C, &&op_0D, &&op_0E, &&op_0F,
        &&op_10, &&op_11, &&op_12, &&op_13, &&op_14, &&op_15, &&op_16, &&op_17,
        &&op_18, &&op_19, &&op_1A, &&op_1B, &&op_1C, &&op_1D, &&op_1E, &&op_1F,
        &&op_20, &&op_21, &&op_22, &&op_23, &&op_24, &&op_25, &&op_26, &&op_27,
        &&op_28, &&op_29, &&op_2A, &&op_2B, &&op_2C, &&op_2D, &&op_2E, &&op_2F,
        &&op_30, &&op_31, &&op_32, &&op_33, &&op_34, &&op_35, &&op_36, &&op_37,
        &&op_38, &&op_39, &&op_3A, &&op_3B, &&op_3C, &&op_3D, &&op_3E, &&op_3F,
        &&op_40, &&op_41, &&op_42, &&op_43, &&op_44, &&op_45, &&op_46, &&op_47,
        &&op_48, &&op_49, &&op_4A, &&op_4B, &&op_4C, &&op_4D, &&op_4E, &&op_4F,
        &&op_50, &&op_51, &&op_52, &&op_53, &&op_54, &&op_55, &&op_56, &&op_57,
        &&op_58, &&op_59, &&op_5A, &&op_5B, &&op_5C, &&op_5D, &&op_5E, &&op_5F,
        &&op_60, &&op_61, &&op_62, &&op_63, &&op_64, &&op_65, &&op_66, &&op_67,
        &&op_68, &&op_69, &&op_6A, &&op_6B, &&op_6C, &&op_6D, &&op_6E, &&op_6F,
        &&op_70, &&op_71, &&op_72, &&op_73, &&op_74, &&op_75, &&op_76, &&op_77,
        &&op_78, &&op_79, &&op_7A, &&op_7B, &&op_7C, &&op_7D, &&op_7E, &&op_7F,
        &&op_80, &&op_81, &&op_82, &&op_83, &&op_84, &&op_85, &&op_86, &&op_87,
        &&op_88, &&op_89, &&op_8A, &&op_8B, &&op_8C, &&op_8D, &&op_8E, &&op_8F,
        &&op_90, &&op_91, &&op_92, &&op_93, &&op_94, &&op_95, &&op_96, &&op_97,
        &&op_98, &&op_99, &&op_9A, &&op_9B, &&op_9C, &&op_9D, &&op_9E, &&op_9F,
        &&op_A0, &&op_A1, &&op_A2, &&op_A3, &&op_A4, &&op_A5, &&op_A6, &&op_A7,
        &&op_A8, &&op_A9, &&op_AA, &&op_AB, &&op_AC, &&op_AD, &&op_AE, &&op_AF,
        &&op_B0, &&op_B1, &&op_B2, &&op_B3, &&op_B4, &&op_B5, &&op_B6, &&op_B7,
        &&op_B8, &&op_B9, &&op_BA, &&op_BB, &&op_BC, &&op_BD, &&op_BE, &&op_BF,
        &&op_C0, &&op_C1, &&op_C2, &&op_C3, &&op_C4, &&op_C5, &&op_C6, &&op_C7,
        &&op_C8, &&op_C9, &&op_CA, &&op_CB, &&op_CC, &&op_CD, &&op_CE, &&op_CF,
        &&op_D0, &&op_D1, &&op_D2, &&op_D3, &&op_D4, &&op_D5, &&op_D6, &&op_D7,
        &&op_D8, &&op_D9, &&op_DA, &&op_DB, &&op_DC, &&op_DD, &&op_DE, &&op_DF,
        &&op_E0, &&op_E1, &&op_E2, &&op_E3, &&op_E4, &&op_E5, &&op_E6, &&op_E7,
        &&op_E8, &&op_E9, &&op_EA, &&op_EB, &&op_EC, &&op_ED, &&op_EE, &&op_EF,
        &&op_F0, &&op_F1, &&op_F2, &&op_F3, &&op_F4, &&op_F5, &&op_F6, &&op_F7,
        &&op_F8, &&op_F9, &&op_FA, &&op_FB, &&op_FC, &&op_FD, &&op_FE, &&op_FF,
    };

    /* Dispatch makra pro computed goto */
#define DISPATCH() do { \
    cpu->op_tstate = 0; \
    uint8_t _op = M1_FETCH(); \
    INC_R(); \
    goto *base_dispatch[_op]; \
} while(0)

    /*
     * NEXT: lightweight - jen přičte cykly a dispatch další instrukce.
     * Interrupt handling a post_step jsou ve slow path (check_interrupts label).
     */
#define NEXT(c) do { \
    Q_UPDATE(); \
    cycles_executed += (c) + cpu->wait_cycles; \
    cpu->wait_cycles = 0; \
    if (__builtin_expect(cycles_executed >= target_cycles, 0)) { \
        cpu->cycles += cycles_executed - last_sync; \
        cpu->total_cycles += cycles_executed - last_sync; \
        last_sync = cycles_executed; \
        goto check_interrupts; \
    } \
    DISPATCH(); \
} while(0)

    /* NEXT_SLOW: pro instrukce co mění interrupt stav (EI, DI, HALT, RETI, RETN) */
#define NEXT_SLOW(c) do { \
    Q_UPDATE(); \
    cycles_executed += (c) + cpu->wait_cycles; \
    cpu->wait_cycles = 0; \
    cpu->cycles += cycles_executed - last_sync; \
    cpu->total_cycles += cycles_executed - last_sync; \
    last_sync = cycles_executed; \
    goto check_interrupts; \
} while(0)

#else /* !USE_COMPUTED_GOTO - fallback na switch */

#define DISPATCH() do { cpu->op_tstate = 0; goto dispatch_switch; } while(0)
#define NEXT(c) do { \
    Q_UPDATE(); \
    cycles_executed += (c) + cpu->wait_cycles; \
    cpu->wait_cycles = 0; \
    if (cycles_executed >= target_cycles) { \
        cpu->cycles += cycles_executed - last_sync; \
        cpu->total_cycles += cycles_executed - last_sync; \
        last_sync = cycles_executed; \
        goto check_interrupts; \
    } \
    DISPATCH(); \
} while(0)
#define NEXT_SLOW(c) NEXT(c)

#endif /* USE_COMPUTED_GOTO */

    /* Vstupní bod: počáteční interrupt check a start dispatch */
    if (!cpu->external_int_handling) {
        bool ld_a_ir_was = cpu->ld_a_ir;
        cpu->ld_a_ir = false;
        if (cpu->nmi_pending || (cpu->iff1 && !cpu->ei_delay)) {
            WRITEBACK();
            int int_cyc = handle_interrupts_internal(cpu);
            if (int_cyc > 0) {
                if (ld_a_ir_was) cpu->af.l &= ~PF;
                cpu->cycles += int_cyc;
                cpu->total_cycles += int_cyc;
                cycles_executed += int_cyc;
                last_sync = cycles_executed;
                RELOAD();
            } else {
                RELOAD();
            }
        }
    }
    /* ei_delay se musí vyčistit vždy - i při external_int_handling */
    cpu->ei_delay = false;

    if (cpu->halted) {
        /* Opakovaný fetch HALT (po 4 T, kvůli refreshi DRAM) je M1 cyklus,
         * proto R += 1 (báze cpu/z80/07-interrupts.md "HALT instrukce" +
         * 09-r-register.md; z80ex totéž). Bez toho loader Interkarate
         * po HALT dešifroval špatným klíčem (regrese v2.0.2). */
        INC_R();
        /* Opakovaný fetch HALT je interní NOP: příznaky nezapisuje, Q = 0. */
        rQ = 0;
        cycles = 4;
        cpu->cycles += cycles;
        cpu->total_cycles += cycles;
        cycles_executed += cycles;
        last_sync = cycles_executed;
        goto done;
    }

    DISPATCH();

    /* Slow path: zpracování přerušení, post_step, kontrola halted */
check_interrupts:
    {
        /* Post-step callback (MZ-700 per-line WAIT) */
        if (cpu->post_step_cb) {
            WRITEBACK();
            cpu->wait_cycles = 0;
            cpu->post_step_cb(cpu, cpu->post_step_data);
            if (cpu->wait_cycles > 0) {
                cpu->cycles += cpu->wait_cycles;
                cpu->total_cycles += cpu->wait_cycles;
                cycles_executed += cpu->wait_cycles;
                last_sync = cycles_executed;
            }
            RELOAD();
        }

        /* Zpracování přerušení */
        if (!cpu->external_int_handling) {
            bool ld_a_ir_was = cpu->ld_a_ir;
            cpu->ld_a_ir = false;

            if (cpu->nmi_pending || (cpu->iff1 && !cpu->ei_delay)) {
                WRITEBACK();
                int int_cyc = handle_interrupts_internal(cpu);
                if (int_cyc > 0) {
                    if (ld_a_ir_was) cpu->af.l &= ~PF;
                    cpu->cycles += int_cyc;
                    cpu->total_cycles += int_cyc;
                    cycles_executed += int_cyc;
                    last_sync = cycles_executed;
                    RELOAD();
                } else {
                    RELOAD();
                }
            }
        }
#if !Z80_DIRECT_REGS
        /* BATCH (Z80_DIRECT_REGS=0, veřejné z80_execute): vstupní clear na
         * začátku z80_execute() běží jen 1x na celou dávku, proto ei_delay
         * nastavený instrukcí EI uvnitř dávky musí smazat tento epilog-clear,
         * jinak by zůstal viset přes další instrukce v jednom z80_execute
         * volání a maskovatelné přerušení by se po EI uvnitř dávky nepřijalo. */
        cpu->ei_delay = false;
#else
        /* PER-STEP (Z80_DIRECT_REGS=1, z80_step) - EI-delay fix (gun-runner):
         * clear je ZÁMĚRNĚ VYNECHÁN. ei_delay nastavený op_FB (EI) musí PŘEŽÍT
         * návrat z z80_step(), aby ho mzarch interrupt poll (mzarch_main_process_
         * interrupt: 'iff1 && !ei_delay') ještě viděl nastavený a přerušení po EI
         * zablokoval. Vyčistí ho až vstupní clear na začátku PŘÍŠTÍHO z80_step,
         * PŘED dispatchem instrukce následující po EI -> EI-delay = přesně jedna
         * instrukce. Replikuje noint_once z80ex jádra (set v EI(), clear na
         * začátku příštího stepu). Bez tohoto byl interrupt přijat hned po EI:
         * u sekvence "EI; LD SP,nn" pak push šel na starý zásobník a poškodil
         * paměť (regrese v2.0.2 - hra Gun Runner se zasekla). */
#endif

        /* HALT - cykly už přičteny v NEXT_SLOW(4) */
        if (cpu->halted) goto done;

        if (cycles_executed >= target_cycles) goto done;

        /* INVARIANT (EI-delay lifetime): v per-step variantě (Z80_DIRECT_REGS=1,
         * target_cycles=1) je tento DISPATCH() nedosažitelný - řádek výše vždy
         * skočí na 'done' (každá instrukce spotřebuje >=4 T). Používá se jen v
         * batch. POZOR: kdyby se v per-step na tento DISPATCH v budoucnu dalo
         * dostat (např. pre-empce / wait state), obešel by vstupní clear ei_delay
         * a delay po EI by se protáhl na 2 instrukce. */
        DISPATCH();
    }

    /* ========== Base opkody (0x00-0xFF) ========== */

    /* NOP */
    op_00: NEXT(4);

    /* LD BC, nn */
    op_01: rC = FETCH(); rB = FETCH(); NEXT(10);

    /* LD (BC), A */
    op_02: WR(BC_VAL, rA); SET_WZL((BC_VAL + 1) & 0xFF); SET_WZH(rA); NEXT(7);

    /* INC BC */
    op_03: { uint16_t bc = BC_VAL + 1; SET_BC(bc); NEXT(6); }

    /* INC B */
    op_04: rB = INC8(rB); NEXT(4);

    /* DEC B */
    op_05: rB = DEC8(rB); NEXT(4);

    /* LD B, n */
    op_06: rB = FETCH(); NEXT(7);

    /* RLCA */
    op_07: { uint8_t c = rA >> 7; rA = (rA << 1) | c; F_WRITE = (rF & (SF|ZF|PF)) | (rA & (F3|F5)) | c; NEXT(4); }

    /* EX AF, AF' */
    op_08: { uint16_t tmp = AF_VAL; uint16_t af2 = cpu->af2.w; SET_AF(af2); cpu->af2.w = tmp; NEXT(4); }

    /* ADD HL, BC */
    op_09: rWZ = HL_VAL + 1; ADD16(rH, rL, BC_VAL); NEXT(11);

    /* LD A, (BC) */
    op_0A: rA = RD(BC_VAL); rWZ = BC_VAL + 1; NEXT(7);

    /* DEC BC */
    op_0B: { uint16_t bc = BC_VAL - 1; SET_BC(bc); NEXT(6); }

    /* INC C */
    op_0C: rC = INC8(rC); NEXT(4);

    /* DEC C */
    op_0D: rC = DEC8(rC); NEXT(4);

    /* LD C, n */
    op_0E: rC = FETCH(); NEXT(7);

    /* RRCA */
    op_0F: { uint8_t c = rA & 1; rA = (rA >> 1) | (c << 7); F_WRITE = (rF & (SF|ZF|PF)) | (rA & (F3|F5)) | c; NEXT(4); }

    /* DJNZ */
    op_10: { int8_t d = (int8_t)FETCH(); rB--; if (rB != 0) { rPC += d; rWZ = rPC; NEXT(13); } else { NEXT(8); } }

    /* LD DE, nn */
    op_11: rE = FETCH(); rD = FETCH(); NEXT(10);

    /* LD (DE), A */
    op_12: WR(DE_VAL, rA); SET_WZL((DE_VAL + 1) & 0xFF); SET_WZH(rA); NEXT(7);

    /* INC DE */
    op_13: { uint16_t de = DE_VAL + 1; SET_DE(de); NEXT(6); }

    /* INC D */
    op_14: rD = INC8(rD); NEXT(4);

    /* DEC D */
    op_15: rD = DEC8(rD); NEXT(4);

    /* LD D, n */
    op_16: rD = FETCH(); NEXT(7);

    /* RLA */
    op_17: { uint8_t c = rA >> 7; rA = (rA << 1) | (rF & CF); F_WRITE = (rF & (SF|ZF|PF)) | (rA & (F3|F5)) | c; NEXT(4); }

    /* JR d */
    op_18: { int8_t d = (int8_t)FETCH(); rPC += d; rWZ = rPC; NEXT(12); }

    /* ADD HL, DE */
    op_19: rWZ = HL_VAL + 1; ADD16(rH, rL, DE_VAL); NEXT(11);

    /* LD A, (DE) */
    op_1A: rA = RD(DE_VAL); rWZ = DE_VAL + 1; NEXT(7);

    /* DEC DE */
    op_1B: { uint16_t de = DE_VAL - 1; SET_DE(de); NEXT(6); }

    /* INC E */
    op_1C: rE = INC8(rE); NEXT(4);

    /* DEC E */
    op_1D: rE = DEC8(rE); NEXT(4);

    /* LD E, n */
    op_1E: rE = FETCH(); NEXT(7);

    /* RRA */
    op_1F: { uint8_t c = rA & 1; rA = (rA >> 1) | ((rF & CF) << 7); F_WRITE = (rF & (SF|ZF|PF)) | (rA & (F3|F5)) | c; NEXT(4); }

    /* JR NZ, d */
    op_20: { int8_t d = (int8_t)FETCH(); if (!(rF & ZF)) { rPC += d; rWZ = rPC; NEXT(12); } else { NEXT(7); } }

    /* LD HL, nn */
    op_21: rL = FETCH(); rH = FETCH(); NEXT(10);

    /* LD (nn), HL */
    op_22: { uint16_t addr = FETCH16(); WR16(addr, HL_VAL); rWZ = addr + 1; NEXT(16); }

    /* INC HL */
    op_23: { uint16_t hl = HL_VAL + 1; SET_HL(hl); NEXT(6); }

    /* INC H */
    op_24: rH = INC8(rH); NEXT(4);

    /* DEC H */
    op_25: rH = DEC8(rH); NEXT(4);

    /* LD H, n */
    op_26: rH = FETCH(); NEXT(7);

    /* DAA */
    op_27: {
        uint16_t daa_idx = rA | ((rF & CF) ? 0x100 : 0) | ((rF & HF) ? 0x200 : 0) | ((rF & NF) ? 0x400 : 0);
        uint16_t daa_val = daa_table[daa_idx];
        rA = (uint8_t)daa_val;
        F_WRITE = (uint8_t)(daa_val >> 8);
        NEXT(4);
    }

    /* JR Z, d */
    op_28: { int8_t d = (int8_t)FETCH(); if (rF & ZF) { rPC += d; rWZ = rPC; NEXT(12); } else { NEXT(7); } }

    /* ADD HL, HL */
    op_29: rWZ = HL_VAL + 1; ADD16(rH, rL, HL_VAL); NEXT(11);

    /* LD HL, (nn) */
    op_2A: { uint16_t addr = FETCH16(); uint16_t v = RD16(addr); SET_HL(v); rWZ = addr + 1; NEXT(16); }

    /* DEC HL */
    op_2B: { uint16_t hl = HL_VAL - 1; SET_HL(hl); NEXT(6); }

    /* INC L */
    op_2C: rL = INC8(rL); NEXT(4);

    /* DEC L */
    op_2D: rL = DEC8(rL); NEXT(4);

    /* LD L, n */
    op_2E: rL = FETCH(); NEXT(7);

    /* CPL */
    op_2F: rA = ~rA; F_WRITE = (rF & (SF|ZF|PF|CF)) | (rA & (F3|F5)) | NF | HF; NEXT(4);

    /* JR NC, d */
    op_30: { int8_t d = (int8_t)FETCH(); if (!(rF & CF)) { rPC += d; rWZ = rPC; NEXT(12); } else { NEXT(7); } }

    /* LD SP, nn */
    op_31: rSP = FETCH16(); NEXT(10);

    /* LD (nn), A */
    op_32: { uint16_t addr = FETCH16(); WR(addr, rA); SET_WZL((addr + 1) & 0xFF); SET_WZH(rA); NEXT(13); }

    /* INC SP */
    op_33: rSP++; NEXT(6);

    /* INC (HL) */
    op_34: { uint8_t v = RD(HL_VAL); WR(HL_VAL, INC8(v)); NEXT(11); }

    /* DEC (HL) */
    op_35: { uint8_t v = RD(HL_VAL); WR(HL_VAL, DEC8(v)); NEXT(11); }

    /* LD (HL), n */
    op_36: WR(HL_VAL, FETCH()); NEXT(10);

    /* SCF */
    op_37: F_WRITE = (rF & (SF|ZF|PF)) | (((rQ ^ rF) | rA) & (F3|F5)) | CF; NEXT(4);

    /* JR C, d */
    op_38: { int8_t d = (int8_t)FETCH(); if (rF & CF) { rPC += d; rWZ = rPC; NEXT(12); } else { NEXT(7); } }

    /* ADD HL, SP */
    op_39: rWZ = HL_VAL + 1; ADD16(rH, rL, rSP); NEXT(11);

    /* LD A, (nn) */
    op_3A: { uint16_t addr = FETCH16(); rA = RD(addr); rWZ = addr + 1; NEXT(13); }

    /* DEC SP */
    op_3B: rSP--; NEXT(6);

    /* INC A */
    op_3C: rA = INC8(rA); NEXT(4);

    /* DEC A */
    op_3D: rA = DEC8(rA); NEXT(4);

    /* LD A, n */
    op_3E: rA = FETCH(); NEXT(7);

    /* CCF */
    op_3F: F_WRITE = (rF & (SF|ZF|PF)) | (((rQ ^ rF) | rA) & (F3|F5)) | ((rF & CF) ? HF : 0) | ((rF & CF) ^ CF); NEXT(4);

    /* LD r, r' */
    op_40: NEXT(4);           /* LD B, B */
    op_41: rB = rC; NEXT(4);
    op_42: rB = rD; NEXT(4);
    op_43: rB = rE; NEXT(4);
    op_44: rB = rH; NEXT(4);
    op_45: rB = rL; NEXT(4);
    op_46: rB = RD(HL_VAL); NEXT(7);
    op_47: rB = rA; NEXT(4);
    op_48: rC = rB; NEXT(4);
    op_49: NEXT(4);           /* LD C, C */
    op_4A: rC = rD; NEXT(4);
    op_4B: rC = rE; NEXT(4);
    op_4C: rC = rH; NEXT(4);
    op_4D: rC = rL; NEXT(4);
    op_4E: rC = RD(HL_VAL); NEXT(7);
    op_4F: rC = rA; NEXT(4);
    op_50: rD = rB; NEXT(4);
    op_51: rD = rC; NEXT(4);
    op_52: NEXT(4);           /* LD D, D */
    op_53: rD = rE; NEXT(4);
    op_54: rD = rH; NEXT(4);
    op_55: rD = rL; NEXT(4);
    op_56: rD = RD(HL_VAL); NEXT(7);
    op_57: rD = rA; NEXT(4);
    op_58: rE = rB; NEXT(4);
    op_59: rE = rC; NEXT(4);
    op_5A: rE = rD; NEXT(4);
    op_5B: NEXT(4);           /* LD E, E */
    op_5C: rE = rH; NEXT(4);
    op_5D: rE = rL; NEXT(4);
    op_5E: rE = RD(HL_VAL); NEXT(7);
    op_5F: rE = rA; NEXT(4);
    op_60: rH = rB; NEXT(4);
    op_61: rH = rC; NEXT(4);
    op_62: rH = rD; NEXT(4);
    op_63: rH = rE; NEXT(4);
    op_64: NEXT(4);           /* LD H, H */
    op_65: rH = rL; NEXT(4);
    op_66: rH = RD(HL_VAL); NEXT(7);
    op_67: rH = rA; NEXT(4);
    op_68: rL = rB; NEXT(4);
    op_69: rL = rC; NEXT(4);
    op_6A: rL = rD; NEXT(4);
    op_6B: rL = rE; NEXT(4);
    op_6C: rL = rH; NEXT(4);
    op_6D: NEXT(4);           /* LD L, L */
    op_6E: rL = RD(HL_VAL); NEXT(7);
    op_6F: rL = rA; NEXT(4);
    op_70: WR(HL_VAL, rB); NEXT(7);
    op_71: WR(HL_VAL, rC); NEXT(7);
    op_72: WR(HL_VAL, rD); NEXT(7);
    op_73: WR(HL_VAL, rE); NEXT(7);
    op_74: WR(HL_VAL, rH); NEXT(7);
    op_75: WR(HL_VAL, rL); NEXT(7);

    /* HALT */
    op_76: cpu->halted = true;
           if (cpu->halt_cb) { WRITEBACK(); cpu->halt_cb(cpu, cpu->halt_data); RELOAD(); }
           /* CPU control event - HALT entry. Nezávislý na halt_cb (= BC).
            * Event vidí PC = X+1 (post-fetch, adresa za HALT) - sémantika
            * eventu beze změny (viz test_eventlog). */
           if (cpu->cpu_ctrl_event_cb) {
               WRITEBACK();
               cpu->cpu_ctrl_event_cb(cpu, (uint8_t)Z80_CPU_CTRL_HALT_ENTER,
                                      cpu->pc, cpu->cpu_ctrl_event_data);
               RELOAD();
           }
           /* BUG1 fix: během HALT drží reálný Z80 PC = X = adresa instrukce
            * HALT (instrukce se re-fetchuje cyklicky kvůli DRAM refreshi,
            * PC se inkrementuje až před skokem do INT/NMI rutiny - viz KB
            * 12-cpu-control.md:27, 07-interrupts.md:147,151). FETCH() však
            * PC už inkrementoval na X+1, tak ho vrátíme na X. Inkrement
            * zpět na X+1 (návratová adresa za HALT) provede INT/NMI accept
            * v handle_interrupts_internal(). Až PO HALT_ENTER eventu výše,
            * aby event viděl X+1. Dekrementujeme lokální rPC - WRITEBACK na
            * done: ho propíše do cpu->pc. */
           rPC--;
           NEXT_SLOW(4);

    op_77: WR(HL_VAL, rA); NEXT(7);
    op_78: rA = rB; NEXT(4);
    op_79: rA = rC; NEXT(4);
    op_7A: rA = rD; NEXT(4);
    op_7B: rA = rE; NEXT(4);
    op_7C: rA = rH; NEXT(4);
    op_7D: rA = rL; NEXT(4);
    op_7E: rA = RD(HL_VAL); NEXT(7);
    op_7F: NEXT(4);           /* LD A, A */

    /* ADD A, r */
    op_80: rA = ADD8(rA, rB); NEXT(4);
    op_81: rA = ADD8(rA, rC); NEXT(4);
    op_82: rA = ADD8(rA, rD); NEXT(4);
    op_83: rA = ADD8(rA, rE); NEXT(4);
    op_84: rA = ADD8(rA, rH); NEXT(4);
    op_85: rA = ADD8(rA, rL); NEXT(4);
    op_86: rA = ADD8(rA, RD(HL_VAL)); NEXT(7);
    op_87: rA = ADD8(rA, rA); NEXT(4);

    /* ADC A, r */
    op_88: rA = ADC8(rA, rB); NEXT(4);
    op_89: rA = ADC8(rA, rC); NEXT(4);
    op_8A: rA = ADC8(rA, rD); NEXT(4);
    op_8B: rA = ADC8(rA, rE); NEXT(4);
    op_8C: rA = ADC8(rA, rH); NEXT(4);
    op_8D: rA = ADC8(rA, rL); NEXT(4);
    op_8E: rA = ADC8(rA, RD(HL_VAL)); NEXT(7);
    op_8F: rA = ADC8(rA, rA); NEXT(4);

    /* SUB r */
    op_90: rA = SUB8(rA, rB); NEXT(4);
    op_91: rA = SUB8(rA, rC); NEXT(4);
    op_92: rA = SUB8(rA, rD); NEXT(4);
    op_93: rA = SUB8(rA, rE); NEXT(4);
    op_94: rA = SUB8(rA, rH); NEXT(4);
    op_95: rA = SUB8(rA, rL); NEXT(4);
    op_96: rA = SUB8(rA, RD(HL_VAL)); NEXT(7);
    op_97: rA = SUB8(rA, rA); NEXT(4);

    /* SBC A, r */
    op_98: rA = SBC8(rA, rB); NEXT(4);
    op_99: rA = SBC8(rA, rC); NEXT(4);
    op_9A: rA = SBC8(rA, rD); NEXT(4);
    op_9B: rA = SBC8(rA, rE); NEXT(4);
    op_9C: rA = SBC8(rA, rH); NEXT(4);
    op_9D: rA = SBC8(rA, rL); NEXT(4);
    op_9E: rA = SBC8(rA, RD(HL_VAL)); NEXT(7);
    op_9F: rA = SBC8(rA, rA); NEXT(4);

    /* AND r */
    op_A0: AND8(rB); NEXT(4);
    op_A1: AND8(rC); NEXT(4);
    op_A2: AND8(rD); NEXT(4);
    op_A3: AND8(rE); NEXT(4);
    op_A4: AND8(rH); NEXT(4);
    op_A5: AND8(rL); NEXT(4);
    op_A6: AND8(RD(HL_VAL)); NEXT(7);
    op_A7: AND8(rA); NEXT(4);

    /* XOR r */
    op_A8: XOR8(rB); NEXT(4);
    op_A9: XOR8(rC); NEXT(4);
    op_AA: XOR8(rD); NEXT(4);
    op_AB: XOR8(rE); NEXT(4);
    op_AC: XOR8(rH); NEXT(4);
    op_AD: XOR8(rL); NEXT(4);
    op_AE: XOR8(RD(HL_VAL)); NEXT(7);
    op_AF: XOR8(rA); NEXT(4);

    /* OR r */
    op_B0: OR8(rB); NEXT(4);
    op_B1: OR8(rC); NEXT(4);
    op_B2: OR8(rD); NEXT(4);
    op_B3: OR8(rE); NEXT(4);
    op_B4: OR8(rH); NEXT(4);
    op_B5: OR8(rL); NEXT(4);
    op_B6: OR8(RD(HL_VAL)); NEXT(7);
    op_B7: OR8(rA); NEXT(4);

    /* CP r */
    op_B8: CP8(rB); NEXT(4);
    op_B9: CP8(rC); NEXT(4);
    op_BA: CP8(rD); NEXT(4);
    op_BB: CP8(rE); NEXT(4);
    op_BC: CP8(rH); NEXT(4);
    op_BD: CP8(rL); NEXT(4);
    op_BE: CP8(RD(HL_VAL)); NEXT(7);
    op_BF: CP8(rA); NEXT(4);

    /* RET NZ/Z/NC/C/PO/PE/P/M (fire ret_cb jen v taken-větvi) */
    op_C0: if (!(rF & ZF)) { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }
    op_C8: if (rF & ZF)    { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }
    op_D0: if (!(rF & CF)) { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }
    op_D8: if (rF & CF)    { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }
    op_E0: if (!(rF & PF)) { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }
    op_E8: if (rF & PF)    { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }
    op_F0: if (!(rF & SF)) { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }
    op_F8: if (rF & SF)    { FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(11); } else { NEXT(5); }

    /* RET */
    op_C9: FIRE_RET(); rPC = POP(); rWZ = rPC; NEXT(10);

    /* POP rr */
    op_C1: { uint16_t v = POP(); SET_BC(v); NEXT(10); }
    op_D1: { uint16_t v = POP(); SET_DE(v); NEXT(10); }
    op_E1: { uint16_t v = POP(); SET_HL(v); NEXT(10); }
    op_F1: { uint16_t v = POP(); SET_AF(v); NEXT(10); }

    /* PUSH rr */
    op_C5: PUSH(BC_VAL); NEXT(11);
    op_D5: PUSH(DE_VAL); NEXT(11);
    op_E5: PUSH(HL_VAL); NEXT(11);
    op_F5: PUSH(AF_VAL); NEXT(11);

    /* JP cc, nn */
    op_C2: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & ZF)) rPC = addr; NEXT(10); }
    op_CA: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & ZF)    rPC = addr; NEXT(10); }
    op_D2: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & CF)) rPC = addr; NEXT(10); }
    op_DA: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & CF)    rPC = addr; NEXT(10); }
    op_E2: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & PF)) rPC = addr; NEXT(10); }
    op_EA: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & PF)    rPC = addr; NEXT(10); }
    op_F2: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & SF)) rPC = addr; NEXT(10); }
    op_FA: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & SF)    rPC = addr; NEXT(10); }

    /* JP nn */
    op_C3: { uint16_t addr = FETCH16(); rWZ = addr; rPC = addr; NEXT(10); }

    /* CALL cc, nn (fire call_cb jen v taken-větvi) */
    op_C4: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & ZF)) { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }
    op_CC: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & ZF)    { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }
    op_D4: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & CF)) { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }
    op_DC: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & CF)    { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }
    op_E4: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & PF)) { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }
    op_EC: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & PF)    { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }
    op_F4: { uint16_t addr = FETCH16(); rWZ = addr; if (!(rF & SF)) { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }
    op_FC: { uint16_t addr = FETCH16(); rWZ = addr; if (rF & SF)    { FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); } else { NEXT(10); } }

    /* CALL nn */
    op_CD: { uint16_t addr = FETCH16(); rWZ = addr; FIRE_CALL(addr); PUSH(rPC); rPC = addr; NEXT(17); }

    /* Aritmetika s okamžitým operandem */
    op_C6: { uint8_t v = FETCH(); rA = ADD8(rA, v); NEXT(7); }
    op_CE: { uint8_t v = FETCH(); rA = ADC8(rA, v); NEXT(7); }
    op_D6: { uint8_t v = FETCH(); rA = SUB8(rA, v); NEXT(7); }
    op_DE: { uint8_t v = FETCH(); rA = SBC8(rA, v); NEXT(7); }
    op_E6: { uint8_t v = FETCH(); AND8(v); NEXT(7); }
    op_EE: { uint8_t v = FETCH(); XOR8(v); NEXT(7); }
    op_F6: { uint8_t v = FETCH(); OR8(v); NEXT(7); }
    op_FE: { uint8_t v = FETCH(); CP8(v); NEXT(7); }

    /*
     * RST nn
     *
     * Před PUSH PC fire cpu_ctrl_event_cb (= konzument vidí původní PC
     * předtím než se změní na vektor). PC předávaný do cb je rPC = adresa
     * za RST opcode (= shodné s tím co se push na stack). WRITEBACK/RELOAD
     * sjednocení lokální cache s cpu->pc na dobu callbacku.
     */
#define RST_HOOK(_evt) do { \
    if (cpu->cpu_ctrl_event_cb) { \
        WRITEBACK(); \
        cpu->cpu_ctrl_event_cb(cpu, (uint8_t)(_evt), cpu->pc, \
                               cpu->cpu_ctrl_event_data); \
        RELOAD(); \
    } \
} while (0)

    op_C7: RST_HOOK(Z80_CPU_CTRL_RST_00); PUSH(rPC); rPC = 0x00; rWZ = rPC; NEXT(11);
    op_CF: RST_HOOK(Z80_CPU_CTRL_RST_08); PUSH(rPC); rPC = 0x08; rWZ = rPC; NEXT(11);
    op_D7: RST_HOOK(Z80_CPU_CTRL_RST_10); PUSH(rPC); rPC = 0x10; rWZ = rPC; NEXT(11);
    op_DF: RST_HOOK(Z80_CPU_CTRL_RST_18); PUSH(rPC); rPC = 0x18; rWZ = rPC; NEXT(11);
    op_E7: RST_HOOK(Z80_CPU_CTRL_RST_20); PUSH(rPC); rPC = 0x20; rWZ = rPC; NEXT(11);
    op_EF: RST_HOOK(Z80_CPU_CTRL_RST_28); PUSH(rPC); rPC = 0x28; rWZ = rPC; NEXT(11);
    op_F7: RST_HOOK(Z80_CPU_CTRL_RST_30); PUSH(rPC); rPC = 0x30; rWZ = rPC; NEXT(11);
    op_FF: RST_HOOK(Z80_CPU_CTRL_RST_38); PUSH(rPC); rPC = 0x38; rWZ = rPC; NEXT(11);
#undef RST_HOOK

    /* OUT (n), A */
    op_D3: { uint8_t port = FETCH(); uint16_t pa = (uint16_t)((rA << 8) | port); IO_WR(pa, rA); SET_WZL((port+1)&0xFF); SET_WZH(rA); NEXT(11); }

    /* IN A, (n) */
    op_DB: { uint8_t port = FETCH(); uint16_t pa = (uint16_t)((rA << 8) | port); rWZ = pa + 1; rA = IO_RD(pa); NEXT(11); }

    /* EX DE, HL */
    op_EB: { uint8_t t; t = rD; rD = rH; rH = t; t = rE; rE = rL; rL = t; NEXT(4); }

    /* EX (SP), HL */
    op_E3: { uint16_t v = RD16(rSP); WR16(rSP, HL_VAL); SET_HL(v); rWZ = v; NEXT(19); }

    /* JP (HL) */
    op_E9: rPC = HL_VAL; NEXT(4);

    /* LD SP, HL */
    op_F9: rSP = HL_VAL; NEXT(6);

    /* DI */
    op_F3: cpu->iff1 = 0; cpu->iff2 = 0;
           if (cpu->di_cb || cpu->iff_change_cb) {
               WRITEBACK();
               if (cpu->di_cb) cpu->di_cb(cpu, cpu->di_data);
               FIRE_IFF_CHANGE(Z80_IFF_REASON_DI);
               RELOAD();
           }
           NEXT_SLOW(4);

    /* EI */
    op_FB: cpu->iff1 = 1; cpu->iff2 = 1; cpu->ei_delay = true;
           if (cpu->ei_cb || cpu->iff_change_cb) {
               WRITEBACK();
               if (cpu->ei_cb) cpu->ei_cb(cpu, cpu->ei_data);
               FIRE_IFF_CHANGE(Z80_IFF_REASON_EI);
               RELOAD();
           }
           NEXT_SLOW(4);

    /* EXX */
    op_D9: {
        uint8_t t;
        t = rB; rB = cpu->bc2.h; cpu->bc2.h = t;
        t = rC; rC = cpu->bc2.l; cpu->bc2.l = t;
        t = rD; rD = cpu->de2.h; cpu->de2.h = t;
        t = rE; rE = cpu->de2.l; cpu->de2.l = t;
        t = rH; rH = cpu->hl2.h; cpu->hl2.h = t;
        t = rL; rL = cpu->hl2.l; cpu->hl2.l = t;
        NEXT(4);
    }

    /* ========== CB prefix ========== */
    op_CB: {
        INC_R();
        uint8_t cbop = M1_FETCH();
        int reg = cbop & 0x07;
        int bit_n = (cbop >> 3) & 0x07;
        int group = (cbop >> 6) & 0x03;

        uint8_t val;
        switch (reg) {
            case 0: val = rB; break;
            case 1: val = rC; break;
            case 2: val = rD; break;
            case 3: val = rE; break;
            case 4: val = rH; break;
            case 5: val = rL; break;
            case 6: val = RD(HL_VAL); break;
            case 7: val = rA; break;
            default: val = 0; break;
        }

        uint8_t result = val;
        int cb_cycles = 8;

        switch (group) {
            case 0: /* Rotace/posuvy */
                switch (bit_n) {
                    case 0: result = RLC(val); break;
                    case 1: result = RRC(val); break;
                    case 2: result = RL(val); break;
                    case 3: result = RR(val); break;
                    case 4: result = SLA(val); break;
                    case 5: result = SRA(val); break;
                    case 6: result = SLL(val); break;
                    case 7: result = SRL(val); break;
                }
                if (reg == 6) cb_cycles = 15;
                break;
            case 1: /* BIT */
                if (reg == 6) {
                    BIT_MEM(bit_n, val, WZH_VAL);
                    NEXT(12);
                } else {
                    BIT_OP(bit_n, val);
                    NEXT(8);
                }
            case 2: /* RES */
                result = val & ~(1 << bit_n);
                if (reg == 6) cb_cycles = 15;
                break;
            case 3: /* SET */
                result = val | (1 << bit_n);
                if (reg == 6) cb_cycles = 15;
                break;
        }

        switch (reg) {
            case 0: rB = result; break;
            case 1: rC = result; break;
            case 2: rD = result; break;
            case 3: rE = result; break;
            case 4: rH = result; break;
            case 5: rL = result; break;
            case 6: WR(HL_VAL, result); break;
            case 7: rA = result; break;
        }
        NEXT(cb_cycles);
    }

    /* ========== ED prefix ========== */

    /**
     * @brief Příznaky opakující iterace LDIR/LDDR/CPIR/CPDR: YF = PC.13, XF = PC.11.
     *
     * Když se bloková instrukce opakuje, 5T cyklus, který vrací PC o 2 zpět,
     * přepíše YF (bit 5) a XF (bit 3) bity 13 a 11 adresy instrukce (prefixu
     * EDh). Ostatní bity zůstanou jako po LDI/CPI. Program to vidí, když se
     * instrukce zastaví mezi iteracemi (přijetí INT nebo NMI, krok
     * v debuggeru, instrukce přepíše svůj opkód). Změřeno na reálných
     * procesorech Z80 i na MZ-800 (báze cpu/z80/04-instructions/
     * 09-block-transfer.md).
     *
     * @pre rPC už je vrácený na adresu prefixu EDh (rPC -= 2) a rF obsahuje
     *      příznaky iterace spočtené jako u LDI/LDD/CPI/CPD.
     * @post Bity F3 a F5 v rF jsou kopií PC.11 a PC.13, ostatní beze změny.
     */
#define BLOCK_REPEAT_FLAGS() do { \
    F_WRITE = (uint8_t)((rF & ~(F3|F5)) | ((rPC >> 8) & (F3|F5))); \
} while(0)

    /**
     * @brief Příznaky opakující iterace INIR/INDR/OTIR/OTDR.
     *
     * Kromě YF = PC.13 a XF = PC.11 mění 5T cyklus opakování i PF a HF podle
     * CF, bitu 7 přeneseného bajtu a dolních bitů už dekrementovaného B
     * (Parity() = PF pro sudou paritu, jako parity_table):
     *
     *     CF = 1, data.7 = 1: PF ^= Parity((B - 1) & 7) ^ 1, HF = (B & 0Fh) == 00h
     *     CF = 1, data.7 = 0: PF ^= Parity((B + 1) & 7) ^ 1, HF = (B & 0Fh) == 0Fh
     *     CF = 0:             PF ^= Parity(B & 7) ^ 1, HF beze změny
     *
     * Změřeno na reálných procesorech Z80 i na MZ-800 (báze
     * cpu/z80/04-instructions/10-block-io.md).
     *
     * @param val Přenesený bajt (přečtený z portu u INxR, z paměti u OTxR).
     * @pre rPC už je vrácený na adresu prefixu EDh, rB je dekrementovaný
     *      a rF obsahuje příznaky spočtené jako u INI/IND/OUTI/OUTD.
     * @post rF obsahuje příznaky, které uvidí obsluha přerušení po této
     *       iteraci; SF, ZF, NF a CF beze změny.
     *
     * @note MEMPTR nastavuje opakující větev sama: rWZ = adresa instrukce + 1,
     *       stejně jako u LDxR/CPxR (změřeno na MZ-800 a na procesorech Zilog
     *       a NEC, báze cpu/z80/05-memptr-wz.md). Poslední iterace nechává
     *       MEMPTR jako INI/IND/OUTI/OUTD (BC +- 1).
     */
#define BLOCK_IO_REPEAT_FLAGS(val) do { \
    uint8_t _bf = (uint8_t)((rF & ~(F3|F5)) | ((rPC >> 8) & (F3|F5))); \
    uint8_t _bp; \
    if (_bf & CF) { \
        if ((val) & 0x80) { \
            _bp = parity_table[(uint8_t)(rB - 1) & 7]; \
            _bf = (uint8_t)((_bf & ~HF) | (((rB & 0x0F) == 0x00) ? HF : 0)); \
        } else { \
            _bp = parity_table[(uint8_t)(rB + 1) & 7]; \
            _bf = (uint8_t)((_bf & ~HF) | (((rB & 0x0F) == 0x0F) ? HF : 0)); \
        } \
    } else { \
        _bp = parity_table[rB & 7]; \
    } \
    F_WRITE = (uint8_t)(_bf ^ _bp ^ PF); \
} while(0)

    op_ED: {
        INC_R();
        uint8_t edop = M1_FETCH();

        switch (edop) {
            /* IN r, (C) */
            case 0x40: rWZ = BC_VAL + 1; rB = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[rB]; NEXT(12);
            case 0x48: rWZ = BC_VAL + 1; rC = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[rC]; NEXT(12);
            case 0x50: rWZ = BC_VAL + 1; rD = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[rD]; NEXT(12);
            case 0x58: rWZ = BC_VAL + 1; rE = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[rE]; NEXT(12);
            case 0x60: rWZ = BC_VAL + 1; rH = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[rH]; NEXT(12);
            case 0x68: rWZ = BC_VAL + 1; rL = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[rL]; NEXT(12);
            case 0x70: { rWZ = BC_VAL + 1; uint8_t t = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[t]; NEXT(12); }
            case 0x78: rWZ = BC_VAL + 1; rA = IO_RD(BC_VAL); F_WRITE = (rF & CF) | sz53p_table[rA]; NEXT(12);

            /* OUT (C), r */
            case 0x41: IO_WR(BC_VAL, rB); rWZ = BC_VAL + 1; NEXT(12);
            case 0x49: IO_WR(BC_VAL, rC); rWZ = BC_VAL + 1; NEXT(12);
            case 0x51: IO_WR(BC_VAL, rD); rWZ = BC_VAL + 1; NEXT(12);
            case 0x59: IO_WR(BC_VAL, rE); rWZ = BC_VAL + 1; NEXT(12);
            case 0x61: IO_WR(BC_VAL, rH); rWZ = BC_VAL + 1; NEXT(12);
            case 0x69: IO_WR(BC_VAL, rL); rWZ = BC_VAL + 1; NEXT(12);
            case 0x71: IO_WR(BC_VAL, 0);  rWZ = BC_VAL + 1; NEXT(12);
            case 0x79: IO_WR(BC_VAL, rA); rWZ = BC_VAL + 1; NEXT(12);

            /* SBC HL, rr */
            case 0x42: rWZ = HL_VAL + 1; SBC16(BC_VAL); NEXT(15);
            case 0x52: rWZ = HL_VAL + 1; SBC16(DE_VAL); NEXT(15);
            case 0x62: rWZ = HL_VAL + 1; SBC16(HL_VAL); NEXT(15);
            case 0x72: rWZ = HL_VAL + 1; SBC16(rSP); NEXT(15);

            /* ADC HL, rr */
            case 0x4A: rWZ = HL_VAL + 1; ADC16(BC_VAL); NEXT(15);
            case 0x5A: rWZ = HL_VAL + 1; ADC16(DE_VAL); NEXT(15);
            case 0x6A: rWZ = HL_VAL + 1; ADC16(HL_VAL); NEXT(15);
            case 0x7A: rWZ = HL_VAL + 1; ADC16(rSP); NEXT(15);

            /* LD (nn), rr */
            case 0x43: { uint16_t addr = FETCH16(); WR16(addr, BC_VAL); rWZ = addr + 1; NEXT(20); }
            case 0x53: { uint16_t addr = FETCH16(); WR16(addr, DE_VAL); rWZ = addr + 1; NEXT(20); }
            case 0x63: { uint16_t addr = FETCH16(); WR16(addr, HL_VAL); rWZ = addr + 1; NEXT(20); }
            case 0x73: { uint16_t addr = FETCH16(); WR16(addr, rSP); rWZ = addr + 1; NEXT(20); }

            /* LD rr, (nn) */
            case 0x4B: { uint16_t addr = FETCH16(); uint16_t v = RD16(addr); SET_BC(v); rWZ = addr + 1; NEXT(20); }
            case 0x5B: { uint16_t addr = FETCH16(); uint16_t v = RD16(addr); SET_DE(v); rWZ = addr + 1; NEXT(20); }
            case 0x6B: { uint16_t addr = FETCH16(); uint16_t v = RD16(addr); SET_HL(v); rWZ = addr + 1; NEXT(20); }
            case 0x7B: { uint16_t addr = FETCH16(); rSP = RD16(addr); rWZ = addr + 1; NEXT(20); }

            /* NEG */
            case 0x44: case 0x4C: case 0x54: case 0x5C:
            case 0x64: case 0x6C: case 0x74: case 0x7C: {
                uint8_t t = rA; rA = 0; rA = SUB8(rA, t); NEXT(8);
            }

            /* RETN */
            case 0x45: case 0x55: case 0x65: case 0x75:
                cpu->iff1 = cpu->iff2; rPC = POP(); rWZ = rPC;
                if (cpu->iff_change_cb) {
                    WRITEBACK();
                    FIRE_IFF_CHANGE(Z80_IFF_REASON_RETN);
                    RELOAD();
                }
                NEXT_SLOW(14);

            /* RETI */
            case 0x4D: case 0x5D: case 0x6D: case 0x7D:
                cpu->iff1 = cpu->iff2; rPC = POP(); rWZ = rPC;
                if (cpu->reti_cb || cpu->iff_change_cb) {
                    WRITEBACK();
                    if (cpu->reti_cb) cpu->reti_cb(cpu, cpu->reti_data);
                    FIRE_IFF_CHANGE(Z80_IFF_REASON_RETI);
                    RELOAD();
                }
                NEXT_SLOW(14);

            /* IM */
            case 0x46: case 0x66: cpu->im = 0;
                if (cpu->im_cb) { WRITEBACK(); cpu->im_cb(cpu, 0, cpu->im_data); RELOAD(); }
                NEXT(8);
            case 0x56: case 0x76: cpu->im = 1;
                if (cpu->im_cb) { WRITEBACK(); cpu->im_cb(cpu, 1, cpu->im_data); RELOAD(); }
                NEXT(8);
            case 0x5E: case 0x7E: cpu->im = 2;
                if (cpu->im_cb) { WRITEBACK(); cpu->im_cb(cpu, 2, cpu->im_data); RELOAD(); }
                NEXT(8);
            case 0x4E: case 0x6E: cpu->im = 0;
                if (cpu->im_cb) { WRITEBACK(); cpu->im_cb(cpu, 0, cpu->im_data); RELOAD(); }
                NEXT(8);

            /* LD I, A / LD R, A */
            case 0x47: cpu->i = rA; NEXT(9);
            case 0x4F: rR = rA; NEXT(9);

            /* LD A, I */
            case 0x57:
                rA = cpu->i;
                F_WRITE = (rF & CF) | sz53_table[rA] | (cpu->iff2 ? PF : 0);
                cpu->ld_a_ir = true;
                NEXT_SLOW(9);

            /* LD A, R */
            case 0x5F:
                rA = rR;
                F_WRITE = (rF & CF) | sz53_table[rA] | (cpu->iff2 ? PF : 0);
                cpu->ld_a_ir = true;
                NEXT_SLOW(9);

            /* RRD */
            case 0x67: {
                uint8_t val = RD(HL_VAL);
                uint8_t nv = (rA << 4) | (val >> 4);
                rA = (rA & 0xF0) | (val & 0x0F);
                WR(HL_VAL, nv);
                F_WRITE = (rF & CF) | sz53p_table[rA];
                rWZ = HL_VAL + 1;
                NEXT(18);
            }

            /* RLD */
            case 0x6F: {
                uint8_t val = RD(HL_VAL);
                uint8_t nv = (val << 4) | (rA & 0x0F);
                rA = (rA & 0xF0) | (val >> 4);
                WR(HL_VAL, nv);
                F_WRITE = (rF & CF) | sz53p_table[rA];
                rWZ = HL_VAL + 1;
                NEXT(18);
            }

            /* LDI */
            case 0xA0: {
                uint8_t val = RD(HL_VAL);
                uint16_t de = DE_VAL; WR(de, val);
                de++; SET_DE(de);
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t n = val + rA;
                F_WRITE = (rF & (SF|ZF|CF)) | (bc != 0 ? PF : 0) | (n & F3) | ((n & 0x02) ? F5 : 0);
                NEXT(16);
            }

            /* CPI */
            case 0xA1: {
                uint8_t val = RD(HL_VAL);
                uint8_t res = rA - val;
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t nhf = (rA ^ val ^ res) & HF;
                uint8_t n = res - (nhf ? 1 : 0);
                F_WRITE = (rF & CF) | NF | (bc != 0 ? PF : 0) | sz53_table[res & 0xFF] | nhf;
                F_WRITE = (rF & ~(F3|F5)) | (n & F3) | ((n & 0x02) ? F5 : 0);
                rWZ++;
                NEXT(16);
            }

            /* INI */
            case 0xA2: {
                rWZ = BC_VAL + 1;
                uint8_t val = IO_RD(BC_VAL);
                WR(HL_VAL, val);
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                rB--;
                uint16_t k = (uint16_t)val + (uint16_t)((rC + 1) & 0xFF);
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                NEXT(16);
            }

            /* OUTI */
            case 0xA3: {
                uint8_t val = RD(HL_VAL);
                rB--;
                IO_WR(BC_VAL, val);
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                rWZ = BC_VAL + 1;
                uint16_t k = (uint16_t)val + (uint16_t)rL;
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                NEXT(16);
            }

            /* LDD */
            case 0xA8: {
                uint8_t val = RD(HL_VAL);
                uint16_t de = DE_VAL; WR(de, val);
                de--; SET_DE(de);
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t n = val + rA;
                F_WRITE = (rF & (SF|ZF|CF)) | (bc != 0 ? PF : 0) | (n & F3) | ((n & 0x02) ? F5 : 0);
                NEXT(16);
            }

            /* CPD */
            case 0xA9: {
                uint8_t val = RD(HL_VAL);
                uint8_t res = rA - val;
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t nhf = (rA ^ val ^ res) & HF;
                uint8_t n = res - (nhf ? 1 : 0);
                F_WRITE = (rF & CF) | NF | (bc != 0 ? PF : 0) | sz53_table[res & 0xFF] | nhf;
                F_WRITE = (rF & ~(F3|F5)) | (n & F3) | ((n & 0x02) ? F5 : 0);
                rWZ--;
                NEXT(16);
            }

            /* IND */
            case 0xAA: {
                rWZ = BC_VAL - 1;
                uint8_t val = IO_RD(BC_VAL);
                WR(HL_VAL, val);
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                rB--;
                uint16_t k = (uint16_t)val + (uint16_t)((rC - 1) & 0xFF);
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                NEXT(16);
            }

            /* OUTD */
            case 0xAB: {
                uint8_t val = RD(HL_VAL);
                rB--;
                IO_WR(BC_VAL, val);
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                rWZ = BC_VAL - 1;
                uint16_t k = (uint16_t)val + (uint16_t)rL;
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                NEXT(16);
            }

            /* LDIR */
            case 0xB0: {
                uint8_t val = RD(HL_VAL);
                uint16_t de = DE_VAL; WR(de, val);
                de++; SET_DE(de);
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t n = val + rA;
                F_WRITE = (rF & (SF|ZF|CF)) | (bc != 0 ? PF : 0) | (n & F3) | ((n & 0x02) ? F5 : 0);
                if (bc != 0) { rPC -= 2; rWZ = rPC + 1; BLOCK_REPEAT_FLAGS(); NEXT(21); }
                NEXT(16);
            }

            /* CPIR */
            case 0xB1: {
                uint8_t val = RD(HL_VAL);
                uint8_t res = rA - val;
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t nhf = (rA ^ val ^ res) & HF;
                uint8_t n = res - (nhf ? 1 : 0);
                F_WRITE = (rF & CF) | NF | (bc != 0 ? PF : 0) | sz53_table[res & 0xFF] | nhf;
                F_WRITE = (rF & ~(F3|F5)) | (n & F3) | ((n & 0x02) ? F5 : 0);
                if (bc != 0 && !(rF & ZF)) { rPC -= 2; rWZ = rPC + 1; BLOCK_REPEAT_FLAGS(); NEXT(21); }
                rWZ++;
                NEXT(16);
            }

            /* INIR */
            case 0xB2: {
                rWZ = BC_VAL + 1;
                uint8_t val = IO_RD(BC_VAL);
                WR(HL_VAL, val);
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                rB--;
                uint16_t k = (uint16_t)val + (uint16_t)((rC + 1) & 0xFF);
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                if (rB != 0) { rPC -= 2; rWZ = rPC + 1; BLOCK_IO_REPEAT_FLAGS(val); NEXT(21); }
                NEXT(16);
            }

            /* OTIR */
            case 0xB3: {
                uint8_t val = RD(HL_VAL);
                rB--;
                IO_WR(BC_VAL, val);
                uint16_t hl = HL_VAL + 1; SET_HL(hl);
                rWZ = BC_VAL + 1;
                uint16_t k = (uint16_t)val + (uint16_t)rL;
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                if (rB != 0) { rPC -= 2; rWZ = rPC + 1; BLOCK_IO_REPEAT_FLAGS(val); NEXT(21); }
                NEXT(16);
            }

            /* LDDR */
            case 0xB8: {
                uint8_t val = RD(HL_VAL);
                uint16_t de = DE_VAL; WR(de, val);
                de--; SET_DE(de);
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t n = val + rA;
                F_WRITE = (rF & (SF|ZF|CF)) | (bc != 0 ? PF : 0) | (n & F3) | ((n & 0x02) ? F5 : 0);
                if (bc != 0) { rPC -= 2; rWZ = rPC + 1; BLOCK_REPEAT_FLAGS(); NEXT(21); }
                NEXT(16);
            }

            /* CPDR */
            case 0xB9: {
                uint8_t val = RD(HL_VAL);
                uint8_t res = rA - val;
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                uint16_t bc = BC_VAL - 1; SET_BC(bc);
                uint8_t nhf = (rA ^ val ^ res) & HF;
                uint8_t n = res - (nhf ? 1 : 0);
                F_WRITE = (rF & CF) | NF | (bc != 0 ? PF : 0) | sz53_table[res & 0xFF] | nhf;
                F_WRITE = (rF & ~(F3|F5)) | (n & F3) | ((n & 0x02) ? F5 : 0);
                if (bc != 0 && !(rF & ZF)) { rPC -= 2; rWZ = rPC + 1; BLOCK_REPEAT_FLAGS(); NEXT(21); }
                rWZ--;
                NEXT(16);
            }

            /* INDR */
            case 0xBA: {
                rWZ = BC_VAL - 1;
                uint8_t val = IO_RD(BC_VAL);
                WR(HL_VAL, val);
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                rB--;
                uint16_t k = (uint16_t)val + (uint16_t)((rC - 1) & 0xFF);
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                if (rB != 0) { rPC -= 2; rWZ = rPC + 1; BLOCK_IO_REPEAT_FLAGS(val); NEXT(21); }
                NEXT(16);
            }

            /* OTDR */
            case 0xBB: {
                uint8_t val = RD(HL_VAL);
                rB--;
                IO_WR(BC_VAL, val);
                uint16_t hl = HL_VAL - 1; SET_HL(hl);
                rWZ = BC_VAL - 1;
                uint16_t k = (uint16_t)val + (uint16_t)rL;
                F_WRITE = sz53_table[rB]
                   | ((val & 0x80) ? NF : 0)
                   | (k > 255 ? (HF|CF) : 0)
                   | (parity_table[(uint8_t)((k & 7) ^ rB)] ? PF : 0);
                if (rB != 0) { rPC -= 2; rWZ = rPC + 1; BLOCK_IO_REPEAT_FLAGS(val); NEXT(21); }
                NEXT(16);
            }

            /* Nedokumentované/neplatné ED instrukce - NOP (8T) */
            default: NEXT(8);
        }
    }

    /* ========== DD/FD prefix makro ========== */
    /* Generujeme DD a FD handlery pomocí makra - IX vs IY */

#define DDFD_PREFIX(IDX_W, IDX_H, IDX_L) do { \
    INC_R(); \
    uint8_t ddop = M1_FETCH(); \
    switch (ddop) { \
        /* ADD IX/IY, rr */ \
        case 0x09: rWZ = (IDX_W) + 1; { \
            uint32_t _r32 = (IDX_W) + BC_VAL; \
            uint8_t _rh = (uint8_t)(_r32 >> 8); \
            F_WRITE = (rF & (SF|ZF|PF)) | (_rh & (F3|F5)) | ((_r32 >> 16) & CF) \
               | ((((IDX_W) ^ BC_VAL ^ _r32) >> 8) & HF); \
            (IDX_W) = (uint16_t)_r32; } NEXT(15); \
        case 0x19: rWZ = (IDX_W) + 1; { \
            uint32_t _r32 = (IDX_W) + DE_VAL; \
            uint8_t _rh = (uint8_t)(_r32 >> 8); \
            F_WRITE = (rF & (SF|ZF|PF)) | (_rh & (F3|F5)) | ((_r32 >> 16) & CF) \
               | ((((IDX_W) ^ DE_VAL ^ _r32) >> 8) & HF); \
            (IDX_W) = (uint16_t)_r32; } NEXT(15); \
        case 0x29: rWZ = (IDX_W) + 1; { \
            uint32_t _r32 = (IDX_W) + (IDX_W); \
            uint8_t _rh = (uint8_t)(_r32 >> 8); \
            F_WRITE = (rF & (SF|ZF|PF)) | (_rh & (F3|F5)) | ((_r32 >> 16) & CF) \
               | ((((IDX_W) ^ (IDX_W) ^ _r32) >> 8) & HF); \
            (IDX_W) = (uint16_t)_r32; } NEXT(15); \
        case 0x39: rWZ = (IDX_W) + 1; { \
            uint32_t _r32 = (IDX_W) + rSP; \
            uint8_t _rh = (uint8_t)(_r32 >> 8); \
            F_WRITE = (rF & (SF|ZF|PF)) | (_rh & (F3|F5)) | ((_r32 >> 16) & CF) \
               | ((((IDX_W) ^ rSP ^ _r32) >> 8) & HF); \
            (IDX_W) = (uint16_t)_r32; } NEXT(15); \
        /* LD IX/IY, nn */ \
        case 0x21: { uint8_t _lo = FETCH(); uint8_t _hi = FETCH(); (IDX_W) = (uint16_t)(_lo | (_hi << 8)); NEXT(14); } \
        /* LD (nn), IX/IY */ \
        case 0x22: { uint16_t _a = FETCH16(); WR16(_a, (IDX_W)); rWZ = _a + 1; NEXT(20); } \
        /* INC IX/IY */ \
        case 0x23: (IDX_W)++; NEXT(10); \
        /* INC IXH/IYH */ \
        case 0x24: (IDX_H) = INC8((IDX_H)); NEXT(8); \
        /* DEC IXH/IYH */ \
        case 0x25: (IDX_H) = DEC8((IDX_H)); NEXT(8); \
        /* LD IXH/IYH, n */ \
        case 0x26: (IDX_H) = FETCH(); NEXT(11); \
        /* LD IX/IY, (nn) */ \
        case 0x2A: { uint16_t _a = FETCH16(); (IDX_W) = RD16(_a); rWZ = _a + 1; NEXT(20); } \
        /* DEC IX/IY */ \
        case 0x2B: (IDX_W)--; NEXT(10); \
        /* INC IXL/IYL */ \
        case 0x2C: (IDX_L) = INC8((IDX_L)); NEXT(8); \
        /* DEC IXL/IYL */ \
        case 0x2D: (IDX_L) = DEC8((IDX_L)); NEXT(8); \
        /* LD IXL/IYL, n */ \
        case 0x2E: (IDX_L) = FETCH(); NEXT(11); \
        /* INC (IX/IY+d) */ \
        case 0x34: { int8_t _d = (int8_t)FETCH(); uint16_t _a = (IDX_W) + _d; rWZ = _a; uint8_t _v = RD(_a); WR(_a, INC8(_v)); NEXT(23); } \
        /* DEC (IX/IY+d) */ \
        case 0x35: { int8_t _d = (int8_t)FETCH(); uint16_t _a = (IDX_W) + _d; rWZ = _a; uint8_t _v = RD(_a); WR(_a, DEC8(_v)); NEXT(23); } \
        /* LD (IX/IY+d), n */ \
        case 0x36: { int8_t _d = (int8_t)FETCH(); uint8_t _n = FETCH(); rWZ = (IDX_W) + _d; WR(rWZ, _n); NEXT(19); } \
        /* LD r, IXH/IYH */ \
        case 0x44: rB = (IDX_H); NEXT(8); \
        case 0x4C: rC = (IDX_H); NEXT(8); \
        case 0x54: rD = (IDX_H); NEXT(8); \
        case 0x5C: rE = (IDX_H); NEXT(8); \
        case 0x7C: rA = (IDX_H); NEXT(8); \
        /* LD r, IXL/IYL */ \
        case 0x45: rB = (IDX_L); NEXT(8); \
        case 0x4D: rC = (IDX_L); NEXT(8); \
        case 0x55: rD = (IDX_L); NEXT(8); \
        case 0x5D: rE = (IDX_L); NEXT(8); \
        case 0x7D: rA = (IDX_L); NEXT(8); \
        /* LD IXH/IYH, r */ \
        case 0x60: (IDX_H) = rB; NEXT(8); \
        case 0x61: (IDX_H) = rC; NEXT(8); \
        case 0x62: (IDX_H) = rD; NEXT(8); \
        case 0x63: (IDX_H) = rE; NEXT(8); \
        case 0x64: NEXT(8); /* LD IXH, IXH */ \
        case 0x65: (IDX_H) = (IDX_L); NEXT(8); \
        case 0x67: (IDX_H) = rA; NEXT(8); \
        /* LD IXL/IYL, r */ \
        case 0x68: (IDX_L) = rB; NEXT(8); \
        case 0x69: (IDX_L) = rC; NEXT(8); \
        case 0x6A: (IDX_L) = rD; NEXT(8); \
        case 0x6B: (IDX_L) = rE; NEXT(8); \
        case 0x6C: (IDX_L) = (IDX_H); NEXT(8); \
        case 0x6D: NEXT(8); /* LD IXL, IXL */ \
        case 0x6F: (IDX_L) = rA; NEXT(8); \
        /* LD r, (IX/IY+d) */ \
        case 0x46: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rB=RD(rWZ); NEXT(19); } \
        case 0x4E: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rC=RD(rWZ); NEXT(19); } \
        case 0x56: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rD=RD(rWZ); NEXT(19); } \
        case 0x5E: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rE=RD(rWZ); NEXT(19); } \
        case 0x66: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rH=RD(rWZ); NEXT(19); } \
        case 0x6E: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rL=RD(rWZ); NEXT(19); } \
        case 0x7E: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rA=RD(rWZ); NEXT(19); } \
        /* LD (IX/IY+d), r */ \
        case 0x70: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; WR(rWZ, rB); NEXT(19); } \
        case 0x71: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; WR(rWZ, rC); NEXT(19); } \
        case 0x72: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; WR(rWZ, rD); NEXT(19); } \
        case 0x73: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; WR(rWZ, rE); NEXT(19); } \
        case 0x74: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; WR(rWZ, rH); NEXT(19); } \
        case 0x75: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; WR(rWZ, rL); NEXT(19); } \
        case 0x77: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; WR(rWZ, rA); NEXT(19); } \
        /* Aritmetika s IXH/IYH, IXL/IYL */ \
        case 0x84: rA = ADD8(rA, (IDX_H)); NEXT(8); \
        case 0x85: rA = ADD8(rA, (IDX_L)); NEXT(8); \
        case 0x8C: rA = ADC8(rA, (IDX_H)); NEXT(8); \
        case 0x8D: rA = ADC8(rA, (IDX_L)); NEXT(8); \
        case 0x94: rA = SUB8(rA, (IDX_H)); NEXT(8); \
        case 0x95: rA = SUB8(rA, (IDX_L)); NEXT(8); \
        case 0x9C: rA = SBC8(rA, (IDX_H)); NEXT(8); \
        case 0x9D: rA = SBC8(rA, (IDX_L)); NEXT(8); \
        case 0xA4: AND8((IDX_H)); NEXT(8); \
        case 0xA5: AND8((IDX_L)); NEXT(8); \
        case 0xAC: XOR8((IDX_H)); NEXT(8); \
        case 0xAD: XOR8((IDX_L)); NEXT(8); \
        case 0xB4: OR8((IDX_H)); NEXT(8); \
        case 0xB5: OR8((IDX_L)); NEXT(8); \
        case 0xBC: CP8((IDX_H)); NEXT(8); \
        case 0xBD: CP8((IDX_L)); NEXT(8); \
        /* Aritmetika s (IX/IY+d) */ \
        case 0x86: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rA=ADD8(rA,RD(rWZ)); NEXT(19); } \
        case 0x8E: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rA=ADC8(rA,RD(rWZ)); NEXT(19); } \
        case 0x96: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rA=SUB8(rA,RD(rWZ)); NEXT(19); } \
        case 0x9E: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; rA=SBC8(rA,RD(rWZ)); NEXT(19); } \
        case 0xA6: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; AND8(RD(rWZ)); NEXT(19); } \
        case 0xAE: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; XOR8(RD(rWZ)); NEXT(19); } \
        case 0xB6: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; OR8(RD(rWZ)); NEXT(19); } \
        case 0xBE: { int8_t _d=(int8_t)FETCH(); rWZ=(IDX_W)+_d; CP8(RD(rWZ)); NEXT(19); } \
        /* DD CB / FD CB prefix */ \
        case 0xCB: { \
            int8_t _d = (int8_t)FETCH(); \
            uint8_t _cbop = FETCH(); \
            uint16_t _addr = (IDX_W) + _d; \
            rWZ = _addr; \
            uint8_t _val = RD(_addr); \
            int _reg = _cbop & 0x07; \
            int _bit_n = (_cbop >> 3) & 0x07; \
            int _group = (_cbop >> 6) & 0x03; \
            uint8_t _result = _val; \
            switch (_group) { \
                case 0: \
                    switch (_bit_n) { \
                        case 0: _result = RLC(_val); break; \
                        case 1: _result = RRC(_val); break; \
                        case 2: _result = RL(_val); break; \
                        case 3: _result = RR(_val); break; \
                        case 4: _result = SLA(_val); break; \
                        case 5: _result = SRA(_val); break; \
                        case 6: _result = SLL(_val); break; \
                        case 7: _result = SRL(_val); break; \
                    } \
                    break; \
                case 1: \
                    BIT_MEM(_bit_n, _val, (uint8_t)(_addr >> 8)); \
                    NEXT(20); \
                case 2: _result = _val & ~(1 << _bit_n); break; \
                case 3: _result = _val | (1 << _bit_n); break; \
            } \
            WR(_addr, _result); \
            if (_reg != 6) { \
                switch (_reg) { \
                    case 0: rB = _result; break; \
                    case 1: rC = _result; break; \
                    case 2: rD = _result; break; \
                    case 3: rE = _result; break; \
                    case 4: rH = _result; break; \
                    case 5: rL = _result; break; \
                    case 7: rA = _result; break; \
                } \
            } \
            NEXT(23); \
        } \
        /* POP IX/IY */ \
        case 0xE1: (IDX_W) = POP(); NEXT(14); \
        /* EX (SP), IX/IY */ \
        case 0xE3: { uint16_t _t = RD16(rSP); WR16(rSP, (IDX_W)); (IDX_W) = _t; rWZ = _t; NEXT(23); } \
        /* PUSH IX/IY */ \
        case 0xE5: PUSH((IDX_W)); NEXT(15); \
        /* JP (IX/IY) */ \
        case 0xE9: rPC = (IDX_W); NEXT(8); \
        /* LD SP, IX/IY */ \
        case 0xF9: rSP = (IDX_W); NEXT(10); \
        /* Bajt za prefixem není instrukce s IX/IY: prefix se ignoruje */ \
        /* a bajt se provede v témže kroku, viz DDFD_IGNORED. */ \
        default: DDFD_IGNORED(ddop); \
    } \
} while(0)

    /**
     * @brief Provede opkód za osamoceným prefixem DD/FD v témže kroku.
     *
     * Prefix, za kterým nenásleduje instrukce s IX/IY, se ignoruje: CPU
     * provede následující bajt jako běžný opkód (DD/FD jako nový prefix,
     * platí poslední; ED a CB prefix DD/FD ruší), báze
     * cpu/z80/03-instruction-encoding.md "Kombinace prefixů". Bajt už je
     * načtený M1 cyklem v DDFD_PREFIX (R za něj přičtený), takže se na jeho
     * obsluhu skočí přímo, bez druhého načtení a bez druhého INC_R
     * (DD 00 = R + 2, báze cpu/z80/09-r-register.md). Prefix a instrukce
     * tvoří jeden krok CPU, přerušení se mezi nimi nepřijme (báze
     * cpu/z80/07-interrupts.md "Po DD a FD prefixu").
     *
     * Řetěz prefixů má v jednom kroku strop DDFD_CHAIN_MAX, aby paměť plná
     * DDh/FDh nezacyklila krok navždy. Po stropu se krok rozdělí: bajt se
     * vrátí (rPC--), R za něj se odečte (DISPATCH ho přičte znovu) a krok
     * skončí za prefixem; ten jeden bajt se tedy načte dvakrát.
     *
     * Registr Q se za prefixem nuluje (prefix příznaky nezapisuje), takže
     * SCF/CCF za osamoceným prefixem vidí Q = 0 bez ohledu na instrukci před
     * prefixem. Chování je převzaté z dřívější implementace; na reálném
     * procesoru neověřeno [neověřeno].
     *
     * @param op Bajt načtený za prefixem.
     * @pre Volá se jen z větve default makra DDFD_PREFIX; ddfd_chain počítá
     *      prefixy řetězu od vstupu op_DD/op_FD z dispatch tabulky.
     * @post Řízení pokračuje obsluhou opkódu op (nebo dalším prefixem
     *       op_DD_chain/op_FD_chain); prefix přičetl 4 T do cycles_executed.
     */
#if USE_COMPUTED_GOTO
#define DDFD_IGNORED(op) do { \
    if ((op) == 0xDD || (op) == 0xFD) { \
        if (++ddfd_chain >= DDFD_CHAIN_MAX) { \
            ddfd_chain = 0; \
            rPC--; \
            rR = (rR & 0x80) | ((rR - 1) & 0x7F); \
            NEXT(4); \
        } \
        cycles_executed += 4; \
        rQ = 0; \
        if ((op) == 0xDD) goto op_DD_chain; \
        goto op_FD_chain; \
    } \
    cycles_executed += 4; \
    rQ = 0; \
    goto *base_dispatch[(op)]; \
} while(0)
#else
    /* Bez computed goto nejde skočit na obsluhu opkódu: krok se rozdělí
     * jako po stropu řetězu (bajt se načte dvakrát, R se nezdvojí). */
#define DDFD_IGNORED(op) do { \
    (void)(op); \
    rPC--; \
    rR = (rR & 0x80) | ((rR - 1) & 0x7F); \
    NEXT(4); \
} while(0)
#endif

    /* DD prefix (IX): vstup z dispatch tabulky začíná nový řetěz prefixů */
    op_DD: ddfd_chain = 0;
    op_DD_chain: DDFD_PREFIX(cpu->ix.w, cpu->ix.h, cpu->ix.l);

    /* FD prefix (IY) */
    op_FD: ddfd_chain = 0;
    op_FD_chain: DDFD_PREFIX(cpu->iy.w, cpu->iy.h, cpu->iy.l);

#if !USE_COMPUTED_GOTO
dispatch_switch:
    {
        INC_R();
        uint8_t op = M1_FETCH();
        /* Fallback switch - same labels ale přes switch */
        /* (Pro non-GCC kompilátory - není implementováno v této verzi) */
        (void)op;
        goto done; /* Placeholder */
    }
#endif

done:
    /* Lokální cache zaniká s návratem - odregistrovat */
    cpu->_active_cache = NULL;
    /* Final sync cyklů co se nahromadily v fast path */
    if (cycles_executed > last_sync) {
        cpu->cycles += cycles_executed - last_sync;
        cpu->total_cycles += cycles_executed - last_sync;
    }
    /* Writeback lokálních registrů */
    WRITEBACK();

    /*
     * Úklid maker - NUTNÉ pro dvojitý include této .inc (batch + step).
     * Bez kompletního #undef by druhý include hlásil redefinici maker.
     */
#undef AF_VAL
#undef BC_VAL
#undef DE_VAL
#undef HL_VAL
#undef SET_AF
#undef SET_BC
#undef SET_DE
#undef SET_HL
#undef WZH_VAL
#undef WZL_VAL
#undef SET_WZH
#undef SET_WZL
#undef M1_FETCH
#undef FETCH
#undef FETCH16
#undef INC_R
#undef PUSH
#undef POP
#undef FIRE_IFF_CHANGE
#undef RST_HOOK
#undef WRITEBACK
#undef RELOAD
#undef FIRE_CALL
#undef FIRE_RET
#undef ADD8
#undef ADC8
#undef SUB8
#undef SBC8
#undef AND8
#undef XOR8
#undef OR8
#undef CP8
#undef INC8
#undef DEC8
#undef ADD16
#undef ADC16
#undef SBC16
#undef RLC
#undef RRC
#undef RL
#undef RR
#undef SLA
#undef SRA
#undef SLL
#undef SRL
#undef BIT_OP
#undef BIT_MEM
#undef DISPATCH
#undef F_WRITE
#undef Q_UPDATE
#undef NEXT
#undef NEXT_SLOW
#undef DDFD_PREFIX
#undef DDFD_IGNORED
#undef DDFD_CHAIN_MAX
#undef BLOCK_REPEAT_FLAGS
#undef BLOCK_IO_REPEAT_FLAGS
#undef RD
#undef WR
#undef IO_RD
#undef IO_WR
#undef RD16
#undef WR16

    return cycles_executed;
}

#if Z80_DIRECT_REGS
/* Uvolnění registrových aliasů step varianty (platnost = jen tento include). */
#undef rA
#undef rF
#undef rB
#undef rC
#undef rD
#undef rE
#undef rH
#undef rL
#undef rPC
#undef rSP
#undef rWZ
#undef rR
#undef rQ
#endif
