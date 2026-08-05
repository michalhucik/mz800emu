/**
 * @file emucore_poison.h
 * @brief Poison guard pro compile-once TU knihovny mz_emucore (mzhal 11b).
 *
 * Force-included (-include) do každého TU knihovny mz_emucore. TU se
 * kompilují BEZ -DMZARCH/-DMZTVSYS/-D capability maker; -Werror=undef
 * chytá #if formy a tento poison chytá VŠECHNY zbylé výskyty
 * identifikátorů včetně #ifdef/#ifndef a použití v kódu. Zapomenutá
 * per-arch podmínka v TU nebo v jeho include řetězu = compile error,
 * ne tichá špatná větev.
 *
 * Per-EXE TU (main.c, mzhal.c, whitelist hot core, per-arch podstromy)
 * tuto hlavičku NEdostávají a makra používají dál.
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

#ifndef EMUCORE_POISON_H
#define EMUCORE_POISON_H

#pragma GCC poison MZARCH
#pragma GCC poison MZTVSYS
#pragma GCC poison MZARCH_NAME
#pragma GCC poison MZTVSYS_NAME
#pragma GCC poison HAVE_PSG
#pragma GCC poison HAVE_PIOZ80
#pragma GCC poison HAVE_JOY
#pragma GCC poison CFG_HWEXT_HAVE_FDC
#pragma GCC poison CFG_HWEXT_HAVE_IDE8
#pragma GCC poison CFG_HWEXT_HAVE_RAMDISK
#pragma GCC poison CFG_HWEXT_HAVE_QDISK

#endif /* EMUCORE_POISON_H */
