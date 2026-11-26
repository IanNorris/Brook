#include "gpt.h"
#include "usb_system_layout.h"
#include "usb_system_fsck_policy.h"
#include "usb_write_windows.h"

#include <cassert>
#include <cstring>
#include <cstdio>

using namespace brook;

namespace {
constexpr char good[]=
    "BRKUSB1\n532233445566778899aabbccddeeff00\n"
    "2752512,268435456\nc1002233445566778899aabbccddeeff\n";

void Store64(uint8_t* out,uint64_t value) {
    for(unsigned i=0;i<8;++i)out[i]=value>>(i*8);
}

void Store32(uint8_t* out,uint32_t value) {
    for(unsigned i=0;i<4;++i)out[i]=value>>(i*8);
}

struct ScanDisk {
    uint8_t header[512]{}, entry[128]{};
    int headerBytes=512, entryBytes=128;
};

void CheckBootScan() {
    static const BlockDeviceOps ops{
        .read=[](Device* d,uint64_t offset,void* buf,uint64_t len) {
            const auto& disk=*static_cast<ScanDisk*>(d->priv);
            if(offset==512 && len==sizeof(disk.header)) {
                if(disk.headerBytes>0)std::memcpy(buf,disk.header,disk.headerBytes);
                return disk.headerBytes;
            }
            assert(offset==1024 && len==sizeof(disk.entry));
            if(disk.entryBytes>0)std::memcpy(buf,disk.entry,disk.entryBytes);
            return disk.entryBytes;
        },
        .write=nullptr, .ioctl=nullptr, .close=nullptr,
        .block_count=[](Device*)->uint64_t {return 100000;},
        .block_size=[](Device*)->uint32_t {return 512;},
    };
    ScanDisk disk;
    Device device{reinterpret_cast<const DeviceOps*>(&ops),"usb0",DeviceType::Block,&disk};
    disk.headerBytes=-5;
    assert(GptProbeBootPartition(&device)==-5);
    disk.headerBytes=511;
    assert(GptProbeBootPartition(&device)==-5);
    disk.headerBytes=512;
    assert(GptProbeBootPartition(&device)==0); // A complete sector with no GPT signature.
    std::memcpy(disk.header,"EFI PART",8);
    assert(GptProbeBootPartition(&device)==-22);
    Store32(disk.header+8,0x10000);Store32(disk.header+12,92);
    Store64(disk.header+24,1);Store64(disk.header+32,99999);
    Store64(disk.header+40,34);Store64(disk.header+48,99966);
    disk.header[56]=1;Store64(disk.header+72,2);
    Store32(disk.header+80,128);Store32(disk.header+84,128);
    Store32(disk.header+16,StrictCrc32(disk.header,92,true));
    disk.entryBytes=-5;
    assert(GptProbeBootPartition(&device)==-5);
    disk.entryBytes=127;
    assert(GptProbeBootPartition(&device)==-5);
    disk.entryBytes=128;
    assert(GptProbeBootPartition(&device)==0); // Empty entry 1.
    disk.entry[0]=1;disk.entry[16]=2;
    Store64(disk.entry+32,2048);Store64(disk.entry+40,4095);
    assert(GptProbeBootPartition(&device)==1);
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
    assert(windows.CanOpenUpdate(false) && !windows.CanOpenUpdate(true));
    assert(!windows.Set(true,{512,513},bytes,512));
    assert(!windows.CanOpenUpdate(true));
    assert(!windows.Set(true,{bytes-512,1024},bytes,512));
    assert(windows.Set(true,{4096,8192},bytes,512));
    assert(windows.CanOpenUpdate(true));
    assert(!windows.Set(false,{3584,1024},bytes,512));
    assert(windows.Set(false,{512,3584},bytes,512));
    assert(!windows.Set(false,{12288,512},bytes,512));
    assert(windows.Allows(1024,1) && windows.Allows(1082+4096,2));
    assert(!windows.Allows(4095,2) && !windows.Allows(12287,2) && !windows.Allows(12288,1));
    assert(windows.Set(false,{},bytes,512));
    assert(!windows.Allows(1024,1) && windows.Allows(4096,512));
    assert(windows.Set(true,{},bytes,512));
    assert(!windows.Allows(4096,512));
    assert(windows.CanOpenUpdate(true));
    assert(!UsbWriteWindows{}.CanOpenUpdate(true)); // A different device remains ineligible.
}

void CheckRecordBounds() {
    constexpr char guid[]="532233445566778899aabbccddeeff00";
    constexpr char uuid[]="c1002233445566778899aabbccddeeff";
    constexpr char zero[]="00000000000000000000000000000000";
    char text[128];
    UsbSystemRecord parsed{};
    auto parse=[&](const char* a,const char* extent,const char* b) {
        const int n=std::snprintf(text,sizeof(text),"BRKUSB1\n%s\n%s\n%s\n",a,extent,b);
        assert(n>0 && static_cast<unsigned>(n)<sizeof(text));
        return UsbSystemParse(text,n,parsed);
    };
    assert(parse(guid,"4294967288,8",uuid)); // Last sector fits READ(10).
    assert(parsed.first==4294967288ull && parsed.sectors==8);
    assert(!parse(guid,"4294967296,8",uuid));
    assert(!parse(guid,"8,4294967296",uuid));
    assert(!parse(guid,"4294967289,8",uuid));
    assert(!parse(guid,"0,8",uuid));
    assert(!parse(guid,"8,0",uuid));
    assert(!parse(zero,"8,8",uuid));
    assert(!parse(guid,"8,8",zero));
    assert(!parse("532233445566778899Aabbccddeeff00","8,8",uuid));
    assert(!parse(guid,"8,8","C1002233445566778899aabbccddeeff"));
}
}

namespace brook {
extern "C" bool DeviceRegister(Device*) {return true;}
extern "C" bool DeviceIsRegistered(Device*) {return true;}
extern "C" int DeviceWrite(Device*,uint64_t,const void*,uint64_t) {return -30;}
extern "C" int DeviceFlush(Device*) {return 0;}
extern "C" void KPrintf(const char*,...) {}
extern "C" void SerialPrintf(const char*,...) {}
}

void CheckFsckPolicy() {
    using A=UsbSystemFsckAction;
    assert(UsbSystemFsckClassify(true,0)==A::Continue);
    assert(UsbSystemFsckClassify(true,1)==A::Continue);
    assert(UsbSystemFsckClassify(true,2)==A::Reboot);
    assert(UsbSystemFsckClassify(true,3)==A::Reboot);
    const int refused[]={4,5,8,12,16,32,128,255,-1};
    for(int code:refused)assert(UsbSystemFsckClassify(true,code)==A::Refuse);
    assert(UsbSystemFsckClassify(false,0)==A::Refuse);  // killed by a signal
    uint8_t digest[32];
    for(unsigned i=0;i<32;++i) {
        auto nibble=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
        digest[i]=uint8_t(nibble(UsbSystemFsckSha256[2*i])<<4|nibble(UsbSystemFsckSha256[2*i+1]));
    }
    assert(UsbSystemFsckDigestMatches(digest));
    for(unsigned i=0;i<32;++i) {
        digest[i]^=1;assert(!UsbSystemFsckDigestMatches(digest));digest[i]^=1;
    }
    UsbSystemFsckTranscript<8> transcript;
    transcript.Append("ab\x01\n",4);
    assert(transcript.length==4 && !std::strcmp(transcript.text,"ab?\n") && !transcript.truncated);
    transcript.Append("cdefg",5);
    assert(transcript.length==7 && transcript.truncated && !std::strcmp(transcript.text,"ab?\ncde"));
}

int main() {
    CheckBootScan();
    CheckFsckPolicy();
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
    CheckRecordBounds();
    CheckLayout(record);
    CheckWindows();
}
