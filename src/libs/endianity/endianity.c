/* 
 * File:   endianity.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. února 2017, 10:24
 * 
 * 
 * ----------------------------- License -------------------------------------
 *
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
 *
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>
#include <sys/param.h>

#include "endianity.h"


#ifndef __BYTE_ORDER
#include <stdint.h>
const uint16_t g_ui16 = 0x0001;
uint8_t *g_ui8 = ( uint8_t* ) & g_ui16;
#endif


/**
 * Provede konverzi 16 bitove hodnoty z/do BIG_ENDIAN
 * 
 * @param n
 * @return 
 */
uint16_t endianity_bswap16_BE ( uint16_t n ) {
#ifndef __BYTE_ORDER
    if ( *g_ui8 != 0x01 ) return n;
    return __builtin_bswap16 ( n );
#else    
#if __BYTE_ORDER == __BIG_ENDIAN
    return n;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    return __builtin_bswap16 ( n );
#else
#error "Unknown byte order"
#endif
#endif
}


/**
 * Provede konverzi 16 bitove hodnoty z/do LITTLE_ENDIAN
 * 
 * @param n
 * @return 
 */
uint16_t endianity_bswap16_LE ( uint16_t n ) {
#ifndef __BYTE_ORDER
    if ( *g_ui8 == 0x01 ) return n;
    return __builtin_bswap16 ( n );
#else    
#if __BYTE_ORDER == __BIG_ENDIAN
    return __builtin_bswap16 ( n );
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    return n;
#else
#error "Unknown byte order"
#endif
#endif
}


/**
 * Provede konverzi 32 bitove hodnoty z/do BIG_ENDIAN
 * 
 * @param n
 * @return 
 */
uint32_t endianity_bswap32_BE ( uint32_t n ) {
#ifndef __BYTE_ORDER
    if ( *g_ui8 != 0x01 ) return n;
    return __builtin_bswap32 ( n );
#else    
#if __BYTE_ORDER == __BIG_ENDIAN
    return n;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    return __builtin_bswap32 ( n );
#else
#error "Unknown byte order"
#endif
#endif
}


/**
 * Provede konverzi 32 bitove hodnoty z/do LITTLE_ENDIAN
 * 
 * @param n
 * @return 
 */
uint32_t endianity_bswap32_LE ( uint32_t n ) {
#ifndef __BYTE_ORDER
    if ( *g_ui8 == 0x01 ) return n;
    return __builtin_bswap32 ( n );
#else    
#if __BYTE_ORDER == __BIG_ENDIAN
    return __builtin_bswap32 ( n );
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    return n;
#else
#error "Unknown byte order"
#endif
#endif
}


/**
 * Provede konverzi 64 bitove hodnoty z/do BIG_ENDIAN
 * 
 * @param n
 * @return 
 */
uint64_t endianity_bswap64_BE ( uint64_t n ) {
#ifndef __BYTE_ORDER
    if ( *g_ui8 != 0x01 ) return n;
    return __builtin_bswap64 ( n );
#else    
#if __BYTE_ORDER == __BIG_ENDIAN
    return n;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    return __builtin_bswap64 ( n );
#else
#error "Unknown byte order"
#endif
#endif
}


/**
 * Provede konverzi 64 bitove hodnoty z/do LITTLE_ENDIAN
 * 
 * @param n
 * @return 
 */
uint64_t endianity_bswap64_LE ( uint64_t n ) {
#ifndef __BYTE_ORDER
    if ( *g_ui8 == 0x01 ) return n;
    return __builtin_bswap64 ( n );
#else
#if __BYTE_ORDER == __BIG_ENDIAN
    return __builtin_bswap64 ( n );
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    return n;
#else
#error "Unknown byte order"
#endif
#endif
}


const char* endianity_version ( void ) {
    return ENDIANITY_VERSION;
}

