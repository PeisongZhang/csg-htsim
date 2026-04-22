// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include "hpccpacket.h"

PacketDB<HPCCPacket> HPCCPacket::_packetdb;
PacketDB<HPCCAck> HPCCAck::_packetdb;
PacketDB<HPCCNack> HPCCNack::_packetdb;

void HPCCAck::copy_int_info(IntEntry* info, int cnt){
    // AstraSim (P4): _int_info is a std::vector now — assign grows as needed.
    _int_info.assign(info, info + cnt);
    _int_hop = cnt;
};
