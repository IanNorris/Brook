#include <cassert>
#include <cstdarg>
#include <cstring>
#include <vector>
#include "../../kernel/src/panic_snapshot.cpp"

namespace brook {
namespace {
Vnode file{};bool present=true,owned=true,isDirectory=false;uint64_t size=PanicJournalSectors*PanicJournalSector;
unsigned reads=0,releases=0,failRead=0;
std::vector<uint8_t> disk(PanicJournalSectors*PanicJournalSector);
struct Writer {
    bool read(uint32_t sector,uint8_t* out) {std::memcpy(out,disk.data()+sector*PanicJournalSector,PanicJournalSector);return true;}
    bool write(uint32_t sector,const uint8_t* in) {std::memcpy(disk.data()+sector*PanicJournalSector,in,PanicJournalSector);return true;}
    bool flush() {return true;}
};
void reset() {attempted=false;ready=false;previousReady=false;snapshot={};previousSnapshot={};present=true;owned=true;isDirectory=false;size=disk.size();reads=releases=failRead=0;}
}
extern "C" Vnode* VfsOpen(const char* path,int flags) {assert(!std::strcmp(path,"/media/system/PANIC.BIN") && flags==VFS_O_STREAM);return present?&file:nullptr;}
void VfsRelease(Vnode* node) {assert(node==&file);++releases;}
bool AhciSystemVolumeOwnsMember(Vnode* node) {assert(node==&file);return owned;}
int VfsStat(Vnode* node,VnodeStat* st) {assert(node==&file);*st={};st->size=size;st->isDir=isDirectory;return 0;}
extern "C" int VfsRead(Vnode* node,void* out,uint64_t len,uint64_t* offset) {
    assert(node==&file && len==PanicJournalSector && *offset+len<=disk.size());
    if(++reads==failRead)return -5;
    std::memcpy(out,disk.data()+*offset,len);*offset+=len;return int(len);
}
extern "C" void KPrintf(const char*,...) {}
}
int main() {
    using namespace brook;
    Writer writer;const uint8_t contents[]={'p','a','n','i','c'};
    assert(PanicJournalWrite(writer,contents,sizeof(contents))==PanicJournalResult::Ok);
    GuardPanicSnapshot out{};
    reset();owned=false;PanicNormalSnapshotLoad();assert(!GuardRecoveryPanicSnapshot(out) && reads==0 && releases==1);
    reset();isDirectory=true;PanicNormalSnapshotLoad();assert(!GuardRecoveryPanicSnapshot(out) && reads==0);
    reset();size--;PanicNormalSnapshotLoad();assert(!GuardRecoveryPanicSnapshot(out) && reads==0);
    reset();failRead=2;PanicNormalSnapshotLoad();assert(!GuardRecoveryPanicSnapshot(out) && reads==2);
    reset();disk[PanicJournalSector]^=1;PanicNormalSnapshotLoad();assert(!GuardRecoveryPanicSnapshot(out));disk[PanicJournalSector]^=1;
    reset();PanicNormalSnapshotLoad();assert(GuardRecoveryPanicSnapshot(out) && out.sequence==1 && out.bytes==sizeof(contents));
    assert(!std::memcmp(out.data,contents,sizeof(contents)) && out.crc==PanicJournalCrc(contents,sizeof(contents)));
    const unsigned before=reads;disk[PanicJournalSector]^=1;PanicNormalSnapshotLoad();
    assert(reads==before && GuardRecoveryPanicSnapshot(out) && !std::memcmp(out.data,contents,sizeof(contents)));
    disk[PanicJournalSector]^=1;
    const uint8_t newer[]={'n','e','w'};
    assert(PanicJournalWrite(writer,newer,sizeof(newer))==PanicJournalResult::Ok);
    reset();PanicNormalSnapshotLoad();
    assert(GuardRecoveryPanicSnapshot(out) && out.sequence==2 && !std::memcmp(out.data,newer,sizeof(newer)));
    assert(GuardRecoveryPanicPreviousSnapshot(out) && out.sequence==1 && !std::memcmp(out.data,contents,sizeof(contents)));
}
