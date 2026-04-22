// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-        
#include "queue_lossless_input.h"
#include <math.h>
#include <iostream>
#include <sstream>
#include "switch.h"

uint64_t LosslessInputQueue::_high_threshold = 0;
uint64_t LosslessInputQueue::_low_threshold = 0;

LosslessInputQueue::LosslessInputQueue(EventList& eventlist)
    : Queue(speedFromGbps(1),Packet::data_packet_size()*2000,eventlist,NULL),
      VirtualQueue(),
      _state_recv(READY)
{
    assert(_high_threshold>0);
    assert(_high_threshold > _low_threshold);

    _wire = NULL;
}

LosslessInputQueue::LosslessInputQueue(EventList& eventlist,BaseQueue* peer)
    : Queue(speedFromGbps(1),Packet::data_packet_size()*2000,eventlist,NULL),
      VirtualQueue(),
      _state_recv(READY)
{
    assert(_high_threshold>0);
    assert(_high_threshold > _low_threshold);

    stringstream ss;
    ss << "VirtualQueue("<< peer->_name<< ")";
    _nodename = ss.str();
    _remoteEndpoint = peer;
    _switch = NULL;
    _wire = NULL;

    peer->setRemoteEndpoint(this);
}

LosslessInputQueue::LosslessInputQueue(EventList& eventlist,BaseQueue* peer, Switch* sw, simtime_picosec wire_latency)
    : Queue(speedFromGbps(1),Packet::data_packet_size()*2000,eventlist,NULL),
      VirtualQueue(),
      _state_recv(READY)
{
    assert(_high_threshold>0);
    assert(_high_threshold > _low_threshold);

    stringstream ss;
    ss << "VirtualQueue("<< peer->_name<< ")";
    _nodename = ss.str();
    _remoteEndpoint = peer;
    _switch = sw;

    _wire = new CallbackPipe(wire_latency, eventlist, _remoteEndpoint);

    assert(_switch);

    peer->setRemoteEndpoint(this);
}


void
LosslessInputQueue::receivePacket(Packet& pkt)
{
    // AstraSim: when the very next hop in the route is the terminal sink
    // (e.g. RoceSink/TcpSink), there is no downstream output queue to call
    // completedService back on us, so tracking backlog here would leak and
    // eventually stall the link via a stuck PAUSE.  Detect that case and
    // just forward without enqueue tracking — PFC backpressure is pointless
    // at the last hop because the sink is the consumer.
    const bool next_is_terminal = pkt.route()
        && (pkt.nexthop() + 1 == pkt.route()->size());

    if (!next_is_terminal) {
        /* normal packet, enqueue it */
        _queuesize += pkt.size();

        //send PAUSE notifications if that is the case!
        assert(_queuesize > 0);
        if ((uint64_t)_queuesize > _high_threshold && _state_recv!=PAUSED){
            _state_recv = PAUSED;
            sendPause(1000);
        }

        if (_queuesize > _maxsize){
            static const bool _astrasim_verbose = (std::getenv("ASTRASIM_HTSIM_VERBOSE") != nullptr);
            if (_astrasim_verbose) {
                cout << " Queue " << _name << " LOSSLESS not working! I should have dropped this packet" << _queuesize / Packet::data_packet_size() << endl;
            }
        }
    }

    //tell the output queue we're here!
    if (pkt.nexthop() < pkt.route()->size()){
        //this should not work...
        //assert(0);
        pkt.sendOn2(this);
    }
    else {
        assert(_switch);
        pkt.set_ingress_queue(this);
        _switch->receivePacket(pkt);
    }
}

void LosslessInputQueue::completedService(Packet& pkt){
    _queuesize -= pkt.size();

    //unblock if that is the case
    assert(_queuesize >= 0);
    if ((uint64_t)_queuesize < _low_threshold && _state_recv == PAUSED) {
        _state_recv = READY;
        sendPause(0);
    }
}

void LosslessInputQueue::sendPause(unsigned int wait){
    static const bool _astrasim_verbose = (std::getenv("ASTRASIM_HTSIM_VERBOSE") != nullptr);
    if (_astrasim_verbose) {
        cout << "[pfc] t=" << (eventlist().now() / 1e6) << "us "
             << _nodename << " sendPause wait=" << wait
             << " qsize=" << _queuesize << endl;
    }
    uint32_t switchID = 0;
    if (_switch)
        switchID = getSwitch()->getID();

    EthPausePacket* pkt = EthPausePacket::newpkt(wait,switchID);

    if (_wire)
        _wire->receivePacket(*pkt);
    else
        getRemoteEndpoint()->receivePacket(*pkt);
};
