#ifndef FRAMEBUFFER_DONE_H
#define FRAMEBUFFER_DONE_H

#if MZARCH == 800
#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
#include "mzarch/mz800/gdg/mz800_framebuffer_done.h"
#else
#if MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_framebuffer_done.h"
#else
#if MZARCH == 700
#include "mzarch/mz700/gdg/mz700_framebuffer_done.h"
#else
#error "FRAMEBUFFER_DONE_H: Unknown MZARCH value"
#endif /* MZARCH == 700 */
#endif /* MZARCH == 1500 */
#endif /* MZARCH == 800 */

#endif // FRAMEBUFFER_DONE_H