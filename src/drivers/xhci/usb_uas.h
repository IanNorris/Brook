#pragma once
#include <stdint.h>

namespace brook {
static constexpr unsigned UsbUasDepth=4;
enum UsbUasPipe : unsigned { UasCommand,UasStatus,UasDataIn,UasDataOut,UasPipeCount };
struct UsbUasEndpoint { uint8_t address,burst;uint16_t packet; };
struct UsbUasConfig {
    uint8_t interface,alternate;
    UsbUasEndpoint pipes[UasPipeCount];
};

inline bool UsbUasParse(const uint8_t* data,uint32_t size,int botInterface,UsbUasConfig& output) {
    if(!data || size<9 || data[1]!=2 || data[0]<9 || data[0]>size)return false;
    UsbUasConfig current{};
    bool candidate=false,endpoint=false,companion=false;
    uint8_t address=0,burst=0,streams=0,seen=0;
    uint16_t packet=0;
    const auto commit=[&] {
        if(candidate && seen==15) {output=current;return true;}
        return false;
    };
    for(uint32_t i=data[0];i<size;) {
        if(size-i<2 || data[i]<2 || data[i]>size-i)return false;
        const auto* d=data+i;
        if(d[1]==4) {
            if(commit())return true;
            candidate=d[0]>=9 && d[4]==4 && d[5]==8 && d[6]==6 && d[7]==0x62 &&
                (botInterface<0 || d[2]==botInterface);
            current={};
            if(candidate) { current.interface=d[2];current.alternate=d[3]; }
            endpoint=false;
            seen=0;
        } else if(candidate && d[1]==5) {
            endpoint=d[0]>=7 && (d[2]&15) && !(d[2]&0x70) && d[3]==2;
            companion=false;
            if(!endpoint) return false;
            address=d[2];
            packet=uint16_t(d[4]) | uint16_t(d[5])<<8;
            if(packet!=1024)return false;
        } else if(candidate && endpoint && d[1]==0x30) {
            if(d[0]<6 || d[2]>15 || (d[3]&0xe0) || companion)return false;
            burst=d[2];
            streams=d[3]&31;
            companion=true;
        } else if(candidate && endpoint && d[1]==0x24) {
            if(!companion || d[0]!=4 || !d[2] || d[2]>4 || d[3])return false;
            const unsigned pipe=d[2]-1;
            const bool in=pipe==UasStatus || pipe==UasDataIn;
            if(bool(address&0x80)!=in || (seen&(1u<<pipe)) ||
               (pipe!=UasCommand && (streams<2 || streams>16)))return false;
            for(const auto& other:current.pipes)if(other.address==address)return false;
            seen|=1u<<pipe;
            current.pipes[pipe]={address,burst,packet};
            endpoint=false;
        }
        i+=d[0];
    }
    return commit();
}

inline bool UsbUasStatus(const uint8_t* data,uint32_t bytes,uint16_t tag,
    uint8_t& status,uint32_t& senseBytes)
{
    if(bytes<16 || data[0]!=3 || data[1] || (uint16_t(data[2])<<8 | data[3])!=tag)return false;
    status=data[6];
    senseBytes=uint16_t(data[14])<<8 | data[15];
    return senseBytes<=bytes-16;
}
enum class UsbUasProgress { Waiting, Retire, Complete, Timeout };
struct UsbUasFlightState {
    uint8_t awaitingStatus, awaitingData, rejected=0;
    void Status(unsigned tag,uint8_t status) {
        const uint8_t bit=1u<<tag;
        awaitingStatus&=~bit;
        if(status)rejected|=bit;
    }
    void Data(unsigned tag) { awaitingData&=~(1u<<tag); }
    UsbUasProgress Progress(bool expired) const {
        if(!awaitingStatus && !(awaitingData&~rejected))
            return awaitingData?UsbUasProgress::Retire:UsbUasProgress::Complete;
        return expired?UsbUasProgress::Timeout:UsbUasProgress::Waiting;
    }
};
template<class Poll,class Expired,class Retire> bool UsbUasWaitFlight(
    UsbUasFlightState& flight,Poll poll,Expired expired,Retire retire)
{
    for(;;) {
        if(!poll())return false;
        switch(flight.Progress(expired())) {
        case UsbUasProgress::Complete:return true;
        case UsbUasProgress::Retire:return retire(flight.awaitingData);
        case UsbUasProgress::Timeout:return false;
        case UsbUasProgress::Waiting:break;
        }
    }
}
inline bool UsbUasRetryRead(uint8_t status,bool current,uint8_t key) {
    return status==0x08 || status==0x28 || (status==0x02 && current && key==0x0b);
}

template<class Drop,class Select> bool UsbUasRestoreBot(bool configureCompleted,bool bot,Drop drop,Select select) {
    return configureCompleted && drop() && bot && select();
}

}
