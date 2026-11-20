#pragma once
#include <stdint.h>
#include <stddef.h>

namespace brook {
// Native-endian Linux headers; IPv4 values contain network-order bytes.
struct InterfaceView {
    uint32_t index, flags, mtu, address, netmask;
    uint16_t hardwareType;
    uint8_t mac[6];
    char name[16];
};
struct RouteMessage { uint32_t length;uint16_t type,flags;uint32_t sequence,port; };
static_assert(sizeof(RouteMessage)==16);
struct RouteAddress { uint16_t family,pad;uint32_t port,groups; };
static_assert(sizeof(RouteAddress)==12);

// Pure wire builder, also compiled into the host protocol test. No allocation,
// user access, locks, or kernel-specific headers. Failure publishes no bytes.
class RouteReply {
    uint8_t* bytes;uint32_t capacity,used=0;
    bool failed=false;
    static uint32_t align(uint32_t n){return (n+3)&~3u;}
    void append(const void* source,uint32_t n) {
        if(n>capacity-used){failed=true;return;}
        const auto* p=static_cast<const uint8_t*>(source);
        for(uint32_t i=0;i<n;++i)bytes[used++]=p[i];
    }
    void zeros(uint32_t n){uint32_t z=0;while(n){uint32_t part=n<4?n:4;append(&z,part);n-=part;}}
    void attr(uint16_t type,const void* p,uint16_t n) {
        uint16_t header[2]={static_cast<uint16_t>(n+4),type};append(header,4);append(p,n);zeros(align(n)-n);
    }
    void finish(uint32_t start) {
        if(failed)return;
        uint32_t n=used-start;
        for(unsigned i=0;i<4;++i)bytes[start+i]=reinterpret_cast<uint8_t*>(&n)[i];
    }
    static unsigned nameLength(const char* name){unsigned n=0;while(n<15 && name[n])++n;return n+1;}
    static uint32_t word(const uint8_t* input) {
        uint32_t result=0;
        auto* out=reinterpret_cast<uint8_t*>(&result);
        for(unsigned i=0;i<4;++i)out[i]=input[i];
        return result;
    }
    static int setupLoopback(const RouteMessage& request,const uint8_t* payload,
                             uint32_t length,bool permitted) {
        if(!permitted)return -95;
        if(request.type==16) {
            // IFF_UP only, for the fixed namespace-local lo interface.
            if(request.flags!=5 || length!=16 || word(payload)!=0 ||
               word(payload+4)!=1 || word(payload+8)!=1 || word(payload+12)!=1)return -22;
            return 0;
        }
        // Brook creates a private namespace with 127/8 already attached to
        // lo. Confirm that exact address setup; reject other changes.
        if((request.flags&~uint16_t{0x605}) || !(request.flags&4) ||
           length!=24 || payload[0]!=2 || payload[1]!=8 ||
           payload[2]!=128 || payload[3]!=254 || word(payload+4)!=1)return -22;
        unsigned seen=0;
        for(unsigned i=8;i<24;i+=8) {
            if(payload[i]!=8 || payload[i+1] || payload[i+3] ||
               (payload[i+2]!=1 && payload[i+2]!=2))return -22;
            const unsigned bit=1u<<payload[i+2];
            if((seen&bit) || payload[i+4]!=127 || payload[i+5] ||
               payload[i+6] || payload[i+7]!=1)return -22;
            seen|=bit;
        }
        return 0;
    }

public:
    RouteReply(void* p,uint32_t n):bytes(static_cast<uint8_t*>(p)),capacity(n){}
    uint32_t size()const{return failed?0:used;}
    // Complete framing must validate before any socket queue publication.
    int build(const void* input,uint32_t length,uint32_t port,const InterfaceView* interfaces,uint32_t count,bool privateSetup=false) {
        const auto* in=static_cast<const uint8_t*>(input);
        for(uint32_t pos=0;pos<length;) {
            if(length-pos<sizeof(RouteMessage))return -22;
            RouteMessage request{};
            auto* copy=reinterpret_cast<uint8_t*>(&request);
            for(unsigned i=0;i<sizeof(request);++i)copy[i]=in[pos+i];
            if(request.length<sizeof(request) || request.length>length-pos)return -22;
            if(!(request.flags&1))return -22; // NLM_F_REQUEST
            int error=0;
            const bool setup=request.type==16 || request.type==20; // RTM_NEWLINK/NEWADDR
            if(setup)error=setupLoopback(request,in+pos+sizeof(request),request.length-sizeof(request),privateSetup);
            else if(request.type!=18 && request.type!=22)error=-95; // RTM_GETLINK/GETADDR only
            else if((request.flags&0x300)!=0x300)error=-95; // dump, not mutation or individual lookup
            else if(request.length<sizeof(request)+1)error=-22;
            else if(in[pos+16]!=0 && in[pos+16]!=2)error=-97;
            if(error || setup) {
                RouteMessage header{36,2,0x100,request.sequence,port}; // NLMSG_ERROR, capped
                append(&header,16);append(&error,4);append(&request,16);
            } else {
                for(uint32_t i=0;i<count;++i) {
                    const auto& v=interfaces[i];
                    if(request.type==22 && !v.address)continue;
                    const uint32_t start=used;
                    RouteMessage header{0,static_cast<uint16_t>(request.type==18?16:20),2,request.sequence,port};
                    append(&header,16);
                    if(request.type==18) {
                        struct Link {uint8_t family,pad;uint16_t type;int32_t index;uint32_t flags,change;};
                        const Link link{0,0,v.hardwareType,static_cast<int32_t>(v.index),v.flags,0};
                        append(&link,sizeof(link));attr(3,v.name,nameLength(v.name));
                        attr(1,v.mac,6);attr(4,&v.mtu,4);
                        if(v.flags&2){uint8_t broadcast[6]={255,255,255,255,255,255};attr(2,broadcast,6);}
                    } else {
                        uint8_t prefix=0;uint32_t mask=v.netmask;
                        // Masks are stored as bytes in network order, not a
                        // host-order integer. Do not count low-order bit runs.
                        for(unsigned octet=0;octet<4;++octet)
                            for(int bit=7;bit>=0;--bit)if((mask>>(octet*8))&(1u<<bit))++prefix;
                        struct Address {uint8_t family,prefix,flags,scope;uint32_t index;};
                        const Address address{2,prefix,0,static_cast<uint8_t>(v.hardwareType==772?254:0),v.index};
                        append(&address,sizeof(address));attr(1,&v.address,4);attr(2,&v.address,4);
                        attr(3,v.name,nameLength(v.name));
                        if(v.flags&2){uint32_t broadcast=v.address|~v.netmask;attr(4,&broadcast,4);}
                    }
                    finish(start);
                }
                RouteMessage done{20,3,2,request.sequence,port};int32_t status=0;
                append(&done,16);append(&status,4);
            }
            if(failed)return -105; // ENOBUFS, no truncated/missing DONE success
            const uint32_t remaining=length-pos;
            if(request.length==remaining)pos=length;
            else {const uint32_t step=align(request.length);if(step>remaining)return -22;pos+=step;}
        }
        return 0;
    }
};
}
