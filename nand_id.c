/*
 * nand_id.c
 *
 * Copyright (C) 2013 Qiang Yu <yuq825@gmail.com>
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
 */

#include "nand_id.h"

struct nand_chip_param nand_chip_param[] = {
	// id, id_len, ecc_mode, clock_freq
	// SAMSUNG
	{{0xec, 0xd7, 0x94, 0x7a, 0x54, 0x43, 0xff, 0xff}, 6, 3, 30}, // K9GBG08U0A
	{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 0, 0, 0},
};

