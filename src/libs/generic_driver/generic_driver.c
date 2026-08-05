/**
 * @file   generic_driver.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Jádrová implementace univerzální I/O abstrakce a volitelné zabudované callbacky.
 *
 * Tato knihovna slouží jako univerzální prostředek pro přístup k datovým blokům.
 * Virtualizuje datovou vrstvu, pod kterou se může skrývat např. image uložený
 * v souboru na disku, image umístěný v paměti nebo image v bankovaném ramdisku.
 *
 * Nadřazená vrstva používá vždy stejné API — rozdíly v přístupu k datům jsou
 * určeny jen použitým driverem a handlerem.
 *
 * Driver obsahuje callbacky pro operace čtení/zápisu z konkrétního offsetu.
 * Handler existuje ve dvou základních variantách — souborový a paměťový.
 * Pro paměťové handlery je k dispozici dvoustupňový přístup s optimalizací
 * (prepare + ppread/ppwrite), který se vyhne kopírování dat.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
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
 */

#include <string.h>

#ifdef WINDOWS
#include <windows.h>
#elif defined(LINUX)
/* při kompilaci přidat -D_XOPEN_SOURCE=500 */
#include <unistd.h>
#endif

#include "generic_driver.h"

/** @brief Příznak použití baseui_tools jako podkladové vrstvy pro alokaci a souborové operace */
#define HAVE_BASEUI_TOOLS

#ifdef HAVE_BASEUI_TOOLS
#include "baseui/baseui.h"

/** @brief Alokace paměti s vynulováním (přes baseui_tools) */
#define generic_driver_utils_mem_alloc0( size ) baseui_tools_mem_alloc ( size )
/** @brief Uvolnění paměti (přes baseui_tools) */
#define generic_driver_utils_mem_free( ptr ) baseui_tools_mem_free ( ptr )
/** @brief Otevření souboru (přes baseui_tools) */
#define generic_driver_utils_file_open( filename, mode ) baseui_tools_file_open ( filename, mode )
/** @brief Čtení ze souboru (přes baseui_tools) */
#define generic_driver_utils_file_read( buffer, size, count_bytes, fh ) baseui_tools_file_read ( buffer, size, count_bytes, fh )
/** @brief Zápis do souboru (přes baseui_tools) */
#define generic_driver_utils_file_write( buffer, size, count_bytes, fh ) baseui_tools_file_write ( buffer, size, count_bytes, fh )
/** @brief Zavření souboru (přes baseui_tools) */
#define generic_driver_utils_file_close( fh ) baseui_tools_file_close ( fh )

#else

/** @name Fallback implementace utilit — pro použití knihovny bez baseui
 *  @{ */

/**
 * @brief Alokace paměti s vynulováním (fallback bez baseui).
 * @param size požadovaná velikost v bajtech
 * @return ukazatel na alokovanou paměť, nebo NULL při selhání
 */
static inline void* generic_driver_utils_mem_alloc0 ( uint32_t size ) {
    void *ptr = malloc ( size );
    if ( !ptr ) return NULL;
    memset ( ptr, 0x00, size );
    return ptr;
}

/** @brief Uvolnění paměti (fallback bez baseui) */
#define generic_driver_utils_mem_free( ptr ) free ( ptr )
/** @brief Otevření souboru (fallback bez baseui) */
#define generic_driver_utils_file_open( filename, mode ) fopen ( filename, mode )


/**
 * @brief Čtení ze souboru s workaroundem pro Windows stdio bug (fallback bez baseui).
 * @param buffer cílový buffer
 * @param size velikost jednoho prvku
 * @param count_bytes počet prvků ke čtení
 * @param fh file pointer
 * @return počet skutečně přečtených prvků
 */
static inline unsigned int generic_driver_utils_file_read ( void *buffer, unsigned int size, unsigned int count_bytes, FILE *fh ) {
#ifdef WINDOWS
    /* stdio bug projevující se při "RW" mode */
    fseek ( fh, ftell ( fh ), SEEK_SET );
#endif
    return fread ( buffer, size, count_bytes, fh );
}


/**
 * @brief Zápis do souboru s workaroundem pro Windows stdio bug (fallback bez baseui).
 * @param buffer zdrojový buffer
 * @param size velikost jednoho prvku
 * @param count_bytes počet prvků k zápisu
 * @param fh file pointer
 * @return počet skutečně zapsaných prvků
 */
static inline unsigned int generic_driver_utils_file_write ( void *buffer, unsigned int size, unsigned int count_bytes, FILE *fh ) {
#ifdef WINDOWS
    /* stdio bug projevující se při "RW" mode */
    fseek ( fh, ftell ( fh ), SEEK_SET );
#endif
    return fwrite ( buffer, size, count_bytes, fh );
}

/** @brief Zavření souboru (fallback bez baseui) */
#define generic_driver_utils_file_close( fh ) fclose ( fh )

/** @} */

#endif


/**
 * @brief Vrátí textovou chybovou zprávu pro aktuální stav handleru/driveru.
 *
 * Nejprve zkontroluje chybu driveru (d->err), pak handleru (h->err).
 * Pokud není žádná chyba, vrátí "no error".
 *
 * @param h ukazatel na handler (může být NULL)
 * @param d ukazatel na driver (může být NULL)
 * @return textová chybová zpráva (statický řetězec — neuvolňovat)
 */
const char* generic_driver_error_message ( st_HANDLER *h, st_DRIVER *d ) {

    static const char *no_err_msg = "no error";
    static const char *unknown_err_msg = "unknown error";

    static const char *handler_err_msg[] = {
                                     "handler not ready",
                                     "handler is write protected",
    };

    static const char *driver_err_msg[] = {
                                    "driver not ready",
                                    "seek error",
                                    "read error",
                                    "write error",
                                    "size error",
                                    "malloc error",
                                    "realloc error",
                                    "truncate error",
                                    "bad handler type",
                                    "handler is busy",
                                    "fopen error",
                                    "callback not exist",
    };

    if ( d == NULL ) {
        return unknown_err_msg;
    }

    if ( d->err != GENERIC_DRIVER_ERROR_NONE ) {
        if ( d->err >= GENERIC_DRIVER_ERROR_UNKNOWN ) return unknown_err_msg;
        return driver_err_msg [ d->err - 1 ];
    }

    if ( h == NULL ) return unknown_err_msg;

    if ( h->err == HANDLER_ERROR_NONE ) return no_err_msg;
    if ( h->err >= HANDLER_ERROR_USER ) return unknown_err_msg;
    return handler_err_msg [ h->err - 1 ];
}


/**
 * @brief Nastaví callbacky driveru.
 *
 * @param d      ukazatel na driver
 * @param opcb   callback volaný pro otevření média
 * @param clcb   callback volaný pro ukončení práce s médiem
 * @param rdcb   callback volaný při operaci READ
 * @param wrcb   callback volaný při operaci WRITE
 * @param prepcb callback volaný před každou operací READ/WRITE
 * @param trucb  callback volaný při truncate (změna velikosti média)
 */
void generic_driver_setup ( st_DRIVER *d, generic_driver_open_cb opcb, generic_driver_close_cb clcb, generic_driver_read_cb rdcb, generic_driver_write_cb wrcb, generic_driver_prepare_cb prepcb, generic_driver_truncate_cb trucb ) {
    d->open_cb = opcb;
    d->close_cb = clcb;
    d->read_cb = rdcb;
    d->write_cb = wrcb;
    d->prepare_cb = prepcb;
    d->truncate_cb = trucb;
    d->err = GENERIC_DRIVER_ERROR_NONE;
}


/**
 * @brief Vynuluje nastavení handleru a přidělí mu požadovaný typ.
 *
 * @param h    ukazatel na handler
 * @param type typ handleru (HANDLER_TYPE_FILE / HANDLER_TYPE_MEMORY)
 */
void generic_driver_register_handler ( st_HANDLER *h, en_HANDLER_TYPE type ) {
    memset ( h, 0x00, sizeof ( st_HANDLER ) );
    h->type = type;
    h->err = HANDLER_ERROR_NONE;
    h->status = HANDLER_STATUS_NOT_READY;
    h->driver = NULL;
}


/**
 * @brief Otevře souborové médium.
 *
 * @param handler   pokud je NULL, bude alokován nový; jinak bude přepsán
 * @param d         driver — musí obsahovat open_cb
 * @param filename  jméno souboru
 * @param open_mode režim otevření (RO / RW / W)
 * @return ukazatel na handler, nebo NULL při chybě
 */
st_HANDLER* generic_driver_open_file ( st_HANDLER *handler, st_DRIVER *d, char *filename, en_FILE_DRIVER_OPEN_MODE open_mode ) {

    if ( d == NULL ) {
        return NULL;
    }

    st_HANDLER *h;
    if ( handler == NULL ) {
        h = generic_driver_utils_mem_alloc0 ( sizeof ( st_HANDLER ) );
    } else {
        h = handler;
    }

    if ( !d->open_cb ) {
        d->err = GENERIC_DRIVER_ERROR_CB_NOT_EXIST;
        return NULL;
    }

    generic_driver_register_handler ( h, HANDLER_TYPE_FILE );

    h->spec.filespec.filename = filename;
    h->spec.filespec.open_mode = open_mode;
    h->driver = d;

    if ( EXIT_FAILURE == d->open_cb ( h ) ) {
        if ( handler == NULL ) {
            generic_driver_utils_mem_free ( h );
        }
        return NULL;
    }

    return h;
}


/**
 * @brief Otevře paměťové médium.
 *
 * @param handler pokud je NULL, bude alokován nový; jinak bude přepsán
 * @param d       driver — musí obsahovat open_cb
 * @param size    požadovaná velikost bufferu (musí být >= 1)
 * @return ukazatel na handler, nebo NULL při chybě
 */
st_HANDLER* generic_driver_open_memory ( st_HANDLER *handler, st_DRIVER *d, uint32_t size ) {

    if ( d == NULL ) {
        return NULL;
    }

    st_HANDLER *h;
    if ( handler == NULL ) {
        h = generic_driver_utils_mem_alloc0 ( sizeof ( st_HANDLER ) );
    } else {
        h = handler;
    }

    /* Vždy inicializovat handler — konzistentní chování s open_file */
    generic_driver_register_handler ( h, HANDLER_TYPE_MEMORY );

    if ( !d->open_cb ) {
        d->err = GENERIC_DRIVER_ERROR_CB_NOT_EXIST;
        if ( handler == NULL ) {
            generic_driver_utils_mem_free ( h );
        }
        return NULL;
    }

    h->spec.memspec.open_size = size;
    h->driver = d;

    if ( EXIT_FAILURE == d->open_cb ( h ) ) {
        if ( handler == NULL ) {
            generic_driver_utils_mem_free ( h );
        }
        return NULL;
    }

    return h;
}


/**
 * @brief Otevře paměťové médium a inicializuje ho obsahem souboru.
 *
 * @param handler  pokud je NULL, bude alokován nový; jinak bude přepsán
 * @param d        driver — musí obsahovat open_cb
 * @param filename cesta k souboru
 * @return ukazatel na handler, nebo NULL při chybě
 */
st_HANDLER* generic_driver_open_memory_from_file ( st_HANDLER *handler, st_DRIVER *d, const char *filename ) {

    if ( d == NULL ) {
        return NULL;
    }

    FILE *fh = generic_driver_utils_file_open ( filename, "rb" );

    uint32_t size = 1;

    if ( !fh ) {
        return NULL;
    } else {
        fseek ( fh, 0, SEEK_END );
        size = ftell ( fh );
        fseek ( fh, 0, SEEK_SET );
    }

    st_HANDLER *h = generic_driver_open_memory ( handler, d, size );

    if ( !( ( !h ) || ( d->err ) || ( h->err ) ) ) {

        generic_driver_utils_file_read ( h->spec.memspec.ptr, 1, size, fh );

        if ( ferror ( fh ) ) {
            /* Chyba čtení — korektně uzavřít paměťový handler, pak uvolnit */
            generic_driver_close ( h );
            if ( handler == NULL ) {
                generic_driver_utils_mem_free ( h );
            }
            generic_driver_utils_file_close ( fh );
            return NULL;
        }
    }

    generic_driver_utils_file_close ( fh );

    return h;
}


/**
 * @brief Uloží paměťový blok do souboru.
 *
 * @param h        paměťový handler (musí být HANDLER_TYPE_MEMORY a READY)
 * @param filename cesta k výstupnímu souboru
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_save_memory ( st_HANDLER *h, char *filename ) {

    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;

    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    FILE *fh = generic_driver_utils_file_open ( filename, "wb" );

    if ( !fh ) return EXIT_FAILURE;

    fseek ( fh, 0, SEEK_SET );

    generic_driver_utils_file_write ( memspec->ptr, 1, memspec->size, fh );

    generic_driver_utils_file_close ( fh );

    return EXIT_SUCCESS;
}


/**
 * @brief Uzavře handler — zavolá close_cb driveru.
 *
 * @param h handler k uzavření
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_close ( st_HANDLER *h ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    if ( d == NULL ) {
        return EXIT_FAILURE;
    }

    if ( !d->close_cb ) {
        d->err = GENERIC_DRIVER_ERROR_CB_NOT_EXIST;
        return EXIT_FAILURE;
    }
    return d->close_cb ( h );
}


/**
 * @brief Nastaví nebo zruší read-only příznak handleru.
 *
 * @param h        handler
 * @param readonly 0 = zrušit ochranu, nenulová = nastavit ochranu
 */
void generic_driver_set_handler_readonly_status ( st_HANDLER *h, int readonly ) {
    if ( readonly == 0 ) {
        h->status &= ~HANDLER_STATUS_READ_ONLY;
    } else {
        h->status |= HANDLER_STATUS_READ_ONLY;
    }
}


/**
 * @brief Připraví driver na následující I/O operaci.
 *
 * Pokud driver má prepare_cb, zavolá ho. Prepare callback může nastavit
 * *buffer na přímý ukazatel do interních dat (optimalizace pro paměťový
 * handler). Pokud po prepare_cb zůstane *buffer == NULL, nastaví se
 * na work_buffer (záložní buffer dodaný volajícím).
 *
 * @param h           handler
 * @param offset      offset v datech
 * @param buffer      [out] ukazatel na buffer pro I/O
 * @param work_buffer záložní buffer (použije se pokud prepare_cb nenastaví *buffer)
 * @param buffer_size požadovaná velikost
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_prepare ( st_HANDLER *h, uint32_t offset, void **buffer, void *work_buffer, uint32_t buffer_size ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    if ( d == NULL ) {
        return EXIT_FAILURE;
    }

    if ( d->prepare_cb != NULL ) {
        if ( EXIT_SUCCESS != d->prepare_cb ( h, offset, buffer, buffer_size ) ) return EXIT_FAILURE;
    }
    if ( *buffer == NULL ) {
        *buffer = work_buffer;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Čtení z již připraveného driveru (po zavolání generic_driver_prepare).
 *
 * @param h           handler
 * @param offset      offset v datech
 * @param buffer      cílový buffer
 * @param buffer_size počet bajtů ke čtení
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_ppread ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t buffer_size ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    if ( d == NULL ) {
        return EXIT_FAILURE;
    }

    if ( d->read_cb == NULL ) {
        d->err = GENERIC_DRIVER_ERROR_CB_NOT_EXIST;
        return EXIT_FAILURE;
    }

    uint32_t result_len;
    int result = d->read_cb ( h, offset, buffer, buffer_size, &result_len );
    if ( ( result != EXIT_SUCCESS ) || ( result_len != buffer_size ) ) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Zápis do již připraveného driveru (po zavolání generic_driver_prepare).
 *
 * @param h           handler
 * @param offset      offset v datech
 * @param buffer      zdrojový buffer
 * @param buffer_size počet bajtů k zápisu
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_ppwrite ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t buffer_size ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    if ( d == NULL ) {
        return EXIT_FAILURE;
    }

    if ( d->write_cb == NULL ) {
        d->err = GENERIC_DRIVER_ERROR_CB_NOT_EXIST;
        return EXIT_FAILURE;
    }

    uint32_t result_len;
    int result = d->write_cb ( h, offset, buffer, buffer_size, &result_len );
    if ( ( result != EXIT_SUCCESS ) || ( result_len != buffer_size ) ) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Jednokrokové čtení (interně volá prepare + ppread).
 *
 * @param h           handler
 * @param offset      offset v datech
 * @param buffer      cílový buffer
 * @param buffer_size počet bajtů ke čtení
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_read ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t buffer_size ) {
    void *work_buffer = NULL;
    if ( EXIT_SUCCESS != generic_driver_prepare ( h, offset, &work_buffer, NULL, buffer_size ) ) return EXIT_FAILURE;
    return generic_driver_ppread ( h, offset, buffer, buffer_size );
}


/**
 * @brief Přímé čtení s optimalizací pro paměťový handler.
 *
 * Po prepare může *buffer ukazovat přímo do interních dat paměťového
 * handleru (vyhnutí se kopírování). Pro souborový handler se data
 * přečtou do work_buffer.
 *
 * @param h           handler
 * @param offset      offset v datech
 * @param buffer      [in/out] ukazatel na buffer — po návratu ukazuje na data
 * @param work_buffer záložní buffer
 * @param buffer_size počet bajtů ke čtení
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_direct_read ( st_HANDLER *h, uint32_t offset, void **buffer, void *work_buffer, uint32_t buffer_size ) {
    if ( EXIT_SUCCESS != generic_driver_prepare ( h, offset, buffer, work_buffer, buffer_size ) ) return EXIT_FAILURE;
    return generic_driver_ppread ( h, offset, *buffer, buffer_size );
}


/**
 * @brief Jednokrokový zápis (interně volá prepare + ppwrite).
 *
 * @param h           handler
 * @param offset      offset v datech
 * @param buffer      zdrojový buffer
 * @param buffer_size počet bajtů k zápisu
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_write ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t buffer_size ) {
    void *work_buffer = NULL;
    if ( EXIT_SUCCESS != generic_driver_prepare ( h, offset, &work_buffer, NULL, buffer_size ) ) return EXIT_FAILURE;
    return generic_driver_ppwrite ( h, offset, buffer, buffer_size );
}


/**
 * @brief Změní velikost média (truncate).
 *
 * @param h    handler
 * @param size nová velikost
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_truncate ( st_HANDLER *h, uint32_t size ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    if ( d == NULL ) {
        return EXIT_FAILURE;
    }

    if ( d->truncate_cb != NULL ) {
        if ( EXIT_SUCCESS != d->truncate_cb ( h, size ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @name Zabudované callbacky
 *
 * Níže jsou předdefinované sady callbacků pro souborový a paměťový driver.
 * Aktivují se předvolbami GENERIC_DRIVER_FILE_CB a GENERIC_DRIVER_MEMORY_CB
 * v headeru. Jsou určeny pro projekty, které nepotřebují vlastní
 * implementaci driverů (v tomto projektu se místo nich používají
 * src/generic_driver/file_driver.c a memory_driver.c).
 * @{
 */


/**
 * @name Zabudovaný souborový driver (GENERIC_DRIVER_FILE_CB)
 *
 * Aktivace: #define GENERIC_DRIVER_FILE_CB před #include "generic_driver.h"
 *
 * Poskytuje kompletní sadu callbacků pro práci se soubory přes stdio:
 * open/close/read/write/truncate. Truncate je platformově podmíněný
 * (WINDOWS: SetEndOfFile, LINUX: ftruncate).
 *
 * Inicializace: generic_driver_file_init(&driver) nebo
 *               generic_driver_file_init(NULL) pro dynamickou alokaci.
 * @{
 */
#ifdef GENERIC_DRIVER_FILE_CB


/**
 * @brief Callback pro čtení ze souboru.
 *
 * @param h           handler (typ HANDLER_TYPE_FILE)
 * @param offset      offset v souboru
 * @param buffer      cílový buffer
 * @param count_bytes počet bajtů ke čtení
 * @param readlen     [out] skutečně přečtený počet bajtů
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_read_file_cb ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *readlen ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_FILESPC *filespec = &h->spec.filespec;

    *readlen = 0;

    if ( EXIT_SUCCESS != generic_driver_file_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    FILE *fh = filespec->fh;

    if ( EXIT_SUCCESS != fseek ( fh, offset, SEEK_SET ) ) {
        d->err = GENERIC_DRIVER_ERROR_SEEK;
        return EXIT_FAILURE;
    }

#ifdef WINDOWS
    /* stdio bug projevující se při "RW" mode */
    fseek ( fh, ftell ( fh ), SEEK_SET );
#endif

    *readlen = fread ( buffer, 1, count_bytes, fh );
    if ( *readlen != count_bytes ) {
        d->err = GENERIC_DRIVER_ERROR_READ;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Callback pro zápis do souboru.
 *
 * @param h           handler (typ HANDLER_TYPE_FILE)
 * @param offset      offset v souboru
 * @param buffer      zdrojový buffer
 * @param count_bytes počet bajtů k zápisu
 * @param writelen    [out] skutečně zapsaný počet bajtů
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_write_file_cb ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *writelen ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_FILESPC *filespec = &h->spec.filespec;

    *writelen = 0;

    if ( EXIT_SUCCESS != generic_driver_file_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    if ( h->status & HANDLER_STATUS_READ_ONLY ) {
        h->err = HANDLER_ERROR_WRITE_PROTECTED;
        return EXIT_FAILURE;
    }

    FILE *fh = filespec->fh;

    if ( EXIT_SUCCESS != fseek ( fh, offset, SEEK_SET ) ) {
        d->err = GENERIC_DRIVER_ERROR_SEEK;
        return EXIT_FAILURE;
    }

#ifdef WINDOWS
    /* stdio bug projevující se při "RW" mode */
    fseek ( fh, ftell ( fh ), SEEK_SET );
#endif

    *writelen = fwrite ( buffer, 1, count_bytes, fh );
    if ( *writelen != count_bytes ) {
        d->err = GENERIC_DRIVER_ERROR_WRITE;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Callback pro zkrácení souboru na požadovanou délku.
 *
 * Platformově podmíněný (WINDOWS: SetEndOfFile, LINUX: ftruncate).
 *
 * @param h    handler (typ HANDLER_TYPE_FILE)
 * @param size nová velikost souboru
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_truncate_file_cb ( st_HANDLER *h, uint32_t size ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;

    if ( EXIT_SUCCESS != generic_driver_file_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    if ( h->status & HANDLER_STATUS_READ_ONLY ) {
        h->err = HANDLER_ERROR_WRITE_PROTECTED;
        return EXIT_FAILURE;
    }

    FILE *fh = h->spec.filespec.fh;

    if ( fh == NULL ) {
        d->err = GENERIC_DRIVER_ERROR_NOT_READY;
        return EXIT_FAILURE;
    }

#ifdef WINDOWS
    if ( EXIT_SUCCESS != fseek ( fh, size, SEEK_SET ) ) {
        d->err = GENERIC_DRIVER_ERROR_SEEK;
        return EXIT_FAILURE;
    }
    if ( EXIT_SUCCESS != SetEndOfFile ( fh ) ) {
        d->err = GENERIC_DRIVER_ERROR_TRUNCATE;
        return EXIT_FAILURE;
    }
#elif defined(LINUX)
    if ( EXIT_SUCCESS != ftruncate ( fileno ( fh ), size ) ) {
        d->err = GENERIC_DRIVER_ERROR_TRUNCATE;
        return EXIT_FAILURE;
    }
#endif

    return EXIT_SUCCESS;
}


/**
 * @brief Callback pro otevření souborového handleru.
 *
 * @param h handler (typ HANDLER_TYPE_FILE, spec.filespec musí mít nastavený
 *                   filename a open_mode)
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_open_file_cb ( st_HANDLER *h ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_FILESPC *filespec = &h->spec.filespec;

    int fl_readonly = 1;
    char *txt_mode = "rb";

    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;

    if ( h->status & HANDLER_STATUS_READY ) {
        d->err = GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY;
        return EXIT_FAILURE;
    }

    if ( h->type != HANDLER_TYPE_FILE ) {
        d->err = GENERIC_DRIVER_ERROR_HANDLER_TYPE;
        return EXIT_FAILURE;
    }

    if ( filespec->fh != NULL ) {
        d->err = GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY;
        return EXIT_FAILURE;
    }

    d->err = GENERIC_DRIVER_ERROR_NONE;
    h->status = HANDLER_STATUS_NOT_READY;

    switch ( filespec->open_mode ) {
        case FILE_DRIVER_OPMODE_RO:
            txt_mode = "rb";
            fl_readonly = 1;
            break;

        case FILE_DRIVER_OPMODE_RW:
            txt_mode = "r+b";
            fl_readonly = 0;
            break;

        case FILE_DRIVER_OPMODE_W:
            txt_mode = "w+b";
            fl_readonly = 0;
            break;
    }

    filespec->fh = fopen ( filespec->filename, txt_mode );

    if ( !filespec->fh ) {
        d->err = GENERIC_DRIVER_ERROR_FOPEN;
        return EXIT_FAILURE;
    }

    h->status = HANDLER_STATUS_READY;

    if ( fl_readonly ) {
        h->status |= HANDLER_STATUS_READ_ONLY;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Callback pro zavření souborového handleru.
 *
 * @param h handler (typ HANDLER_TYPE_FILE)
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_close_file_cb ( st_HANDLER *h ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_FILESPC *filespec = &h->spec.filespec;

    if ( EXIT_SUCCESS != generic_driver_file_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    fclose ( filespec->fh );

    filespec->fh = NULL;

    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;
    h->status = HANDLER_STATUS_NOT_READY;

    return EXIT_SUCCESS;
}


/**
 * @brief Inicializuje souborový driver se zabudovanými callbacky.
 *
 * @param d ukazatel na driver; pokud je NULL, bude dynamicky alokován
 * @return ukazatel na inicializovaný driver
 */
st_DRIVER* generic_driver_file_init ( st_DRIVER *d ) {

    if ( d == NULL ) {
        d = generic_driver_utils_mem_alloc0 ( sizeof ( st_DRIVER ) );
    }

    generic_driver_setup (
                           d,
                           generic_driver_open_file_cb,
                           generic_driver_close_file_cb,
                           generic_driver_read_file_cb,
                           generic_driver_write_file_cb,
                           NULL,
                           generic_driver_truncate_file_cb );

    return d;
}


/** @} */

#endif /* GENERIC_DRIVER_FILE_CB */


/**
 * @name Zabudovaný paměťový driver (GENERIC_DRIVER_MEMORY_CB)
 *
 * Aktivace: #define GENERIC_DRIVER_MEMORY_CB před #include "generic_driver.h"
 *
 * Volitelně: #define GENERIC_DRIVER_MEMORY_CB_USE_REALLOC
 *   — povolí automatické zvětšování bufferu v prepare_cb, pokud má handler
 *     nastaven memspec.swelling_enabled != 0.
 *
 * Poskytuje kompletní sadu callbacků pro práci s paměťovým bufferem:
 * open/close/read/write/prepare/truncate.
 *
 * Optimalizace: read/write callbacky detekují případ, kdy buffer ukazuje
 * přímo do interních dat (výsledek prepare_cb), a v takovém případě
 * neprovádí kopírování.
 *
 * Inicializace: generic_driver_memory_init(&driver) nebo
 *               generic_driver_memory_init(NULL) pro dynamickou alokaci.
 * @{
 */
#ifdef GENERIC_DRIVER_MEMORY_CB


/**
 * Callback pro přípravu požadované části obrazu v paměti.
 *
 * Nastaví *buffer na přímý ukazatel do interního bufferu handleru.
 * Při GENERIC_DRIVER_MEMORY_CB_USE_REALLOC a swelling_enabled může
 * automaticky zvětšit buffer pokud je požadovaná oblast větší.
 *
 * @param h           handler (typ HANDLER_TYPE_MEMORY)
 * @param offset      offset v datech
 * @param buffer      [out] ukazatel na přímý přístup do interních dat
 * @param count_bytes požadovaný počet bajtů
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_prepare_memory_cb ( st_HANDLER *h, uint32_t offset, void **buffer, uint32_t count_bytes ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;

    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

#ifndef GENERIC_DRIVER_MEMORY_CB_USE_REALLOC
    if ( memspec->ptr == NULL ) {
        d->err = GENERIC_DRIVER_ERROR_NOT_READY;
        return EXIT_FAILURE;
    }
#endif

    uint32_t need_size = offset + count_bytes;
    *buffer = NULL;

#ifdef GENERIC_DRIVER_MEMORY_CB_USE_REALLOC

    if ( memspec->swelling_enabled ) {
        if ( ( offset > memspec->size ) || ( need_size > memspec->size ) ) {
            if ( h->status & HANDLER_STATUS_READ_ONLY ) {
                h->err = HANDLER_ERROR_WRITE_PROTECTED;
                return EXIT_FAILURE;
            }

            memspec->updated = 1;

            uint8_t *new_ptr = realloc ( memspec->ptr, need_size );

            if ( new_ptr == NULL ) {
                d->err = GENERIC_DRIVER_ERROR_REALLOC;
                return EXIT_FAILURE;
            }

            memspec->ptr = new_ptr;
            memspec->size = need_size;
        }
    } else {
        if ( offset > memspec->size ) {
            d->err = GENERIC_DRIVER_ERROR_SEEK;
            return EXIT_FAILURE;
        }

        if ( need_size > memspec->size ) {
            d->err = GENERIC_DRIVER_ERROR_SIZE;
            return EXIT_FAILURE;
        }
    }

#else

    if ( offset > memspec->size ) {
        d->err = GENERIC_DRIVER_ERROR_SEEK;
        return EXIT_FAILURE;
    }

    if ( need_size > memspec->size ) {
        d->err = GENERIC_DRIVER_ERROR_SIZE;
        return EXIT_FAILURE;
    }

#endif

    *buffer = &memspec->ptr[offset];

    return EXIT_SUCCESS;
}


/**
 * Callback pro čtení z paměti.
 *
 * Optimalizace: pokud buffer ukazuje přímo na &memspec->ptr[offset]
 * (výsledek prepare_cb), neprovádí se kopírování.
 *
 * @param h           handler (typ HANDLER_TYPE_MEMORY)
 * @param offset      offset v datech
 * @param buffer      cílový buffer
 * @param count_bytes počet bajtů ke čtení
 * @param readlen     [out] skutečně přečtený počet bajtů
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_read_memory_cb ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *readlen ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;

    *readlen = 0;

    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    if ( offset > memspec->size ) {
        d->err = GENERIC_DRIVER_ERROR_SEEK;
        return EXIT_FAILURE;
    }

    uint32_t need_size = offset + count_bytes;
    if ( need_size > memspec->size ) {
        d->err = GENERIC_DRIVER_ERROR_SIZE;
        return EXIT_FAILURE;
    }

    *readlen = count_bytes;
    if ( &memspec->ptr[offset] != buffer ) {
        memmove ( buffer, &memspec->ptr[offset], count_bytes );
    }

    return EXIT_SUCCESS;
}


/**
 * Callback pro zápis do paměti.
 *
 * Optimalizace: pokud buffer ukazuje přímo na &memspec->ptr[offset]
 * (výsledek prepare_cb), neprovádí se kopírování.
 * Nastaví memspec->updated = 1 při skutečném zápisu.
 *
 * @param h           handler (typ HANDLER_TYPE_MEMORY)
 * @param offset      offset v datech
 * @param buffer      zdrojový buffer
 * @param count_bytes počet bajtů k zápisu
 * @param writelen    [out] skutečně zapsaný počet bajtů
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_write_memory_cb ( st_HANDLER *h, uint32_t offset, void *buffer, uint32_t count_bytes, uint32_t *writelen ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;

    *writelen = 0;

    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    if ( h->status & HANDLER_STATUS_READ_ONLY ) {
        h->err = HANDLER_ERROR_WRITE_PROTECTED;
        return EXIT_FAILURE;
    }

    if ( offset > memspec->size ) {
        d->err = GENERIC_DRIVER_ERROR_SEEK;
        return EXIT_FAILURE;
    }

    uint32_t need_size = offset + count_bytes;
    if ( need_size > memspec->size ) {
        d->err = GENERIC_DRIVER_ERROR_SIZE;
        return EXIT_FAILURE;
    }

    *writelen = count_bytes;
    if ( &memspec->ptr[offset] != buffer ) {
        memspec->updated = 1;
        memmove ( &memspec->ptr[offset], buffer, count_bytes );
    }

    return EXIT_SUCCESS;
}


/**
 * Callback pro zkrácení/změnu velikosti paměťového bufferu.
 *
 * @param h    handler (typ HANDLER_TYPE_MEMORY)
 * @param size nová velikost (musí být >= 1)
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_truncate_memory_cb ( st_HANDLER *h, uint32_t size ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;

    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    if ( h->status & HANDLER_STATUS_READ_ONLY ) {
        h->err = HANDLER_ERROR_WRITE_PROTECTED;
        return EXIT_FAILURE;
    }

    if ( size < 1 ) {
        d->err = GENERIC_DRIVER_ERROR_SIZE;
        return EXIT_FAILURE;
    }

    memspec->updated = 1;

    uint8_t *new_ptr = realloc ( memspec->ptr, size );

    if ( new_ptr == NULL ) {
        d->err = GENERIC_DRIVER_ERROR_REALLOC;
        return EXIT_FAILURE;
    }

    memspec->ptr = new_ptr;
    memspec->size = size;

    return EXIT_SUCCESS;
}


/**
 * Callback pro otevření nového paměťového handleru.
 *
 * Alokuje paměťový buffer o velikosti memspec->open_size.
 *
 * @param h handler (typ HANDLER_TYPE_MEMORY, memspec.open_size musí být >= 1)
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_open_memory_cb ( st_HANDLER *h ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;

    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;

    if ( h->status & HANDLER_STATUS_READY ) {
        d->err = GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY;
        return EXIT_FAILURE;
    }

    if ( h->type != HANDLER_TYPE_MEMORY ) {
        d->err = GENERIC_DRIVER_ERROR_HANDLER_TYPE;
        return EXIT_FAILURE;
    }

    if ( memspec->ptr != NULL ) {
        d->err = GENERIC_DRIVER_ERROR_HANDLER_IS_BUSY;
        return EXIT_FAILURE;
    }

    if ( memspec->open_size < 1 ) {
        d->err = GENERIC_DRIVER_ERROR_SIZE;
        return EXIT_FAILURE;
    }

    d->err = GENERIC_DRIVER_ERROR_NONE;
    h->status = HANDLER_STATUS_NOT_READY;

    uint8_t *new_ptr = malloc ( memspec->open_size );

    if ( new_ptr == NULL ) {
        d->err = GENERIC_DRIVER_ERROR_MALLOC;
        return EXIT_FAILURE;
    }

    memspec->ptr = new_ptr;
    memspec->size = memspec->open_size;

    memspec->updated = 0;
    h->status = HANDLER_STATUS_READY;

    return EXIT_SUCCESS;
}


/**
 * Callback pro uzavření paměťového handleru.
 * Uvolní alokovaný buffer.
 *
 * @param h handler (typ HANDLER_TYPE_MEMORY)
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
int generic_driver_close_memory_cb ( st_HANDLER *h ) {

    st_DRIVER *d = (st_DRIVER *) h->driver;
    st_HANDLER_MEMSPC *memspec = &h->spec.memspec;

    if ( EXIT_SUCCESS != generic_driver_memory_operation_internal_bootstrap ( h ) ) return EXIT_FAILURE;

    free ( memspec->ptr );

    memspec->ptr = NULL;
    memspec->size = 0;

    memspec->updated = 0;
    h->err = HANDLER_ERROR_NONE;
    d->err = GENERIC_DRIVER_ERROR_NONE;
    h->status = HANDLER_STATUS_NOT_READY;

    return EXIT_SUCCESS;
}


/**
 * Inicializuje paměťový driver se zabudovanými callbacky.
 *
 * @param d ukazatel na driver; pokud je NULL, bude dynamicky alokován
 * @return ukazatel na inicializovaný driver
 */
st_DRIVER* generic_driver_memory_init ( st_DRIVER *d ) {

    if ( d == NULL ) {
        d = generic_driver_utils_mem_alloc0 ( sizeof ( st_DRIVER ) );
    }

    generic_driver_setup (
                           d,
                           generic_driver_open_memory_cb,
                           generic_driver_close_memory_cb,
                           generic_driver_read_memory_cb,
                           generic_driver_write_memory_cb,
                           generic_driver_prepare_memory_cb,
                           generic_driver_truncate_memory_cb );

    return d;
}


#endif /* GENERIC_DRIVER_MEMORY_CB */
