// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-        
#include <iostream>
#include "clock.h"
#include "eventlist.h"

Clock::Clock(simtime_picosec period, EventList& eventlist)
  : EventSource(eventlist,"clock"), 
    _period(period), _smallticks(0)
{
    eventlist.sourceIsPendingRel(*this, period);
}

void
Clock::doNextEvent() {
    eventlist().sourceIsPendingRel(*this, _period);
    // AstraSim: the dot/pipe per-tick output can dominate disk when
    // ASTRA-sim runs htsim for hundreds of seconds simtime (30 GB seen
    // at 128 NPU/300s).  Silence by default; restore with
    // ASTRASIM_HTSIM_VERBOSE.
    static const bool _astrasim_verbose = (std::getenv("ASTRASIM_HTSIM_VERBOSE") != nullptr);
    if (_astrasim_verbose) {
        if (_smallticks<10) {
            cout << '.' << flush;
            _smallticks++;
        } else {
            cout << '|' << flush;
            _smallticks=0;
        }
    } else {
        if (_smallticks<10) _smallticks++;
        else _smallticks = 0;
    }
}
