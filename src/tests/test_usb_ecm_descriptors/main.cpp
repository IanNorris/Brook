#include "../../drivers/xhci/usb_ecm_descriptors.h"
#include <cassert>
#include <vector>
using namespace brook;
std::vector<uint8_t> config(bool ss){
    std::vector<uint8_t> b={9,2,0,0,2,2,0,0x80,0,
        9,4,0,0,1,2,6,0,0,5,0x24,6,0,1,
        13,0x24,15,3,0,0,0,0,0xea,5,0,0,0,
        7,5,0x83,3,16,0,8,
        9,4,1,0,0,10,0,0,0,9,4,1,1,2,10,0,0,0};
    for(auto address:{0x81,2}){
        const unsigned packet=ss?1024:64;
        const uint8_t ep[]={7,5,(uint8_t)address,2,(uint8_t)packet,(uint8_t)(packet>>8),0};b.insert(b.end(),ep,ep+7);
        if(ss){const uint8_t companion[]={6,48,3,0,0,0};b.insert(b.end(),companion,companion+6);}
    }
    b[2]=b.size();b[3]=b.size()>>8;return b;
}
int main(){
    for(bool ss:{false,true}){
        auto b=config(ss);UsbEcmConfiguration c{};
        assert(UsbEcmParse(b.data(),b.size(),ss,c));
        assert(c.value==2 && c.control==0 && c.data==1 && c.alternate==1 && c.macString==3);
        assert(c.in==0x81 && c.out==2 && c.segment==1514 && c.inPacket==(ss?1024:64));
        assert(c.inBurst==(ss?3:0) && c.outBurst==(ss?3:0));
        for(unsigned n=0;n<b.size();++n)assert(!UsbEcmParse(b.data(),n,ss,c));
        auto bad=b;bad[26]=0;assert(!UsbEcmParse(bad.data(),bad.size(),ss,c)); // missing MAC string
        bad=b;bad[25]=2;assert(!UsbEcmParse(bad.data(),bad.size(),ss,c)); // not an Ethernet descriptor
        bad=b;bad[23]=255;assert(!UsbEcmParse(bad.data(),bad.size(),ss,c)); // extends past tree
        bad=b;bad[9]=0;assert(!UsbEcmParse(bad.data(),bad.size(),ss,c));
        if(ss){
            bad=b;bad.back()=1;assert(!UsbEcmParse(bad.data(),bad.size(),ss,c));
            for(unsigned packet:{0u,512u,1023u}){
                bad=b;
                for(unsigned at=9;at<bad.size();at+=bad[at])if(bad[at+1]==5 && (bad[at+3]&3)==2){
                    bad[at+4]=packet&255;bad[at+5]=packet>>8;
                }
                assert(!UsbEcmParse(bad.data(),bad.size(),true,c));
            }
        }
    }
    uint8_t descriptor[26]={26,3},mac[6]{};const char* text="0011aAbBcC22";
    for(unsigned i=0;i<12;++i)descriptor[2+i*2]=text[i];
    assert(UsbEcmMac(descriptor,26,mac) && mac[2]==0xaa && mac[5]==0x22);
    descriptor[4]='1';assert(!UsbEcmMac(descriptor,26,mac));
    descriptor[4]='0';descriptor[3]=1;assert(!UsbEcmMac(descriptor,26,mac));
    assert(!UsbEcmMac(descriptor,25,mac));
}
