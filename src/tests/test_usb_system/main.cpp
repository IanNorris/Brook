#include "usb_system_layout.h"
#include "usb_write_windows.h"

#include <cassert>
#include <cstring>

using namespace brook;

namespace {
constexpr char good[]=
    "BRKUSB1\n532233445566778899aabbccddeeff00\n"
    "2752512,268435456\nc1002233445566778899aabbccddeeff\n";

void Store64(uint8_t* out,uint64_t value) {
    for(unsigned i=0;i<8;++i)out[i]=value>>(i*8);
}

void Entry(uint8_t* entry,const uint8_t (&type)[16],const uint8_t (&guid)[16],
           uint64_t first,uint64_t sectors,const char* name) {
    std::memcpy(entry,type,16);
    std::memcpy(entry+16,guid,16);
    Store64(entry+32,first);Store64(entry+40,first+sectors-1);
    for(unsigned i=0;name[i];++i)entry[56+i*2]=name[i];
}

void CheckLayout(const UsbSystemRecord& record) {
    const uint8_t bootType[16]={1},bootGuid[16]={2},wrongBoot[16]={3},backupGuid[16]={4};
    const uint8_t linuxType[]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    constexpr uint64_t first=2048;
    const uint64_t sectors=record.first-first;
    uint8_t entries[3*128]{};
    Entry(entries,bootType,bootGuid,first,sectors,"BROOKBOOT");
    Entry(entries+128,linuxType,record.guid,record.first,record.sectors,"BROOK_SYSTEM");
    Entry(entries+256,bootType,backupGuid,record.first+record.sectors,2048,"Backup");
    StrictGptHeader header{};
    header.count=3;header.entrySize=128;header.firstUsable=34;header.lastUsable=3907029134;
    auto validCopies=[&] {
        header.entriesCrc=StrictCrc32(entries,sizeof(entries));
        return StrictGptCheckCopies(header,entries,header,entries,sizeof(entries));
    };
    auto matches=[&](const uint8_t* guid,uint32_t index=1) {
        return UsbSystemEntriesMatch(header,entries,index,guid,first,sectors,record);
    };
    assert(validCopies()==StrictGptResult::Ok && matches(bootGuid));
    assert(!matches(wrongBoot)); // Same p2 on a different loader volume.
    assert(!matches(bootGuid,2));
    entries[128+56]='X';
    assert(!matches(bootGuid));
    entries[128+56]='B';
    entries[128+16]^=1;
    assert(!matches(bootGuid));
    entries[128+16]^=1;
    Store64(entries+128+40,record.first+record.sectors);
    assert(!matches(bootGuid));
    assert(validCopies()==StrictGptResult::OverlappingPartitions); // p3
    Store64(entries+128+40,record.first+record.sectors-1);
    Store64(entries+40,record.first);
    assert(validCopies()==StrictGptResult::OverlappingPartitions); // p1
}

void CheckWindows() {
    UsbWriteWindows windows;
    constexpr uint64_t bytes=16384;
    assert(!windows.Allows(512,1));
    assert(!windows.Set(true,{512,513},bytes,512));
    assert(!windows.Set(true,{bytes-512,1024},bytes,512));
    assert(windows.Set(true,{4096,8192},bytes,512));
    assert(!windows.Set(false,{3584,1024},bytes,512));
    assert(windows.Set(false,{512,3584},bytes,512));
    assert(!windows.Set(false,{12288,512},bytes,512));
    assert(windows.Allows(1024,1) && windows.Allows(1082+4096,2));
    assert(!windows.Allows(4095,2) && !windows.Allows(12287,2) && !windows.Allows(12288,1));
    assert(windows.Set(false,{},bytes,512));
    assert(!windows.Allows(1024,1) && windows.Allows(4096,512));
    assert(windows.Set(true,{},bytes,512));
    assert(!windows.Allows(4096,512));
}
}

int main() {
    UsbSystemRecord record{};
    assert(UsbSystemParse(good,sizeof(good)-1,record));
    assert(record.first==2752512 && record.sectors==268435456);
    assert(record.guid[0]==0x53 && record.uuid[0]==0xc1);
    for(unsigned i=0;i<sizeof(good)-1;++i)assert(!UsbSystemParse(good,i,record));
    assert(!UsbSystemParse(good,sizeof(good),record));
    char bad[sizeof(good)];
    std::memcpy(bad,good,sizeof(bad));
    bad[8]='G';
    assert(!UsbSystemParse(bad,sizeof(bad)-1,record));
    std::memcpy(bad,good,sizeof(bad));
    bad[55]='7'; // Invalid sector alignment.
    assert(!UsbSystemParse(bad,sizeof(bad)-1,record));
    CheckLayout(record);
    CheckWindows();
}
