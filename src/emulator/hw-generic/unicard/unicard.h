/* 
 * File:   unicard.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 21. června 2018, 16:08
 * 
 * 
 * ----------------------------- License -------------------------------------
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * ---------------------------------------------------------------------------
 */


#ifndef UNICARD_H
#define UNICARD_H

#ifdef __cplusplus
extern "C" {
#endif

#define UNICARD_EMULATED

#include <stdio.h>
#include <stdint.h>
#include <glib.h>

#include "mzarch/mzcommon_config.h"

#ifdef UNICARD_EMULATED
#include "ff_result.h"
#include "ff_open_mode.h"
#include "unicard_sfn.h"  /* generátor FatFS 8.3 SFN aliasu */
#define _MAX_LFN    32  // maximalni povolena delka filename, vcetne terminatoru
#endif


    typedef struct st_UNICARD_FILE {
        char *filepath;
        FILE *fh;
        uint8_t mode;
    } st_UNICARD_FILE;


    /**
     * Informace o souboru/adresáři (mapování FatFS FILINFO).
     *
     * Slouží jako binární výstup příkazů cmdSTAT a cmdREADDIR. Layout
     * členů odpovídá tomu, co očekává guest SW přes data port:
     *   bytes 0-3:   fsize (DWORD, LE)
     *   bytes 4-5:   fdate (WORD, LE, FAT format: (year-1980)<<9 | month<<5 | day)
     *   bytes 6-7:   ftime (WORD, LE, FAT format: hour<<11 | min<<5 | sec/2)
     *   byte  8:     fattrib (AM_RDO/AM_HID/AM_SYS/AM_DIR/AM_ARC)
     *   bytes 9-21:  fname[13] (8.3 short jméno, 0x00 padded)
     *   byte  22:    strlen(lfname)
     *   bytes 23-N:  lfname[_MAX_LFN] (LFN, 0x00 padded; N = 22 + _MAX_LFN)
     *
     * Velikost serializovaného výstupu = 23 + _MAX_LFN bytů.
     */
    typedef struct st_UNICARD_FILINFO {
        uint32_t fsize;          /* velikost souboru v bytech */
        uint16_t fdate;          /* FAT date last modified */
        uint16_t ftime;          /* FAT time last modified */
        uint8_t fattrib;         /* atribut bity (AM_*) */
        char fname[13];          /* 8.3 jméno s 0x00 terminátorem */
        uint8_t lfn_strlen;      /* délka lfname (0..29) */
        char lfname[_MAX_LFN];   /* dlouhé jméno (LFN) */
    } st_UNICARD_FILINFO;


    /* FAT atribut bity (kompatibilní s FatFS AM_*) */
#define UNICARD_AM_RDO  0x01     /* Read-only */
#define UNICARD_AM_HID  0x02     /* Hidden */
#define UNICARD_AM_SYS  0x04     /* System */
#define UNICARD_AM_DIR  0x10     /* Directory */
#define UNICARD_AM_ARC  0x20     /* Archive */


    typedef struct st_UNICARD_DIR {
        char *dirpath;
        GDir *dh;
        int position; // pozice, kterou prave cteme (na nulte pozici vygenerujeme '..')

        /* Workaround pro bug v Unicard manager V2.11b:
         *
         * V2.11b crashuje při root FILELIST listingu, pokud první entry
         * od emu je file a v listingu je i adresář (= sort musi presunut
         * dir na pozici 0). V non-root scenari V2.11b sám vkládá ".."
         * entry na pozici 0 (dir), takže sort dir-na-pos-0 nepřesouvá
         * a bug se neaktivuje. V root chybi ".." → bug.
         *
         * Ochcávka: při root listingu (jen v uc3 simulaci), pokud
         * prescan zjistí že první entry je file, najdeme první dir
         * a uložíme ho do `forced_first_dir`. V `unicard_dir_read_filelist`
         * ten dir emitujeme jako PRVNÍ entry. Normální gdir read pak
         * ten dir skipne (matched podle name).
         *
         * Bez ochcávky: 2 file v root, 1 dir → file emitnut první →
         * V2.11b sort přesune dir na pos 0 → crash.
         */
        char *forced_first_dir;

        /* Kolizní kontext pro FatFS ~N číslování 8.3 aliasů (offset 9).
         * Buduje se inkrementálně v pořadí iterace readdir; init v
         * unicard_dir_open, clear v unicard_dir_close. */
        st_UNICARD_SFN_CTX sfn_ctx;
    } st_UNICARD_DIR;


    typedef struct st_UNICARD_RTC {
        uint8_t hour;
        uint8_t min;
        uint8_t sec;
        uint8_t day;
        uint16_t year;
        uint8_t month;
    } st_UNICARD_RTC;


    typedef enum en_UNICARD_CONNECTION {
        UNICARD_CONNECTION_DISCONNECTED = 0,
        UNICARD_CONNECTION_CONNECTED = 1
    } en_UNICARD_CONNECTION;


    /**
     * Volba simulované verze Unicard firmware.
     *
     * Ovlivňuje chování několika UNIMGR příkazů (cmdREV, cmdREVD,
     * cmdFDDMOUNT / cmdMOUNT, cmdGETCWD) a obsah SD runtime souborů
     * (mgr.mzf vs mgr800.mzf / mgr1500.mzf).
     *
     * - UNICARD_FW_UC1: originální MZ800UKP1 firmware revize 60.
     *   Dostupné jen na MZ-800. Manager V2.4.
     * - UNICARD_FW_UC3: komunitní fork Unicard3 v0.26 subtype 3.
     *   Dostupné na všech platformách (MZ-800, MZ-700, MZ-1500, ...).
     *   Manager V2.11.
     *
     * Změna za běhu způsobí force disconnect + reconnect Unicardu
     * (nelze průběžně přepínat při otevřeném souboru či běžícím
     * příkazu - klient by viděl nekonzistentní stav).
     */
    typedef enum en_UNICARD_FW {
        UNICARD_FW_UC1 = 0,
        UNICARD_FW_UC3 = 1
    } en_UNICARD_FW;


    /* uc3 firmware verze (sdílené konstanty pro cmdREV a cmdREVD).
     * MAJOR.MINOR odpovídá app_version_t z unicard-newmgr/.../version.h.
     * Při bumpu uc3 verze upravit zde. */
    #define UNICARD_FW_UC3_MAJOR    0       /* APP_VER_MAJOR */
    #define UNICARD_FW_UC3_MINOR    26      /* APP_VER_MINOR */
    #define UNICARD_FW_UC3_SUBTYPE  3       /* APP_VER_SUBTYPE */


    /**
     * Global state pro pending runtime-mismatch dialog.
     *
     * Nastaveno v unicard_set_connected pokud byla detekována mismatch
     * mezi obsahem SD runtime a aktuální fw_emulated. UI vrstva
     * (menu_unicard.cpp) v dalším render frame otevře modal dialog
     * s nabídkou {Reinit, Continue, Cancel}.
     *
     * Hodnoty:
     *   0                                   = žádný pending dialog
     *   UNICARD_RUNTIME_CHECK_DETECTED_UC1  = SD má uc1 ale fw je uc3
     *   UNICARD_RUNTIME_CHECK_DETECTED_UC3  = SD má uc3 ale fw je uc1
     *   UNICARD_RUNTIME_CHECK_UNKNOWN       = SD nemá žádný MGR
     */
    extern int g_unicard_runtime_mismatch_pending;

    /**
     * @brief Který FDC řadič spravuje Unicard API (mount DSK do FDC).
     *
     * Unicard API příkazy pro FDD mount/umount se propagují do zvolené
     * instance FDC. NONE = příkazy se nepropagují (Unicard FDD operace
     * jsou no-op). Změna nastavení ovlivní jen budoucí operace - již
     * namountované obrazy zůstávají beze změny.
     */
    typedef enum en_UNICARD_MANAGED_FDC {
        UNICARD_MANAGED_FDC_NONE = 0, /**< Unicard FDD příkazy se nepropagují. */
        UNICARD_MANAGED_FDC_FDC0 = 1, /**< Primární FDC (porty 0xD8-0xDF). Default. */
        UNICARD_MANAGED_FDC_FDC1 = 2  /**< Sekundární FDC (porty 0x58-0x5F). */
    } en_UNICARD_MANAGED_FDC;

    typedef struct st_UNICARD {
        en_UNICARD_CONNECTION connected;
        //char *sd_root;
        char *work_dir; // na unikarte je to vlastnost FatFS
        int readonly;   /* R/O režim emulované SD karty (1 = blokovat zápisy) */

        /* Volba simulované verze firmware (uc1 / uc3). Default UC3.
         * Na ne-MZ-800 platformách je vždy UC3 (uc1 jen na MZ-800). */
        en_UNICARD_FW fw_emulated;

        /* Pokud true (default), při Connect se ověří, zda obsah
         * {SD}/unicard/ odpovídá nastavené verzi firmware (mgr.mzf
         * pro uc1, mgr800.mzf/mgr1500.mzf pro uc3). Mismatch
         * triggeruje dialog s nabídkou reinicializace. */
        int runtime_check_on_connect;

        /* Který FDC řadič spravuje Unicard API (en_UNICARD_MANAGED_FDC).
         * Default FDC0. Typ int kvůli cfgelement INT bind. */
        int managed_fdc;
    } st_UNICARD;

    extern st_UNICARD g_unicard;

    extern void unicard_init ( void );
    extern void unicard_exit ( void );
    extern void unicard_reset ( void );
    extern char* unicard_get_mzfloader_image_filepath ( void );
    extern void unicard_set_connected ( en_UNICARD_CONNECTION conn );

    /**
     * @brief Vrátí, který FDC řadič Unicard API spravuje.
     * @return en_UNICARD_MANAGED_FDC (NONE / FDC0 / FDC1).
     */
    extern en_UNICARD_MANAGED_FDC unicard_get_managed_fdc ( void );

    /**
     * @brief Nastaví, který FDC řadič Unicard API spravuje.
     *
     * Ovlivní jen budoucí FDD operace; již namountované obrazy zůstávají.
     * Hodnota se persistuje do INI (cfg klíč `managed_fdc`).
     *
     * @param managed_fdc NONE / FDC0 / FDC1.
     */
    extern void unicard_set_managed_fdc ( en_UNICARD_MANAGED_FDC managed_fdc );

    extern char* unicard_get_sd_root_dirpath ( void );
    extern void unicard_set_sd_root_dirpath ( char *dirpath );

    /**
     * Vrátí aktuální simulovanou verzi Unicard firmware.
     *
     * @return UNICARD_FW_UC1 nebo UNICARD_FW_UC3
     */
    extern en_UNICARD_FW unicard_get_fw ( void );

    /**
     * Nastaví simulovanou verzi Unicard firmware.
     *
     * Pokud je nová hodnota odlišná od aktuální a Unicard je právě
     * connected, provede force disconnect + reconnect (otevřené file
     * handles se zavřou). Důvod: dispatch logika několika UNIMGR
     * příkazů (cmdREV, cmdREVD, cmdMOUNT, cmdGETCWD) závisí na verzi;
     * průběžné přepnutí by klientovi dalo nekonzistentní pohled.
     *
     * Na ne-MZ-800 platformách hodnota UNICARD_FW_UC1 není povolena
     * (volání se ignoruje, FW zůstane UC3).
     *
     * @param fw nová verze
     */
    extern void unicard_set_fw ( en_UNICARD_FW fw );

    /**
     * Vrátí pc_type hodnotu pro cmdREVD odpověď v uc3 režimu podle
     * aktuální mzarch platformy.
     *
     * Mapování (sjednoceno s app_version.pc_type v unicard-newmgr
     * version.h):
     *   MZ-800   -> 0  (PCTYPE_MZ800)
     *   MZ-1500  -> 3  (PCTYPE_MZ1500)
     *   MZ-700   -> 4  (PCTYPE_MZ700)
     *   MZ-2500  -> 5  (PCTYPE_MZ2500, pokud emulujeme)
     *   MZX      -> 6  (PCTYPE_MZX, pokud emulujeme)
     *
     * Pro neznámou mzarch vrátí 0 (= MZ-800 default).
     *
     * @return uint8_t pc_type
     */
    extern uint8_t unicard_get_uc3_pc_type ( void );

    /**
     * Výsledek detekce verze runtime v {SD}/unicard/.
     *
     * Slouží pro vyhodnocení shody s aktuálním g_unicard.fw_emulated
     * při Connect.
     */
    typedef enum en_UNICARD_RUNTIME_CHECK {
        /// SD/unicard/ neexistuje (= prvotní setup, install bez dotazu)
        UNICARD_RUNTIME_CHECK_NO_DIR = 0,
        /// SD/unicard/ existuje, runtime je uc1 (mgr.mzf)
        UNICARD_RUNTIME_CHECK_DETECTED_UC1 = 1,
        /// SD/unicard/ existuje, runtime je uc3 (mgr800/700/1500.mzf)
        UNICARD_RUNTIME_CHECK_DETECTED_UC3 = 2,
        /// SD/unicard/ existuje, ale žádný známý MGR soubor v něm
        UNICARD_RUNTIME_CHECK_UNKNOWN = 3
    } en_UNICARD_RUNTIME_CHECK;

    /**
     * Detekuje verzi runtime v {SD}/unicard/ podle existence MGR souboru.
     *
     * Konkrétní mapování:
     *   mgr.mzf  -> uc1 (V2.4)
     *   mgr800.mzf / mgr1500.mzf / mgr700.mzf -> uc3 (V2.11b)
     *
     * Pokud existují MGR souboru obou variant najednou (přechodový stav,
     * nedoporučený), priorita: uc3 (= novější). Detekce filemask
     * odpovídá per-mzarch (na MZ-800 hledá mgr800.mzf, ne mgr1500.mzf).
     *
     * @return en_UNICARD_RUNTIME_CHECK hodnota
     */
    extern en_UNICARD_RUNTIME_CHECK unicard_detect_runtime_fw ( void );

    /**
     * Smaže všechny MGR varianty v {SD}/unicard/ a znovu zavolá
     * init který nainstaluje MGR odpovídající požadovanému `fw` +
     * ostatní runtime soubory (mzfloader.mzq, mzfloader.cfg).
     *
     * Volba `fw` je nezávislá na aktuálním g_unicard.fw_emulated -
     * dovoluje uživateli z menu nainstalovat verzi, kterou aktivně
     * nesimuluje (např. pro testovací scénáře).
     *
     * Předpoklady: platný SD root path (Unicard nemusí být CONNECTED,
     * funkce čte path z config přímo).
     *
     * @param fw  UNICARD_FW_UC1 nebo UNICARD_FW_UC3
     * @return EXIT_SUCCESS / EXIT_FAILURE
     */
    extern int unicard_reinit_sd_for_fw ( en_UNICARD_FW fw );

    /**
     * Convenience wrapper - smaže MGR varianty a nainstaluje verzi
     * odpovídající aktuálnímu g_unicard.fw_emulated.
     *
     * Ekvivalent: unicard_reinit_sd_for_fw(g_unicard.fw_emulated).
     */
    extern int unicard_reinit_sd_for_current_fw ( void );

    /**
     * One-shot bypass runtime check pro příští unicard_set_connected.
     *
     * Použito v mismatch dialogu při volbě "Continue" - user chce
     * pokračovat s vědomě mismatchnutým SD obsahem; bez bypassu by
     * příští set_connected znovu detekoval mismatch a otevřel dialog
     * (nekonečná smyčka).
     */
    extern void unicard_bypass_runtime_check_once ( void );

    /**
     * Vrátí 1 pokud je zapnutá kontrola shody SD runtime s nastavenou
     * verzí firmware při Connect, 0 jinak.
     */
    extern int unicard_get_runtime_check_on_connect ( void );

    /**
     * Zabalí MZF data do MZQ (QD image) wrapperu kompatibilního s
     * Unicard boot loaderem.
     *
     * MZF -> MZQ je lossless ZA cenu truncate description z 90 B
     * (standardní MZF) na 38 B (kapacita MZQ MZF_HEAD bloku). Bajty
     * description 38..89 jsou DROP. Pro typický MZF kde description
     * je padding nulami to neva.
     *
     * MZQ layout (viz qdisk.h struct st_QDISK_*):
     *   [0..7]    QD_HEADER 8 B
     *   [8..81]   MZF_HEAD block 74 B (= 4B sign + 1B + 2B + 64B MZF
     *             header + 3B "CRC" placeholder)
     *   [82..]    MZF_BODY block (4B sign + 1B + 2B body_size + body
     *             + 3B "CRC")
     *
     * Výsledná velikost MZQ = 92 + body_size bajtů.
     *
     * CRC pole jsou placeholder "CRC" (literální ASCII), ne reálný
     * checksum - shoduje se se stávajícím konstantním mzfloader.mzq
     * (= emu/HW na CRC v MZQ nečeká).
     *
     * @param mzf_data    MZF binární data (128B header + body)
     * @param mzf_size    velikost MZF souboru (musí být >= 128)
     * @param out_buff    výstupní MZQ buffer (caller alokuje)
     * @param out_buff_size velikost out_buff (musí být >= 92 + body_size)
     * @return Velikost zapsaného MZQ image (= 92 + body_size).
     *         0 při chybě (neplatný MZF, malý buffer).
     */
    extern uint32_t unicard_make_mzq_from_mzf ( const uint8_t *mzf_data, uint32_t mzf_size,
                                                  uint8_t *out_buff, uint32_t out_buff_size );

    /**
     * Sestaví fallback MZQ image z embedded SDERR MZF pro situaci, kdy
     * {SD}/unicard/mzfloader.mzq neexistuje a chceme přesto bootovat.
     *
     * Volba zdrojového SDERR MZF:
     *   - mzarch MZ-1500            -> c_UNICARD_SDERR1500_MZF
     *   - mzarch MZ-800 i MZ-700    -> c_UNICARD_SDERR800_MZF
     *
     * Fallback platí pouze pro uc3 FW (V2.11b ekosystém). Pro uc1
     * firmware funkce vrací 0 - klasický UNIBOOT V2.0 nemá ekvivalentní
     * "chybový" loader, takže pro uc1 ponecháváme původní chybové
     * chování qdisk mountu.
     *
     * @param out_buff      výstupní MZQ buffer (caller alokuje)
     * @param out_buff_size velikost out_buff (musí být >= 92 + body_size
     *                      vybraného SDERR; pro existující SDERR-800
     *                      stačí ~1100 B, doporučeno >= 92 + 0x10000)
     * @return Velikost zapsaného MZQ image (>0), nebo 0 pokud fallback
     *         není aplikovatelný (uc1, malý buffer, neznámý mzarch).
     */
    extern uint32_t unicard_build_fallback_mzq ( uint8_t *out_buff, uint32_t out_buff_size );

    /**
     * Zapne/vypne kontrolu shody SD runtime s firmware verzí při Connect.
     *
     * @param enabled 1 = zapnout (default), 0 = vypnout
     */
    extern void unicard_set_runtime_check_on_connect ( int enabled );

    /**
     * Manuální reinicializace runtime souborů v {SD}/unicard/.
     *
     * Vytvoří chybějící mgr.mzf (resp. per-arch mgr1500.mzf),
     * mzfloader.mzq a mzfloader.cfg v {SD}/unicard/. Existující
     * soubory zůstávají nedotčené.
     *
     * Automatický init při unicard_set_connected běží jen pokud
     * neexistuje SD root - tato funkce je manuální cestou volaná
     * z topmenu (= "Reinitialize Runtime") když user chce obnovit
     * smazaný runtime soubor v existující SD struktuře.
     *
     * Předpoklady: Unicard musí být CONNECTED (= platný SD root path).
     *
     * @return  EXIT_SUCCESS / EXIT_FAILURE.
     */
    extern int unicard_reinit_sd_runtime ( void );

    extern void unicard_write_byte ( uint16_t port, uint8_t data );
    extern uint8_t unicard_read_byte ( uint16_t port );

    extern void unicard_read_rtc ( st_UNICARD_RTC *rtc );

    extern FRESULT unicard_chdir ( char *dirpath );
    extern int unicard_get_cwd ( FRESULT *ff_res, char *buff, int buff_size );

    extern void unicard_dir_init ( st_UNICARD_DIR *dir );
    extern FRESULT unicard_dir_open ( st_UNICARD_DIR *dir, char *dirpath );
    extern FRESULT unicard_dir_close ( st_UNICARD_DIR *dir );
    extern FRESULT unicard_dir_read_filelist ( st_UNICARD_DIR *dir, char *buff, int buff_size );

    /**
     * Načtení další položky adresáře jako binární FILINFO.
     *
     * Položky "." a ".." jsou vynechány (g_dir_read_name je sám ignoruje).
     * Soubory s LFN délkou >= _MAX_LFN se přeskočí (cyklus uvnitř funkce).
     *
     * @param[out] finfo    naplněno pokud *eof == 0
     * @param[out] eof      1 pokud žádná další položka, 0 jinak
     * @retval FR_OK
     * @retval FR_DISK_ERR  dir není otevřený nebo g_stat selhal
     */
    extern FRESULT unicard_dir_read_filinfo ( st_UNICARD_DIR *dir, st_UNICARD_FILINFO *finfo, int *eof );
    extern int unicard_dir_is_open ( st_UNICARD_DIR *dir );

    extern void unicard_file_init ( st_UNICARD_FILE *file );
    extern FRESULT unicard_file_open ( st_UNICARD_FILE *file, char *filepath, uint8_t fa_mode );
    extern FRESULT unicard_file_close ( st_UNICARD_FILE *file );
    extern FRESULT unicard_file_read ( st_UNICARD_FILE *file, uint8_t *buff, uint32_t buff_size, uint32_t *read_len );

    /**
     * Zápis dat do otevřeného souboru.
     *
     * @param[out] write_len    skutečně zapsaný počet bytů
     * @retval FR_OK            úspěch (i částečný zápis vrací FR_OK; volající
     *                          ověří write_len == buff_size pro úplnost)
     * @retval FR_INVALID_PARAMETER soubor není otevřený
     * @retval FR_DISK_ERR      fwrite chyba
     */
    extern FRESULT unicard_file_write ( st_UNICARD_FILE *file, const uint8_t *buff, uint32_t buff_size, uint32_t *write_len );

    /**
     * Sync otevřeného souboru (flush + OS-level sync).
     */
    extern FRESULT unicard_file_sync ( st_UNICARD_FILE *file );

    /**
     * Zkrácení otevřeného souboru na aktuální pozici (ftell).
     * Implementováno přes _chsize_s (Windows) nebo ftruncate (POSIX).
     */
    extern FRESULT unicard_file_truncate ( st_UNICARD_FILE *file );
    extern int unicard_file_is_open ( st_UNICARD_FILE *file );
    extern int unicard_file_is_eof ( st_UNICARD_FILE *file );

    /**
     * Aktuální pozice ukazatele v otevřeném souboru.
     * Vrací 0 pokud soubor není otevřený (file->fh == NULL) nebo ftell selže.
     */
    extern uint32_t unicard_file_tell ( st_UNICARD_FILE *file );

    /**
     * Změna pozice ukazatele v otevřeném souboru.
     *
     * Módy (kompatibilní s firmware do_cmdSEEK):
     *   0 (SET):      offset přímo - fseek(fh, offset, SEEK_SET)
     *   1 (END):      f_size(file) - offset (offset = vzdálenost OD KONCE)
     *   2 (REL_UP):   tell(fh) + offset
     *   3 (REL_DOWN): tell(fh) - offset
     *
     * @retval FR_OK              úspěch
     * @retval FR_INVALID_PARAMETER neplatný mode nebo zavřený soubor
     * @retval FR_DISK_ERR        fseek/ftell selhalo
     */
    extern FRESULT unicard_file_seek ( st_UNICARD_FILE *file, uint8_t mode, uint32_t offset );

    /**
     * Velikost otevřeného souboru v bytech. Implementováno přes
     * fseek SEEK_END + ftell + fseek zpět. Vrací 0 pokud soubor není
     * otevřený.
     */
    extern uint32_t unicard_file_size ( st_UNICARD_FILE *file );

    /**
     * Naplnění st_UNICARD_FILINFO informacemi o cestě (soubor nebo
     * adresář). Cesta je relativně k SD root, prochází path canonicalizací.
     *
     * Mapování z hostitelského FS (POSIX g_stat):
     *   fsize:    statbuf.st_size
     *   fdate:    z localtime(statbuf.st_mtime), FAT format
     *   ftime:    dtto
     *   fattrib:  jen AM_DIR bit pokud je adresář (ostatní 0)
     *   fname:    basename truncated na 12 znaků (zjednodušení 8.3)
     *   lfname:   basename (full, max _MAX_LFN-1 znaků)
     *
     * @retval FR_OK         úspěch
     * @retval FR_NO_FILE    cesta neexistuje
     * @retval FR_DISK_ERR   g_stat selhal
     */
    extern FRESULT unicard_stat ( const char *path, st_UNICARD_FILINFO *finfo );

    /**
     * Informace o volném prostoru SD root volume.
     *
     * Reálná Unicard počítá v FAT clusterech/sektorech (1 sektor = 512 B).
     * Emulace převede výsledek statvfs / GetDiskFreeSpaceEx na byty / 512
     * jako přibližný ekvivalent.
     *
     * @param[out] total_sect   celkový počet 512B "sektorů"
     * @param[out] free_sect    počet volných 512B "sektorů"
     * @retval FR_OK            úspěch
     * @retval FR_DISK_ERR      OS volání selhalo
     */
    extern FRESULT unicard_get_volume_info ( uint32_t *total_sect, uint32_t *free_sect );

    /**
     * Vytvoření adresáře v SD root.
     *
     * @retval FR_OK             úspěch
     * @retval FR_EXIST          adresář/soubor s tímto jménem už existuje
     * @retval FR_DISK_ERR       g_mkdir selhal
     */
    extern FRESULT unicard_mkdir ( const char *path );

    /**
     * Smazání souboru nebo prázdného adresáře.
     *
     * Pro neprázdný adresář volání selže (g_remove vrací non-zero, mapování
     * na FR_DENIED). Pokud cesta neexistuje, vrací FR_NO_FILE.
     *
     * @retval FR_OK
     * @retval FR_NO_FILE        cesta neexistuje
     * @retval FR_DENIED         nelze smazat (např. neprázdný dir)
     */
    extern FRESULT unicard_unlink ( const char *path );

    /**
     * Přejmenování / přesun souboru nebo adresáře v SD root.
     *
     * Cross-FS přesun by selhal s FR_DENIED, ale v praxi jsou oba paths
     * pod SD root (stejné FS), takže g_rename vždy uspěje pokud target
     * neexistuje.
     *
     * @retval FR_OK
     * @retval FR_NO_FILE        zdroj neexistuje
     * @retval FR_EXIST          cíl už existuje
     * @retval FR_DENIED         g_rename selhal z jiného důvodu
     */
    extern FRESULT unicard_rename ( const char *old_path, const char *new_path );

    /**
     * Nastavení časové značky souboru/adresáře.
     *
     * Vstupní 6 bytů odpovídá UNIMGR cmdUTIME formátu:
     *   fat_dt[0]: D (1..31)
     *   fat_dt[1]: M (1..12)
     *   fat_dt[2]: Y - 1980 (0..127)
     *   fat_dt[3]: H (0..23)
     *   fat_dt[4]: M (0..59)
     *   fat_dt[5]: S (0..59)
     *
     * Implementováno přes utime() (POSIX) / _utime() (Windows). FAT
     * sekundová granularita 2s je převedena na POSIX 1s (ztráta
     * precision je tolerovatelná).
     *
     * @retval FR_OK             úspěch
     * @retval FR_NO_FILE        cesta neexistuje
     * @retval FR_INVALID_PARAMETER neplatná hodnota datumu/času
     * @retval FR_DISK_ERR       utime selhalo
     */
    extern FRESULT unicard_utime ( const char *path, const uint8_t fat_dt[6] );

    /**
     * No-op stub pro chmod (FAT atribut bity nelze 1:1 mapovat na POSIX
     * mode). Validuje vstup a vrací FR_OK pokud cesta existuje.
     *
     * Důvod: hostitelský FS nedokáže zachovat AM_RDO/AM_HID/AM_SYS/AM_ARC
     * mezi spuštěními bez shadow metadata souboru. Pro hlavní use case
     * (CP/M na FAT) je akceptovatelné. Pokud bude potřeba, lze v budoucnu
     * přepnout na shadow file v adresáři SD root.
     *
     * @retval FR_OK             úspěch (i když atributy nebyly změněny)
     * @retval FR_NO_FILE        cesta neexistuje
     */
    extern FRESULT unicard_chmod ( const char *path, uint8_t attr, uint8_t mask );

    extern void unicard_fdc_mount ( uint8_t drive_id, char *filepath );
    void unicard_ui_select_sd_root_directory(void);

    /**
     * Bezpečné vyhodnocení emu-supplied cesty proti SD root jailu.
     *
     * Vstupní cesta z emulovaného Z80 programu se normalizuje (sloučení
     * "..", ".", "//" segmentů, konverze backslash na forward slash) a
     * vyhodnotí proti SD root adresáři. Pokud by normalizovaná cesta
     * vedla mimo SD root (tj. emu poslal víc ".." segmentů než kolik
     * jich může absorbovat zanoření work_dir), funkce zaloguje g_warning
     * s detaily pokusu a vrátí NULL.
     *
     * Implementace:
     *   1. Doplnění work_dir pokud emu_path nezačíná '/'
     *   2. Detekce escape: počet ".." segmentů > počet skutečných úrovní
     *   3. Build absolutní cesty SD_root + normalized
     *
     * @param emu_path  Cesta z emulovaného programu (relativní nebo
     *                  začínající '/' = SD-rooted, NE absolutní v
     *                  hostitelském FS). NULL nebo prázdný řetězec
     *                  ekvivalent SD rootu.
     * @return  Nově alokovaná absolutní cesta v rámci SD root, nebo
     *          NULL pokud cesta opouští jail. Volající vlastní výsledek
     *          a musí ho uvolnit přes baseui_tools_mem_free().
     */
    extern char* unicard_jail_resolve ( const char *emu_path );

    /**
     * Vyhodnocení emu-supplied cesty proti jailu s vrácením i naked
     * (= relativní k SD root) podoby cesty.
     *
     * Stejné chování jako unicard_jail_resolve, ale navíc vrací
     * normalizovanou relativní podobu (vede vždy zpod '/' = SD root).
     * Některá call sites tu potřebují pro uložení do struktur
     * (st_UNICARD_FILE.filepath, st_UNICARD_DIR.dirpath, work_dir).
     *
     * @param emu_path     Vstupní cesta z emulovaného programu.
     * @param[out] naked   Naplněno naked cestou pokud return != NULL.
     *                     Caller vlastní (baseui_tools_mem_free).
     * @return  Absolutní cesta v SD root nebo NULL při escape. Při
     *          NULL je *naked taky NULL.
     */
    extern char* unicard_jail_resolve_with_naked ( const char *emu_path, char **naked );

    /**
     * Test "soft" jail bez logování (pro speciální call sites, např.
     * dir_read kde sestavujeme cestu z DIR struct a chceme jen
     * abort flag).
     *
     * @return  TRUE pokud kombinovaná cesta zůstává uvnitř SD root.
     */
    extern int unicard_jail_check_inside ( const char *naked_path );

    /* ------------------------------------------------------------------
     * R/O režim emulované SD karty
     *
     * Při aktivním read-only se blokují všechny modifikační operace
     * Unicard API (file write, truncate, mkdir, unlink, rename, utime,
     * file_open s write/create flagy). Operace vrátí FR_WRITE_PROTECTED
     * a vystaví per-session warning na konzoli (první výskyt info,
     * další potlačené do restartu emulátoru).
     * ------------------------------------------------------------------ */

    /**
     * Nastavení R/O režimu emulované SD karty.
     *
     * Při zapnutí vystaví info zprávu do konzole. Vypnutí je tiché.
     * Změna stavu resetuje per-session counter potlačení warningů.
     */
    extern void unicard_set_readonly ( int readonly );

    /**
     * Test aktuálního R/O stavu.
     *
     * @return  TRUE pokud je SD karta v R/O režimu.
     */
    extern int unicard_test_readonly ( void );

    /**
     * Vystavení per-session warningu o zablokované modifikační operaci.
     *
     * První volání v session vypíše g_warning, další jsou potlačené.
     * Counter se resetuje při restart emulátoru (statická proměnná v
     * .data segmentu) nebo při změně readonly flagu.
     */
    extern void unicard_warn_readonly_blocked ( void );

#define UNICARD_TEST_IS_CONNECTED ( g_unicard.connected == UNICARD_CONNECTION_CONNECTED )

/* Default Unicard SD root obsahuje arch suffix ("SD-<arch>") - kazda
 * platforma ma vlastni adresar (drive sdilene "SD" davaly zmatecny obsah
 * pri prepinani targetu mezi MZ-700/800/1500). Od mzhal kroku 7 se
 * sklada RUNTIME z g_mzhal.arch_name - viz unicard_default_sd_root()
 * v unicard.c; compile-time makro z MZ_PLATFORM_SUFFIX zruseno. */
#define UNICARD_DEFAULT_DIR_MODE     0775

#define UNICARD_UNIMGR_DIR                  "unicard"
    // /unicard/fd0.cfg
#define UNICARD_FDCFG_FILE1                 "/unicard/fd"
#define UNICARD_FDCFG_FILE2                 ".cfg"

#define UNICARD_UNIMGR_MZFLOADER_MZQ        "mzfloader.mzq"
#define UNICARD_UNIMGR_MZFLOADER_CFG        "mzfloader.cfg"
#define UNICARD_UNIMGR_MGR_MZF              "mgr.mzf"
#define UNICARD_UNIMGR_MGR800_MZF           "mgr800.mzf"
#define UNICARD_UNIMGR_MGR1500_MZF          "mgr1500.mzf"
#define UNICARD_UNIMGR_MGR700_MZF           "mgr700.mzf"

#ifdef __cplusplus
}
#endif

#endif /* UNICARD_H */

