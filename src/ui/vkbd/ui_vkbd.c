#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
#include "main.h"
#include <stdio.h>
#include "libs/sdlapp/sdlapp.h"
#include "ui_vkbd.h"
#include "i18n.h"

#include "mzarch/mzarch_platform_functions.h"

/*
 * Spravny pomer velikosti a rozlozeni klaves:
 * -------------------------------------------
 *
 * Row 1 - 4 maji celkovou sirku 15 a 1/2 klavesy.
 *
 * F1 - F5, INS, DEL, TAB, ESC a CURSORS maji sirku 1 a 1/2 klavesy.
 *
 * CTRL a CR maji sirku 1 a 3/4 klavesy.
 *
 * SHIFT_L ma sirku 1 a 1/4 klavesy.
 *
 * SHIFT_R ma sirku 2 a 1/4 klavesy.
 *
 * SPACEBAR ma sirku 9 klaves. Zacina soucasne s klavesou X a konci s klavesou slash.
 *
 * CURSOR LEFT je od CR vzdalen na delku 1 a 1/5 klavesy.
 *
 * F1 - F5, INS, DEL ma vysku 1/2 klavesy.
 *
 * Mezera mezi row0 a row1 je 1/2 klavesy.
 *
 */

#define UI_VKB_TOP_BORDER 15
#define UI_VKB_LEFT_BORDER 15
#define UI_VKB_RIGHT_BORDER 15
#define UI_VKB_BOTOM_BORDER 15

const st_VKBD_KEYDEF vk_row0a_def[] = {
    {SDL_SCANCODE_F1, 9, 7, "row0/f1.png"},
    {SDL_SCANCODE_F2, 9, 6, "row0/f2.png"},
    {SDL_SCANCODE_F3, 9, 5, "row0/f3.png"},
    {SDL_SCANCODE_F4, 9, 4, "row0/f4.png"},
    {SDL_SCANCODE_F5, 9, 3, "row0/f5.png"},
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_row0b_def[] = {
    {SDL_SCANCODE_INSERT, 7, 7, "row0/inst.png"},
    {SDL_SCANCODE_DELETE, 7, 6, "row0/del.png"}, // + SDL_SCANCODE_BACKSPACE
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_row1_def[] = {
    {SDL_SCANCODE_CAPSLOCK, 0, 6, "row1/graph.bmp"},
    {SDL_SCANCODE_1, 5, 7, "row1/1.bmp"},
    {SDL_SCANCODE_2, 5, 6, "row1/2.bmp"},
    {SDL_SCANCODE_3, 5, 5, "row1/3.bmp"},
    {SDL_SCANCODE_4, 5, 4, "row1/4.bmp"},
    {SDL_SCANCODE_5, 5, 3, "row1/5.bmp"},
    {SDL_SCANCODE_6, 5, 2, "row1/6.bmp"},
    {SDL_SCANCODE_7, 5, 1, "row1/7.bmp"},
    {SDL_SCANCODE_8, 5, 0, "row1/8.bmp"},
    {SDL_SCANCODE_9, 6, 2, "row1/9.bmp"},
    {SDL_SCANCODE_0, 6, 3, "row1/0.bmp"},
    {SDL_SCANCODE_MINUS, 6, 5, "row1/minus.bmp"},
    {SDL_SCANCODE_EQUALS, 6, 6, "row1/arrow_up.bmp"},
    {SDL_SCANCODE_F7, 6, 7, "row1/backslash.bmp"},
    {SDL_SCANCODE_ESCAPE, 8, 7, "row1/break.png"}, // + SDL_SCANCODE_END
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_row2_def[] = {
#if MZARCH == 800
    {SDL_SCANCODE_TAB, 0, 3, "row2/tab.png"},
#else
    /* MZ-700/MZ-1500: na pozici TAB je delší klávesa ALPHA (stejná šířka
     * jako TAB na MZ-800). HW tu nemá TAB - bit (0, 3) se nikdy nevystaví.
     * Hostitelská TAB se aplikuje jako ALPHA (viz iface_keyboard.c), proto
     * má klávesa dva PC ekvivalenty (BACKSLASH + TAB). */
    {SDL_SCANCODE_BACKSLASH, 0, 4, "row2/alpha.png"},
#endif
    {SDL_SCANCODE_Q, 2, 7, "row2/q.bmp"},
    {SDL_SCANCODE_W, 2, 1, "row2/w.bmp"},
    {SDL_SCANCODE_E, 4, 3, "row2/e.bmp"},
    {SDL_SCANCODE_R, 2, 6, "row2/r.bmp"},
    {SDL_SCANCODE_T, 2, 4, "row2/t.bmp"},
    {SDL_SCANCODE_Y, 1, 7, "row2/y.bmp"},
    {SDL_SCANCODE_U, 2, 3, "row2/u.bmp"},
    {SDL_SCANCODE_I, 3, 7, "row2/i.bmp"},
    {SDL_SCANCODE_O, 3, 1, "row2/o.bmp"},
    {SDL_SCANCODE_P, 3, 0, "row2/p.bmp"},
    {SDL_SCANCODE_F6, 1, 5, "row2/at.bmp"},
    {SDL_SCANCODE_LEFTBRACKET, 1, 4, "row2/bracketleft.bmp"},
    {SDL_SCANCODE_F9, 0, 5, "row2/libra.bmp"},
    {SDL_SCANCODE_GRAVE, 0, 7, "row2/blank.bmp"},
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_row3_def[] = {
    {SDL_SCANCODE_LCTRL, 8, 6, "row3/ctrl.png"},
    {SDL_SCANCODE_A, 4, 7, "row3/a.bmp"},
    {SDL_SCANCODE_S, 2, 5, "row3/s.bmp"},
    {SDL_SCANCODE_D, 4, 4, "row3/d.bmp"},
    {SDL_SCANCODE_F, 4, 2, "row3/f.bmp"},
    {SDL_SCANCODE_G, 4, 1, "row3/g.bmp"},
    {SDL_SCANCODE_H, 4, 0, "row3/h.bmp"},
    {SDL_SCANCODE_J, 3, 6, "row3/j.bmp"},
    {SDL_SCANCODE_K, 3, 5, "row3/k.bmp"},
    {SDL_SCANCODE_L, 3, 4, "row3/l.bmp"},
    {SDL_SCANCODE_SEMICOLON, 0, 2, "row3/semicolon.bmp"},
    {SDL_SCANCODE_APOSTROPHE, 0, 1, "row3/colon.bmp"},
    {SDL_SCANCODE_RIGHTBRACKET, 1, 3, "row3/bracketright.bmp"},
    {SDL_SCANCODE_RETURN, 0, 0, "row3/cr.png"}, // SDL_SCANCODE_RETURN2 A SDL_SCANCODE_KP_ENTER
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_row4_def[] = {
    {SDL_SCANCODE_LSHIFT, 8, 0, "row4/shift_l.png"}, // duplicitni funkce jako SDL_SCANCODE_RSHIFT
#if MZARCH == 800
    {SDL_SCANCODE_BACKSLASH, 0, 4, "row4/alpha.bmp"},
#endif
    /* MZ-700/MZ-1500: ALPHA je v row2 (na pozici TAB), takže tato řada má
     * o jednu klávesu méně a levý SHIFT je stejně velký jako pravý. */
    {SDL_SCANCODE_Z, 1, 6, "row4/z.bmp"},
    {SDL_SCANCODE_X, 2, 0, "row4/x.bmp"},
    {SDL_SCANCODE_C, 4, 5, "row4/c.bmp"},
    {SDL_SCANCODE_V, 2, 2, "row4/v.bmp"},
    {SDL_SCANCODE_B, 4, 6, "row4/b.bmp"},
    {SDL_SCANCODE_N, 3, 2, "row4/n.bmp"},
    {SDL_SCANCODE_M, 3, 3, "row4/m.bmp"},
    {SDL_SCANCODE_COMMA, 6, 1, "row4/coma.bmp"},
    {SDL_SCANCODE_PERIOD, 6, 0, "row4/period.bmp"},
    {SDL_SCANCODE_SLASH, 7, 0, "row4/slash.bmp"},
    {SDL_SCANCODE_F8, 7, 1, "row4/question.bmp"},
    {SDL_SCANCODE_RSHIFT, 8, 0, "row4/shift_r.png"}, // duplicitni funkce jako SDL_SCANCODE_LSHIFT
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_row5_def[] = {
    {SDL_SCANCODE_SPACE, 6, 4, "row5/space.bmp"},
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_cursorUp_def[] = {
    {SDL_SCANCODE_UP, 7, 5, "cursor_keys/up.png"},
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_cursorLR_def[] = {
    {SDL_SCANCODE_LEFT, 7, 2, "cursor_keys/left.png"},
    {SDL_SCANCODE_RIGHT, 7, 3, "cursor_keys/right.png"},
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_KEYDEF vk_cursorDown_def[] = {
    {SDL_SCANCODE_DOWN, 7, 4, "cursor_keys/down.png"},
    {0, 0, 0, (char *)NULL},
};

const st_VKBD_OPTKEYDEF vk_optdef[] = {
    {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_DELETE},
    {SDL_SCANCODE_END, SDL_SCANCODE_ESCAPE},
    {SDL_SCANCODE_RETURN2, SDL_SCANCODE_RETURN},
    {SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_RETURN},
    {0, 0},
};

#ifdef WINDOWS
#include <windows.h>

static void ui_vkbd_F11_reset_request(void)
{
    if (IsDebuggerPresent())
    {
        printf("Debugger is present! F11 - is now remapped as RESET.\n");
        mzarch_platform_fn_reset_request();
    };
}
#endif

const st_VKBD_OPTFUNC vk_optfunc[] = {
#ifdef WINDOWS
    {SDL_SCANCODE_F11, ui_vkbd_F11_reset_request},
#endif
    {SDL_SCANCODE_F12, mzarch_platform_fn_reset_request},
    {0, NULL}};


/**
 * @brief Vrátí hint s PC ekvivalentem pro "neintuitivně" mapované Sharp
 *        klávesy.
 *
 * Pokrývá klávesy, které na PC nejsou tam, kde by uživatel čekal (viz sekce
 * mapování klávesnice v docs/{cz,en}/README.md). Texty jsou anglické a
 * značené N_() (= jen registrace do .pot); caller je přeloží přes _().
 *
 * @param scancode  SDL scancode klávesy (st_VKBD_KEYDEF.scancode).
 * @return  Konstantní řetězec (N_-marker) s hintem, nebo NULL pokud je
 *          klávesa na PC na intuitivním místě (= žádný hint).
 */
const char *ui_vkbd_pc_hint(SDL_Scancode scancode)
{
    switch (scancode)
    {
        case SDL_SCANCODE_CAPSLOCK:                 /* GRAPH */
            return N_("PC key: CapsLock");
        case SDL_SCANCODE_BACKSLASH:                /* ALPHA */
#if MZARCH == 800
            return N_("PC key: \\");
#else
            /* MZ-700/1500: ALPHA reaguje i na hostitelskou klávesu Tab. */
            return N_("PC key: \\ or Tab");
#endif
        case SDL_SCANCODE_GRAVE:                    /* BLANK */
            return N_("PC key: ~");
        case SDL_SCANCODE_ESCAPE:                   /* BREAK / ESC */
            return N_("PC key: Esc or End");
        case SDL_SCANCODE_INSERT:                   /* INST */
            return N_("PC key: Insert");
        case SDL_SCANCODE_DELETE:                   /* DEL */
            return N_("PC key: Backspace or Delete");
        case SDL_SCANCODE_F6:                       /* @ */
            return N_("PC key: F6");
        case SDL_SCANCODE_F7:                       /* \ (backslash znak) */
            return N_("PC key: F7");
        case SDL_SCANCODE_F8:                       /* ? */
            return N_("PC key: F8");
        case SDL_SCANCODE_F9:                       /* LIBRA */
            return N_("PC key: F9");
        default:
            return NULL;
    };
}

