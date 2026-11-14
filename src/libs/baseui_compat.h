/**
 * @file   baseui_compat.h
 * @brief  Kompatibilni vrstva pro standalone build bez baseui z mz800emu.
 *
 * Pokud neni definovano GENERIC_DRIVER_NO_BASEUI, tento soubor se nepouzije
 * a skutecny baseui.h se includuje normalne. Pokud je definovano
 * GENERIC_DRIVER_NO_BASEUI, poskytne fallback makra pro baseui_tools_*
 * funkce pomoci standardnich malloc/calloc/free/fopen/fread/fwrite/fclose.
 */

#ifndef BASEUI_COMPAT_H
#define BASEUI_COMPAT_H

#ifdef GENERIC_DRIVER_NO_BASEUI

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static inline void* baseui_tools_mem_alloc ( size_t size ) {
    void *p = malloc ( size );
    if ( p ) memset ( p, 0, size );
    return p;
}

#define baseui_tools_mem_alloc0( size )          baseui_tools_mem_alloc ( size )
#define baseui_tools_mem_realloc( ptr, size )   realloc ( ptr, size )
#define baseui_tools_mem_free( ptr )            free ( ptr )

#define baseui_tools_file_open( filename, mode )                fopen ( filename, mode )
#define baseui_tools_file_read( buffer, size, count, fh )       fread ( buffer, size, count, fh )
#define baseui_tools_file_write( buffer, size, count, fh )      fwrite ( buffer, size, count, fh )
#define baseui_tools_file_close( fh )                           fclose ( fh )

#else
/* pouzit skutecny baseui z mz800emu */
#include "baseui/baseui.h"
#endif

#endif /* BASEUI_COMPAT_H */
