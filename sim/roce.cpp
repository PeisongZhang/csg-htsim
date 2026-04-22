// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include <math.h>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include "roce.h"
#include "queue.h"
#include <stdio.h>
#include "switch.h"
#include "trigger.h"
#include "ecn.h"   // AstraSim U3 — ECN_CE / ECN_ECHO
using namespace std;

////////////////////////////////////////////////////////////////
//  ROCE SOURCE
////////////////////////////////////////////////////////////////

/* When you're debugging, sometimes it's useful to enable debugging on
   a single ROCE receiver, rather than on all of them.  Set this to the
   node ID and recompile if you need this; otherwise leave it
   alone. */
//#define LOGSINK 2332
#define LOGSINK   0 


/* keep track of RTOs.  Generally, we shouldn't see RTOs if
   return-to-sender is enabled.  Otherwise we'll see them with very
   large incasts. */
uint32_t RoceSrc::_global_node_count = 0;
uint32_t RoceSrc::_global_rto_count = 0;

/* _min_rto can be tuned using SetMinRTO. Don't change it here.  */
simtime_picosec RoceSrc::_min_rto = timeFromUs((uint32_t)DEFAULT_RTO_MIN);

RoceSrc::RoceSrc(RoceLogger* logger, TrafficLogger* pktlogger, EventList &eventlist, linkspeed_bps rate)
    : BaseQueue(rate,eventlist,NULL), _flow(pktlogger), _logger(logger)
{
    _mss = Packet::data_packet_size();
    _end_trigger = NULL;

    _stop_time = 0;
    _flow_started = false;
    _base_rtt = timeInf;
    _acked_packets = 0;
    _packets_sent = 0;
    _new_packets_sent = 0;
    _rtx_packets_sent = 0;
    _acks_received = 0;
    _nacks_received = 0;

    _highest_sent = 0;
    _last_acked = 0;
    _dstaddr = UINT32_MAX;

    _sink = 0;
    _done = false;

    _rtt = 0;
    _rto = timeFromMs(20);
    _mdev = 0;
    _drops = 0;
    _flow_size = ((uint64_t)1)<<63;
  
    _node_num = _global_node_count++;
    _nodename = "rocesrc " + to_string(_node_num);

    // AstraSim: upstream re-seeds rand() from wall-time on every RoceSrc
    // construction.  With thousands of flows that causes subtle run-to-run
    // non-determinism in RTO jitter and path selection.  HTSim frontend
    // seeds rand()/random() once during session init instead.
    _pathid = random()%256;

    //cout << _nodename << " path id is " << _pathid << endl;

    // debugging hack
    _log_me = false;
    //if (get_id() == 144212)
    //    _log_me = true;

    _state_send = READY;
    _time_last_sent = 0;
    _packet_spacing = (simtime_picosec)((Packet::data_packet_size()+RocePacket::ACKSIZE) * (pow(10.0,12.0) * 8) / _bitrate);

    // AstraSim U3 — DCQCN defaults (overridden by enable_dcqcn if called).
    _cc_current_bps = _bitrate;
    _cc_target_bps = _bitrate;
    uint64_t ai = _bitrate / 100;
    if (ai < 1000000ULL) ai = 1000000ULL;
    if (ai > 50000000ULL) ai = 50000000ULL;
    _cc_ai_bps = ai;
    uint64_t minr = _bitrate / 1000;
    if (minr < 100000000ULL) minr = 100000000ULL;
    _cc_min_bps = minr;
}

void RoceSrc::enable_dcqcn(linkspeed_bps ai_bps,
                           linkspeed_bps min_bps,
                           uint64_t byte_threshold,
                           double g) {
    _cc_dcqcn = true;
    if (ai_bps > 0) _cc_ai_bps = ai_bps;
    if (min_bps > 0) _cc_min_bps = min_bps;
    if (byte_threshold > 0) _cc_update_byte_threshold = byte_threshold;
    if (g > 0.0) _cc_g = g;
    _cc_current_bps = _bitrate;
    _cc_target_bps = _bitrate;
    _cc_alpha = 0.0;
    _cc_bytes_since_update = 0;
    _cc_marked_ack_ct = 0;
    _cc_unmarked_ack_ct = 0;
    _cc_incstage = 0;
    _cc_unmarked_runs = 0;
}

/*mem_b RoceSrc::queuesize(){
  return 0;
  }

  mem_b RoceSrc::maxsize(){
  return 0;
  }*/

void RoceSrc::set_traffic_logger(TrafficLogger* pktlogger) {
    _flow.set_logger(pktlogger);
}

void RoceSrc::log_me() {
    // avoid looping
    if (_log_me == true)
        return;

    cout << "Enabling logging on RoceSrc " << _nodename << endl;
    _log_me = true;
    if (_sink)
        _sink->log_me();
}

void RoceSrc::startflow(){
    static const bool _astrasim_verbose_startflow = (std::getenv("ASTRASIM_HTSIM_VERBOSE") != nullptr);
    if (_astrasim_verbose_startflow)
        cout << "startflow " << _flow._name << " at " << timeAsUs(eventlist().now()) << endl;
    _flow_started = true;
    _highest_sent = 0;
    _last_acked = 0;
    
    _acked_packets = 0;
    _packets_sent = 0;
    _done = false;
    
    eventlist().sourceIsPendingRel(*this,0);
}

void RoceSrc::set_end_trigger(Trigger& end_trigger) {
    _end_trigger = &end_trigger;
}

void RoceSrc::connect(Route* routeout, Route* routeback, RoceSink& sink, simtime_picosec starttime) {
    assert(routeout);
    _route = routeout;
    
    _sink = &sink;
    _flow.set_id(get_id()); // identify the packet flow with the ROCE source that generated it
    _flow._name = _name;
    _sink->connect(*this, routeback);

    if (starttime != TRIGGER_START) {
        //eventlist().sourceIsPending(*this,starttime);
        startflow();
    }
    //else cout << "TRIGGER START " << _nodename << endl; 
}

/* Process a NACK.  Generally this involves queuing the NACKed packet
   for retransmission, but then waiting for a PULL to actually resend
   it.  However, sometimes the NACK has the PULL bit set, and then we
   resend immediately */
void RoceSrc::processNack(const RoceNack& nack){
    _last_acked = nack.ackno();
    _rtx_packets_sent += _highest_sent - _last_acked;

    if (_log_me)
        cout << "Src " << get_id() << " go back n from " <<  _highest_sent << " to " << _last_acked << " at " << timeAsUs(eventlist().now()) << " us" << endl;

    if (_flow_size && _highest_sent>=_flow_size && _last_acked < _flow_size){
        //restart the pacing of packets, this has stopped once we've passed the flow size but now a packet in the last window was lost.
        if (_log_me)
            cout << "Src " << get_id() << " restarting pacing\n";
        eventlist().sourceIsPendingRel(*this,0);
    }

    _highest_sent = _last_acked;
    _nacks_received ++;

    //this packet be sent when it is time to send a new packet!
}

/* Process an ACK.  Mostly just housekeeping*/
void RoceSrc::processAck(const RoceAck& ack) {
    RoceAck::seq_t ackno = ack.ackno();
    simtime_picosec ts = ack.ts();

    // Compute rtt.  This comes originally from TCP, and may not be optimal for ROCE */
    uint64_t m = eventlist().now()-ts;

    if (m!=0){
        if (_rtt>0){
            uint64_t abs;
            if (m>_rtt)
                abs = m - _rtt;
            else
                abs = _rtt - m;

            _mdev = 3 * _mdev / 4 + abs/4;
            _rtt = 7*_rtt/8 + m/8;

            _rto = _rtt + 4*_mdev;
        } else {
            _rtt = m;
            _mdev = m/2;
            _rto = _rtt + 4*_mdev;
        }
        if (_base_rtt==timeInf || _base_rtt > m)
            _base_rtt = m;
    }

    if (_rto < _min_rto)
        _rto = _min_rto * ((drand() * 0.5) + 0.75);

    // AstraSim U3 — DCQCN AIMD CC update.  Gated on _cc_dcqcn (set by the
    // DCQCN frontend via enable_dcqcn()).
    if (_cc_dcqcn) {
        uint64_t bytes_acked = 0;
        if (ackno > _last_acked) bytes_acked = ackno - _last_acked;
        _cc_bytes_since_update += bytes_acked;

        bool marked = (ack.flags() & ECN_ECHO) != 0;
        if (marked) _cc_marked_ack_ct++; else _cc_unmarked_ack_ct++;

        _cc_alpha = (1.0 - _cc_g) * _cc_alpha + _cc_g * (marked ? 1.0 : 0.0);

        bool changed = false;
        if (marked) {
            _cc_target_bps = _cc_current_bps;
            double factor = 1.0 - _cc_alpha / 2.0;
            if (factor < 0.0) factor = 0.0;
            uint64_t new_bps = (uint64_t)(_cc_current_bps * factor);
            if (new_bps < _cc_min_bps) new_bps = _cc_min_bps;
            _cc_current_bps = new_bps;
            _cc_incstage = 0;
            _cc_unmarked_runs = 0;
            changed = true;
        }

        if (_cc_bytes_since_update >= _cc_update_byte_threshold) {
            bool had_marks = (_cc_marked_ack_ct > 0);
            _cc_bytes_since_update = 0;
            _cc_marked_ack_ct = 0;
            _cc_unmarked_ack_ct = 0;
            if (!had_marks) {
                _cc_unmarked_runs++;
                if (_cc_unmarked_runs >= 5) {
                    uint64_t mid = (_cc_current_bps + _cc_target_bps) / 2;
                    uint64_t new_bps = mid + _cc_ai_bps;
                    if (new_bps > _bitrate) new_bps = _bitrate;
                    _cc_current_bps = new_bps;
                } else {
                    uint64_t new_bps = _cc_current_bps + _cc_ai_bps;
                    if (new_bps > _bitrate) new_bps = _bitrate;
                    _cc_current_bps = new_bps;
                }
                _cc_incstage++;
                changed = true;
            }
        }

        if (changed && _cc_current_bps > 0) {
            _packet_spacing = (simtime_picosec)(
                (Packet::data_packet_size() + RocePacket::ACKSIZE) *
                (pow(10.0, 12.0) * 8.0) / (double)_cc_current_bps);
        }
    }

    if (ackno > _last_acked) { // a brand new ack
        // we should probably cancel the rtx timer for any acked by
        // the cumulative ack, but we'll get an ACK or NACK anyway in
        // due course.
        _last_acked = ackno;
    }
    if (_logger) _logger->logRoce(*this, RoceLogger::ROCE_RCV);

    if (_log_me)
        cout << "Src " << get_id() << " ackno " << ackno << endl;
    if (ackno >= _flow_size){
        static const bool _astrasim_verbose_roce = (std::getenv("ASTRASIM_HTSIM_VERBOSE") != nullptr);
        if (_astrasim_verbose_roce) {
            cout << "Flow " << _name << " " << get_id() << " finished at " << timeAsUs(eventlist().now()) << " total bytes " << ackno << endl;
        }
        // AstraSim hook — fire send-finished callback exactly once.
        if (astrasim_flow_finish_send_cb && !_astrasim_send_finished) {
            _astrasim_send_finished = true;
            int tag = _flow.flow_id();
            int src_id = _debug_srcid;
            int dst_id = _debug_dstid;
            if (_astrasim_verbose_roce) {
                std::cout << "Finish sending flow " << tag << " from " << src_id
                          << " to " << dst_id << std::endl;
            }
            astrasim_flow_finish_send_cb(src_id, dst_id, _flow_size, tag);
        }
        _done = true;
        if (_end_trigger) {
            _end_trigger->activate();
        }

        return;
    }
}

void RoceSrc::processPause(const EthPausePacket& p) {
    if (p.sleepTime()>0){
        //remote end is telling us to shut up.
        //cout << "Source " << str() << " PAUSE " << timeAsUs(eventlist().now()) << endl;
        //assert(_state_send != PAUSED);
        _state_send = PAUSED;
    } else {
        //we are allowed to send!
        //assert(_state_send != READY);
        _state_send = READY;
        //cout << "Source " << str() << " RESUME " << timeAsUs(eventlist().now()) << endl;
        eventlist().sourceIsPendingRel(*this,0);
    }
}

void RoceSrc::receivePacket(Packet& pkt) 
{
    if (!_flow_started){
        assert(pkt.type()==ETH_PAUSE);
        return; 
    }

    if (_stop_time && eventlist().now() >= _stop_time) {
        // stop sending new data, but allow us to finish any retransmissions
        _flow_size = _highest_sent+_mss;
        _stop_time = 0;
    }

    if (_done)
        return;

    switch (pkt.type()) {
    case ETH_PAUSE:
        processPause((const EthPausePacket&)pkt);
        pkt.free();
        return;
    case ROCENACK: 
        _nacks_received++;
        processNack((const RoceNack&)pkt);
        pkt.free();
        return;
    case ROCEACK:
        _acks_received++;
        processAck((const RoceAck&)pkt);
        pkt.free();
        return;
    default:
        abort();
    }
}

// Note: the data sequence number is the number of Byte1 of the packet, not the last byte.
void RoceSrc::send_packet() {
    RocePacket* p = NULL;
    bool last_packet = false;
    if (_log_me)
        cout << "Src " << get_id() << " send_packet\n";
    assert(_flow_started);

    if (_flow_size && (_last_acked >= _flow_size || _highest_sent > _flow_size)) {
        //flow is finished
        if (_log_me)
            cout << "Src " << get_id() << " flow is finished, not sending\n";
        return;
    }

    if (_flow_size && _highest_sent + _mss >= _flow_size) {
        last_packet = true;
        if (_log_me) {
            cout << _name << " " << get_id() << " sending last packet with SEQNO " << _highest_sent+1 << " at " << timeAsUs(eventlist().now()) << endl;
        }
    }

    p = RocePacket::newpkt(_flow, *_route, _highest_sent+1, _mss, false, last_packet,_dstaddr);
    
    assert(p);
    p->set_pathid(_pathid);

    p->flow().logTraffic(*p,*this,TrafficLogger::PKT_CREATESEND);
    p->set_ts(eventlist().now());
    
    if (_log_me) {
        cout << "Src " << get_id() << " sent " << _highest_sent+1 << " Flow Size: " << _flow_size << endl;
    }
    _highest_sent += _mss;
    _packets_sent++;

    //cout << "Sent " << _highest_sent+1 << " Flow Size: " << _flow_size << " Flow " << _name << " time " << timeAsUs(eventlist().now()) << endl;

    p->sendOn();
}

void RoceSrc::doNextEvent() {
    /*if (!_flow_started){
      startflow();
      return;
      }*/

    assert(_flow_started);
    if (_log_me) 
        cout << "Src " << get_id() << " do next event\n";
        

    if (_state_send==PAUSED) {
        if (_log_me) 
            cout << "Src " << get_id() << " paused\n";
        return;
    }

    if (_flow_size && _highest_sent >= _flow_size) { 
        if (_log_me) 
            cout << "Src " << get_id()  << " stopping send coz highest_sent is " << _highest_sent << endl;
        return;
    }

    if (_time_last_sent==0 || eventlist().now() - _time_last_sent >= _packet_spacing){
        send_packet();
        _time_last_sent = eventlist().now();
    }

    simtime_picosec next_send = _time_last_sent + _packet_spacing;
    assert(next_send > eventlist().now());

    eventlist().sourceIsPending(*this, next_send);
}

////////////////////////////////////////////////////////////////
//  ROCE SINK
////////////////////////////////////////////////////////////////

/* Only use this constructor when there is only one for to this receiver */
RoceSink::RoceSink()
    : DataReceiver("roce_sink"),_cumulative_ack(0) , _total_received(0) 
{
    _src = 0;
    
    _nodename = "rocesink";
    _highest_seqno = 0;
    _log_me = false;
    //if (get_id() == 144214)
    //    _log_me = true;
    _total_received = 0;
}

void RoceSink::log_me() {
    // avoid looping
    if (_log_me == true)
        return;

    _log_me = true;

    if (_src)
        _src->log_me();  
}

/* Connect a src to this sink. */ 
void RoceSink::connect(RoceSrc& src, Route* route)
{
    _src = &src;
    _route = route;
    _cumulative_ack = 0;
    _drops = 0;
}


// Receive a packet.
// Note: _cumulative_ack is the last byte we've ACKed.
// seqno is the first byte of the new packet.
void RoceSink::receivePacket(Packet& pkt) {
    /*
      if (random()%10==0){
      pkt.free();
      return;
      }*/

    assert(pkt.dst () == _src->_dstaddr);

    switch (pkt.type()) {
    case ROCE:
        break;
    default:
        abort();
    }

    RocePacket *p = (RocePacket*)(&pkt);
    RocePacket::seq_t seqno = p->seqno();
    if (_log_me) {
        cout << "Sink " << get_id() << " recv'd " << seqno << endl;
    }
    simtime_picosec ts = p->ts();
    //bool last_packet = ((RocePacket*)&pkt)->last_packet();

    if (seqno > _cumulative_ack+1){
        send_nack(ts,_cumulative_ack);  
        pkt.flow().logTraffic(pkt,*this,TrafficLogger::PKT_RCVDESTROY);

        p->free();

        //cout << "Wrong seqno received at Roce SINK " << seqno << " expecting " << _cumulative_ack << endl;
        return;
    }

    int size = p->size()-RocePacket::ACKSIZE; 

    if (seqno == _cumulative_ack+1) { // it's the next expected seq no
        _cumulative_ack = seqno + size - 1;
    } else if (seqno < _cumulative_ack+1) {
        //must have been a bad retransmit
    }
    // AstraSim U3 — echo ECN_CE as ECN_ECHO on the ACK.
    bool ecn_marked_here = (pkt.flags() & ECN_CE) != 0;
    send_ack(ts, ecn_marked_here);
    // AstraSim hook — fire receive-finished callback exactly once.
    if (astrasim_flow_finish_recv_cb && _cumulative_ack + 1 >= _src->_flow_size
        && !_astrasim_recv_finished) {
        _astrasim_recv_finished = true;
        int tag = _src->flow_id();
        int src_id = _debug_srcid;
        int dst_id = _debug_dstid;
        astrasim_flow_finish_recv_cb(src_id, dst_id, _src->_flow_size, tag);
    }
    // have we seen everything yet?
    pkt.flow().logTraffic(pkt,*this,TrafficLogger::PKT_RCVDESTROY);
    pkt.free();
}

void RoceSink::send_ack(simtime_picosec ts, bool ecn_echo) {
    RoceAck *ack = 0;
    ack = RoceAck::newpkt(_src->_flow, *_route, _cumulative_ack,_srcaddr);
    if (_log_me)
        cout << "Sink " << get_id() << " sending ack " << _cumulative_ack << endl;
    ack->set_pathid(0);
    // AstraSim U3 — carry ECN_ECHO if the data packet arrived with ECN_CE.
    if (ecn_echo) {
        ack->set_flags(ack->flags() | ECN_ECHO);
    }
    ack->sendOn();
}

void RoceSink::send_nack(simtime_picosec ts, RocePacket::seq_t ackno) {
    RoceNack *nack = NULL;
    nack = RoceNack::newpkt(_src->_flow, *_route, ackno,_srcaddr);
    if (_log_me)
        cout << "Sink " << get_id() << " sending nack " << ackno << endl;

    nack->set_pathid(0);
    assert(nack);
    nack->flow().logTraffic(*nack,*this,TrafficLogger::PKT_CREATE);
    nack->set_ts(ts);
    nack->sendOn();
}




