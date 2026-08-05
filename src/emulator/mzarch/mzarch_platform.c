#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
#include "main.h"
#include "mzarch_platform.h"
#include "mztvsys.h"
#include "hw-generic/gdg/gdgclk.h"
#include <stdio.h>

#if !defined(MZARCH)
#error "MZARCH not defined"
#define MZARCH 0
#else
#if MZARCH != 700 && MZARCH != 800 && MZARCH != 1500
#error "MZARCH not defined"
#endif
#endif

#if !defined(MZARCH_NAME)
#error "MZARCH_NAME not defined"
#define MZARCH_NAME "mzarch_unknown"
#endif

#if !defined(MZTVSYS) || !defined(MZTVSYS_PAL) || !defined(MZTVSYS_NTSC)
#error "MZTVSYS not defined (expected MZTVSYS_PAL=50 or MZTVSYS_NTSC=60)"
#endif

#if MZARCH == 800
const char *g_mzarch_full_name = "MZ-800";
#elif MZARCH == 1500
const char *g_mzarch_full_name = "MZ-1500";
#elif MZARCH == 700
  #if MZTVSYS == MZTVSYS_PAL
const char *g_mzarch_full_name = "MZ-700 (PAL)";
  #elif MZTVSYS == MZTVSYS_NTSC
const char *g_mzarch_full_name = "MZ-700 (NTSC)";
  #else
const char *g_mzarch_full_name = "MZ-700 (?)";
  #endif
#else
const char *g_mzarch_full_name = "MZ-UNKNOWN";
#endif

const char *g_mzarch_platform_name = MZARCH_NAME;

const uint32_t g_mzarch_platform_pxclk = GDGCLK_BASE;

int g_mzarch_platform_numeric = MZARCH;
