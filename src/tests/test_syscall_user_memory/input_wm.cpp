// H-01/H-02: Brook input and WM syscalls must reach user memory only through
// the checked copies. Real handlers from syscall_brook_system.cpp and
// syscall_brook_wm.cpp; the window manager underneath is a fake.
#include "harness.h"
#include "syscall_brook_system.cpp"
#include "syscall_brook_wm.cpp"

using namespace harness;
namespace {
struct FakeWm {
    bool exists=false;
    unsigned creates=0,destroys=0,titles=0,resizes=0;
    char title[64]{};
    brook::Window::WmInputEvent queue[300]{};
    uint32_t head=0,tail=0;
    void Reset() { *this=FakeWm{}; }
    void Push(uint32_t n) { for(uint32_t i=0;i<n;++i) { queue[head].type=static_cast<uint8_t>(i+1);queue[head].x=static_cast<int16_t>(i);++head; } }
} g_wm;
uint32_t g_inputPopped;
}

extern "C" void MouseGetPosition(int32_t* x,int32_t* y) { *x=100;*y=50; }
namespace brook {
WmCreateWindowResult WmCreateWindowForProcess(Process*,uint16_t,uint16_t,const char* title,bool,bool) {
    ++g_wm.creates;g_wm.exists=true;
    if(title)for(int i=0;i<64;++i)g_wm.title[i]=title[i];
    WmCreateWindowResult result{};result.wmId=3;result.vfbStride=4096;
    result.vfbUser=reinterpret_cast<void*>(0x40000000);return result;
}
void WmSetClientSideDecoration(int,bool,bool,uint16_t,uint16_t) {}
void WmDestroyWindowById(Process*,uint32_t) { ++g_wm.destroys;g_wm.exists=false; }
void WmSetTitleById(Process*,uint32_t,const char* title) { ++g_wm.titles;for(int i=0;i<64;++i)g_wm.title[i]=title[i]; }
int WmResizeVfbForProcess(Process*,uint32_t wmId,uint16_t,uint16_t,WmCreateWindowResult* out) {
    ++g_wm.resizes;out->wmId=wmId;out->vfbStride=8192;out->vfbUser=reinterpret_cast<void*>(0x50000000);return 0;
}
Window* WmFindWindowById(Process*,uint32_t) { return g_wm.exists?reinterpret_cast<Window*>(&g_wm):nullptr; }
// Only the pre-fix handler drains the window directly.
__attribute__((weak)) uint32_t WmInputPop(Window*,Window::WmInputEvent* out,uint32_t max) {
    uint32_t n=0;while(n<max && g_wm.tail!=g_wm.head)out[n++]=g_wm.queue[g_wm.tail++];
    return n;
}
int WmInputPopById(Process*,uint32_t,Window::WmInputEvent* out,uint32_t max) {
    if(!g_wm.exists)return -2;
    uint32_t n=0;while(n<max && g_wm.tail!=g_wm.head)out[n++]=g_wm.queue[g_wm.tail++];
    return static_cast<int>(n);
}
volatile uint64_t g_profOpenCount,g_profMmapCount,g_profFaultCount,g_profCacheHit,g_profCacheMiss,g_profPreloadPages;
volatile uint64_t g_blockCacheHits,g_blockCacheMisses;
}

static void PushInput(brook::Process* proc,uint32_t n) {
    for(uint32_t i=0;i<n;++i) {
        brook::InputEvent event{};event.type=brook::InputEventType::KeyPress;event.scanCode=static_cast<uint8_t>(i+1);
        brook::ProcessInputPush(proc,event);
    }
}
static uint32_t QueuedInput(brook::Process* proc) {
    return (proc->inputHead+brook::Process::INPUT_QUEUE_SIZE-proc->inputTail)%brook::Process::INPUT_QUEUE_SIZE;
}

static void InputPop() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    PushInput(proc,5);
    // Kernel destination: refused, nothing written, no event lost.
    CHECK(brook::sys_brook_input_pop(g_kernel.Address(),4,0,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(QueuedInput(proc)==5);
    // A buffer whose tail is unmapped is refused before popping.
    Protect(PageSize,PageSize,Access::None);
    CHECK(brook::sys_brook_input_pop(UserAddress(PageSize-32),4,0,0,0,0)==-brook::EFAULT);
    CHECK(QueuedInput(proc)==5);
    // Read-only destination is not writable.
    Protect(2*PageSize,PageSize,Access::Read);
    CHECK(brook::sys_brook_input_pop(UserAddress(2*PageSize),4,0,0,0,0)==-brook::EFAULT);
    CHECK(QueuedInput(proc)==5);
    // A valid buffer receives the encoded events.
    const uint64_t out=UserAddress(4*PageSize);
    CHECK(brook::sys_brook_input_pop(out,8,0,0,0,0)==5);
    auto* bytes=reinterpret_cast<const uint8_t*>(out);
    CHECK(bytes[0]==static_cast<uint8_t>(brook::InputEventType::KeyPress) && bytes[1]==1);
    CHECK(bytes[4*16+1]==5);
    CHECK(QueuedInput(proc)==0);
    // More events than one staging batch.
    PushInput(proc,40);
    CHECK(brook::sys_brook_input_pop(out,256,0,0,0,0)==40);
    CHECK(bytes[39*16+1]==40);
}

static void CreateWindow() {
    Reset();ResetProcess();g_wm.Reset();
    const uint64_t out=UserAddress(0),title=UserAddress(PageSize);
    // Kernel title pointer: no window, nothing disclosed.
    CHECK(brook::sys_brook_wm_create_window(64,64,g_kernel.Address(),out,0,0)==-brook::EFAULT);
    CHECK(g_wm.creates==0);
    // Kernel result pointer: refused before creating a window.
    CHECK(brook::sys_brook_wm_create_window(64,64,0,g_kernel.Address(),0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(g_wm.creates==0);
    // A valid call writes the result.
    __builtin_memcpy(reinterpret_cast<void*>(title),"term",5);
    CHECK(brook::sys_brook_wm_create_window(64,64,title,out,0,0)==0);
    CHECK(g_wm.creates==1 && g_wm.exists);
    CHECK(brook::StrEq(g_wm.title,"term"));
    struct { uint32_t id,stride;uint64_t user; } result{};
    __builtin_memcpy(&result,reinterpret_cast<void*>(out),sizeof(result));
    CHECK(result.id==3 && result.stride==4096 && result.user==0x40000000);
}

static void SetTitle() {
    Reset();ResetProcess();g_wm.Reset();g_wm.exists=true;
    CHECK(brook::sys_brook_wm_set_title(3,g_kernel.Address(),0,0,0,0)==-brook::EFAULT);
    CHECK(g_wm.titles==0);
    // A title running into an unmapped page faults rather than reading on.
    Protect(PageSize,PageSize,Access::None);
    char* edge=reinterpret_cast<char*>(UserAddress(PageSize-4));
    __builtin_memcpy(edge,"abcd",4);
    CHECK(brook::sys_brook_wm_set_title(3,UserAddress(PageSize-4),0,0,0,0)==-brook::EFAULT);
    CHECK(g_wm.titles==0);
    // 63, 64 and 65 byte titles keep at most 63 bytes, always terminated.
    const uint32_t lengths[]={63u,64u,65u};
    for(uint32_t length:lengths) {
        char* text=reinterpret_cast<char*>(UserAddress(2*PageSize));
        for(uint32_t i=0;i<length;++i)text[i]='a'+static_cast<char>(i%26);
        text[length]=0;
        CHECK(brook::sys_brook_wm_set_title(3,UserAddress(2*PageSize),0,0,0,0)==0);
        uint32_t stored=0;while(stored<64 && g_wm.title[stored])++stored;
        CHECK(stored==63);
    }
}

static void PopInput() {
    Reset();ResetProcess();g_wm.Reset();g_wm.exists=true;g_wm.Push(10);
    CHECK(brook::sys_brook_wm_pop_input(3,g_kernel.Address(),10,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(g_wm.head-g_wm.tail==10);
    const uint64_t out=UserAddress(0);
    CHECK(brook::sys_brook_wm_pop_input(3,out,4,0,0,0)==4);
    CHECK(reinterpret_cast<const brook::Window::WmInputEvent*>(out)[3].type==4);
    // More events than one staging batch.
    g_wm.Push(200);
    CHECK(brook::sys_brook_wm_pop_input(3,out,256,0,0,0)==206);
    g_wm.exists=false;
    CHECK(brook::sys_brook_wm_pop_input(3,out,4,0,0,0)==-brook::ENOENT);
}

static void ResizeVfb() {
    Reset();ResetProcess();g_wm.Reset();g_wm.exists=true;
    CHECK(brook::sys_brook_wm_resize_vfb(3,32,32,g_kernel.Address(),0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(g_wm.resizes==0);
    CHECK(brook::sys_brook_wm_resize_vfb(3,32,32,UserAddress(0),0,0)==0);
    CHECK(reinterpret_cast<const uint32_t*>(UserAddress(0))[1]==8192);
}

void RunInputWmTests() {
    InputPop();
    CreateWindow();
    SetTitle();
    PopInput();
    ResizeVfb();
}
