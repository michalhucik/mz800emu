#ifndef MZ700_CONFIG_H
#define MZ700_CONFIG_H

/*
 * Platform suffix pro per-arch oddelene default soubory a adresare
 * (ramdisk image, Unicard SD root). Compile-time string literal pro
 * Od mzhal kroku 7 se skladaji RUNTIME z g_mzhal.arch_name;
 * compile-time MZ_PLATFORM_SUFFIX zrusen.
 */

/*
 * Konfiguracni vypnuti periferii MZ-700
 * =======================================
 *
 * Nastavuje se 1, nebo 0
 *
 */
#define CFG_HWEXT_HAVE_FDC 1
#define CFG_HWEXT_HAVE_IDE8 1
#define CFG_HWEXT_HAVE_RAMDISK 1
#define CFG_HWEXT_HAVE_QDISK 1

#define HAVE_PIOZ80 0

/* PSG: MZ-700 zadny zvukovy generator nema (vystup pres CTC0 → speaker) */
#define HAVE_PSG 0

#endif /* MZ700_CONFIG_H */
