#pragma once
#include "net.h"

namespace brook {
// Validate a candidate reply for an already selected local address and
// outstanding next hop. The caller must separately validate interface and
// configuration generation. ARP is unauthenticated even after these checks.
static inline bool ArpReplyMatches(const void* frame,uint32_t length,
                                   const MacAddr& localMac,uint32_t localIp,
                                   uint32_t nextHop,uint64_t* packedMac)
{
    if(!frame || !packedMac || !localIp || !nextHop || localIp==nextHop ||
       length<sizeof(EthHeader)+sizeof(ArpPacket))return false;
    const auto* bytes=static_cast<const uint8_t*>(frame);
    const auto* eth=reinterpret_cast<const EthHeader*>(bytes);
    const auto* arp=reinterpret_cast<const ArpPacket*>(bytes+sizeof(EthHeader));
    if(ntohs(eth->etherType)!=ETH_TYPE_ARP || ntohs(arp->htype)!=1 ||
       ntohs(arp->ptype)!=ETH_TYPE_IPV4 || arp->hlen!=6 || arp->plen!=4 ||
       ntohs(arp->oper)!=ARP_OP_REPLY || arp->spa!=nextHop || arp->tpa!=localIp ||
       (arp->sha.b[0]&1))return false;
    uint64_t mac=0;
    for(unsigned i=0;i<6;++i){
        if(eth->dst.b[i]!=localMac.b[i] || arp->tha.b[i]!=localMac.b[i] ||
           eth->src.b[i]!=arp->sha.b[i])return false;
        mac|=static_cast<uint64_t>(arp->sha.b[i])<<(8*i);
    }
    if(!mac)return false;
    *packedMac=mac;return true;
}
}
