/**
 * @file mz1500_bus_timing.h
 * @brief MZ-1500: délky sběrnicových pulzů v GDG taktech.
 *
 * [neověřeno] Hodnoty PŘEVZATÉ z měření na reálném MZ-800 - MZ-1500 má
 * CPU deličku 4, takže je Michal musí změřit a pak se budou lišit
 * (Michal 2026-07-29). Čtení probíhá při náběžné hraně pulzu.
 *
 * Vyčleněno z mz1500_gdg.h (mzhal krok 6) - jediný zdroj těchto hodnot,
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

#ifndef MZ1500_BUS_TIMING_H
#define MZ1500_BUS_TIMING_H

#define IORQ_RD_TICKS 12   /* Delka IORQ RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define IORQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka IORQ WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_M1_TICKS 7 /* Delka MREQ M1 RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_TICKS 9    /* Delka MREQ DATA RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka MREQ DATA WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */

#endif /* MZ1500_BUS_TIMING_H */
