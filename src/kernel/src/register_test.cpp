#include "register_test.h"
#include "irq_wrapper.h"
#include "process.h"
#include "serial.h"
#include "string.h"

extern "C" {
uint64_t kr_seed[15],kr_actual[17],kr_expected_rsp,kr_actual_guard[4];
uint64_t kr_inner_rsp,kr_inner_flags,kr_arrivals;
alignas(16) unsigned char kr_seed_fx[512],kr_actual_fx[512];
void kr_probe_irq(),kr_probe_ud(),kr_probe_gp(),kr_probe_pf(),kr_clobber();
extern const char kr_ud_instruction[],kr_ud_resume[],kr_gp_instruction[],kr_gp_resume[];
extern const char kr_pf_instruction[],kr_pf_resume[];
IRQ_NAKED_HANDLER(RegisterTestIrqWrapper,kr_clobber)
void (*kr_irq_target)()=RegisterTestIrqWrapper;
}

namespace brook {
namespace {
uint32_t busy;
Process* owner;
unsigned selected;
bool frameBad;
constexpr uint64_t badAddress=0xffffff0000000000ULL;
constexpr uint64_t flagsMask=0xcd7;
const char* names[]={"rax","rbx","rcx","rdx","rsi","rdi","rbp","r8","r9","r10","r11","r12","r13","r14","r15"};
void Seed() {
    for(unsigned i=0;i<15;++i)kr_seed[i]=0x1234567800000100ULL+i*17;
    memset(kr_seed_fx,0,sizeof(kr_seed_fx));
    const uint16_t fcw=0x077f;const uint32_t mxcsr=0x3f80;
    memcpy(kr_seed_fx,&fcw,2);memcpy(kr_seed_fx+24,&mxcsr,4);
    for(unsigned i=0;i<256;++i)kr_seed_fx[160+i]=static_cast<unsigned char>(i*13+37);
    kr_arrivals=kr_inner_rsp=kr_inner_flags=0;frameBad=false;
}
bool Check(const char* name) {
    bool ok=!frameBad;
    for(unsigned i=0;i<15;++i)if(kr_actual[i]!=kr_seed[i]) {
        SerialPrintf("KREGISTER FAIL %s %s expected=%lx actual=%lx\n",name,names[i],kr_seed[i],kr_actual[i]);ok=false;
    }
    if(kr_actual[15]!=kr_expected_rsp || (kr_actual[16]&flagsMask)!=flagsMask)ok=false;
    for(unsigned i=0;i<4;++i)if(kr_actual_guard[i]!=0x35792468)ok=false;
    if(kr_arrivals!=1 || (kr_inner_rsp&15)!=8 || (kr_inner_flags&0x600))ok=false;
    if(memcmp(kr_seed_fx,kr_actual_fx,2) || memcmp(kr_seed_fx+24,kr_actual_fx+24,4) ||
       memcmp(kr_seed_fx+160,kr_actual_fx+160,256))ok=false;
    SerialPrintf("KREGISTER %s %s rsp=%lx expected=%lx inner=%lx flags=%lx arrivals=%lu frame_bad=%u\n",
        name,ok?"PASS":"FAIL",kr_actual[15],kr_expected_rsp,kr_inner_rsp,kr_actual[16],kr_arrivals,frameBad?1:0);
    return ok;
}
}

bool RegisterTestException(FullExceptionFrame* frame,uint64_t vector,uint64_t address) {
    // Exact owned instruction only: never recover another task's kernel fault.
    auto* actor=__atomic_load_n(&owner,__ATOMIC_ACQUIRE);
    if(!actor || actor!=ProcessCurrent() || (frame->cs&3))return false;
    uint64_t instruction=0,resume=0,error=0,expectedVector=0;
    if(selected==1) {instruction=reinterpret_cast<uint64_t>(kr_ud_instruction);resume=reinterpret_cast<uint64_t>(kr_ud_resume);expectedVector=6;}
    if(selected==2) {instruction=reinterpret_cast<uint64_t>(kr_gp_instruction);resume=reinterpret_cast<uint64_t>(kr_gp_resume);expectedVector=13;error=0x7ff8;}
    if(selected==3) {instruction=reinterpret_cast<uint64_t>(kr_pf_instruction);resume=reinterpret_cast<uint64_t>(kr_pf_resume);expectedVector=14;error=2;}
    if(!instruction || frame->rip!=instruction || vector!=expectedVector)return false;
    const uint64_t values[]={frame->rax,frame->rbx,frame->rcx,frame->rdx,frame->rsi,frame->rdi,frame->rbp,
        frame->r8,frame->r9,frame->r10,frame->r11,frame->r12,frame->r13,frame->r14,frame->r15};
    if(memcmp(values,kr_seed,sizeof(values)) || frame->rsp!=kr_expected_rsp ||
       (frame->rflags&flagsMask)!=flagsMask || frame->errorCode!=error ||
       (vector==14 && address!=badAddress))frameBad=true;
    kr_clobber();
    frame->rip=resume;
    return true;
}

int64_t RegisterTestControl(uint64_t action,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {
    auto* actor=ProcessCurrent();if(!actor || actor->euid!=0)return -1;
    if(action)return -22;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&busy,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return -16;
    uint64_t originalFlags;
    __asm__ volatile("pushfq; popq %0; cli":"=r"(originalFlags)::"memory");
    // No allocation, sleeping lock or asynchronous maskable delivery while the
    // shared snapshots are live. The whole batch is four bounded local traps.
    alignas(64) unsigned char saved[1152]{};
    __asm__ volatile("xsave64 %0":"=m"(saved):"a"(7),"d"(0):"memory");
    __atomic_store_n(&owner,actor,__ATOMIC_RELEASE);
    unsigned passed=0;
    Seed();selected=0;kr_probe_irq();passed+=Check("synthetic-irq-wrapper");
    Seed();selected=1;kr_probe_ud();passed+=Check("hardware-UD");
    Seed();selected=2;kr_seed[0]=0x7ff8;kr_probe_gp();passed+=Check("hardware-GP");
    Seed();selected=3;kr_seed[5]=badAddress;kr_probe_pf();passed+=Check("hardware-PF");
    __atomic_store_n(&owner,static_cast<Process*>(nullptr),__ATOMIC_RELEASE);selected=0;
    __asm__ volatile("xrstor64 %0"::"m"(saved),"a"(7),"d"(0):"memory");
    __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
    if(originalFlags&512)__asm__ volatile("sti":::"memory");
    return passed==4?4:-5;
}
}
