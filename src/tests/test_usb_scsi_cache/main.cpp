#include "../../drivers/xhci/usb_scsi_cache.h"
#include <cassert>
using namespace brook;
int main() {
    uint8_t fixed[18]{};fixed[0]=0x70;fixed[2]=5;fixed[7]=10;fixed[12]=0x20;
    auto sense=UsbScsiParseSense(fixed,18);assert(UsbScsiUnsupportedSync(sense));
    assert(UsbScsiWriteThroughFlush(UsbScsiCache::WriteThrough,sense));
    assert(!UsbScsiWriteThroughFlush(UsbScsiCache::Unknown,sense));
    assert(!UsbScsiWriteThroughFlush(UsbScsiCache::WriteBack,sense));
    for(unsigned n=0;n<14;n++)assert(!UsbScsiParseSense(fixed,n).valid);
    fixed[0]=0x71;assert(!UsbScsiUnsupportedSync(UsbScsiParseSense(fixed,18)));fixed[0]=0x70;
    fixed[2]=3;assert(!UsbScsiUnsupportedSync(UsbScsiParseSense(fixed,18)));fixed[2]=5;
    fixed[12]=0x24;assert(!UsbScsiUnsupportedSync(UsbScsiParseSense(fixed,18)));fixed[12]=0x20;
    fixed[13]=1;assert(!UsbScsiUnsupportedSync(UsbScsiParseSense(fixed,18)));fixed[13]=0;
    fixed[7]=5;assert(!UsbScsiParseSense(fixed,18).valid);
    uint8_t descriptor[8]={0x72,5,0x20,0,0,0,0,0};assert(UsbScsiUnsupportedSync(UsbScsiParseSense(descriptor,8)));
    descriptor[0]=0x73;assert(!UsbScsiUnsupportedSync(UsbScsiParseSense(descriptor,8)));
    uint8_t mode[32]{};mode[0]=23;mode[4]=8;mode[5]=18;
    assert(UsbScsiModeCache6(mode,24)==UsbScsiCache::WriteThrough);
    mode[6]=4;assert(UsbScsiModeCache6(mode,24)==UsbScsiCache::WriteBack);mode[6]=0;
    for(unsigned n=0;n<24;n++)assert(UsbScsiModeCache6(mode,n)==UsbScsiCache::Unknown);
    mode[3]=25;assert(UsbScsiModeCache6(mode,32)==UsbScsiCache::Unknown);mode[3]=0;
    mode[4]=0x48;assert(UsbScsiModeCache6(mode,32)==UsbScsiCache::Unknown);mode[4]=8;
    mode[5]=30;assert(UsbScsiModeCache6(mode,32)==UsbScsiCache::Unknown);mode[5]=18;
    mode[4]=7;assert(UsbScsiModeCache6(mode,24)==UsbScsiCache::Unknown);
    // Honor a block descriptor rather than reading a fixed WCE offset.
    uint8_t blocks[32]{};blocks[0]=31;blocks[3]=8;blocks[12]=8;blocks[13]=18;
    assert(UsbScsiModeCache6(blocks,32)==UsbScsiCache::WriteThrough);
    assert(UsbScsiShortInCompletion(true,true,true,18,13,4));
    assert(!UsbScsiShortInCompletion(false,true,true,18,13,4));
    assert(!UsbScsiShortInCompletion(true,false,true,18,13,4));
    assert(!UsbScsiShortInCompletion(true,true,false,18,13,4));
    assert(!UsbScsiShortInCompletion(true,true,true,18,13,19));
    assert(!UsbScsiShortInCompletion(true,true,true,18,1,1));
    assert(!UsbScsiShortInCompletion(true,true,true,18,6,0));
    assert(UsbScsiShortInCompletion(true,true,true,18,1,0));
}
