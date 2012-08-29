/* Copyright © 2012 Brandon L Black <blblack@gmail.com>
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

#ifndef _GDNSD_ZTREE_H
#define _GDNSD_ZTREE_H

#include "config.h"
#include "gdnsd.h"

#include <inttypes.h>
#include "ltarena.h"

// mutually-dependent stuff between zone.h and ltree.h
struct _zone_struct;
typedef struct _zone_struct zone_t;

#include "ltree.h"

struct _zone_struct {
    unsigned hash;        // hash of dname
    unsigned serial;      // SOA serial from zone data
    time_t mtime;         // mod time of source
    char* src;            // string description of src, e.g. "rfc1035:example.com"
    const uint8_t* dname; // zone name as a dname (stored in ->arena)
    ltarena_t* arena;     // arena for dname/label storage
    ltree_node_t* root;   // the zone root
    zone_t* next;         // init to NULL, owned by ztree...
};

// Singleton init
void ztree_init(void);

// primary interface for zone data sources
void ztree_update(zone_t* z_old, zone_t* z_new);

// These are for zsrc_* code to create/delete detached zone_t's used
//   in ztree_update() calls.
F_NONNULL
zone_t* zone_new(const char* zname, const char* source);
F_NONNULL
bool zone_finalize(zone_t* zone);
F_NONNULL
void zone_delete(zone_t* zone);

// primary interface for zone data runtime lookups from dnsio threads
// Argument is any legal fully-qualified dname
// Output is the zone_t structure for the known containing zone,
//   or NULL if no current zone contains the name.
// auth_depth_out is mostly useful for dnspacket.c, it tells you
//   how many bytes into the dname the authoritative zone name
//   starts at.
F_NONNULL
zone_t* ztree_find_zone_for(const uint8_t* dname, unsigned* auth_depth_out);

// ztree locking for readers (DNS I/O threads):
// thread start -> ztree_reader_thread_start()
//  loop:
//   enter i/o wait (epoll/recvmsg) -> ztree_reader_offline()
//   return from i/o wait -> ztree_reader_online()
//   ztree_reader_lock()
//   z = ztree_find_zone_for(...)
//   finish using all data subordinate to "z"
//   ztree_reader_unlock()
//   goto loop

#ifdef HAVE_QSBR

#define _LGPL_SOURCE 1
#include <urcu-qsbr.h>

F_UNUSED static void ztree_reader_thread_start(void) { rcu_register_thread(); }
F_UNUSED static void ztree_reader_thread_end(void) { rcu_unregister_thread(); }
F_UNUSED static void ztree_reader_online(void) { rcu_thread_online(); } 
F_UNUSED static void ztree_reader_lock(void) { rcu_read_lock(); }
F_UNUSED static void ztree_reader_unlock(void) { rcu_read_unlock(); }
F_UNUSED static void ztree_reader_offline(void) { rcu_thread_offline(); }

#else

F_UNUSED static void ztree_reader_thread_start(void) { }
F_UNUSED static void ztree_reader_thread_end(void) { }
F_UNUSED static void ztree_reader_online(void) { }
void ztree_reader_lock(void);
void ztree_reader_unlock(void);
F_UNUSED static void ztree_reader_offline(void) { }

#endif

#endif // _GDNSD_ZTREE_H