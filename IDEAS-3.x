======================
Basic Improvements (could go in future 2.N releases, technically, but 3.x timeline isn't that awfully long):
======================
* adhere to edns-client-subnet RFC, which clarified many unclear things from back when we implemented.
* add EDNS0 cookie support?
* add more-advanced TCP support
* Consider adding DNS-over-TLS support, or at least validating that another TLS proxy daemon (e.g. haproxy, nginx, etc) can be used for it.

======================
Code Quality:
======================
* VLAs vs malloc contentions.  Why isn't there a specifically-thread-local malloc? :P
* size_t-ification of many of our current "unsigned"?  Is it worth the time spent dealing with the conversion, does it cause a speed hit in places?
* Make more objects where possible (sub-structs)?
* Cleanup globals, eliminate where possible
* Destruct some things that aren't currently destructed on shutdown, e.g. gcfg, socks_cfg?
* Add clang-tidy to qa/ ?
* afl-fuzz for DNS packets?

======================
Remaining daemon-takeover-related topics:
======================
* Hand off accumulated stats during takeover?
** It's a little complicated but theoretically possible.
** Doing it as a JSON import would mean adding a JSON parser (ewww)
** Doing it as a memory block would meaning designing the layout well for future updates
*** And if we did that, we might as well just switch the export to that form over shared memory, and have the gdnsdctl tool handle output formatting to e.g. JSON.
** Regardless, should we split up "stats" (dnsio) from "state" (monitoring) as separate sets of data anyways?

======================
Perf:
======================
* Optionally auto-detect cpu count for e.g. "udp_threads = auto"?  Default?
* Optional CPU pinning of dnsio threads? (also, data reloader threads and/or main thread?)
* setsockopt(SO_INCOMINGCPU) for Linux 4.5+ seems like a related win.
** But I've heard rumblings the newer and more-flexible (and more-complicated :/) REUSEPORT_EBPF stuff broke basic SO_INCOMINGCPU :/

======================
Zone Data:
======================
* Do we need still need a separate ztree? It was split for async single-zone reloading, which was gutted.  Do we want to support adding back an optional backend that acts similarly to rfc1035_auto in the future, and thus preserve this?
* Assuming we're going down the no-ztree path:
** should we bring back cross-zone warn/fatal checks (e.g. warning about PTR/A mismatch, etc)?
** should we bring back $INCLUDE support?
** Should we kill ooz-glue feature while we're here?  it's kind of silly to support it and might simplify code a bit
* Earlier changes broke defaulting zscan_djb's defaulting of zone serial to mtime, does that need restoring?

======================
Other Data (geoip db, nets file, admin_state):
======================
* Should all of these become explicitly-triggered via gdnsdctl as well?  Optional either way?

======================
Plugins:
======================
* Let's jettison plugins completely???
** Complexity/abstraction/indirection have costs
** Poorly-design interfaces that are unstable, and very little real-world use
** Probably most of the pragmatic problem-space is explored in the current
   in-tree plugins, and source patches are always possible and probably simpler,
   to add new functionality.
** What to do with them:
** General:
*** static, null can be ignored/dropped, they're just examples
** Monitoring:
*** generic interfaces are state-files and custom command execution, with tcp/http monitor code preserved to make common stuff easy.
*** extfile remains in spirit, but is in core like admin_state
*** extmon also remains in spirit, but rolled into core
*** tcp+http rolled into extmon helper, which is only spawned if any cmd/tcp/http services are defined
** Resolvers:
*** There will be an internal DYN[AC] resolver "API", but it's for source patches, kinda like ztree + rfc1035/djb.
*** simplefo, multifo, metafo, weighted, and geoip can be collapsed into a single resolver, as they have a ton of overlap (e.g. multifo can be emulated using weighted with even weights, etc), leaving only "reflect" as separate.
**** eliminate synthesis
**** mode=>linear|random|multi|mapped.  Weights default to 1.0, layering via self-reference.
**** Q? mapped should layer as well, splitting "nets" from geoip?

* Remove the whole dynamic-TTL concept, as it's fundamentally incompatible/blocking for other improvements (state-triggering, proportional geoip recalculation, etc).
* Replace with "weight", which should be runtime-tunable via admin_state, at least, at all layers.
* Q's about collapsing configured+monitoring+admin state/weight, and whether we do a generic abstraction of state/weight aggregation to avoid recursing through layers for runtime resolve?
* Switch to a model where plugins register interest in monitored/admin_state state changes and get callbacks in the main thread, and do their own recalculations and PRCU-swaps.  Their runtime lookup code gets much simpler/faster, too.

======================
plugin_geoip:
======================
* Load databases once globally (currently the same DB file referenced by N maps is loaded N times)
* Switch runtime per-network results to single-depth rather than lists.  This enables other stuff below and gives us larger edns scope masks in responses.
* Add simple per-DC "weight" value which acts as a distance multiplier and is proportional.  client_dist = raw_dist*(this_dc_weight/sum_dc_weights).  Should be settable from admin_state as well (while we're at it, maybe plugin_weighted should get admin_state weight support, too).
* Design for having multiple distance algorithm choices configurable, try to design to allow for a future one that takes a client population map into account for true load weighting.
** Short of client population mapping, a simpler improvement would just be to approximate the network better.  The current geographic distances don't map well to submarine cables and such.  But when you look at cable maps, there's a sort of "internet equator" that runs a bit north of the real equator, and is kinda-straight.  Or at a little more precision, it kinda looks like a sine wave (rises over western hemisphere, drops over asia).  There might be a relatively-simple calculation which approximates following some approximation of that path for long-distance calculations.
