#ifndef MZ1500_CONFIG_H
#define MZ1500_CONFIG_H

/*
 * Platform suffix pro per-arch oddelene default soubory a adresare
 * (ramdisk image, Unicard SD root). Compile-time string literal pro
 * Od mzhal kroku 7 se skladaji RUNTIME z g_mzhal.arch_name;
 * compile-time MZ_PLATFORM_SUFFIX zrusen.
 */

/*
 * Konfiguracni vypnuti periferii MZ-1500
 * =======================================
 *
 * Nastavuje se 1, nebo 0
 *
 */
#define CFG_HWEXT_HAVE_FDC 1
#define CFG_HWEXT_HAVE_IDE8 1
#define CFG_HWEXT_HAVE_RAMDISK 1
#define CFG_HWEXT_HAVE_QDISK 1


/* Z80 PIO: MZ-1500 ma Z80 PIO chip (joystick + keyboard interrupt) */
#define HAVE_PIOZ80 1

/* PSG: MZ-1500 ma stereo PSG (dva chipy SN76489 - L+R) */
#define HAVE_PSG 2

#endif /* MZ1500_CONFIG_H */
