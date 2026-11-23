// Host test for nv_disp_driver.h / nv_disp_flipper.h (RTX 2080 resident display
// driver, milestone D1): both outputs brought up and torn down through the quiet
// runners, flips through the flipper (ring wrap, mailbox, watchdog), shared instmem,
// failure fallbacks and the head-0 invariant check, against the shared fake engine.
#include "../test_nv_disp_head/head_fake.h"
#include "nv_disp_driver.h"
static Rom g_rom;

struct DRig{
    Core c;HBackend b{c};Runner<HBackend>* r0;HeadRunner<HBackend>* r1;NvDisplayDriver<HBackend>* d;
    DRig(){
        g_rom=MakeRom();
        r0=new Runner<HBackend>();r0->b=&b;r0->base=GopFlipConfig(3840,2160,16384,0xcf,256ull<<20);
        r1=new HeadRunner<HBackend>();r1->b=&b;r1->vb=g_rom.V();r1->cfg.base=r0->base;
        assert(ResolveHeadConfig(r1->vb,4,r1->cfg,r1->dpout));r1->resolved=true;
        c.cfg=r1->cfg;c.vb=&r1->vb;c.dpout=r1->dpout;c.img=&r1->img;c.hp=&r1->pushes;c.Gop();
        b.s3busy=true;                                                  // the driver must coexist anyway
        d=new NvDisplayDriver<HBackend>();d->b=&b;d->r0=r0;d->r1=r1;
    }
    ~DRig(){delete d;delete r1;delete r0;}
    uint32_t Arm(uint32_t ch,uint32_t m){return c.R(RegArm(ch,m));}
    void Gop0(){ // head 0 / window 0 as the GOP left it
        assert(Arm(1,c57e::SET_CONTEXT_DMA_ISO0)==kHandleGop&&Arm(1,c57e::SET_OFFSET0)==0);
        assert((c.R(RegChnCtl(1))&0x13)==0x01&&c.R(RegPbLo(1))==1);
        assert(c.R(RegArm(kChidCore,c57d::HeadRasterSize(0)))==r0->base.gop.headRaster);}
    void Flip(uint32_t o,uint32_t n){for(uint32_t i=0;i<n;++i){const int8_t bb=d->Back(o);assert(bb>=0&&d->Present(o,bb)>=0);
        for(int k=0;k<100&&(d->f[o].state==FlipState::Armed||d->f[o].queued>=0);++k)d->Tick();
        assert(d->f[o].state==FlipState::Idle&&d->f[o].front==bb);}}
};
static uint32_t Errors(const HBackend& b){return uint32_t(b.Count("\"ok\":0"));}

static void TestOutput0(){
    DRig g;
    assert(g.d->Up0()&&g.d->Live(0)&&g.d->f[0].front==0);
    assert(g.Arm(1,c57e::SET_CONTEXT_DMA_ISO0)==kHandleIsoPitch&&g.Arm(1,c57e::SET_OFFSET0)==0); // first flip: no visible change
    assert(g.c.R(kRegInstTarget)==9&&Errors(g.b)==0&&g.r0->quietSteps>50);
    assert(g.b.Count("nv_disp_step")==0);                               // quiet: no per-step JSON
    g.Flip(0,1);assert(g.Arm(1,c57e::SET_OFFSET0)==uint32_t(g.r0->cfg.surf[1].vram>>8));
    g.Flip(0,200);                                                      // ring wraps (16 words per flip)
    assert(g.d->f[0].st.completes==201&&g.d->f[0].st.wraps>=3&&g.d->f[0].fail==FlipFail::None);
    // mailbox: a second present while armed replaces the queued one
    g.c.wstall.insert(1);g.d->f[0].paced=false;
    assert(g.d->Present(0,g.d->Back(0))==0&&g.d->Present(0,0)==1&&g.d->Present(0,1)==1&&g.d->f[0].st.replaced==1);
    g.c.wstall.erase(1);g.c.ExecuteW(1,g.d->f[0].put);                // the engine catches up
    for(int k=0;k<10;++k)g.d->Tick();
    assert(g.d->f[0].state==FlipState::Idle&&g.d->f[0].front==1&&g.d->f[0].queued==-1);g.d->f[0].paced=true;
    assert(g.d->Down0()&&!g.d->Live(0));
    g.Gop0();assert(g.c.R(kRegInstTarget)==1&&g.c.R(kRegInstAddr)==0&&!g.r0->st.live);
    assert(g.Arm(1,c57e::SET_CONTEXT_DMA_NOTIFIER)==0);
    // up again (the runner re-preflights)
    assert(g.d->Up0()&&g.d->Down0());g.Gop0();
}
static void TestBothOutputs(){
    for(int order=0;order<2;++order){
        DRig g;
        if(order==0){assert(g.d->Up0());assert(g.d->Up1());}else{assert(g.d->Up1());assert(g.d->Up0());}
        assert(g.d->Live(0)&&g.d->Live(1)&&!memcmp(g.r0->img.words,g.r1->img.words,sizeof(g.r0->img.words))); // one shared image
        assert(g.Arm(3,c57e::SET_CONTEXT_DMA_ISO0)==kHandleIsoPitch&&g.c.R(0x689100)==1&&g.c.R(0x689000)==0);
        g.Flip(0,5);g.Flip(1,5);
        assert(g.Arm(3,c57e::SET_OFFSET0)==uint32_t(g.d->f[1].surf[g.d->f[1].front].vram>>8));
        assert(g.d->f[1].surf[1].vram==0x6600000&&g.d->f[1].surf[0].vram==0x4400000);
        // head 1 down first keeps instmem (window 0 live); head 0 keeps flipping
        assert(g.d->Down1()&&!g.d->Live(1)&&g.c.R(kRegInstTarget)==9&&g.Arm(3,c57e::SET_CONTEXT_DMA_ISO0)==0);
        assert((g.c.R(RegChnCtl(3))&0x13)==0&&(g.c.R(0x688340)&0xff)==0);
        g.Flip(0,3);
        assert(g.d->Down0());g.Gop0();assert(g.c.R(kRegInstTarget)==1);
        assert(g.r1->st.guardHits==0&&Errors(g.b)==0);
    }
    {DRig g;assert(g.d->Up0()&&g.d->Up1());
     assert(g.d->Down0()&&g.c.R(kRegInstTarget)==9);                   // head 0 down first: core live, instmem kept
     g.Gop0();g.Flip(1,3);
     assert(g.d->Down1()&&g.c.R(kRegInstTarget)==1&&(g.c.R(RegChnCtl(0))&0x13)==0x01);}
    {DRig g;assert(g.d->Up0()&&g.d->Up1());g.d->Quiesce();             // pre-reset: both down, GOP state
     assert(!g.d->AnyLive());g.Gop0();assert(g.c.R(kRegInstTarget)==1&&(g.c.R(RegChnCtl(3))&0x13)==0);}
    {DRig g;g.d->Command("up both");assert(g.d->AnyLive()&&g.b.Count("\"nv_driver_status\"")==1);
     g.d->Command("bench 0 30");assert(g.b.Count("\"completed\":30")==1);
     g.d->Command("bench 1 30");assert(g.b.Count("\"completed\":30")==2);
     g.d->Command("flip 9 0");assert(g.b.Count("\"usage\"")==1);
     g.d->Command("down all");assert(!g.d->AnyLive());g.Gop0();
     for(auto& l:g.b.lines)assert(l.size()<318&&l.back()=='\n');              // no truncated status lines
     assert(g.b.Count("\"nv_driver_out\":1")>=1);}
}
static void TestFailures(){
    // A wedged window 0: the flip times out, the restore cannot land, the abort runs and
    // instmem stays valid under the live reference (guard).
    {DRig g;assert(g.d->Up0());g.Flip(0,1);g.d->f[0].paced=false;g.c.wstall.insert(1);
     assert(g.d->Present(0,g.d->Back(0))==0);
     for(int k=0;k<200000&&g.d->Live(0);++k)g.d->Tick();
     assert(!g.d->Live(0)&&g.d->out[0]==OutState::Failed&&g.d->f[0].fail==FlipFail::Timeout&&g.d->failures[0]==1);
     assert(g.b.Count("\"nv_driver\":\"flip-failed\"")==1&&!g.r0->st.live&&g.c.R(kRegInstTarget)==9);
     assert(g.d->Down0()&&g.d->out[0]==OutState::Off);}             // a failed output can be cleared
    // A transient wedge: the flip fails but the restore lands: head 0 back on the GOP surface.
    {DRig g;assert(g.d->Up0());g.Flip(0,1);g.d->f[0].paced=false;g.c.wstall.insert(1);
     assert(g.d->Present(0,g.d->Back(0))==0);
     for(int k=0;k<200000&&g.d->f[0].state==FlipState::Armed;++k){g.d->f[0].TickVblank();(void)g.d->f[0].Poll();}
     g.c.wstall.erase(1);g.c.ExecuteW(1,g.d->f[0].put);g.d->Tick();
     assert(g.d->out[0]==OutState::Failed);g.Gop0();assert(g.c.R(kRegInstTarget)==1);}
    // Exception on a head-1 flip: output 1 goes down, output 0 keeps flipping.
    {DRig g;assert(g.d->Up0()&&g.d->Up1());g.d->f[1].paced=false;g.c.wexcept=true;
     assert(g.d->Present(1,g.d->Back(1))==0);g.d->Tick();
     assert(g.d->out[1]==OutState::Failed&&g.d->f[1].fail==FlipFail::Exception&&g.d->Live(0));
     g.c.wexcept=false;g.Flip(0,2);assert(g.d->Down0());g.Gop0();}
    // Head-0 invariant changed under us: everything goes down.
    {DRig g;assert(g.d->Up0()&&g.d->Up1());g.c.R(RegArm(kChidCore,c57d::HeadPixelClock(0)))^=1;g.b.t+=2000000;g.d->Tick();
     assert(!g.d->AnyLive()&&g.d->invariantFails==1);}
    // The flipper refuses writes outside its channel.
    {DRig g;assert(g.d->Up0());const size_t n=g.c.written.size();g.d->f[0].Wr(RegPut(3),4);
     assert(g.d->f[0].state==FlipState::Failed&&g.d->f[0].fail==FlipFail::Guard&&g.c.written.size()==n);
     g.d->Tick();assert(g.d->out[0]==OutState::Failed);}
    // Preflight refused (instmem owned by someone else): nothing written.
    {DRig g;g.c.R(kRegInstTarget)=9;g.c.R(kRegInstAddr)=0x1234;assert(!g.d->Up0()&&g.c.written.empty()&&g.d->out[0]==OutState::Failed);}
}
// Pacing: at most one completed flip per scanned frame; completions record the scanline.
static void TestPacing(){
    DRig g;assert(g.d->Up0());
    g.Flip(0,40);
    const auto& st=g.d->f[0].st;
    assert(st.completes==40&&st.frames>=39&&st.paced>0&&st.latchBlank+st.latchActive==40);
    g.d->f[0].paced=false;const uint64_t fr=st.frames;g.Flip(0,40);   // unpaced: no per-frame gate
    assert(st.completes==80&&st.frames-fr<40);
    g.d->Command("interval 0 1");assert(g.d->f[0].interval==1);g.Flip(0,1);
    assert(g.c.R(RegArm(1,c57e::SET_PRESENT_CONTROL))==1);
    g.d->Command("interval 0 0");g.Flip(0,1);
    assert(g.d->Down0());
}
// D2 age repair: B is stale at bring-up (full copy); afterwards each buffer gets this
// frame's damage plus the previous frame's.
static void TestAgeRepair(){
    AgeRepair a;a.Reset();std::vector<brook::DamageRect> got;
    auto cp=[&](const brook::DamageRect& r){got.push_back(r);};
    brook::DamageRects d1;d1.Add({10,10,20,20});
    assert(a.Apply(1,d1,3840,2160,cp)==1&&got[0]==(brook::DamageRect{0,0,3840,2160}));got.clear();   // B stale: full
    brook::DamageRects d2;d2.Add({100,100,110,110});
    assert(a.Apply(0,d2,3840,2160,cp)==2&&got[0]==d2.rects[0]&&got[1]==d1.rects[0]);got.clear();      // A: d2 + d1
    brook::DamageRects d3;d3.Add({5,5,6,6});
    assert(a.Apply(1,d3,3840,2160,cp)==2&&got[0]==d3.rects[0]&&got[1]==d2.rects[0]);got.clear();      // B: d3 + d2
    const brook::DamageRects f=brook::DamageRects::Full(3840,2160);
    assert(a.Apply(0,f,3840,2160,cp)==1);got.clear();
    assert(a.Apply(9,d1,3840,2160,cp)==0);
}
static void TestVblankCounter(){
    DRig g;assert(g.d->Up0());
    g.c.R(0x611800)=0x4;g.d->Tick();assert(g.d->f[0].st.vblanks==1&&g.c.R(0x611800)==0);
    g.c.R(0x611800)=0x3;g.d->Tick();assert(g.d->f[0].st.vblanks==1&&g.c.R(0x611800)==0x3); // only VBLANK is acked
    assert(g.d->Down0());
}
int main(){
    TestOutput0();TestBothOutputs();TestFailures();TestPacing();TestAgeRepair();TestVblankCounter();
    puts("test_nv_disp_driver: ok");return 0;
}
