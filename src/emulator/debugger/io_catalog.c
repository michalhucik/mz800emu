/*
 * io_catalog.c - Implementace statického katalogu I/O portů (D.7).
 *
 * Datová tabulka je definovaná na konci souboru jako g_io_ports[].
 * Nejdřív jsou implementace per-port read_value() callbacků a per-port
 * bit description tabulek + value decoderů.
 *
 * V1.5 naming konvence (per UX spec ux-io-ports.md):
 *   "<chip> - <function> (<direction>)"
 *
 *   Příklady:
 *     "GDG - WF (W)"
 *     "GDG - DMD (W)"
 *     "GDG - Status (R)"
 *     "8255 PPI - Port A (R/W)"
 *     "8253 CTC - Counter 0 (R/W)"
 *     "Z80 PIO - Port A data (R/W)"
 *     "JOY0 (R)"
 *     "Memory bank - E0 (R/W)"
 *     "Memory ext - MEMEXT bank (W)"
 *
 * V1.5 multi-entry per addr: kde IORQ IN a IORQ OUT na téže adrese mají
 * různý význam (např. 0CEh: OUT=DMD, IN=Status), tabulka má 2 oddělené
 * entries (= různý direction + různé bit popisy).
 *
 * Live čtení hodnot:
 *   - read_value() funkce čtou DIRECT register state (g_gdg, g_pio8255,
 *     g_vramctrl atd.) - side-effect free.
 *   - Side-effect path (např. CPU IN -> psg_write_byte / fdc cmd) NEPOUŽÍVÁME.
 *   - Pokud port nemá readback (write-only HW jako WF/RF), vracíme
 *     interní mirror, který si emulátor sám udržuje.
 *
 * Pokrytí V1 (priority 1): GDG WF/RF/DMD/Status/paleta, banking E0-E4,
 * 8255 PPI 0D0h-0D3h, Z80 PIO 0FCh-0FFh, JOY0/JOY1.
 *
 * Některé porty (CTC, FDC) jsou ponechány jako "placeholder" bez bit
 * dekódu - lze doplnit ve V1.5.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#include "io_catalog.h"

#include <stdio.h>
#include <stddef.h>

#include "mzarch/mzarch_config.h"

/* Direct register access - read-only side-effect free.
 * Per-arch include - mz800_gdg.h a mz1500_gdg.h definuji stejne nazvy
 * struktur (st_GDG, g_gdg) ale s ruznymi field sady. Includujeme jen
 * jeden podle aktualni MZARCH compile-time selekce. */
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_gdg.h"
#include "mzarch/mz1500/gdg/mz1500_vramctrl.h"
#elif MZARCH == 700
#include "mzarch/mz700/gdg/mz700_gdg.h"
#include "mzarch/mz700/gdg/mz700_vramctrl.h"
#endif

#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/joy/joy.h"
#include "mzarch/mzarch.h"

/* Per-arch memory.h - flagy pro g_memory.map bit dekódy v Memory bank
 * sekci. Stejně jako u GDG includujeme jen jeden podle MZARCH. */
#if MZARCH == 800
#include "mzarch/mz800/memory/mz800_memory.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/memory/mz1500_memory.h"
#elif MZARCH == 700
#include "mzarch/mz700/memory/mz700_memory.h"
#endif

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/wd279x.h"
#endif

/* ========================================================================= */
/*  read_value() callbacky                                                   */
/* ========================================================================= */

#if ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 )

/**
 * Read DMD register (Display Mode) - mirror v g_gdg.regDMD.
 *
 * Pozn.: Bit 3 ma jiny vyznam mezi MZ-800 (= MZ-700 mode) a MZ-1500
 * (= MZ-1500 mode flag). UI panel zobrazuje syrovy byte; bit popis
 * v bits_gdg_dmd vychazi z MZ-800 perspektivy (knowledge base).
 *
 * @return Aktuální obsah DMD (8-bit; reálně se používají bity 0-3).
 */
static uint8_t read_gdg_dmd(void)
{
    return (uint8_t)(g_gdg.regDMD & 0xFF);
}

/**
 * Read GDG status registr (= IN 0CEh).
 *
 * Skládá ho dohromady z bitů: TEMPO, SW1 (DIP), CKSW, vždy 0,
 * VSYNC, HSYNC, VBLNK, HBLNK. Pro účely viewer panelu agregujeme
 * z internal stavu.
 *
 * Bit 1 (SW1) - reálně závisí na DIP switch konfiguraci. Pro V1
 * vracíme 0 ("MZ-800 mode"); přesné DIP polling by vyžadovalo
 * další API call. [neověřeno v UI kontextu]
 *
 * @return 8-bit Status registr.
 */
static uint8_t read_gdg_status(void)
{
    uint8_t s = 0;
    s |= (g_gdg.tempo & 1) ? 0x01 : 0x00;
    /* SW1 - DIP switch [neověřeno]: V1 vrací 0 */
#if MZARCH == 800
    s |= (g_gdg.cksw ? 0x04 : 0x00);  /* bit 2: CKSW (Superimpose) */
#endif
    s |= (g_gdg.sts_vsync ? 0x10 : 0x00);
    s |= (g_gdg.sts_hsync ? 0x20 : 0x00);
    s |= (g_gdg.vbln ? 0x40 : 0x00);
    s |= (g_gdg.hbln ? 0x80 : 0x00);
    return s;
}

#if MZARCH == 800

/**
 * Read GDG WF register (Write Format) - mirror přes g_vramctrl.
 *
 * Reconstruct z internal struktury (regWF_PLANE + regWF_MODE +
 * regWFRF_VBANK):
 *   bit 0-3 = plane select (regWF_PLANE)
 *   bit 4   = VBANK
 *   bit 5-7 = WF mode (regWF_MODE)
 *
 * MZ-1500 nepouziva GDG WF/RF stejnym zpusobem - na MZ-1500 buildu
 * je tato funkce nedostupna (= viz read_gdg_wf_placeholder pro MZ-1500).
 *
 * @return Rekonstrukce posledního zapsaného WF.
 */
static uint8_t read_gdg_wf(void)
{
    uint8_t v = 0;
    v |= (uint8_t)(g_vramctrl.regWF_PLANE & 0x0F);
    v |= (g_vramctrl.regWFRF_VBANK ? 0x10 : 0x00);
    v |= (uint8_t)((g_vramctrl.regWF_MODE & 0x07) << 5);
    return v;
}

/**
 * Read GDG RF register (Read Format) - mirror přes g_vramctrl.
 *
 * Reconstruct:
 *   bit 0-3 = plane select (regRF_PLANE)
 *   bit 4   = VBANK
 *   bit 5-6 = reserved
 *   bit 7   = SEARCH
 *
 * @return Rekonstrukce posledního zapsaného RF.
 */
static uint8_t read_gdg_rf(void)
{
    uint8_t v = 0;
    v |= (uint8_t)(g_vramctrl.regRF_PLANE & 0x0F);
    v |= (g_vramctrl.regWFRF_VBANK ? 0x10 : 0x00);
    v |= (g_vramctrl.regRF_SEARCH ? 0x80 : 0x00);
    return v;
}

#else /* MZARCH != 800 - na MZ-1500 / MZ-700 nemame WF/RF mirror */

/**
 * MZ-1500 / MZ-700 placeholder - vrati 0 (= UI zobrazi 00).
 *
 * MZ-1500 ma odlisny VRAM model (viz mz1500_vramctrl.h), takze
 * presny content WF/RF zde nelze rekonstruovat bez per-arch reseni.
 * Pro V1 vracime 0 a pole bude v UI vypadat jako write-only registr
 * bez readbacku - akceptovatelny trade-off.
 */
static uint8_t read_gdg_wf(void) { return 0; }
static uint8_t read_gdg_rf(void) { return 0; }

#endif /* MZARCH == 800 */


/**
 * Read joystick 0 input (IORQ 0F0h, MZ-800 only).
 *
 * Side-effect free: čte aktuální stav joysticku z `joy_read_byte()` (= čistá
 * HW probe, žádná mutace chip state). Pokud PPI Port A enable flag pro
 * JOY1 (`g_pio8255.signal_PA_joy1_enabled`) není aktivní, vrací 0xFF (= žádný
 * vstup, stejné chování jako reálný IORQ read). Konzistentní s
 * `port_read_no_se_cb` v `mz800_iorq.c`.
 *
 * Bity (active LOW dle `en_JOY_STATEBIT`): 0=UP, 1=DOWN, 2=LEFT, 3=RIGHT,
 * 4=TRIG1, 5=TRIG2. Bity 6-7 vždy 1.
 *
 * @return Aktuální joystick byte, 0xFF pokud disabled.
 */
static uint8_t read_joy0(void)
{
    if (!g_pio8255.signal_PA_joy1_enabled)
        return 0xFF;
    return joy_read_byte(JOY_DEVID_0);
}

/**
 * Read joystick 1 input (IORQ 0F1h, MZ-800 only).
 *
 * Side-effect free, mechanika shodná s `read_joy0()`. Enable flag
 * `g_pio8255.signal_PA_joy2_enabled`.
 *
 * @return Aktuální joystick byte, 0xFF pokud disabled.
 */
static uint8_t read_joy1(void)
{
    if (!g_pio8255.signal_PA_joy2_enabled)
        return 0xFF;
    return joy_read_byte(JOY_DEVID_1);
}

/**
 * Bit popisky pro JOY0/JOY1 byte (active LOW - bit = 0 znamená pressed).
 */
static const st_IO_BIT_DESC bits_joy[] = {
    { 0, 1, "UP",    "Up (pressed when 0)" },
    { 1, 1, "DOWN",  "Down (pressed when 0)" },
    { 2, 1, "LEFT",  "Left (pressed when 0)" },
    { 3, 1, "RIGHT", "Right (pressed when 0)" },
    { 4, 1, "TRIG1", "Trigger 1 (pressed when 0)" },
    { 5, 1, "TRIG2", "Trigger 2 (pressed when 0)" }
};


/**
 * Read GDG palette/border value (mirror).
 *
 * GDG má 4 paletové registry + border (BOR). Hodnota na 0F0h OUT je
 * close k regBOR pro border, ale vlastní paletový mechanismus je
 * složitější (palette group). Pro V1 vracíme regBOR jako "poslední
 * border".
 *
 * @return regBOR hodnotu (0..15 pro 4-bit barvu).
 */
static uint8_t read_gdg_palette(void)
{
    return (uint8_t)(g_gdg.regBOR & 0xFF);
}

#if MZARCH == 800

/**
 * Read SOF0 - dolních 8 bitů scroll offset (g_hwscroll.regSOF & 0xFF).
 *
 * @return Spodní byte aktualne nastaveneho SOF.
 */
static uint8_t read_gdg_sof0(void)
{
    return (uint8_t)(g_hwscroll.regSOF & 0xFF);
}

/**
 * Read SOF1 - horní 2 bity scroll offset (g_hwscroll.regSOF >> 8 & 0x03).
 *
 * Bity 0-1 = horní 2 bity SOF (= bity 8-9 plné 10-bit hodnoty).
 * Bity 2-7 jsou ignorovány.
 *
 * @return Spodní 2 bity = horní 2 bity SOF.
 */
static uint8_t read_gdg_sof1(void)
{
    return (uint8_t)((g_hwscroll.regSOF >> 8) & 0x03);
}

/**
 * Read SW (scroll width) z g_hwscroll.regSW.
 *
 * @return SW jako 8-bit.
 */
static uint8_t read_gdg_sw(void)
{
    return (uint8_t)(g_hwscroll.regSW & 0xFF);
}

/**
 * Read SSA (scroll start address) z g_hwscroll.regSSA.
 *
 * @return SSA jako 8-bit.
 */
static uint8_t read_gdg_ssa(void)
{
    return (uint8_t)(g_hwscroll.regSSA & 0xFF);
}

/**
 * Read SEA (scroll end address) z g_hwscroll.regSEA.
 *
 * @return SEA jako 8-bit.
 */
static uint8_t read_gdg_sea(void)
{
    return (uint8_t)(g_hwscroll.regSEA & 0xFF);
}

/**
 * Read BCOL (border color) z g_gdg.regBOR.
 *
 * BCOL je 4-bit barva (bits 0-3). Bity 4-7 jsou GDG ignorovány.
 *
 * @return regBOR (0..15).
 */
static uint8_t read_gdg_bcol(void)
{
    return (uint8_t)(g_gdg.regBOR & 0x0F);
}

/**
 * Read CKSW (Superimpose) - mirror z g_gdg.cksw.
 *
 * CKSW se nastavuje zapisem na 0xCF07 bit 7. bits_gdg_cksw popisuje
 * 0xCF07 jako 16-bit IORQ port, kde bit 7 = CKSW state. Vracime stejnou
 * konvenci: bit 7 = aktualni stav CKSW.
 *
 * @return Byte se CKSW na bit 7 (= 0x80 pokud Superimpose ON, jinak 0).
 */
static uint8_t read_gdg_cksw(void)
{
    return g_gdg.cksw ? 0x80 : 0x00;
}

#endif /* MZARCH == 800 */

/**
 * DMD value decoder - vrací textovou interpretaci pro různé módy.
 *
 * Bity 3-2 určují základní rozlišení:
 *   00 = 320x200 (MZ-800), 01 = 640x200, 10 = MZ-700, 11 = nepovolené.
 *
 * @param value DMD byte.
 * @return Statický řetězec (žádný caller free).
 */
static const char *decode_dmd(uint8_t value)
{
    static char buf[80];
    const char *mode;
    switch ((value >> 2) & 0x03)
    {
    case 0: mode = "320x200 (MZ-800)"; break;
    case 1: mode = "640x200 (MZ-800)"; break;
    case 2: mode = "MZ-700 mode"; break;
    default: mode = "illegal"; break;
    }
    snprintf(buf, sizeof(buf), "screen=%s, gfx-bits=%u",
             mode, (unsigned)(value & 0x03));
    return buf;
}

/**
 * WF mode decoder - lidsky čitelný název write mode.
 *
 * @param value WF byte.
 * @return Statický řetězec.
 */
static const char *decode_wf(uint8_t value)
{
    static char buf[80];
    const char *mode;
    unsigned m = (value >> 5) & 0x07;
    switch (m)
    {
    case 0: mode = "SINGLE"; break;
    case 1: mode = "EXOR"; break;
    case 2: mode = "OR"; break;
    case 3: mode = "RESET"; break;
    case 4: mode = "REPLACE"; break;
    case 6: mode = "PSET"; break;
    default: mode = "?"; break;
    }
    snprintf(buf, sizeof(buf), "mode=%s, plane=0x%X, vbank=%u",
             mode, (unsigned)(value & 0x0F),
             (unsigned)((value >> 4) & 1));
    return buf;
}

#endif /* MZARCH == 800 || 1500 */

/**
 * Read PPI 8255 Port A (klávesnice column select + JOY enable).
 *
 * @return signal_PA jako byte.
 */
static uint8_t read_ppi_pa(void)
{
    return (uint8_t)(g_pio8255.signal_PA & 0xFF);
}

/**
 * Read PPI 8255 Port B (klávesnicový row data + vkbd overlay) - mirror.
 *
 * Side-effect free klidový snapshot: vrací řádek `keyboard_matrix[col]`
 * AND `vkbd_matrix[col]` pro aktuální column select v
 * `signal_PA_keybord_column` (= LSB nibble Port A). NEvolá
 * `iface_keyboard_pool_keyboard_events()` ani neaktualizuje
 * vkbd autotype state, takže reflektuje stav z posledního CPU IORQ
 * cyklu na portu 0xD1/E001 (= "co viděl CPU naposled"). UI tedy ukazuje
 * klidový obsah cache, ne live HW probe - viz konvence
 * `feedback_io_mirror_pending_bits` v memory.
 *
 * Bity = invertované klávesy v zvoleném řádku: bit i = 0 znamená
 * klávesa stisknuta, 1 = nestisknuta. Bity přes vkbd_matrix dovolují
 * GUI virtual keyboard overlayu vstoupit do scanu.
 *
 * @return keyboard_matrix[col] & vkbd_matrix[col]
 */
static uint8_t read_ppi_pb(void)
{
    unsigned col = g_pio8255.signal_PA_keybord_column;
    if (col >= 10) {
        /* Default fallback - column select mimo 0..9 (= illegal HW stav)
         * vrátí 0xFF "nic stisknuto" namísto OOB čtení do pole 10 entries. */
        return 0xFF;
    };
    uint8_t kb  = (uint8_t)(g_pio8255.keyboard_matrix[col] & 0xFF);
    uint8_t vkb = (uint8_t)(g_pio8255.vkbd_matrix[col] & 0xFF);
    return (uint8_t)(kb & vkb);
}

/**
 * Read PPI 8255 Control word - debug/UI mirror.
 *
 * Vrací `g_pio8255.last_cw_byte` - poslední byte zapsaný na port 0xD3
 * (resp. MMIO 0xE003). 8255 hardware nemá readable Control register
 * (= sequencer), tento mirror je čistě cache pro debugger UI udržovaný
 * v IORQ write path (`pio8255_write` case `DEF_PIO8255_MASTER`).
 *
 * Default 0x00 dokud HW init nebyl proveden. MZ-800 ROM init sekvence
 * obvykle napíše 0x8A (Mode Set: PA out, PCh in, PB in, PCl out).
 *
 * @return cached g_pio8255.last_cw_byte
 */
static uint8_t read_ppi_cw(void)
{
    return g_pio8255.last_cw_byte;
}

/**
 * Read PPI 8255 Port C (CMT + INT control + cursor timer + VBLN).
 *
 * Rebuild byte z jednotlivych signalu (g_pio8255.signal_PC neukladame -
 * je vzdy 0). OUT bity 0-3 jsou latch (signal_pc00..pc03), IN bity 4-7
 * jsou live signaly cmt_motor/cmt_data/cursor_timer/VBLN.
 *
 * Side-effect free: cte primo cached state, NEvolá cmt_read_data() ani
 * pio8255_read() ktere maji side effecty (cmt_update_output, BP fire).
 *
 * @return PortC value rebuilt z aktualnich signalu.
 */
static uint8_t read_ppi_pc(void)
{
    uint8_t retval = 0;

    /* OUT bity 0-3 (latched output values) */
    retval |= g_pio8255.signal_pc00 ? (1u << 0) : 0;
    retval |= g_pio8255.signal_pc01 ? (1u << 1) : 0;
    retval |= g_pio8255.signal_pc02 ? (1u << 2) : 0;
    retval |= g_pio8255.signal_pc03 ? (1u << 3) : 0;

    /* IN bity 4-7 (live signals, side-effect free reading) */
    retval |= g_pio8255.signal_pc04 ? (1u << 4) : 0;            /* CMT motor state */
    retval |= (g_cmt.output & 1u) << 5;                          /* CMT data in (cached) */
    retval |= (mz800_main_get_cursor_timer_state() & 1u) << 6;   /* cursor blink */
    retval |= SIGNAL_GDG_VBLNK ? (1u << 7) : 0;                  /* VBLN */

    return retval;
}

/**
 * Read Z80 PIO Port A data.
 *
 * @return data_output Port A.
 */
static uint8_t read_pioz80_a_data(void)
{
    return g_pioz80.port[PIOZ80_PORT_A].data_output;
}

/**
 * Read Z80 PIO Port B data.
 *
 * @return data_output Port B.
 */
static uint8_t read_pioz80_b_data(void)
{
    return g_pioz80.port[PIOZ80_PORT_B].data_output;
}

/**
 * Read Z80 PIO Port A control - debug/UI mirror.
 *
 * Vrací `g_pioz80_port_a_last_ctrl_byte` - posledni byte zapsany na
 * port 0xFC. Z80 PIO control je sequencer: byte muze byt MCW
 * (Mode Set), ICW (Interrupt Control), INT Vector, nebo follow-up byte
 * (IOW mask po MCW Mode 3, IM mask po ICW s MF bit). HW samotne neumi
 * readback, mirror je debug-only cache udrzovana v pioz80_write_byte.
 *
 * Pro plnou interpretaci pouzij `decode_pioz80_ctrl` ktery rozhoduje
 * podle nizsich bitu byte. Aktualni interni stav sequenceru je v
 * g_pioz80.port[PORT_A].ctrl_expect a .mode.
 *
 * @return cached g_pioz80_port_a_last_ctrl_byte
 */
static uint8_t read_pioz80_a_ctrl(void)
{
    return g_pioz80_port_a_last_ctrl_byte;
}

/**
 * Read Z80 PIO Port B control - debug/UI mirror.
 * Stejny pattern jako Port A; cache pro port 0xFD.
 *
 * @return cached g_pioz80_port_b_last_ctrl_byte
 */
static uint8_t read_pioz80_b_ctrl(void)
{
    return g_pioz80_port_b_last_ctrl_byte;
}

/**
 * Read 8253 CTC counter aktuální dolní byte hodnoty.
 *
 * Vrací @c g_ctc8253[N].value & 0xFF - aktuální hodnotu čítače
 * (= side-effect free direct read interní structuralní hodnoty,
 * NE přes ctc8253_read_byte, který může spouštět latch operaci).
 *
 * Aktuální value reflektuje běžící countdown; zobrazí v UI okamžitý
 * stav čítače. Pro detail (status, mode, gate) by byl potřeba
 * dedicated CTC panel - V1.7+.
 *
 * @return LSB g_ctc8253[N].value (0..255).
 */
static uint8_t read_ctc0_value(void)
{
    return (uint8_t)(g_ctc8253[0].value & 0xFF);
}

static uint8_t read_ctc1_value(void)
{
    return (uint8_t)(g_ctc8253[1].value & 0xFF);
}

static uint8_t read_ctc2_value(void)
{
    return (uint8_t)(g_ctc8253[2].value & 0xFF);
}

/**
 * Helper: vrátí textový summary stavu CTC counteru N.
 *
 * Formát: "ModeM, [BCD/Bin], <state>, preset=0xNNNN, val16=0xNNNN"
 *
 * Důvod helper + per-counter wrappers: io_catalog `decode` callback
 * dostává jen `value` byte bez kontextu (= žádný counter index). Wrappery
 * `decode_ctc0/1/2_value` volají tento helper s pevným indexem 0/1/2,
 * což překlenuje API limitaci.
 *
 * Counter `.value` field je 16-bit (`unsigned`), zatímco Value sloupec
 * v Overview tabu ukáže jen 8-bit LSB. Decode zobrazí plné 16-bit current
 * value + preset pro úplný kontext.
 *
 * @param cs Counter index 0..2
 * @return Static buffer s decoded summary
 */
static const char *ctc_counter_describe(unsigned cs)
{
    static char buf[96];
    if (cs > 2) {
        snprintf(buf, sizeof(buf), "?");
        return buf;
    };
    const st_CTC8253 *ctc = &g_ctc8253[cs];
    static const char *mode_name[] = {
        "Mode0", "Mode1", "Mode2", "Mode3", "Mode4", "Mode5"
    };
    static const char *state_name[] = {
        "INIT", "INIT_DONE", "LOAD", "PRESET_ERR", "LOAD_DONE",
        "WAIT_GATE1", "PRESET", "PRESET32", "COUNTDOWN",
        "MODE1_TRG_ERR", "BLIND_COUNT"
    };
    const char *mode_str = (ctc->mode <= CTC_MODE5) ? mode_name[ctc->mode] : "Mode?";
    unsigned st = (unsigned)ctc->state;
    const char *state_str = (st < sizeof(state_name) / sizeof(state_name[0]))
                          ? state_name[st] : "STATE?";
    const char *bcd_str = ctc->bcd ? "BCD" : "Bin";
    snprintf(buf, sizeof(buf), "%s, %s, %s, preset=0x%04X, val16=0x%04X",
             mode_str, bcd_str, state_str,
             (unsigned)(ctc->preset_value & 0xFFFFu),
             (unsigned)(ctc->value & 0xFFFFu));
    return buf;
}

/* Per-counter decode wrappers - io_catalog decode API potrebuje counter
 * index, ktery z value byte nezjistime. Konstantni wrapper per port. */
static const char *decode_ctc0_value(uint8_t value)
{
    (void)value; /* state je v g_ctc8253[0], ne v parameter value */
    return ctc_counter_describe(0);
}

static const char *decode_ctc1_value(uint8_t value)
{
    (void)value;
    return ctc_counter_describe(1);
}

static const char *decode_ctc2_value(uint8_t value)
{
    (void)value;
    return ctc_counter_describe(2);
}

/**
 * Read 8253 CTC Control word - debug/UI mirror.
 *
 * Vrací `g_ctc8253_last_cw_byte` - poslední byte zapsaný na port 0xD7
 * (resp. MMIO 0xE007). HW samotné CW neumí přečíst (= sequencer). Mirror
 * je cache udržovaná v `ctc8253_write_byte` při zápisu na CTCADDR_CWREG.
 *
 * Default 0x00 dokud ROM init neproběhne. Zachycuje VŠECHNY CW writes
 * včetně CS_ILLEGAL (= bity 7:6 == 11), což dovoluje vidět malformed
 * konfigurace v UI.
 *
 * @return cached g_ctc8253_last_cw_byte
 */
static uint8_t read_ctc_cw(void)
{
    return g_ctc8253_last_cw_byte;
}

/**
 * Read 0xE008 status mirror (V1.5.E - MMIO entry).
 *
 * MMIO read na 0xE008 v MZ-700 mode vraci agregovany status (HBLNK + TEMPO
 * MZ-700 mode na MZ-800 emuluje status shodny s 0xCE).
 * Side-effect free - jen agregace signalu.
 *
 * Pro UI panel zobrazujeme tento mirror v "MZ-700 mem-mapped IO" sekci
 * Overview tabu. Reuse existing gdg_read_dmd_status_memop wrapper, ktery
 * uz dela presne tu agregaci jak ji vidi MZ-700 ROM po LD A,(0E008h).
 *
 * @return 8-bit status (HBLNK | TEMPO | optional JOY mask)
 */
static uint8_t read_mmio_e008_status(void)
{
    return gdg_read_dmd_status_memop();
}

#if CFG_HWEXT_HAVE_FDC
/**
 * Read WD279x status register - side-effect free mirror.
 *
 * Volá `wd279x_mirror_status_get()` (= Type I rebuild s live NOT_READY,
 * Type II/III raw regSTATUS). Pokud FDC není připojen na sběrnici, vrací
 * 0xFFh (= pull-up bus, simulace odpojeného zařízení) místo "klamavé"
 * hodnoty z neaktivního chipu.
 *
 * @return 8-bit status byte (true-bus, bez Sharp inverze - UI ho zobrazí
 *         tak, jak by ho viděl chip na svých pinech)
 */
static uint8_t read_fdc_status(void)
{
    if (FDC_TEST_NOT_CONNECTED) return 0xFFu;
    return wd279x_mirror_status_get(&g_fdc[FDC0].wd279x);
}

/**
 * Read WD279x track register - side-effect free mirror.
 * @return 8-bit track register hodnota
 */
static uint8_t read_fdc_track(void)
{
    if (FDC_TEST_NOT_CONNECTED) return 0xFFu;
    return wd279x_mirror_track_get(&g_fdc[FDC0].wd279x);
}

/**
 * Read WD279x sector register - side-effect free mirror.
 * @return 8-bit sector register hodnota
 */
static uint8_t read_fdc_sector(void)
{
    if (FDC_TEST_NOT_CONNECTED) return 0xFFu;
    return wd279x_mirror_sector_get(&g_fdc[FDC0].wd279x);
}

/**
 * Read FDC Motor / Drive select latch (0xDC W-only).
 *
 * Sharp external logic kolem WD279x - port 0xDC drží Motor on/off bit +
 * drive select (= ne uvnitř WD279x chipu samotného, ale ve wd279x struct
 * jako 8-bit latch). Side-effect free direct field read.
 *
 * @return MOTOR latch byte, 0xFF pokud FDC odpojen
 */
static uint8_t read_fdc_motor(void)
{
    if (FDC_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC0].wd279x.MOTOR;
}

/**
 * Read FDC Side latch (0xDD W-only).
 *
 * Bit 0 = side select (0 = side A, 1 = side B). Sharp inverse bus
 * (= viz BUS xlate, ale latch ve struct už po xlate). Side-effect free.
 *
 * @return SIDE latch byte (typicky 0 nebo 1), 0xFF pokud FDC odpojen
 */
static uint8_t read_fdc_side(void)
{
    if (FDC_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC0].wd279x.SIDE;
}

/**
 * Read FDC Density latch (0xDE W-only).
 *
 * Bit 0 = density (0 = single, 1 = double). Side-effect free.
 *
 * @return DENSITY latch byte (typicky 0 nebo 1), 0xFF pokud FDC odpojen
 */
static uint8_t read_fdc_density(void)
{
    if (FDC_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC0].wd279x.DENSITY;
}

/**
 * Read FDC HD Patch EINT latch (0xDF W-only).
 *
 * HD Patch je MZ-800 specific external logic. Když je obvod nainstalován
 * (= g_fdc[FDC0].hd_patch == 1), port 0xDF kontroluje interrupt enable pin.
 * Side-effect free.
 *
 * @return EINT latch byte, 0xFF pokud FDC odpojen
 */
static uint8_t read_fdc_eint(void)
{
    if (FDC_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC0].wd279x.EINT;
}

/* ===== FDC1 (sekundární) mirror gettery - čtou g_fdc[FDC1] ===== */

/** Test: FDC1 není připojen (analogie FDC_TEST_NOT_CONNECTED pro FDC0). */
#define FDC1_TEST_NOT_CONNECTED (g_fdc[FDC1].connected != FDC_CONNECTED)

static uint8_t read_fdc1_status(void)
{
    if (FDC1_TEST_NOT_CONNECTED) return 0xFFu;
    return wd279x_mirror_status_get(&g_fdc[FDC1].wd279x);
}

static uint8_t read_fdc1_track(void)
{
    if (FDC1_TEST_NOT_CONNECTED) return 0xFFu;
    return wd279x_mirror_track_get(&g_fdc[FDC1].wd279x);
}

static uint8_t read_fdc1_sector(void)
{
    if (FDC1_TEST_NOT_CONNECTED) return 0xFFu;
    return wd279x_mirror_sector_get(&g_fdc[FDC1].wd279x);
}

static uint8_t read_fdc1_motor(void)
{
    if (FDC1_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC1].wd279x.MOTOR;
}

static uint8_t read_fdc1_side(void)
{
    if (FDC1_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC1].wd279x.SIDE;
}

static uint8_t read_fdc1_density(void)
{
    if (FDC1_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC1].wd279x.DENSITY;
}

static uint8_t read_fdc1_eint(void)
{
    if (FDC1_TEST_NOT_CONNECTED) return 0xFFu;
    return g_fdc[FDC1].wd279x.EINT;
}
#endif /* CFG_HWEXT_HAVE_FDC */

/**
 * Read aktuální Sharp banking state byte (g_memory.map).
 *
 * Banking porty 0xE0-0xE6 nemají vlastní hex value (= IORQ data je
 * irelevantní, dispatch jen na adresu), ale celá Sharp banking vrstva
 * udržuje aktuální stav v `g_memory.map` jako per-arch bitfield.
 * V Overview tabu všech 7 entries 0xE0-0xE6 ukazují stejnou hodnotu
 * (= global state, ne per-port).
 *
 * Side-effect free direct read globální struktury.
 *
 * Bity dekódují per-arch `bits_memory_map_*` array (viz níže). MZ-800
 * navíc význam `CGRAM_VRAM` flagu závisí na `g_gdg.regDMD` bitu 3
 * (700 compat / 800 native mode) - popisek pokrývá oba významy.
 *
 * Reference: mz{arch}_memory.h MEMORY_MZ{ARCH}_MAP_FLAG_*.
 *
 * @return 8-bit g_memory.map byte
 */
static uint8_t read_memory_map_byte(void)
{
    return (uint8_t)g_memory.map;
}

#if MZARCH == 1500
/**
 * Decode MZ-1500 SPEC bit-field (bity 2-4 v g_memory.map).
 *
 * SPEC řídí mapování oblasti 0xD000-0xEFFF: 0=žádné special mapování,
 * 1=CGROM, 2=PCG1, 3=PCG2, 4=PCG3. UI dekodér zobrazí symbolický název
 * misto numerické hodnoty.
 *
 * Reference: mz1500_memory.h MEMORY_MZ1500_MAP_D000_*.
 *
 * @param value bit-field value (3-bit, 0..7)
 * @return statický string popisující SPEC stav
 */
static const char *decode_mz1500_spec(uint8_t value)
{
    switch (value & 0x7u)
    {
    case 0: return "None (no special D000-EFFF mapping)";
    case 1: return "CGROM at D000-EFFF";
    case 2: return "PCG1 at D000-EFFF";
    case 3: return "PCG2 at D000-EFFF";
    case 4: return "PCG3 at D000-EFFF";
    default: return "(reserved)";
    }
}
#endif

/* ========================================================================= */
/*  Bit description tabulky                                                  */
/* ========================================================================= */

#if ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 )

/**
 * DMD register (0CEh OUT) - bit popisy.
 *
 * Bity 0-1 = grafické bloky (viz hw/09-video-mz800-modes.md).
 * Bity 2-3 = volba rozlišení.
 * Bity 4-7 = neviděny GDG (nepoužité).
 */
static const st_IO_BIT_DESC bits_gdg_dmd[] = {
    { 0, 1, "DMD0", "Volba grafickeho bloku LSB" },
    { 1, 1, "DMD1", "Volba grafickeho bloku" },
    { 2, 1, "DMD2", "Sirka rezimu (0=320, 1=640) - MZ-800 only" },
    { 3, 1, "DMD3", "MZ-700 rezim (1=MZ-700)" },
    { 4, 1, "-", "neaktivni" },
    { 5, 1, "-", "neaktivni" },
    { 6, 1, "-", "neaktivni" },
    { 7, 1, "-", "neaktivni" }
};

/**
 * GDG Status register bit popisy.
 *
 * Pouze ke čtení; agregace HW signálů. Per-arch:
 *   - MZ-800: full status (0CEh IN nebo 0xE008 IN v MZ-700 mode) -
 *     HBLN, VBLN, VSYNC, HSYNC, CKSW, SW1, TEMPO.
 *   - MZ-700/MZ-1500: jednodušší 0xE008 IN - HBLN, TEMPO + bity 1-4
 *     volitelně JOY mask přes joymz (active LOW).
 */
#if MZARCH == 800
static const st_IO_BIT_DESC bits_gdg_status[] = {
    { 0, 1, "TEMPO", "Stav signalu TEMPO" },
    { 1, 1, "SW1",   "DIP SW1 (1=MZ-700, 0=MZ-800)" },
    { 2, 1, "CKSW",  "Aktualni stav signalu CKSW (Superimpose)" },
    { 3, 1, "0",     "Vzdy 0 (overeno)" },
    { 4, 1, "VSYNC", "Stav signalu VSYNC" },
    { 5, 1, "HSYNC", "Stav signalu HSYNC" },
    { 6, 1, "VBLNK", "Stav signalu VBLNK" },
    { 7, 1, "HBLNK", "Stav signalu HBLNK" }
};
#else  /* MZ-700 / MZ-1500 */
static const st_IO_BIT_DESC bits_gdg_status[] = {
    { 0, 1, "TEMPO", "Stav signalu TEMPO" },
    { 1, 1, "JA1",   "JOY-A SW1 (display) / X osa (vblank), pres joymz; jinak 0" },
    { 2, 1, "JA2",   "JOY-A SW2 (display) / Y osa (vblank), pres joymz; jinak 0" },
    { 3, 1, "JB1",   "JOY-B SW1 (display) / X osa (vblank), pres joymz; jinak 0" },
    { 4, 1, "JB2",   "JOY-B SW2 (display) / Y osa (vblank), pres joymz; jinak 0" },
    { 7, 1, "HBLNK", "Stav signalu HBLNK" }
};
#endif

/**
 * WF register (0CCh OUT) - Write Format bit popisy.
 *
 *   bit 0-3 = plane select (PL0..PL3) - 0..15 maska 4 plánů
 *   bit 4   = VBANK (8000h-BFFFh secondary bank)
 *   bit 5-7 = write mode (000=SINGLE,001=EXOR,010=OR,011=RESET,
 *                         100=REPLACE,110=PSET)
 */
static const st_IO_BIT_DESC bits_gdg_wf[] = {
    { 0, 1, "PL0",    "Plane 0 select" },
    { 1, 1, "PL1",    "Plane 1 select" },
    { 2, 1, "PL2",    "Plane 2 select" },
    { 3, 1, "PL3",    "Plane 3 select" },
    { 4, 1, "VBANK",  "VRAM bank (0=primary, 1=secondary)" },
    { 5, 3, "MODE",   "Write mode (0=SINGLE,1=EXOR,2=OR,3=RESET,4=REPLACE,6=PSET)" }
};

/**
 * RF register (0CDh OUT) - Read Format bit popisy.
 */
static const st_IO_BIT_DESC bits_gdg_rf[] = {
    { 0, 1, "PL0",    "Plane 0 select" },
    { 1, 1, "PL1",    "Plane 1 select" },
    { 2, 1, "PL2",    "Plane 2 select" },
    { 3, 1, "PL3",    "Plane 3 select" },
    { 4, 1, "VBANK",  "VRAM bank (0=primary, 1=secondary)" },
    { 5, 1, "-",      "rezerva" },
    { 6, 1, "-",      "rezerva" },
    { 7, 1, "SEARCH", "Search mode (1=hledani 1. pixelu)" }
};

#if MZARCH == 800

/* ===== 0xCF 16-bit CRTC family bit popisy ===== */

/**
 * SOF0 (0xCF01 W) - dolních 8 bitů scroll offset.
 *
 * Plná SOF hodnota = 10-bit (SOF1 bity 0-1 = horní 2 bity, SOF0 = dolní 8 bitů).
 */
static const st_IO_BIT_DESC bits_gdg_sof0[] = {
    { 0, 8, "SOF[7:0]", "Dolnich 8 bitu scroll offset (SOF)" }
};

/**
 * SOF1 (0xCF02 W) - horní 2 bity scroll offset (bity 0-1 zápisu).
 */
static const st_IO_BIT_DESC bits_gdg_sof1[] = {
    { 0, 2, "SOF[9:8]", "Hornich 2 bity scroll offset (SOF)" },
    { 2, 6, "-",        "ignorovano" }
};

/**
 * SW (0xCF03 W) - scroll width (počet znaků na řádek scroll oblasti).
 */
static const st_IO_BIT_DESC bits_gdg_sw[] = {
    { 0, 8, "SW", "Scroll width (chars / radek scroll oblasti)" }
};

/**
 * SSA (0xCF04 W) - scroll start address.
 */
static const st_IO_BIT_DESC bits_gdg_ssa[] = {
    { 0, 8, "SSA", "Scroll start address (adresa zacatku scroll oblasti)" }
};

/**
 * SEA (0xCF05 W) - scroll end address.
 */
static const st_IO_BIT_DESC bits_gdg_sea[] = {
    { 0, 8, "SEA", "Scroll end address (adresa konce scroll oblasti)" }
};

/**
 * BCOL (0xCF06 W) - border color (4-bit, bity 0-3 jsou platné).
 */
static const st_IO_BIT_DESC bits_gdg_bcol[] = {
    { 0, 4, "BCOL", "Border color (4-bit, paleta GDG)" },
    { 4, 4, "-",    "ignorovano" }
};

/**
 * CKSW (0xCF07 W) - Superimpose enable (bit 7).
 *
 * Aktualni stav signalu CKSW lze cist v 0xCE Status, bit 2.
 */
static const st_IO_BIT_DESC bits_gdg_cksw[] = {
    { 0, 7, "-",    "ignorovano" },
    { 7, 1, "CKSW", "Superimpose enable (1 = ON)" }
};

/* ===== 0xCF decode helpers ===== */

/**
 * Decode BCOL: 4-bit hodnotu na čitelný název barvy.
 *
 * Paleta GDG (4-bit RGB+I):
 *   0=Black, 1=Blue, 2=Red, 3=Magenta, 4=Green, 5=Cyan, 6=Yellow, 7=White,
 *   8..15 = stejné s vyšším jasem (typicky GDG má jen 0..7 + I bit nebo
 *   16-color paleta).
 *
 * @param value Vstupní 8-bit (4-bit barva v bits 0-3).
 * @return Statický řetězec.
 */
static const char *decode_bcol(uint8_t value)
{
    static char buf[40];
    static const char *names[16] = {
        "Black", "Blue", "Red", "Magenta",
        "Green", "Cyan", "Yellow", "White",
        "(8)", "(9)", "(A)", "(B)",
        "(C)", "(D)", "(E)", "(F)"
    };
    unsigned c = value & 0x0F;
    snprintf(buf, sizeof(buf), "%s", names[c]);
    return buf;
}

/**
 * Decode CKSW: bit 7 jako "Superimpose ON/OFF".
 *
 * @param value Vstupní 8-bit.
 * @return Statický řetězec.
 */
static const char *decode_cksw(uint8_t value)
{
    return (value & 0x80) ? "Superimpose ON" : "Superimpose OFF";
}

#endif /* MZARCH == 800 - 0xCF sub-registry */

#endif /* MZARCH == 800 || 1500 */

/**
 * PPI 8255 Port A (0D0h IORQ na MZ-800, MMIO 0xE000 na MZ-700/1500/MZ-800
 * v 700 mode) - klavesnicovy column + JOY enable + cursor timer reset.
 *
 * Bity 4 a 5 (JOY1EN/JOY2EN) jsou MZ-800 specificke (= JOY na 0xF0/F1
 * IORQ). Na MZ-700/MZ-1500 nepouzite.
 * Bit 6 nepouzity (vsechny platformy).
 * Bit 7 = CURST - cursor blinking timer reset (0 = reset).
 */
#if MZARCH == 800
static const st_IO_BIT_DESC bits_ppi_pa[] = {
    { 0, 4, "KCOL",   "Klavesnicovy column 0..9" },
    { 4, 1, "JOY1EN", "JOY1 enable (0 = scan)" },
    { 5, 1, "JOY2EN", "JOY2 enable (0 = scan)" },
    { 7, 1, "CURST",  "Cursor blinking timer reset (0 = reset)" }
};
#else  /* MZ-700 / MZ-1500: bity 4 a 5 nepouzite */
static const st_IO_BIT_DESC bits_ppi_pa[] = {
    { 0, 4, "KCOL",   "Klavesnicovy column 0..9" },
    { 7, 1, "CURST",  "Cursor blinking timer reset (0 = reset)" }
};
#endif

/**
 * PPI 8255 Port B (0D1h IORQ na MZ-800, MMIO 0xE001 na vsech archu)
 * - klavesnicovy row data input.
 *
 * Bity 0-7 reprezentuji 8 klaves v aktualne zvolenem radku
 * (= keyboard column 0..9 vybrana v Port A LSB nibble). Aktivni-LOW:
 * bit = 0 znamena klavesa stisknuta, 1 = nestisknuta. Mapping konkretnich
 * klaves per radek viz keyboard layout dokumentace.
 *
 * Citaje to mirror snapshot - klidovy stav cache po poslednim CPU IORQ
 * scanu. Bez probe HW klavesnice (= live press nemusi byt jeste videt
 * dokud CPU neprovede dalsi IN 0D1h cyklus).
 */
static const st_IO_BIT_DESC bits_ppi_pb[] = {
    { 0, 1, "KB0", "Keyboard row bit 0 (0 = pressed)" },
    { 1, 1, "KB1", "Keyboard row bit 1 (0 = pressed)" },
    { 2, 1, "KB2", "Keyboard row bit 2 (0 = pressed)" },
    { 3, 1, "KB3", "Keyboard row bit 3 (0 = pressed)" },
    { 4, 1, "KB4", "Keyboard row bit 4 (0 = pressed)" },
    { 5, 1, "KB5", "Keyboard row bit 5 (0 = pressed)" },
    { 6, 1, "KB6", "Keyboard row bit 6 (0 = pressed)" },
    { 7, 1, "KB7", "Keyboard row bit 7 (0 = pressed)" }
};

/**
 * PPI 8255 Control word (0D3h IORQ na MZ-800, MMIO 0xE003 na vsech archu)
 * - debug mirror posledniho write byte.
 *
 * Bit layout zalezi na bit 7 hodnoty:
 *  - bit 7 = 1: Mode Set (typicky 0x8A v init sekvenci).
 *    Bity 6-5 = Group A Mode (00/01/1x = Mode 0/1/2).
 *    Bit 4 = PA dir (1 = input, 0 = output).
 *    Bit 3 = PC upper dir (1 = input, 0 = output).
 *    Bit 2 = Group B Mode (0 = Mode 0, 1 = Mode 1).
 *    Bit 1 = PB dir (1 = input, 0 = output).
 *    Bit 0 = PC lower dir (1 = input, 0 = output).
 *  - bit 7 = 0: Bit Set/Reset na PC bitu.
 *    Bity 3-1 = PC bit index (0..7).
 *    Bit 0 = hodnota (0 = reset, 1 = set).
 *
 * bits_ppi_cw popisuje primarne Mode Set layout (= castejsi pripad);
 * decode_ppi_cw vykresli interpretation textove podle bit 7.
 */
static const st_IO_BIT_DESC bits_ppi_cw[] = {
    { 0, 1, "PCL_DIR", "PC lower dir (Mode Set: 1=in, 0=out) / BSR: bit value" },
    { 1, 1, "PB_DIR",  "PB dir (Mode Set: 1=in, 0=out) / BSR: PC bit idx LSB" },
    { 2, 1, "GB_MODE", "Group B Mode (Mode Set: 0/1) / BSR: PC bit idx" },
    { 3, 1, "PCH_DIR", "PC upper dir (Mode Set: 1=in, 0=out) / BSR: PC bit idx MSB" },
    { 4, 1, "PA_DIR",  "PA dir (Mode Set only: 1=in, 0=out)" },
    { 5, 2, "GA_MODE", "Group A Mode (Mode Set only: 00=M0, 01=M1, 1x=M2)" },
    { 7, 1, "FLAG",    "1 = Mode Set, 0 = PC Bit Set/Reset" }
};

/**
 * 8253 CTC Control word (0D7h IORQ na MZ-800, MMIO 0xE007 na vsech archu)
 * - debug mirror posledniho write byte.
 *
 * Bit layout:
 *  - Bity 7:6 = SC (Counter Select): 00=CTC0, 01=CTC1, 10=CTC2, 11=Illegal
 *  - Bity 5:4 = RLF (Read/Load Format): 00=Latch, 01=LSB, 10=MSB, 11=LSB+MSB
 *  - Bity 3:1 = M (Mode): 000=Mode0, 001=Mode1, 010=Mode2, 011=Mode3,
 *                          100=Mode4, 101=Mode5, 110/111=invalid
 *  - Bit 0    = BCD: 0=Binary, 1=BCD
 *
 * Latch operace (RLF=00) je one-shot: read latch counter value pro
 * nasledny IN, nemeni mode ani format.
 */
static const st_IO_BIT_DESC bits_ctc_cw[] = {
    { 0, 1, "BCD",  "Counting mode: 0 = binary, 1 = BCD" },
    { 1, 3, "MODE", "Mode 0..5 (M2:M1:M0)" },
    { 4, 2, "RLF",  "Read/Load Format: 00=Latch, 01=LSB, 10=MSB, 11=LSB+MSB" },
    { 6, 2, "SC",   "Counter Select: 00=CTC0, 01=CTC1, 10=CTC2, 11=Illegal" }
};

/**
 * Decode 8253 CTC Control word do textoveho summary.
 *
 * @param value Mirror byte z g_ctc8253_last_cw_byte.
 * @return Static buffer ("CTC0, LSB+MSB, Mode3, Binary").
 */
static const char *decode_ctc_cw(uint8_t value)
{
    static char buf[80];
    static const char *sc_name[]   = { "CTC0", "CTC1", "CTC2", "Illegal" };
    static const char *rlf_name[]  = { "Latch", "LSB", "MSB", "LSB+MSB" };
    static const char *mode_name[] = { "Mode0", "Mode1", "Mode2", "Mode3",
                                       "Mode4", "Mode5", "Mode?", "Mode?" };
    unsigned sc   = (unsigned)((value >> 6) & 0x03);
    unsigned rlf  = (unsigned)((value >> 4) & 0x03);
    unsigned mode = (unsigned)((value >> 1) & 0x07);
    const char *bcd = (value & 0x01) ? "BCD" : "Binary";
    snprintf(buf, sizeof(buf), "%s, %s, %s, %s",
             sc_name[sc], rlf_name[rlf], mode_name[mode], bcd);
    return buf;
}

/**
 * Decode PPI 8255 Control word do textoveho summary.
 *
 * @param value Mirror byte z g_pio8255.last_cw_byte.
 * @return Static buffer ("Mode Set: ..." or "BSR: PC%d=%d").
 */
static const char *decode_ppi_cw(uint8_t value)
{
    static char buf[96];
    if (value & 0x80) {
        const char *ga_mode = (value & 0x40) ? "M2" : ((value & 0x20) ? "M1" : "M0");
        const char *pa_dir  = (value & 0x10) ? "in" : "out";
        const char *pch_dir = (value & 0x08) ? "in" : "out";
        const char *gb_mode = (value & 0x04) ? "M1" : "M0";
        const char *pb_dir  = (value & 0x02) ? "in" : "out";
        const char *pcl_dir = (value & 0x01) ? "in" : "out";
        snprintf(buf, sizeof(buf), "Mode: GA=%s,PA=%s,PCh=%s / GB=%s,PB=%s,PCl=%s",
                 ga_mode, pa_dir, pch_dir, gb_mode, pb_dir, pcl_dir);
    } else {
        unsigned bit_idx = (unsigned)((value >> 1) & 0x07);
        unsigned bit_val = (unsigned)(value & 0x01);
        snprintf(buf, sizeof(buf), "BSR: PC%u = %u", bit_idx, bit_val);
    }
    return buf;
}

/**
 * PPI 8255 Port C (0D2h IORQ na MZ-800, MMIO 0xE002 na MZ-700/1500/MZ-800
 * v 700 mode) - CMT + INT signaly + audio gate + cursor timer + VBLN.
 *
 * Bit 0 (PC0 = audio gate z CTC0) je MZ-800 specificky. Na MZ-700/MZ-1500
 * je audio z CTC0 hradlovano pres GATE0 (bit 0 portu 0xE008), nikoliv
 * pres PC0 - bit 0 PortC je zde nepouzity.
 * Bit 6 = cursor blinking timer (IN, vsechny platformy).
 * Bit 7 = VBLN signal (IN, vsechny platformy).
 */
#if MZARCH == 800
static const st_IO_BIT_DESC bits_ppi_pc[] = {
    { 0, 1, "PC0",  "OUT: blokovani audio z CTC0 (0=mute)" },
    { 1, 1, "PC1",  "OUT: data do CMT" },
    { 2, 1, "PC2",  "OUT: blokovani INT z CTC2 (0=disabled)" },
    { 3, 1, "PC3",  "OUT: rizeni motoru CMT (rising edge)" },
    { 4, 1, "PC4",  "IN:  stav motoru CMT" },
    { 5, 1, "PC5",  "IN:  data z CMT" },
    { 6, 1, "PC6",  "IN:  cursor blinking timer" },
    { 7, 1, "PC7",  "IN:  VBLN" }
};
#else  /* MZ-700 / MZ-1500: bit 0 nepouzity */
static const st_IO_BIT_DESC bits_ppi_pc[] = {
    { 1, 1, "PC1",  "OUT: data do CMT" },
    { 2, 1, "PC2",  "OUT: blokovani INT z CTC2 (0=disabled)" },
    { 3, 1, "PC3",  "OUT: rizeni motoru CMT (rising edge)" },
    { 4, 1, "PC4",  "IN:  stav motoru CMT" },
    { 5, 1, "PC5",  "IN:  data z CMT" },
    { 6, 1, "PC6",  "IN:  cursor blinking timer" },
    { 7, 1, "PC7",  "IN:  VBLN" }
};
#endif

/**
 * Z80 PIO Port A data (0FEh) - tiskárnové výstupní data + vstupy CTC0/VBLN.
 *
 * Bit interpretace závisí na nastaveni I/O masky (= Mode 3 user
 * direction), což je runtime stav. Popisy jsou indikativní.
 */
static const st_IO_BIT_DESC bits_pioz80_a[] = {
    { 0, 1, "PA0",  "Tiskarna data bit 0 (OUT)" },
    { 1, 1, "PA1",  "Tiskarna data bit 1 (OUT)" },
    { 2, 1, "PA2",  "Tiskarna data bit 2 (OUT)" },
    { 3, 1, "PA3",  "Tiskarna data bit 3 (OUT)" },
    { 4, 1, "CTC0", "Vstup z CTC0 (audio gate, IN)" },
    { 5, 1, "VBLN", "Vstup VBLN (vertical blank, IN)" },
    { 6, 1, "PA6",  "Tiskarna control (OUT)" },
    { 7, 1, "PA7",  "Tiskarna strobe (OUT)" }
};

/**
 * Z80 PIO control byte (porty 0xFC = Port A, 0xFD = Port B) - dekodovat
 * podle nejnizsich bitu byte.
 *
 * Z80 PIO control je sequencer - tentyz port prijima ruzne typy control
 * bytes podle nejnizsich bitu:
 *  - LSB == 0: Interrupt Vector (bit 7-1 = 7-bit vector)
 *  - bit[3:0] == 1111: MCW (Mode Control Word, bit 7-6 = mode 0..3)
 *  - bit[3:0] == 0111: ICW (Interrupt Control Word, MF bit + AND/OR + LVL)
 *  - bit[3:0] == 0011: IDW (Interrupt Disable Word, ENA bit)
 *  - jinak: follow-up byte (= IO mask po MCW Mode 3, INT mask po ICW MF=1)
 *
 * bits array je generic - LSBs hraje roli "type tag". Detail decode
 * provede decode_pioz80_ctrl callback.
 */
static const st_IO_BIT_DESC bits_pioz80_ctrl[] = {
    { 0, 4, "TYPE", "LSB nibble selects type: 0=Vec, 0011=IDW, 0111=ICW, 1111=MCW" },
    { 4, 4, "DATA", "MSB nibble: mode for MCW (b7:6), MF/AND/HIGH for ICW, etc." }
};

/**
 * Decode Z80 PIO control byte do textoveho summary.
 *
 * Interpretuje LSB nibble jako type tag a dodava popis dle typu:
 *  - "INT Vector 0x%02X" (= vector zachovavany v port.interrupt_vector)
 *  - "MCW: Mode <0..3 name>"
 *  - "ICW: ENA=%d, FNC=%s, LVL=%s, MF=%d"
 *  - "IDW: ENA=%d"
 *  - "Follow-up byte" (= bez plne interpretace, treba sledovat
 *    port.ctrl_expect pro kontext)
 *
 * @param value Mirror byte z g_pioz80_port_a/b_last_ctrl_byte.
 * @return Static buffer s decoded summary.
 */
static const char *decode_pioz80_ctrl(uint8_t value)
{
    static char buf[96];
    if ((value & 0x01) == 0) {
        snprintf(buf, sizeof(buf), "INT Vector 0x%02X", (unsigned)(value & 0xFE));
        return buf;
    };
    unsigned tag = (unsigned)(value & 0x0F);
    if (tag == 0x0F) {
        /* MCW: bit 7-6 = mode */
        static const char *mode_str[] = {
            "0 (Output)", "1 (Input)", "2 (Bidir)", "3 (User)"
        };
        unsigned mode = (unsigned)((value >> 6) & 0x03);
        snprintf(buf, sizeof(buf), "MCW: Mode %s", mode_str[mode]);
    } else if (tag == 0x07) {
        /* ICW: bit 7 = ENA, bit 6 = AND/OR, bit 5 = HIGH/LOW, bit 4 = MF */
        unsigned ena = (unsigned)((value >> 7) & 0x01);
        const char *fnc = (value & 0x40) ? "AND" : "OR";
        const char *lvl = (value & 0x20) ? "HIGH" : "LOW";
        unsigned mf  = (unsigned)((value >> 4) & 0x01);
        snprintf(buf, sizeof(buf), "ICW: ENA=%u, FNC=%s, LVL=%s, MF=%u",
                 ena, fnc, lvl, mf);
    } else if (tag == 0x03) {
        unsigned ena = (unsigned)((value >> 7) & 0x01);
        snprintf(buf, sizeof(buf), "IDW: ENA=%u", ena);
    } else {
        snprintf(buf, sizeof(buf), "Follow-up byte (IO/INT mask)");
    }
    return buf;
}

#if CFG_HWEXT_HAVE_FDC
/**
 * FDC Motor / Drive select latch (port 0xDC) - bit popisky.
 *
 * Bit 0-1 = drive ID (0..3 mechanika; aplikuje se jen pri bit 2 = EDR = 1).
 * Bit 2 = EDR (Enable Drive change): 1 = bity 0-1 se zapisi, 0 = drive ID
 *         se nemeni.
 * Bity 3-6 = unused.
 * Bit 7 = motor on/off (1 = motor pracuje).
 */
static const st_IO_BIT_DESC bits_fdc_motor[] = {
    { 0, 2, "DRV",   "Drive ID 0..3 (only when EDR=1)" },
    { 2, 1, "EDR",   "Enable Drive change (1 = bits 0-1 are written)" },
    { 7, 1, "MOTOR", "Motor on/off (1 = on)" }
};

/**
 * FDC Side select latch (port 0xDD) - bit popisky.
 *
 * Bit 0 = side select. Sharp BUS xlate (= invert) je aplikovan pred
 * zapisem do chip latch, takze field hodnota odpovida finalnimu HW signalu.
 * Bity 1-7 = unused.
 */
static const st_IO_BIT_DESC bits_fdc_side[] = {
    { 0, 1, "SIDE", "Side select (0 = side A, 1 = side B)" }
};

/**
 * FDC Density latch (port 0xDE) - bit popisky.
 *
 * Bit 0 = density. Bity 1-7 = unused.
 */
static const st_IO_BIT_DESC bits_fdc_density[] = {
    { 0, 1, "DEN", "Density (0 = single, 1 = double)" }
};

/**
 * FDC HD Patch EINT latch (port 0xDF) - bit popisky.
 *
 * Bit 0 = EINT (interrupt enable). Aktivni jen pokud g_fdc[FDC0].hd_patch == 1
 * (= HD Patch obvod je osazen). Bez HD Patch porta 0xDF neexistuje na HW
 * urovni, ale latch v emu se stale zaznamenava.
 */
static const st_IO_BIT_DESC bits_fdc_eint[] = {
    { 0, 1, "EINT", "Interrupt enable (only when g_fdc[FDC0].hd_patch == 1)" }
};
#endif /* CFG_HWEXT_HAVE_FDC */

/* ===== Memory bank state (g_memory.map) per-arch bit popisy ============ */
/* g_memory.map je per-arch bitfield reprezentující současný Sharp banking
 * stav. Banking porty 0xE0-0xE6 nemají vlastní hodnotu, ale dispatchují
 * na changes tohoto byte. UI v Overview tabu zobrazí pro každý banking
 * port stejnou hodnotu (= global state, ne per-port). Bit význam se liší
 * mezi platformami - každá architektura má vlastní bits_memory_map_<arch>
 * array.
 *
 * Reference: mz{arch}_memory.h MEMORY_MZ{ARCH}_MAP_FLAG_*. */

#if MZARCH == 800
static const st_IO_BIT_DESC bits_memory_map_mz800[] = {
    { 0, 1, "ROM_0000", "ROM mapped at 0x0000-0x0FFF (Monitor)" },
    { 1, 1, "ROM_1000", "ROM mapped at 0x1000-0x1FFF (CG-ROM in 800 native, depends on GDG.DMD bit 3)" },
    { 2, 1, "CGRAM_VRAM", "MZ-700 mode: CGRAM at C000-CFFF; MZ-800 native: VRAM at 8000-9FFF/BFFF (depends on GDG.DMD bit 3)" },
    { 3, 1, "ROM_E000", "ROM at E000-FFFF (+ in MZ-700 mode also VRAM at D000-DFFF)" },
    { 4, 1, "PROHIBITED", "Prohibited mode (OUT 0xE5 set, OUT 0xE6 clear). Reads E009-FFFF return 0x1A shadow." },
    { 5, 1, "-", "unused" },
    { 6, 1, "-", "unused" },
    { 7, 1, "-", "unused" }
};
#endif

#if MZARCH == 700
static const st_IO_BIT_DESC bits_memory_map_mz700[] = {
    { 0, 1, "ROM_0000", "ROM mapped at 0x0000-0x0FFF (Monitor)" },
    { 1, 1, "ROM_E000", "ROM at E000-FFFF + VRAM text at D000-DFFF, mapped IO ports E000-E00F" },
    { 2, 1, "PROHIBITED", "Prohibited mode (OUT 0xE5 set, OUT 0xE6 clear). E800-FFFF detached, reads return 0xFF." },
    { 3, 1, "-", "unused" },
    { 4, 1, "-", "unused" },
    { 5, 1, "-", "unused" },
    { 6, 1, "-", "unused" },
    { 7, 1, "-", "unused" }
};
#endif

#if MZARCH == 1500
static const st_IO_BIT_DESC bits_memory_map_mz1500[] = {
    { 0, 1, "ROM_0000", "ROM mapped at 0x0000-0x0FFF (Monitor)" },
    { 1, 1, "ROM_UPPER", "Upper area: ROM at E800-FFFF + VRAM text D000 + mapped IO ports E000-E00F" },
    /* SPEC = 3-bit field at bits 2-4 (D000-EFFF special mapping). decode_mz1500_spec(). */
    { 2, 3, "SPEC", "D000-EFFF special mapping selector (0=None, 1=CGROM, 2=PCG1, 3=PCG2, 4=PCG3)" },
    { 5, 1, "-", "unused" },
    { 6, 1, "-", "unused" },
    { 7, 1, "-", "unused" }
};

/* Decode pro celý g_memory.map byte - extrahuje SPEC bit-field a delegate
 * na decode_mz1500_spec. Použije se v io_catalog entries jako `decode`. */
static const char *decode_mz1500_memory_map(uint8_t value)
{
    /* Vrátí jen popis SPEC pole - ROM_0000 a ROM_UPPER jsou single bity
     * pokryté `bits` array, nemají hodnotu pro decode. */
    return decode_mz1500_spec((value >> MEMORY_MZ1500_FLAG_SPEC_BITPOS) & 0x7u);
}
#endif

/* ========================================================================= */
/*  Hlavní katalog g_io_ports[]                                              */
/* ========================================================================= */

/**
 * Statický katalog I/O portů. Pořadí cca podle adresy, s GDG bloky
 * pohromadě a banking jako samostatná sekce.
 */
const st_IO_PORT_DESC g_io_ports[] = {
#if ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 )
    /* ===== GDG video registry (8-bit IORQ) - MZ-800 jen =====
     * MZ-1500 a MZ-700 nemaji GDG registry na IORQ 0xCC-0xCE.
     * (Na MZ-1500 jsou DMD/paleta na IORQ 0xF0/F1, na MZ-700 nativne ditto.) */
    {
        0x00CC, "GDG - WF (W)", "GDG Write Format register (write-only)",
        IO_PORT_DIR_W,
        bits_gdg_wf, sizeof(bits_gdg_wf) / sizeof(bits_gdg_wf[0]),
        read_gdg_wf, decode_wf,
        MZ_AVAIL_800
    },
    {
        0x00CD, "GDG - RF (W)", "GDG Read Format register (write-only)",
        IO_PORT_DIR_W,
        bits_gdg_rf, sizeof(bits_gdg_rf) / sizeof(bits_gdg_rf[0]),
        read_gdg_rf, NULL,
        MZ_AVAIL_800
    },
    /* 0CEh OUT = DMD (Display Mode), IN = Status - oddělené entries */
    {
        0x00CE, "GDG - DMD (W)", "Display Mode register (write)",
        IO_PORT_DIR_W,
        bits_gdg_dmd, sizeof(bits_gdg_dmd) / sizeof(bits_gdg_dmd[0]),
        read_gdg_dmd, decode_dmd,
        MZ_AVAIL_800
    },
    {
        0x00CE, "GDG - Status (R)", "Status register (read): VBLN/HBLN/VSY/HSY/TEMPO/CKSW/SW1",
        IO_PORT_DIR_R,
        bits_gdg_status, sizeof(bits_gdg_status) / sizeof(bits_gdg_status[0]),
        read_gdg_status, NULL,
        MZ_AVAIL_800
    },

#if MZARCH == 800
    /* ===== 0xCF 16-bit CRTC family (write-only) =====
     * Bus addr je 0xRRCF (B=register index, C=0xCF). MZ notace 0xCF<RR>.
     * UI panel zobrazuje MZ notaci 0xCF01..0xCF07. IORQ_W BP integrace
     * vyzaduje port_mode = BP_PORT_16BIT (V1.5.A7). MZ-1500 nema HW scroll
     * ani 0xCF family, proto entries jsou MZ_AVAIL_800 only. */
    {
        0xCF01, "GDG - SOF0 (W)", "Scroll offset low 8 bits (16-bit IORQ, bus 0x01CF)",
        IO_PORT_DIR_W,
        bits_gdg_sof0, sizeof(bits_gdg_sof0) / sizeof(bits_gdg_sof0[0]),
        read_gdg_sof0, NULL,
        MZ_AVAIL_800
    },
    {
        0xCF02, "GDG - SOF1 (W)", "Scroll offset upper 2 bits (16-bit IORQ, bus 0x02CF)",
        IO_PORT_DIR_W,
        bits_gdg_sof1, sizeof(bits_gdg_sof1) / sizeof(bits_gdg_sof1[0]),
        read_gdg_sof1, NULL,
        MZ_AVAIL_800
    },
    {
        0xCF03, "GDG - SW (W)", "Scroll width (16-bit IORQ, bus 0x03CF)",
        IO_PORT_DIR_W,
        bits_gdg_sw, sizeof(bits_gdg_sw) / sizeof(bits_gdg_sw[0]),
        read_gdg_sw, NULL,
        MZ_AVAIL_800
    },
    {
        0xCF04, "GDG - SSA (W)", "Scroll start address (16-bit IORQ, bus 0x04CF)",
        IO_PORT_DIR_W,
        bits_gdg_ssa, sizeof(bits_gdg_ssa) / sizeof(bits_gdg_ssa[0]),
        read_gdg_ssa, NULL,
        MZ_AVAIL_800
    },
    {
        0xCF05, "GDG - SEA (W)", "Scroll end address (16-bit IORQ, bus 0x05CF)",
        IO_PORT_DIR_W,
        bits_gdg_sea, sizeof(bits_gdg_sea) / sizeof(bits_gdg_sea[0]),
        read_gdg_sea, NULL,
        MZ_AVAIL_800
    },
    {
        0xCF06, "GDG - BCOL (W)", "Border color (16-bit IORQ, bus 0x06CF)",
        IO_PORT_DIR_W,
        bits_gdg_bcol, sizeof(bits_gdg_bcol) / sizeof(bits_gdg_bcol[0]),
        read_gdg_bcol, decode_bcol,
        MZ_AVAIL_800
    },
    {
        0xCF07, "GDG - CKSW (W)", "Superimpose enable (16-bit IORQ, bus 0x07CF)",
        IO_PORT_DIR_W,
        bits_gdg_cksw, sizeof(bits_gdg_cksw) / sizeof(bits_gdg_cksw[0]),
        read_gdg_cksw, decode_cksw,
        MZ_AVAIL_800
    },
#endif /* MZARCH == 800 - 0xCF sub-registry */

    /* 0xF0/F1 - per-arch ruzna semantika:
     *  MZ-800:  0xF0 W = GDG palette + 0xF0 R = JOY0, 0xF1 R = JOY1
     *  MZ-1500: 0xF0 W = GDG DMD register, 0xF1 W = GDG paleta (oba write-only)
     *  MZ-700:  0xF0 W = GDG DMD register, 0xF1 W = GDG paleta (oba write-only)
     * Joystick na MZ-1500/MZ-700 jde pres 8255 PPI, ne na techto portech. */

    /* MZ-800: 0xF0 OUT = paleta GDG, IN = JOY0 */
    {
        0x00F0, "GDG - Palette (W)", "Palette / palette group register (write-only)",
        IO_PORT_DIR_W,
        NULL, 0,
        read_gdg_palette, NULL,
        MZ_AVAIL_800
    },
    {
        /* JOY0 - mirror přes joy_read_byte() (side-effect free probe).
         * Respektuje PPI Port A signal_PA_joy1_enabled gate; pokud
         * disabled, mirror vrátí 0xFF (= shodné s reálným IORQ chováním
         * v port_read_no_se_cb / mz800_iorq.c). Bity active LOW. */
        0x00F0, "JOY0 (R)", "Joystick 0 input (read; mirror via joy_read_byte)",
        IO_PORT_DIR_R,
        bits_joy, sizeof(bits_joy) / sizeof(bits_joy[0]),
        read_joy0, NULL,
        MZ_AVAIL_800
    },
    {
        0x00F1, "JOY1 (R)", "Joystick 1 input (read; mirror via joy_read_byte)",
        IO_PORT_DIR_R,
        bits_joy, sizeof(bits_joy) / sizeof(bits_joy[0]),
        read_joy1, NULL,
        MZ_AVAIL_800
    },

    /* MZ-1500/MZ-700: 0xF0 W (GDG DMD) a 0xF1 W (GDG paleta) jsou v emu
     * implementovane (mz1500_iorq.c / mz700_iorq.c volaji gdg_write_byte),
     * ale do I/O Overview je nepridavame - aby se "GDG" sekce zcela skryla
     * na MZ-1500/MZ-700 (user request: "nema GDG, odstranit"). */
#endif

    /* ===== Memory banking (0E0h-0E6h) + MemExt (0E7h) =====
     * 0E0h-0E6h jsou Sharp banking porty (GDG dispatch, RAM/ROM/VRAM/CG-ROM
     * mapping v adresním prostoru CPU). NEJSOU součástí MemExt rozšíření -
     * to je oddělená HW karta na portu 0E7h. Banking hodnota IORQ je
     * irelevantní (= dispatch jen na adresu).
     *
     * V1.7+ 2.9 (Banking decoded display): všech 7 entries 0E0h-0E6h má
     * `read_value = read_memory_map_byte` (= mirror `g_memory.map` byte) +
     * per-arch `bits` array s popisky flagů. UI v Overview tabu pro každý
     * banking port zobrazí stejnou hex hodnotu (= jeden global state byte,
     * ne per-port). Detail panel pro plný mapping přehled je Memory Map
     * window (= cross-window, click na řádek otevře/scrolluje Memory Map).
     *
     * MZ-800 specifika: bit 2 (CGRAM_VRAM) význam závisí na g_gdg.regDMD
     * bitu 3 (MZ-700 compat mode vs MZ-800 native mode). Popisek bitu
     * pokrývá oba významy. Pro plný kontext otevřít Memory Map okno.
     *
     * MZ-1500 specifika: bity 2-4 = SPEC 3-bit pole (D000-EFFF special
     * mapping selector). `decode` callback ho překládá na symbolický
     * název.
     *
     * Reference: mz800-knowledge reference/agent/hw/03-banking.md
     * (Sharp banking via GDG, porty 0E0h-0E6h) a hw/23-memext.md (MemExt
     * 64 kB -> 512 kB SRAM karta, port 0E7h). */

#if MZARCH == 800
#define MZ_BITS_MEMORY_MAP bits_memory_map_mz800, sizeof(bits_memory_map_mz800)/sizeof(bits_memory_map_mz800[0])
#define MZ_DECODE_MEMORY_MAP NULL
#elif MZARCH == 700
#define MZ_BITS_MEMORY_MAP bits_memory_map_mz700, sizeof(bits_memory_map_mz700)/sizeof(bits_memory_map_mz700[0])
#define MZ_DECODE_MEMORY_MAP NULL
#elif MZARCH == 1500
#define MZ_BITS_MEMORY_MAP bits_memory_map_mz1500, sizeof(bits_memory_map_mz1500)/sizeof(bits_memory_map_mz1500[0])
#define MZ_DECODE_MEMORY_MAP decode_mz1500_memory_map
#else
#define MZ_BITS_MEMORY_MAP NULL, 0
#define MZ_DECODE_MEMORY_MAP NULL
#endif

    {
        0x00E0, "Memory bank - E0 (R/W)", "Sharp banking via GDG (state mirror = g_memory.map byte)",
        IO_PORT_DIR_RW,
        MZ_BITS_MEMORY_MAP,
        read_memory_map_byte, MZ_DECODE_MEMORY_MAP,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },
    {
        0x00E1, "Memory bank - E1 (R/W)", "Sharp banking via GDG (state mirror = g_memory.map byte)",
        IO_PORT_DIR_RW,
        MZ_BITS_MEMORY_MAP,
        read_memory_map_byte, MZ_DECODE_MEMORY_MAP,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },
    {
        0x00E2, "Memory bank - E2 (R/W)", "Sharp banking via GDG (state mirror = g_memory.map byte)",
        IO_PORT_DIR_RW,
        MZ_BITS_MEMORY_MAP,
        read_memory_map_byte, MZ_DECODE_MEMORY_MAP,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },
    {
        0x00E3, "Memory bank - E3 (R/W)", "Sharp banking via GDG (state mirror = g_memory.map byte)",
        IO_PORT_DIR_RW,
        MZ_BITS_MEMORY_MAP,
        read_memory_map_byte, MZ_DECODE_MEMORY_MAP,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },
    {
        0x00E4, "Memory bank - E4 reset (R/W)", "Sharp banking reset to default mapping (state mirror = g_memory.map byte)",
        IO_PORT_DIR_RW,
        MZ_BITS_MEMORY_MAP,
        read_memory_map_byte, MZ_DECODE_MEMORY_MAP,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },
    {
        0x00E5, "Memory bank - E5 (W)", "Sharp banking - Prohibited mode (state mirror = g_memory.map byte)",
        IO_PORT_DIR_W,
        MZ_BITS_MEMORY_MAP,
        read_memory_map_byte, MZ_DECODE_MEMORY_MAP,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },
    {
        0x00E6, "Memory bank - E6 (W)", "Sharp banking - clear Prohibited (state mirror = g_memory.map byte)",
        IO_PORT_DIR_W,
        MZ_BITS_MEMORY_MAP,
        read_memory_map_byte, MZ_DECODE_MEMORY_MAP,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },
    {
        0x00E7, "Memory ext - MEMEXT bank (W)", "Memory expansion (memext) bank select - per-page state in g_memext.map[16], see Memory Map window",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_ALL,
        IO_CROSS_MEMORY_MAP
    },

    /* ===== 8255 PPI IORQ (0D0h-0D3h) - jen MZ-800 =====
     * MZ-1500 a MZ-700 nemaji PPI na IORQ, pristup je pres MMIO 0xE000-0xE003. */
    {
        0x00D0, "8255 PPI - Port A (R/W)", "Klavesnicovy column select + JOY enable",
        IO_PORT_DIR_RW,
        bits_ppi_pa, sizeof(bits_ppi_pa) / sizeof(bits_ppi_pa[0]),
        read_ppi_pa, NULL,
        MZ_AVAIL_800
    },
    {
        /* Port B = keyboard row data input. Real read má side-effect
         * (= iface_keyboard_pool_keyboard_events probe + vkbd autotype
         * state update). Mirror čte klidový snapshot keyboard_matrix[col]
         * & vkbd_matrix[col] bez probe - reflektuje co bylo cached při
         * posledním CPU IORQ na 0xD1. Konvence dle memory
         * feedback_io_mirror_pending_bits. */
        0x00D1, "8255 PPI - Port B (R/W)", "Klavesnicovy row data (keyboard scan)",
        IO_PORT_DIR_RW,
        bits_ppi_pb, sizeof(bits_ppi_pb) / sizeof(bits_ppi_pb[0]),
        read_ppi_pb, NULL,
        MZ_AVAIL_800
    },
    {
        0x00D2, "8255 PPI - Port C (R/W)", "CMT data + audio gate + INT mask",
        IO_PORT_DIR_RW,
        bits_ppi_pc, sizeof(bits_ppi_pc) / sizeof(bits_ppi_pc[0]),
        read_ppi_pc, NULL,
        MZ_AVAIL_800
    },
    {
        /* 8255 control word je sequencer - HW nema readable register.
         * Mirror zachycen z IORQ write path (g_pio8255.last_cw_byte)
         * pro debug zobrazeni. Default 0x00 dokud ROM init nenapisi
         * 0x8A (Mode Set: PA out, PCh in, PB in, PCl out). */
        0x00D3, "8255 PPI - Control (W)", "8255 Control word (mode select / Bit Set-Reset)",
        IO_PORT_DIR_W,
        bits_ppi_cw, sizeof(bits_ppi_cw) / sizeof(bits_ppi_cw[0]),
        read_ppi_cw, decode_ppi_cw,
        MZ_AVAIL_800
    },

    /* ===== 8253 CTC IORQ (0D4h-0D7h) - jen MZ-800 =====
     * read_ctcN_value čte g_ctc8253[N].value (= aktuální countdown hodnota,
     * LSB). Side-effect free direct read interní structuralní hodnoty,
     * NE přes ctc8253_read_byte (= mohl by spustit latch operaci).
     * MZ-1500 a MZ-700 nemaji CTC na IORQ, pristup je pres MMIO 0xE004-0xE007. */
    {
        0x00D4, "8253 CTC - Counter 0 (R/W)", "Audio output counter (gate via PPI PC0)",
        IO_PORT_DIR_RW,
        NULL, 0,
        read_ctc0_value, decode_ctc0_value,
        MZ_AVAIL_800
    },
    {
        0x00D5, "8253 CTC - Counter 1 (R/W)", "HSYNC divider counter",
        IO_PORT_DIR_RW,
        NULL, 0,
        read_ctc1_value, decode_ctc1_value,
        MZ_AVAIL_800
    },
    {
        0x00D6, "8253 CTC - Counter 2 (R/W)", "INT divider counter (50Hz frame INT)",
        IO_PORT_DIR_RW,
        NULL, 0,
        read_ctc2_value, decode_ctc2_value,
        MZ_AVAIL_800
    },
    {
        /* CTC control word je sequencer - HW nema readable register.
         * Mirror z g_ctc8253_last_cw_byte zachycen v IORQ write path
         * (ctc8253_write_byte) pro debug zobrazeni. Default 0x00 do
         * prvniho HW init writu. */
        0x00D7, "8253 CTC - Control (W)", "8253 Control word (counter select + Read/Load Format + Mode + BCD)",
        IO_PORT_DIR_W,
        bits_ctc_cw, sizeof(bits_ctc_cw) / sizeof(bits_ctc_cw[0]),
        read_ctc_cw, decode_ctc_cw,
        MZ_AVAIL_800
    },

    /* ===== Mem-mapped IO (0xE000-0xE008) =====
     * V1.5.E: Memory-mapped pristup k PPI/CTC/GDG v ROM space.
     *
     *  - Na MZ-800/MZ-1500: aktivni jen v MZ-700 compat modu (banking E2/E0
     *    => 0xE000-0xFFFF mapped jako MZ-700 monitor/charROM s embedded IO).
     *    Pri CPU LD A,(addr) / LD (addr),A v rozsahu 0xE000-0xE008 jdou data
     *    pres memory_*_with_logging_cb a internal_read/write_e000_efff_sync
     *    ktere routuje na pio8255 / ctc8253 / gdg jak by sla IORQ.
     *  - Na MZ-700: NATIVNI (jediny zpusob pristupu, IORQ 0xD0-0xD7 neexistuje).
     *
     * Direction = RW (= memory cell vs IO sequencer): UI render zobrazi
     * R/W ridici per MR/MW event. read_value pro Overview cte stejny
     * mirror jako primarni IORQ entry (= side-effect free). */
    {
        0x00E000, "MMIO - PPI Port A (R/W)", "Memory-mapped 8255 PPI Port A (klavesnicovy column select + JOY enable)",
        IO_PORT_DIR_RW,
        bits_ppi_pa, sizeof(bits_ppi_pa) / sizeof(bits_ppi_pa[0]),
        read_ppi_pa, NULL,
        MZ_AVAIL_ALL
    },
    {
        /* PPI Port B - keyboard row scan; klidovy mirror snapshot
         * (= keyboard_matrix[col] & vkbd_matrix[col]) bez probe.
         * Stejny callback jako primarni IORQ entry 0xD1. */
        0x00E001, "MMIO - PPI Port B (R/W)", "Memory-mapped 8255 PPI Port B (klavesnicovy row data)",
        IO_PORT_DIR_RW,
        bits_ppi_pb, sizeof(bits_ppi_pb) / sizeof(bits_ppi_pb[0]),
        read_ppi_pb, NULL,
        MZ_AVAIL_ALL
    },
    {
        0x00E002, "MMIO - PPI Port C (R/W)", "Memory-mapped 8255 PPI Port C (CMT data + audio gate + INT mask)",
        IO_PORT_DIR_RW,
        bits_ppi_pc, sizeof(bits_ppi_pc) / sizeof(bits_ppi_pc[0]),
        read_ppi_pc, NULL,
        MZ_AVAIL_ALL
    },
    {
        /* PPI Control - sequencer; mirror z g_pio8255.last_cw_byte.
         * Stejny callback jako IORQ entry 0xD3. */
        0x00E003, "MMIO - PPI Control (W)", "Memory-mapped 8255 PPI Control word (mode select / Bit Set-Reset)",
        IO_PORT_DIR_W,
        bits_ppi_cw, sizeof(bits_ppi_cw) / sizeof(bits_ppi_cw[0]),
        read_ppi_cw, decode_ppi_cw,
        MZ_AVAIL_ALL
    },
    {
        0x00E004, "MMIO - CTC Counter 0 (R/W)", "Memory-mapped 8253 CTC Counter 0 (audio output)",
        IO_PORT_DIR_RW,
        NULL, 0,
        read_ctc0_value, decode_ctc0_value,
        MZ_AVAIL_ALL
    },
    {
        0x00E005, "MMIO - CTC Counter 1 (R/W)", "Memory-mapped 8253 CTC Counter 1 (HSYNC divider)",
        IO_PORT_DIR_RW,
        NULL, 0,
        read_ctc1_value, decode_ctc1_value,
        MZ_AVAIL_ALL
    },
    {
        0x00E006, "MMIO - CTC Counter 2 (R/W)", "Memory-mapped 8253 CTC Counter 2 (INT divider)",
        IO_PORT_DIR_RW,
        NULL, 0,
        read_ctc2_value, decode_ctc2_value,
        MZ_AVAIL_ALL
    },
    {
        /* CTC Control - sequencer; mirror z g_ctc8253_last_cw_byte.
         * Stejny callback jako IORQ entry 0xD7. */
        0x00E007, "MMIO - CTC Control (W)", "Memory-mapped 8253 CTC Control word (counter select + RLF + Mode + BCD)",
        IO_PORT_DIR_W,
        bits_ctc_cw, sizeof(bits_ctc_cw) / sizeof(bits_ctc_cw[0]),
        read_ctc_cw, decode_ctc_cw,
        MZ_AVAIL_ALL
    },
    {
        /* 0xE008 OUT = CTC0 GATE0 (bit 0) - NENI DMD! Zapis jde do
         * gdg_write_byte case 0x08, kde se z hodnoty bere jen bit 0 a ridi
         * se jim hradlovani CTC0 (ctc8253_gate(0, value & 1)) = audio gate
         * (viz pozn. u bits_pio8255_portc bit 0). DMD registr je IORQ 0xCE.
         * 0xE008 IN  = GDG status mirror (HBLNK + TEMPO + na MZ-800
         * navic CKSW; na MZ-700/1500 bity 1-4 mohou byt JOY pres joymz).
         * Sdili bits_gdg_status[] s 0xCE Status pro UI bit detail (IN). */
        0x00E008, "MMIO - CTC0 GATE0 / GDG Status (R/W)", "Memory-mapped CTC0 GATE0 bit 0 (W) / GDG Status (R)",
        IO_PORT_DIR_RW,
        bits_gdg_status, sizeof(bits_gdg_status) / sizeof(bits_gdg_status[0]),
        read_mmio_e008_status, NULL,
        MZ_AVAIL_ALL
    },

    /* ===== FDC WD279x (0D8h-0DBh + write mirror DC-DF) =====
     * V1.7+ 2.3 (WD279x.status mirror): Status (0xD8 R), Track (0xD9 R) a
     * Sector (0xDA R) napojeny na side-effect free `wd279x_mirror_*_get()`
     * API z fdc/wd279x.c. Mirror funkce NEVYVOLÁVAJÍ side-effecty reálné
     * IORQ READ cesty (pending_busy_status, pending_drq, reading_status_
     * counter, INTRQ reset) - zobrazují "klidový" stav chipu.
     *
     * Data registr (0xDB) NEMÁ mirror - reálný read posouvá buffer stream
     * a multi-block sector transition; UI ponechává "??" pro D registr. */
    {
        0x00D8, "FDC - Status / Command (R/W)", "WD279x status (R) / command (W)",
        IO_PORT_DIR_RW,
        NULL, 0,
#if CFG_HWEXT_HAVE_FDC
        read_fdc_status, NULL,
#else
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x00D9, "FDC - Track (R/W)", "WD279x track register",
        IO_PORT_DIR_RW,
        NULL, 0,
#if CFG_HWEXT_HAVE_FDC
        read_fdc_track, NULL,
#else
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x00DA, "FDC - Sector (R/W)", "WD279x sector register",
        IO_PORT_DIR_RW,
        NULL, 0,
#if CFG_HWEXT_HAVE_FDC
        read_fdc_sector, NULL,
#else
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x00DB, "FDC - Data (R/W)", "WD279x data register (Sharp invertuje data + side)",
        IO_PORT_DIR_RW,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_ALL
    },

    /* ===== FDC Sharp external logic (0xDC-0xDF) - write only =====
     * Tyto porty nejsou součástí WD279x chipu, ale externí Sharp logiky
     * pro řízení mechaniky. BUS xlate inverze se na ně neaplikuje
     * (jen na chip porty 0..3). Reference: wd279x_internal.h
     * en_FDCPORT_OFFSET (FDCPORT_MOTOR..FDCPORT_EINT). */
    {
        0x00DC, "FDC - Motor / Drive select (W)",
        "Motor on/off + drive select (Sharp external logic, not WD279x); mirror from g_fdc[FDC0].wd279x.MOTOR latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_motor, sizeof(bits_fdc_motor) / sizeof(bits_fdc_motor[0]),
        read_fdc_motor, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x00DD, "FDC - Side (W)",
        "Side select (Sharp inverts value vs standard logic); mirror from g_fdc[FDC0].wd279x.SIDE latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_side, sizeof(bits_fdc_side) / sizeof(bits_fdc_side[0]),
        read_fdc_side, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x00DE, "FDC - Density (W)",
        "Density select (single/double); mirror from g_fdc[FDC0].wd279x.DENSITY latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_density, sizeof(bits_fdc_density) / sizeof(bits_fdc_density[0]),
        read_fdc_density, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x00DF, "FDC - HD Patch EINT (W)",
        "HD Patch external interrupt enable (Sharp HD/PATCH mod); mirror from g_fdc[FDC0].wd279x.EINT latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_eint, sizeof(bits_fdc_eint) / sizeof(bits_fdc_eint[0]),
        read_fdc_eint, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },

    /* ===== FDC1 (sekundární) 0x58-0x5F - Unicard-suppressed řadič =====
     * Stejný layout jako FDC0 (0xD8-0xDF), jen na portech 0x58-0x5F.
     * Mirror gettery čtou g_fdc[FDC1]. Bit deskriptory sdílené s FDC0. */
    {
        0x0058, "FDC1 - Status / Command (R/W)", "WD279x status (R) / command (W) - sekundarni FDC",
        IO_PORT_DIR_RW,
        NULL, 0,
#if CFG_HWEXT_HAVE_FDC
        read_fdc1_status, NULL,
#else
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x0059, "FDC1 - Track (R/W)", "WD279x track register - sekundarni FDC",
        IO_PORT_DIR_RW,
        NULL, 0,
#if CFG_HWEXT_HAVE_FDC
        read_fdc1_track, NULL,
#else
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x005A, "FDC1 - Sector (R/W)", "WD279x sector register - sekundarni FDC",
        IO_PORT_DIR_RW,
        NULL, 0,
#if CFG_HWEXT_HAVE_FDC
        read_fdc1_sector, NULL,
#else
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x005B, "FDC1 - Data (R/W)", "WD279x data register (Sharp invertuje data + side) - sekundarni FDC",
        IO_PORT_DIR_RW,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_ALL
    },
    {
        0x005C, "FDC1 - Motor / Drive select (W)",
        "Motor on/off + drive select (Sharp external logic, not WD279x); mirror from g_fdc[FDC1].wd279x.MOTOR latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_motor, sizeof(bits_fdc_motor) / sizeof(bits_fdc_motor[0]),
        read_fdc1_motor, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x005D, "FDC1 - Side (W)",
        "Side select (Sharp inverts value vs standard logic); mirror from g_fdc[FDC1].wd279x.SIDE latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_side, sizeof(bits_fdc_side) / sizeof(bits_fdc_side[0]),
        read_fdc1_side, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x005E, "FDC1 - Density (W)",
        "Density select (single/double); mirror from g_fdc[FDC1].wd279x.DENSITY latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_density, sizeof(bits_fdc_density) / sizeof(bits_fdc_density[0]),
        read_fdc1_density, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },
    {
        0x005F, "FDC1 - HD Patch EINT (W)",
        "HD Patch external interrupt enable (Sharp HD/PATCH mod); mirror from g_fdc[FDC1].wd279x.EINT latch",
        IO_PORT_DIR_W,
#if CFG_HWEXT_HAVE_FDC
        bits_fdc_eint, sizeof(bits_fdc_eint) / sizeof(bits_fdc_eint[0]),
        read_fdc1_eint, NULL,
#else
        NULL, 0,
        NULL, NULL,
#endif
        MZ_AVAIL_ALL
    },

    /* ===== PSG (per-arch ruzny port layout) - all write only =====
     * SN76489 nema readable status, UI zobrazi "??".
     *
     * MZ-800: 0xF2 = mono (nebo oba pri allow_psg1), 0xF3 = stereo left
     *               (jen pri allow_psg1), 0xF9 = stereo right (jen pri allow_psg1)
     * MZ-1500: 0xE9 = mono/oba (fallback), 0xF2 = left, 0xF3 = right
     * MZ-700: bez PSG */

    /* MZ-800 PSG entries */
    {
        0x00F2, "PSG - SN76489 (W)", "SN76489AN sound generator (mono / stereo right)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_800
    },
    {
        0x00F3, "PSG - Stereo left (W)", "Druhy PSG channel left (jen pri allow_psg1 stereo)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_800
    },
    {
        0x00F9, "PSG - Stereo right (W)", "Druhy PSG channel right (jen pri allow_psg1 stereo)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_800
    },

    /* MZ-1500 PSG entries (jiny port layout - native stereo)
     *
     * Pouzivame distinkcni prefix "PSG stereo -" misto MZ-800 "PSG -",
     * aby UI io_window.cpp dokazalo zaradit entries do samostatne sekce
     * "PSG (stereo SN76489 x2)" (viz g_sections v io_window.cpp).
     * V MZ-800/MZ-700 buildech se MZ-1500 sekce nezobrazi (sec_visible==0
     * diky filtru available_for_mzarch). */
    {
        0x00E9, "PSG stereo - Mono fallback (W)", "SN76489 obe kanaly (write to both - mono fallback)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_1500
    },
    {
        0x00F2, "PSG stereo - Left (W)", "SN76489 levy kanal (PSG0)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_1500
    },
    {
        0x00F3, "PSG stereo - Right (W)", "SN76489 pravy kanal (PSG1)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_1500
    },

    /* ===== Z80 PIO (0FCh-0FFh) =====
     * MZ-800 a MZ-1500 maji Z80 PIO. MZ-700 ne (HAVE_PIOZ80=0).
     * Control registry 0xFC/0xFD jsou sequencer (Mode + IO mask + ICW),
     * ne single 8-bit register. HW samotne nelze precist; mirror je
     * cache posledniho write byte v g_pioz80_port_a/b_last_ctrl_byte
     * (tracking v pioz80_write_byte). Decode rozhodne podle LSB nibble. */
    {
        0x00FC, "Z80 PIO - Port A control (R/W)", "Z80 PIO control register A (MCW / ICW / Vector / follow-up byte)",
        IO_PORT_DIR_RW,
        bits_pioz80_ctrl, sizeof(bits_pioz80_ctrl) / sizeof(bits_pioz80_ctrl[0]),
        read_pioz80_a_ctrl, decode_pioz80_ctrl,
        MZ_AVAIL_800 | MZ_AVAIL_1500
    },
    {
        0x00FD, "Z80 PIO - Port B control (R/W)", "Z80 PIO control register B (MCW / ICW / Vector / follow-up byte)",
        IO_PORT_DIR_RW,
        bits_pioz80_ctrl, sizeof(bits_pioz80_ctrl) / sizeof(bits_pioz80_ctrl[0]),
        read_pioz80_b_ctrl, decode_pioz80_ctrl,
        MZ_AVAIL_800 | MZ_AVAIL_1500
    },
    {
        0x00FE, "Z80 PIO - Port A data (R/W)", "Tiskarna data + CTC0/VBLN inputs",
        IO_PORT_DIR_RW,
        bits_pioz80_a, sizeof(bits_pioz80_a) / sizeof(bits_pioz80_a[0]),
        read_pioz80_a_data, NULL,
        MZ_AVAIL_800 | MZ_AVAIL_1500
    },
    {
        0x00FF, "Z80 PIO - Port B data (R/W)", "Tiskarna control bits",
        IO_PORT_DIR_RW,
        NULL, 0,
        read_pioz80_b_data, NULL,
        MZ_AVAIL_800 | MZ_AVAIL_1500
    },

    /* ===== CMT hack (0x01, 0x02 W) - emu loader trampoliny =====
     * ROM ve vsech 3 archech vola tyto IORQ pro trigger emu-side CMT loaderu
     * (cmthack_load_file / cmthack_read_mzf_body). Touch-triggered =
     * dispatch jen na adresu, write hodnota se ignoruje, zadny register,
     * zadny mirror, zadne bity k dekodovani (V1.7+ 2.1). */
    {
        0x0001, "CMT hack - Load file (W)",
        "Touch-triggered (write value ignored): activates emu-side "
        "MZF load (cmthack_load_file)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_ALL
    },
    {
        0x0002, "CMT hack - Read MZF body (W)",
        "Touch-triggered (write value ignored): activates emu-side "
        "MZF body read (cmthack_read_mzf_body)",
        IO_PORT_DIR_W,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_ALL
    },

    /* ===== Unicard (0x50, 0x51 R/W) =====
     * SD/FAT storage extension. unimgr API:
     *   0x50 = CMD register (en_UNIMGR_ADDR_CMD)
     *   0x51 = DATA register (en_UNIMGR_ADDR_DATA)
     * Ridi se runtime UNICARD_TEST_IS_CONNECTED. */
    {
        0x0050, "Unicard - CMD (R/W)", "Unicard manager command register (unimgr CMD)",
        IO_PORT_DIR_RW,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_ALL
    },
    {
        0x0051, "Unicard - DATA (R/W)", "Unicard manager data register (unimgr DATA)",
        IO_PORT_DIR_RW,
        NULL, 0,
        NULL, NULL,
        MZ_AVAIL_ALL
    },

    /* ===== Ramdisk Pezik 68 (0x68-0x6F R/W) =====
     * Pezik typ 0 - posunute umisteni (0x68 = bank 0). Banking primarne pres
     * ADRESU portu (ne data bity):
     *   bit 7 adresy (0/1) selektuje pezik typ (0 = Pezik 68, 1 = Pezik E8)
     *   bit 0-2  adresy = bank (0..7)
     *   port_high byte (= adresa) slouzi jako bank/offset latch.
     * Data bytes = obsah bank, ne bit-mapped registr. Bit-by-bit dekody
     * se neimplementuji (V1.7+ 2.1: banking via address, not register-bit). */
    { 0x0068, "Ramdisk - Pezik 68 bank 0 (R/W)", "Pezik ramdisk type 0 (shifted), bank 0 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x0069, "Ramdisk - Pezik 68 bank 1 (R/W)", "Pezik ramdisk type 0, bank 1 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x006A, "Ramdisk - Pezik 68 bank 2 (R/W)", "Pezik ramdisk type 0, bank 2 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x006B, "Ramdisk - Pezik 68 bank 3 (R/W)", "Pezik ramdisk type 0, bank 3 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x006C, "Ramdisk - Pezik 68 bank 4 (R/W)", "Pezik ramdisk type 0, bank 4 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x006D, "Ramdisk - Pezik 68 bank 5 (R/W)", "Pezik ramdisk type 0, bank 5 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x006E, "Ramdisk - Pezik 68 bank 6 (R/W)", "Pezik ramdisk type 0, bank 6 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x006F, "Ramdisk - Pezik 68 bank 7 (R/W)", "Pezik ramdisk type 0, bank 7 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },

    /* ===== IDE8 (0x78-0x7F R/W) - 8-bit IDE/CompactFlash ATA interface =====
     * Standardni ATA register set, 8-bit data port. Adresa = port_lsb & 0x07.
     * Command/data interface (en_IDE8_ADDR/en_IDE8_CMD enums v ide8.h):
     *   - data/feature/sector/CHS/drive porty = address-indexed registry
     *   - status (0x7F R) ma bity (BUSY/READY/DRQ/...) ale je R/O informativni
     *   - command (0x7F W) zapis spousti operaci, ne bit toggle
     * Bit-by-bit dekody se neimplementuji (V1.7+ 2.1: command/data port,
     * not bit-mapped). Per-port description nese ATA semantiku. */
    { 0x0078, "IDE8 - Data (R/W)",             "ATA data register (8-bit data port) (ATA command/data, not bit-mapped)",     IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x0079, "IDE8 - Error / Features (R/W)", "ATA error (R) / features (W) (ATA command/data, not bit-mapped)",            IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x007A, "IDE8 - Sector count (R/W)",     "ATA sector count register (ATA command/data, not bit-mapped)",               IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x007B, "IDE8 - Sector number (R/W)",    "ATA sector number register (LBA0-7) (ATA command/data, not bit-mapped)",     IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x007C, "IDE8 - Cylinder low (R/W)",     "ATA cylinder low register (LBA8-15) (ATA command/data, not bit-mapped)",     IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x007D, "IDE8 - Cylinder high (R/W)",    "ATA cylinder high register (LBA16-23) (ATA command/data, not bit-mapped)",   IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x007E, "IDE8 - Drive / Head (R/W)",     "ATA drive/head register (LBA24-27 + drv) (ATA command/data, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x007F, "IDE8 - Status / Command (R/W)", "ATA status (R, R/O info bits BUSY/READY/DRQ/...) / command (W) (ATA command/data, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },

    /* ===== Ramdisk Pezik E8 (0xE8-0xEF R/W) - jen MZ-800 =====
     * Pezik typ 1 - puvodni umisteni. 0xE9-0xEB se sdili se Std ramdisk
     * (write fallback pokud Pezik E8 disconnected).
     * Na MZ-1500 a MZ-700 NENI dostupny - port 0xE9 koliduje s PSG (MZ-1500)
     * resp. neni vubec routed (MZ-700, viz mz700_iorq.c).
     * Banking semantika viz sekce Pezik 68 vyse. Bit-by-bit dekody se
     * neimplementuji (V1.7+ 2.1: banking via address, not register-bit). */
    { 0x00E8, "Ramdisk - Pezik E8 bank 0 (R/W)",        "Pezik ramdisk type 1, bank 0 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00E9, "Ramdisk - Pezik E8 bank 1 / Std bank (R/W)",   "Pezik bank 1 OR Std ramdisk bank-select (W) (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00EA, "Ramdisk - Pezik E8 bank 2 / Std data (R/W)",   "Pezik bank 2 OR Std ramdisk data byte (auto-incr) (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00EB, "Ramdisk - Pezik E8 bank 3 / Std offset (R/W)", "Pezik bank 3 OR Std ramdisk offset latch (W) (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00EC, "Ramdisk - Pezik E8 bank 4 (R/W)",        "Pezik ramdisk type 1, bank 4 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00ED, "Ramdisk - Pezik E8 bank 5 (R/W)",        "Pezik ramdisk type 1, bank 5 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00EE, "Ramdisk - Pezik E8 bank 6 (R/W)",        "Pezik ramdisk type 1, bank 6 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00EF, "Ramdisk - Pezik E8 bank 7 (R/W)",        "Pezik ramdisk type 1, bank 7 (banking, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_800 },

    /* ===== QDISK (0xF4-0xF7 R/W) - Z80 SIO (Sharp Quick Disk) =====
     * Z80 SIO chip, dva nezavisle kanaly (A/B), kazdy s data a control reg.
     * Command/data interface (en_QDSIO_WR0CMD enum v qdisk.h, parsovany v
     * qdisk.c:1106 jako (value >> 3) & 0x07): zapisy do Ctrl A/B jsou SIO
     * commands (Wreg0-7 state sekvencer), zapisy do Data A/B jsou FIFO
     * data byty. Bit-by-bit dekody se neimplementuji (V1.7+ 2.1: SIO
     * Wreg/Rreg jsou sekvencery, ne bit-mapped HW registry). */
    { 0x00F4, "QDISK - SIO Data A (R/W)", "Z80 SIO Channel A data register (Z80 SIO command/data, not bit-mapped)",    IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x00F5, "QDISK - SIO Data B (R/W)", "Z80 SIO Channel B data register (Z80 SIO command/data, not bit-mapped)",    IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x00F6, "QDISK - SIO Ctrl A (R/W)", "Z80 SIO Channel A control register (Z80 SIO command/data, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },
    { 0x00F7, "QDISK - SIO Ctrl B (R/W)", "Z80 SIO Channel B control register (Z80 SIO command/data, not bit-mapped)", IO_PORT_DIR_RW, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },

    /* ===== Ramdisk Std =====
     * Standardni ramdisk - oddelena adresova mapa od Pezik. Per-arch ruzny
     * port mapping pro read:
     *   MZ-800:        0xF8 R reset, 0xF9 R read, 0xFA W write
     *   MZ-1500/MZ-700: 0xEA R read, 0xFA W write (no reset port)
     * 0xE9 W (bank-select) a 0xEB W (offset latch) jsou na MZ-800 sdilene
     * s Pezik E8 (uvedene v Pezik E8 sekci, MZ-800 only).
     * Reset / read / write = touch-trigger nebo data byte, ne bit-mapped
     * registr. Bit-by-bit dekody se neimplementuji (V1.7+ 2.1: banking,
     * not register-bit). */
    { 0x00F8, "Ramdisk - Std reset (R)", "Std ramdisk reset bank/offset to 0 (read trigger) (banking, not bit-mapped)", IO_PORT_DIR_R, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00F9, "Ramdisk - Std read (R)",  "Std ramdisk read byte at offset (auto-incr) (banking, not bit-mapped)",       IO_PORT_DIR_R, NULL, 0, NULL, NULL, MZ_AVAIL_800 },
    { 0x00EA, "Ramdisk - Std read (R)",  "Std ramdisk read byte at offset (auto-incr) - MZ-1500/MZ-700 layout (banking, not bit-mapped)", IO_PORT_DIR_R, NULL, 0, NULL, NULL, MZ_AVAIL_1500 | MZ_AVAIL_700 },
    { 0x00FA, "Ramdisk - Std write (W)", "Std ramdisk write byte at offset (auto-incr) (banking, not bit-mapped)",      IO_PORT_DIR_W, NULL, 0, NULL, NULL, MZ_AVAIL_ALL },

    /* Sentinel */
    { 0xFFFF, NULL, NULL, IO_PORT_DIR_R, NULL, 0, NULL, NULL, 0 }
};

/**
 * Počet platných záznamů v g_io_ports[] (mimo sentinel).
 *
 * Spočítá se compile-time přes sizeof - 1 (sentinel).
 */
const size_t g_io_ports_count =
    (sizeof(g_io_ports) / sizeof(g_io_ports[0])) - 1;

/**
 * Side-effect-free probe portu přes mirror gettery z `g_io_ports[]`.
 *
 * Viz doc v `io_catalog.h`. Lineární scan tabulky; pro každý port LSB
 * obvykle 1-2 R/RW entries (např. 0xCE má GDG Status R + DMD W,
 * 0xE004-E006 mají IORQ + MMIO duplicitu). Bere první nalezený match
 * (= pořadí v `g_io_ports[]` určuje prioritu při shodě více R entries
 * na stejné LSB).
 */
int io_catalog_probe_byte(uint8_t port_lsb, uint8_t *out_value)
{
    for (size_t i = 0; i < g_io_ports_count; i++)
    {
        const st_IO_PORT_DESC *e = &g_io_ports[i];
        if ((uint8_t)(e->addr & 0xFFu) != port_lsb)
            continue;
        /* MMIO entries (high byte != 0, např. 0xE000+) přeskočit -
         * patří do memory map space, ne IORQ portů. */
        if ((e->addr & 0xFF00u) != 0u)
            continue;
        if (e->direction != IO_PORT_DIR_R && e->direction != IO_PORT_DIR_RW)
            continue;
        if (e->read_value == NULL)
            continue;
        if (out_value != NULL)
            *out_value = e->read_value();
        return 1;
    }
    return 0;
}
