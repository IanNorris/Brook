#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../kernel/src/tmpfs.cpp"
#include "../../kernel/src/tmpfs_mount_options.h"
#include "memory/physical_memory.h"
namespace
{
std::mutex tracking;
std::unordered_set<void *> allocations;
std::unordered_map<brook::KMutex *, std::unique_ptr<std::mutex>> locks;
thread_local long failAllocation = -1;
thread_local brook::Process process{};
uint64_t rtcSeconds = 1792707600;
bool mappingTest = false,failFrame = false;
std::unordered_map<uint64_t,unsigned> frames;
} // namespace
extern "C" [[noreturn]] void KernelPanic(const char *, ...)
{
    std::abort();
}
namespace brook
{
extern "C" void *kmalloc(uint64_t bytes)
{
    if (failAllocation == 0)
    {
        failAllocation = -1;
        return nullptr;
    }
    if (failAllocation > 0)
        --failAllocation;
    auto *p = std::malloc(bytes);
    if (p)
    {
        std::lock_guard<std::mutex> g(tracking);
        assert(allocations.insert(p).second);
    }
    return p;
}
extern "C" void kfree(void *p)
{
    if (p)
    {
        std::lock_guard<std::mutex> g(tracking);
        assert(allocations.erase(p) == 1);
        std::free(p);
    }
}
void KMutexInit(KMutex *m)
{
    std::lock_guard<std::mutex> g(tracking);
    locks[m] = std::make_unique<std::mutex>();
}
void KMutexLock(KMutex *m)
{
    std::mutex *p;
    {
        std::lock_guard<std::mutex> g(tracking);
        p = locks.at(m).get();
    }
    p->lock();
}
void KMutexUnlock(KMutex *m)
{
    std::mutex *p;
    {
        std::lock_guard<std::mutex> g(tracking);
        p = locks.at(m).get();
    }
    p->unlock();
}
Process *ProcessCurrent()
{
    return &process;
}
CredentialIds ProcessReadIds(Process *p, bool group)
{
    return group ? CredentialIds{p->gid, p->egid, p->sgid} : CredentialIds{p->uid, p->euid, p->suid};
}
SupplementaryGroups *ProcessGroupsAcquire(Process *)
{
    return nullptr;
}
void ProcessGroupsRelease(SupplementaryGroups *)
{
}
FileCredentialAccess ProcessFileAccess(Process *p, uint32_t uid, uint32_t gid, uint16_t mode, bool real)
{
    uint32_t u = real ? p->uid : p->euid, g = real ? p->gid : p->egid;
    bool root = u == 0 && !p->userNamespace;
    return {root, u == uid, g == gid,
            uint8_t(root       ? 7
                    : u == uid ? (mode >> 6) & 7
                    : g == gid ? (mode >> 3) & 7
                               : mode & 7)};
}
bool ProcessCapable(Process *p, UserNamespace *ns, unsigned cap)
{
    return UserNamespaceCapable({p->userNamespace, p->euid, p->egid, p->capabilities.effective}, ns, cap);
}
FileActorSnapshot ProcessFileActorAcquire(Process *p, UserNamespace *owner, bool real)
{
    FileActorSnapshot out{};
    out.uid = real ? p->uid : p->euid;
    out.gid = real ? p->gid : p->egid;
    out.initialNamespace = !p->userNamespace;
    if (p->userNamespace)
    {
        if (p->userNamespace->users.written)
            out.mappedUid = p->userNamespace->users.canonical;
        if (p->userNamespace->groups.written)
            out.mappedGid = p->userNamespace->groups.canonical;
    }
    const unsigned caps[] = {CapChown, CapDacOverride, CapFowner, CapFsetid};
    for (auto c : caps)
        if (ProcessCapable(p, owner, c))
            out.capabilities |= uint64_t{1} << c;
    if (real)
    {
        if (!out.initialNamespace || out.uid != 0)
            out.capabilities = 0;
        else
            out.capabilities |= uint64_t{1} << CapDacOverride;
    }
    return out;
}
uint64_t RtcNow()
{
    return rtcSeconds;
}
PhysicalAddress PmmAllocPage(MemTag, uint16_t)
{
    assert(mappingTest && "tmpfs ordinary I/O must not allocate canonical pages");
    if(failFrame)return {};
    void* memory=nullptr;assert(!posix_memalign(&memory,4096,4096));
    auto address=reinterpret_cast<uint64_t>(memory);
    assert(frames.emplace(address,1).second);return PhysicalAddress(address);
}
void PmmRefPage(PhysicalAddress page)
{
    assert(frames.at(page.raw()));++frames.at(page.raw());
}
void PmmUnrefPage(PhysicalAddress page)
{
    assert(frames.at(page.raw()));
    if(!--frames.at(page.raw())) {frames.erase(page.raw());std::free(reinterpret_cast<void*>(page.raw()));}
}
extern "C" void VfsClose(Vnode *vn)
{
    auto *object = vn->backendObject;
    vn->ops->close(vn);
    kfree(vn);
    VfsObjectRelease(object);
}
} // namespace brook
using namespace brook;
static void identity(unsigned uid)
{
    process.uid = process.euid = process.suid = uid;
    process.gid = process.egid = process.sgid = 100;
}
static Tmpfs *make()
{
    int e = 0;
    auto *fs = TmpfsCreate(1000, 100, nullptr, 6, &e);
    assert(fs && !e);
    return fs;
}
static VfsObject *root(Tmpfs *fs)
{
    VfsObject *o = nullptr;
    assert(!TmpfsOperations()->root_object(fs, 0, &o));
    return o;
}
static VfsObject *child(VfsObject *p, const char *name)
{
    VfsObject *o = nullptr;
    assert(!p->ops->lookup(p, name, &o, false));
    return o;
}
static Vnode *create(VfsObject *p, const char *name)
{
    int e = 0;
    auto *v = p->ops->create(p, name, VFS_O_WRITE | VFS_O_READWRITE, 0644, &e);
    assert(v && !e);
    return v;
}
static void clean()
{
    auto s = TmpfsGlobalUsage();
    assert(!s.bytes && !s.inodes && !s.mounts && allocations.empty() && frames.empty());
}
static void mappedQuota()
{
    mappingTest=true;
    for(int fail=-1;fail<3;++fail) {
        auto* fs=make();auto* r=root(fs);auto* f=create(r,"sparse");auto* object=f->ops->file_object(f);
        assert(!FileObjectTruncateUnmapped(object,f,8192));
        failAllocation=fail;failFrame=fail==2;
        int error=0;auto page=FileObjectGetPage(object,f,0,true,&error);
        failAllocation=-1;failFrame=false;
        if(fail>=0)assert(!page && (error==-12 || error==-28));
        else {
            assert(page && !error && frames.size()==1);
            auto charged=fs->bytes;
            auto alias=FileObjectGetPage(object,f,0,true,&error);assert(alias==page && fs->bytes==charged);
            PmmUnrefPage(alias);PmmUnrefPage(page);
            assert(!FileObjectTruncateUnmapped(object,f,0) && frames.empty());
        }
        assert(!r->ops->remove(r,"sparse",false));VfsClose(f);VfsObjectRelease(r);assert(!TmpfsDestroy(fs));clean();
    }
    auto* fs=make();auto* r=root(fs);auto* f=create(r,"sparse");auto* object=f->ops->file_object(f);
    assert(!FileObjectTruncateUnmapped(object,f,8192));
    int error=0;auto page=FileObjectGetPage(object,f,0,true,&error);assert(page && !error);
    assert(FindPage(VnodeNode(f),0)); // Persistent writeback storage exists before exposure.
    KMutexLock(&object->gate);assert(FileObjectExposeWritableLocked(object,f,0,page));KMutexUnlock(&object->gate);
    reinterpret_cast<unsigned char*>(page.raw())[0]=0x5a;
    auto spare=TmpfsDefaultBytes-fs->bytes;assert(Reserve(fs,spare));
    auto before=fs->bytes;
    auto denied=FileObjectGetPage(object,f,1,true,&error);assert(!denied && error==-28 && fs->bytes==before);
    assert(!FileObjectWriteback(object,f,0,4096)); // No ENOSPC despite zero spare quota.
    char value=0;uint64_t offset=0;assert(f->ops->read(f,&value,1,&offset)==1 && value==0x5a);
    assert(!r->ops->remove(r,"sparse",false));PmmUnrefPage(page);
    assert(f->refCount==2);--f->refCount; // Close user description; queue is final vnode owner.
    // Snapshot allocation itself is outside tmpfs quota. Draining must reclaim
    // the inode even when the filesystem has no spare capacity.
    assert(!FileObjectWritebackAll() && frames.empty());
    Unreserve(fs,spare);VfsObjectRelease(r);assert(!TmpfsDestroy(fs));clean();
    mappingTest=false;
}
static void indexedSparsePages()
{
    auto* fs=make();auto* r=root(fs);auto* f=create(r,"indexed");
    constexpr unsigned count=320;
    auto pageIndex=[](unsigned i)->uint64_t {return i<256?i:(uint64_t{1}<<32)+uint64_t{i-256}*64;};
    auto value=[](unsigned i)->unsigned char {return static_cast<unsigned char>(i*17+3);};
    for(unsigned ordinal=0;ordinal<count;++ordinal) {
        unsigned i=(ordinal*73)%count;
        // Fail only the optional initial/replacement table allocation, after
        // the backing page allocation. Existing data and write success remain.
        if(ordinal==31 || ordinal==159)failAllocation=1;
        auto byte=value(i);uint64_t offset=pageIndex(i)*4096+17;
        assert(f->ops->write(f,&byte,1,&offset)==1);
        if(ordinal==31 || ordinal==159)assert(failAllocation==-1);
    }
    for(unsigned i=0;i<count;++i) {
        unsigned char byte=0;uint64_t offset=pageIndex(i)*4096+17;
        assert(f->ops->read(f,&byte,1,&offset)==1 && byte==value(i));
    }
    // Drop sparse high indices and many collision-chain members, keeping an
    // indexed file and a partially retained final page.
    unsigned char byte=0x7f;uint64_t offset=128*4096+200;
    assert(f->ops->write(f,&byte,1,&offset)==1);
    assert(!f->ops->truncate(f,128*4096+123));
    assert(!f->ops->truncate(f,256*4096));
    for(unsigned i=0;i<256;++i) {
        offset=uint64_t{i}*4096+17;byte=0xff;
        assert(f->ops->read(f,&byte,1,&offset)==1 && byte==(i<=128?value(i):0));
    }
    offset=128*4096+200;byte=0xff;assert(f->ops->read(f,&byte,1,&offset)==1 && !byte);
    // Crossing below the index threshold preserves survivors, and regrowth
    // must not resurrect freed buckets/pages or the discarded EOF tail.
    assert(!f->ops->truncate(f,16*4096));
    assert(!f->ops->truncate(f,256*4096));
    for(unsigned i=0;i<256;++i) {
        offset=uint64_t{i}*4096+17;byte=0xff;
        assert(f->ops->read(f,&byte,1,&offset)==1 && byte==(i<16?value(i):0));
        byte=value(i);offset=uint64_t{i}*4096+17;assert(f->ops->write(f,&byte,1,&offset)==1);
    }
    assert(!f->ops->truncate(f,0));
    VfsClose(f);VfsObjectRelease(r);assert(!TmpfsDestroy(fs));clean();
}
static void indexedMappedQuota()
{
    mappingTest=true;
    auto* fs=make();auto* r=root(fs);auto* f=create(r,"indexed-mapped");auto* node=VnodeNode(f);
    assert(!f->ops->truncate(f,32*4096));
    for(unsigned i=0;i<31;++i)assert(!PrepareBackingPage(node,i));
    // Leave exactly enough for backing page 32 and the first bucket table,
    // but no canonical-frame quota. Metadata is honestly charged; this fault
    // must fail without publishing a canonical page or losing the backend.
    const uint64_t backend=sizeof(Page)+sizeof(Allocation)+64*sizeof(Page*)+sizeof(Allocation);
    const uint64_t spare=fs->byteLimit-fs->bytes-backend;assert(Reserve(fs,spare));
    int error=0;auto* object=f->ops->file_object(f);auto page=FileObjectGetPage(object,f,31,true,&error);
    assert(!page && error==-28 && fs->bytes==fs->byteLimit && frames.empty());
    unsigned char byte=0x5a;uint64_t offset=31*4096+17;
    assert(f->ops->write(f,&byte,1,&offset)==1); // Existing indexed page: no allocation.
    Unreserve(fs,spare);
    page=FileObjectGetPage(object,f,31,true,&error);assert(page && !error);
    assert(reinterpret_cast<unsigned char*>(page.raw())[17]==0x5a);PmmUnrefPage(page);
    assert(!FileObjectTruncateUnmapped(object,f,0) && frames.empty());
    VfsClose(f);VfsObjectRelease(r);assert(!TmpfsDestroy(fs));clean();mappingTest=false;
}
int main()
{
    identity(1000);
    clean();
    mappedQuota();
    indexedSparsePages();
    indexedMappedQuota();
    {
        auto* fs=make();auto* r=root(fs);int error=0;
        auto* fifo=r->ops->create_fifo(r,"fifo",0640,&error);assert(fifo && !error && fifo->type==VnodeType::Device);
        VnodeStat stat{};assert(!fifo->ops->stat(fifo,&stat) && (stat.mode&0177777)==0010640 && stat.uid==1000);
        assert(!r->ops->create_fifo(r,"fifo",0600,&error) && error==-17);
        auto* object=child(r,"fifo");assert(!r->ops->rename(r,"fifo",r,"renamed"));
        auto* again=object->ops->open(object,VFS_O_WRITE|VFS_O_READWRITE,&error);assert(again && !error);
        assert(!r->ops->remove(r,"renamed",false));assert(!fifo->ops->stat(fifo,&stat) && !stat.nlink);
        VfsClose(again);VfsObjectRelease(object);VfsClose(fifo);VfsObjectRelease(r);assert(!TmpfsDestroy(fs));clean();
    }
    // The boot budget scales without preallocating memory. Existing instances
    // pin the policy so accounting cannot shrink underneath live allocations.
    const uint64_t originalBudget=TmpfsMaximumBytes();
    assert(TmpfsConfigureByteBudget(0)==-22);
    assert(TmpfsConfigureByteBudget(originalBudget+1)==-22);
    assert(TmpfsMaximumBytes()==originalBudget);
    assert(!TmpfsConfigureByteBudget(1024ULL*1024*1024));
    {
        TmpfsMountOptions large{};
        assert(!ParseTmpfsMountOptions("size=512M",&large) && large.bytes==512ULL*1024*1024);
        int error=0;auto* custom=TmpfsCreate(1000,100,nullptr,6,&error,01777,large.bytes);
        assert(custom && !error && TmpfsConfigureByteBudget(originalBudget)==-16);
        auto* object=root(custom);auto* file=create(object,"budget");VfsSpace space{};
        assert(!Space(file,&space) && space.blocks==large.bytes/4096);
        VfsClose(file);VfsObjectRelease(object);assert(!TmpfsDestroy(custom));clean();
    }
    assert(!TmpfsConfigureByteBudget(originalBudget));
    {
        int error=0;auto* large=TmpfsCreate(1000,100,nullptr,6,&error,01777,originalBudget);
        auto* peer=make();assert(large && !error);
        auto* object=root(peer);auto* file=create(object,"headroom");
        const auto remaining=originalBudget-TmpfsGlobalUsage().bytes;
        assert(Reserve(large,remaining));
        VfsSpace space{};assert(!Space(file,&space) && !space.availableBlocks);
        Unreserve(large,remaining);
        assert(!Space(file,&space) && space.availableBlocks);
        VfsClose(file);VfsObjectRelease(object);
        assert(!TmpfsDestroy(peer) && !TmpfsDestroy(large));clean();
    }
    TmpfsMountOptions options{};
    assert(!ParseTmpfsMountOptions(nullptr,&options) && options.mode==01777);
    assert(!ParseTmpfsMountOptions("",&options) && options.mode==01777);
    assert(!ParseTmpfsMountOptions("mode=0700,mode=0755",&options) && options.mode==0755);
    for(unsigned mode=0;mode<=07777;++mode) {
        char text[32];std::snprintf(text,sizeof(text),"mode=%o",mode);
        assert(!ParseTmpfsMountOptions(text,&options) && options.mode==mode);
    }
    for(const char* text:{"mode=","mode=8","mode=-1","mode=+755","mode=0x755","mode=10000","mode=777777777777777777777777777777777",",mode=755","mode=755,","mode=755,,mode=700","uid=0","size=","size=0","size=-1","size=1%","size=257M","size=1G","size=18446744073709551616","size=1Mx","mod","mode=755x"}) {
        options.mode=0123;options.bytes=8192;
        assert(ParseTmpfsMountOptions(text,&options)==-22 && options.mode==0123 && options.bytes==8192);
    }
    for(auto item:{std::pair{"size=1",uint64_t{4096}}, {"size=4097",uint64_t{8192}},
                   {"size=64k",uint64_t{65536}}, {"mode=0700,size=128M",uint64_t{128}*1024*1024},
                   {"size=256M",TmpfsMaximumBytes()}, {"size=1M,size=8192",uint64_t{8192}}})
        assert(!ParseTmpfsMountOptions(item.first,&options) && options.bytes==item.second);
    for(uint64_t quota:{uint64_t{65536},uint64_t{128}*1024*1024,TmpfsMaximumBytes()}) {
        int error=0;auto* custom=TmpfsCreate(1000,100,nullptr,6,&error,01777,quota);assert(custom && !error);
        auto* object=root(custom);auto* file=create(object,"quota");VfsSpace space{};
        assert(!Space(file,&space) && space.blocks==quota/4096);
        auto remaining=quota-custom->bytes;assert(Reserve(custom,remaining));
        auto before=TmpfsGlobalUsage().bytes;
        assert(!Reserve(custom,1) && !Allocate(custom,1) && TmpfsGlobalUsage().bytes==before);
        assert(!Space(file,&space) && !space.availableBlocks);
        Unreserve(custom,remaining);VfsClose(file);VfsObjectRelease(object);assert(!TmpfsDestroy(custom));clean();
    }
    for(uint64_t quota:{uint64_t{0},uint64_t{4095},uint64_t{4097},TmpfsMaximumBytes()+4096}) {
        int error=0;assert(!TmpfsCreate(1000,100,nullptr,6,&error,01777,quota) && error==-22);clean();
    }
    for(uint16_t mode:{uint16_t(0),uint16_t(0755),uint16_t(01777),uint16_t(07777)}) {
        int error=0;auto* custom=TmpfsCreate(1000,100,nullptr,6,&error,mode);assert(custom && !error);
        auto* object=root(custom);VnodeStat stat{};assert(!object->ops->stat(object,&stat) && (stat.mode&07777)==mode && stat.uid==1000 && stat.gid==100);
        VfsObjectRelease(object);assert(!TmpfsDestroy(custom));clean();
    }
    int invalidModeError=0;assert(!TmpfsCreate(1000,100,nullptr,6,&invalidModeError,010000) && invalidModeError==-22);clean();
    auto *fs = make();
    auto *other = make();
    auto *r = root(fs);
    auto *r2 = root(other);
    VnodeStat a{}, b{};
    assert(!r->ops->stat(r, &a) && !r2->ops->stat(r2, &b) && a.dev != b.dev && a.mode == 0041777 && a.uid == 1000);
    auto *file = create(r, "file");
    uint64_t off = 8193;
    assert(FileObjectWrite(file->ops->file_object(file), file, "ABC", 3, &off) == 3);
    char buffer[8200];
    off = 0;
    assert(FileObjectRead(file->ops->file_object(file), file, buffer, sizeof(buffer), &off) == 8196);
    for (unsigned i = 0; i < 8193; ++i)
        assert(!buffer[i]);
    assert(!memcmp(buffer + 8193, "ABC", 3));
    assert(!FileObjectTruncateUnmapped(file->ops->file_object(file), file, 8194));
    assert(!FileObjectTruncateUnmapped(file->ops->file_object(file), file, 9000));
    off = 8193;
    memset(buffer, 0xff, 10);
    assert(FileObjectRead(file->ops->file_object(file), file, buffer, 10, &off) == 10 && buffer[0] == 'A');
    for (unsigned i = 1; i < 10; ++i)
        assert(!buffer[i]);
    assert(!file->ops->deny_mapping);
    VfsObject *missing = nullptr;
    assert(r2->ops->lookup(r2, "file", &missing, false) == -2 && !missing);
    assert(!r->ops->mkdir(r, "dir", 0755));
    auto *dir = child(r, "dir");
    assert(!r->ops->symlink(r, "file", "link"));
    auto *link = child(r, "link");
    assert(link->ops->readlink(link, buffer, sizeof(buffer)) == 4 && !memcmp(buffer, "file", 4));
    assert(!link->ops->object_path(link,buffer,sizeof(buffer)) && !strcmp(buffer,"/link"));
    assert(!r->ops->rename(r, "file", dir, "moved"));
    assert(!file->backendObject->ops->object_path(file->backendObject,buffer,sizeof(buffer)) && !strcmp(buffer,"/dir/moved"));
    VfsObject* entryParent=nullptr;char entryLeaf[256];
    assert(file->backendObject->ops->entry_parent(file->backendObject,&entryParent,entryLeaf,2)==-36 && !entryParent);
    assert(!file->backendObject->ops->entry_parent(file->backendObject,&entryParent,entryLeaf,sizeof(entryLeaf)));
    assert(entryParent && entryParent->identity==dir->identity && !strcmp(entryLeaf,"moved"));
    VfsObjectRelease(entryParent);entryParent=nullptr;
    assert(!dir->ops->remove(dir, "moved", false));
    assert(file->backendObject->ops->object_path(file->backendObject,buffer,sizeof(buffer))==-2);
    assert(file->backendObject->ops->entry_parent(file->backendObject,&entryParent,entryLeaf,sizeof(entryLeaf))==-2 && !entryParent);
    assert(!file->ops->stat(file, &a) && !a.nlink);
    off = 8193;
    assert(FileObjectRead(file->ops->file_object(file), file, buffer, 1, &off) == 1 && buffer[0] == 'A');
    auto heldBytes = TmpfsGlobalUsage().bytes;
    VfsClose(file);
    assert(TmpfsGlobalUsage().bytes < heldBytes);
    assert(!r->ops->mkdir(r, "inside", 0755));
    auto *inside = child(r, "inside");
    assert(!inside->ops->mkdir(inside, "sub", 0755));
    auto *sub = child(inside, "sub");
    assert(r->ops->rename(r, "inside", sub, "cycle") == -22);
    VfsObjectRelease(sub);
    VfsObjectRelease(inside);
    auto *first = create(r, "one");
    auto *victim = create(r, "two");
    assert(!r->ops->rename(r, "one", r, "two"));
    assert(!victim->ops->stat(victim, &a) && !a.nlink);
    VfsClose(victim);
    VfsClose(first);
    identity(1001);
    assert(r->ops->remove(r, "two", false) == -1);
    assert(dir->ops->mkdir(dir, "forbidden", 0755) == -13);
    assert(dir->ops->chmod(dir, 0700) == -1);
    identity(1000);
    assert(!dir->ops->chmod(dir, 0000));
    assert(dir->ops->lookup(dir, "x", &missing, false) == -13);
    assert(!dir->ops->chmod(dir, 0700));
    assert(!r->ops->remove(r, "dir", true));
    VfsObject *parent = nullptr;
    assert(!dir->ops->parent(dir, &parent, false));
    VfsObjectRelease(parent);
    assert(dir->ops->directory_path(dir, buffer, sizeof(buffer)) == -2);
    VfsObjectRelease(dir);
    VfsObjectRelease(link);
    VfsObjectRelease(r);
    VfsObjectRelease(r2);
    assert(!TmpfsDestroy(fs) && !TmpfsDestroy(other));
    clean();
    // Both OMIT selectors do not require ownership and do not alter ctime.
    fs = make();
    r = root(fs);
    file = create(r, "times");
    assert(!file->ops->stat(file, &a));
    ++rtcSeconds;
    identity(1001);
    VnodeTime omit[2] = {{0, VFS_UTIME_OMIT}, {0, VFS_UTIME_OMIT}};
    assert(!file->ops->set_times(file, omit));
    assert(!file->ops->stat(file, &b) && b.ctime.sec == a.ctime.sec && b.mtime.sec == a.mtime.sec);
    identity(1000);
    VfsClose(file);
    // SGID-directory symlinks inherit the directory GID, like files/dirs.
    identity(0);
    assert(!r->ops->chown(r, 1000, 200));
    assert(!r->ops->chmod(r, 02777));
    identity(1000);
    assert(!r->ops->symlink(r, "target", "sgid-link"));
    link = child(r, "sgid-link");
    assert(!link->ops->stat(link, &a) && a.gid == 200);
    VfsObjectRelease(link);
    VfsObjectRelease(r);
    assert(!TmpfsDestroy(fs));
    clean();
    // Scoped capabilities cannot override ownership outside the actor's map.
    auto *owner = static_cast<UserNamespace *>(kmalloc(sizeof(UserNamespace)));
    *owner = {};
    owner->owner = 1000;
    owner->users = {0, 1000, true};
    owner->groups = {0, 100, true};
    int scopedError = 0;
    fs = TmpfsCreate(1000, 100, owner, 6, &scopedError);
    assert(fs);
    r = root(fs);
    file = create(r, "unmapped");
    identity(0);
    assert(!file->ops->chown(file, 2000, 200));
    assert(!r->ops->chown(r, 3000, 300));
    process.userNamespace = owner;
    process.capabilities.effective = UserCapabilities;
    identity(1000);
    assert(file->ops->chmod(file, 0777) == -1);
    assert(file->ops->chown(file, 1000, 100) == -1);
    assert(r->ops->remove(r, "unmapped", false) == -1);
    VnodeTime explicitTimes[2] = {{5, 0}, {5, 0}};
    assert(file->ops->set_times(file, explicitTimes) == -1);
    process.userNamespace = nullptr;
    process.capabilities = {};
    identity(1000);
    VfsClose(file);
    VfsObjectRelease(r);
    assert(!TmpfsDestroy(fs));
    UserNamespaceRelease(owner);
    clean();
    // Every allocation boundary in preparation and each publishing operation.
    for (long fault = 0; fault < 8; ++fault)
    {
        auto before = TmpfsGlobalUsage();
        failAllocation = fault;
        int e = 0;
        auto *f = TmpfsCreate(1000, 100, nullptr, 6, &e);
        failAllocation = -1;
        if (f)
            assert(!TmpfsDestroy(f));
        else
            assert(e < 0);
        auto after = TmpfsGlobalUsage();
        assert(before.bytes == after.bytes && before.inodes == after.inodes && before.mounts == after.mounts);
        clean();
    }
    for (unsigned operation = 0; operation < 4; ++operation)
        for (long fault = 0; fault < 10; ++fault)
        {
            fs = make();
            r = root(fs);
            auto baseline = TmpfsGlobalUsage();
            failAllocation = fault;
            int e = 0, result = 0;
            Vnode *v = nullptr;
            if (operation == 0)
                v = r->ops->create(r, "x", VFS_O_WRITE, 0600, &e);
            else if (operation == 1)
                result = r->ops->mkdir(r, "x", 0700);
            else if (operation == 2)
                result = r->ops->symlink(r, "target", "x");
            else
            {
                VfsObject *temp = nullptr;
                result = r->ops->lookup(r, "none", &temp, false);
                assert(!temp);
            }
            failAllocation = -1;
            if (v)
                VfsClose(v);
            if ((operation == 0 && !v) || ((operation == 1 || operation == 2) && result < 0))
            {
                auto after = TmpfsGlobalUsage();
                assert(baseline.bytes == after.bytes && baseline.inodes == after.inodes);
            }
            VfsObjectRelease(r);
            assert(!TmpfsDestroy(fs));
            clean();
        }
    // Concurrent appends use the actual shared FileObject gates.
    fs = make();
    r = root(fs);
    file = create(r, "append");
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker)
        workers.emplace_back(
            [&, worker]
            {
                char text[8];
                memset(text, 'A' + worker, sizeof(text));
                for (unsigned i = 0; i < 500; ++i)
                {
                    uint64_t position = 0;
                    assert(FileObjectAppend(file->ops->file_object(file), file, text, sizeof(text), &position) == 8);
                }
            });
    for (auto &t : workers)
        t.join();
    assert(!file->ops->stat(file, &a) && a.size == 16000);
    std::vector<char> appended(a.size);
    off = 0;
    assert(FileObjectRead(file->ops->file_object(file), file, appended.data(), appended.size(), &off) == 16000);
    for (unsigned i = 0; i < appended.size(); i += 8)
        for (unsigned j = 1; j < 8; ++j)
            assert(appended[i] == appended[i + j]);
    VfsClose(file);
    VfsObjectRelease(r);
    assert(!TmpfsDestroy(fs));
    clean();
    // Depth bound and iterative teardown, including held removed directories.
    fs = make();
    r = root(fs);
    auto *current = r;
    VfsObjectRetain(current);
    for (unsigned i = 0; i < 255; ++i)
    {
        assert(!current->ops->mkdir(current, "x", 0700));
        auto *next = child(current, "x");
        VfsObjectRelease(current);
        current = next;
    }
    assert(current->ops->mkdir(current, "too-deep", 0700) == -40);
    VfsObjectRelease(current);
    VfsObjectRelease(r);
    assert(!TmpfsDestroy(fs));
    clean();
    // Per-instance inode quota and exact reclamation.
    fs = make();
    r = root(fs);
    for (unsigned i = 0; i < 1023; ++i)
    {
        char name[24];
        snprintf(name, sizeof(name), "f%u", i);
        auto *v = create(r, name);
        VfsClose(v);
    }
    int e = 0;
    assert(!r->ops->create(r, "full", VFS_O_WRITE, 0600, &e) && e == -28);
    VfsObjectRelease(r);
    assert(!TmpfsDestroy(fs));
    clean();
    // Per-instance sparse-page quota: retained unlinked data stays charged.
    fs = make();
    r = root(fs);
    file = create(r, "pages");
    char page[4096] = {};
    unsigned written = 0;
    for (;;)
    {
        off = uint64_t(written) * 4096;
        int n = FileObjectWrite(file->ops->file_object(file), file, page, sizeof(page), &off);
        if (n < 0)
        {
            assert(n == -28);
            break;
        }
        assert(n == 4096);
        ++written;
        assert(written < 4096);
    }
    assert(written > 4000);
    assert(!r->ops->remove(r, "pages", false));
    heldBytes = TmpfsGlobalUsage().bytes;
    VfsClose(file);
    assert(TmpfsGlobalUsage().bytes + uint64_t(written) * 4096 < heldBytes);
    VfsObjectRelease(r);
    assert(!TmpfsDestroy(fs));
    clean();
    // Heap failure after a committed page preserves exactly the completed prefix.
    for (long fault = 0; fault < 4; ++fault)
    {
        fs = make();
        r = root(fs);
        file = create(r, "partial");
        char twoPages[8192];
        memset(twoPages, 'P', sizeof(twoPages));
        off = 0;
        failAllocation = fault;
        int result = FileObjectWrite(file->ops->file_object(file), file, twoPages, sizeof(twoPages), &off);
        failAllocation = -1;
        assert(result == -28 || result == 4096 || result == 8192);
        assert(!file->ops->stat(file, &a));
        assert(a.size == (result > 0 ? uint64_t(result) : 0) && off == a.size);
        VfsClose(file);
        VfsObjectRelease(r);
        assert(!TmpfsDestroy(fs));
        clean();
    }
    // Aggregate byte quota is independent of every instance's own ceiling.
    std::vector<Tmpfs *> aggregate;
    for (unsigned i = 0; i < 32; ++i)
        aggregate.push_back(make());
    std::vector<std::pair<Tmpfs *, void *>> blocks;
    bool full = false;
    for (unsigned i = 0; i < 300; ++i)
    {
        auto *f = aggregate[i % aggregate.size()];
        auto *block = Allocate(f, 1024 * 1024);
        if (!block)
        {
            assert(f->bytes < TmpfsDefaultBytes - 1024 * 1024);
            full = true;
            break;
        }
        blocks.push_back({f, block});
    }
    assert(full && TmpfsGlobalUsage().bytes <= GlobalBytes);
    for (auto item : blocks)
        Free(item.first, item.second);
    for (auto *f : aggregate)
        assert(!TmpfsDestroy(f));
    clean();
    // Aggregate inode quota reached while the selected instance has free slots.
    aggregate.clear();
    for (unsigned i = 0; i < 17; ++i)
        aggregate.push_back(make());
    std::vector<Node *> prepared;
    full = false;
    for (unsigned i = 0; i < 17000; ++i)
    {
        auto *f = aggregate[i % aggregate.size()];
        auto *n = NewNode(f, Regular | 0600, 1000, 100);
        if (!n)
        {
            assert(f->inodes < InstanceInodes && TmpfsGlobalUsage().inodes == GlobalInodes);
            full = true;
            break;
        }
        prepared.push_back(n);
    }
    assert(full);
    for (auto *n : prepared)
        Drop(n);
    for (auto *f : aggregate)
        assert(!TmpfsDestroy(f));
    clean();
    // Global mount quota cannot be bypassed by separate user namespaces/instances.
    std::vector<Tmpfs *> mounts;
    for (unsigned i = 0; i < 128; ++i)
        mounts.push_back(make());
    e = 0;
    assert(!TmpfsCreate(1000, 100, nullptr, 6, &e) && e == -28);
    for (auto *f : mounts)
        assert(!TmpfsDestroy(f));
    clean();
    puts("TMPFS_BACKEND_PASS sparse/metadata/ownership/rename/lifetime/rollback/append/depth/quota");
}
