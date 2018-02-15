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

#include <config.h>
#include "dnsio_tcp.h"

#include "conf.h"
#include "dnswire.h"
#include "dnspacket.h"
#include "socks.h"

#include <gdnsd/alloc.h>
#include <gdnsd/log.h>
#include <gdnsd/misc.h>
#include <gdnsd/net.h>

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <ev.h>
#include <urcu-qsbr.h>

typedef enum {
    READING_INITIAL = 0,
    READING_MORE,
    WRITING,
} tcpdns_state_t;

// per-thread state
typedef struct {
    dnspacket_stats_t* stats;
    void* dnsp_ctx;
    struct ev_loop* loop;
    ev_io accept_watcher;
    ev_prepare prep_watcher;
    ev_async stop_watcher;
    unsigned timeout;
    unsigned max_clients;
    unsigned num_conn_watchers;
    bool rcu_is_online;
    bool shutting_down;
} tcpdns_thread_t;

// per-connection state
typedef struct {
    tcpdns_thread_t* ctx;
    ev_io read_watcher;
    ev_io write_watcher;
    ev_timer timeout_watcher;
    gdnsd_anysin_t asin;
    unsigned size;
    unsigned size_done;
    tcpdns_state_t state;
    uint8_t buffer[0];
} tcpdns_conn_t;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static tcpdns_thread_t** registry = NULL;
static size_t registry_size = 0;
static size_t registry_init = 0;

void dnsio_tcp_init(unsigned num_threads)
{
    registry_size = (size_t)num_threads;
    registry = xcalloc_n(registry_size, sizeof(*registry));
}

void dnsio_tcp_request_threads_stop(void)
{
    gdnsd_assert(registry_size == registry_init);
    for (unsigned i = 0; i < registry_init; i++) {
        tcpdns_thread_t* ctx = registry[i];
        ev_async* stop_watcher = &ctx->stop_watcher;
        ev_async_send(ctx->loop, stop_watcher);
    }
}

static void register_thread(tcpdns_thread_t* ctx)
{
    pthread_mutex_lock(&registry_lock);
    gdnsd_assert(registry_init < registry_size);
    registry[registry_init++] = ctx;
    pthread_mutex_unlock(&registry_lock);
}

static void stop_handler(struct ev_loop* loop, ev_async* w, int revents V_UNUSED)
{
    gdnsd_assert(revents == EV_ASYNC);
    tcpdns_thread_t* ctx = w->data;
    ev_async* stop_watcher = &ctx->stop_watcher;
    ev_async_stop(loop, stop_watcher);
    ev_prepare* prep_watcher = &ctx->prep_watcher;
    ev_prepare_stop(loop, prep_watcher);
    ev_io* accept_watcher = &ctx->accept_watcher;
    ev_io_stop(loop, accept_watcher);
    ctx->shutting_down = true;
}

F_NONNULL
static void cleanup_conn_watchers(struct ev_loop* loop, tcpdns_conn_t* tdata)
{
    ev_io* read_watcher = &tdata->read_watcher;
    shutdown(read_watcher->fd, SHUT_RDWR);
    close(read_watcher->fd);
    ev_io_stop(loop, read_watcher);

    ev_timer* timeout_watcher = &tdata->timeout_watcher;
    ev_timer_stop(loop, timeout_watcher);
    ev_io* write_watcher = &tdata->write_watcher;
    ev_io_stop(loop, write_watcher);

    if (tdata->ctx->num_conn_watchers-- == tdata->ctx->max_clients) {
        ev_io* accept_watcher = &tdata->ctx->accept_watcher;
        ev_io_start(loop, accept_watcher);
    }

    free(tdata);
}

F_NONNULL
static void tcp_timeout_handler(struct ev_loop* loop V_UNUSED, ev_timer* t, const int revents V_UNUSED)
{
    gdnsd_assert(revents == EV_TIMER);

    tcpdns_conn_t* tdata = t->data;
    log_devdebug("TCP DNS Connection timed out while %s %s",
                 tdata->state == WRITING ? "writing to" : "reading from", logf_anysin(&tdata->asin));

    if (tdata->state == WRITING)
        stats_own_inc(&tdata->ctx->stats->tcp.sendfail);
    else
        stats_own_inc(&tdata->ctx->stats->tcp.recvfail);

    cleanup_conn_watchers(loop, tdata);
}

F_NONNULL
static void tcp_write_handler(struct ev_loop* loop, ev_io* w, const int revents V_UNUSED)
{
    gdnsd_assert(revents == EV_WRITE);

    tcpdns_conn_t* tdata = w->data;
    ev_io* read_watcher = &tdata->read_watcher;
    ev_timer* timeout_watcher = &tdata->timeout_watcher;

    const size_t wanted = tdata->size - tdata->size_done;
    const uint8_t* source = tdata->buffer + tdata->size_done;

    const ssize_t send_rv = send(w->fd, source, wanted, 0);
    if (unlikely(send_rv < 0)) {
        if (!ERRNO_WOULDBLOCK) {
            log_devdebug("TCP DNS send() failed, dropping response to %s: %s", logf_anysin(&tdata->asin), logf_errno());
            stats_own_inc(&tdata->ctx->stats->tcp.sendfail);
            cleanup_conn_watchers(loop, tdata);
            return;
        }
    } else { // we sent something...
        tdata->size_done += (size_t)send_rv;
        if (likely(tdata->size_done == tdata->size)) {
            if (tdata->ctx->shutting_down) {
                // if shutting down, take the opportunity to close cleanly
                // after sending a response, instead of waiting for another
                // request on this connection
                cleanup_conn_watchers(loop, tdata);
            } else {
                ev_timer_again(loop, timeout_watcher);
                tdata->state = READING_INITIAL;
                ev_io_stop(loop, w);
                ev_io_start(loop, read_watcher);
                tdata->size_done = 0;
                tdata->size = 0;
            }
            return;
        }
    }

    // Start write watcher if necc
    ev_io* write_watcher = &tdata->write_watcher;
    ev_io_start(loop, write_watcher);
}

F_NONNULL
static void tcp_read_handler(struct ev_loop* loop, ev_io* w, const int revents V_UNUSED)
{
    gdnsd_assert(revents == EV_READ);
    tcpdns_conn_t* tdata = w->data;

    gdnsd_assert(tdata);
    gdnsd_assert(tdata->state == READING_INITIAL || tdata->state == READING_MORE);

    uint8_t* destination = &tdata->buffer[tdata->size_done];
    const size_t wanted =
        (tdata->state == READING_INITIAL ? (DNS_RECV_SIZE + 2) : tdata->size)
        - tdata->size_done;

    const ssize_t pktlen = recv(w->fd, destination, wanted, 0);
    if (pktlen < 1) {
        if (unlikely(pktlen == -1 || tdata->size_done)) {
            if (pktlen == -1) {
                if (ERRNO_WOULDBLOCK) {
#                   ifdef TCP_DEFER_ACCEPT
                    ev_io_start(loop, w);
#                   endif
                    return;
                }
                log_devdebug("TCP DNS recv() from %s: %s", logf_anysin(&tdata->asin), logf_errno());
            } else if (tdata->size_done) {
                log_devdebug("TCP DNS recv() from %s: Unexpected EOF", logf_anysin(&tdata->asin));
            }
            stats_own_inc(&tdata->ctx->stats->tcp.recvfail);
        }
        cleanup_conn_watchers(loop, tdata);
        return;
    }

    tdata->size_done += pktlen;

    if (likely(tdata->state == READING_INITIAL)) {
        if (likely(tdata->size_done > 1)) {
            tdata->size = ((unsigned)tdata->buffer[0] << 8U) + (unsigned)tdata->buffer[1] + 2U;
            if (unlikely(tdata->size > DNS_RECV_SIZE)) {
                log_devdebug("Oversized TCP DNS query of length %u from %s", tdata->size, logf_anysin(&tdata->asin));
                stats_own_inc(&tdata->ctx->stats->tcp.recvfail);
                cleanup_conn_watchers(loop, tdata);
                return;
            }
            tdata->state = READING_MORE;
        }
    }

    if (unlikely(tdata->size_done < tdata->size)) {
#       ifdef TCP_DEFER_ACCEPT
        ev_io_start(loop, w);
#       endif
        return;
    }

    //  Process the query and start the writer
    if (!tdata->ctx->rcu_is_online) {
        tdata->ctx->rcu_is_online = true;
        rcu_thread_online();
    }
    tdata->size = process_dns_query(tdata->ctx->dnsp_ctx, tdata->ctx->stats, &tdata->asin, &tdata->buffer[2], tdata->size - 2);
    if (!tdata->size) {
        cleanup_conn_watchers(loop, tdata);
        return;
    }

    ev_io_stop(loop, w);
    tdata->buffer[0] = (uint8_t)(tdata->size >> 8U);
    tdata->buffer[1] = (uint8_t)(tdata->size & 0xFF);
    tdata->size += 2;
    tdata->size_done = 0;
    tdata->state = WRITING;

    // Most likely the response fits in the socket buffers
    //  as well as the window size, and therefore a complete
    //  write can proceed immediately, so try it without
    //  going through the loop.  tcp_write_handler() will
    //  start its own watcher if necc.
    ev_io* write_watcher = &tdata->write_watcher;
    tcp_write_handler(loop, write_watcher, EV_WRITE);
}

F_NONNULL
static void accept_handler(struct ev_loop* loop, ev_io* w, const int revents V_UNUSED)
{
    gdnsd_assert(revents == EV_READ);

    gdnsd_anysin_t asin;
    memset(&asin, 0, sizeof(asin));
    asin.len = GDNSD_ANYSIN_MAXLEN;

    const int sock = accept4(w->fd, &asin.sa, &asin.len, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (unlikely(sock < 0)) {
        switch (errno) {
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINTR:
            break;
#ifdef ENONET
        case ENONET:
#endif
        case ENETDOWN:
#ifdef EPROTO
        case EPROTO:
#endif
        case EHOSTDOWN:
        case EHOSTUNREACH:
        case ENETUNREACH:
            log_devdebug("TCP DNS: early tcp socket death: %s", logf_errno());
            break;
        default:
            log_err("TCP DNS: accept() failed: %s", logf_errno());
        }
        return;
    }

    log_devdebug("Received TCP DNS connection from %s", logf_anysin(&asin));

    tcpdns_thread_t* ctx = w->data;

    if (++ctx->num_conn_watchers == ctx->max_clients) {
        ev_io* accept_watcher = &ctx->accept_watcher;
        ev_io_stop(loop, accept_watcher);
    }

    // buffer[0] is last element of struct, sized to max_response + 2.
    tcpdns_conn_t* tdata = xcalloc(sizeof(*tdata) + (gcfg->max_response + 2));
    tdata->state = READING_INITIAL;
    memcpy(&tdata->asin, &asin, sizeof(asin));
    tdata->ctx = ctx;

    ev_io* read_watcher = &tdata->read_watcher;
    ev_io_init(read_watcher, tcp_read_handler, sock, EV_READ);
    ev_set_priority(read_watcher, 0);
    read_watcher->data = tdata;

    ev_io* write_watcher = &tdata->write_watcher;
    ev_io_init(write_watcher, tcp_write_handler, sock, EV_WRITE);
    ev_set_priority(write_watcher, 1);
    write_watcher->data = tdata;

    ev_timer* timeout_watcher = &tdata->timeout_watcher;
    ev_timer_init(timeout_watcher, tcp_timeout_handler, 0, ctx->timeout);
    ev_set_priority(timeout_watcher, -1);
    ev_timer_again(loop, timeout_watcher);
    timeout_watcher->data = tdata;

#ifdef TCP_DEFER_ACCEPT
    // Since we use DEFER_ACCEPT, the request is likely already
    //  queued and available at this point, so start read()-ing
    //  without going through the event loop
    tcp_read_handler(loop, read_watcher, EV_READ);
#else
    ev_io_start(loop, read_watcher);
#endif
}

#ifndef SOL_IPV6
#define SOL_IPV6 IPPROTO_IPV6
#endif

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

void tcp_dns_listen_setup(dns_thread_t* t)
{
    const dns_addr_t* addrconf = t->ac;
    gdnsd_assert(addrconf);

    const gdnsd_anysin_t* asin = &addrconf->addr;
    gdnsd_assert(asin);

    const bool isv6 = asin->sa.sa_family == AF_INET6 ? true : false;
    gdnsd_assert(isv6 || asin->sa.sa_family == AF_INET);

    bool need_bind = false;
    if (t->sock == -1) { // not acquired via takeover
        t->sock = socket(isv6 ? PF_INET6 : PF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, gdnsd_getproto_tcp());
        if (t->sock < 0)
            log_fatal("Failed to create IPv%c TCP socket: %s", isv6 ? '6' : '4', logf_errno());
        need_bind = true;
    }

    const int opt_one = 1;
    if (setsockopt(t->sock, SOL_SOCKET, SO_REUSEADDR, &opt_one, sizeof(opt_one)) == -1)
        log_fatal("Failed to set SO_REUSEADDR on TCP socket: %s", logf_errno());

    if (setsockopt(t->sock, SOL_SOCKET, SO_REUSEPORT, &opt_one, sizeof(opt_one)) == -1)
        log_fatal("Failed to set SO_REUSEPORT on TCP socket: %s", logf_errno());

#ifdef TCP_DEFER_ACCEPT
    const int opt_timeout = (int)addrconf->tcp_timeout;
    if (setsockopt(t->sock, SOL_TCP, TCP_DEFER_ACCEPT, &opt_timeout, sizeof(opt_timeout)) == -1)
        log_fatal("Failed to set TCP_DEFER_ACCEPT on TCP socket: %s", logf_errno());
#endif

    if (isv6) {
        // Guard IPV6_V6ONLY with a getsockopt(), because Linux fails here if a
        // socket is already bound (in which case we also should've already set
        // this in the previous daemon instance), because it affects how binding
        // works...
        int opt_v6o = 0;
        socklen_t opt_v6o_len = sizeof(opt_v6o);
        if (getsockopt(t->sock, SOL_IPV6, IPV6_V6ONLY, &opt_v6o, &opt_v6o_len) == -1)
            log_fatal("Failed to get IPV6_V6ONLY on TCP socket: %s", logf_errno());
        if (!opt_v6o)
            if (setsockopt(t->sock, SOL_IPV6, IPV6_V6ONLY, &opt_one, sizeof(opt_one)) == -1)
                log_fatal("Failed to set IPV6_V6ONLY on TCP socket: %s", logf_errno());
    }

    if (need_bind)
        socks_bind_sock("TCP DNS", t->sock, asin);
}

static void set_rcu_offline(struct ev_loop* loop V_UNUSED, ev_prepare* w V_UNUSED, int revents V_UNUSED)
{
    tcpdns_thread_t* ctx = w->data;
    if (ctx->rcu_is_online) {
        ctx->rcu_is_online = false;
        rcu_thread_offline();
    }
}

void* dnsio_tcp_start(void* thread_asvoid)
{
    gdnsd_thread_setname("gdnsd-io-tcp");

    const dns_thread_t* t = thread_asvoid;
    gdnsd_assert(!t->is_udp);

    const dns_addr_t* addrconf = t->ac;

    tcpdns_thread_t* ctx = xcalloc(sizeof(*ctx));
    register_thread(ctx);

    if (listen(t->sock, (int)addrconf->tcp_clients_per_thread) == -1)
        log_fatal("Failed to listen(s, %u) on TCP socket %s: %s", addrconf->tcp_clients_per_thread, logf_anysin(&addrconf->addr), logf_errno());

    ctx->dnsp_ctx = dnspacket_ctx_init(&ctx->stats, false);

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    ctx->num_conn_watchers = 0;
    ctx->timeout = addrconf->tcp_timeout;
    ctx->max_clients = addrconf->tcp_clients_per_thread;
    ctx->shutting_down = false;

    ev_io* accept_watcher = &ctx->accept_watcher;
    ev_io_init(accept_watcher, accept_handler, t->sock, EV_READ);
    ev_set_priority(accept_watcher, -2);
    accept_watcher->data = ctx;

    ev_prepare* prep_watcher = &ctx->prep_watcher;
    ev_prepare_init(prep_watcher, set_rcu_offline);
    prep_watcher->data = ctx;

    ev_async* stop_watcher = &ctx->stop_watcher;
    ev_async_init(stop_watcher, stop_handler);
    ev_set_priority(stop_watcher, 2);
    stop_watcher->data = ctx;

    struct ev_loop* loop = ctx->loop = ev_loop_new(EVFLAG_AUTO);
    if (!loop)
        log_fatal("ev_loop_new() failed");

    ev_async_start(loop, stop_watcher);
    ev_io_start(loop, accept_watcher);
    ev_prepare_start(loop, prep_watcher);

    rcu_register_thread();
    ctx->rcu_is_online = true;

    ev_run(loop, 0);

    rcu_unregister_thread();

    // de-allocate explicitly when debugging, for leaks
#ifndef NDEBUG
    ev_loop_destroy(loop);
    dnspacket_ctx_debug_cleanup(ctx->dnsp_ctx);
    free(ctx);
#endif

    return NULL;
}
