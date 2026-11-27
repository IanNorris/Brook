#pragma once
// RTX 2080 resident display driver, hot path: page flips on one window channel
// without the Plan executor or per-step logging (design: notes
// rtx2080-dual-monitor-design.md §3). Proven on hardware by the step-3 runs:
// SET_PRESENT_CONTROL 0 (no MPI wait under the GOP head), NON_TEARING, image
// state + window-only UPDATE; completion = GET == PUT, ARM offset/ISO == the
// submission, METHOD_EXEC idle and UPD_STATE IDLE (window notifiers only reach BEGUN).
//
// Guard by construction: the flipper writes only its channel's PUT, the BAR1 flush
// register and its head's EVT_STAT_HEAD_TIMING (W1C of VBLANK), and VRAM inside its
// own ring. Anything else is refused and fails the flipper.
#include "nv_disp_plan.h"
#include "display_output.h"

namespace brook::nvdisp {

enum class FlipState:uint8_t{Off,Idle,Armed,Failed};
enum class FlipFail:uint8_t{None,Guard,Exception,Timeout,Flush,Bounds};
// frames: RG scanline wraps seen by Tick (vline decreased); vblanks: EVT_STAT VBLANK bits
// seen (live a087416d: only 5 over 300 flips, so it does not latch every frame unserviced).
// latchBlank/latchActive: the scanline when a completion was first seen, inside the
// vertical blank (>= active height) or in the active area (tear test).
// latHist: completion latency (arm -> ARM shows the submission), bins <100us, <1ms, <4ms,
// <8ms, <12ms, <17ms, >=17ms. "ARM" is the armed state; whether the scanout swaps then or
// at the next LOADV is what the latency and line histograms show. lineHist: the scanline
// at completion in 8 bins of the vertical total. Blank = outside [blank end, blank start]
// (HEAD_SET_RASTER_BLANK_END/START, ARM), as drm/nouveau classify scanout position.
struct FlipStats{uint32_t presents=0,completes=0,replaced=0,wraps=0,lastLatUs=0,maxLatUs=0,latchBlank=0,latchActive=0,lastLatchLine=0,paced=0;
                 uint64_t vblanks=0,frames=0,latSumUs=0;uint32_t latHist[7]{},lineHist[8]{};};

constexpr uint32_t RegHeadTimingEvt(uint32_t h){return 0x611800+h*4;} // EVT_STAT_HEAD_TIMING, bit 2 VBLANK (W1C)
constexpr uint32_t kEvtVblank=0x4;
constexpr uint32_t RegRgPos(uint32_t h){return 0x616330+h*0x800;} // vline 15:0 (gv100_head_rgpos)
// NV_PDISP_POSTCOMP_HEAD_LOADV_COUNTER (dev_disp.h v03_00): counts LOADV, the vblank-time
// load of the ARMED state (0x690800+ is ARMED_BASEADR, not active) into the active state.
// Live 11b0ec05: ARM matched the submission in microseconds (latch_active 120/120), which
// is the armed state; the scanout switches at the next LOADV. Completion therefore waits
// for this counter to move past its value at arm time.
constexpr uint32_t RegLoadvCounter(uint32_t h){return 0x61611c+h*0x800;}

// NV_UFLUSH_L2_FLUSH_DIRTY: write 1, poll bits 1:0 clear (nouveau gf100_ltc_flush). Write-back only.
constexpr uint32_t kRegL2FlushDirty=0x070010;
// Kick ordering (27 Nov NVK head freeze): 0 = BAR1 flush then PUT (the D1 path); 1 = also read the
// last word back through BAR1 before the flush; 2 = 1 + L2 flush-dirty before PUT. Default 1: with
// GSP-RM up BAR1 is virtual (our identity tables in VRAM), so a posted ring write can wait on a
// BAR1 page walk while the PUT (BAR0) overtakes it and the window fetches stale words. A BAR read
// is how nouveau flushes BAR writes under GSP-RM (r535_bar_flush reads its BAR2 flush page).
// Backend: rd, wr, vrd, vwr, fence, nowUs, relax (as the runners' backends).
template<typename B> struct NvWindowFlipper{
    static constexpr uint32_t kMaxBuffers=3;    // triple buffering for the desktop (D2); bench uses 2
    uint32_t nbuf=2;
    static constexpr uint32_t kWatchdogUs=250000;   // >> 3 frames at 60 Hz
    static constexpr uint32_t kRingBytes=kPushBytes;
    B* b=nullptr;
    uint32_t chid=0,window=0,head=0,handle=kHandleIsoPitch;
    uint64_t ring=0;uint32_t put=0;
    Surface surf[kMaxBuffers]{};
    FlipState state=FlipState::Off;FlipFail fail=FlipFail::None;
    int8_t front=0,armed=-1,queued=-1;
    uint64_t armedAtUs=0;uint32_t armedTarget=0,armedIso=0;
    FlipStats st{};
    uint32_t interval=0;     // MIN_PRESENT_INTERVAL (0 = GOP value; 1 waits on MPI, see design §7)
    bool paced=true;         // at most one completed flip per scanned frame
    bool ackAllEvt=false;    // experiment: also W1C LOADV / LAST_DATA (bits 0-1) when polling
    uint32_t lastVline=0;uint64_t latchFrame=~0ull;
    uint32_t vtotal=0,vblankEnd=0,vblankStart=0;     // from the head's ARM raster (Init)
    uint32_t frameUs=16667;                          // htotal*vtotal/pclk from ARM (Init), else 60 Hz
    // probe: per-flip raw samples for the latch question (driver `probe`).
    struct Sample{uint32_t armLine,doneLine,latUs,evtAfter;};
    static constexpr uint32_t kProbeMax=32;Sample probe[kProbeMax]{};uint32_t probeCount=0,probeWant=0;

    void SetThird(const Surface& c){surf[2]=c;nbuf=3;}
    void Init(B* backend,uint32_t window_,uint32_t head_,uint64_t ringVram,uint32_t putBytes,const Surface& a,const Surface& bb,int8_t frontBuf){
        b=backend;window=window_;chid=ChidWndw(window_);head=head_;ring=ringVram;put=putBytes;
        surf[0]=a;surf[1]=bb;surf[2]=Surface{};nbuf=2;front=frontBuf;armed=queued=-1;state=FlipState::Idle;fail=FlipFail::None;st=FlipStats{};
        lastVline=0;latchFrame=~0ull;probeCount=probeWant=0;
        vtotal=b->rd(RegArm(kChidCore,0x2064+head_*0x400))>>16;
        vblankEnd=b->rd(RegArm(kChidCore,0x206c+head_*0x400))>>16;vblankStart=b->rd(RegArm(kChidCore,0x2070+head_*0x400))>>16;
        const uint32_t htotal=b->rd(RegArm(kChidCore,0x2064+head_*0x400))&0xffff,pclk=b->rd(RegArm(kChidCore,0x200c+head_*0x400))&0x7fffffff;
        frameUs=16667;
        if(htotal&&vtotal&&pclk>=25000000u){const uint64_t us=uint64_t(htotal)*vtotal*1000000ull/pclk;if(us>=4000&&us<=50000)frameUs=uint32_t(us);}}
    // Microseconds from scanline `v` until the frame wraps. The LOADV that puts an armed
    // flip on screen lands at the end of the vertical blank (live a27588f7: done_line
    // 2219-2221 of vtotal 2222), so this is the time until the armed frame shows.
    uint32_t UsToWrap(uint32_t v)const{
        if(!vtotal||v>=vtotal)return 0;
        return uint32_t(uint64_t(vtotal-v)*frameUs/vtotal);}
    bool InBlank(uint32_t v)const{return vblankStart>vblankEnd?(v<vblankEnd||v>vblankStart):v>=surf[0].height;}
    // Whole milliseconds (1..frame) until the next vblank start from scanline `v`, at
    // `frameUs` per frame, plus 1 so the waiter wakes just after the LOADV.
    uint32_t MsToVblank(uint32_t v,uint32_t periodUs=16667)const{
        if(!vtotal||v>=vtotal)return 1;
        const uint32_t start=vblankStart&&vblankStart<vtotal?vblankStart:surf[0].height;
        const uint32_t lines=v<start?start-v:vtotal-v+start;
        const uint32_t ms=uint32_t(uint64_t(lines)*periodUs/vtotal/1000)+1;
        const uint32_t cap=periodUs/1000+1;
        return ms>cap?cap:ms;}
    bool Live()const{return state==FlipState::Idle||state==FlipState::Armed;}
    bool Allowed(uint32_t a)const{return a==RegPut(chid)||a==kRegBar1Flush||a==kRegL2FlushDirty||a==RegHeadTimingEvt(head);}
    void Fail(FlipFail f){if(state!=FlipState::Failed){fail=f;state=FlipState::Failed;}}
    void Wr(uint32_t a,uint32_t v){if(!Allowed(a)){Fail(FlipFail::Guard);return;}b->wr(a,v);}

    // Vblank counter from the latched event bit (polled; the IRQ path comes later).
    void TickVblank(){
        const uint32_t e=b->rd(RegHeadTimingEvt(head));
        const uint32_t ack=e&(ackAllEvt?0x7u:kEvtVblank);
        if(e&kEvtVblank)++st.vblanks;
        if(ack)Wr(RegHeadTimingEvt(head),ack);
        const uint32_t v=b->rd(RegRgPos(head))&0xffff;
        if(v<lastVline)++st.frames;
        lastVline=v;
        // A paced present waiting for the next frame goes out now.
        if(state==FlipState::Idle&&queued>=0&&(!paced||st.frames!=latchFrame)){const int8_t q=queued;queued=-1;Arm(q);}
    }

    // Copy n words into the ring at PUT (JUMP 0 first if they do not fit), flush, kick.
    // Only called with nothing in flight (GET == PUT), so the JUMP is safe.
    bool Kick(const uint32_t* w,uint32_t n){
        const uint32_t bytes=n*4;
        if(bytes+8>kRingBytes){Fail(FlipFail::Bounds);return false;}
        if(put+bytes+4>kRingBytes){b->vwr(ring+put,PushJump(0));put=0;++st.wraps;}
        lastKickOff=put;lastKickN=n;
        if(w!=lastW)for(uint32_t i=0;i<n&&i<kWordsMax;++i)lastW[i]=w[i];
        for(uint32_t i=0;i<n;++i)b->vwr(ring+put+i*4,w[i]);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);b->fence();
        if(kickMode>=1&&n)(void)b->vrd(ring+put+(n-1)*4);   // non-posted read: the posted ring writes landed
        Wr(kRegBar1Flush,1);
        bool flushed=false;for(uint32_t t=0;t<20000;++t)if(!(b->rd(kRegBar1Flush)&0x2)){flushed=true;break;}
        if(!flushed){Fail(FlipFail::Flush);return false;}
        if(kickMode>=2){Wr(kRegL2FlushDirty,1);flushed=false;for(uint32_t t=0;t<20000;++t)if(!(b->rd(kRegL2FlushDirty)&0x3)){flushed=true;break;}
            if(!flushed){Fail(FlipFail::Flush);return false;}}
        put+=bytes;Wr(RegPut(chid),put);
        return state!=FlipState::Failed;
    }
    uint32_t armLine=0,armLoadv=0;bool armLoadvSure=false;uint32_t armLoadvUnsure=0;
    uint32_t lastKickOff=0,lastKickN=0;   // ring offset and word count of the last kick (failure diagnostics)
    uint8_t kickMode=1;
    // Stale-fetch recovery (27 Nov): the window fetched the kick (GET == PUT, idle, no exception)
    // but ARM kept the previous surface while LOADV moved on. The same words are kicked again
    // (at most kMaxRekicks per flip); rekicks/rekickOk count it.
    static constexpr uint32_t kMaxRekicks=3,kWordsMax=32;
    uint32_t lastW[kWordsMax]{};uint32_t rekicksThis=0,rekicks=0,rekickOk=0;bool rekicked=false;
    bool loadvLive=true;   // false if the counter never moves (then: armed-state completion, flagged)
    uint32_t loadvStuck=0;
    bool ArmWords(const uint32_t* w,uint32_t n,uint32_t iso,uint32_t offset){
        armLoadv=b->rd(RegLoadvCounter(head));
        if(!Kick(w,n))return false;
        // 27 Nov (one-frame bands of another frame on head 0, Ian's photo): a vblank between the
        // sample above and the engine taking the UPDATE moves the counter without latching this
        // flip, so Complete() reported it a frame early and the buffer still being scanned out
        // was rewritten by the next present. Re-sample once the engine has taken the methods
        // (GET==PUT, ARM shows the target); if that is not seen within a few us, completion
        // needs two LOADVs instead (a frame late at worst, never early).
        armLoadvSure=false;
        for(uint32_t i=0;i<64;++i){
            if(b->rd(RegGet(chid))==put&&b->rd(RegArm(chid,c57e::SET_OFFSET0))==offset){
                armLoadv=b->rd(RegLoadvCounter(head));armLoadvSure=true;break;}
        }
        if(!armLoadvSure)++armLoadvUnsure;
        armLine=b->rd(RegRgPos(head))&0xffff;
        armedIso=iso;armedTarget=offset;armedAtUs=b->nowUs();state=FlipState::Armed;return true;}
    void Arm(int8_t buf){
        Push p;PushWindowFlip(p,surf[buf],handle,interval);PushWindowUpdate(p,window);
        rekicksThis=0;rekicked=false;
        if(ArmWords(p.words,p.count,handle,uint32_t(surf[buf].vram>>8)))armed=buf;
    }
    // Present buffer `buf` at the next vblank. 0 = armed now, 1 = queued (mailbox:
    // replaces an older queued frame), -1 = not live.
    int Present(int8_t buf){
        if(buf<0||buf>=int8_t(nbuf))return -1;
        if(state==FlipState::Idle){++st.presents;
            // With LOADV completion an armed flip shows at the next vblank anyway, so only
            // the scanline fallback needs the one-per-frame gate.
            if(paced&&!loadvLive&&st.frames==latchFrame){queued=buf;++st.paced;return 1;}
            Arm(buf);return state==FlipState::Armed?0:-1;}
        if(state==FlipState::Armed){++st.presents;if(queued>=0)++st.replaced;queued=buf;return 1;}
        return -1;
    }
    bool Armed()const{return true;}
    bool Complete(){
        const bool armedOk=b->rd(RegGet(chid))==put&&b->rd(RegArm(chid,c57e::SET_OFFSET0))==armedTarget&&
               b->rd(RegArm(chid,c57e::SET_CONTEXT_DMA_ISO0))==armedIso&&
               (b->rd(RegChnStatus(chid))&(kWinMethodExecBusy|0xf00))==0x100;
        if(!armedOk)return false;
        if(!loadvLive)return true;
        if(b->rd(RegLoadvCounter(head))-armLoadv>=(armLoadvSure?1u:2u))return true;   // loaded at a vblank: on screen
        // A counter that never moves (>= 3 frames' time armed) is not trusted: fall back.
        if(b->nowUs()-armedAtUs>60000){loadvLive=false;++loadvStuck;return true;}
        return false;}
    // Non-blocking; call every compositor loop / driver tick.
    FlipState Poll(){
        if(state!=FlipState::Armed)return state;
        if(b->rd(kRegExcWin)&(1u<<window)){Fail(FlipFail::Exception);return state;}
        const uint64_t now=b->nowUs();
        if(Complete()){
            const uint32_t line=b->rd(RegRgPos(head))&0xffff;     // first, closest to the detection
            const uint32_t evt=b->rd(RegHeadTimingEvt(head));
            const uint32_t lat=uint32_t(now-armedAtUs);st.lastLatUs=lat;if(lat>st.maxLatUs)st.maxLatUs=lat;++st.completes;st.latSumUs+=lat;
            ++st.latHist[lat<100?0:lat<1000?1:lat<4000?2:lat<8000?3:lat<12000?4:lat<17000?5:6];
            if(vtotal)++st.lineHist[(uint64_t(line)*8/vtotal)>7?7:(uint64_t(line)*8/vtotal)];
            if(probeCount<probeWant&&probeCount<kProbeMax)probe[probeCount++]=Sample{armLine,line,lat,evt};
            if(rekicked){++rekickOk;rekicked=false;}
            if(armed>=0)front=armed;armed=-1;state=FlipState::Idle;
            st.lastLatchLine=line;
            if(InBlank(line))++st.latchBlank;else ++st.latchActive;
            latchFrame=st.frames;
            if(queued>=0&&(!paced||loadvLive)){const int8_t q=queued;queued=-1;Arm(q);} // newest frame -> next vblank
            return state;}
        // Stale fetch: fetched and idle, ARM still not the submission, >= 2 LOADVs since the kick.
        if(rekicksThis<kMaxRekicks&&lastKickN&&lastKickN<=kWordsMax&&now-armedAtUs>2*uint64_t(frameUs)&&
           b->rd(RegLoadvCounter(head))-armLoadv>=2&&b->rd(RegGet(chid))==put&&
           (b->rd(RegChnStatus(chid))&(kWinMethodExecBusy|0xf00))==0x100&&b->rd(RegArm(chid,c57e::SET_OFFSET0))!=armedTarget){
            ++rekicksThis;++rekicks;rekicked=true;
            const uint32_t n=lastKickN;
            if(!ArmWords(lastW,n,armedIso,armedTarget))return state;
            return state;
        }
        if(now-armedAtUs>kWatchdogUs)Fail(FlipFail::Timeout);
        return state;
    }
    // Back buffer for the compositor: the one neither on screen nor armed.
    // Mailbox: a buffer neither on screen nor armed nor queued; with all taken, the queued
    // (not yet kicked) one is reclaimed for the newer frame.
    int8_t Back(){
        for(int8_t i=0;i<int8_t(nbuf);++i)if(i!=front&&i!=armed&&i!=queued)return i;
        if(queued>=0){const int8_t q=queued;queued=-1;++st.replaced;return q;}
        return -1;}
    // Synchronous submission for teardown (restore / hide): waits for anything in
    // flight, kicks the words, waits until ARM shows (iso, offset). Bounded.
    bool SubmitWait(const uint32_t* w,uint32_t n,uint32_t iso,uint32_t offset,uint32_t timeoutUs){
        const uint64_t t0=b->nowUs();
        while(state==FlipState::Armed&&b->nowUs()-t0<timeoutUs){(void)Poll();b->relax();}
        if(state!=FlipState::Idle)return false;
        queued=-1;
        if(!ArmWords(w,n,iso,offset))return false;
        while(state==FlipState::Armed&&b->nowUs()-t0<timeoutUs){(void)Poll();b->relax();}
        armed=-1;
        return state==FlipState::Idle;
    }
};
// Buffer-age repair (D2/D3). Each buffer accumulates the damage of every frame presented
// since it was last written (merged rects; an overflow widens to their bounds, never
// loses damage), so rendering into it copies exactly what it lacks plus this frame's
// damage. A never-written buffer (B and the third at bring-up) or `full` damage gets the
// whole area. The D2 version kept the last 4 frames only: the third buffer, used only
// when a flip was still armed at present time, was nearly always older than that and
// got a full 4K copy (22 ms over the gen1 x8 link: 2-3 missed frames, live de398730).
struct AgeRepair{
    static constexpr uint32_t kBufs=3;
    brook::DamageRects missing[kBufs]{};
    void Reset(){for(auto& m:missing)m=brook::DamageRects{};missing[1].full=missing[2].full=true;} // buffer 0 (A) holds the last GOP frame
    void Stale(int8_t bb){if(bb>=0&&bb<int8_t(kBufs)){missing[bb]=brook::DamageRects{};missing[bb].full=true;}}
    template<typename F> uint32_t Apply(int8_t bb,const brook::DamageRects& d,uint32_t w,uint32_t h,F copy){
        if(bb<0||bb>=int8_t(kBufs))return 0;
        brook::DamageRects& m=missing[bb];
        uint32_t n=0;
        if(m.full||d.full){copy(brook::DamageRect{0,0,int32_t(w),int32_t(h)});n=1;}
        else{for(uint32_t i=0;i<d.count;++i)m.Add(d.rects[i]);
             for(uint32_t i=0;i<m.count;++i){copy(m.rects[i]);++n;}}
        m=brook::DamageRects{};
        for(int8_t i=0;i<int8_t(kBufs);++i){
            if(i==bb||missing[i].full)continue;
            if(d.full){missing[i]=brook::DamageRects{};missing[i].full=true;continue;}
            for(uint32_t k=0;k<d.count;++k)missing[i].Add(d.rects[k]);
        }
        return n;
    }
};
// D3 frame cost: the compositor's damage is conservative (a cursor or taskbar redrawn
// with the same pixels, a whole window for every client commit, both windows of a
// move). Before anything crosses the GPU's gen1 x8 link, each damaged rect is compared
// with a RAM shadow of what the head shows; only the bounding box of pixels that really
// changed is copied (and stored into the shadow). No change on a head: no flip at all.
// Returns the pixels compared.
// 4 pixels equal (two 64-bit loads each side; the kernel has no memcmp).
inline bool Same4(const uint32_t* a,const uint32_t* b){
    uint64_t a0,a1,b0,b1;__builtin_memcpy(&a0,a,8);__builtin_memcpy(&a1,a+2,8);__builtin_memcpy(&b0,b,8);__builtin_memcpy(&b1,b+2,8);
    return ((a0^b0)|(a1^b1))==0;}
inline uint64_t ShadowDiff(const uint32_t* src,uint32_t srcStride,uint32_t* shadow,uint32_t shadowStride,
                           uint32_t width,uint32_t height,const brook::DamageRect& rIn,brook::DamageRects& out){
    const int32_t x0=rIn.x0<0?0:rIn.x0,y0=rIn.y0<0?0:rIn.y0;
    const int32_t x1=rIn.x1>int32_t(width)?int32_t(width):rIn.x1,y1=rIn.y1>int32_t(height)?int32_t(height):rIn.y1;
    if(x1<=x0||y1<=y0)return 0;
    int32_t bx0=x1,bx1=x0,by0=y1,by1=y0;
    const uint32_t n=uint32_t(x1-x0);
    for(int32_t y=y0;y<y1;++y){
        const uint32_t* a=src+uint64_t(y)*srcStride+x0;uint32_t* b=shadow+uint64_t(y)*shadowStride+x0;
        uint32_t i=0;
        while(i+4<=n&&Same4(a+i,b+i))i+=4;
        while(i<n&&a[i]==b[i])++i;
        if(i==n)continue;
        uint32_t j=n;
        while(j>=i+4&&Same4(a+j-4,b+j-4))j-=4;
        while(j>i&&a[j-1]==b[j-1])--j;
        __builtin_memcpy(b+i,a+i,uint64_t(j-i)*4);
        if(int32_t(x0+i)<bx0)bx0=x0+int32_t(i);
        if(int32_t(x0+j)>bx1)bx1=x0+int32_t(j);
        if(y<by0)by0=y;
        by1=y+1;
    }
    if(bx0<bx1&&by0<by1)out.Add({bx0,by0,bx1,by1});
    return uint64_t(n)*uint32_t(y1-y0);
}
} // namespace brook::nvdisp
