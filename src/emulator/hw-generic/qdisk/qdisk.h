/*
 * File:   qdisk.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 11. února 2016, 18:04
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

#ifndef QDISK_H
#define QDISK_H

#include <stdint.h>

#ifdef WINDOWS
#define COMPILE_FOR_EMULATOR
#undef COMPILE_FOR_UNICARD
#undef FS_LAYER_FATFS
#elif defined(LINUX)
#define COMPILE_FOR_EMULATOR
#undef COMPILE_FOR_UNICARD
#undef FS_LAYER_FATFS
#else
#undef COMPILE_FOR_EMULATOR
#define COMPILE_FOR_UNICARD
#define FS_LAYER_FATFS
#endif

#ifdef COMPILE_FOR_EMULATOR
#include "libs/generic_driver/generic_driver.h"

/**
 * @brief Storage mode pro IMAGE / UNICARD QD image.
 *
 * Volba se persistuje v INI klíči `mz1f11_storage_mode` (TEXT) a aplikuje se
 * při mount v `qdisk_open_image()`. Hodnota zůstává v `st_QDISK.storage_mode`
 * pro runtime větvení (flush guard, switch popup).
 *
 * Hodnoty:
 *  - `QDISK_STORAGE_CACHED`  (0): celé image v RAM (memory_driver), sync do
 *    souboru při reset / umount / exit / manual. Default.
 *  - `QDISK_STORAGE_DIRECT`  (1): immediate write-through na FILE*
 *    (file_driver). Žádný RAM buffer.
 *  - `QDISK_STORAGE_DISCARD` (2): celé image v RAM (memory_driver), ale NIKDY
 *    sync zpět. Pro test runs bez modifikace zdrojového souboru.
 *
 * Existuje jen v emulátor buildu (COMPILE_FOR_EMULATOR); UC FW build storage
 * mode nemá - FS_LAYER cesta je vždy ekvivalentní DIRECT.
 */
typedef enum en_QDISK_STORAGE_MODE
{
    QDISK_STORAGE_CACHED  = 0,
    QDISK_STORAGE_DIRECT  = 1,
    QDISK_STORAGE_DISCARD = 2
} en_QDISK_STORAGE_MODE;
#endif

// tuto hodnotu jseme kdysi v konferenci namerili jako max
// #define QDISK_FORMAT_SIZE            61454

// v teto velikosti se ke mne dostal udajne "lehce nafoukly" image od Mikese
#define QDISK_FORMAT_SIZE 82958

/*
 *  Podle provedenych mereni se povedlo na QD zapsat nad ramec formatu
 * jeste jeden MZF o velikosti 0x1a80
 *
 */
#define QDISK_IMAGE_MAX_SIZE (QDISK_FORMAT_SIZE + 84 + 0x1a80)

#define QDISK_DISCONNECTED 0
#define QDISK_CONNECTED 1

#define QDSIO_CHANNEL_A 0
#define QDSIO_CHANNEL_B 1

#define QDSTS_NO_DISC 0
#define QDSTS_IMG_READY 1
#define QDSTS_HEAD_HOME 2
#define QDSTS_IMG_SYNC 4
#define QDSTS_IMG_READONLY 8

#define QDISK_TYPE_IMAGE 0
#define QDISK_TYPE_VIRTUAL 1
#define QDISK_TYPE_UNICARD 2

#ifndef FS_LAYER_FATFS
#define QDISKK_FILENAME_LENGTH 1024
#endif

#define QDISK_VIRT_TEMP_FNAME "qd_temp.tmp"

#define QDISK_MZF_FILENAME_LENGTH 17

typedef enum en_QDSIO_ADDR
{
    QDSIO_ADDR_DATA_A = 0,
    QDSIO_ADDR_DATA_B,
    QDSIO_ADDR_CTRL_A,
    QDSIO_ADDR_CTRL_B
} en_QDSIO_ADDR;

typedef enum en_QDSIO_REGADRR
{
    QDSIO_REGADDR_0 = 0, /* zakladni prikazy */
    QDSIO_REGADDR_1,     /* nastaveni generovani interruptu a signalu Wait/Ready */
    QDSIO_REGADDR_2,     /* vektor preruseni (zastoupen jen u kanalu B) */
    QDSIO_REGADDR_3,     /* rizeni prijmu */
    QDSIO_REGADDR_4,     /* nastaveni vlastnosti prenosu */
    QDSIO_REGADDR_5,     /* nastaveni odesilani */
    QDSIO_REGADDR_6,     /* sync1 */
    QDSIO_REGADDR_7      /* sync2 */
} en_QDSIO_REGADRR;

typedef enum en_QDSIO_WR0CMD
{
    QDSIO_WR0CMD_NONE = 0,
    QDSIO_WR0CMD_SDLC_STOP,        /* ukoncit vysilani (pouze v rezimu SDLC) */
    QDSIO_WR0CMD_RESET_INTF,       /* reset priznaku preruseni */
    QDSIO_WR0CMD_RESET,            /* reset kanalu */
    QDSIO_WR0CMD_ENABLE_INT,       /* povol generovani interruptu pri dalsim prijatem znaku */
    QDSIO_WR0CMD_RESET_OUTBUF_INT, /* resetuj generovani interruptu pri prazdnem output bufferu */
    QDSIO_WR0CMD_RESET_ERRFL,      /* reset  priznaku chyby */
    QDSIO_WR0CMD_RETI              /* navrat z preruseni */
} en_QDSIO_WR0CMD;

typedef struct st_QDSIO_CHANNEL
{
    char name;
    en_QDSIO_REGADRR REG_addr;
    uint8_t Wreg[8];
    uint8_t Rreg[3];
} st_QDSIO_CHANNEL;

typedef enum st_QDISK_VRTSTS
{
    QDISK_VRTSTS_QDHEADER,
    QDISK_VRTSTS_MZFHEAD,
    QDISK_VRTSTS_MZFBODY,
    QDISK_VRTSTS_FREE_FILEAREA,
    QDISK_VRTSTS_WR_MZFHEAD,
    QDISK_VRTSTS_WR_MZFBODY,
    QDISK_VRTSTS_FORMATING,
} st_QDISK_VRTSTS;

/* 0x0000 - 0x0007 ( 8 bajtu ) */
typedef struct st_QDISK_HEADER
{
    uint8_t start_sign[4];     /* 0x00, 0x16, 0x16, 0xa5 */
    uint8_t file_blocks_count; /* pocet pouzitych bloku (resp. pocet souboru * 2) */
    uint8_t crc[3];            /* C, R, C */
} st_QDSTRT_BLOCK;

/* 0x0008 - 0x00051 ( 10 + 64 = 74 bajtu ) */
typedef struct st_QDISK_MZF_HEADER
{
    uint8_t start_sign[4];   /* 0x00, 0x16, 0x16, 0xa5 */
    uint8_t mzf_header_sign; /* 0x00 */
    uint16_t data_size;      /* 0x40, 0x00 - tzn: 64 bajtu */
    uint8_t mzf_ftype;
    uint8_t mzf_fname[16];
    uint8_t mzf_fname_end; /* 0x0d */
    uint8_t unused1[2];    /* 0x00 */
    uint16_t mzf_size;
    uint16_t mzf_start;
    uint16_t mzf_exec;
    uint8_t mzf_header_description[38]; /* Prvnich 38 bajtu z mzf description */
    uint8_t crc[3];                     /* C, R, C */
} st_QDISK_MZF_HEADER;

/* 0x0052 - 0x???? ( 10 + body_size bajtu ) */
typedef struct st_QDISK_MZF_BODY
{
    uint8_t start_sign[4];   /* 0x00, 0x16, 0x16, 0xa5 */
    uint8_t mzf_body_sign;   /* 0x05 */
    uint16_t data_size;      /* body_size */
    uint8_t mzf_body[65535]; /* body [ body_size ] */
    uint8_t crc[3];          /* C, R, C */
} st_QDISK_MZF_BODY;

typedef struct st_QDISK
{
    unsigned connected;
    unsigned type;
    unsigned status;

    st_QDSIO_CHANNEL channel[2];
#ifdef COMPILE_FOR_UNICARD
    /* UC FW build: drží FILE* handle nad otevřeným .mzq souborem v FATFS.
     * V emulátoru build se I/O cesta řeší přes generic_driver (st_HANDLER
     * handler níže), tohle pole tedy v emulátoru build neexistuje. */
    FILE *image_fp;
#endif
    uint16_t out_crc16;
    unsigned image_position;

    unsigned virt_status;
    unsigned virt_files_count;
    unsigned virt_file_num;
    char virt_saved_filename[QDISK_MZF_FILENAME_LENGTH + 4];
    uint16_t virt_mzfbody_size;
    FILE *mzf_fp;
#ifdef COMPILE_FOR_EMULATOR
    char *ui_std_filepath;
    char *ui_virt_filepath;
    int ui_wrprt;

    /* Fáze 1 (qdisk-rewrite): generic_driver napojení pro IMAGE / UNICARD.
     * VIRTUAL režim handler nepoužívá. Storage mode volba (CACHED / DIRECT
     * / DISCARD) přijde v pozdější fázi - aktuálně se vždy používá
     * g_memory_driver_static (= CACHED ekvivalent). */
    st_HANDLER handler;        /**< generic_driver handle pro IMAGE / UNICARD */
    int handler_valid;         /**< 1 = handler je otevřený, je třeba close */
    char filename[1024];       /**< plná cesta k aktuálně mountnutému image souboru */

    /* Fáze 2 (qdisk-rewrite): 3-state Read-Only model (paralela k FDC).
     *
     *   user_readonly = persistentní user volba (CFGELM "mz1f11_write_protected")
     *   fs_readonly   = runtime auto-detekce z filesystému (W_OK access)
     *   readonly      = efektivní (= user_readonly || fs_readonly)
     *
     * Status flag QDSTS_IMG_READONLY i handler.status READ_ONLY bit reflektují
     * efektivní hodnotu. Pole se přepočítává v qdisk_open_image() a v
     * qdisk_set_write_protected(); v qdisk_close() se vynulují. */
    int user_readonly;         /**< persistentní user pref (mirror CFGELM mz1f11_write_protected) */
    int fs_readonly;           /**< runtime: soubor nemá W_OK přístup */
    int readonly;              /**< efektivní = user_readonly || fs_readonly */

    /* Fáze 3 (qdisk-rewrite): volba storage mode (CACHED / DIRECT / DISCARD).
     * Hodnota se čte z CFGELM mz1f11_storage_mode v qdisk_open_image() a řídí
     * jaký generic_driver se použije pro mount + zda se flushne při close. */
    en_QDISK_STORAGE_MODE storage_mode; /**< aktuální storage mode mountnutého image */
#endif
} st_QDISK;

extern st_QDISK g_qdisk;


#define QDISK_TEST_CONNECTED (g_qdisk.connected == QDISK_CONNECTED)
#define QDISK_TEST_TYPE_IMAGE (g_qdisk.type == QDISK_TYPE_IMAGE)
#define QDISK_TEST_TYPE_VIRTUAL (g_qdisk.type == QDISK_TYPE_VIRTUAL)
#define QDISK_TEST_TYPE_UNICARD (g_qdisk.type == QDISK_TYPE_UNICARD)


#define QDISK_TEST_IS_WRITE_PROTECTED (g_qdisk.status & QDSTS_IMG_READONLY)

#define QDISK_SET_CONNECTED() (g_qdisk.connected = QDISK_CONNECTED)
#define QDISK_SET_DISCONNECTED() (g_qdisk.connected = QDISK_DISCONNECTED)

#define QDISK_SET_TYPE_IMAGE() (g_qdisk.type = QDISK_TYPE_IMAGE)
#define QDISK_SET_TYPE_VIRTUAL() (g_qdisk.type = QDISK_TYPE_VIRTUAL)
#define QDISK_SET_TYPE_UNICARD() (g_qdisk.type = QDISK_TYPE_UNICARD)

#ifdef COMPILE_FOR_EMULATOR
#define QDISK_TEST_STD_PATH_IS_SET (g_qdisk.ui_std_filepath != NULL && g_qdisk.ui_std_filepath[0] != 0)
#define QDISK_TEST_VIRT_PATH_IS_SET (g_qdisk.ui_virt_filepath != NULL && g_qdisk.ui_virt_filepath[0] != 0)
#define qdisc_get_ui_std_filepath() (g_qdisk.ui_std_filepath)
#define qdisc_get_ui_virt_filepath() (g_qdisk.ui_virt_filepath)
#endif


#ifdef __cplusplus
extern "C"
{
#endif

    void qdisk_init(void);
    void qdisk_exit(void);
    uint8_t qdisk_read_byte(en_QDSIO_ADDR SIO_addr);
    void qdisk_write_byte(en_QDSIO_ADDR SIO_addr, uint8_t value);
    void qdisk_close(void);
    void qdisk_open(void);
    void qdisk_ui_mount(void);
    void qdisk_umount(void);
    void qdisk_set_write_protected(int value);
    void qdisk_create_image(char *filename);
    void qdisk_activate_unicard_boot_loader(void);
    void qdisk_deactivate_unicard_boot_loader(void);
    int qdisc_get_write_protected(void);

#ifdef COMPILE_FOR_EMULATOR
    /**
     * @brief Vrať aktuální storage mode jako string z CFGELM.
     *
     * Hodnota je vždy jedna z "cached"/"direct"/"discard" (CFGELM default
     * "cached" garantuje, že nevrací NULL nebo prázdný string).
     *
     * @return Konstantní string z CFGELM bufferu. NEUVOLŇOVAT.
     */
    const char *qdisk_get_storage_mode_str(void);

    /**
     * @brief Aplikuje switch storage mode - zapíše do CFGELM a remountuje.
     *
     * Volá `cfgelement_set_text_value(g_elm_storage_mode, target)` a pokud je
     * aktuálně mountnutý image, provede close + open (čte INI znovu, čímž se
     * aplikuje nový mode i nový driver). Bez popup logiky - volá se přímo
     * pokud není potřeba prompt (UI vrstva má vlastní switch popup).
     *
     * @param target_mode "cached" | "direct" | "discard". Neznámá hodnota =
     *                    fallback na CACHED (přes parser).
     */
    void qdisk_apply_storage_mode_switch(const char *target_mode);

    /**
     * @brief Existují aktuálně dirty RAM změny v memory bufferu?
     *
     * Pro switch popup trigger - oznámí, zda by switch mode v CACHED nebo
     * DISCARD znamenal ztrátu dat (memspec.updated && handler MEMORY).
     *
     * @return 1 = handler je memory + updated flag set, 0 = nic dirty
     *         (mounted DIRECT, nemountnut, nebo no writes since open).
     */
    int qdisk_drive_has_ram_changes(void);

    /**
     * @brief Existují neuložené změny, které čekají na flush do souboru?
     *
     * Veřejný equivalent interního flush-guard testu. Vrací 1 pouze pokud
     * by `qdisk_sync_drive()` měl skutečně co zapsat - tj. handler je MEMORY,
     * není R/O, storage_mode == CACHED a memspec.updated flag je set. Pro
     * UI accelerator (Save now menu item enable/disable) a snapshot save
     * warning hook.
     *
     * @return 1 = dirty (CACHED + memory + !RO + updated), 0 = clean nebo n/a.
     */
    int qdisk_drive_has_unsaved_changes(void);

    /**
     * @brief Flushne RAM buffer image souboru do souboru (jen pokud je co).
     *
     * Veřejný obal nad interním flush helperem. Provede save pouze pokud
     * `qdisk_drive_has_unsaved_changes()` vrací 1; jinak no-op s návratem 1.
     * Po úspěšném flushi se `memspec.updated` vynuluje. Volat z UI vrstvy
     * (Save now), lifecycle hooků (close, motor-off, exit) a snapshot
     * save pre-flush.
     *
     * @return 1 = OK (uloženo, nebo nebylo co), 0 = chyba zápisu.
     */
    int qdisk_sync_drive(void);

    /**
     * @brief Vynucený save RAM bufferu do souboru (bypass storage_mode gate).
     *
     * Provede `generic_driver_save_memory` na aktuální handler pokud je
     * typu MEMORY a není R/O - bez ohledu na storage_mode (= zapíše i pro
     * DISCARD). Po úspěšném flushi se `memspec.updated` vynuluje. Použití:
     * "Save and switch" akce ve storage switch popupu (DISCARD -> jiný mod),
     * kde user explicitně chce uložit i přes aktuální DISCARD nastavení.
     *
     * @return 1 = OK, 0 = neaplikovatelné (no handler / file / RO) nebo
     *         chyba zápisu.
     */
    int qdisk_drive_force_save_to_file(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* QDISK_H */
