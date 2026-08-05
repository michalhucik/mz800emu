/*
 * dbg_iasm_help.cpp — Stub
 *
 * Help okno inline assembleru je nyní implementováno jako vnořený modální
 * popup přímo v dbg_inline_asm.cpp. Tento soubor obsahuje jen prázdné
 * implementace API pro zpětnou kompatibilitu.
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

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "dbg_iasm_help.h"

/* Help je nyní vnořený modal uvnitř dbg_inline_asm.cpp */
void dbg_iasm_help_open(void) {}
void dbg_iasm_help_render(void) {}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
