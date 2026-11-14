/**
 * @file   cmt_stream_all.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.1
 * @brief  Sdílené definice pro všechny typy CMT streamů.
 *
 * Obsahuje výčtový typ polarity signálu, společný pro bitstream i vstream.
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


#ifndef CMT_STREAM_ALL_H
#define CMT_STREAM_ALL_H

#ifdef __cplusplus
extern "C" {
#endif


    /** @brief Polarita CMT signálu — normální nebo invertovaná */
    typedef enum en_CMT_STREAM_POLARITY {
        CMT_STREAM_POLARITY_NORMAL = 0,     /**< normální polarita */
        CMT_STREAM_POLARITY_INVERTED        /**< invertovaná polarita */
    } en_CMT_STREAM_POLARITY;



#ifdef __cplusplus
}
#endif

#endif /* CMT_STREAM_ALL_H */
