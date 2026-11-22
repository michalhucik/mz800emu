# tests/libs/cpu_z80/CMakeLists.txt
# Standalone test - mzlib_cpu_z80 + Unity (zadny emu core).
#
# Pokriva IFF1/IFF2 change callback (z80_set_iff_change) - V17+ debugger
# BP event cpu:iff1_change source pripravy. Viz emu-experiments/
# z80-iff1-hook/README.md.

mz_add_test(cpu_z80_iff_change
    TYPE     STANDALONE
    SOURCES  test_iff_change.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva cpu_ctrl_event_cb (HALT enter/exit + RST nn) - event-viewer
# mutant Vlna 1 Commit 4. Viz emu-experiments/event-viewer/README.md.
mz_add_test(cpu_z80_cpu_ctrl_event
    TYPE     STANDALONE
    SOURCES  test_cpu_ctrl_event.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva call_cb / ret_cb (CALL / RET dispatch hook) - callstack mutant
# Faze 1A. Viz emu-experiments/callstack/README.md.
mz_add_test(cpu_z80_call_ret_hooks
    TYPE     STANDALONE
    SOURCES  test_call_ret_hooks.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva EI-delay v per-step variante jadra (regrese v2.0.2 - hra Gun Runner).
# INT po EI se smi prijmout az po nasledujici instrukci; jinak "EI; LD SP,nn"
# pushne na stary zasobnik a poskodi pamet. Viz emu-experiments/gun-runner/.
mz_add_test(cpu_z80_ei_delay
    TYPE     STANDALONE
    SOURCES  test_ei_delay.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva inkrement R pri opakovanem fetchi HALT a pri prijeti INT/NMI
# (regrese v2.0.2 - loader hry Interkarate desifruje klicem z R po HALT).
# Viz emu-experiments/interkarate-cmt/.
mz_add_test(cpu_z80_r_register
    TYPE     STANDALONE
    SOURCES  test_r_register.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva navratovou hodnotu z80_step() na hranicich total_cycles 2^31 a 2^32
# (puvodne odecet pres int = signed overflow, UB).
mz_add_test(cpu_z80_step_cycles
    TYPE     STANDALONE
    SOURCES  test_step_cycles.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokrývá nulování cpu->op_tstate v z80_reset(). Po resetu nesmí zůstat délka
# poslední instrukce, emulátor op_tstate čte i mimo instrukci (bootstrap MZF,
# ladicí příkazy). Viz emu-experiments/z80-reset-optstate/README.md.
mz_add_test(cpu_z80_reset_state
    TYPE     STANDALONE
    SOURCES  test_reset_state.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pořadí zápisů na zásobník: PUSH, CALL, RST, přijetí INT (IM 0/1/2) a NMI
# zapisují vyšší bajt dřív (Zilog UM0080), LD (nn),rr nižší dřív.
# Viz emu-experiments/z80-push-order/README.md.
mz_add_test(cpu_z80_push_order
    TYPE     STANDALONE
    SOURCES  test_push_order.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pořadí čtení 16bitové hodnoty z paměti: LD rr,(nn), POP, RET, RETI
# a EX (SP),HL čtou nižší bajt dřív (Zilog UM0080). Na pořadí záleží
# u čekacích taktů VRAM a u mapovaných portů se stavem.
mz_add_test(cpu_z80_read16_order
    TYPE     STANDALONE
    SOURCES  test_read16_order.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Příznaky v opakující iteraci LDIR/LDDR/CPIR/CPDR/INIR/INDR/OTIR/OTDR
# (YF/XF z adresy instrukce, u INxR/OTxR i PF/HF) podle měření BLKFLAGS
# na MZ-800 a na reálných čipech Z80. Viz emu-experiments/z80-block-flags/.
mz_add_test(cpu_z80_block_repeat
    TYPE     STANDALONE
    SOURCES  test_block_repeat.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Interní registr Q: YF/XF po SCF a CCF = ((Q xor F) or A) and 28h, Q = F po
# instrukci se zápisem příznaků, jinak 0. Krokování, dávka i hranice volání
# musí dát stejný výsledek (program syscheck na MZ-800: 64 shod ze 64).
# Viz emu-experiments/q-register/README.md.
mz_add_test(cpu_z80_q_register
    TYPE     STANDALONE
    SOURCES  test_q_register.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)
