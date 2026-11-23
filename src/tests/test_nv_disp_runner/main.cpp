// Host test for nv_disp_runner.h: the gated live runner (verbs, boot-scoped
// preflight gate, pause/stop/continue, JSON step log, auto-abort with restore
// attempt, quiesce) driven through its Io adapter against the fake engine.
#include "nv_disp_runner.h"
#include "../test_nv_disp_plan/fake_disp.h"
using namespace brook::nvdisp;
using namespace fake_disp;

struct Backend{
    Fake& f;uint64_t t=0;uint32_t hooks=0;std::vector<std::string> lines;bool quiet=true;
    explicit Backend(Fake& fake):f(fake){}
    uint32_t rd(uint32_t a){t+=2;return f.rd(a);}
    void wr(uint32_t a,uint32_t v){t+=2;f.wr(a,v);}
    uint32_t vrd(uint64_t a){t+=2;return f.vrd(a);}
    void vwr(uint64_t a,uint32_t v){f.vwr(a,v);}
    void fill(uint64_t v,uint32_t b,uint32_t p,uint32_t c,uint32_t m,uint32_t l){f.fill(v,b,p,c,m,l);}
    void fence(){}
    uint64_t nowUs(){return t+=10;}
    void relax(){}
    void emit(const char* l){lines.push_back(l);if(!quiet)fputs(l,stdout);}
    void hookQuiesce(){++hooks;}
    size_t Count(const char* needle)const{size_t n=0;for(auto& l:lines)if(l.find(needle)!=std::string::npos)++n;return n;}
};
struct Rig{
    Fake f;Backend b{f};Runner<Backend>* r;
    Rig(){ // the runner's own buffers back the fake's blobs
        r=new Runner<Backend>();r->b=&b;r->base=GopFlipConfig(3840,2160,16384,c57e::FORMAT_A8R8G8B8,256ull<<20);
        f.img=&r->img;f.push=&r->pushes;f.Gop(r->base);
    }
    ~Rig(){delete r;}
    void GopState(){ // final state must equal what the GOP left
        assert(f.R(kRegInstTarget)==1&&f.R(kRegInstAddr)==0&&f.R(kRegPraminBase)==0x1fff0);
        assert((f.R(RegChnCtl(1))&0x6013)==0x01&&(f.R(RegChnStatus(1))&kWinStateMask)==kWinUnconnected);
        assert(f.R(RegPbLo(1))==1&&f.R(RegPbClient(1))==0);
        assert(f.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0))==kHandleGop&&f.R(RegArm(1,c57e::SET_OFFSET0))==0);
    }
};

// The kernel instance is a global with no .init_array: it must be constant-initialised.
#if defined(__clang__)
[[clang::require_constant_initialization]] static Runner<Backend> g_constInit{};
#endif
static void TestLayout(){
#if defined(__clang__)
    assert(g_constInit.b==nullptr&&!g_constInit.st.live&&g_constInit.plan.count==0);
#endif
    Config c=GopFlipConfig(3840,2160,16384,c57e::FORMAT_A8R8G8B8,256ull<<20);
    Config d=DellConfig();
    assert(c.surf[1].vram==d.surf[1].vram&&c.instVram==d.instVram&&c.wndwPushVram==d.wndwPushVram);
    assert(c.corePushVram==d.corePushVram&&c.syncVram==d.syncVram&&c.isoLimit==d.isoLimit);
}
static void TestGate(){
    Rig g;
    g.r->Command("noop");                                   // no preflight this boot
    assert(g.b.Count("\"nv_disp\":\"refused\"")==1&&g.f.written.empty());
    g.r->Command("plan flips 2");                           // touches nothing
    assert(g.f.written.empty()&&g.b.Count("\"list\":\"plan\"")==g.r->plan.count);
    g.r->Command("preflight");
    assert(g.r->st.preflightOk&&g.f.written.empty());       // preflight is read-only
    g.r->Command("bogus");assert(g.b.Count("\"nv_disp\":\"usage\"")==1);
    g.r->Command("flip 0 10");assert(g.b.Count("\"nv_disp\":\"usage\"")==2);
    g.r->Command("flip 2 9000");assert(g.b.Count("\"nv_disp\":\"usage\"")==3);
}
static void TestNoop(){
    Rig g;g.r->Command("preflight");g.r->Command("noop");
    assert(g.b.Count("\"nv_disp\":\"done\"")==1&&g.f.flips==2&&g.f.exceptions==0&&g.b.hooks==1);
    assert(!g.r->st.live&&!g.r->st.preflightOk);
    g.GopState();
    g.r->Command("noop");assert(g.b.Count("\"nv_disp\":\"refused\"")==1); // re-arm needs a new preflight
    // Every step logged once as JSON.
    for(auto& l:g.b.lines)assert(l.front()=='{'&&l.back()=='\n');
    assert(g.b.Count("\"list\":\"run\"")==g.r->plan.count);
}
static void TestObserverPauseAndContinue(){
    Rig g;g.r->Command("preflight");g.r->Command("noop pause");
    assert(g.r->st.paused&&g.r->st.live&&g.b.Count("\"nv_disp\":\"paused\"")==1);
    // Paused between window disconnect and instmem off: instmem still valid.
    assert(g.f.R(kRegInstTarget)==9&&(g.f.R(RegChnCtl(1))&0x13)==0x01);
    g.r->Command("noop");assert(g.b.Count("\"nv_disp\":\"refused\"")==1);  // cannot start while paused
    g.r->Command("continue");
    assert(!g.r->st.paused&&!g.r->st.live&&g.b.Count("\"nv_disp\":\"done\"")==1);
    g.GopState();
    g.r->Command("continue");assert(g.b.Count("nothing paused")==1);
}
static void TestStopThenQuiesce(){
    Rig g;g.r->Command("preflight");
    g.r->Build(Stage::A,0,0,false);
    const uint32_t stop=g.r->plan.phaseBegin[uint32_t(Phase::FirstFlip)]+3; // inside the first flip
    g.r->Command(("noop stop "+std::to_string(stop)).c_str());
    assert(g.r->st.paused&&g.r->st.cursor==stop&&g.r->st.live);
    g.r->Quiesce();                                        // pre-reset hook path
    assert(!g.r->st.live&&!g.r->st.paused&&g.r->st.aborts==1);
    assert(g.f.R(kRegInstTarget)==1&&(g.f.R(RegChnCtl(1))&0x6013)==0x01&&g.f.R(RegPbLo(1))==1);
    Rig h;h.r->Quiesce();assert(h.f.written.empty());    // nothing live: quiesce writes nothing
}
static void TestFlip(){
    Rig g;g.r->Command("preflight");g.r->Command("flip 2 100");
    assert(g.b.Count("\"nv_disp\":\"done\"")==1&&g.f.flips==4&&g.f.fills==1&&g.f.exceptions==0);
    // B filled with the marker on its first line, grey below.
    const uint64_t bv=g.r->cfg.surf[1].vram;
    assert(g.f.vrd(bv)==kFillMarker&&g.f.vrd(bv+uint64_t(kFillMarkerLines)*16384)==kFillGrey);
    g.GopState();
}
static void TestAutoAbortAtConnect(){
    Rig g;g.f.idleOnConnect=false;g.f.R(RegChnStatus(1))=0x02010100;
    g.r->Command("preflight");g.r->Command("noop");
    assert(g.r->st.aborts==1&&g.b.Count("\"nv_disp\":\"failed\"")==1&&!g.r->st.live);
    assert(g.f.flips==0&&g.f.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0))==kHandleGop);
    assert(g.f.R(kRegInstTarget)==1&&(g.f.R(RegChnCtl(1))&0x6013)==0x01);
    assert(g.b.Count("final window CHNSTATUS")==1);        // the abort's closing LOGs
}
static void TestRestoreAttemptOnFlipFailure(){
    Rig g;g.r->Command("preflight");
    g.f.failUpdateAt=2;                                    // first flip + B ok, the flip back to A faults
    g.r->Command("flip 2 0");
    assert(g.f.exceptions==1&&g.r->st.aborts==1&&g.b.Count("\"list\":\"restore\"")>=1);
    assert(g.b.Count("nv_disp_exc")==1);                   // logged by the restore path's ack
    // The restore submission put the GOP surface back before the abort list ran.
    assert(g.f.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0))==kHandleGop&&g.f.R(RegArm(1,c57e::SET_OFFSET0))==0);
    assert(g.f.R(kRegInstTarget)==1&&(g.f.R(RegChnCtl(1))&0x6013)==0x01);
}
static void TestStageB(){
    Rig g;g.r->Command("preflight");g.r->Command("noop b");
    assert(g.b.Count("\"nv_disp\":\"WARNING\"")==1&&g.b.Count("head1 (step 4) will refuse")==1);
    assert(g.b.Count("\"nv_disp\":\"done\"")==1&&g.f.flips==2);
    assert((g.f.R(RegChnCtl(0))&0x6013)==0x01);            // stage B leaves the core ALLOCATE only
}
static void TestDumpAndBigFlip(){
    Rig g;g.r->Command("preflight");g.r->Command("noop dump-after-connect");
    assert(g.b.Count("\"nv_disp\":\"done\"")==1&&g.b.Count("nv_disp_dump")==2*0x180/8);
    assert(g.b.Count("nv_disp_diff")==3);                  // connect reset 0x22c/0x2ec/0x460, all rewritten
    Rig h;h.r->Command("preflight");h.r->Command("flip 16 0");
    assert(h.b.Count("\"nv_disp\":\"done\"")==1&&h.f.flips==18&&h.b.Count("nv_disp_dump")==0);
    h.GopState();
}
static void TestAbortVerb(){
    Rig g;g.r->Command("abort");assert(g.b.Count("nothing live")==1&&g.f.written.empty());
    g.r->Command("preflight");g.r->Command("noop pause");g.r->Command("abort");
    assert(!g.r->st.live&&g.r->st.aborts==1);
    assert(g.f.R(kRegInstTarget)==1&&(g.f.R(RegChnCtl(1))&0x6013)==0x01);
    g.r->Command("status");assert(g.b.Count("nv_disp_status")>=1);
}
// flip_4_1000: GET stalls inside the 4th flip. Diagnostics run, the restore is kicked and
// waited for (fetched and latched on ARM) before the abort trashes the channel, so the
// GOP handle is back on ARM and only then is instmem invalidated.
static void TestGetStallRestoreLands(){
    Rig g;g.r->Command("plan flips 4");const uint32_t at=g.r->pushes.end[3]+12;
    g.f.stallAt=at;g.r->Command("preflight");g.r->Command("flip 4 10");
    assert(g.r->st.aborts==1&&g.b.Count("\"list\":\"diag\"")>10&&g.b.Count("pushbuffer word in VRAM")==(g.r->pushes.end[4]-g.r->pushes.end[3])/4);
    assert(g.b.Count("\"list\":\"restore\"")>=5);
    g.GopState();
    assert(g.f.R(RegArm(1,c57e::SET_CONTEXT_DMA_NOTIFIER))==0);
}
// Same stall, but the channel never moves again: the restore cannot land, ARM still
// references our ISO handle, so instmem is left valid (no invalidation under a live reference).
static void TestGetStallRestoreStuck(){
    Rig g;g.r->Command("plan flips 4");g.f.stallAt=g.r->pushes.end[3]+12;g.f.stallSticky=true;
    g.r->Command("preflight");g.r->Command("flip 4 10");
    assert(g.r->st.aborts==1&&g.f.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0))==kHandleIsoPitch);
    assert(g.f.R(kRegInstTarget)==9&&g.f.R(kRegInstAddr)!=0);                 // instmem kept valid
    assert(g.b.Count("\"op\":\"WRITEIF\"")>=2);
}
int main(){
    TestLayout();TestGate();TestNoop();TestObserverPauseAndContinue();TestStopThenQuiesce();
    TestFlip();TestAutoAbortAtConnect();TestRestoreAttemptOnFlipFailure();TestStageB();TestDumpAndBigFlip();TestAbortVerb();TestGetStallRestoreLands();TestGetStallRestoreStuck();
    puts("test_nv_disp_runner: ok");return 0;
}
