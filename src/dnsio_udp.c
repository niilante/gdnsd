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
#include "dnsio_udp.h"

#include "conf.h"
#include "dnswire.h"
#include "dnspacket.h"
#include "socks.h"

#include <gdnsd/log.h>
#include <gdnsd/misc.h>

#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>

#include <urcu-qsbr.h>

#ifndef SOL_IPV6
#define SOL_IPV6 IPPROTO_IPV6
#endif

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

// RCU perf magic value:
// This is the longest time for which we'll delay writers in rcu_synchronize()
// (e.g. geoip/zonefile data reloaders waiting to reclaim dead data) in the
// worst case.  Note the current value is a prime number of us, and also a
// prime number of ms at lower resolution.  This is to help avoid getting into
// ugly patterns.
#define MAX_PRCU_DELAY_US 109367

// Similar to the above, this is added to the above amount as the maximum we'll
// artificially delay a thread shutdown request on daemon termination, in a
// corner-case race condition.  Normally, SIGUSR2 will interrupt recvmsg() and
// we'll immediately catch the thread_shutdown!=0 condition at the top of the
// runtime loop, exiting fairly quickly.  However, it's rarely possible that
// the interrupt arrives in the short time interval between checking the
// variable at the top and entry into a long-delay recvmsg() call shortly
// afterwards.  The long delay value is set by this parameter.  The tradeoff
// pressure against making this smaller to keep the maximum shutdown delay
// shorter is that an idle dnsio_udp thread that's receiving no traffic will
// wake up once per this interval "pointlessly" by returning from recvmsg()
// with EAGAIN then re-entering recvmsg() again.
// Note that when combined with the above number, this number is also still
// prime at us (3109367) and ms (3109) resolution for the same reasons.
#define MAX_SHUTDOWN_DELAY_S 3

static __thread volatile sig_atomic_t thread_shutdown = 0;
static void sighand_stop(int s V_UNUSED)
{
    thread_shutdown = 1;
}

void dnsio_udp_init(void)
{
    struct sigaction sa;
    sa.sa_handler = sighand_stop;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, 0);
}

#ifdef USE_SENDMMSG

// check for sendmmsg() actually working + desirable
static bool use_mmsg(void)
{
    static int memoize = -1;
    if (memoize == -1) {
        /* this causes no harm and exits immediately */
        sendmmsg(-1, 0, 0, 0);
        memoize = (int)(errno != ENOSYS);
    }
    return (bool)memoize;
}

#endif

static void udp_sock_opts_v4(const int sock V_UNUSED, const bool any_addr)
{
    const int opt_one V_UNUSED = 1;
    // If all variants we know of don't exist, we simply assume the IP
    //  stack will *not* set the DF bit on UDP packets.  We may need
    //  more variants here for other operating systems.
#if defined IP_MTU_DISCOVER && defined IP_PMTUDISC_DONT
    const int mtu_type = IP_PMTUDISC_DONT;
    if (setsockopt(sock, SOL_IP, IP_MTU_DISCOVER, &mtu_type, sizeof(mtu_type)) == -1)
        log_fatal("Failed to disable Path MTU Discovery for UDP socket: %s", logf_errno());
#endif
#if defined IP_DONTFRAG
    const int opt_zero = 0;
    if (setsockopt(sock, SOL_IP, IP_DONTFRAG, &opt_zero, sizeof(opt_zero)) == -1)
        log_fatal("Failed to disable DF bit for UDP socket: %s", logf_errno());
#endif

    if (any_addr) {
#if defined IP_PKTINFO
        if (setsockopt(sock, SOL_IP, IP_PKTINFO, &opt_one, sizeof(opt_one)) == -1)
            log_fatal("Failed to set IP_PKTINFO on UDP socket: %s", logf_errno());
#elif defined IP_RECVDSTADDR
        if (setsockopt(sock, SOL_IP, IP_RECVDSTADDR, &opt_one, sizeof(opt_one)) == -1)
            log_fatal("Failed to set IP_RECVDSTADDR on UDP socket: %s", logf_errno());
#else
        log_fatal("IPv4 any-address '0.0.0.0' not supported for DNS listening on your platform (no IP_PKTINFO or IP_RECVDSTADDR)");
#endif
    }

    // This is just a latency hack, it's not necessary for correct operation
#if defined IP_TOS && defined IPTOS_LOWDELAY
    const int opt_tos = IPTOS_LOWDELAY;
    if (setsockopt(sock, SOL_IP, IP_TOS, &opt_tos, sizeof(opt_tos)) == -1)
        log_warn("Failed to set IPTOS_LOWDELAY on UDP socket: %s", logf_errno());
#endif

}

/* Here, we assume that if neither IPV6_USE_MIN_MTU or IPV6_MTU is
 *  available that the kernel will fragment for us by default.  This
 *  may or may not be a safe assumption on all OS's.
 * To test: set up an environment where one link in the client<->server
 *  path has a smaller MTU than the server interface MTU, and the
 *  server's interface MTU is >1280. Send an IPv6 query that results in
 *  a response greater than the path MTU, but smaller than the server's
 *  interface MTU.
 *  If the response does not reach the client, this platform is broken,
 *  and we need to find a platform-specific way to make it fragment to
 *  1280 or disable IPv6 completely for this platform.
 */

static void udp_sock_opts_v6(const int sock)
{
    const int opt_one = 1;

#if defined IPV6_USE_MIN_MTU
    if (setsockopt(sock, SOL_IPV6, IPV6_USE_MIN_MTU, &opt_one, sizeof(opt_one)) == -1)
        log_fatal("Failed to set IPV6_USE_MIN_MTU on UDP socket: %s", logf_errno());
#elif defined IPV6_MTU
#    ifndef IPV6_MIN_MTU
#      define IPV6_MIN_MTU 1280
#    endif
    const int min_mtu = IPV6_MIN_MTU;
    if (setsockopt(sock, SOL_IPV6, IPV6_MTU, &min_mtu, sizeof(min_mtu)) == -1)
        log_fatal("Failed to set IPV6_MTU on UDP socket: %s", logf_errno());
#endif

    // Guard IPV6_V6ONLY with a getsockopt(), because Linux fails here if a
    // socket is already bound (in which case we also should've already set
    // this in the previous daemon instance), because it affects how binding
    // works...
    int opt_v6o = 0;
    socklen_t opt_v6o_len = sizeof(opt_v6o);
    if (getsockopt(sock, SOL_IPV6, IPV6_V6ONLY, &opt_v6o, &opt_v6o_len) == -1)
        log_fatal("Failed to get IPV6_V6ONLY on UDP socket: %s", logf_errno());
    if (!opt_v6o)
        if (setsockopt(sock, SOL_IPV6, IPV6_V6ONLY, &opt_one, sizeof(opt_one)) == -1)
            log_fatal("Failed to set IPV6_V6ONLY on UDP socket: %s", logf_errno());

#if defined IPV6_MTU_DISCOVER && defined IPV6_PMTUDISC_DONT
    const int mtu_type = IPV6_PMTUDISC_DONT;
    if (setsockopt(sock, SOL_IPV6, IPV6_MTU_DISCOVER, &mtu_type, sizeof(mtu_type)) == -1)
        log_fatal("Failed to disable Path MTU Discovery for UDP socket: %s", logf_errno());
#endif

#if defined IPV6_DONTFRAG
    // There have been reports in https://github.com/gdnsd/gdnsd/issues/115 of
    // the IPV6_DONTFRAG setsockopt failing within the context of some
    // OpenVZ+Debian environments.
    // RFC 3542 says "By default, this socket option is disabled", so what
    // we're doing here is just reinforcing the default as a sanity-check
    // against bad defaults.
    // Therefore, we'll merely warn rather than fatal on this, in hopes it
    // clears up whatever's wrong with these OpenVZ environments.
    const int opt_zero = 0;
    if (setsockopt(sock, SOL_IPV6, IPV6_DONTFRAG, &opt_zero, sizeof(opt_zero)) == -1)
        log_warn("Failed to disable DF bit for UDP socket: %s", logf_errno());
#endif

#if defined IPV6_RECVPKTINFO
    if (setsockopt(sock, SOL_IPV6, IPV6_RECVPKTINFO, &opt_one, sizeof(opt_one)) == -1)
        log_fatal("Failed to set IPV6_RECVPKTINFO on UDP socket: %s", logf_errno());
#elif defined IPV6_PKTINFO
    if (setsockopt(sock, SOL_IPV6, IPV6_PKTINFO, &opt_one, sizeof(opt_one)) == -1)
        log_fatal("Failed to set IPV6_PKTINFO on UDP socket: %s", logf_errno());
#else
#   error IPV6_RECVPKTINFO or IPV6_PKTINFO required; this host lacks both
#endif

#if defined IPV6_TCLASS && defined IPTOS_LOWDELAY
    const int opt_tos = IPTOS_LOWDELAY;
    if (setsockopt(sock, SOL_IPV6, IPV6_TCLASS, &opt_tos, sizeof(opt_tos)) == -1)
        log_warn("Failed to set IPTOS_LOWDELAY on UDP socket: %s", logf_errno());
#endif
}

F_NONNULL
static void negotiate_udp_buffer(int sock, int which, const unsigned pktsize, const unsigned width, const gdnsd_anysin_t* asin)
{
    gdnsd_assert(sock > -1);
    gdnsd_assert(which == SO_SNDBUF || which == SO_RCVBUF);
    gdnsd_assert(pktsize >= 512);
    gdnsd_assert(pktsize <= 65536);
    gdnsd_assert(width > 0);
    gdnsd_assert(width <= 64);

    // Our default desired buffer.  This is based on enough room for
    //   recv_width * 8 packets.  recv_width is counted as "4" if less than 4
    //   (including the non-sendmmsg() case).
    const int desired_buf = (int)(pktsize * 8 * ((width < 4) ? 4 : width));
    gdnsd_assert(desired_buf >= 16384); // 512 * 8 * 4
    gdnsd_assert(desired_buf <= 33554432); // 64K * 8 * 64

    // Bare minimum buffer we'll accept: the greater of 16K or pktsize
    const int min_buf = (int)((pktsize < 16384) ? 16384 : pktsize);
    gdnsd_assert(min_buf >= 16384);
    gdnsd_assert(min_buf <= 65536);

    // For log messages below
    const char* which_str = (which == SO_SNDBUF) ? "SO_SNDBUF" : "SO_RCVBUF";

    // Negotiate with the kernel: if it reports <desired, try to set desired,
    //   cutting in half on failure so long as we stay above the min, and then
    //   eventually trying the exact minimum.  If we can't set the min, fail fatally.
    int opt_size;
    socklen_t size_size = sizeof(opt_size);
    if (getsockopt(sock, SOL_SOCKET, which, &opt_size, &size_size) == -1)
        log_fatal("Failed to get %s on UDP socket: %s", which_str, logf_errno());
    if (opt_size < desired_buf) {
        opt_size = desired_buf;
        while (setsockopt(sock, SOL_SOCKET, which, &opt_size, sizeof(opt_size)) == -1) {
            if (opt_size > (min_buf << 1))
                opt_size >>= 1;
            else if (opt_size > min_buf)
                opt_size = min_buf;
            else
                log_fatal("Failed to set %s to %i for UDP socket %s: %s.  You may need to reduce the max_edns_response and/or udp_recv_width, or specify workable buffer sizes explicitly in the config", which_str, opt_size, logf_anysin(asin), logf_errno());
        }
    }

    // If we had to endure some reductions above, complain about it
    if (opt_size < desired_buf)
        log_info("UDP socket %s: %s: wanted %i, got %i", logf_anysin(asin), which_str, desired_buf, opt_size);
}

void udp_sock_setup(dns_thread_t* t)
{
    dns_addr_t* addrconf = t->ac;
    gdnsd_assert(addrconf);

    const gdnsd_anysin_t* asin = &addrconf->addr;

    if (!use_mmsg())
        addrconf->udp_recv_width = 1;

    const bool isv6 = asin->sa.sa_family == AF_INET6 ? true : false;
    gdnsd_assert(isv6 || asin->sa.sa_family == AF_INET);

    bool need_bind = false;
    if (t->sock == -1) { // not acquired via takeover
        t->sock = socket(isv6 ? PF_INET6 : PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, gdnsd_getproto_udp());
        if (t->sock == -1)
            log_fatal("Failed to create IPv%c UDP socket: %s", isv6 ? '6' : '4', logf_errno());
        need_bind = true;
    }

    const int opt_one = 1;
    if (setsockopt(t->sock, SOL_SOCKET, SO_REUSEADDR, &opt_one, sizeof(opt_one)) == -1)
        log_fatal("Failed to set SO_REUSEADDR on UDP socket: %s", logf_errno());

    if (setsockopt(t->sock, SOL_SOCKET, SO_REUSEPORT, &opt_one, sizeof(opt_one)) == -1)
        log_fatal("Failed to set SO_REUSEPORT on UDP socket: %s", logf_errno());

    if (addrconf->udp_rcvbuf) {
        int opt_size = (int)addrconf->udp_rcvbuf;
        if (setsockopt(t->sock, SOL_SOCKET, SO_RCVBUF, &opt_size, sizeof(opt_size)) == -1)
            log_fatal("Failed to set SO_RCVBUF to %i for UDP socket %s: %s", opt_size,
                      logf_anysin(asin), logf_errno());
    } else {
        negotiate_udp_buffer(t->sock, SO_RCVBUF, DNS_RECV_SIZE, addrconf->udp_recv_width, asin);
    }

    if (addrconf->udp_sndbuf) {
        int opt_size = (int)addrconf->udp_sndbuf;
        if (setsockopt(t->sock, SOL_SOCKET, SO_SNDBUF, &opt_size, sizeof(opt_size)) == -1)
            log_fatal("Failed to set SO_SNDBUF to %i for UDP socket %s: %s", opt_size,
                      logf_anysin(asin), logf_errno());
    } else {
        negotiate_udp_buffer(t->sock, SO_SNDBUF, gcfg->max_edns_response, addrconf->udp_recv_width, asin);
    }

    if (isv6)
        udp_sock_opts_v6(t->sock);
    else
        udp_sock_opts_v4(t->sock, gdnsd_anysin_is_anyaddr(asin));

    if (need_bind)
        socks_bind_sock("UDP DNS", t->sock, asin);
}

static unsigned get_pgsz(void)
{
    long pgsz = sysconf(_SC_PAGESIZE);
    // if sysconf() error or ridiculous value, use 4K
    if (pgsz < 1024 || pgsz > (1 << 20))
        pgsz = 4096;
    return (unsigned)pgsz;
}

// A reasonable guess for v4/v6 dstaddr pktinfo + cmsg header?
#define CMSG_BUFSIZE 256

F_HOT F_NONNULL
static void mainloop(const int fd, void* dnsp_ctx, dnspacket_stats_t* stats, const bool use_cmsg)
{
    const unsigned cmsg_size = use_cmsg ? CMSG_BUFSIZE : 1U;
    const unsigned pgsz = get_pgsz();
    const unsigned max_rounded = ((gcfg->max_response + pgsz - 1) / pgsz) * pgsz;

    gdnsd_anysin_t asin;
    void* buf = gdnsd_xpmalign(pgsz, max_rounded);
    struct iovec iov = {
        .iov_base = buf,
        .iov_len  = 0
    };
    struct msghdr msg_hdr;
    char cmsg_buf[cmsg_size];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    memset(&msg_hdr, 0, sizeof(msg_hdr));
    msg_hdr.msg_name       = &asin.sa;
    msg_hdr.msg_iov        = &iov;
    msg_hdr.msg_iovlen     = 1;
    msg_hdr.msg_control    = use_cmsg ? cmsg_buf : NULL;

    const struct timeval tmout_long  = { .tv_sec = MAX_SHUTDOWN_DELAY_S, .tv_usec = MAX_PRCU_DELAY_US };
    const struct timeval tmout_short = { .tv_sec = 0, .tv_usec = MAX_PRCU_DELAY_US };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmout_short, sizeof(tmout_short)))
        log_fatal("Failed to set SO_RCVTIMEO on UDP socket: %s", logf_errno());
    bool is_online = true;

    while (1) {
        if (unlikely(thread_shutdown))
            break;

        iov.iov_len = DNS_RECV_SIZE;
        msg_hdr.msg_controllen = cmsg_size;
        msg_hdr.msg_namelen    = GDNSD_ANYSIN_MAXLEN;
        msg_hdr.msg_flags      = 0;

        ssize_t recvmsg_rv;

        if (likely(is_online)) {
            rcu_quiescent_state();
            recvmsg_rv = recvmsg(fd, &msg_hdr, 0);
            if (unlikely(recvmsg_rv < 0)) {
                if (errno == EINTR)
                    continue;
                if (ERRNO_WOULDBLOCK) {
                    rcu_thread_offline();
                    is_online = false;
                    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmout_long, sizeof(tmout_long));
                    continue;
                }
            }
        } else {
            recvmsg_rv = recvmsg(fd, &msg_hdr, 0);
            if (unlikely(recvmsg_rv < 0 && (ERRNO_WOULDBLOCK || errno == EINTR)))
                continue;
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmout_short, sizeof(tmout_short));
            is_online = true;
            rcu_thread_online();
        }

        if (unlikely(
                    (asin.sa.sa_family == AF_INET && !asin.sin.sin_port)
                    || (asin.sa.sa_family == AF_INET6 && !asin.sin6.sin6_port)
                )) {
            stats_own_inc(&stats->dropped);
        } else if (unlikely(recvmsg_rv < 0)) {
            log_err("UDP recvmsg() error: %s", logf_errno());
            stats_own_inc(&stats->udp.recvfail);
        } else {
            size_t buf_in_len = (size_t)recvmsg_rv;
            asin.len = msg_hdr.msg_namelen;
            iov.iov_len = process_dns_query(dnsp_ctx, stats, &asin, buf, buf_in_len);
            if (likely(iov.iov_len)) {
                while (1) {
                    int sent = sendmsg(fd, &msg_hdr, 0);
                    if (unlikely(sent < 0)) {
                        if (errno == EINTR || ERRNO_WOULDBLOCK)
                            continue;
                        stats_own_inc(&stats->udp.sendfail);
                        log_err("UDP sendmsg() of %zu bytes to client %s failed: %s", iov.iov_len, logf_anysin(&asin), logf_errno());
                    }
                    break;
                }
            }
        }
    }

#ifndef NDEBUG
    free(buf);
#endif
}

#ifdef USE_SENDMMSG

F_HOT F_NONNULL
static void mainloop_mmsg(const unsigned width, const int fd, void* dnsp_ctx, dnspacket_stats_t* stats, const bool use_cmsg)
{
    const unsigned cmsg_size = use_cmsg ? CMSG_BUFSIZE : 1U;

    // gcfg->max_response, rounded up to the next nearest multiple of the page size
    const unsigned pgsz = get_pgsz();
    const unsigned max_rounded = ((gcfg->max_response + pgsz - 1) / pgsz) * pgsz;

    uint8_t* bufs = gdnsd_xpmalign_n(pgsz, width, max_rounded);
    uint8_t* buf[width];
    struct iovec iov[width][1];
    struct mmsghdr dgrams[width];
    char cmsg_buf[width][cmsg_size];
    gdnsd_anysin_t asin[width];

    /* Set up packet buffers */
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    for (unsigned i = 0; i < width; i++)
        iov[i][0].iov_base = buf[i] = &bufs[i * max_rounded];

    const struct timeval tmout_long  = { .tv_sec = MAX_SHUTDOWN_DELAY_S, .tv_usec = MAX_PRCU_DELAY_US };
    const struct timeval tmout_short = { .tv_sec = 0, .tv_usec = MAX_PRCU_DELAY_US };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmout_short, sizeof(tmout_short)))
        log_fatal("Failed to set SO_RCVTIMEO on UDP socket: %s", logf_errno());
    bool is_online = true;

    while (1) {
        if (unlikely(thread_shutdown))
            break;

        /* Set up msg_hdr stuff: moving initialization inside of the loop was
             necessitated by the memmove() below */
        for (unsigned i = 0; i < width; i++) {
            iov[i][0].iov_len = DNS_RECV_SIZE;
            dgrams[i].msg_hdr.msg_iov        = iov[i];
            dgrams[i].msg_hdr.msg_iovlen     = 1;
            dgrams[i].msg_hdr.msg_name       = &asin[i].sa;
            dgrams[i].msg_hdr.msg_namelen    = GDNSD_ANYSIN_MAXLEN;
            dgrams[i].msg_hdr.msg_control    = use_cmsg ? cmsg_buf[i] : NULL;
            dgrams[i].msg_hdr.msg_controllen = cmsg_size;
            dgrams[i].msg_hdr.msg_flags      = 0;
        }

        int mmsg_rv;

        if (likely(is_online)) {
            rcu_quiescent_state();
            mmsg_rv = recvmmsg(fd, dgrams, width, MSG_WAITFORONE, NULL);
            if (unlikely(mmsg_rv < 0)) {
                if (errno == EINTR)
                    continue;
                if (ERRNO_WOULDBLOCK) {
                    rcu_thread_offline();
                    is_online = false;
                    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmout_long, sizeof(tmout_long));
                    continue;
                }
            }
        } else {
            mmsg_rv = recvmmsg(fd, dgrams, width, MSG_WAITFORONE, NULL);
            if (unlikely(mmsg_rv < 0 && (ERRNO_WOULDBLOCK || errno == EINTR)))
                continue;
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmout_short, sizeof(tmout_short));
            is_online = true;
            rcu_thread_online();
        }

        gdnsd_assert(mmsg_rv != 0);
        if (unlikely(mmsg_rv < 0)) {
            stats_own_inc(&stats->udp.recvfail);
            log_err("UDP recvmmsg() error: %s", logf_errno());
            continue;
        }

        unsigned pkts = (unsigned)mmsg_rv;
        gdnsd_assert(pkts <= width);
        for (unsigned i = 0; i < pkts; i++) {
            if (unlikely((asin[i].sa.sa_family == AF_INET && !asin[i].sin.sin_port)
                         || (asin[i].sa.sa_family == AF_INET6 && !asin[i].sin6.sin6_port))) {
                // immediately fail with no log output for packets with source port zero
                stats_own_inc(&stats->dropped);
                iov[i][0].iov_len = 0; // skip send, still need memmove below
            } else {
                asin[i].len = dgrams[i].msg_hdr.msg_namelen;
                iov[i][0].iov_len = process_dns_query(dnsp_ctx, stats, &asin[i], buf[i], dgrams[i].msg_len);
            }
        }

        /* This block adjusts the array of mmsg entries to account for skips where
         *   process_query() (or source port zero) decided we don't owe the
         *   sender a response packet.
         */
        /* This could be far simpler if sendmmsg() had an interface for skipping packets,
         *   e.g. a msg_flags flag that indicates the sendmmsg() internal loop should take
         *   no action for this entry, but still count it in the total number of successes
         */
        unsigned i = 0;
        while (i < pkts) {
            if (unlikely(!dgrams[i].msg_hdr.msg_iov[0].iov_len)) {
                const unsigned next = i + 1;
                if (next < pkts)
                    memmove(&dgrams[i], &dgrams[next], sizeof(dgrams[i]) * (pkts - next));
                pkts--;
            } else {
                i++;
            }
        }

        struct mmsghdr* dgptr = dgrams;
        while (pkts) {
            mmsg_rv = sendmmsg(fd, dgptr, pkts, 0);
            gdnsd_assert(mmsg_rv != 0); // not possible, sendmmsg returns >0 or -1+errno
            if (unlikely(mmsg_rv < 0)) {
                if (errno == EINTR || ERRNO_WOULDBLOCK)
                    continue; // retry same sendmmsg() call
                stats_own_inc(&stats->udp.sendfail);
                log_err("UDP sendmmsg() of %zu bytes to client %s failed: %s", dgptr[0].msg_hdr.msg_iov[0].iov_len, logf_anysin((const gdnsd_anysin_t*)dgptr[0].msg_hdr.msg_name), logf_errno());
                mmsg_rv = 1; // count as one packet "handled", so we
                // don't re-send the erroring packet
            }
            gdnsd_assert(mmsg_rv >= 1);
            gdnsd_assert(mmsg_rv <= (int)pkts);
            const unsigned sent = (unsigned)mmsg_rv;
            dgptr += sent; // skip past the handled packets
            pkts -= sent; // drop the count of all handled packets
        }
    }

#ifndef NDEBUG
    free(bufs);
#endif
}

#endif // USE_SENDMMSG

// We need to use cmsg stuff in the case of any IPv6 address (at minimum,
//  to copy the flow label correctly, if not the interface + source addr),
//  as well as the IPv4 any-address (for correct source address).
F_NONNULL F_PURE
static bool needs_cmsg(const gdnsd_anysin_t* asin)
{
    gdnsd_assert(asin->sa.sa_family == AF_INET6 || asin->sa.sa_family == AF_INET);
    return (asin->sa.sa_family == AF_INET6 || gdnsd_anysin_is_anyaddr(asin))
           ? true
           : false;
}

void* dnsio_udp_start(void* thread_asvoid)
{
    gdnsd_thread_setname("gdnsd-io-udp");

    const dns_thread_t* t = thread_asvoid;
    gdnsd_assert(t->is_udp);

    const dns_addr_t* addrconf = t->ac;

    dnspacket_stats_t* stats;
    void* dnsp_ctx = dnspacket_ctx_init(&stats, true);

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    // main thread blocks all sigs when spawning both tcp and udp io threads.
    // for dnsio_udp, unblock SIGUSR2, which we use to stop cleanly
    sigset_t sigmask_dnsio_udp;
    sigfillset(&sigmask_dnsio_udp);
    sigdelset(&sigmask_dnsio_udp, SIGUSR2);
    if (pthread_sigmask(SIG_SETMASK, &sigmask_dnsio_udp, NULL))
        log_fatal("pthread_sigmask() failed");

    const bool need_cmsg = needs_cmsg(&addrconf->addr);

    rcu_register_thread();

#ifdef USE_SENDMMSG
    if (addrconf->udp_recv_width > 1) {
        log_debug("sendmmsg() with a width of %u enabled for UDP socket %s",
                  addrconf->udp_recv_width, logf_anysin(&addrconf->addr));
        mainloop_mmsg(addrconf->udp_recv_width, t->sock, dnsp_ctx, stats, need_cmsg);
    } else
#endif
    {
        mainloop(t->sock, dnsp_ctx, stats, need_cmsg);
    }

    rcu_unregister_thread();
#ifndef NDEBUG
    dnspacket_ctx_debug_cleanup(dnsp_ctx);
#endif
    return NULL;
}
