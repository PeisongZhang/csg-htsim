// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef EVENTLIST_H
#define EVENTLIST_H

#include <map>
#include <vector>
#include <sys/time.h>
#include "config.h"
#include "loggertypes.h"

class EventList;
class TriggerTarget;

class EventSource : public Logged {
public:
    EventSource(EventList& eventlist, const string& name) : Logged(name), _eventlist(eventlist) {};
    EventSource(const string& name);
    virtual ~EventSource() {};
    virtual void doNextEvent() = 0;
    inline EventList& eventlist() const {return _eventlist;}
protected:
    EventList& _eventlist;
};

// ASTRA-sim/htsim frontend optimization: EventList now uses a vector-backed
// binary heap for the common sourceIsPending / doNextEvent path (much faster
// than std::multimap because of cache locality and zero per-event allocation).
// The original multimap path is retained as a fallback for the Handle-based
// API (eqds.cpp only), which compiles but is not linked into our front-end.
class EventList {
public:
    typedef multimap <simtime_picosec, EventSource*>::iterator Handle;
    EventList();
    static void setEndtime(simtime_picosec endtime); // end simulation at endtime (rather than forever)
    static bool doNextEvent(); // returns true if it did anything, false if there's nothing to do
    static void sourceIsPending(EventSource &src, simtime_picosec when);
    static Handle sourceIsPendingGetHandle(EventSource &src, simtime_picosec when);
    static void sourceIsPendingRel(EventSource &src, simtime_picosec timefromnow)
    { sourceIsPending(src, EventList::now()+timefromnow); }
    static void cancelPendingSource(EventSource &src);
    // optimized cancel, if we know the expiry time
    static void cancelPendingSourceByTime(EventSource &src, simtime_picosec when);
    // optimized cancel by handle - be careful to ensure handle is still valid
    static void cancelPendingSourceByHandle(EventSource &src, Handle handle);
    static void reschedulePendingSource(EventSource &src, simtime_picosec when);
    static void triggerIsPending(TriggerTarget &target);
    static inline simtime_picosec now() {return EventList::_lasteventtime;}
    static Handle nullHandle() {return _slow_map.end();}


    static EventList& getTheEventList();
    EventList(const EventList&)      = delete;  // disable Copy Constructor
    void operator=(const EventList&) = delete;  // disable Assign Constructor

private:
    static simtime_picosec _endtime;
    static simtime_picosec _lasteventtime;

    // Primary fast-path: min-heap of (when, src, seq).  `seq` is a strictly
    // increasing tie-breaker so heap order matches multimap FIFO-by-insertion
    // within the same time.
    struct FastEntry {
        simtime_picosec when;
        uint64_t seq;
        EventSource* src;
    };
    struct FastEntryGreater {
        bool operator()(const FastEntry& a, const FastEntry& b) const {
            // std::*_heap default produces a max-heap; invert for min-heap.
            if (a.when != b.when) return a.when > b.when;
            return a.seq > b.seq;
        }
    };
    static std::vector<FastEntry> _heap;
    static uint64_t _heap_seq;

    // Slow-path fallback for Handle-based API (eqds only; not exercised in
    // our build).  Kept live so eqds.cpp still compiles.
    typedef multimap <simtime_picosec, EventSource*> slow_map_t;
    static slow_map_t _slow_map;

    static std::vector <TriggerTarget*> _pending_triggers;

    static int _instanceCount;
    static EventList* _theEventList;
};

#endif
