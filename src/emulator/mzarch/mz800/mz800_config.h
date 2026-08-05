#ifndef MZ800_CONFIG_H
#define MZ800_CONFIG_H

/*
 * Per-arch oddelene default soubory a adresare (ramdisk image, Unicard
 * SD root) se od mzhal kroku 7 skladaji RUNTIME z g_mzhal.arch_name;
 * compile-time MZ_PLATFORM_SUFFIX zrusen.
 */

/*
 * Konfiguracni vypnuti periferii MZ-800
 * =======================================
 *
 * Nastavuje se 1, nebo 0
 *
 */
#define CFG_HWEXT_HAVE_FDC 1
#define CFG_HWEXT_HAVE_IDE8 1
#define CFG_HWEXT_HAVE_RAMDISK 1
#define CFG_HWEXT_HAVE_QDISK 1

#define HAVE_PIOZ80 1

/* PSG: MZ-800 nativne mono PSG (SN76489), ale HW experiment allow_psg1
 * umoznuje za behu pripojit druhy PSG → stereo. HAVE_PSG=2 znamena
 * "max pocet PSG instanci compile-time", runtime g_psg_module.stereo
 * rozhoduje aktualni stav. */
#define HAVE_PSG 2

#endif /* MZ800_CONFIG_H */
