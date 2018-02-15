/* Copyright © 2016 Brandon L Black <blblack@gmail.com>
 *
 * This file is part of gdnsd.
 *
 * gdnsd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gdnsd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gdnsd.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef GDNSD_CS_H
#define GDNSD_CS_H

#include <gdnsd/compiler.h>

#include <inttypes.h>

typedef union {
    char raw[8];
    struct {
        char key;
        uint32_t v : 24;
        uint32_t d;
    } S_PACKED;
} csbuf_t;

// Legal values for "key"
#define REQ_INFO 'I'
#define REQ_STAT 'S'
#define REQ_STOP 'X'
#define REQ_ZREL 'Z'
#define REQ_TAKE 'T'
#define REQ_REPL 'R'
#define RESP_ACK 'A'
#define RESP_NAK 'N'

// AFAIK there's no portable way to know the max FDs that can be sent in an
// SCM_RIGHTS message.  I only know the Linux limit for sure, so in the
// non-Linux case I'm picking a fairly conservative value of 32 for now.
#ifdef __linux__
#  define SCM_MAX_FDS 253U
#else
#  define SCM_MAX_FDS 32U
#endif

#endif // GDNSD_CS_H
