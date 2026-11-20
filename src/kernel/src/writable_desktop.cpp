#include "writable_desktop.h"
#include "fat_image_device.h"
#include "ext2_vfs.h"
#include "device.h"
#include "vfs.h"
#include "vfs_path.h"
#include "mount_namespace.h"
#include "kprintf.h"
#include "string.h"
namespace brook {
namespace {
WritableDesktopInfo info;
WritableDesktopSession session;
Device* image=nullptr;
int finalError=0;
bool finalClean=false,finalUncertain=false,attempted=false;
bool fallbackAllowed=true;
bool directStore=false;
int Read(void* p,uint64_t off,void* data,uint32_t n){auto* d=static_cast<Device*>(p);return d->ops->read(d,off,data,n);}
int Write(void* p,uint64_t off,const void* data,uint32_t n){return DeviceWrite(static_cast<Device*>(p),off,data,n);}
int Flush(void* p){return DeviceFlush(static_cast<Device*>(p));}
WritableDesktopIo Io(){return {image,Read,Write,Flush};}
bool Record(const char* path,WritableDesktopPlan& plan) {
    auto* node=VfsOpen(path,VFS_O_STREAM);if(!node)return false;
    VnodeStat st{};char data[97]{};uint64_t offset=0;
    bool valid=!VfsStat(node,&st) && !st.isDir && !st.isSymlink && st.size && st.size<=96;
    if(valid)valid=VfsRead(node,data,st.size,&offset)==int(st.size) && WritableDesktopPlanParse(data,st.size,plan);
    VfsRelease(node);return valid;
}
bool PathPrefix(const char* path,const char* prefix) {
    while(*prefix){char c=*path++;if(c>='A'&&c<='Z')c+=32;if(c!=*prefix++)return false;}
    return !*path || *path=='/';
}
bool NamespaceClear() {
    auto* ns=VfsMountNamespaceInitial();bool ramData=false;
    for(uint32_t i=0;i<VfsMountNamespaceCount(ns);++i){
        const auto mount=VfsMountNamespaceSnapshotAt(ns,i);if(!mount.used)continue;
        if(PathPrefix(mount.mountPoint,"/nix") || PathPrefix(mount.mountPoint,"/data/home"))return false;
        if(PathPrefix(mount.mountPoint,"/data") && strlen(mount.mountPoint)==5 && !strcmp(mount.fsType,"tmpfs"))ramData=true;
    }
    return ramData;
}
int PrepareTarget() {
    VfsPath root{};int result=VfsPathRoot(&root);if(result)return result;
    VnodeStat st{};result=VfsStatAt(root,root,"/data/home",&st,false);
    if(result==-2)result=VfsMkdirAt(root,root,"/data/home",0755,0);
    else if(!result && (!st.isDir || st.isSymlink))result=-20;
    VfsPathRelease(root);return result;
}
struct ProfileView {
#if defined(BROOK_SSD_AUTO_RECOVERY)
    VfsPath root{};
    const void* attachment=nullptr;
#endif

    int Stat(const char* path,VnodeStat* out,bool follow=true) const {
#if defined(BROOK_SSD_AUTO_RECOVERY)
        if(attachment) {
            VfsPath object{};
            int error=VfsPathResolve(root,root,path,&object,follow);
            if(!error)error=object.attachment==attachment?VfsPathStat(object,out):-18;
            VfsPathRelease(object);
            return error;
        }
#endif
        return follow?VfsStatPath(path,out):VfsLstatPath(path,out);
    }
    int Readlink(const char* path,char* out,uint32_t capacity) const {
#if defined(BROOK_SSD_AUTO_RECOVERY)
        if(attachment) {
            VfsPath object{};
            int error=VfsPathResolve(root,root,path,&object,false);
            if(!error)error=object.attachment==attachment?VfsPathReadlink(object,out,capacity):-18;
            VfsPathRelease(object);
            return error;
        }
#endif
        return VfsReadlink(path,out,capacity);
    }
};

bool Directory(const ProfileView& view,const char* path,bool writable) {
    VnodeStat st{};
    return !view.Stat(path,&st,false) && st.isDir && !st.isSymlink &&
        (!writable || (st.uid==1000 && st.gid==1000 && (st.mode&0300)==0300));
}
// Signed profile staging publishes immutable generations through two fixed
// relative aliases. Only these exact spellings are admitted; anything else
// (an absolute target, a different generation root) stays in maintenance.
bool ManagedAlias(const ProfileView& view,const char* path,const char* expected) {
    VnodeStat st{};
    if(view.Stat(path,&st,false) || !st.isSymlink)return false;
    char target[64]{};
    const int length=view.Readlink(path,target,sizeof(target)-1);
    const int want=int(strlen(expected));
    if(length!=want || memcmp(target,expected,want)!=0)return false;
    VnodeStat resolved{};
    return !view.Stat(path,&resolved) && resolved.isDir;
}
bool ResolvedDirectory(const ProfileView& view,const char* path) {
    VnodeStat st{};return !view.Stat(path,&st) && st.isDir;
}
bool ProfileValid(const ProfileView& view) {
    const char* inner[]={"/nix/profile/bin","/nix/profile/lib","/nix/profile/share/applications"};
    // Legacy layout: the installer mutates the profile in place.
    if(Directory(view,"/nix/profile",true) && Directory(view,"/nix/share/applications",true)) {
        for(const char* path:inner)if(!Directory(view,path,true))return false;
        return true;
    }
    // Managed layout: generations are replaced, never edited, so only
    // structure is required inside them.
    // current must name a sibling generation exactly, and that generation
    // must be a real directory: no absolute, traversing or chained targets.
    VnodeStat current{};
    if(view.Stat("/nix/var/.brook-profiles/current",&current,false) || !current.isSymlink)return false;
    char name[64]{};
    const int nameLength=view.Readlink("/nix/var/.brook-profiles/current",name,sizeof(name)-1);
    if(nameLength<0 || !WritableDesktopGenerationName(name,nameLength))return false;
    char generation[96]="/nix/var/.brook-profiles/";
    memcpy(generation+strlen(generation),name,size_t(nameLength)+1);
    if(!Directory(view,generation,false) ||
       !ManagedAlias(view,"/nix/profile","var/.brook-profiles/current") ||
       !ManagedAlias(view,"/nix/share/applications","../profile/.brook-launcher"))return false;
    for(const char* path:inner)if(!ResolvedDirectory(view,path))return false;
    return true;
}
bool PayloadValid(const ProfileView& view={}) {
    const char* directories[]={"/nix/store","/nix/home/brook","/nix/var/cache/narinfo"};
    for(const char* path:directories)if(!Directory(view,path,true))return false;
    if(!ProfileValid(view))return false;
    const char* files[]={"/nix/profile/manifest.tsv","/nix/index/packages.idx","/nix/share/applications/applications.idx"};
    for(const char* path:files){VnodeStat st{};if(view.Stat(path,&st)||st.isDir||!st.size)return false;}
    const char* executables[]={"/nix/bin/nix","/nix/bin/nix-fetch","/nix/bin/nar-unpack",
        "/nix/profile/bin/brook-firefox","/nix/profile/bin/brook-chromium","/nix/profile/bin/brook-gallery"};
    for(const char* path:executables){VnodeStat st{};if(view.Stat(path,&st)||st.isDir||!(st.mode&0111))return false;}
    return true;
}
WritableDesktopBoot Fail(int error,bool mounted=false,bool publishFailed=false) {
    info.error=error;
    if(mounted && !VfsUnmount("/nix")){
        KPrintf("writable desktop: cleanup failed; maintenance only (error=%d)\n",error);
        return WritableDesktopBoot::Maintenance;
    }
    // A failed VfsMountConfigured can leave a retiring backing after failed
    // private rollback. Confirm all backing teardown before selecting fallback.
    if(publishFailed){
        VfsDrainMountCleanup();
        const int cleanup=VfsSync(nullptr);
        if(cleanup || VfsMountCleanupPending()){
            KPrintf("writable desktop: backing cleanup pending; maintenance only\n");
            return WritableDesktopBoot::Maintenance;
        }
    }
    KPrintf("writable desktop: unavailable stage=%s error=%d; %s\n",info.stage,error,
            fallbackAllowed?"use accepted RAM applications":"system volume requires maintenance");
    return fallbackAllowed?WritableDesktopBoot::Fallback:WritableDesktopBoot::Maintenance;
}
void DestroyPrivate() {
    if(image && !FatImageDeviceDestroyUnregistered(image)){image=nullptr;info.pinned=false;}
}
}
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool WritableDesktopVerifyRepairedProfile(Device* device)
{
    VfsPath initial{},root{},cwd{},target{},mounted{};
    MountNamespace* ns=nullptr;
    int error=VfsPathRoot(&initial);
    if(!error)error=VfsMountNamespaceCopy(VfsMountNamespaceInitial(),initial,initial,&ns,&root,&cwd);
    VfsPathRelease(initial);
    VfsPathRelease(cwd);
    if(error)return false;

    const bool active=VfsMountNamespaceActivate(ns);
    uint8_t drive=0;
    bool valid=false;
    if(active && !VfsPathResolve(root,root,"/nix",&target,false) && target.attachment==root.attachment &&
       Ext2MountGuardProfile(ns,target,device,drive) &&
       !VfsPathResolve(root,root,"/nix",&mounted) && mounted.attachment!=target.attachment)
        valid=PayloadValid({root,mounted.attachment});

    VfsPathRelease(mounted);
    VfsPathRelease(target);
    VfsPathRelease(root);
    if(active)VfsMountNamespaceLeave(ns,false);
    else VfsMountNamespaceAbandon(ns,false);
    VfsMountNamespaceRelease(ns);
    VfsDrainMountCleanup();
    Ext2SyncProbe probe{};
    return valid && !Ext2GetSyncProbe(drive,probe);
}
#endif
bool WritableDesktopPrepareDirect() {return NamespaceClear() && !PrepareTarget();}
WritableDesktopPublish WritableDesktopPublishDirect() {
    if(!PayloadValid())return WritableDesktopPublish::SharedProfileRejected;
    VfsPath root{};int error=VfsPathRoot(&root);
    if(!error)error=VfsMountNamespaceBindAt(VfsMountNamespaceInitial(),root,root,"/nix/home/brook","/data/home");
    VfsPathRelease(root);
    return error ? WritableDesktopPublish::HomeBindFailed : WritableDesktopPublish::Ready;
}
void WritableDesktopMarkDirectReady(uint8_t drive,uint64_t bytes) {
    directStore=true;attempted=true;info.configured=true;info.ready=true;info.pinned=true;
    info.dirty=true;info.drive=drive;info.bytes=bytes;info.stage="ready";
}
void WritableDesktopDirectFinalResult(int error,bool writeAttempted) {
    __atomic_store_n(&finalError,error,__ATOMIC_RELEASE);
    __atomic_store_n(&finalUncertain,bool(error && writeAttempted),__ATOMIC_RELEASE);
    if(!error)__atomic_store_n(&finalClean,true,__ATOMIC_RELEASE);
}
WritableDesktopBoot WritableDesktopBootMount(uint8_t* nextDrive,WritableDesktopSource sourceVolume) {
    if(attempted)return info.ready?WritableDesktopBoot::Ready:WritableDesktopBoot::Maintenance;
    attempted=true;
    if(sourceVolume!=WritableDesktopSource::Usb && sourceVolume!=WritableDesktopSource::System) {
        info.stage="source";info.error=-22;return WritableDesktopBoot::Maintenance;
    }
    fallbackAllowed=sourceVolume==WritableDesktopSource::Usb;
    auto* config=VfsOpen("/etc/writable-desktop",VFS_O_STREAM);
    if(!config) {
        if(fallbackAllowed)return WritableDesktopBoot::Absent;
        info.stage="record";info.error=-2;return WritableDesktopBoot::Maintenance;
    }
    VfsRelease(config);info.configured=true;
    info.stage="namespace";
    if(!NamespaceClear()){
        info.error=-16;KPrintf("writable desktop: existing application/home namespace; maintenance only\n");
        return WritableDesktopBoot::Maintenance;
    }
    WritableDesktopPlan plan{},ready{};
    info.stage="record";
    const char* markerPath=sourceVolume==WritableDesktopSource::System?"/media/system/STORE.OK":"/media/usb/STORE.OK";
    const char* imagePath=sourceVolume==WritableDesktopSource::System?"/media/system/STORE.IMG":"/media/usb/STORE.IMG";
    if(!Record("/etc/writable-desktop",plan) || !Record(markerPath,ready) || !WritableDesktopPlanSame(plan,ready))return Fail(-22);
    info.bytes=plan.bytes;
    info.stage="drive-slot";
    if(!nextDrive || !Ext2DriveAvailable(*nextDrive))return Fail(-16);
    info.stage="home-target";
    if(int error=PrepareTarget())return Fail(error);
    // Writable FAT opens are uncached already. STREAM is a read-only policy
    // and intentionally cannot be combined with these RW lease admissions.
    info.stage="source";
    auto* source=VfsOpen(imagePath,VFS_O_WRITE|VFS_O_READWRITE);
    if(!source)return Fail(-2);
    info.stage="lease";
    int error=0;image=FatImageDeviceCreate(source,plan.bytes,"desktop-image",&error);
    // The image owns its extra reference now. VfsClose is final destruction,
    // not a reference drop, and would invalidate the still-live backing lease.
    VfsRelease(source);
    if(!image)return Fail(error);
    info.pinned=true;
    info.stage="header-and-dirty";
    error=session.Begin(Io(),plan);
    if(error){if(error==-117)info.stage="needs-filesystem-check";info.stateUncertain=session.dirtyAttempted;DestroyPrivate();return Fail(error);}
    info.dirty=true;
    info.stage="register";
    if(!DeviceRegister(image)){DestroyPrivate();return Fail(-12);}
    info.stage="bind";
    if(!Ext2BindDevice(*nextDrive,image)){
        if(DeviceUnregister(image))DestroyPrivate();return Fail(-16);
    }
    info.drive=*nextDrive; ++*nextDrive;
    // Once bound, keep the device and source lease pinned until reboot even
    // when mount/admission fails. Ext2 slots never rebind beneath cache IDs.
    info.stage="mount";
    if(!VfsMountConfigured("/nix","ext2",info.drive,6))return Fail(-5,false,true);
    info.stage="payload";
    if(!PayloadValid())return Fail(-22,true);
    info.stage="home-bind";
    VfsPath root{};error=VfsPathRoot(&root);
    if(!error)error=VfsMountNamespaceBindAt(VfsMountNamespaceInitial(),root,root,"/nix/home/brook","/data/home");
    VfsPathRelease(root);
    if(error)return Fail(error,true);
    // There are no fallible steps after publishing the home bind. Final sync
    // can mark clean only after this complete admission, never after fallback.
    if(!session.Arm())__builtin_trap();
    info.ready=true;
    info.stage="ready";
    KPrintf("writable desktop: ready bytes=%lu drive=%u home=/data/home (persistent)\n",info.bytes,info.drive);
    return WritableDesktopBoot::Ready;
}
WritableDesktopInfo WritableDesktopRead() {
    auto out=info;out.clean=__atomic_load_n(&finalClean,__ATOMIC_ACQUIRE);
    out.dirty=info.dirty && !out.clean;
    out.stateUncertain=info.stateUncertain || __atomic_load_n(&finalUncertain,__ATOMIC_ACQUIRE);
    out.finalError=__atomic_load_n(&finalError,__ATOMIC_ACQUIRE);return out;
}
int WritableDesktopFinalSync() {
    if(!info.ready)return 0;
    if(directStore)return 0; // the direct partition owns its own final-sync session
    const int error=session.Finish(Io(),DeviceFinalSyncOwner());
    __atomic_store_n(&finalError,error,__ATOMIC_RELEASE);
    __atomic_store_n(&finalUncertain,bool(error && session.cleanAttempted),__ATOMIC_RELEASE);
    if(!error)__atomic_store_n(&finalClean,true,__ATOMIC_RELEASE);
    return error;
}
}
