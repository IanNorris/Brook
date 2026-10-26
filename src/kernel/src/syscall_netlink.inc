// Read-only route-query sockets. Included inside namespace brook after the
// syscall lifetime/user-copy helpers. Queue locks never cover user memory.
struct NetlinkPacket {NetlinkPacket* next;uint32_t length;uint8_t bytes[8192];};
struct NetlinkWait;
struct NetlinkSocket {
    SpinLock lock{};uint32_t refs=1;bool nonblock=false,connected=false;
    uint32_t port=0,type=0,queued=0,receiveLimit=65536;
    SocketIdentity identity{};NetlinkSocket* next=nullptr;
    NetlinkPacket *head=nullptr,*tail=nullptr;NetlinkWait* waiters=nullptr;
};
static SpinLock g_netlinkRegistryLock{};
static NetlinkSocket* g_netlinkSockets=nullptr;
bool NetlinkGetIdentity(void* handle,SocketIdentity* out) {
    if(!handle || !out)return false;
    *out=static_cast<NetlinkSocket*>(handle)->identity;return true;
}
struct NetlinkWait {NetlinkSocket* socket;Process* process;NetlinkWait *nextSocket,*nextGroup;};
static void NetlinkRef(NetlinkSocket* s){__atomic_fetch_add(&s->refs,1,__ATOMIC_RELAXED);}
static void NetlinkUnref(NetlinkSocket* s) {
    if(__atomic_fetch_sub(&s->refs,1,__ATOMIC_ACQ_REL)!=1)return;
    SpinLockAcquire(&g_netlinkRegistryLock);
    auto** link=&g_netlinkSockets;while(*link && *link!=s)link=&(*link)->next;
    if(*link)*link=s->next;
    SpinLockRelease(&g_netlinkRegistryLock);
    while(s->head){auto* p=s->head;s->head=p->next;kfree(p);}kfree(s);
}
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
static uint32_t NetlinkReady(NetlinkSocket* s,uint32_t events) {
    SpinLockAcquire(&s->lock);const uint32_t ready=(events&4)|((s->head && (events&1))?1u:0u);SpinLockRelease(&s->lock);return ready;
}
static uint32_t NetlinkAvailable(NetlinkSocket* s) {
    SpinLockAcquire(&s->lock);uint32_t n=s->head?s->head->length:0;SpinLockRelease(&s->lock);return n;
}
static int NetlinkBind(NetlinkSocket* s,uint32_t requested,bool automatic=false) {
    SpinLockAcquire(&g_netlinkRegistryLock);
    uint32_t port=__atomic_load_n(&s->port,__ATOMIC_ACQUIRE);
    if(port){SpinLockRelease(&g_netlinkRegistryLock);return automatic || port==requested?0:-EINVAL;}
    uint32_t candidate=requested?requested:ProcessCurrent()->tgid;
    if(!candidate)candidate=1;
    for(;;) {
        bool occupied=false;for(auto* n=g_netlinkSockets;n;n=n->next)if(n!=s && n->port==candidate){occupied=true;break;}
        if(!occupied)break;
        if(requested){SpinLockRelease(&g_netlinkRegistryLock);return -98;}
        // Stable full-width local port IDs; never confuse them with task IDs.
        candidate=candidate>=0xfffffffeu?1:candidate+1;
    }
    __atomic_store_n(&s->port,candidate,__ATOMIC_RELEASE);SpinLockRelease(&g_netlinkRegistryLock);return 0;
}
static int NetlinkDestination(uint64_t address,uint64_t length,bool optional) {
    if(!address)return optional?0:-EFAULT;
    if(length<sizeof(RouteAddress))return -EINVAL;
    RouteAddress dest{};if(!CopyFromUser(&dest,address,sizeof(dest)))return -EFAULT;
    if(dest.family!=16)return -EINVAL;
    return dest.port || dest.groups?-95:0; // kernel requests only; no fake subscriptions
}
static int NetlinkCopyName(uint32_t port,uint64_t address,uint64_t lengthAddress) {
    uint32_t length=0;if(!CopyFromUser(&length,lengthAddress,4))return -EFAULT;
    if(length>0x7fffffffu)return -EINVAL;
    RouteAddress name{16,0,port,0};uint32_t n=length<sizeof(name)?length:sizeof(name),actual=sizeof(name);
    if((n && !CopyToUser(address,&name,n)) || !CopyToUser(lengthAddress,&actual,4))return -EFAULT;
    return 0;
}
static int64_t NetlinkSend(NetlinkSocket* s,uint64_t address,uint64_t length,uint64_t flags,
                           uint64_t destination=0,uint64_t destinationLength=0,bool user=true) {
    if(flags&~uint64_t(0x40|0x4000))return -95;
    if(int e=NetlinkDestination(destination,destinationLength,true))return e;
    if(length>65536)return -90;
    if(!length)return -61;
    if(int e=NetlinkBind(s,0,true))return e;
    struct Storage {void* request;NetlinkPacket* reply;~Storage(){kfree(request);kfree(reply);}} stage{
        kmalloc(length),static_cast<NetlinkPacket*>(kmalloc(sizeof(NetlinkPacket)))};
    if(!stage.request || !stage.reply)return -ENOMEM;
    if(user){if(!CopyFromUser(stage.request,address,length))return -EFAULT;}
    else memcpy(stage.request,reinterpret_cast<void*>(address),length);
    InterfaceView views[5]{};uint32_t count=NetInterfaceViews(views,5);
    RouteReply reply(stage.reply->bytes,sizeof(stage.reply->bytes));
    if(int e=reply.build(stage.request,static_cast<uint32_t>(length),__atomic_load_n(&s->port,__ATOMIC_ACQUIRE),views,count))return e;
    stage.reply->length=reply.size();stage.reply->next=nullptr;
    SpinLockAcquire(&s->lock);
    if(s->queued>s->receiveLimit || sizeof(NetlinkPacket)>s->receiveLimit-s->queued){SpinLockRelease(&s->lock);return -105;}
    if(s->tail)s->tail->next=stage.reply;else s->head=stage.reply;
    s->tail=stage.reply;s->queued+=sizeof(NetlinkPacket);stage.reply=nullptr;
    for(auto* n=s->waiters;n;n=n->nextSocket)WakeProcess(n->process);
    SpinLockRelease(&s->lock);return static_cast<int64_t>(length);
}
static int64_t NetlinkReceive(NetlinkSocket* s,void* kernelBytes,uint32_t capacity,uint64_t flags,uint32_t& fullSize) {
    if(flags&~uint64_t(2|0x20|0x40|0x4000|0x40000000))return -95;
    NetlinkWaitGroup wait;
    for(;;) {
        SpinLockAcquire(&s->lock);auto* p=s->head;
        if(p){fullSize=p->length;uint32_t n=capacity<fullSize?capacity:fullSize;
            if(n)memcpy(kernelBytes,p->bytes,n);
            if(!(flags&2)){s->head=p->next;if(!s->head)s->tail=nullptr;s->queued-=sizeof(NetlinkPacket);}
            SpinLockRelease(&s->lock);if(!(flags&2))kfree(p);return n;
        }
        bool nonblock=__atomic_load_n(&s->nonblock,__ATOMIC_ACQUIRE);SpinLockRelease(&s->lock);
        if((flags&0x40)||nonblock)return -EAGAIN;
        if(HasPendingSignals())return -EINTR;
        if(!wait.nodes){if(!wait.add(s,ProcessCurrent()))return -ENOMEM;continue;}
        ProcessCurrent()->wakeupTick=ApicTickCount()+100;
        SchedulerBlockInterruptible(ProcessCurrent());
    }
}
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
static int64_t NetlinkVectorIo(NetlinkSocket* s,uint64_t address,uint64_t count,bool writing) {
    KernelOperationGuard operation(ProcessCurrent());
    NetlinkVector stage;if(int e=stage.init(address,count,writing))return e;if(!stage.total)return 0;
    if(writing)return NetlinkSend(s,reinterpret_cast<uint64_t>(stage.bytes),stage.total,0,0,0,false);
    uint32_t size=0;auto n=NetlinkReceive(s,stage.bytes,stage.capacity,0,size);if(n<0)return n;
    if(int e=stage.scatter(n))return e;return n;
}
static int64_t NetlinkReadUser(NetlinkSocket* s,uint64_t address,uint64_t length,uint64_t flags,
                              uint64_t source=0,uint64_t sourceLength=0) {
    uint32_t capacity=static_cast<uint32_t>(length<8192?length:8192);
    if(capacity && !UserBufferWritable(address,capacity))return -EFAULT;
    uint32_t nameLength=0;
    if(source && (!CopyFromUser(&nameLength,sourceLength,4) || !UserBufferWritable(sourceLength,4)))return -EFAULT;
    if(source && (nameLength>0x7fffffffu || !UserBufferWritable(source,nameLength<12?nameLength:12)))return -EFAULT;
    auto* bytes=static_cast<uint8_t*>(kmalloc(capacity?capacity:1));if(!bytes)return -ENOMEM;
    uint32_t size=0;auto n=NetlinkReceive(s,bytes,capacity,flags,size);
    if(n>=0 && n && !CopyToUser(address,bytes,n))n=-EFAULT;
    kfree(bytes);if(n<0)return n;
    if(source){if(int e=NetlinkCopyName(0,source,sourceLength))return e;}
    return (flags&0x20)?size:n;
}
static int NetlinkInterfaceIoctl(uint64_t command,uint64_t address) {
    KernelOperationGuard operation(ProcessCurrent());
    // Native x86-64 struct ifreq: 16-byte name + 24-byte union.
    uint8_t request[40]{};if(!CopyFromUser(request,address,sizeof(request)))return -EFAULT;
    InterfaceView views[5]{};uint32_t count=NetInterfaceViews(views,5);const InterfaceView* found=nullptr;
    uint32_t index=0;memcpy(&index,request+16,4);
    for(uint32_t i=0;i<count;++i) {
        bool match=command==0x8910?index==views[i].index:!memcmp(request,views[i].name,16);
        if(match){found=&views[i];break;}
    }
    // ifreq names need not have zero padding after their terminator.
    if(!found && command!=0x8910)for(uint32_t i=0;i<count;++i) {
        unsigned j=0;for(;j<16 && request[j] && request[j]==static_cast<uint8_t>(views[i].name[j]);++j){}
        if(j<16 && !request[j] && !views[i].name[j]){found=&views[i];break;}
    }
    if(!found)return -19;
    switch(command){
    case 0x8910:memcpy(request,found->name,16);break; // SIOCGIFNAME
    case 0x8933:memcpy(request+16,&found->index,4);break;
    case 0x8913:{uint16_t flags=found->flags;memcpy(request+16,&flags,2);break;}
    case 0x8921:memcpy(request+16,&found->mtu,4);break;
    case 0x8927:memset(request+16,0,16);memcpy(request+16,&found->hardwareType,2);memcpy(request+18,found->mac,6);break;
    case 0x8915:case 0x8919:case 0x891b: { // address, broadcast, netmask
        if(!found->address)return -99;
        uint16_t family=2;memset(request+16,0,16);memcpy(request+16,&family,2);
        uint32_t ip=command==0x891b?found->netmask:command==0x8919?(found->address|~found->netmask):found->address;
        memcpy(request+20,&ip,4);break;
    }
    default:return -25;
    }return CopyToUser(address,request,sizeof(request))?0:-EFAULT;
}
