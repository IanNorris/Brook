#pragma once
#include "ahci_data_selector.h"
#include "large_ext2_geometry.h"

namespace brook {
enum class AhciDataInspectionState { Invalid, NeedsCheck, Clean };
struct AhciDataInspection {
    AhciBootSelection esp{}, data{};
    LargeExt2Geometry geometry{};
};

// Pure, read-only inspection of caller-owned stable buffers. Caller separately
// verifies ATA identity and sector geometry, reads sb and gdt from offsets 1024
// and 4096 of the selected partition, and pins the layout and stable contents through any
// eventual admission. All pointers must reference buffers of the stated size.
// This reports format eligibility, not bitmap consistency, a mount, or
// permission to write.
// Output must not overlap selectors or any supplied buffer.
inline AhciDataInspectionState AhciInspectBootAndData(
        const AhciBootSelector& boot,const AhciDataSelector& data,uint64_t diskSectors,
        const uint8_t* mbr,const uint8_t* primary,const uint8_t* backup,
        const uint8_t* entries,const uint8_t* backupEntries,uint32_t entryBytes,
        const uint8_t* sb,uint32_t sbBytes,const uint8_t* gdt,uint32_t gdtBytes,
        AhciDataInspection& out) {
    out={};AhciDataInspection next{};
    if(!AhciSelectBootAndData(boot,data,diskSectors,mbr,primary,backup,entries,
                              backupEntries,entryBytes,next.esp,next.data))return AhciDataInspectionState::Invalid;
    const auto state=LargeExt2ClassifyHeader(sb,sbBytes,next.data.count*512,
                                             data.filesystemUuid,next.geometry);
    if(state==LargeExt2HeaderState::Invalid)return AhciDataInspectionState::Invalid;
    // A dirty header is eligible only for offline checking; descriptors have
    // not been inspected and no clean/writable admission is implied.
    if(state==LargeExt2HeaderState::NeedsCheck){out=next;return AhciDataInspectionState::NeedsCheck;}
    if(!gdt || gdtBytes!=next.geometry.descriptorBytes)return AhciDataInspectionState::Invalid;
    const uint32_t ipg=LargeExt2Read32(sb+40);
    for(uint32_t group=0;group<next.geometry.groups;++group)
        if(!LargeExt2GroupDescriptorValid(next.geometry,group,ipg,gdt+group*32,32))
            return AhciDataInspectionState::Invalid;
    out=next;return AhciDataInspectionState::Clean;
}
}
