/**
 * @file mz800_bus_timing.h
 * @brief MZ-800: délky sběrnicových pulzů v GDG taktech.
 *
 * Hodnoty změřené Michalem na reálném MZ-800 (položky s TODO jsou
 * odhad, čekají na změření). Čtení probíhá při náběžné hraně pulzu.
 *
 * Vyčleněno z mz800_gdg.h (mzhal krok 6) - jediný zdroj těchto hodnot,
 * odsud se plní i g_mzhal.
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

#ifndef MZ800_BUS_TIMING_H
#define MZ800_BUS_TIMING_H

#define IORQ_RD_TICKS 12   /* Delka IORQ RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define IORQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka IORQ WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_M1_TICKS 7 /* Delka MREQ M1 RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_TICKS 9    /* Delka MREQ DATA RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka MREQ DATA WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */

#endif /* MZ800_BUS_TIMING_H */
