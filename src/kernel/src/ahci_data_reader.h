#pragma once
#include "ahci_data_inspection.h"

namespace brook {
// Exact-read adapter: returns the number of bytes read or a negative error.
// The caller owns the disk and must hold layout/content stable throughout the
// operation. Scratch buffers must be disjoint, at least their stated sizes,
// and must not overlap selectors or output. No write, mount or disk identity
// admission is performed here.
struct AhciDataReadIo {
    void* context;
    int (*read)(void*,uint64_t,void*,uint32_t);
};
inline AhciDataInspectionState AhciReadAndInspectBootData(
        const AhciDataReadIo& io,const AhciBootSelector& boot,const AhciDataSelector& data,
        uint64_t sectors,uint8_t* entries,uint8_t* backupEntries,uint32_t entryCapacity,
        uint8_t* gdt,uint32_t gdtCapacity,AhciDataInspection& out) {
    out={};
    if(!io.read || !entries || !backupEntries || !gdt || sectors<68 || sectors>~uint64_t{0}/512)
        return AhciDataInspectionState::Invalid;
    uint8_t mbr[512]{},primary[512]{},backup[512]{};
    if(io.read(io.context,0,mbr,512)!=512 || io.read(io.context,512,primary,512)!=512 ||
       io.read(io.context,(sectors-1)*512,backup,512)!=512)return AhciDataInspectionState::Invalid;
    StrictGptHeader a{},b{};
    if(StrictGptProtectiveMbr(mbr,sectors)!=StrictMbrResult::Protective ||
       StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok ||
       a.count!=b.count || a.entrySize!=b.entrySize)return AhciDataInspectionState::Invalid;
    const uint32_t entryBytes=a.count*a.entrySize; // bounded by the header check
    if(entryBytes>entryCapacity || io.read(io.context,a.entriesLba*512,entries,entryBytes)!=int(entryBytes) ||
       io.read(io.context,b.entriesLba*512,backupEntries,entryBytes)!=int(entryBytes))
        return AhciDataInspectionState::Invalid;
    AhciBootSelection esp{},part{};
    if(!AhciSelectBootAndData(boot,data,sectors,mbr,primary,backup,entries,backupEntries,entryBytes,esp,part) ||
       part.count<10)return AhciDataInspectionState::Invalid;
    const uint64_t start=part.first*512, partitionBytes=part.count*512;
    uint8_t sb[1024]{};
    if(io.read(io.context,start+1024,sb,sizeof(sb))!=int(sizeof(sb)))return AhciDataInspectionState::Invalid;
    LargeExt2Geometry geometry{};
    const auto header=LargeExt2ClassifyHeader(sb,sizeof(sb),partitionBytes,data.filesystemUuid,geometry);
    if(header==LargeExt2HeaderState::Invalid)return AhciDataInspectionState::Invalid;
    // Dirty media is an offline-check candidate; don't read or trust its GDT.
    if(header==LargeExt2HeaderState::NeedsCheck)
        return AhciInspectBootAndData(boot,data,sectors,mbr,primary,backup,entries,backupEntries,entryBytes,
                                      sb,sizeof(sb),nullptr,0,out);
    if(geometry.descriptorBytes>gdtCapacity || geometry.descriptorBytes>partitionBytes-4096 ||
       io.read(io.context,start+4096,gdt,geometry.descriptorBytes)!=int(geometry.descriptorBytes))
        return AhciDataInspectionState::Invalid;
    return AhciInspectBootAndData(boot,data,sectors,mbr,primary,backup,entries,backupEntries,entryBytes,
                                  sb,sizeof(sb),gdt,geometry.descriptorBytes,out);
}
}
