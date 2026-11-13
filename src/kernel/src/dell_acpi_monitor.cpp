#include "dell_acpi_monitor.h"
#include "acpi.h"
#include "acpi_embedded_controller.h"
#include "apic.h"
#include "idt.h"
#include "klog.h"
#include "memory/heap.h"
#include "memory/virtual_memory.h"
#include "pci.h"
#include "portio.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "shutdown.h"
#include "spinlock.h"
#include "sync/kmutex.h"
#include "vfs.h"

#include <uacpi/acpi.h>
#include <uacpi/context.h>
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>

namespace brook {
namespace {
constexpr uint16_t EcData=0x930,EcCommand=0x934,PowerControl=0x1804,Timer=0x1808;
constexpr uint16_t SmiCommand=0xb2;
constexpr uint8_t AcpiEnable=0xd1;
constexpr uint16_t SciEnable=1;
constexpr uint64_t FacsLockOffset=16;
uint64_t rsdp=0;
uint8_t sciPin=0,sciVector=0;
uint8_t ecInitialized=0;
bool failed=false,initializing=true,irqEnabled=false;
uint32_t sciCount=0;
uint64_t sciWindow=0;
uacpi_interrupt_handler sciHandler=nullptr;
uacpi_handle sciContext=nullptr;
AcpiEmbeddedController ec;

struct Span {uint64_t base,bytes;};
constexpr Span fixedIo[]={{0x1800,4},{PowerControl,2},{Timer,4},{0x1850,1},{0x1880,32}};
constexpr Span readNvs[]={
    {0xdb7eae18,0x18a},{0xdb7ed000,8},{0xdb7ef000,0x35b},
    {0xdb7f1d98,0xe4},{0xdb7f2fe3,7},
};
constexpr Span initWriteNvs[]={{0xdb7eae60,1},{0xdb7ed000,8}};
constexpr Span sampleWriteNvs[]={{0xdb7ef00f,1}};

bool Within(Span span,uint64_t base,uint64_t size) {
    return size && base>=span.base && base-span.base<span.bytes && size<=span.bytes-(base-span.base);
}
template<unsigned N> bool WithinAny(const Span (&spans)[N],uint64_t base,uint64_t size) {
    for(const auto& span:spans)if(Within(span,base,size))return true;
    return false;
}
bool Failed(){return __atomic_load_n(&failed,__ATOMIC_ACQUIRE);}
uacpi_status Reject(const char* operation,uint64_t address=0,uint64_t width=0) {
    if(!__atomic_exchange_n(&failed,true,__ATOMIC_ACQ_REL))
        SerialPrintf("dell-acpi: denied %s address=%lx width=%lu\n",operation,address,width);
    return UACPI_STATUS_DENIED;
}
bool WritableNvs(Span span) {
    if(!AcpiNvsContains(span.base,uint32_t(span.bytes)))return false;
    const uint64_t end=span.base+span.bytes;
    for(uint64_t page=span.base&~uint64_t{4095};page<end;page+=4096) {
        KernelAliasMapping identity{},direct{};
        if(!VmmInspectKernelAliases(page,identity,direct) || !identity.present || !identity.writable ||
           identity.physical!=page || (direct.present && direct.patIndex!=identity.patIndex))return false;
    }
    return true;
}
bool LpcDecodes(uint16_t port) {
    for(uint8_t reg=0x84;reg<=0x90;reg+=4) {
        const uint32_t decode=PciConfigRead32(0,31,0,reg);
        const uint32_t addressMask=0xfffcu & ~(decode>>16 & 0xfcu);
        if(decode!=~uint32_t{0} && (decode&1) && (port&addressMask)==(decode&addressMask))return true;
    }
    return false;
}
bool Requested() {
    auto* file=VfsOpen("/etc/dell-acpi-monitor",VFS_O_STREAM);
    if(!file)return false;
    VnodeStat st{};uint64_t offset=0;char bytes[2]{};
    const bool requested=!VfsStat(file,&st) && !st.isDir && !st.isSymlink && st.size==2 &&
        VfsRead(file,bytes,2,&offset)==2 && bytes[0]=='1' && bytes[1]=='\n';
    VfsRelease(file);
    return requested;
}
void Sci() {
    const auto now=ApicTickCount();
    if(now-sciWindow>=10){sciWindow=now;sciCount=0;}
    if(++sciCount>64) {
        IoApicMaskIrq(sciPin);
        __atomic_store_n(&failed,true,__ATOMIC_RELEASE);
        return;
    }
    if(sciHandler)sciHandler(sciContext);
}
void Stall(uint8_t us) {
    const uint32_t begin=inl(Timer)&0xffffff;
    const uint32_t ticks=(uint32_t(us)*3580+999)/1000;
    for(unsigned poll=0;poll<16384;++poll) {
        if(((inl(Timer)-begin)&0xffffff)>=ticks)return;
        __asm__ volatile("pause");
    }
    Reject("stopped PM timer",Timer,4);
}
struct EcIo {
    uint64_t nowUs(){return ApicTickCount()*1000;}
    void delayPoll(){Stall(10);}
    uint8_t status(){return Failed()?0xff:inb(EcCommand);}
    void command(uint8_t byte){outb(EcCommand,byte);}
    void writeData(uint8_t byte){outb(EcData,byte);}
    uint8_t readData(){return inb(EcData);}
};
uacpi_status EcRegion(uacpi_region_op op,uacpi_handle data) {
    if(op==UACPI_REGION_OP_ATTACH || op==UACPI_REGION_OP_DETACH)return UACPI_STATUS_OK;
    if(op!=UACPI_REGION_OP_READ && op!=UACPI_REGION_OP_WRITE)return Reject("EC operation");
    auto& rw=*static_cast<uacpi_region_rw_data*>(data);
    if(Failed() || rw.byte_width!=1 ||
       (op==UACPI_REGION_OP_READ && (rw.offset!=6 && (!initializing || rw.offset!=0))) ||
       (op==UACPI_REGION_OP_WRITE && (!initializing || (rw.offset!=1 && rw.offset!=5))))
        return Reject("EC byte",rw.offset,rw.byte_width);
    EcIo io;
    uint8_t byte=uint8_t(rw.value);
    const auto result=op==UACPI_REGION_OP_READ?ec.Read(io,uint8_t(rw.offset),byte):ec.Write(io,uint8_t(rw.offset),byte);
    if(result!=AcpiEcResult::Ok)return Reject("EC transaction",rw.offset,uint64_t(result));
    if(op==UACPI_REGION_OP_READ)rw.value=byte;
    else ecInitialized|=rw.offset==1?1:2;
    return UACPI_STATUS_OK;
}
template<class T> T ReadMemory(uint64_t address) {
    return *reinterpret_cast<const volatile T*>(address);
}
template<class T> void WriteMemory(uint64_t address,T value) {
    *reinterpret_cast<volatile T*>(address)=value;
}
uacpi_status MemoryRegion(uacpi_region_op op,uacpi_handle data) {
    if(op==UACPI_REGION_OP_ATTACH || op==UACPI_REGION_OP_DETACH)return UACPI_STATUS_OK;
    if(op!=UACPI_REGION_OP_READ && op!=UACPI_REGION_OP_WRITE)return Reject("memory operation");
    auto& rw=*static_cast<uacpi_region_rw_data*>(data);
    const uint64_t address=rw.address;
    const uint8_t width=rw.byte_width;
    if(Failed() || (width!=1 && width!=2 && width!=4 && width!=8) || (address&(width-1)) ||
       !WithinAny(readNvs,address,width) || !AcpiNvsContains(address,width))
        return Reject("AML memory",address,width);
    if(op==UACPI_REGION_OP_READ) {
        switch(width) {
        case 1:rw.value=ReadMemory<uint8_t>(address);break;
        case 2:rw.value=ReadMemory<uint16_t>(address);break;
        case 4:rw.value=ReadMemory<uint32_t>(address);break;
        case 8:rw.value=ReadMemory<uint64_t>(address);break;
        }
        return UACPI_STATUS_OK;
    }
    if(!WithinAny(sampleWriteNvs,address,width) &&
       (!initializing || !WithinAny(initWriteNvs,address,width)))
        return Reject("AML memory write",address,width);
    switch(width) {
    case 1:WriteMemory(address,uint8_t(rw.value));break;
    case 2:WriteMemory(address,uint16_t(rw.value));break;
    case 4:WriteMemory(address,uint32_t(rw.value));break;
    case 8:WriteMemory(address,uint64_t(rw.value));break;
    }
    return UACPI_STATUS_OK;
}
uacpi_status IoRegion(uacpi_region_op op,uacpi_handle data) {
    if(op==UACPI_REGION_OP_ATTACH || op==UACPI_REGION_OP_DETACH)return UACPI_STATUS_OK;
    if(op!=UACPI_REGION_OP_READ && op!=UACPI_REGION_OP_WRITE)return Reject("I/O operation");
    auto& rw=*static_cast<uacpi_region_rw_data*>(data);
    if(!Failed() && initializing && op==UACPI_REGION_OP_WRITE &&
       rw.address==SmiCommand && rw.byte_width==1 && rw.value==4) {
        outb(SmiCommand,4);
        return UACPI_STATUS_OK;
    }
    return Reject("AML I/O",rw.address,rw.byte_width);
}
uacpi_table_installation_disposition RejectDynamicTable(struct acpi_sdt_hdr*,uacpi_u64*) {
    Reject("dynamic ACPI table");
    return UACPI_TABLE_INSTALLATION_DISPOSITON_DENY;
}
bool Check(uacpi_status status,const char* stage) {
    if(status==UACPI_STATUS_OK && !Failed())return true;
    KLog("dell-acpi: %s failed (%s, access failure=%u)",
        stage,uacpi_status_to_string(status),Failed()?1u:0u);
    return false;
}
bool Initialize() {
    constexpr uint64_t flags=UACPI_FLAG_NO_ACPI_MODE | UACPI_FLAG_BAD_CSUM_FATAL |
        UACPI_FLAG_BAD_TBL_SIGNATURE_FATAL;
    uacpi_context_set_loop_timeout(2);
    if(!Check(uacpi_initialize(flags),"interpreter initialization") ||
       !Check(uacpi_set_table_installation_handler(RejectDynamicTable),"table policy"))return false;
    auto* root=uacpi_namespace_root();
    constexpr auto mem=UACPI_ADDRESS_SPACE_SYSTEM_MEMORY;
    constexpr auto io=UACPI_ADDRESS_SPACE_SYSTEM_IO;
    if(!Check(uacpi_uninstall_address_space_handler(root,mem),"default memory handler") ||
       !Check(uacpi_uninstall_address_space_handler(root,io),"default I/O handler") ||
       !Check(uacpi_install_address_space_handler(root,mem,MemoryRegion,nullptr),"restricted memory handler") ||
       !Check(uacpi_install_address_space_handler(root,io,IoRegion,nullptr),"restricted I/O handler") ||
       !Check(uacpi_namespace_load(),"firmware namespace") || !sciHandler)return false;
    // Event zero is reserved by pinned uACPI for its internal Global Lock.
    constexpr auto globalLockEvent=static_cast<uacpi_fixed_event>(0);
    uacpi_event_info info{};
    if(!Check(uacpi_fixed_event_info(globalLockEvent,&info),"firmware lock event") ||
       !(info&UACPI_EVENT_INFO_HW_ENABLED) || !(info&UACPI_EVENT_INFO_HAS_HANDLER))return false;
    // The callback and fixed event handlers are complete before first unmask.
    IoApicUnmaskIrq(sciPin,sciVector);
    irqEnabled=true;
    uacpi_namespace_node* controller=nullptr;
    if(!Check(uacpi_namespace_node_find(root,"\\_SB.PCI0.LPCB.ECDV",&controller),"EC device") ||
       !Check(uacpi_install_address_space_handler(controller,UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                    EcRegion,nullptr),"EC region registration and activation"))
        return false;
    uacpi_u64 direct=0;
    if(!Check(uacpi_eval_simple_integer(root,"\\ECRD",&direct),"EC ownership") ||
       direct!=1 || ecInitialized!=3)return false;
    initializing=false;
    return true;
}
bool EnterAcpiMode() {
    uint16_t control=inw(PowerControl);
    if(control==0xffff) {
        KLog("dell-acpi: PM1 control unavailable");
        return false;
    }
    if(control&SciEnable)return true;

    // The FADT command runs once on the BSP with SCI still masked. Fixed
    // events and AML remain untouched until firmware acknowledges ownership.
    KLog("dell-acpi: requesting ACPI mode");
    outb(SmiCommand,AcpiEnable);
    constexpr uint64_t timeoutMs=5000;
    const uint64_t start=ApicTickCount();
    for(unsigned poll=0;poll<=timeoutMs;++poll) {
        control=inw(PowerControl);
        if(control==0xffff)break;
        if(control&SciEnable) {
            KLog("dell-acpi: firmware enabled SCI");
            return true;
        }
        if(ApicTickCount()-start>=timeoutMs || poll==timeoutMs)break;
        SchedulerSleepMs(1);
    }
    KLog("dell-acpi: ACPI mode unavailable (PM1 control=%x)",unsigned(control));
    return false;
}
void Worker(void*) {
    if(!EnterAcpiMode())return;
    const uint16_t oldFixedEnable=inw(0x1802);
    uint8_t oldGpeEnable[16]{};
    for(unsigned i=0;i<16;++i)oldGpeEnable[i]=inb(uint16_t(0x1890+i));
    if(Initialize()) {
        auto last=AcPowerSource::Unknown;
        for(;;) {
            uacpi_u64 value=0;
            if(!Check(uacpi_eval_simple_integer(nullptr,"\\_SB.AC._PSR",&value),"AC source") || value>1)break;
            const auto source=value?AcPowerSource::Online:AcPowerSource::Offline;
            if(last!=source)KLog("dell-acpi: AC source %s",value?"online":"offline");
            last=source;
            ShutdownReportAcSource(source);
            SchedulerSleepMs(1000);
        }
    }
    // Keep initialized firmware objects alive. No new AML is evaluated after
    // a failed or uncertain initialization or transaction.
    if(irqEnabled){IoApicMaskIrq(sciPin);irqEnabled=false;}
    outw(0x1802,oldFixedEnable);
    for(unsigned i=0;i<16;++i)outb(uint16_t(0x1890+i),oldGpeEnable[i]);
    ShutdownReportAcSource(AcPowerSource::Unknown);
    KLog("dell-acpi: monitor stopped; AC status unavailable");
}

struct IoMapping {uint16_t base;uint16_t bytes;};
template<class T> uacpi_status IoPort(uacpi_handle handle,uacpi_size offset,uint16_t& port) {
    const auto* map=static_cast<const IoMapping*>(handle);
    if(!map || offset>map->bytes || sizeof(T)>map->bytes-offset)
        return Reject("fixed ACPI I/O bounds",offset,sizeof(T));
    port=uint16_t(map->base+offset);
    if(port&(sizeof(T)-1))return Reject("fixed ACPI I/O alignment",port,sizeof(T));
    return UACPI_STATUS_OK;
}
struct Event {uint32_t count;};
}
void DellAcpiMonitorConfigure() {
    if(!Requested())return;
    const auto& power=AcpiGetLegacyPowerInfo();
    const auto& route=AcpiGetSciRoute();
    const uint64_t admitted=AcpiAdmitDellMonitorFirmware();
    if(!admitted || power.pm1aControl!=PowerControl || power.pm1bControl ||
       power.smiCommand!=SmiCommand || power.acpiEnable!=AcpiEnable ||
       AcpiGetPmTimerInfo().port!=Timer || AcpiGetPmTimerInfo().wide) {
        KLog("dell-acpi: platform admission failed (firmware tables)");return;
    }
    if(PciConfigRead32(0,31,0,0)!=0x9cc38086u || (PciConfigRead32(0,31,0,0x40)&0xff80u)!=0x1800 ||
       !LpcDecodes(EcData) || !LpcDecodes(EcCommand)) {
        KLog("dell-acpi: platform admission failed (LPC decode)");return;
    }
    if(!WritableNvs({power.facs,64})){
        KLog("dell-acpi: platform admission failed (FACS mapping)");return;
    }
    for(const auto& span:readNvs)if(!WritableNvs(span)){
        KLog("dell-acpi: platform admission failed (firmware NVS at %lx)",span.base);return;
    }
    if(!IoApicSelectBootLevelGsi(route.gsi,route.activeLow,sciPin) ||
       !(sciVector=IoApicInstallMaskedBootHandler(sciPin,reinterpret_cast<void*>(Sci)))) {
        KLog("dell-acpi: SCI route unavailable");return;
    }
    rsdp=admitted;
}
void DellAcpiMonitorStart() {
    if(!rsdp)return;
    auto* worker=KernelThreadCreate("dell_acpi",Worker,nullptr,3);
    if(!worker){SerialPuts("dell-acpi: could not start worker\n");return;}
    worker->cpuAffinity=0;
    SchedulerAddProcess(worker);
}

extern "C" {
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out) {
    *out=rsdp;return rsdp?UACPI_STATUS_OK:UACPI_STATUS_NOT_FOUND;
}
void* uacpi_kernel_map(uacpi_phys_addr address,uacpi_size len) {
    if(!len || len>0xffffffffu || !AcpiFirmwareRange(address,uint32_t(len)))return UACPI_MAP_FAILED;
    return reinterpret_cast<void*>(address);
}
void uacpi_kernel_unmap(void*,uacpi_size){}
void uacpi_kernel_log(uacpi_log_level level,const char* message) {
    if(level==UACPI_LOG_ERROR)Reject("interpreter error");
    SerialPrintf("dell-acpi: %s",message);
}
void* uacpi_kernel_alloc(uacpi_size size){return size<=1024*1024?kmalloc(size):nullptr;}
void uacpi_kernel_free(void* p){if(p)kfree(p);}
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(){return ApicTickCount()*1000000;}
void uacpi_kernel_stall(uacpi_u8 us){Stall(us);}
void uacpi_kernel_sleep(uacpi_u64 ms) {
    if(ms>1000){Reject("AML sleep",ms);return;}
    if(ms)SchedulerSleepMs(uint32_t(ms));
}
uacpi_thread_id uacpi_kernel_get_thread_id(){return reinterpret_cast<uacpi_thread_id>(ProcessCurrent());}
uacpi_interrupt_state uacpi_kernel_disable_interrupts() {
    uint64_t flags=0;
    __asm__ volatile("pushfq; popq %0; cli":"=r"(flags)::"memory");
    return flags;
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state flags) {
    if(flags&0x200)__asm__ volatile("sti":::"memory");
}
uacpi_handle uacpi_kernel_create_mutex() {
    auto* mutex=static_cast<KMutex*>(kmalloc(sizeof(KMutex)));
    if(mutex)KMutexInit(mutex);
    return mutex;
}
void uacpi_kernel_free_mutex(uacpi_handle mutex){kfree(mutex);}
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle,uacpi_u16 timeout) {
    auto* mutex=static_cast<KMutex*>(handle);
    if(timeout==0xffff){KMutexLock(mutex);return UACPI_STATUS_OK;}
    const auto start=ApicTickCount();
    do {
        if(KMutexTryLock(mutex))return UACPI_STATUS_OK;
        if(!timeout)break;
        SchedulerSleepMs(1);
    } while(ApicTickCount()-start<timeout);
    return UACPI_STATUS_TIMEOUT;
}
void uacpi_kernel_release_mutex(uacpi_handle handle){KMutexUnlock(static_cast<KMutex*>(handle));}
uacpi_handle uacpi_kernel_create_event() {
    auto* event=static_cast<Event*>(kmalloc(sizeof(Event)));
    if(event)event->count=0;
    return event;
}
void uacpi_kernel_free_event(uacpi_handle handle){kfree(handle);}
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle,uacpi_u16 timeout) {
    auto& event=*static_cast<Event*>(handle);
    const auto start=ApicTickCount();
    do {
        uint32_t value=__atomic_load_n(&event.count,__ATOMIC_ACQUIRE);
        if(value && __atomic_compare_exchange_n(&event.count,&value,value-1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return true;
        if(!timeout)break;
        SchedulerSleepMs(1);
    } while(timeout==0xffff || ApicTickCount()-start<timeout);
    return false;
}
void uacpi_kernel_signal_event(uacpi_handle handle) {
    auto& event=*static_cast<Event*>(handle);
    uint32_t value=__atomic_load_n(&event.count,__ATOMIC_RELAXED);
    while(value!=~uint32_t{0} &&
          !__atomic_compare_exchange_n(&event.count,&value,value+1,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED)){}
}
void uacpi_kernel_reset_event(uacpi_handle handle) {
    __atomic_store_n(&static_cast<Event*>(handle)->count,0,__ATOMIC_RELEASE);
}
uacpi_handle uacpi_kernel_create_spinlock() {
    auto* lock=static_cast<IrqSpinLock*>(kmalloc(sizeof(IrqSpinLock)));
    if(lock)*lock={};
    return lock;
}
void uacpi_kernel_free_spinlock(uacpi_handle handle){kfree(handle);}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    return IrqSpinLockAcquire(static_cast<IrqSpinLock*>(handle));
}
void uacpi_kernel_unlock_spinlock(uacpi_handle handle,uacpi_cpu_flags flags) {
    IrqSpinLockRelease(static_cast<IrqSpinLock*>(handle),flags);
}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,uacpi_interrupt_handler handler,
                                                  uacpi_handle context,uacpi_handle* out) {
    if(irq!=AcpiGetLegacyPowerInfo().sci || !handler || !sciVector || sciHandler)
        return Reject("unexpected firmware interrupt",irq);
    sciHandler=handler;sciContext=context;*out=&sciHandler;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,uacpi_handle handle) {
    if(handle!=&sciHandler || handler!=sciHandler)return UACPI_STATUS_INVALID_ARGUMENT;
    if(irqEnabled){IoApicMaskIrq(sciPin);irqEnabled=false;}
    sciHandler=nullptr;sciContext=nullptr;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type,uacpi_work_handler,uacpi_handle) {
    // The narrow monitor registers no Notify consumers or general GPE methods.
    return Reject("unadmitted deferred AML work");
}
uacpi_status uacpi_kernel_wait_for_work_completion(){return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request*){return Reject("firmware request");}
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base,uacpi_size len,uacpi_handle* out) {
    if(!WithinAny(fixedIo,base,len))return Reject("fixed ACPI I/O map",base,len);
    auto* map=static_cast<IoMapping*>(kmalloc(sizeof(IoMapping)));
    if(!map)return UACPI_STATUS_OUT_OF_MEMORY;
    *map={uint16_t(base),uint16_t(len)};*out=map;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle handle){kfree(handle);}
#define BROOK_FIXED_IO(bits,type,input,output) \
uacpi_status uacpi_kernel_io_read##bits(uacpi_handle handle,uacpi_size offset,type* value) { \
    uint16_t port=0;const auto status=IoPort<type>(handle,offset,port); \
    if(status==UACPI_STATUS_OK)*value=input(port); \
    return status; \
} \
uacpi_status uacpi_kernel_io_write##bits(uacpi_handle handle,uacpi_size offset,type value) { \
    uint16_t port=0;const auto status=IoPort<type>(handle,offset,port); \
    if(status!=UACPI_STATUS_OK)return status; \
    if(port>=PowerControl && port<PowerControl+2 && \
       (port!=PowerControl || sizeof(type)!=2 || (value&(1u<<13)) || ((value^inw(PowerControl))&1))) \
        return Reject("PM1 control write",port,sizeof(type)); \
    output(port,value);return UACPI_STATUS_OK; \
}
BROOK_FIXED_IO(8,uacpi_u8,inb,outb)
BROOK_FIXED_IO(16,uacpi_u16,inw,outw)
BROOK_FIXED_IO(32,uacpi_u32,inl,outl)
#undef BROOK_FIXED_IO

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,uacpi_handle* out) {
    if(address.segment || address.bus ||
       !((address.device==0 && address.function==0) ||
         (address.device==31 && (address.function==0 || address.function==3))))
        return Reject("AML PCI function",address.device,address.function);
    *out=reinterpret_cast<uacpi_handle>(uintptr_t(address.device*8+address.function)+1);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle){}
#define BROOK_PCI_ACCESS(bits,type,read) \
uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle handle,uacpi_size offset,type* value) { \
    const auto bdf=uintptr_t(handle)-1; \
    const bool allowed=(bdf==0 && sizeof(type)==4 && (offset==0x48 || offset==0x60)) || \
        (bdf==248 && sizeof(type)==1 && ((offset>=0x40 && offset<=0x41) || \
            (offset>=0x48 && offset<=0x49) || (offset>=0xf1 && offset<=0xf3))) || \
        (bdf==251 && sizeof(type)==4 && offset==0x20); \
    if(Failed() || !initializing || !allowed) \
        return Reject("AML PCI read",offset,sizeof(type)); \
    *value=read(0,uint8_t(bdf/8),uint8_t(bdf%8),uint8_t(offset));return UACPI_STATUS_OK; \
} \
uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle,uacpi_size offset,type) { \
    return Reject("AML PCI write",offset,sizeof(type)); \
}
BROOK_PCI_ACCESS(8,uacpi_u8,PciConfigRead8)
BROOK_PCI_ACCESS(16,uacpi_u16,PciConfigRead16)
BROOK_PCI_ACCESS(32,uacpi_u32,PciConfigRead32)
#undef BROOK_PCI_ACCESS
#define BROOK_MMIO(bits,type) \
type uacpi_kernel_mmio_read##bits(void* pointer) { \
    const auto address=reinterpret_cast<uint64_t>(pointer); \
    if(!AcpiFirmwareRange(address,sizeof(type))) {Reject("ACPI fixed memory read",address,sizeof(type));return 0;} \
    return ReadMemory<type>(address); \
} \
void uacpi_kernel_mmio_write##bits(void* pointer,type value) { \
    const auto address=reinterpret_cast<uint64_t>(pointer); \
    if(address!=AcpiGetLegacyPowerInfo().facs+FacsLockOffset || sizeof(type)!=4) { \
        Reject("ACPI fixed memory write",address,sizeof(type));return; \
    } \
    WriteMemory(address,value); \
}
BROOK_MMIO(8,uacpi_u8)
BROOK_MMIO(16,uacpi_u16)
BROOK_MMIO(32,uacpi_u32)
BROOK_MMIO(64,uacpi_u64)
#undef BROOK_MMIO
}
}
