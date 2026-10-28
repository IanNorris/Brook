#include "compressed_vfs.h"
#include "compressed_image.h"
#include "vfs.h"
#include "vfs_object.h"
#include "device.h"
#include "memory/heap.h"
#include "memory/file_page_cache.h"
#include "process.h"
#include "sync/kmutex.h"
#include "fw_cfg.h"
#include "serial.h"
#include "string.h"
#include "lz4.h"
namespace brook {
namespace {
constexpr uint32_t None=~uint32_t{0};
Device* devices[DEVICE_MAX]{};
uint32_t live[DEVICE_MAX]{};
struct Mount {
    Device* device;uint8_t drive;uint32_t references,cacheNamespace;
    KMutex io;
    CompressedImage image;
    void* index;CompressedImageNodeState* nodes;uint32_t* links;
    uint8_t *encoded,*decoded;
};
struct Object { VfsObject base;Mount* mount;uint32_t inode,entry; };
Object* Private(VfsObject* object){return static_cast<Object*>(object->priv);}
void Retain(Mount* m){__atomic_add_fetch(&m->references,1,__ATOMIC_RELAXED);}
void Drop(Mount* m) {
    if(__atomic_sub_fetch(&m->references,1,__ATOMIC_ACQ_REL))return;
    __atomic_sub_fetch(&live[m->drive],1,__ATOMIC_RELEASE);
    kfree(m->index);kfree(m->nodes);kfree(m->links);kfree(m->encoded);kfree(m->decoded);kfree(m);
}
int DeviceRead(void* context,uint64_t offset,void* out,uint64_t bytes) {
    auto* m=static_cast<Mount*>(context);
    if(!DeviceIsRegistered(m->device))return -5;
    return m->device->ops->read(m->device,offset,out,bytes);
}
int Decode(const void* in,int bytes,void* out,int capacity){return LZ4_decompress_safe(static_cast<const char*>(in),static_cast<char*>(out),bytes,capacity);}
int FillStat(Mount* m,uint32_t id,VnodeStat* out) {
    auto* n=m->image.Inode(id);if(!n || !out)return -22;
    *out={};out->size=n->size;out->ino=id+1;out->uid=n->uid;out->gid=n->gid;
    out->mode=n->mode;out->isDir=n->kind==CompressedImageKind::Directory;out->isSymlink=n->kind==CompressedImageKind::Symlink;
    out->mtime={n->mtime,0};out->atime=out->ctime=out->mtime;out->nlink=m->links[id];
    out->dev=(253ULL<<32)|m->cacheNamespace;return 0;
}
int Access(Mount*,const VnodeStat* st,uint8_t required,bool real) {
    if(required&2)return -30;
    return ProcessCheckFileAccess(ProcessCurrent(),st->uid,st->gid,st->mode,required,real);
}
const VfsObjectOps* ObjectOps();
int NewObject(Mount* m,uint32_t id,uint32_t entry,VfsObject** out) {
    if(!out || !m->image.Inode(id))return -22;*out=nullptr;
    auto* o=static_cast<Object*>(kmalloc(sizeof(Object)));if(!o)return -12;
    memset(o,0,sizeof(*o));o->mount=m;o->inode=id;o->entry=entry;
    o->base={ObjectOps(),o,(uint64_t(m->cacheNamespace)<<32)|(uint64_t(id)+1),1};
    Retain(m);*out=&o->base;return 0;
}
void ObjectDestroy(VfsObject* object){auto* o=Private(object);auto* m=o->mount;kfree(o);Drop(m);}
int ObjectStat(VfsObject* object,VnodeStat* out){auto* o=Private(object);return FillStat(o->mount,o->inode,out);}
int ObjectAccess(VfsObject* object,uint8_t required,bool real){VnodeStat st{};int e=ObjectStat(object,&st);return e?e:Access(Private(object)->mount,&st,required,real);}
int ObjectLookup(VfsObject* object,const char* name,VfsObject** out,bool real) {
    if(out)*out=nullptr;auto* o=Private(object);
    if(!out || !name)return -22;
    if(int e=ObjectAccess(object,1,real))return e;
    uint32_t length=0;for(;name[length];++length)if(length>=255)return -36;else if(name[length]=='/')return -22;
    if(!length || (length==1 && name[0]=='.') || (length==2 && name[0]=='.' && name[1]=='.'))return -22;
    auto found=o->mount->image.FindChild(o->inode,name,length);if(found<0)return found;
    return NewObject(o->mount,o->mount->image.Entry(found)->inode,found,out);
}
int ObjectParent(VfsObject* object,VfsObject** out,bool real) {
    auto* o=Private(object);if(!out)return -22;*out=nullptr;
    if(o->mount->image.Inode(o->inode)->kind!=CompressedImageKind::Directory)return -20;
    if(int e=ObjectAccess(object,1,real))return e;
    uint32_t parent=o->inode?o->mount->image.State(o->inode)->parent:0;
    return NewObject(o->mount,parent,parent?o->mount->image.State(parent)->directoryEntry:None,out);
}
int ObjectReadlink(VfsObject* object,char* out,uint32_t capacity) {
    auto* o=Private(object);auto* n=o->mount->image.Inode(o->inode);
    if(n->kind!=CompressedImageKind::Symlink)return -22;
    uint32_t bytes=n->targetBytes<capacity?n->targetBytes:capacity;
    if(!out && bytes)return -22;
    if(bytes)memcpy(out,o->mount->image.Target(o->inode),bytes);return bytes;
}
int ObjectPath(VfsObject* object,char* out,uint32_t capacity) {
    auto* o=Private(object);if(!out || capacity<2)return -36;
    if(!o->inode){out[0]='/';out[1]=0;return 0;}
    uint32_t chain[129],count=0,entry=o->entry;
    if(entry==None)return -95;
    for(;;) {
        if(count==129)return -36;chain[count++]=entry;
        auto parent=o->mount->image.Entry(entry)->parent;if(!parent)break;
        entry=o->mount->image.State(parent)->directoryEntry;
    }
    uint32_t length=1;
    for(uint32_t i=0;i<count;++i)length+=o->mount->image.Entry(chain[i])->nameBytes+(i!=0);
    if(length>=capacity)return -36;
    uint32_t at=0;out[at++]='/';
    for(uint32_t i=count;i;--i) {
        auto* e=o->mount->image.Entry(chain[i-1]);
        memcpy(out+at,o->mount->image.Name(*e),e->nameBytes);at+=e->nameBytes;if(i!=1)out[at++]='/';
    }
    out[at]=0;return 0;
}
int DirectoryPath(VfsObject* object,char* out,uint32_t capacity) {
    auto* o=Private(object);return o->mount->image.Inode(o->inode)->kind==CompressedImageKind::Directory?ObjectPath(object,out,capacity):-20;
}
int ObjectEntryParent(VfsObject* object,VfsObject** out,char* leaf,uint32_t capacity) {
    if(!out || !leaf)return -22;*out=nullptr;auto* o=Private(object);if(o->entry==None)return -2;
    auto* e=o->mount->image.Entry(o->entry);if(capacity<=e->nameBytes)return -36;
    int error=NewObject(o->mount,e->parent,e->parent?o->mount->image.State(e->parent)->directoryEntry:None,out);
    if(!error){memcpy(leaf,o->mount->image.Name(*e),e->nameBytes);leaf[e->nameBytes]=0;}return error;
}
int VnodeRead(Vnode* vn,void* out,uint64_t length,uint64_t* offset) {
    if(!offset)return -22;auto* o=Private(vn->backendObject);auto* m=o->mount;
    if(m->image.Inode(o->inode)->kind==CompressedImageKind::Symlink)return -40;
    if(length>0x7fffffffu)length=0x7fffffffu;
    KMutexLock(&m->io);int result=m->image.Read(o->inode,*offset,out,length);KMutexUnlock(&m->io);
    if(result>0)*offset+=result;return result;
}
int VnodeWrite(Vnode*,const void*,uint64_t,uint64_t*){return -30;}
int VnodeStatFn(Vnode* vn,VnodeStat* out){return ObjectStat(vn->backendObject,out);}
int VnodeAccess(Vnode* vn,uint8_t required,bool real){return ObjectAccess(vn->backendObject,required,real);}
int VnodeDirectoryPath(Vnode* vn,char* out,uint32_t capacity){return DirectoryPath(vn->backendObject,out,capacity);}
int VnodeReadDir(Vnode* vn,DirEntry* out,uint32_t* cookie) {
    if(!out || !cookie)return -22;auto* o=Private(vn->backendObject);auto& image=o->mount->image;
    if(image.Inode(o->inode)->kind!=CompressedImageKind::Directory)return -20;
    *out={};
    if(*cookie<2){out->name[0]='.';if(*cookie==1)out->name[1]='.';out->isDir=true;++*cookie;return 1;}
    uint64_t low=0,high=image.Header().direntCount;
    while(low<high){auto mid=low+(high-low)/2;if(image.Entry(mid)->parent<o->inode)low=mid+1;else high=mid;}
    uint64_t at=low+uint64_t(*cookie-2);if(at>=image.Header().direntCount)return 0;
    auto* e=image.Entry(at);if(e->parent!=o->inode)return 0;
    memcpy(out->name,image.Name(*e),e->nameBytes);out->name[e->nameBytes]=0;
    auto* n=image.Inode(e->inode);out->size=n->size;out->isDir=n->kind==CompressedImageKind::Directory;++*cookie;return 1;
}
void VnodeClose(Vnode*){} // VFS releases the owned backendObject after close.
int VnodeSync(Vnode*){return 0;}
int VnodeTimes(Vnode*,const VnodeTime*){return -30;}
int VnodeChmod(Vnode*,uint16_t){return -30;}
int VnodeTruncate(Vnode*,uint64_t){return -30;}
int VnodeChown(Vnode*,uint32_t,uint32_t){return -30;}
int VnodeSpace(Vnode* vn,VfsSpace* out) {
    if(!out)return -22;auto* o=Private(vn->backendObject);auto& h=o->mount->image.Header();
    *out={};out->type=0x42524b50;out->blockSize=4096;out->blocks=h.imageBytes/4096;out->files=h.inodeCount;
    out->identity=o->mount->cacheNamespace;out->maxName=255;out->fragmentSize=4096;out->flags=1;return 0;
}
const VnodeOps vnodeOps={nullptr,VnodeRead,VnodeWrite,VnodeReadDir,VnodeClose,VnodeStatFn,VnodeSync,
    VnodeTimes,VnodeChmod,VnodeTruncate,nullptr,VnodeChown,nullptr,VnodeSpace,VnodeAccess,VnodeDirectoryPath};
Vnode* ObjectOpen(VfsObject* object,int flags,int* error) {
    auto fail=[&](int e)->Vnode*{if(error)*error=e;return nullptr;};
    auto* o=Private(object);auto kind=o->mount->image.Inode(o->inode)->kind;
    if(flags&(VFS_O_WRITE|VFS_O_TRUNC|VFS_O_READWRITE))return fail(-30);
    if((flags&(VFS_O_CREATE|VFS_O_EXCL))==(VFS_O_CREATE|VFS_O_EXCL))return fail(-17);
    if(!(flags&VFS_O_METADATA)) {
        if(kind==CompressedImageKind::Symlink)return fail(-40);
        if(int e=ObjectAccess(object,(flags&VFS_O_EXEC)?1:4,false))return fail(e);
    }
    auto* vn=static_cast<Vnode*>(kmalloc(sizeof(Vnode)));if(!vn)return fail(-12);
    memset(vn,0,sizeof(*vn));vn->ops=&vnodeOps;vn->type=kind==CompressedImageKind::Directory?VnodeType::Dir:VnodeType::File;
    vn->priv=o;vn->refCount=1;vn->backendObject=object;VfsObjectRetain(object);
    vn->cacheId=kind==CompressedImageKind::File?object->identity:0;if(error)*error=0;return vn;
}
Vnode* ObjectCreate(VfsObject*,const char*,int,uint16_t,int* error){if(error)*error=-30;return nullptr;}
int ObjectMkdir(VfsObject*,const char*,uint16_t){return -30;}
int ObjectRemove(VfsObject*,const char*,bool){return -30;}
int ObjectRename(VfsObject*,const char*,VfsObject*,const char*){return -30;}
int ObjectSymlink(VfsObject*,const char*,const char*){return -30;}
Vnode* ObjectSpecial(VfsObject*,const char*,uint16_t,int* error){if(error)*error=-30;return nullptr;}
int ObjectTimes(VfsObject*,const VnodeTime*){return -30;}
int ObjectChmod(VfsObject*,uint16_t){return -30;}
int ObjectChown(VfsObject*,uint32_t,uint32_t){return -30;}
int ObjectLink(VfsObject*,VfsObject*,const char*){return -30;}
const VfsObjectOps objectOps={ObjectDestroy,ObjectStat,ObjectAccess,ObjectLookup,ObjectParent,ObjectReadlink,ObjectOpen,
    ObjectCreate,ObjectMkdir,ObjectRemove,ObjectRename,ObjectSymlink,ObjectSpecial,ObjectTimes,ObjectChmod,ObjectChown,
    DirectoryPath,nullptr,ObjectPath,ObjectSpecial,ObjectEntryParent,ObjectLink};
const VfsObjectOps* ObjectOps(){return &objectOps;}
bool MountImage(uint8_t drive,void** out) {
    if(!out || drive>=DEVICE_MAX || !devices[drive])return false;*out=nullptr;
    auto* dev=devices[drive];if(!DeviceIsRegistered(dev) || dev->type!=DeviceType::Block || !dev->ops->read)return false;
    uint64_t count=DeviceBlockCount(dev),bytes=DeviceBlockSize(dev);
    if(!bytes || count>~uint64_t{0}/bytes)return false;bytes*=count;
    CompressedImageHeader header{};
    if(dev->ops->read(dev,0,&header,sizeof(header))!=sizeof(header) || !CompressedImageHeaderValid(header,bytes))return false;
    auto* m=static_cast<Mount*>(kmalloc(sizeof(Mount)));if(!m)return false;
    memset(m,0,sizeof(*m));m->references=1;m->device=dev;m->drive=drive;KMutexInit(&m->io);
    __atomic_add_fetch(&live[drive],1,__ATOMIC_RELAXED);
    m->index=kmalloc(header.dataOffset-sizeof(header));m->nodes=static_cast<CompressedImageNodeState*>(kmalloc(header.inodeCount*sizeof(CompressedImageNodeState)));
    m->links=static_cast<uint32_t*>(kmalloc(header.inodeCount*sizeof(uint32_t)));m->encoded=static_cast<uint8_t*>(kmalloc(65536));m->decoded=static_cast<uint8_t*>(kmalloc(65536));
    if(!m->index || !m->nodes || !m->links || !m->encoded || !m->decoded ||
       dev->ops->read(dev,sizeof(header),m->index,header.dataOffset-sizeof(header))!=int(header.dataOffset-sizeof(header)) ||
       !m->image.Initialize(header,bytes,m->index,header.dataOffset-sizeof(header),m->nodes,header.inodeCount,{m,DeviceRead,Decode},m->encoded,m->decoded)) {Drop(m);return false;}
    m->cacheNamespace=FilePageCacheAllocateNamespace();if(!m->cacheNamespace){Drop(m);return false;}
    for(uint32_t i=0;i<header.inodeCount;++i)m->links[i]=m->image.Inode(i)->kind==CompressedImageKind::Directory?2:m->image.State(i)->links;
    for(uint32_t i=0;i<header.direntCount;++i){auto* e=m->image.Entry(i);if(m->image.Inode(e->inode)->kind==CompressedImageKind::Directory)++m->links[e->parent];}
    SerialPrintf("brookimg: mounted inodes=%lu index=%lu node_state=%lu cache=131072 ns=%u\n",header.inodeCount,header.dataOffset-sizeof(header),header.inodeCount*(sizeof(CompressedImageNodeState)+4),m->cacheNamespace);
    *out=m;return true;
}
int UnmountImage(void* privateMount){Drop(static_cast<Mount*>(privateMount));return 0;}
Vnode* OpenPath(void* p,uint8_t,const char* path,int flags,uint16_t,int* error) {
    auto* m=static_cast<Mount*>(p);uint32_t entry=None;auto id=m->image.FindPath(path,&entry);
    if(id<0){if(error)*error=(id==-2 && (flags&VFS_O_CREATE))?-30:id;return nullptr;}
    VfsObject* object=nullptr;int e=NewObject(m,id,entry,&object);if(e){if(error)*error=e;return nullptr;}
    auto* vn=ObjectOpen(object,flags,error);VfsObjectRelease(object);return vn;
}
Vnode* OpenLegacy(void* p,uint8_t drive,const char* path,int flags){return OpenPath(p,drive,path,flags,0,nullptr);}
int StatPath(void* p,uint8_t,const char* path,VnodeStat* out){auto* m=static_cast<Mount*>(p);auto id=m->image.FindPath(path);return id<0?id:FillStat(m,id,out);}
int ReadlinkPath(void* p,uint8_t,const char* path,char* out,uint64_t length){auto* m=static_cast<Mount*>(p);uint32_t entry=None;auto id=m->image.FindPath(path,&entry);if(id<0)return id;VfsObject* object=nullptr;int e=NewObject(m,id,entry,&object);if(e)return e;int n=ObjectReadlink(object,out,length>0xffffffffu?0xffffffffu:length);VfsObjectRelease(object);return n;}
int FsUnlink(void*,uint8_t,const char*){return -30;}
int FsMkdir(void*,uint8_t,const char*,uint16_t){return -30;}
int FsRename(void*,uint8_t,const char*,const char*){return -30;}
int FsSymlink(void*,uint8_t,const char*,const char*){return -30;}
int FsSync(void*){return 0;}
int FsTimes(void*,uint8_t,const char*,const VnodeTime*,bool){return -30;}
int FsChmod(void*,uint8_t,const char*,uint16_t){return -30;}
int FsChown(void*,uint8_t,const char*,uint32_t,uint32_t,bool){return -30;}
int FsRemove(void*,uint8_t,const char*,bool){return -30;}
Vnode* FsSocket(void*,uint8_t,const char*,uint16_t,int* error){if(error)*error=-30;return nullptr;}
int FsAccess(void* p,const VnodeStat* st,uint8_t required){return Access(static_cast<Mount*>(p),st,required,false);}
int FsRealAccess(void* p,const VnodeStat* st,uint8_t required){return Access(static_cast<Mount*>(p),st,required,true);}
int RootObject(void* p,uint8_t,VfsObject** out){return NewObject(static_cast<Mount*>(p),0,None,out);}
int FromVnode(Vnode* vn,VfsObject** out){if(!out || !vn || !vn->backendObject || vn->backendObject->ops!=&objectOps)return -9;*out=vn->backendObject;VfsObjectRetain(*out);return 0;}
const VfsFsOps fsOps={.mount=MountImage,.unmount=UnmountImage,.open=OpenLegacy,.stat_path=StatPath,.lstat_path=StatPath,
    .unlink=FsUnlink,.mkdir=FsMkdir,.rename=FsRename,.symlink=FsSymlink,.readlink=ReadlinkPath,.sync=FsSync,
    .set_times=FsTimes,.chmod=FsChmod,.remove_path=FsRemove,.chown=FsChown,
    .open_mode_checked=OpenPath,.check_access=FsAccess,.check_access_real=FsRealAccess,
    .create_socket=FsSocket,.root_object=RootObject,.vnode_object=FromVnode};
}
const VfsFsOps* CompressedVfsOperations(){return &fsOps;}
bool CompressedVfsBindDevice(uint8_t drive,Device* device) {
    if(drive>=DEVICE_MAX || !device || __atomic_load_n(&live[drive],__ATOMIC_ACQUIRE))return false;
    devices[drive]=device;return true;
}
void CompressedVfsRegister(){VfsRegisterFs("brookimg",&fsOps);}
void CompressedVfsBootMount() {
    char device[32]{},path[64]{};auto n=FwCfgReadFile("opt/compressed-image-device",device,sizeof(device));
    if(!n || n>=sizeof(device))return;device[n]=0;
    auto length=FwCfgReadFile("opt/compressed-image-mount",path,sizeof(path));
    if(length>=sizeof(path))return;
    if(!length)memcpy(path,"/data/packed",13);else path[length]=0;
    auto* dev=DeviceFind(device);
    if(path[0]!='/' || !CompressedVfsBindDevice(0,dev))return;
    VnodeStat st{};int result=VfsStatPath(path,&st);
    if(result==-2)result=VfsMkdir(path,0755);
    if(result || !VfsMountConfigured(path,"brookimg",0,7))SerialPrintf("brookimg: boot mount failed device=%s path=%s\n",device,path);
    else {FilePageCacheEnableReclaim();SerialPrintf("brookimg: ready device=%s path=%s readonly\n",device,path);}
}
}
