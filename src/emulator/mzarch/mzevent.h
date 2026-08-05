#ifndef MZEVENT_H
#define MZEVENT_H

/* Společné toggly přímo (ne přes mzarch_config.h) - číslování enumu
 * nesmí záviset na tom, jestli si TU vzpomněl na správný include. */
#include "mzcommon_config.h"

typedef enum en_MZEVENT
{
    //        MZEVENT_GDG_STS_HSYNC_END,
    MZEVENT_GDG_HBLN_END,
    MZEVENT_GDG_HBLN_START,
    MZEVENT_GDG_STS_VSYNC_END,
    MZEVENT_GDG_STS_VSYNC_START,
    MZEVENT_GDG_AFTER_LAST_SCREEN_PIXEL,
    //        MZEVENT_GDG_STS_HSYNC_START,
    MZEVENT_GDG_AFTER_LAST_VISIBLE_PIXEL,
    MZEVENT_GDG_REAL_HSYNC_START,
    MZEVENT_GDG_SCREEN_ROW_END,

    // jine, nez GDG eventy
    MZEVENT_NO_GDG, /* pouze hranicni hodnota - neni skutecny event */

    MZEVENT_PIOZ80,

    /* Event existuje v OBOU CLK1M1 variantách (mzhal krok 5) - číslování
     * enumu (persistované mj. ve snapshot event_name) nesmí záviset na
     * build volbě. V SLOW variantě se jen nikdy nenaplánuje. */
    MZEVENT_CTC0,

    MZEVENT_CUSTOM_SPEED_SYNCHRONISATION,
    MZEVENT_BREAK, /* pouze hranicni hodnota - neni skutecny event */

    // V prubehu zpracovani instrukce vznikl MZ800 interrupt.
    // Tento event slouzi k tomu, aby jsme vybehli z instrukcni smycky a pokusili se jej prevzit.
    MZEVENT_BREAK_MZARCH_INTERRUPT,

    MZEVENT_BREAK_EMULATION_PAUSED,
} en_MZEVENT;

typedef struct st_EMUEVENT
{
    en_MZEVENT event_name;
    unsigned ticks;
} st_EMUEVENT;

#endif /* MZEVENT_H */
