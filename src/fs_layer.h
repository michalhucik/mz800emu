/* 
 * File:   fs_layer.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 11. února 2016, 7:27
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

#ifndef FS_LAYER_H
#define FS_LAYER_H

#ifdef __cplusplus
extern "C" {
#endif


#ifdef FS_LAYER_FATFS

#include "ff.h"

#define FS_LAYER_FOPEN(fh,path,mode) f_open ( &fh, path, mode )
#define FS_LAYER_FCLOSE(fh) f_close ( &fh )

#define FS_LAYER_FSEEK(fh,offset)    f_lseek ( &fh, offset )

#define FS_LAYER_FREAD(fh,buffer,count_bytes,readlen) f_read( &fh, buffer, count_bytes, readlen )
#define FS_LAYER_FWRITE(fh,buffer,count_bytes,writelen) f_write( &fh, buffer, count_bytes, writelen )

#define FS_LAYER_FSYNC(fh) f_sync ( &fh )
#define FS_LAYER_FTRUNCATE(fh) f_truncate ( &fh )

#define FS_LAYER_FMODE_RO    ( FA_READ )
#define FS_LAYER_FMODE_RW    ( FA_READ | FA_WRITE )
#define FS_LAYER_FMODE_W     ( FA_CREATE_NEW | FA_WRITE )

#else /* FS_LAYER_FATFS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "baseui/baseui.h"

    typedef BASEUI_TOOLS_DIR_HANDLE FS_LAYER_DIR_HANDLE;
    typedef BASEUI_TOOLS_DIR_ITEM FS_LAYER_DIR_ITEM;

    extern int fs_layer_fopen ( FILE **fh, char *path, char *mode );
    extern int fs_layer_fread ( FILE **fh, void *buffer, unsigned count_bytes, unsigned *readlen );
    extern int fs_layer_fwrite ( FILE **fh, void *buffer, unsigned count_bytes, unsigned *writelen );

#define FS_LAYER_FR_OK           0
#define FS_LAYER_DISK_ERR     1

#define FS_LAYER_FOPEN(fh,path,mode) fs_layer_fopen ( &fh, path, mode )
#define FS_LAYER_FCLOSE(fh) fclose ( fh )

#define FS_LAYER_FSEEK(fh,offset)   fseek ( fh, offset, SEEK_SET )
#define FS_LAYER_FSEEK_END(fh,offset)   fseek ( fh, offset, SEEK_END )

#define FS_LAYER_FREAD(fh,buffer,count_bytes,readlen) fs_layer_fread( &fh, buffer, count_bytes, readlen )
#define FS_LAYER_FWRITE(fh,buffer,count_bytes,writelen) fs_layer_fwrite( &fh, buffer, count_bytes, writelen )

#define FS_LAYER_FSYNC(fh) fflush ( fh )
#define FS_LAYER_FTELL(fh) ftell ( fh )

#define FS_LAYER_DIR_OPEN(path) baseui_tools_dir_open ( path )
#define FS_LAYER_DIR_CLOSE(dh) baseui_tools_dir_close ( dh )
#define FS_LAYER_DIR_READ(dh) baseui_tools_dir_read ( dh )
#define FS_LAYER_DITEM_IS_FILE(path, ditem) baseui_tools_ditem_is_file ( path, ditem )
#define FS_LAYER_DITEM_GET_NAME(ditem) baseui_tools_ditem_get_name ( ditem )
#define FS_LAYER_GET_ERROR_MESSAGE() baseui_tools_get_error_message ( )
#define FS_LAYER_DITEM_GET_FILEPATH(path, ditem) baseui_tools_ditem_get_filepath ( path, ditem )
#define FS_LAYER_DITEM_FREE_FILEPATH(filepath) baseui_tools_mem_free ( (void*) filepath )

#ifdef WINDOWS

#include <windows.h>

    extern int fs_layer_win32_truncate ( FILE **fh );

#define FS_LAYER_FTRUNCATE(fh) fs_layer_win32_truncate ( &fh )

#elif defined(LINUX)

    /* pri kompilaci pridat -D_XOPEN_SOURCE=500 */
#include <unistd.h>
#define FS_LAYER_FTRUNCATE(fh) ftruncate ( fileno ( fh ), ftell ( fh ) )

#endif

#define FS_LAYER_FMODE_RO   "rb"
#define FS_LAYER_FMODE_RW   "r+b"
#define FS_LAYER_FMODE_W    "w+b"

#endif /* ! FS_LAYER_FATFS */

#ifdef __cplusplus
}
#endif

#endif /* FS_LAYER_H */

