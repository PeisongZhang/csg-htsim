// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-

#include "eventlist.h"
#include "trigger.h"
#include <algorithm>
#include <cstdlib>

simtime_picosec EventList::_endtime = 0;
simtime_picosec EventList::_lasteventtime = 0;
std::vector<EventList::FastEntry> EventList::_heap;
uint64_t EventList::_heap_seq = 0;
EventList::slow_map_t EventList::_slow_map;
std::vector <TriggerTarget*> EventList::_pending_triggers;
int EventList::_instanceCount = 0;
EventList* EventList::_theEventList = nullptr;

EventList::EventList()
{
    if (EventList::_instanceCount != 0)
    {
        std::cerr << "There should be only one instance of EventList. Abort." << std::endl;
        abort();
    }

    EventList::_theEventList = this;
    EventList::_instanceCount += 1;
    // §24.7/D8 fix: pre-allocate configurable initial capacity to avoid
    // vector's doubling-with-copy pattern during large-scale runs.  At 256-NPU
    // gpt_39b L48 B256, the event queue peaks at ~50M entries; without
    // pre-reserve, vector grows 65K→131K→...→64M with each step copying the
    // old buffer → transient 2× RSS spike that accounted for ~4 GB per shard
    // in the golden re-verify (§24.7).  Pre-reserving eliminates that spike.
    //
    // Default: 65K entries (1.5 MB).  For large workloads, set
    // ASTRASIM_HTSIM_EVENT_RESERVE to the expected peak event count, e.g.
    //   ASTRASIM_HTSIM_EVENT_RESERVE=50000000   # gpt_39b L48 B256 (1.2 GB)
    //   ASTRASIM_HTSIM_EVENT_RESERVE=500000000  # gpt_39b L48 B1536 (12 GB)
    // The reserved buffer is amortized over the run; setting too low pays a
    // few doubling copies, setting too high wastes one-time RSS but avoids
    // any transient spikes.
    size_t reserve_n = 1u << 16;
    if (const char* e = std::getenv("ASTRASIM_HTSIM_EVENT_RESERVE")) {
        const long long v = std::atoll(e);
        if (v > 0) reserve_n = static_cast<size_t>(v);
    }
    EventList::_heap.reserve(reserve_n);
}

EventList&
EventList::getTheEventList()
{
    if (EventList::_theEventList == nullptr)
    {
        EventList::_theEventList = new EventList();
    }
    return *EventList::_theEventList;
}

void
EventList::setEndtime(simtime_picosec endtime)
{
    EventList::_endtime = endtime;
}

bool
EventList::doNextEvent()
{
    // triggers happen immediately - no time passes; no guarantee that
    // they happen in any particular order (don't assume FIFO or LIFO).
    if (!_pending_triggers.empty()) {
        TriggerTarget *target = _pending_triggers.back();
        _pending_triggers.pop_back();
        target->activate();
        return true;
    }

    // Pick the earlier of heap top and slow_map front.  Slow_map is almost
    // always empty (eqds unused), so the multimap check is a single hot-cache
    // lookup per event.
    const bool heap_has  = !_heap.empty();
    const bool slow_has  = !_slow_map.empty();
    if (!heap_has && !slow_has)
        return false;

    simtime_picosec next_time;
    EventSource* next_src;
    bool pick_heap;
    if (heap_has && slow_has) {
        pick_heap = (_heap.front().when <= _slow_map.begin()->first);
    } else {
        pick_heap = heap_has;
    }

    if (pick_heap) {
        next_time = _heap.front().when;
        next_src  = _heap.front().src;
        std::pop_heap(_heap.begin(), _heap.end(), FastEntryGreater());
        _heap.pop_back();
    } else {
        next_time = _slow_map.begin()->first;
        next_src  = _slow_map.begin()->second;
        _slow_map.erase(_slow_map.begin());
    }

    assert(next_time >= _lasteventtime);
    _lasteventtime = next_time;
    next_src->doNextEvent();
    return true;
}


void
EventList::sourceIsPending(EventSource &src, simtime_picosec when)
{
    assert(when>=now());
    if (_endtime==0 || when<_endtime) {
        _heap.push_back({when, _heap_seq++, &src});
        std::push_heap(_heap.begin(), _heap.end(), FastEntryGreater());
    }
}

EventList::Handle
EventList::sourceIsPendingGetHandle(EventSource &src, simtime_picosec when)
{
    // Slow path: only used by eqds.  Keeps stable-iterator semantics.
    assert(when>=now());
    if (_endtime==0 || when<_endtime) {
        return _slow_map.insert(make_pair(when,&src));
    }
    return _slow_map.end();
}

void
EventList::triggerIsPending(TriggerTarget &target) {
    _pending_triggers.push_back(&target);
}

void
EventList::cancelPendingSource(EventSource &src) {
    // Unused in our front-end (only swift/mpswift/strack call this, and those
    // aren't linked).  Implemented as a correct but slow linear scan over the
    // heap plus the slow_map, followed by make_heap to restore invariant.
    bool heap_changed = false;
    for (auto it = _heap.begin(); it != _heap.end(); ) {
        if (it->src == &src) {
            it = _heap.erase(it);
            heap_changed = true;
        } else {
            ++it;
        }
    }
    if (heap_changed) {
        std::make_heap(_heap.begin(), _heap.end(), FastEntryGreater());
    }
    for (auto it = _slow_map.begin(); it != _slow_map.end(); ) {
        if (it->second == &src) {
            it = _slow_map.erase(it);
        } else {
            ++it;
        }
    }
}

void
EventList::cancelPendingSourceByTime(EventSource &src, simtime_picosec when) {
    // Fast cancellation given an expected time.  Search the slow_map first
    // (correct by construction since this is only called for handle-holding
    // scenarios), then fall back to linear heap scan.
    auto range = _slow_map.equal_range(when);
    for (auto i = range.first; i != range.second; ++i) {
        if (i->second == &src) {
            _slow_map.erase(i);
            return;
        }
    }
    for (auto it = _heap.begin(); it != _heap.end(); ++it) {
        if (it->src == &src && it->when == when) {
            _heap.erase(it);
            std::make_heap(_heap.begin(), _heap.end(), FastEntryGreater());
            return;
        }
    }
    abort();
}


void EventList::cancelPendingSourceByHandle(EventSource &src, EventList::Handle handle) {
    // Handle API always targets the slow_map (see sourceIsPendingGetHandle).
    assert(handle->second == &src);
    assert(handle != _slow_map.end());
    assert(handle->first >= now());

    _slow_map.erase(handle);
}

void
EventList::reschedulePendingSource(EventSource &src, simtime_picosec when) {
    cancelPendingSource(src);
    sourceIsPending(src, when);
}

EventSource::EventSource(const string& name) : EventSource(EventList::getTheEventList(), name)
{
}
