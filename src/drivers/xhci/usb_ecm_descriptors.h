#pragma once
#include <stdint.h>
namespace brook {
struct UsbEcmConfiguration {
    uint8_t value=0,control=0,data=0,alternate=0,macString=0;
    uint8_t in=0,out=0,inBurst=0,outBurst=0;
    uint16_t inPacket=0,outPacket=0,segment=0;
    uint8_t notify=0,notifyInterval=0;uint16_t notifyPacket=0;
};
inline bool UsbEcmParse(const uint8_t* b,uint32_t bytes,bool superspeed,UsbEcmConfiguration& result){
    if(!b || bytes<9 || b[0]<9 || b[1]!=2 || (uint32_t(b[2])|(uint32_t(b[3])<<8))!=bytes || !b[5])return false;
    UsbEcmConfiguration c{};c.value=b[5];unsigned iface=256,klass=0,subclass=0;
    bool comm=false,united=false,ether=false;
    for(unsigned at=b[0];at<bytes;){
        unsigned len=b[at];if(len<2 || len>bytes-at)return false;
        if(b[at+1]==4){if(len<9)return false;iface=b[at+2];klass=b[at+5];subclass=b[at+6];
            if(klass==2 && subclass==6 && b[at+7]==0 && b[at+3]==0){if(comm)return false;comm=true;c.control=iface;}}
        else if(b[at+1]==0x24 && klass==2 && subclass==6 && iface==c.control && comm){
            if(len<3)return false;
            if(b[at+2]==6){if(len!=5 || b[at+3]!=c.control)return false;c.data=b[at+4];united=true;}
            if(b[at+2]==15){if(len<13)return false;c.macString=b[at+3];c.segment=uint16_t(b[at+8])|(uint16_t(b[at+9])<<8);ether=true;}
        }
        else if(b[at+1]==5 && klass==2 && subclass==6 && iface==c.control && comm){
            if(len<7 || c.notify || (b[at+3]&3)!=3 || !(b[at+2]&128) || !(b[at+2]&15))return false;
            c.notify=b[at+2];c.notifyPacket=uint16_t(b[at+4])|(uint16_t(b[at+5])<<8);c.notifyInterval=b[at+6];
            if(c.notifyPacket<8 || c.notifyPacket>64 || !c.notifyInterval || (superspeed && c.notifyInterval>16))return false;
        }
        at+=len;
    }
    if(!comm || !united || !ether || !c.macString || c.data==c.control || c.segment<1514 || c.segment>2048)return false;
    bool active=false,inComp=false,outComp=false;unsigned lastEp=0;
    for(unsigned at=b[0];at<bytes;){
        unsigned len=b[at];if(len<2 || len>bytes-at)return false;
        const unsigned type=b[at+1];
        if(type==4){
            if(len<9)return false;
            active=b[at+2]==c.data && b[at+3]!=0 && b[at+5]==10;
            if(active){if(c.alternate)return false;c.alternate=b[at+3];}
            lastEp=0;
        }else if(type==5 && active){
            if(len<7 || (b[at+3]&3)!=2 || !(b[at+2]&15) || (b[at+2]&0x70))return false;
            const unsigned packet=unsigned(b[at+4])|(unsigned(b[at+5])<<8);
            if(superspeed ? packet!=1024u : (packet!=64 && packet!=512))return false;
            lastEp=b[at+2];
            if(lastEp&128){if(c.in)return false;c.in=lastEp;c.inPacket=packet;}
            else{if(c.out)return false;c.out=lastEp;c.outPacket=packet;}
        }else if(type==48 && active && lastEp){
            if(len<6 || b[at+2]>15 || b[at+3] || b[at+4] || b[at+5])return false;
            if(lastEp&128){c.inBurst=b[at+2];inComp=true;}else{c.outBurst=b[at+2];outComp=true;}
            lastEp=0;
        }else lastEp=0;
        at+=len;
    }
    if(!c.notify || !c.in || !c.out || !c.alternate || (superspeed && (!inComp || !outComp)))return false;
    result=c;return true;
}
inline int UsbEcmHex(uint8_t x){return x>='0'&&x<='9'?x-'0':x>='a'&&x<='f'?x-'a'+10:x>='A'&&x<='F'?x-'A'+10:-1;}
inline bool UsbEcmMac(const uint8_t* b,unsigned bytes,uint8_t* mac){
    if(!b || !mac || bytes!=26 || b[0]!=26 || b[1]!=3)return false;
    unsigned any=0;
    for(unsigned i=0;i<6;++i){int a=UsbEcmHex(b[2+i*4]),c=UsbEcmHex(b[4+i*4]);
        if(a<0 || c<0 || b[3+i*4] || b[5+i*4])return false;
        mac[i]=(a<<4)|c;any|=mac[i];}
    return any && !(mac[0]&1);
}
}
