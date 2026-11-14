/**
 * @file   generic_driver.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.2
 * @brief  Univerzální I/O abstrakce pro práci s datovými bloky (soubory, paměťové buffery).
 *
 * @par Historie:
 * - 2026-11-09: memory_driver přes makra baseui_tools_mem_* (2.0.2)
 *
 * Virtualizuje datovou vrstvu — nadřazená vrstva používá vždy stejné API
 * nezávisle na typu úložiště. Obsahuje definice datových typů, enumů,
 * callback prototypů a inline bootstrap funkcí.
 *
 * Předvolby pro podmíněnou kompilaci (definovat PŘED #include):
 *
 *   GENERIC_DRIVER_FILE    — povolit souborový handler (st_HANDLER_FILESPC)
 *   GENERIC_DRIVER_MEMORY  — povolit paměťový handler (st_HANDLER_MEMSPC)
 *
 *   GENERIC_DRIVER_FILE_CB — zabudované callbacky pro souborový driver
 *                            (open/close/read/write/truncate pro FILE*)
 *   GENERIC_DRIVER_MEMORY_CB — zabudované callbacky pro paměťový driver
 *                              (open/close/read/write/prepare/truncate
 *                               pro malloc buffer)
 *   GENERIC_DRIVER_MEMORY_CB_USE_REALLOC — povolí automatické zvětšování
 *                                          bufferu v zabudovaném prepare_cb
 *                                          (vyžaduje GENERIC_DRIVER_MEMORY_CB)
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef GENERIC_DRIVER_H
#define GENERIC_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>


/** @brief Výchozí aktivace obou typů handlerů.
 *  Při použití v jiném projektu lze před #include definovat jen ty, které jsou potřeba. */
#ifndef GENERIC_DRIVER_NO_DEFAULTS
/** @brief Povolit souborový handler (st_HANDLER_FILESPC) */
#define GENERIC_DRIVER_FILE
/** @brief Povolit paměťový handler (st_HANDLER_MEMSPC) */
#define GENERIC_DRIVER_MEMORY
#endif


    /** @brief Chyba handleru — popisuje stav konkrétního otevřeného média */
    typedef enum en_HANDLER_ERROR {
        HANDLER_ERROR_NONE = 0,             /**< žádná chyba */
        HANDLER_ERROR_NOT_READY = 1,        /**< handler není připravený */
        HANDLER_ERROR_WRITE_PROTECTED = 2,  /**< handler je chráněný proti zápisu */
        HANDLER_ERROR_USER                  /**< první volná hodnota pro uživatelské chyby */
    } en_HANDLER_ERROR;


    /** @brief Chyba driveru — popisuje selhání I/O operace */
    typedef enum en_GENERIC_DRIVER_ERROR {
        GENERIC_DRIVER_ERROR_NONE = 0,          /**< žádná chyba */
        GENERIC_DRIVER_ERROR_NOT_READY,         /**< driver není připravený */
        GENERIC_DRIVER_ERROR_SEEK,              /**< chyba při hledání pozice (seek) */
        GENERIC_DRIVER_ERROR_READ,              /**< chyba při čtení */
        GENERIC_DRIVER_ERROR_WRITE,             /**< chyba při zápisu */
        GENERIC_DRIVER_ERROR_SIZE,              /**< chyba velikosti (offset + count > size) */
        GENERIC_DRIVER_ERROR_MALLOC,            /**< chyba alokace paměti (malloc) */
        GENERIC_DRIVER_ERROR_REALLOC,           /**< chyba realokace paměti (realloc) */
        GENERIC_DRIVER_ERROR_TRUNCATE,          /**< chyba při truncate operaci */
        GENERIC_DRIVER_ERROR_HANDLER_TYPE,      /**< nesprávný typ handleru */
        GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY,   /**< handler je již otevřený */
        GENERIC_DRIVER_ERROR_FOPEN,             /**< chyba při otevírání souboru (fopen) */
        GENERIC_DRIVER_ERROR_CB_NOT_EXIST,      /**< požadovaný callback není nastaven */
        GENERIC_DRIVER_ERROR_UNKNOWN            /**< neznámá chyba */
    } en_GENERIC_DRIVER_ERROR;


    /** @brief Typ handleru (soubor / paměť) */
    typedef enum en_HANDLER_TYPE {
        HANDLER_TYPE_UNKNOWN = 0,   /**< neznámý / neinicializovaný typ */
        HANDLER_TYPE_FILE,          /**< souborový handler */
        HANDLER_TYPE_MEMORY,        /**< paměťový handler */
    } en_HANDLER_TYPE;


    /** @brief Stav handleru — používá se jako bitové pole (READY | READ_ONLY) */
    typedef enum en_HANDLER_STATUS {
        HANDLER_STATUS_NOT_READY = 0,   /**< handler není otevřený */
        HANDLER_STATUS_READY     = 1,   /**< handler je otevřený a připravený */
        HANDLER_STATUS_READ_ONLY = 2,   /**< ochrana proti zápisu */
    } en_HANDLER_STATUS;


    /** @brief Režim otevření souboru */
    typedef enum en_FILE_DRIVER_OPEN_MODE {
        FILE_DRIVER_OPMODE_RO = 0,  /**< jen čtení ("rb") */
        FILE_DRIVER_OPMODE_RW,      /**< čtení a zápis do existujícího ("r+b") */
        FILE_DRIVER_OPMODE_W,       /**< vytvoření / přepsání ("w+b") */
    } en_FILE_DRIVER_OPEN_MODE;


    /** @brief Specifikace paměťového handleru */
    typedef struct st_HANDLER_MEMSPC {
        uint8_t *ptr;               /**< ukazatel na alokovaný buffer */
        size_t open_size;           /**< požadovaná velikost při otevření */
        size_t size;                /**< aktuální velikost bufferu */
        int swelling_enabled;       /**< nenulová hodnota povoluje automatické zvětšování v prepare_cb */
        int updated;                /**< nenulová hodnota signalizuje, že proběhla WR operace */
    } st_HANDLER_MEMSPC;


    /** @brief Specifikace souborového handleru */
    typedef struct st_HANDLER_FILESPC {
        FILE *fh;                   /**< systémový file pointer */
        en_FILE_DRIVER_OPEN_MODE open_mode; /**< režim otevření souboru */
        char *filename;             /**< cesta k souboru (handler jen drží ukazatel, nekopíruje) */
    } st_HANDLER_FILESPC;


    /** @brief Union pro handler-specifická data */
    typedef union un_HANDLER_SPEC {
#ifdef GENERIC_DRIVER_FILE
        st_HANDLER_FILESPC filespec;    /**< specifikace souborového handleru */
#endif
#ifdef GENERIC_DRIVER_MEMORY
        st_HANDLER_MEMSPC memspec;      /**< specifikace paměťového handleru */
#endif
    } un_HANDLER_SPEC;


    /** @brief Handler — reprezentuje jedno otevřené médium */
    typedef struct st_HANDLER {
        en_HANDLER_TYPE type;       /**< typ handleru (soubor / paměť) */
        en_HANDLER_STATUS status;   /**< stav handleru (bitové pole: READY | READ_ONLY) */
        en_HANDLER_ERROR err;       /**< chybový kód handleru */
        un_HANDLER_SPEC spec;       /**< handler-specifická data (union) */
        void *driver;               /**< zpětný ukazatel na přidružený st_DRIVER */
    } st_HANDLER;


    /** @brief Callback pro otevření média */
    typedef int (*generic_driver_open_cb )( st_HANDLER *h );
    /** @brief Callback pro uzavření média */
    typedef int (*generic_driver_close_cb )( st_HANDLER *h );
    /** @brief Callback pro přípravu I/O operace (nastavení přímého přístupu do bufferu) */
    typedef int (*generic_driver_prepare_cb )( st_HANDLER *h, uint32_t offset, void **prepared_buffer, uint32_t count_bytes );
    /** @brief Callback pro čtení dat z média */
    typedef int (*generic_driver_read_cb )( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *readlen );
    /** @brief Callback pro zápis dat do média */
    typedef int (*generic_driver_write_cb )( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *writelen );
    /** @brief Callback pro změnu velikosti média (truncate) */
    typedef int (*generic_driver_truncate_cb )( st_HANDLER *h, uint32_t new_size );


    /** @brief Driver — sada callbacků pro konkrétní typ I/O */
    typedef struct st_DRIVER {
        generic_driver_open_cb open_cb;         /**< callback pro otevření média */
        generic_driver_close_cb close_cb;       /**< callback pro uzavření média */
        generic_driver_read_cb read_cb;         /**< callback pro čtení dat */
        generic_driver_write_cb write_cb;       /**< callback pro zápis dat */
        generic_driver_prepare_cb prepare_cb;   /**< callback pro přípravu I/O (může být NULL) */
        generic_driver_truncate_cb truncate_cb; /**< callback pro truncate (může být NULL) */
        en_GENERIC_DRIVER_ERROR err;            /**< chybový kód driveru */
    } st_DRIVER;


    /** @name Jádrové API
     *  @{ */

    /** @brief Nastaví callbacky driveru. */
    extern void generic_driver_setup ( st_DRIVER *d, generic_driver_open_cb opcb, generic_driver_close_cb clcb, generic_driver_read_cb rdcb, generic_driver_write_cb wrcb, generic_driver_prepare_cb prepcb, generic_driver_truncate_cb trunccb );
    /** @brief Vrátí textovou chybovou zprávu pro aktuální stav handleru/driveru. */
    extern const char* generic_driver_error_message ( st_HANDLER *h, st_DRIVER *d );
    /** @brief Vynuluje nastavení handleru a přidělí mu požadovaný typ. */
    extern void generic_driver_register_handler ( st_HANDLER *h, en_HANDLER_TYPE type );
    /** @brief Otevře paměťové médium. */
    extern st_HANDLER* generic_driver_open_memory ( st_HANDLER *handler, st_DRIVER *d, uint32_t size );
    /** @brief Otevře souborové médium. */
    extern st_HANDLER* generic_driver_open_file ( st_HANDLER *handler, st_DRIVER *d, char *filename, en_FILE_DRIVER_OPEN_MODE open_mode );
    /** @brief Otevře paměťové médium a inicializuje ho obsahem souboru. */
    extern st_HANDLER* generic_driver_open_memory_from_file ( st_HANDLER *handler, st_DRIVER *d, const char *filename );
    /** @brief Uloží paměťový blok do souboru. */
    extern int generic_driver_save_memory ( st_HANDLER *h, char *filename );
    /** @brief Uzavře handler — zavolá close_cb driveru. */
    extern int generic_driver_close ( st_HANDLER *h );

    /** @brief Nastaví nebo zruší read-only příznak handleru. */
    extern void generic_driver_set_handler_readonly_status ( st_HANDLER *h, int readonly );

    /** @name Dvoustupňové I/O (prepare + ppread/ppwrite)
     *  @{ */
    /** @brief Připraví driver na následující I/O operaci. */
    extern int generic_driver_prepare ( st_HANDLER *h, uint32_t offset, void **buffer, void *tmpbuffer, uint32_t size );
    /** @brief Čtení z již připraveného driveru (po zavolání generic_driver_prepare). */
    extern int generic_driver_ppread ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t size );
    /** @brief Zápis do již připraveného driveru (po zavolání generic_driver_prepare). */
    extern int generic_driver_ppwrite ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t size );
    /** @} */

    /** @name Jednokrokové I/O (interně volá prepare + ppread/ppwrite)
     *  @{ */
    /** @brief Jednokrokové čtení (interně volá prepare + ppread). */
    extern int generic_driver_read ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t buffer_size );
    /** @brief Jednokrokový zápis (interně volá prepare + ppwrite). */
    extern int generic_driver_write ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t buffer_size );
    /** @brief Změní velikost média (truncate). */
    extern int generic_driver_truncate ( st_HANDLER *h, uint32_t size );
    /** @} */

    /** @brief Přímé čtení s optimalizací pro paměťový handler (vyhnutí se kopírování). */
    extern int generic_driver_direct_read ( st_HANDLER *h, uint32_t offset, void **buffer, void *work_buffer, uint32_t buffer_size );

    /** @} */


    /** @name Zabudované callbacky (volitelné — aktivují se předvolbami)
     *  @{ */

#ifdef GENERIC_DRIVER_FILE_CB
    /** @brief Inicializuje souborový driver se zabudovanými callbacky. */
    extern st_DRIVER* generic_driver_file_init ( st_DRIVER *d );
#endif


#ifdef GENERIC_DRIVER_MEMORY_CB
    /** @brief Inicializuje paměťový driver se zabudovanými callbacky. */
    extern st_DRIVER* generic_driver_memory_init ( st_DRIVER *d );
#endif

    /** @} */


    /**
     * @brief Validační bootstrap pro paměťový handler.
     *
     * Ověří typ, stav a platnost bufferu před I/O operací.
     * Resetuje chybové kódy handleru i driveru.
     *
     * @param h ukazatel na handler
     * @return EXIT_SUCCESS pokud je handler připravený k operaci,
     *         EXIT_FAILURE pokud ne (s nastaveným chybovým kódem)
     */
    static inline int generic_driver_memory_operation_internal_bootstrap ( st_HANDLER *h ) {

        st_HANDLER_MEMSPC *memspec = &h->spec.memspec;
        st_DRIVER *d = (st_DRIVER *) h->driver;

        h->err = HANDLER_ERROR_NONE;
        d->err = GENERIC_DRIVER_ERROR_NONE;

        if ( h->type != HANDLER_TYPE_MEMORY ) {
            d->err = GENERIC_DRIVER_ERROR_HANDLER_TYPE;
            return EXIT_FAILURE;
        }

        if ( !( h->status & HANDLER_STATUS_READY ) ) {
            h->err = HANDLER_ERROR_NOT_READY;
            return EXIT_FAILURE;
        }

        if ( memspec->ptr == NULL ) {
            d->err = GENERIC_DRIVER_ERROR_NOT_READY;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }


    /**
     * @brief Validační bootstrap pro souborový handler.
     *
     * Ověří typ, stav a platnost file pointeru před I/O operací.
     * Resetuje chybové kódy handleru i driveru.
     *
     * @param h ukazatel na handler
     * @return EXIT_SUCCESS pokud je handler připravený k operaci,
     *         EXIT_FAILURE pokud ne (s nastaveným chybovým kódem)
     */
    static inline int generic_driver_file_operation_internal_bootstrap ( st_HANDLER *h ) {

        st_HANDLER_FILESPC *filespec = &h->spec.filespec;
        st_DRIVER *d = (st_DRIVER *) h->driver;

        h->err = HANDLER_ERROR_NONE;
        d->err = GENERIC_DRIVER_ERROR_NONE;

        if ( h->type != HANDLER_TYPE_FILE ) {
            d->err = GENERIC_DRIVER_ERROR_HANDLER_TYPE;
            return EXIT_FAILURE;
        }

        if ( !( h->status & HANDLER_STATUS_READY ) ) {
            h->err = HANDLER_ERROR_NOT_READY;
            return EXIT_FAILURE;
        }

        if ( filespec->fh == NULL ) {
            d->err = GENERIC_DRIVER_ERROR_NOT_READY;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    /** @brief Verze knihovny generic_driver. */
#define GENERIC_DRIVER_VERSION "2.0.2"

    /**
     * @brief Vrátí řetězec s verzí knihovny generic_driver.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* generic_driver_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* GENERIC_DRIVER_H */
