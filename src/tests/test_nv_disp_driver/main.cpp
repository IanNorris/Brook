// Host test for nv_disp_driver.h / nv_disp_flipper.h (RTX 2080 resident display
// driver, milestone D1): both outputs brought up and torn down through the quiet
// runners, flips through the flipper (ring wrap, mailbox, watchdog), shared instmem,
// failure fallbacks and the head-0 invariant check, against the shared fake engine.
#include "../test_nv_disp_head/head_fake.h"
#include "nv_disp_driver.h"
#include "nv_disp_hole.h"
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
        for(int k=0;k<5000&&(d->f[o].state==FlipState::Armed||d->f[o].queued>=0);++k)d->Tick();
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
     g.d->Command("probe 1 8");assert(g.b.Count("\"nv_driver_probe\":1")==8&&g.b.Count("\"nv_driver_hist\":1")>=1);
     assert(g.d->f[1].st.latHist[0]+g.d->f[1].st.latHist[1]+g.d->f[1].st.latHist[2]+g.d->f[1].st.latHist[3]+g.d->f[1].st.latHist[4]+
            g.d->f[1].st.latHist[5]+g.d->f[1].st.latHist[6]==g.d->f[1].st.completes);
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
    assert(st.completes==40&&st.latchBlank+st.latchActive==40&&g.d->f[0].loadvLive); // LOADV paces; the scanline gate is only the fallback
    g.d->f[0].paced=false;const uint64_t fr=st.frames;g.Flip(0,40);   // unpaced: no per-frame gate
    assert(st.completes==80&&st.frames-fr<40);
    g.d->Command("interval 0 1");assert(g.d->f[0].interval==1);g.Flip(0,1);
    assert(g.c.R(RegArm(1,c57e::SET_PRESENT_CONTROL))==1);
    g.d->Command("interval 0 0");g.Flip(0,1);
    assert(g.d->Down0());
}
// D2 age repair: B is stale at bring-up (full copy); afterwards each buffer gets this
// frame's damage plus the previous frame's.
// Completion waits for the LOADV counter (the armed state reaching the scanout); a
// counter that never moves falls back to armed-state completion and is flagged.
static void TestLoadv(){
    {DRig g;assert(g.d->Up0());g.Flip(0,10);assert(g.d->f[0].loadvLive&&g.d->f[0].st.completes==10);assert(g.d->Down0());}
    {DRig g;g.c.loadvFrozen=true;assert(g.d->Up0());g.d->f[0].paced=false;g.Flip(0,1);
     for(int k=0;k<200000&&g.d->f[0].state==FlipState::Armed;++k)g.d->Tick();
     g.Flip(0,3);assert(!g.d->f[0].loadvLive&&g.d->f[0].loadvStuck==1&&g.d->f[0].fail==FlipFail::None);assert(g.d->Down0());}
}
static void TestAgeRepair(){
    AgeRepair a;a.Reset();std::vector<brook::DamageRect> got;
    auto cp=[&](const brook::DamageRect& r){got.push_back(r);};
    const brook::DamageRect full{0,0,3840,2160};
    brook::DamageRects d[6];for(int i=1;i<6;++i)d[i].Add({i*10,i*10,i*10+5,i*10+5});
    assert(a.Apply(1,d[1],3840,2160,cp)==1&&got[0]==full);got.clear();                 // B stale: full
    assert(a.Apply(0,d[2],3840,2160,cp)==2&&got[0]==d[1].rects[0]&&got[1]==d[2].rects[0]);got.clear(); // A: frames 1..2
    assert(a.Apply(2,d[3],3840,2160,cp)==1&&got[0]==full);got.clear();                 // third stale: full
    assert(a.Apply(1,d[4],3840,2160,cp)==3&&got[0]==d[2].rects[0]&&got[2]==d[4].rects[0]);got.clear(); // B: frames 2..4
    assert(a.Apply(0,d[5],3840,2160,cp)==3&&got[0]==d[3].rects[0]&&got[2]==d[5].rects[0]);got.clear(); // A: frames 3..5
    for(int k=0;k<4;++k)(void)a.Apply(1,d[1],3840,2160,cp);got.clear();
    // The third, last written 6 frames ago, gets exactly what it missed (d4, d5, d1 merged), not a full copy.
    assert(a.Apply(2,d[1],3840,2160,cp)==3&&got[0]==d[4].rects[0]&&got[1]==d[5].rects[0]&&got[2]==d[1].rects[0]);got.clear();
    a.Stale(1);assert(a.Apply(1,d[2],3840,2160,cp)==1&&got[0]==full);got.clear();      // marked stale: full
    assert(a.Apply(0,brook::DamageRects::Full(3840,2160),3840,2160,cp)==1);got.clear();
    // The same rows damaged every frame (an animation) are copied once, not once per frame.
    brook::DamageRects rows=brook::DamageRects::Rows(0,48,3840);
    for(int k=0;k<3;++k)(void)a.Apply(int8_t(k),rows,3840,2160,cp);got.clear();
    assert(a.Apply(0,rows,3840,2160,cp)==1&&got.size()==1&&got[0]==rows.rects[0]);got.clear();
    assert(a.Apply(9,d[1],3840,2160,cp)==0);
}
// Mailbox with three buffers: never blocks; a reclaimed queued buffer is reused.
static void TestTripleMailbox(){
    DRig g;assert(g.d->Up0());auto& f=g.d->f[0];
    Surface t=g.d->HeadZeroThird();assert(t.vram==0x8800000);f.SetThird(t);
    g.c.wstall.insert(1);
    int8_t b1=f.Back();assert(f.Present(b1)==0);                       // armed (engine stalled: stays armed)
    int8_t b2=f.Back();assert(b2>=0&&b2!=b1&&b2!=f.front&&f.Present(b2)==1);
    int8_t b3=f.Back();assert(b3==b2&&f.st.replaced==1);               // all three taken: the queued one is reclaimed
    assert(f.Present(b3)==1&&f.queued==b3);
    g.c.wstall.erase(1);g.c.ExecuteW(1,f.put);
    for(int k=0;k<5000&&(f.state==FlipState::Armed||f.queued>=0);++k)g.d->Tick();
    assert(f.state==FlipState::Idle&&f.front==b3);
    assert(g.d->Down0());
}
static void TestVblankCounter(){
    DRig g;assert(g.d->Up0());
    g.c.R(0x611800)=0x4;g.d->Tick();assert(g.d->f[0].st.vblanks==1&&g.c.R(0x611800)==0);
    g.c.R(0x611800)=0x3;g.d->Tick();assert(g.d->f[0].st.vblanks==1&&g.c.R(0x611800)==0x3); // only VBLANK is acked
    assert(g.d->Down0());
}
// Poll hint while a flip is in flight: time to the next vblank start, 1..17 ms at 60 Hz.
static void TestMsToVblank(){
    DRig g;assert(g.d->Up0());auto f=g.d->f[0];
    f.vtotal=2222;f.vblankStart=2218;f.vblankEnd=58;                    // 3840x2160@60 as read on the RTX
    assert(f.MsToVblank(2217)==1);                                     // one line before: just after it
    assert(f.MsToVblank(2218)==17);                                    // at the start: a whole frame
    const uint32_t mid=f.MsToVblank(1109);assert(mid>=8&&mid<=10);
    for(uint32_t v=0;v<f.vtotal;v+=37){const uint32_t ms=f.MsToVblank(v);assert(ms>=1&&ms<=17);}
    assert(f.MsToVblank(2222)==1&&f.MsToVblank(5,0)==1);               // out of range / no frame time
    f.vblankStart=0;assert(f.MsToVblank(2159)==1);                     // unknown start: active height
    f.vtotal=0;assert(f.MsToVblank(100)==1);
    assert(g.d->Down0());
}
// Vblank-driven wake (D3): time to the frame wrap, where the LOADV of an armed flip lands,
// from the head's own frame period (htotal*vtotal/pclk from ARM).
static void TestUsToWrap(){
    DRig g;assert(g.d->Up0());auto f=g.d->f[0];
    f.vtotal=2222;f.frameUs=16667;
    assert(f.UsToWrap(0)==16667&&f.UsToWrap(2221)==7&&f.UsToWrap(2222)==0&&f.UsToWrap(9999)==0);
    const uint32_t mid=f.UsToWrap(1111);assert(mid>=8330&&mid<=8340);
    for(uint32_t v=1;v<f.vtotal;++v)assert(f.UsToWrap(v)<f.UsToWrap(v-1));   // strictly falling to the wrap
    f.vtotal=0;assert(f.UsToWrap(5)==0);
    // Init derives the period: 4000x2222 at 533.28 MHz = 16667 us; a bogus clock keeps 60 Hz.
    auto& c=g.c;c.R(RegArm(kChidCore,c57d::HeadRasterSize(1)))=(2222u<<16)|4000u;c.R(RegArm(kChidCore,c57d::HeadPixelClock(1)))=533280000u;
    NvWindowFlipper<decltype(g.b)> h;h.Init(&g.b,2,1,0x0ff10000,0,Surface{},Surface{},0);
    assert(h.vtotal==2222&&h.frameUs>=16660&&h.frameUs<=16670);
    c.R(RegArm(kChidCore,c57d::HeadPixelClock(1)))=1000u;h.Init(&g.b,2,1,0x0ff10000,0,Surface{},Surface{},0);assert(h.frameUs==16667);
    assert(g.d->Down0());
}
// Head 1's third buffer sits after head 0's third and below instmem; none overlap.
static void TestHeadOneThird(){
    DRig g;assert(g.d->Up0()&&g.d->Up1());
    const Surface t0=g.d->HeadZeroThird(),t1=g.d->HeadOneThird(g.d->f[1].surf[0]);
    assert(t0.vram==0x8800000&&t1.vram==0xaa00000&&t1.pitch==g.d->f[1].surf[0].pitch);
    const uint64_t bytes=uint64_t(t1.pitch)*t1.height;
    const uint64_t starts[6]={g.d->f[0].surf[0].vram,g.d->f[0].surf[1].vram,g.d->f[1].surf[0].vram,g.d->f[1].surf[1].vram,t0.vram,t1.vram};
    for(int i=0;i<6;++i)for(int j=i+1;j<6;++j)assert(starts[i]+bytes<=starts[j]||starts[j]+bytes<=starts[i]);
    assert(t1.vram+bytes<=g.d->r0->cfg.instVram);
    g.d->f[1].SetThird(t1);assert(g.d->f[1].nbuf==3);
    g.d->Quiesce();
}
// Down1 with a frame queued behind an armed one (the desktop's mailbox): the queued flip
// lands while the teardown waits, so the hide must target the offset ARM shows after it
// (live aabe56ba: the hide timed out and the head-1 abort list ran).
static void TestDown1WithQueuedFrame(){
    DRig g;assert(g.d->Up0()&&g.d->Up1());auto& f=g.d->f[1];
    f.SetThird(g.d->HeadOneThird(f.surf[0]));
    const uint32_t ch=ChidWndw(2);
    g.c.wstall.insert(ch);
    int8_t b1=f.Back();assert(f.Present(b1)==0);
    int8_t b2=f.Back();assert(f.Present(b2)==1&&f.queued==b2);
    g.c.wstall.erase(ch);g.c.ExecuteW(ch,f.put);
    assert(g.d->Down1());
    assert(g.d->out[1]==OutState::Off&&g.d->failures[1]==0&&f.fail==FlipFail::None);
    assert(g.d->Down0());
}
// D3: only pixels that differ from the shadow are reported (and recorded in it).
static void TestShadowDiff(){
    const uint32_t W=64,H=32;std::vector<uint32_t> src(W*H,7),sh(W*H,7);
    brook::DamageRects out;
    assert(ShadowDiff(src.data(),W,sh.data(),W,W,H,brook::DamageRect{0,0,64,32},out)==W*H&&out.empty());   // redrawn, same pixels
    src[5*W+9]=1;src[7*W+30]=2;
    assert(ShadowDiff(src.data(),W,sh.data(),W,W,H,brook::DamageRect{0,0,64,32},out)==W*H);
    assert(out.count==1&&out.rects[0]==(brook::DamageRect{9,5,31,8})&&sh[5*W+9]==1&&sh[7*W+30]==2);
    out=brook::DamageRects{};
    assert(ShadowDiff(src.data(),W,sh.data(),W,W,H,brook::DamageRect{0,0,64,32},out)&&out.empty());     // now recorded
    src[0]=3;src[W*H-1]=4;                                                  // the very first and last pixel
    assert(ShadowDiff(src.data(),W,sh.data(),W,W,H,brook::DamageRect{-5,-5,99,99},out)==W*H);           // clipped to the head
    assert(out.count==1&&out.rects[0]==(brook::DamageRect{0,0,64,32}));
    out=brook::DamageRects{};src[3*W+2]=9;
    assert(ShadowDiff(src.data(),W,sh.data(),W,W,H,brook::DamageRect{10,0,20,32},out)==10*32&&out.empty()); // outside the rect: not looked at
    // Overflowing damage merges locally: two clusters stay apart.
    brook::DamageRects d;for(int i=0;i<20;++i)d.Add({i*2,0,i*2+1,1});d.Add({5000,0,5001,1});
    assert(d.count<=brook::DamageRects::Max);bool far=false;for(uint32_t i=0;i<d.count;++i)far|=d.rects[i].x0==5000;assert(far);
}

// Zero-copy step 2 (nv_disp_hole.h): a direct window's hole is filled from its layer on every
// flip and kept out of the shadow diff; when it moves or goes away its old rectangle is a
// foreign region (BO pixels in the buffers, a stale shadow) that must be rewritten in every
// buffer. Model of the CE presenter (shadow -> target copies, then the layer copy) over three
// buffers with AgeRepair; the buffer flipped must always equal the composed frame.
static void TestSubtractRect(){
    brook::DamageRect out[4];
    const brook::DamageRect r{0,0,10,10};
    assert(SubtractRect(r,{20,20,30,30},out)==1&&out[0]==r);                    // apart
    assert(SubtractRect(r,{0,0,10,10},out)==0);                                 // covered
    assert(SubtractRect(r,{2,3,5,7},out)==4);                                   // inside: 4 bands
    assert(out[0]==(brook::DamageRect{0,0,10,3})&&out[1]==(brook::DamageRect{0,7,10,10})&&
           out[2]==(brook::DamageRect{0,3,2,7})&&out[3]==(brook::DamageRect{5,3,10,7}));
    assert(SubtractRect(r,{5,-5,20,20},out)==1&&out[0]==(brook::DamageRect{0,0,5,10}));   // right part
    uint64_t area=0;for(uint32_t i=0;i<SubtractRect(r,{2,3,5,7},out);++i)area+=brook::DamageRects::Area(out[i]);
    assert(area==100-12);
}
struct HoleSim{
    static constexpr uint32_t W=64,H=48;
    std::vector<uint32_t> src=std::vector<uint32_t>(W*H),shadow=std::vector<uint32_t>(W*H),bo,buf[3];
    bool shadowValid=false;int8_t front=0;AgeRepair age;brook::DamageRects pending;brook::DamageRect hole{};
    uint32_t seed=1;uint64_t layerCopies=0,repairs=0,compared=0;
    uint32_t Rnd(){seed=seed*1103515245u+12345u;return (seed>>8)|0xff000000u;}
    HoleSim(){for(auto& b:buf){b.resize(W*H);for(auto& v:b)v=Rnd();}age.Reset();age.Stale(0);}
    void Fill(const brook::DamageRect& r){for(int32_t y=r.y0;y<r.y1;++y)for(int32_t x=r.x0;x<r.x1;++x)src[y*W+x]=Rnd();}
    void NewLayer(){bo.resize(W*H);for(auto& v:bo)v=Rnd()&0x00ffffffu;}       // XRGB: undefined X byte
    // One compositor frame: damage (output-local) and the requested hole.
    void Present(const brook::DamageRects& damage,const brook::DamageRect& want){
        brook::DamageRects changed;
        if(!shadowValid){shadow=src;changed=brook::DamageRects::Full(W,H);shadowValid=true;hole=want;}
        else{
            if(!(want==hole)){if(RepairHole(src.data(),W,shadow.data(),W,W,H,hole,brook::DamageRect{},changed))++repairs;hole=want;}
            for(uint32_t i=0;i<damage.count;++i)compared+=HoleDiff(src.data(),W,shadow.data(),W,W,H,damage.rects[i],hole,changed);
        }
        pending.AddAll(changed);
        const int8_t bb=int8_t((front+1)%3);
        auto& d=buf[bb];
        (void)age.Apply(bb,pending,W,H,[&](const brook::DamageRect& r){ForEachOutside(r,hole,[&](const brook::DamageRect& q){
            for(int32_t y=q.y0;y<q.y1;++y)for(int32_t x=q.x0;x<q.x1;++x)d[y*W+x]=shadow[y*W+x];});});
        if(!hole.empty()){CopyLayerRect(d.data(),W,hole,bo.data()+hole.y0*W+hole.x0,W);++layerCopies;}
        pending=brook::DamageRects{};front=bb;
    }
    // What the head must show: the composed frame with the hole from the layer.
    bool Shows(const std::vector<uint32_t>& b) const{
        for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
            const bool in=!hole.empty()&&int32_t(x)>=hole.x0&&int32_t(x)<hole.x1&&int32_t(y)>=hole.y0&&int32_t(y)<hole.y1;
            const uint32_t want=in?(bo[y*W+x]|0xff000000u):src[y*W+x];
            if(b[y*W+x]!=want)return false;}
        return true;}
};
static void TestHolePresent(){
    HoleSim s;s.NewLayer();
    const brook::DamageRect none{},h1{8,8,40,32},h2{20,12,52,36},bar{0,44,64,48};
    auto dmg=[](std::initializer_list<brook::DamageRect> l){brook::DamageRects d;for(auto& r:l)d.Add(r);return d;};
    for(auto& v:s.src)v=s.Rnd();
    s.Present(brook::DamageRects::Full(64,48),none);assert(s.Shows(s.buf[s.front]));
    for(int i=0;i<3;++i){s.Fill(bar);s.Present(dmg({bar}),none);assert(s.Shows(s.buf[s.front]));}
    // Direct at h1: the back buffer holds garbage there (the compositor draws nothing) and the
    // window's damage is marked every frame; the taskbar keeps changing outside the hole.
    const std::vector<uint32_t> before=s.src;
    for(int i=0;i<6;++i){s.NewLayer();s.Fill(h1);s.Fill(bar);s.Present(dmg({h1,bar}),h1);assert(s.Shows(s.buf[s.front]));}
    assert(s.layerCopies==6);
    // A damage rect straddling the hole (garbage inside, real change outside).
    {const brook::DamageRect straddle{4,4,44,12};s.Fill(straddle);s.NewLayer();s.Present(dmg({straddle}),h1);assert(s.Shows(s.buf[s.front]));}
    // Move to h2 (overlapping h1): the scene redraws the old position (wallpaper) and the new one.
    s.Fill(h1);s.Fill(h2);s.NewLayer();s.Present(dmg({h1,h2}),h2);assert(s.Shows(s.buf[s.front])&&s.repairs==1);
    for(int i=0;i<3;++i){s.NewLayer();s.Fill(h2);s.Present(dmg({h2}),h2);assert(s.Shows(s.buf[s.front]));}
    // Leave direct, and the foreign case: the composed window content equals what the stale
    // shadow holds there (pre-direct pixels), so a plain shadow diff would find nothing to copy
    // and leave the BO's pixels in every buffer.
    for(int32_t y=h2.y0;y<h2.y1;++y)for(int32_t x=h2.x0;x<h2.x1;++x)s.src[y*64+x]=s.shadow[y*64+x];
    s.Present(dmg({h2}),none);assert(s.Shows(s.buf[s.front])&&s.repairs==2);
    // No stale pixels in any buffer once each has been flipped again (no new damage at all).
    for(int i=0;i<3;++i){s.Present(brook::DamageRects{},none);assert(s.Shows(s.buf[s.front]));}
    for(auto& b:s.buf)assert(s.Shows(b));
    // Without the repair the foreign case is wrong: replay with RepairHole skipped.
    {HoleSim t;t.NewLayer();for(auto& v:t.src)v=t.Rnd();t.Present(brook::DamageRects::Full(64,48),none);
     t.Fill(h1);t.Present(dmg({h1}),h1);
     for(int32_t y=h1.y0;y<h1.y1;++y)for(int32_t x=h1.x0;x<h1.x1;++x)t.src[y*64+x]=t.shadow[y*64+x];
     t.hole=none;   // forget the hole without repairing it
     t.Present(dmg({h1}),none);
     for(int i=0;i<2;++i)t.Present(brook::DamageRects{},none);
     bool allOk=true;for(auto& b:t.buf)allOk&=t.Shows(b);assert(!allOk);}   // the BO's frame lingers
    (void)before;
}
int main(){
    TestSubtractRect();TestHolePresent();
    TestShadowDiff();TestUsToWrap();TestHeadOneThird();TestDown1WithQueuedFrame();
    TestOutput0();TestBothOutputs();TestFailures();TestPacing();TestLoadv();TestAgeRepair();TestTripleMailbox();TestVblankCounter();
    TestMsToVblank();
    puts("test_nv_disp_driver: ok");return 0;
}
