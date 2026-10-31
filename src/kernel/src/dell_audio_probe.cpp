#include "dell_audio_probe.h"
#include "acpi.h"
#include "memory/heap.h"
#include "vfs.h"

namespace brook {
namespace {
bool ReadReviewedHash(uint8_t hash[32]) {
    auto* node=VfsOpen("/etc/dell-audio-dsdt.sha256",VFS_O_STREAM);if(!node)return false;
    VnodeStat st{};char record[65]{};uint64_t offset=0;
    bool ok=!VfsStat(node,&st) && !st.isDir && !st.isSymlink && (st.size==64 || st.size==65) &&
        VfsRead(node,record,st.size,&offset)==int(st.size) && (st.size==64 || record[64]=='\n');
    VfsRelease(node);if(!ok)return false;
    for(uint32_t i=0;i<64;++i) {
        const char c=record[i];const unsigned v=c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):16;
        if(v>15)return false;
        if(i&1)hash[i/2]|=uint8_t(v);else hash[i/2]=uint8_t(v<<4);
    }
    return true;
}
struct NvsIo {
    bool containsNvs(uint64_t address,uint32_t bytes){return AcpiNvsContains(address,bytes);}
    bool readNvs(uint64_t address,void* out,uint32_t bytes){return AcpiNvsRead(address,out,bytes);}
};
}
bool DellAudioProbeRead(DellAudioFirmwareSnapshot& out,const char*& reason) {
    reason="not provisioned or invalid digest";uint8_t hash[32]{};
    if(!ReadReviewedHash(hash))return false;
    AcpiSdtHeader table{};reason="DSDT unavailable";
    if(!AcpiInventoryRead(0,0,nullptr,0,table) || table.length<36 || table.length>1024*1024)return false;
    auto* data=static_cast<uint8_t*>(kmalloc(table.length));reason="allocation failed";if(!data)return false;
    bool ok=true;reason="DSDT read failed";
    for(uint32_t offset=0;offset<table.length;) {
        const uint32_t n=table.length-offset>512?512:table.length-offset;AcpiSdtHeader current{};
        if(!AcpiInventoryRead(0,offset,data+offset,n,current) || current.length!=table.length){ok=false;break;}
        offset+=n;
    }
    DellAudioFirmwareResult result=DellAudioFirmwareResult::Invalid;
    if(ok){NvsIo io;result=DellAudioFirmwareCapture(data,table.length,hash,io,out);}
    kfree(data);
    if(!ok)return false;
    switch(result) {
    case DellAudioFirmwareResult::Ok:reason="ok";return true;
    case DellAudioFirmwareResult::HashMismatch:reason="DSDT digest mismatch";break;
    case DellAudioFirmwareResult::Layout:reason="unrecognized AML layout";break;
    case DellAudioFirmwareResult::Nvs:reason="NVS unavailable or outside ACPI NVS";break;
    case DellAudioFirmwareResult::Unstable:reason="firmware values changed during capture";break;
    default:reason="invalid DSDT";break;
    }
    return false;
}
}
