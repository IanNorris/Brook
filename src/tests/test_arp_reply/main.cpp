#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../kernel/src/arp_reply_validation.h"
using namespace brook;

int main() {
    struct __attribute__((packed)) Frame { EthHeader eth; ArpPacket arp; } good{};
    const MacAddr local={{2,3,4,5,6,7}},remote={{8,9,10,11,12,13}};
    const uint32_t localIp=htonl(0x0a000201),remoteIp=htonl(0x0a000202);
    good.eth.dst=local;good.eth.src=remote;good.eth.etherType=htons(ETH_TYPE_ARP);
    good.arp.htype=htons(1);good.arp.ptype=htons(ETH_TYPE_IPV4);good.arp.hlen=6;good.arp.plen=4;
    good.arp.oper=htons(ARP_OP_REPLY);good.arp.sha=remote;good.arp.spa=remoteIp;
    good.arp.tha=local;good.arp.tpa=localIp;
    uint64_t mac=0;
    auto valid=[&](const Frame& f){return ArpReplyMatches(&f,sizeof(f),local,localIp,remoteIp,&mac);};
    assert(valid(good) && mac==0x0d0c0b0a0908ULL);
    for(uint32_t size=0;size<sizeof(good);++size){
        mac=123;assert(!ArpReplyMatches(&good,size,local,localIp,remoteIp,&mac) && mac==123);
    }
    uint8_t misaligned[sizeof(good)+2]{};memcpy(misaligned+1,&good,sizeof(good));
    assert(ArpReplyMatches(misaligned+1,sizeof(good)+1,local,localIp,remoteIp,&mac));
    assert(!ArpReplyMatches(nullptr,sizeof(good),local,localIp,remoteIp,&mac));
    assert(!ArpReplyMatches(&good,sizeof(good),local,localIp,remoteIp,nullptr));
    Frame bad=good;bad.arp.oper=htons(ARP_OP_REQUEST);assert(!valid(bad));
    bad=good;bad.arp.htype=htons(2);assert(!valid(bad));
    bad=good;bad.arp.ptype=htons(ETH_TYPE_ARP);assert(!valid(bad));
    bad=good;bad.arp.hlen=5;assert(!valid(bad));
    bad=good;bad.arp.plen=5;assert(!valid(bad));
    bad=good;bad.eth.etherType=htons(ETH_TYPE_IPV4);assert(!valid(bad));
    bad=good;bad.arp.spa=localIp;assert(!valid(bad));
    bad=good;bad.arp.tpa=remoteIp;assert(!valid(bad));
    bad=good;bad.arp.tha.b[0]^=2;assert(!valid(bad));
    bad=good;bad.eth.dst.b[0]^=2;assert(!valid(bad));
    bad=good;bad.eth.src.b[1]^=2;assert(!valid(bad));
    bad=good;bad.arp.sha.b[0]|=1;bad.eth.src=bad.arp.sha;assert(!valid(bad));
    bad=good;memset(&bad.arp.sha,0,sizeof(MacAddr));bad.eth.src=bad.arp.sha;assert(!valid(bad));
    assert(!ArpReplyMatches(&good,sizeof(good),local,localIp,localIp,&mac));
    assert(!ArpReplyMatches(&good,sizeof(good),local,0,remoteIp,&mac));
    assert(!ArpReplyMatches(&good,sizeof(good),local,localIp,0,&mac));
    std::puts("ARP_REPLY_VALIDATION_PASS");
}
