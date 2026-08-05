/*
 * File:   inline_asm.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 16. září 2015, 8:43
 *
 * Kompatibilní wrapper nad knihovnou libs/iasm.
 * Přidává zobrazení chybových hlášek přes baseui.
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

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "inline_asm.h"
#include "baseui/baseui.h"
#include "i18n.h"


unsigned debugger_iasm_assemble_line ( uint16_t addr, const char *assemble_txt, st_IASMBIN *compiled_output ) {

    iasm_error_t error;
    unsigned result = iasm_assemble_line ( addr, assemble_txt, compiled_output, &error );

    if ( result == 0 ) {
        const char *errmsg;
        switch ( error ) {
            case IASM_ERR_VALUE_OUT_OF_RANGE:
                errmsg = _("Value is out of range!");
                break;
            case IASM_ERR_INVALID_INSTRUCTION:
            default:
                errmsg = _("Invalid instruction opcode or parameter!");
                break;
        };
        baseui_show_error_message ( _("Inline Assembler Error - %s\n\nAddr = 0x%04x\nInstruction = '%s'\n"), errmsg, addr, assemble_txt );
    };

    return result;
}


#endif
