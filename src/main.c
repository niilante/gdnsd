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
#include "main.h"

#include "conf.h"
#include "socks.h"
#include "daemon.h"
#include "dnsio_tcp.h"
#include "dnsio_udp.h"
#include "dnspacket.h"
#include "statio.h"
#include "ztree.h"
#include "zsrc_rfc1035.h"
#include "zsrc_djb.h"
#include "css.h"
#include "csc.h"

#include <gdnsd-prot/plugapi.h>
#include <gdnsd-prot/misc.h>
#include <gdnsd-prot/mon.h>
#include <gdnsd/alloc.h>
#include <gdnsd/log.h>
#include <gdnsd/net.h>
#include <gdnsd/vscf.h>
#include <gdnsd/paths.h>
#include <gdnsd/misc.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pwd.h>
#include <time.h>

// Signal we were killed by, for final raise()
static int killed_by = 0;

// primary/default libev loop for main thread
static struct ev_loop* def_loop = NULL;

// libev watchers for signals+async
static ev_signal* sig_int = NULL;
static ev_signal* sig_term = NULL;
static ev_async* async_reloadz = NULL;

// custom atexit-like stuff, only for resource
//   de-allocation in debug builds to check for leaks

#ifndef NDEBUG

static void (**exitfuncs)(void) = NULL;
static unsigned exitfuncs_pending = 0;

void gdnsd_atexit_debug(void (*f)(void))
{
    exitfuncs = xrealloc(exitfuncs, (exitfuncs_pending + 1) * sizeof(void (*)(void)));
    exitfuncs[exitfuncs_pending++] = f;
}

static void atexit_debug_execute(void)
{
    while (exitfuncs_pending--)
        exitfuncs[exitfuncs_pending]();
}

#else

void gdnsd_atexit_debug(void (*f)(void) V_UNUSED) { }
static void atexit_debug_execute(void) { }

#endif

F_NONNULL F_NORETURN
static void syserr_for_ev(const char* msg)
{
    log_fatal("%s: %s", msg, logf_errno());
}

static pthread_t zones_reloader_threadid;

static bool join_zones_reloader_thread(void)
{
    void* raw_exit_status = (void*)42U;
    int pthread_err = pthread_join(zones_reloader_threadid, &raw_exit_status);
    if (pthread_err)
        log_err("pthread_join() of zone data loading thread failed: %s", logf_strerror(pthread_err));
    return !!raw_exit_status;
}

// Spawns a new thread to reload zone data.  Initial loading at startup sets
// the "initial" flag for the thread, which means it doesn't send an async
// notification back to us on completion, as we'll be waiting for it
// synchronously in this case.
static void spawn_zones_reloader_thread(const bool initial)
{
    // Block all signals using the pthreads interface while starting threads,
    //  which causes them to inherit the same mask.
    sigset_t sigmask_all;
    sigfillset(&sigmask_all);
    sigset_t sigmask_prev;
    sigemptyset(&sigmask_prev);
    if (pthread_sigmask(SIG_SETMASK, &sigmask_all, &sigmask_prev))
        log_fatal("pthread_sigmask() failed");

    pthread_attr_t attribs;
    pthread_attr_init(&attribs);
    pthread_attr_setdetachstate(&attribs, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&attribs, PTHREAD_SCOPE_SYSTEM);

    int pthread_err = pthread_create(&zones_reloader_threadid, &attribs, &ztree_zones_reloader_thread, (void*)initial);
    if (pthread_err)
        log_fatal("pthread_create() of zone data thread failed: %s", logf_strerror(pthread_err));

    // Restore the original mask in the main thread, so
    //  we can continue handling signals like normal
    if (pthread_sigmask(SIG_SETMASK, &sigmask_prev, NULL))
        log_fatal("pthread_sigmask() failed");
    pthread_attr_destroy(&attribs);
}

static bool initialize_zones(void)
{
    spawn_zones_reloader_thread(true);
    return join_zones_reloader_thread();
}

void spawn_async_zones_reloader_thread(void)
{
    spawn_zones_reloader_thread(false);
}

F_NONNULL
static void terminal_signal(struct ev_loop* loop, struct ev_signal* w, const int revents V_UNUSED)
{
    gdnsd_assert(revents == EV_SIGNAL);
    gdnsd_assert(w->signum == SIGTERM || w->signum == SIGINT);
    css_t* css = w->data;
    if (!css_stop_ok(css)) {
        log_err("Ignoring terminating signal %i because a takeover or replacement attempt is in progress!", w->signum);
    } else {
        log_info("Exiting cleanly on receipt of terminating signal %i", w->signum);
        killed_by = w->signum;
        ev_break(loop, EVBREAK_ALL);
    }
}

F_NONNULL
static void reload_zones_done(struct ev_loop* loop V_UNUSED, struct ev_async* a V_UNUSED, const int revents V_UNUSED)
{
    gdnsd_assert(revents == EV_ASYNC);
    css_t* css = a->data;
    const bool failed = join_zones_reloader_thread();

    if (failed)
        log_err("Reloading zone data failed");
    else
        log_info("Reloading zone data successful");

    if (css_notify_zone_reloaders(css, failed))
        spawn_async_zones_reloader_thread();
}

// called by ztree reloader thread just before it exits
void notify_reload_zones_done(void)
{
    ev_async_send(def_loop, async_reloadz);
}

// Set up our terminal signal handlers via libev
F_NONNULL
static void setup_signals(css_t* css)
{
    sig_int = malloc(sizeof(ev_signal));
    ev_signal_init(sig_int, terminal_signal, SIGINT);
    sig_int->data = css;
    ev_signal_start(def_loop, sig_int);

    sig_term = malloc(sizeof(ev_signal));
    ev_signal_init(sig_term, terminal_signal, SIGTERM);
    sig_term->data = css;
    ev_signal_start(def_loop, sig_term);
}

static void setup_reload_zones(css_t* css)
{
    async_reloadz = malloc(sizeof(ev_async));
    ev_async_init(async_reloadz, reload_zones_done);
    async_reloadz->data = css;
    ev_async_start(def_loop, async_reloadz);
}

F_NONNULL F_NORETURN
static void usage(const char* argv0)
{
    const char* def_cfdir = gdnsd_get_default_config_dir();
    fprintf(stderr,
            PACKAGE_NAME " version " PACKAGE_VERSION "\n"
            "Usage: %s [-c %s] [-D] [-l] [-S] [-T] <action>\n"
            "  -c - Configuration directory, default '%s'\n"
            "  -D - Enable verbose debug output\n"
            "  -l - Send logs to syslog rather than stderr\n"
            "  -S - Force 'zones_strict_data = true' for this invocation\n"
            "  -T - Allow downtime-less takeover of another instance\n"
            "Actions:\n"
            "  checkconf - Checks validity of config and zone files\n"
            "  start - Start as a regular foreground process\n"
            "  daemonize - Start as a background daemon (implies -l)\n"
            "\nFeatures: " BUILD_FEATURES
            "\nBuild Info: " BUILD_INFO
            "\nBug report URL: " PACKAGE_BUGREPORT
            "\nGeneral info URL: " PACKAGE_URL
            "\n",
            argv0, def_cfdir, def_cfdir
           );
    exit(2);
}

F_NONNULL
static void start_threads(socks_cfg_t* socks_cfg)
{
    dnsio_udp_init();
    unsigned num_tcp_threads = 0;
    for (unsigned i = 0; i < socks_cfg->num_dns_threads; i++)
        if (!socks_cfg->dns_threads[i].is_udp)
            num_tcp_threads++;
    dnsio_tcp_init(num_tcp_threads);

    // Block all signals using the pthreads interface while starting threads,
    //  which causes them to inherit the same mask.
    sigset_t sigmask_all;
    sigfillset(&sigmask_all);
    sigset_t sigmask_prev;
    sigemptyset(&sigmask_prev);
    if (pthread_sigmask(SIG_SETMASK, &sigmask_all, &sigmask_prev))
        log_fatal("pthread_sigmask() failed");

    // system scope scheduling, joinable threads
    pthread_attr_t attribs;
    pthread_attr_init(&attribs);
    pthread_attr_setdetachstate(&attribs, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&attribs, PTHREAD_SCOPE_SYSTEM);

    int pthread_err;

    for (unsigned i = 0; i < socks_cfg->num_dns_threads; i++) {
        dns_thread_t* t = &socks_cfg->dns_threads[i];
        if (t->is_udp)
            pthread_err = pthread_create(&t->threadid, &attribs, &dnsio_udp_start, t);
        else
            pthread_err = pthread_create(&t->threadid, &attribs, &dnsio_tcp_start, t);
        if (pthread_err)
            log_fatal("pthread_create() of DNS thread %u (for %s:%s) failed: %s",
                      i, t->is_udp ? "UDP" : "TCP", logf_anysin(&t->ac->addr), logf_strerror(pthread_err));
    }

    // Restore the original mask in the main thread, so
    //  we can continue handling signals like normal
    if (pthread_sigmask(SIG_SETMASK, &sigmask_prev, NULL))
        log_fatal("pthread_sigmask() failed");
    pthread_attr_destroy(&attribs);
}

static void request_io_threads_stop(socks_cfg_t* socks_cfg)
{
    dnsio_tcp_request_threads_stop();
    for (unsigned i = 0; i < socks_cfg->num_dns_threads; i++) {
        dns_thread_t* t = &socks_cfg->dns_threads[i];
        if (t->is_udp)
            pthread_kill(t->threadid, SIGUSR2);
    }
}

typedef enum {
    ACT_UNDEF = 0,
    ACT_CHECKCONF,
    ACT_START,
    ACT_DAEMONIZE
} cmdline_action_t;

typedef struct {
    const char* cfg_dir;
    bool force_zsd;
    bool takeover_ok;
    cmdline_action_t action;
} cmdline_opts_t;

F_NONNULL
static void parse_args(const int argc, char** argv, cmdline_opts_t* copts)
{
    int optchar;
    while ((optchar = getopt(argc, argv, "c:DlST"))) {
        switch (optchar) {
        case 'c':
            copts->cfg_dir = optarg;
            break;
        case 'D':
            gdnsd_log_set_debug(true);
            break;
        case 'l':
            gdnsd_log_set_syslog(true);
            break;
        case 'S':
            copts->force_zsd = true;
            break;
        case 'T':
            copts->takeover_ok = true;
            break;
        case -1:
            if (optind == (argc - 1)) {
                if (!strcasecmp("checkconf", argv[optind])) {
                    copts->action = ACT_CHECKCONF;
                    return;
                } else if (!strcasecmp("start", argv[optind])) {
                    copts->action = ACT_START;
                    return;
                } else if (!strcasecmp("daemonize", argv[optind])) {
                    copts->action = ACT_DAEMONIZE;
                    gdnsd_log_set_syslog(true);
                    return;
                }
            }
        // fall-through
        default:
            usage(argv[0]);
        }
    }
    usage(argv[0]);
}

int main(int argc, char** argv)
{
    umask(022);
    // Parse args, getting the config path
    //   returning the action.  Exits on cmdline errors,
    //   does not use assert/log stuff.
    cmdline_opts_t copts = {
        .cfg_dir = NULL,
        .force_zsd = false,
        .takeover_ok = false,
        .action = ACT_UNDEF
    };

    parse_args(argc, argv, &copts);
    gdnsd_assert(copts.action != ACT_UNDEF);

    // Initialize libgdnsd basic paths/config stuff
    if (copts.action != ACT_CHECKCONF)
        gdnsd_init_daemon(copts.action == ACT_DAEMONIZE);
    vscf_data_t* cfg_root = gdnsd_init_paths(copts.cfg_dir, copts.action != ACT_CHECKCONF);

    // Load full configuration and expose through the globals
    socks_cfg_t* socks_cfg = socks_conf_load(cfg_root);
    cfg_t* cfg = conf_load(cfg_root, socks_cfg, copts.force_zsd);
    gcfg = cfg;
    vscf_destroy(cfg_root);

    // Load zone data (final step if checkconf) synchronously
    ztree_init();
    if (initialize_zones())
        log_fatal("Initial load of zone data failed");

    if (copts.action == ACT_CHECKCONF)
        exit(0);

    // Initialize the network and PRNG bits of libgdnsd for runtime operation
    gdnsd_init_net();
    gdnsd_init_rand();

    // init locked control socket, can fail if concurrent daemon...
    csc_t* csc = NULL;
    css_t* css = css_new(argv[0], socks_cfg, NULL);
    if (!css) {
        if (!copts.takeover_ok)
            log_fatal("Another instance is running and has the control socket locked!");
        log_info("Another instance is running, connecting to control socket for takeover");
        csc = csc_new(13);
        log_info("Connected to existing instance v%s at pid %li",
                 csc_get_server_version(csc), (long)csc_get_server_pid(csc));
    }

    // init the stats code
    statio_init(socks_cfg->num_dns_threads);

    // Lock whole daemon into memory, including all future allocations.
    if (cfg->lock_mem)
        if (mlockall(MCL_CURRENT | MCL_FUTURE))
            log_fatal("mlockall(MCL_CURRENT|MCL_FUTURE) failed: %s (you may need to disabled the lock_mem config option if your system or your ulimits do not allow it)", logf_errno());

    // Initialize dnspacket stuff
    dnspacket_global_setup(socks_cfg);

    // Set up libev error callback
    ev_set_syserr_cb(&syserr_for_ev);

    // default ev loop in main process to handle statio, monitors, control
    // socket, signals, etc.
    def_loop = ev_default_loop(EVFLAG_AUTO);
    if (!def_loop)
        log_fatal("Could not initialize the default libev loop");

    // set up monitoring, which expects an initially empty loop
    gdnsd_mon_start(def_loop);

    // Call plugin pre-run actions
    gdnsd_plugins_action_pre_run();

    // Now that we're past potentially long-running operations like initial
    // monitoring and plugin pre_run actions, initiate the takeover operation
    // over the control socket connection.  During those long-running
    // operations, another starting daemon could beat us in a race here and
    // we'll be denied (unless we were initiated by "replace" fork->execve,
    // which gaurantees us winning and denies racers), or some other control
    // socket client could have requested the old server to stop, either of
    // which will cause failure here.
    if (!css)
        css = css_new(argv[0], socks_cfg, &csc);

    // main thread signal handlers
    setup_signals(css);

    // Initialize+bind DNS listening sockets
    socks_dns_lsocks_init(socks_cfg);

    // Start up all of the UDP and TCP i/o threads
    start_threads(socks_cfg);

    // This waits for all of the stat structures to be allocated by the i/o
    //  threads before continuing on.  They must be ready before ev_run()
    //  below, because statio event handlers hit them.
    // This also incidentally waits for all TCP threads to have hit their
    //  listen() call as well, whereas UDP is already at least buffering queued
    //  requests at the socket layer from the time it's bound.
    dnspacket_wait_stats(socks_cfg);

    // Notify the user that the listeners are up
    log_info("DNS listeners started");

    // Notify 3rd parties of readiness (systemd, or fg process if daemonizing)
    gdnsd_daemon_notify_ready();

    // Stop old daemon after establishing the new one's listeners
    if (csc) {
        if (!csc_stop_server(csc))
            csc_wait_stopping_server(csc);
        csc_delete(csc);
        csc = NULL;
    }

    // Set up zone reload mechanism and control socket handlers in the loop
    setup_reload_zones(css);
    css_start(css, def_loop);

    // The daemon stays in this libev loop for life,
    // until there's a reason to cleanly exit
    ev_run(def_loop, 0);

    // request i/o threads to exit
    request_io_threads_stop(socks_cfg);

    // Stop the terminal signal handlers
    ev_signal_stop(def_loop, sig_term);
    ev_signal_stop(def_loop, sig_int);

    // get rid of child procs (e.g. extmon helper)
    gdnsd_kill_registered_children();

    // wait for i/o threads to exit
    for (unsigned i = 0; i < socks_cfg->num_dns_threads; i++) {
        dns_thread_t* t = &socks_cfg->dns_threads[i];
        void* raw_exit_status = (void*)42U;
        int pthread_err = pthread_join(t->threadid, &raw_exit_status);
        if (pthread_err)
            log_err("pthread_join() of DNS thread failed: %s", logf_strerror(pthread_err));
        if (raw_exit_status != NULL)
            log_err("pthread_join() of DNS thread returned %p", raw_exit_status);
    }

    // deallocate resources in debug mode
    atexit_debug_execute();

    // We delete this last, because in the case of "gdnsdctl stop" this is
    // where the active connection to gdnsdctl will be broken, sending it into
    // a loop waiting on our PID to cease existing.
    css_delete(css);

#ifdef GDNSD_COVERTEST_EXIT
    // We have to use exit() when testing coverage, as raise()
    //   skips over writing out gcov data
    exit(0);
#else
    // kill self with same signal, so that our exit status is correct
    //   for any parent/manager/whatever process that may be watching
    if (killed_by)
        raise(killed_by);
    else
        exit(0);
#endif

    // raise should not return
    gdnsd_assert(0);
}
