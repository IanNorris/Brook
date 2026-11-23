#pragma once
// RTX 2080 resident display driver (D1; design: notes rtx2080-dual-monitor-design.md §3).
// Output 0 = head 0 / window 0 (the GOP head), output 1 = head 1 / window 2 (conn2).
//
// Bring-up and teardown reuse the hardware-proven, host-tested step-3 Runner and
// step-4 HeadRunner plans in quiet mode (only failures are emitted). Presents use
// NvWindowFlipper (nv_disp_flipper.h), with no Plan executor or JSON on the hot path.
// Both outputs share one instmem image (Config bindCore/extraWindowChid make the step-3
// image byte-identical to the head plan's), and each teardown keeps instmem valid while
// the other output is live.
//
// Safety, as in the runners: preflight before every bring-up, the head-1 MMIO allow-list,
// abort/quiesce lists, the instmem guard. Added here: a head-0 invariant check while
// live, and every failure ends with head 0 back on the GOP surface ("DAVE"), so the
// GOP software path keeps working.
#include "nv_disp_runner.h"
#include "nv_disp_head_runner.h"
#include "nv_disp_flipper.h"

namespace brook::nvdisp {

enum class OutState:uint8_t{Off,Live,Failed};
constexpr uint32_t kDriverOutputs=2;
constexpr uint32_t kDriverWindow[kDriverOutputs]={0,2};
constexpr uint32_t kFillD=0xffe07010,kFillDMarker=0xff000000;

template<typename B> struct NvDisplayDriver{
    B* b=nullptr;Runner<B>* r0=nullptr;HeadRunner<B>* r1=nullptr;
    NvWindowFlipper<B> f[kDriverOutputs]{};
    OutState out[kDriverOutputs]{};
    uint32_t ups[kDriverOutputs]{},downs[kDriverOutputs]{},failures[kDriverOutputs]{};
    uint64_t lastCheckUs=0;uint32_t invariantFails=0;

    struct Line{char t[320];uint32_t n=0;
        void raw(const char* x){while(*x&&n+2<sizeof(t))t[n++]=*x++;t[n]=0;}
        void s(const char* x){while(*x&&n+2<sizeof(t)){char ch=*x++;t[n++]=ch=='"'||ch=='\\'?'\'':ch;}t[n]=0;}
        void hex(uint64_t v){char d[17];int k=0;do{d[k++]="0123456789abcdef"[v&15];v>>=4;}while(v&&k<16);while(k)t[n<sizeof(t)-2?n++:n]=d[--k];t[n]=0;}
        void dec(uint64_t v){char d[21];int k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v);while(k)t[n<sizeof(t)-2?n++:n]=d[--k];t[n]=0;}
    };
    void Msg(const char* kind,const char* text){Line l;l.raw("{\"nv_driver\":\"");l.s(kind);l.raw("\",\"msg\":\"");l.s(text);l.raw("\"}\n");b->emit(l.t);}
    bool Live(uint32_t o)const{return o<kDriverOutputs&&out[o]==OutState::Live;}
    bool AnyLive()const{return Live(0)||Live(1);}

    // Shared-instmem configuration for both plans (identical images).
    void ShareConfig(){
        r0->base.bindCore=true;r0->base.extraWindowChid=ChidWndw(kDriverWindow[1]);r0->base.allowSharedInstmem=true;
        r1->cfg.base.allowSharedInstmem=true;r1->cfg.base.bindCore=true;r1->cfg.window=kDriverWindow[1];
    }
    static Surface Second(const Surface& s){const uint64_t bytes=uint64_t(s.pitch)*s.height,two=2ull<<20;
        Surface t=s;t.vram=s.vram+((bytes+two-1)&~(two-1));return t;}

    // ---- output 0: step 3 stage A up to the restore phase, then the flipper owns window 0
    bool Up0(){
        if(Live(0))return true;
        ShareConfig();r0->quiet=true;r0->externalRing=false;
        if(!r0->Preflight()){Msg("up0","preflight refused (state not as the GOP or a head-1 run left it)");out[0]=OutState::Failed;return false;}
        if(!r0->Build(Stage::A,0,0,false)){out[0]=OutState::Failed;return false;}
        const uint32_t stop=r0->plan.phaseBegin[uint32_t(Phase::Restore)];
        r0->Start(Stage::A,0,0,false,stop);
        if(!(r0->st.paused&&r0->st.cursor==stop&&r0->st.firstFlipOk)){
            Msg("up0","bring-up failed (the runner aborted to the GOP state)");out[0]=OutState::Failed;++failures[0];return false;}
        r0->externalRing=true;
        f[0].Init(b,kDriverWindow[0],0,r0->cfg.wndwPushVram,r0->pushes.end[0],r0->cfg.surf[0],r0->cfg.surf[1],0);
        out[0]=OutState::Live;++ups[0];Msg("up0","head 0 flips live (window 0, buffers A=GOP surface, B)");return true;
    }
    // The GOP's window state (BuildRestore: notifier off, present control, "DAVE"), then
    // the runner's teardown phase (fini, pushbuffer restore, instmem guard).
    bool RestoreHead0(){
        Push ring;Ring rr;(void)BuildRestore(ring,rr,r0->cfg);
        if(f[0].state==FlipState::Failed){f[0].state=FlipState::Idle;f[0].armed=-1;} // best effort: try anyway
        return f[0].SubmitWait(ring.words,rr.put/4,RestoreHandle(r0->cfg),uint32_t(r0->cfg.surf[0].vram>>8),1000000);
    }
    bool Down0(){
        if(out[0]==OutState::Off)return true;
        if(out[0]==OutState::Failed&&!r0->st.live&&!r0->st.paused){out[0]=OutState::Off;return true;}
        const bool restored=RestoreHead0();
        f[0].state=FlipState::Off;
        if(restored&&r0->st.paused){
            r0->st.cursor=r0->plan.phaseBegin[uint32_t(Phase::Teardown)];r0->Continue();
        }
        if(r0->st.live||r0->st.paused)r0->Abort(false); // teardown failed or restore failed: abort list
        const bool clean=restored&&!r0->st.live;
        out[0]=OutState::Off;++downs[0];
        Msg("down0",clean?"head 0 back on the GOP surface; window 0 restored":"head 0 abort list ran (restore did not land; instmem kept valid if referenced)");
        return clean;
    }

    // ---- output 1: head-1 enable + window 2 attach, then the flipper owns window 2
    bool Up1(){
        if(Live(1))return true;
        ShareConfig();r1->quiet=true;r1->coexist=true;r1->winExternal=false;
        r1->Preflight();
        if(!r1->st.preflightOk){Msg("up1","head1 preflight refused");out[1]=OutState::Failed;return false;}
        r1->Enable(true,~0u,false,0);
        if(!r1->st.enabled){Msg("up1","head 1 enable failed (head abort ran)");out[1]=OutState::Failed;++failures[1];return false;}
        r1->Show();
        if(!r1->st.windowLive){Msg("up1","window 2 attach failed; disabling head 1");r1->Disable();out[1]=OutState::Failed;++failures[1];return false;}
        r1->winExternal=true;
        const Surface c=r1->win.surf,dd=Second(c);
        // D gets a different pattern from C (orange, black bar) so head-1 flips are visible.
        b->fill(dd.vram,uint32_t(uint64_t(dd.pitch)*dd.height),dd.pitch,kFillD,kFillDMarker,kHeadMarkerLines);
        f[1].Init(b,kDriverWindow[1],1,r1->win.pushVram,r1->win.showEnd,c,dd,0);
        out[1]=OutState::Live;++ups[1];Msg("up1","head 1 flips live (window 2, buffers C, D)");return true;
    }
    bool Down1(){
        if(out[1]==OutState::Off)return true;
        if(out[1]==OutState::Failed&&!r1->st.live&&!r1->st.paused){out[1]=OutState::Off;return true;}
        bool hidden=false;
        if(r1->st.windowLive){
            Push p;p.Mthd(c57e::SET_PRESENT_CONTROL,0u);p.Mthd(c57e::SET_CONTEXT_DMA_ISO0,0u);PushWindowUpdate(p,kDriverWindow[1]);
            if(f[1].state==FlipState::Failed){f[1].state=FlipState::Idle;f[1].armed=-1;}
            const uint32_t off=b->rd(RegArm(ChidWndw(kDriverWindow[1]),c57e::SET_OFFSET0));
            hidden=f[1].SubmitWait(p.words,p.count,0,off,1000000)&&r1->WindowFini();
        }else hidden=true;
        f[1].state=FlipState::Off;
        if(hidden&&r1->st.enabled)r1->Disable();
        if(r1->st.live||r1->st.paused)r1->Abort(false);  // window abort (no kick: winExternal) + head abort
        out[1]=OutState::Off;++downs[1];
        Msg("down1",hidden?"window 2 off, head 1 disabled":"head 1 abort list ran");
        return hidden;
    }

    // ---- presentation
    int Present(uint32_t o,int8_t buf){return Live(o)?f[o].Present(buf):-1;}
    int8_t Back(uint32_t o)const{return Live(o)?f[o].Back():-1;}
    // Non-blocking tick: vblank count, flip completion, failure handling, invariants.
    void Tick(){
        for(uint32_t o=0;o<kDriverOutputs;++o){
            if(!Live(o))continue;
            f[o].TickVblank();
            if(f[o].Poll()==FlipState::Failed)OnFailure(o);
        }
        const uint64_t now=b->nowUs();
        if(AnyLive()&&now-lastCheckUs>1000000){lastCheckUs=now;if(!Head0Intact()){++invariantFails;
            Msg("WARNING","head-0 invariant changed (raster/pclk/SOR1/window-0 owner): taking all outputs down");Quiesce();}}
    }
    bool Head0Intact(){
        const auto& g=r0->base.gop;
        return b->rd(RegArm(kChidCore,c57d::HeadRasterSize(0)))==g.headRaster&&b->rd(RegArm(kChidCore,c57d::HeadPixelClock(0)))==g.headPclk&&
               b->rd(RegArm(kChidCore,c57d::SorSetControl(g.sor)))==g.sorCtl&&(b->rd(RegArm(kChidCore,c57d::WindowSetControl(0)))&0xf)==0;}
    void OnFailure(uint32_t o){
        ++failures[o];
        Line l;l.raw("{\"nv_driver\":\"flip-failed\",\"output\":");l.dec(o);l.raw(",\"code\":");l.dec(uint32_t(f[o].fail));
        const uint32_t ch=ChidWndw(kDriverWindow[o]);
        l.raw(",\"get\":\"");l.hex(b->rd(RegGet(ch)));l.raw("\",\"put\":\"");l.hex(f[o].put);
        l.raw("\",\"status\":\"");l.hex(b->rd(RegChnStatus(ch)));l.raw("\",\"exc\":\"");l.hex(b->rd(RegExcept(ch)));
        l.raw("\",\"arm_offset\":\"");l.hex(b->rd(RegArm(ch,c57e::SET_OFFSET0)));l.raw("\"}\n");b->emit(l.t);
        if(o==0)(void)Down0();else(void)Down1();
        out[o]=OutState::Failed;
    }
    // Pre-reset / shutdown: head 1 first (its teardown keeps instmem while window 0 is live).
    void Quiesce(){
        if(out[1]!=OutState::Off)(void)Down1();
        if(out[0]!=OutState::Off)(void)Down0();
    }

    // ---- bridge verbs (debug / manual control on top of the same driver)
    // One line per output: a single line overflowed the 320-byte Line (live a087416d).
    void Status(){
        Line h;h.raw("{\"nv_driver_status\":1,\"outputs\":");h.dec(kDriverOutputs);h.raw(",\"invariant_fails\":");h.dec(invariantFails);
        h.raw("}\n");b->emit(h.t);
        for(uint32_t o=0;o<kDriverOutputs;++o){
            Line l;l.raw("{\"nv_driver_out\":");l.dec(o);l.raw(",\"state\":");l.dec(uint32_t(out[o]));l.raw(",\"flip\":");l.dec(uint32_t(f[o].state));
            l.raw(",\"front\":");l.dec(uint32_t(f[o].front));l.raw(",\"presents\":");l.dec(f[o].st.presents);l.raw(",\"completes\":");l.dec(f[o].st.completes);
            l.raw(",\"replaced\":");l.dec(f[o].st.replaced);l.raw(",\"wraps\":");l.dec(f[o].st.wraps);l.raw(",\"lat_us\":");l.dec(f[o].st.lastLatUs);
            l.raw(",\"max_lat_us\":");l.dec(f[o].st.maxLatUs);l.raw(",\"vblanks\":");l.dec(f[o].st.vblanks);l.raw(",\"frames\":");l.dec(f[o].st.frames);
            l.raw(",\"latch_blank\":");l.dec(f[o].st.latchBlank);l.raw(",\"latch_active\":");l.dec(f[o].st.latchActive);
            l.raw(",\"paced\":");l.dec(f[o].st.paced);l.raw(",\"interval\":");l.dec(f[o].interval);l.raw(",\"fail\":");l.dec(uint32_t(f[o].fail));
            l.raw(",\"ups\":");l.dec(ups[o]);l.raw(",\"downs\":");l.dec(downs[o]);l.raw(",\"failures\":");l.dec(failures[o]);l.raw("}\n");b->emit(l.t);}
    }
    // n flips alternating the back buffer, each waited for (paced: one per frame unless
    // `pace <o> 0`); reports latency, frames and where completions landed (tear test).
    void Bench(uint32_t o,uint32_t n){
        if(!Live(o)){Msg("refused","output not live");return;}
        uint32_t done=0;const uint64_t tb=b->nowUs();
        for(uint32_t i=0;i<n&&Live(o);++i){
            const int8_t bb=f[o].Back();if(bb<0||Present(o,bb)<0)break;
            const uint64_t t0=b->nowUs();
            while(Live(o)&&(f[o].state==FlipState::Armed||f[o].queued>=0)&&b->nowUs()-t0<500000){Tick();b->relax();}
            if(Live(o)&&f[o].state==FlipState::Idle)++done;else break;
        }
        Line l;l.raw("{\"nv_driver\":\"bench\",\"output\":");l.dec(o);l.raw(",\"requested\":");l.dec(n);l.raw(",\"completed\":");l.dec(done);
        l.raw(",\"ms\":");l.dec(uint32_t((b->nowUs()-tb)/1000));
        l.raw(",\"last_lat_us\":");l.dec(f[o].st.lastLatUs);l.raw(",\"max_lat_us\":");l.dec(f[o].st.maxLatUs);l.raw(",\"frames\":");l.dec(f[o].st.frames);
        l.raw(",\"latch_blank\":");l.dec(f[o].st.latchBlank);l.raw(",\"latch_active\":");l.dec(f[o].st.latchActive);l.raw("}\n");b->emit(l.t);
        Status();
    }
    static bool Word(const char*& p,const char* w){
        while(*p==' ')++p;const char* q=p;while(*w){if(*q!=*w)return false;++q;++w;}
        if(*q&&*q!=' ')return false;p=q;return true;}
    static bool Num(const char*& p,uint32_t& v){
        while(*p==' ')++p;if(*p<'0'||*p>'9')return false;v=0;
        while(*p>='0'&&*p<='9'){v=v*10+uint32_t(*p-'0');++p;if(v>100000)return false;}return true;}
    // nv-disp driver up [0|1|both] | down [0|1|all] | flip <o> <0|1> | bench <o> <n> | tick | status
    void Command(const char* p){
        uint32_t o=0,v=0;
        if(Word(p,"status")){Status();return;}
        if(Word(p,"up")){
            if(Word(p,"1")){(void)Up1();}else if(Word(p,"both")){if(Up0())(void)Up1();}else{(void)Word(p,"0");(void)Up0();}
            Status();return;}
        if(Word(p,"down")){
            if(Word(p,"0"))(void)Down0();else if(Word(p,"1"))(void)Down1();else{(void)Word(p,"all");Quiesce();}
            Status();return;}
        if(Word(p,"flip")){if(!Num(p,o)||!Num(p,v)||o>=kDriverOutputs||v>1){Msg("usage","flip <0|1> <0|1>");return;}
            Msg("flip",Present(o,int8_t(v))<0?"refused (not live)":"presented");return;}
        if(Word(p,"bench")){if(!Num(p,o)||!Num(p,v)||o>=kDriverOutputs||!v||v>3600){Msg("usage","bench <0|1> <1..3600>");return;}
            Bench(o,v);return;}
        if(Word(p,"tick")){Tick();Status();return;}
        // Experiments (design §7): pacing on/off, MIN_PRESENT_INTERVAL, acking all head-timing
        // event bits. Try `interval 1 1` on head 1 first: a hung MPI wait only loses head 1.
        if(Word(p,"pace")){if(!Num(p,o)||!Num(p,v)||o>=kDriverOutputs||v>1){Msg("usage","pace <o> <0|1>");return;}f[o].paced=v;Status();return;}
        if(Word(p,"interval")){if(!Num(p,o)||!Num(p,v)||o>=kDriverOutputs||v>1){Msg("usage","interval <o> <0|1>");return;}f[o].interval=v;Status();return;}
        if(Word(p,"ackevt")){if(!Num(p,o)||!Num(p,v)||o>=kDriverOutputs||v>1){Msg("usage","ackevt <o> <0|1>");return;}f[o].ackAllEvt=v;Status();return;}
        Msg("usage","nv-disp driver up [0|1|both]|down [0|1|all]|flip <o> <buf>|bench <o> <n>|pace|interval|ackevt <o> <0|1>|tick|status");
    }
};
} // namespace brook::nvdisp
