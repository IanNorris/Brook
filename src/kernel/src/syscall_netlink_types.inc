#pragma once
// Netlink route-socket object types, shared by syscall_netlink.inc and by the
// descriptor code that polls, closes or inspects netlink sockets. Included
// inside namespace brook after SpinLock, SocketIdentity, Process, kmalloc/kfree,
// ProcessRef/ProcessUnref, the user-copy helpers and EINVAL/ENOMEM are declared
// (split from syscall_netlink.inc).
struct NetlinkPacket {NetlinkPacket* next;uint32_t length;uint8_t bytes[8192];};
struct NetlinkWait;
struct NetlinkSocket {
    SpinLock lock{};uint32_t refs=1;bool nonblock=false,connected=false;
    uint32_t port=0,type=0,queued=0,receiveLimit=65536;
    uint64_t networkScope=0;
    SocketIdentity identity{};NetlinkSocket* next=nullptr;
    NetlinkPacket *head=nullptr,*tail=nullptr;NetlinkWait* waiters=nullptr;
};
struct NetlinkWait {NetlinkSocket* socket;Process* process;NetlinkWait *nextSocket,*nextGroup;};
void NetlinkRef(NetlinkSocket* s);
void NetlinkUnref(NetlinkSocket* s);
struct NetlinkWaitGroup {
    NetlinkWait* nodes=nullptr;
    bool add(NetlinkSocket* s,Process* p) {
        for(auto* n=nodes;n;n=n->nextGroup)if(n->socket==s)return true;
        auto* n=static_cast<NetlinkWait*>(kmalloc(sizeof(NetlinkWait)));if(!n)return false;
        NetlinkRef(s);ProcessRef(p);
        SpinLockAcquire(&s->lock);*n={s,p,s->waiters,nodes};s->waiters=n;nodes=n;SpinLockRelease(&s->lock);return true;
    }
    ~NetlinkWaitGroup(){while(nodes){auto* n=nodes;nodes=n->nextGroup;auto* s=n->socket;
        SpinLockAcquire(&s->lock);auto** link=&s->waiters;while(*link && *link!=n)link=&(*link)->nextSocket;
        if(*link)*link=n->nextSocket;SpinLockRelease(&s->lock);
        ProcessUnref(n->process);NetlinkUnref(s);kfree(n);
    }}
};
struct NetlinkVector {
    struct Vector {uint64_t address,length;};
    Vector* vectors=nullptr;uint8_t* bytes=nullptr;uint64_t count=0,total=0;uint32_t capacity=0;
    ~NetlinkVector(){kfree(vectors);kfree(bytes);}
    int init(uint64_t address,uint64_t n,bool writing) {
        if(n>1024)return -EINVAL;
        count=n;vectors=static_cast<Vector*>(kmalloc(n?n*sizeof(Vector):1));if(!vectors)return -ENOMEM;
        if(n && !CopyFromUser(vectors,address,n*sizeof(Vector)))return -EFAULT;
        for(uint64_t i=0;i<n;++i){if(vectors[i].length>0x7fffffffULL-total)return -EINVAL;total+=vectors[i].length;}
        if(writing && total>65536)return -90;
        capacity=static_cast<uint32_t>(total<(writing?65536u:8192u)?total:(writing?65536u:8192u));
        bytes=static_cast<uint8_t*>(kmalloc(capacity?capacity:1));if(!bytes)return -ENOMEM;
        uint64_t offset=0;
        for(uint64_t i=0;i<n && offset<capacity;++i){uint64_t part=vectors[i].length;if(part>capacity-offset)part=capacity-offset;
            if(part && (writing?!CopyFromUser(bytes+offset,vectors[i].address,part):!UserBufferWritable(vectors[i].address,part)))return -EFAULT;
            offset+=part;
        }return 0;
    }
    int scatter(uint32_t length){uint32_t offset=0;for(uint64_t i=0;i<count && offset<length;++i){
        uint32_t part=static_cast<uint32_t>(vectors[i].length<length-offset?vectors[i].length:length-offset);
        if(part && !CopyToUser(vectors[i].address,bytes+offset,part))return -EFAULT;offset+=part;
    }return 0;}
};
