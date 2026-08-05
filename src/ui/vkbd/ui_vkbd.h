#ifndef UI_VKBD_H
#define UI_VKBD_H
#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */

#include "main.h"
#include <stdbool.h>
#include "libs/sdlapp/sdlapp.h"
#include "emulator/hw-generic/pio8255/pio8255.h"


/* Bázová cesta k obrázkům kláves virtuální klávesnice - per MZARCH.
 * MZ-700 a MZ-1500 mají odlišnou klávesnici (bez TAB, delší ALPHA), proto
 * vlastní sadu obrázků. Viz vk_row2_def / vk_row4_def níže. */
#if MZARCH == 700
#define VKBD_IMAGE_BASE_PATH "ui_resources/mz700-vkbd/"
#elif MZARCH == 1500
#define VKBD_IMAGE_BASE_PATH "ui_resources/mz1500-vkbd/"
#else
#define VKBD_IMAGE_BASE_PATH "ui_resources/mz800-vkbd/"
#endif

typedef struct st_VKBD_KEYDEF
{
    SDL_Scancode scancode;
    int mzcolumn;
    int mzbit;
    const char *filename;
} st_VKBD_KEYDEF;

extern const st_VKBD_KEYDEF vk_row0a_def[];
extern const st_VKBD_KEYDEF vk_row0b_def[];
extern const st_VKBD_KEYDEF vk_row1_def[];
extern const st_VKBD_KEYDEF vk_row2_def[];
extern const st_VKBD_KEYDEF vk_row3_def[];
extern const st_VKBD_KEYDEF vk_row4_def[];
extern const st_VKBD_KEYDEF vk_row5_def[];
extern const st_VKBD_KEYDEF vk_cursorUp_def[];
extern const st_VKBD_KEYDEF vk_cursorLR_def[];
extern const st_VKBD_KEYDEF vk_cursorDown_def[];

static inline bool ui_vkbd_is_active(const st_VKBD_KEYDEF *vkdef)
{
    // aktivni klavesa ma hodnotu 0
    return PIO8255_KBDALL_BIT_GET(vkdef->mzcolumn, vkdef->mzbit) ? false : true;
}


typedef struct st_VKBD_OPTKEYDEF
{
    SDL_Scancode aditional_scancode;
    SDL_Scancode scancode;
} st_VKBD_OPTKEYDEF;

extern const st_VKBD_OPTKEYDEF vk_optdef[];

typedef void (*vkbd_optfunc_cb)(void);

typedef struct st_VKBD_OPTFUNC
{
    SDL_Scancode scancode;
    vkbd_optfunc_cb callback;
} st_VKBD_OPTFUNC;

extern const st_VKBD_OPTFUNC vk_optfunc[];

typedef enum en_VKBD_ACT_CALLER
{
    VK_ACTCALL_NONE = 0,
    VK_ACTCALL_MOUSE = 1,
    VK_ACTCALL_KEY1 = 2,
    VK_ACTCALL_KEY2 = 4,
} en_VKBD_ACT_CALLER;

#define VK_ALL_ACTCALLERS (VK_ACTCALL_MOUSE | VK_ACTCALL_KEY1 | VK_ACTCALL_KEY2)

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Vrátí hint s PC ekvivalentem pro "neintuitivně" mapované Sharp
 *        klávesy (viz docs/{cz,en}/README.md, sekce mapování klávesnice).
 *
 * @param scancode  SDL scancode klávesy (st_VKBD_KEYDEF.scancode).
 * @return  Anglický řetězec značený N_() (caller přeloží přes _()), nebo
 *          NULL pokud klávesa hint nemá (je na PC na intuitivním místě).
 */
const char *ui_vkbd_pc_hint(SDL_Scancode scancode);

#ifdef __cplusplus
}
#endif

#endif /* UI_VKBD_H */
