#include "panic_snapshot.h"
#if !defined(BROOK_SSD_GUARD_CHECK)
#include "ahci_system_volume.h"
#include "panic_journal.h"
#include "vfs.h"
#include "kprintf.h"
namespace brook {
namespace {
uint8_t packet[PanicJournalPayloadMax]{};
uint8_t previousPacket[PanicJournalPayloadMax]{};
GuardPanicSnapshot snapshot{};
GuardPanicSnapshot previousSnapshot{};
bool ready=false;
bool previousReady=false;
bool attempted=false;
}
void PanicNormalSnapshotLoad() {
    if(attempted)return;
    attempted=true;
    // This is a read-only, boot-time snapshot; it does not pin sectors or
    // authorize the panic writer. The ESP must already be the admitted mount.
    Vnode* node=VfsOpen("/media/system/PANIC.BIN",VFS_O_STREAM);
    VnodeStat stat{};
    if(!node)return;
    if(!AhciSystemVolumeOwnsMember(node) || VfsStat(node,&stat) || stat.isDir || stat.isSymlink ||
       stat.size!=uint64_t(PanicJournalSectors)*PanicJournalSector) {VfsRelease(node);return;}
    struct Reader {
        Vnode* node;
        bool read(uint32_t sector,uint8_t* out) {
            if(sector>=PanicJournalSectors)return false;
            uint64_t offset=uint64_t(sector)*PanicJournalSector;
            return VfsRead(node,out,PanicJournalSector,&offset)==int(PanicJournalSector);
        }
    } reader{node};
    PanicJournalRecord record{};
    const auto result=PanicJournalReadLatest(reader,packet,sizeof(packet),record);
    PanicJournalRecord previous{};
    const auto previousResult=result==PanicJournalResult::Ok ?
        PanicJournalReadRanked(reader,previousPacket,sizeof(previousPacket),previous,1) : result;
    VfsRelease(node);
    if(result!=PanicJournalResult::Ok)return;
    snapshot={packet,record.sequence,record.bytes,record.checksum};
    __atomic_store_n(&ready,true,__ATOMIC_RELEASE);
    if(previousResult==PanicJournalResult::Ok && previous.sequence<record.sequence) {
        previousSnapshot={previousPacket,previous.sequence,previous.bytes,previous.checksum};
        __atomic_store_n(&previousReady,true,__ATOMIC_RELEASE);
    }
    KPrintf("NORMAL_PANIC_PRIOR sequence=%lu bytes=%u crc=%x\n",record.sequence,record.bytes,record.checksum);
}
bool GuardRecoveryPanicSnapshot(GuardPanicSnapshot& out) {
    out={};if(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))return false;
    out=snapshot;return true;
}
bool GuardRecoveryPanicPreviousSnapshot(GuardPanicSnapshot& out) {
    out={};if(!__atomic_load_n(&previousReady,__ATOMIC_ACQUIRE))return false;
    out=previousSnapshot;return true;
}
}
#endif
