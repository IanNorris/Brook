#pragma once
#include "large_ext2_geometry.h"
#include "writable_desktop_plan.h"

namespace brook {
// Bounded session over an already identity-selected partition. The caller
// owns the backing device exclusively and supplies independent scratch. This
// class does not establish device identity, mount, or filesystem consistency.
struct LargeDataSession {
    bool dirty=false,armed=false,clean=false,dirtyAttempted=false,cleanAttempted=false;
    int Begin(const WritableDesktopIo& io,uint64_t bytes,const uint8_t uuid[16],
              uint8_t* gdt,uint32_t capacity) {
        if(dirty || armed || dirtyAttempted || !io.read || !io.write || !io.flush || !gdt)return -22;
        uint8_t sb[1024]{};
        int r=io.read(io.context,1024,sb,sizeof(sb));if(r!=int(sizeof(sb)))return r<0?r:-5;
        LargeExt2Geometry geometry{};
        const auto state=LargeExt2ClassifyHeader(sb,sizeof(sb),bytes,uuid,geometry);
        if(state==LargeExt2HeaderState::Invalid)return -22;
        if(state==LargeExt2HeaderState::NeedsCheck)return -117;
        if(geometry.descriptorBytes>capacity || bytes<4096 || geometry.descriptorBytes>bytes-4096)return -22;
        r=io.read(io.context,4096,gdt,geometry.descriptorBytes);
        if(r!=int(geometry.descriptorBytes))return r<0?r:-5;
        const uint32_t ipg=LargeExt2Read32(sb+40);
        for(uint32_t group=0;group<geometry.groups;++group)
            if(!LargeExt2GroupDescriptorValid(geometry,group,ipg,gdt+group*32,32))return -22;
        const uint8_t stateBytes[2]={0,0};dirtyAttempted=true;
        r=io.write(io.context,1082,stateBytes,2);if(r!=2)return r<0?r:-5;
        r=io.flush(io.context);if(r)return r<0?r:-5;
        // A successful write and flush are not enough to authorize a later
        // reset: confirm the loader will see this same filesystem as dirty.
        r=io.read(io.context,1024,sb,sizeof(sb));if(r!=int(sizeof(sb)))return r<0?r:-5;
        LargeExt2Geometry verified{};
        if(LargeExt2ClassifyHeader(sb,sizeof(sb),bytes,uuid,verified)!=LargeExt2HeaderState::NeedsCheck ||
           LargeExt2Read16(sb+58)!=0)return -5;
        dirty=true;return 0;
    }
    bool Arm(){if(!dirty || clean)return false;armed=true;return true;}
    int Finish(const WritableDesktopIo& io,bool finalSyncOwner) {
        if(!armed)return 0;
        if(!finalSyncOwner)return -1;
        if(clean)return 0;
        if(cleanAttempted)return -5; // uncertain prior write/flush is not replayed
        const uint8_t stateBytes[2]={1,0};cleanAttempted=true;
        const int r=io.write(io.context,1082,stateBytes,2);if(r!=2)return r<0?r:-5;
        const int f=io.flush(io.context);if(f)return f<0?f:-5;
        clean=true;return 0;
    }
};
}
