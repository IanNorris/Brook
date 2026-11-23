#pragma once
// Gated live runner for the step-4 head-1 plan (nv_disp_head.h): head 1 ->
// SOR2 DP link A -> conn2 next to the GOP's head 0. Pure; the kernel glue
// (nv_disp_live.cpp) supplies the Backend. Verbs (bridge hardware nv-disp head1 ...):
//   plan                    print the op list, touch nothing
//   preflight               read-only phase; arms one enable this boot
//   enable [skip-step3] [stop N]  SOR power, DP train, core UPDATE + supervisors, verify;
//                           parks "enabled" (head 1 lit, run still live)
//   disable                 detach, supervisors, DisableLT, core fini (from "enabled");
//                           hides a shown window first
//   show / hide             content on head 1 (from "enabled"): window 2 channel, a fresh
//                           surface (blue + white bar) / window off and channel restored
//   unblank                 separate SET_GET_BLANKING_CTRL(1) UNBLANK + UPDATE
//   continue                resume after a stop point
//   abort [force]           the head abort list (supervisor flush, detach, DisableLT, core fini)
//   status
// Rules: preflight required this boot; enable also requires a successful
// step-3 run this boot (noop/flip) unless `skip-step3`; refused while a
// step-3 run is live; any failure auto-aborts; a write guard refuses every
// MMIO write to head-0 / SOR1 / VPLL0 / window-0 registers and aborts; the
// core pushbuffer is rejected if it holds any head-0, SOR1 or window-0 method.
#include "nv_disp_head.h"
#include "nv_aux_xfer.h"

namespace brook::nvdisp {

struct HeadState{
    bool preflightOk=false,live=false,enabled=false,paused=false,hooked=false,head0Flagged=false,fe=false,dpIdleHeld=false,windowLive=false;uint32_t softWarnings=0;
    Kicked kicked=Kicked::None;uint32_t openStage=0; // a supervisor stage acked by the plan and not yet released
    uint32_t cursor=0,stopAt=~0u,failAt=~0u,runs=0,aborts=0,guardHits=0;
};

// Backend: as Runner's (rd, wr, vrd, vwr, fill, fence, nowUs, relax, emit, hookQuiesce)
// plus bool step3Busy(), bool step3Succeeded(), void dpIdleSuspend(bool).
template<typename B> struct HeadRunner{
    B* b=nullptr;
    HeadConfig cfg{};DpOut dpout{};Vbios vb{};uint32_t auxCh=4;bool resolved=false;
    InstImage img{};HeadPushes pushes{};Plan plan{},abort{};HeadState st{};DpTrainResult tr{};HeadWindow win{};
    // Resident driver mode: quiet = only failures emitted; coexist = enable while step 3 is
    // live (shared instmem); winExternal = the driver's flipper owns window 2's ring.
    bool quiet=false,coexist=false,winExternal=false;uint32_t quietSteps=0,quietDiffs=0;

    struct Line{char t[320];uint32_t n=0;
        void s(const char* x){while(*x&&n+2<sizeof(t)){char ch=*x++;t[n++]=ch=='"'||ch=='\\'?'\'':ch;}t[n]=0;}
        void raw(const char* x){while(*x&&n+2<sizeof(t))t[n++]=*x++;t[n]=0;}
        void hex(uint64_t v){char d[17];int k=0;do{d[k++]="0123456789abcdef"[v&15];v>>=4;}while(v&&k<16);while(k)t[n<sizeof(t)-2?n++:n]=d[--k];t[n]=0;}
        void dec(uint32_t v){char d[11];int k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v);while(k)t[n<sizeof(t)-2?n++:n]=d[--k];t[n]=0;}
    };
    void Msg(const char* kind,const char* text){Line l;l.raw("{\"nv_head1\":\"");l.s(kind);l.raw("\",\"msg\":\"");l.s(text);l.raw("\"}\n");b->emit(l.t);}
    HPhase PhaseOf(uint32_t i)const{HPhase ph=HPhase::Preflight;
        for(uint32_t k=0;k<uint32_t(HPhase::Count);++k)if(plan.phaseBegin[k]<=i&&plan.phaseBegin[k+1]>i)ph=HPhase(k);return ph;}

    // Register Io with the head-0 write guard.
    struct RegIo{HeadRunner& r;bool violation=false;
        uint32_t rd(uint32_t a){return r.b->rd(a);}
        void wr(uint32_t a,uint32_t v){if(!HeadAllowed(a)){violation=true;++r.st.guardHits;r.Guard(a,v);return;}r.b->wr(a,v);
            if(a==kRegSuperAck)r.st.openStage=v&7;                       // stage acked
            if(a==kRegSuperStat&&v==kSuperRelease)r.st.openStage=0;}     // stage released
        void udelay(uint32_t us){const uint64_t end=r.b->nowUs()+us;while(r.b->nowUs()<end)r.b->relax();}
    };
    void Guard(uint32_t a,uint32_t v){Line l;l.raw("{\"nv_head1_guard\":\"");l.hex(a);l.raw("\",\"value\":\"");l.hex(v);l.raw("\"}\n");b->emit(l.t);}
    // Executor Io.
    struct Io{
        HeadRunner& r;const char* tag;RegIo reg{r};
        uint32_t rd(uint32_t a){return reg.rd(a);} void wr(uint32_t a,uint32_t v){reg.wr(a,v);}
        uint32_t vrd(uint64_t a){return r.b->vrd(a);} void vwr(uint64_t a,uint32_t v){r.b->vwr(a,v);}
        uint32_t blob(Blob k,uint32_t off){
            if(k==Blob::Inst)return r.img.words[off/4];
            if(k==Blob::CorePush)return off/4<r.pushes.core.count?r.pushes.core.words[off/4]:0;
            if(k==Blob::WndwPush)return off/4<r.win.ring.count?r.win.ring.words[off/4]:0;
            return 0;}
        void vcopy(uint64_t v,Blob k,uint32_t bytes){for(uint32_t i=0;i<bytes;i+=4)r.b->vwr(v+i,blob(k,i));}
        void fill(uint64_t v,uint32_t bytes,uint32_t pitch,uint32_t c,uint32_t m,uint32_t lines){r.b->fill(v,bytes,pitch,c,m,lines);}
        void fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);r.b->fence();}
        bool expired(uint32_t ms,uint32_t& t){const uint64_t now=r.b->nowUs();if(!t){t=uint32_t(now)|1;return false;}
            uint32_t el=uint32_t(now)-t;if(el>0x80000000u)el=0;  // t|1 may be 1 us ahead (step-3 runner note)
            r.b->relax();return el>uint64_t(ms)*1000;}
        void udelay(uint32_t us){reg.udelay(us);}
        void except(uint32_t chid,uint32_t es,uint32_t d,uint32_t c){ // review N3
            Line l;l.raw("{\"nv_head1_exc\":1,\"chid\":");l.dec(chid);l.raw(",\"stat\":\"");l.hex(es);l.raw("\",\"data\":\"");l.hex(d);
            l.raw("\",\"code\":\"");l.hex(c);l.raw("\",\"mthd\":\"");l.hex((es&0xfff)<<2);l.raw("\",\"type\":");l.dec((es>>12)&7);
            l.raw("}\n");r.b->emit(l.t);}
        void diff(uint32_t m,uint32_t a,uint32_t c){if(r.quiet){++r.quietDiffs;return;}
            Line l;l.raw("{\"nv_head1_diff\":\"");l.hex(m);l.raw("\",\"assy\":\"");l.hex(a);l.raw("\",\"arm\":\"");l.hex(c);
            l.raw("\",\"ours\":");l.dec(r.plan.Rewritten(m)?1:0);l.raw("}\n");r.b->emit(l.t);}
        void pause(const char*){}
        void dump(uint32_t addr,const uint32_t* w,uint32_t n){
            Line l;l.raw("{\"nv_head1_dump\":\"");l.hex(addr);l.raw("\",\"w\":[");
            for(uint32_t i=0;i<n;++i){if(i)l.raw(",");l.raw("\"");l.hex(w[i]);l.raw("\"");}
            l.raw("]}\n");r.b->emit(l.t);}
        void flag(uint32_t i,const Step& s,uint32_t got){
            // Only the supervisor head-0 check means "head 0 flagged"; other soft checks
            // (pclk / VPLL read-backs, guarded writes) are plain warnings (third review).
            if(s.op==Op::ExpectSoft&&s.reg==RegSuperHead(0))r.st.head0Flagged=true;else ++r.st.softWarnings;
            Line l;l.raw("{\"nv_head1\":\"WARNING\",\"step\":");l.dec(i);l.raw(",\"what\":\"");l.s(s.what);
            l.raw("\",\"got\":\"");l.hex(got);l.raw("\"}\n");r.b->emit(l.t);}
        void note(const char* what,uint32_t v){Line l;l.raw("{\"nv_head1_note\":\"");l.s(what);l.raw("\",\"v\":\"");l.hex(v);l.raw("\"}\n");r.b->emit(l.t);}
        bool call(uint32_t id,uint32_t ms){
            if(id==kCallDpTrain)return r.Train(reg);
            if(id==kCallSuperFlush||id==kCallSuperFlushVpll){
                (void)SupervisorFlush(*this,r.cfg,id==kCallSuperFlushVpll,ms?ms:100,r.st.openStage);return true;}
            return false;}
        void log(uint32_t i,const Step& s,uint32_t got,bool ok){
            if(r.quiet&&ok){++r.quietSteps;return;}
            Line l;l.raw("{\"nv_head1_step\":");l.dec(i);l.raw(",\"list\":\"");l.s(tag);l.raw("\",\"ph\":\"");l.s(HPhaseName(r.PhaseOf(i)));
            l.raw("\",\"reg\":\"");l.hex(s.op==Op::VramCopy||s.op==Op::VramWrite||s.op==Op::VramPoll?s.vram:s.reg);
            l.raw("\",\"exp\":\"");l.hex(s.value);l.raw("\",\"got\":\"");l.hex(got);l.raw("\",\"ok\":");l.dec(ok?1:0);
            l.raw(",\"what\":\"");l.s(s.what);l.raw("\"}\n");r.b->emit(l.t);}
    };
    // DP training over AUX (nv_aux_xfer.h) with the VBIOS drive table.
    bool Train(RegIo& reg){
        NvAux<RegIo> aux{reg,auxCh};
        if(int rc=aux.Acquire()){Msg("dp","AUX acquire failed");(void)rc;aux.Release();return false;}
        DpLink l=cfg.dp;bool ok=false;
        if(aux.Dpcd(true,0x000,l.dpcd,16)==0){
            if(l.dpcd[0]<0x11||l.dpcd[1]<l.bw||(l.dpcd[2]&0x1f)<l.nr)Msg("dp","sink caps below the planned link");
            else{
                struct TIo{RegIo& reg;NvAux<RegIo>& ax;HeadRunner& r;
                    uint32_t rd(uint32_t a){return reg.rd(a);} void wr(uint32_t a,uint32_t v){reg.wr(a,v);}
                    void udelay(uint32_t us){reg.udelay(us);}
                    int aux(bool read,uint32_t a,uint8_t* d,uint32_t n){return ax.Dpcd(read,a,d,n);}
                    bool drive(uint32_t,uint8_t pc,uint8_t vs,uint8_t pe,DpCfg& o){return DpDrive(r.vb,r.dpout,pc,vs,pe,o);}
                } t{reg,aux,*this};
                ok=DpTrain(t,l,tr)&&!reg.violation;
            }
        }else Msg("dp","DPCD caps read failed");
        aux.Release();
        if(tr.spreadNote)Msg("note","0x00ea80 bit4 (source spread) is set but the sink lacks DPCD 0x003 bit0; DPCD 0x107 bit4 left clear");
        Line m;m.raw("{\"nv_head1_dp\":1,\"cr\":");m.dec(tr.cr);m.raw(",\"eq\":");m.dec(tr.eq);m.raw(",\"cr_tries\":");m.dec(tr.crTries);
        m.raw(",\"eq_tries\":");m.dec(tr.eqTries);m.raw(",\"conf\":\"");m.hex(uint32_t(tr.conf[0])|tr.conf[1]<<8|tr.conf[2]<<16|uint32_t(tr.conf[3])<<24);
        m.raw("\",\"lanes\":\"");m.hex(uint32_t(tr.stat[0])|tr.stat[1]<<8|tr.stat[2]<<16);m.raw("\"}\n");b->emit(m.t);
        return ok;
    }

    bool Build(){
        if(!resolved){Msg("error","VBIOS tables/scripts/hash not matched (DCB[4], DP table, PLL, script pointers)");return false;}
        cfg.pauseAfterModeset=false; // "enabled" is the observer point
        cfg.base.feEnable=st.fe;
        if(b)ReadCoreReplay(cfg,[&](uint32_t a){return b->rd(a);}); // replay = ARM values now
        img=InstImage{};pushes=HeadPushes{};plan=Plan{};
        const Err e=BuildHeadPlan(cfg,img,pushes,plan);
        if(e==Err::Usage){Msg("error","core push would change a head-0 / SOR1 / window-0 method");return false;}
        if(e!=Err::Ok){Msg("error","head plan build failed");return false;}
        return true;
    }
    Kicked KickOf(uint32_t put)const{
        return put==pushes.replayEnd?Kicked::Replay:put==pushes.rungEnd?Kicked::Rung:put==pushes.enableEnd?Kicked::Enable:
               put==pushes.disableEnd?Kicked::Disable:Kicked::None;}
    void DpIdle(bool suspend){if(suspend!=st.dpIdleHeld){b->dpIdleSuspend(suspend);st.dpIdleHeld=suspend;}}
    static bool ReadOnly(Op o){return o==Op::Expect||o==Op::ExpectOr||o==Op::ExpectNot||o==Op::Log||o==Op::AssyArmDiff||o==Op::ExpectVpll||o==Op::ExpectSoft;}
    void Status(){
        Line l;l.raw("{\"nv_head1_status\":1,\"preflight_ok\":");l.dec(st.preflightOk);l.raw(",\"live\":");l.dec(st.live);
        l.raw(",\"enabled\":");l.dec(st.enabled);l.raw(",\"soft_warnings\":");l.dec(st.softWarnings);l.raw(",\"window\":");l.dec(st.windowLive);l.raw(",\"paused\":");l.dec(st.paused);l.raw(",\"cursor\":");l.dec(st.cursor);
        l.raw(",\"runs\":");l.dec(st.runs);l.raw(",\"aborts\":");l.dec(st.aborts);l.raw(",\"guard_hits\":");l.dec(st.guardHits);
        l.raw(",\"vpll\":\"");l.hex((cfg.vpll.P<<16)|(cfg.vpll.N<<8)|cfg.vpll.M);l.raw("\",\"fN\":\"");l.hex(cfg.vpll.fN);
        l.raw("\",\"watermark\":");l.dec(cfg.sf.watermark);l.raw("}\n");b->emit(l.t);
    }
    bool Busy(){if(st.live||st.paused){Msg("refused","a head-1 run is live");return true;}
        if(!coexist&&b->step3Busy()){Msg("refused","a step-3 run is live");return true;}return false;}
    void Preflight(){
        if(Busy()||!Build())return;
        const uint32_t end=plan.phaseBegin[uint32_t(HPhase::Vram)];
        for(uint32_t i=0;i<end;++i)if(!ReadOnly(plan.steps[i].op)){Msg("error","preflight contains a write op");return;}
        Io io{*this,"preflight"};
        st.preflightOk=Run(plan,0,end,io)==end;
        Msg(st.preflightOk?"preflight":"preflight-failed",st.preflightOk?"ok: head1 enable armed for this boot":"mismatch: nothing armed");
    }
    // Named stop points, resolved on the built plan: "rung" = just before the rung kick
    // (everything up to the strict post-replay diff), "kick" = just before the enable kick.
    uint32_t NamedStop(uint32_t code)const{
        const uint32_t put=code==1?pushes.rungEnd:pushes.enableEnd;
        for(uint32_t i=0;i<plan.count;++i)if(plan.steps[i].op==Op::Kick&&plan.steps[i].value==put)return i>0&&plan.steps[i-1].op==Op::VramWrite?i-1:i;
        return ~0u;}
    void Enable(bool skipStep3,uint32_t stopAt,bool fe,uint32_t named=0){
        if(Busy())return;
        if(!st.preflightOk){Msg("refused","run head1 preflight first (this boot)");return;}
        if(!skipStep3&&!b->step3Succeeded()){Msg("refused","a step-3 noop/flip must succeed first this boot (or skip-step3)");return;}
        st.fe=fe;if(!Build())return;
        if(named)stopAt=NamedStop(named);
        st.cursor=0;st.stopAt=stopAt;st.failAt=~0u;st.preflightOk=false;st.enabled=false;st.head0Flagged=false;
        st.kicked=Kicked::None;++st.runs;
        DpIdle(true); // dp-idle idle-off and link watch suspended for the session
        RunTo(plan.phaseBegin[uint32_t(HPhase::Disable)],true);
    }
    void Disable(){
        if(!st.enabled){Msg("refused","head 1 is not enabled");return;}
        if(st.windowLive&&!Hide())return;                // window off before the core drops its owner
        st.enabled=false;RunTo(plan.count,true);
    }
    void Continue(){
        if(!st.paused){Msg("refused","nothing paused");return;}
        RunTo(st.cursor<plan.phaseBegin[uint32_t(HPhase::Disable)]?plan.phaseBegin[uint32_t(HPhase::Disable)]:plan.count,false);
    }
    void RunTo(uint32_t end,bool fresh){
        bool resume=!fresh;st.paused=false;
        const uint32_t vramBegin=plan.phaseBegin[uint32_t(HPhase::Vram)];
        for(;st.cursor<end;++st.cursor){
            // Head 0 flagged during the modeset: all stages were finished with the head-1
            // work; skip verify and run a normal disable (review issue 3).
            if(st.head0Flagged&&st.cursor==plan.phaseBegin[uint32_t(HPhase::Verify)]){
                Msg("WARNING","head 0 was flagged by a supervisor: skipping verify, running a normal disable");
                st.cursor=plan.phaseBegin[uint32_t(HPhase::Disable)];end=plan.count;resume=true;
                if(st.cursor>=end)break;}
            const uint32_t i=st.cursor;const Step& s=plan.steps[i];
            if(resume)resume=false;
            else if(i==st.stopAt||s.op==Op::Pause){st.paused=true;Msg("paused",s.op==Op::Pause?s.what:"stop point");Status();return;}
            if(s.op==Op::Pause)continue;
            if(i>=vramBegin&&!st.live){st.live=true;if(!st.hooked){b->hookQuiesce();st.hooked=true;}}
            if(s.op==Op::Kick){const Kicked k=KickOf(s.value);if(k!=Kicked::None)st.kicked=k;}
            Io io{*this,"run"};
            const bool ok=Run(plan,i,i+1,io)==i+1&&!io.reg.violation;
            if(ok&&s.op==Op::VramPoll&&st.kicked==Kicked::Rung)st.kicked=Kicked::None; // rung UPDATE completed: nothing to flush
            if(!ok){st.failAt=i;
                Msg("failed",io.reg.violation?"write outside the head-1 allow-list":"auto-abort");
                RunAbort(PhaseOf(i),2000);return;}
        }
        if(end==plan.count){st.live=false;DpIdle(false);Msg("done","head 1 detached (VPLL1 left running and head-1 raster ARMed until reset); run head1 preflight again before the next enable");}
        else{st.enabled=true;Msg("enabled","head 1 lit on conn2; head1 disable when done");}
        Status();
    }
    // Separate UNBLANK of head 1 while it runs (only if the post-modeset read-back says 1).
    void Unblank(){
        if(!st.enabled||!st.live){Msg("refused","head 1 is not enabled");return;}
        abort=Plan{};
        AddCoreKick(abort,cfg,pushes.unblankEnd,"core PUT: SET_GET_BLANKING_CTRL(1) = UNBLANK, UPDATE");
        abort.Add(Op::PollOr,RegGet(kChidCore),~0u,pushes.unblankEnd,kTimeoutMs,"core GET == PUT (fails at once on a supervisor)",
                  (uint64_t(7)<<48)|(uint64_t(kRegCtrlDisp)<<24)|kRegSuperAck,RegGet(kChidCore),kRegExcOther,1);
        abort.Add(Op::VramPoll,0,3u<<kNtfyStatusShift,kNtfyFinished<<kNtfyStatusShift,kTimeoutMs,"core notifier FINISHED",cfg.base.syncVram+0);
        abort.Add(Op::Expect,kRegExcOther,1,0,0,"no core exception");
        abort.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(0)),~0u,cfg.base.gop.headRaster,0,"head 0 raster unchanged");
        abort.Add(Op::Log,RegAssy(kChidCore,0x240+cfg.head*4),0,0,0,"HEAD 1 BLANKING (0x680244) after unblank: 2 = unblanked");
        abort.End();
        st.kicked=Kicked::Enable;
        Io io{*this,"unblank"};const bool ok=Run(abort,0,abort.count,io)==abort.count&&!io.reg.violation;
        if(!ok){Msg("failed","unblank: auto-abort");RunAbort(HPhase::Disable,2000);return;}
        Msg("unblanked","head 1 UNBLANK pushed; head1 disable when done");Status();
    }
    // Content on head 1 through window 2 (owned by head 1 since the enable UPDATE).
    void Show(){
        if(!st.enabled||!st.live){Msg("refused","head 1 is not enabled");return;}
        if(st.windowLive){Msg("refused","a window is already shown on head 1");return;}
        const uint32_t ch=ChidWndw(cfg.window);
        win.initCtl=b->rd(RegChnCtl(ch));
        win.initPb[0]=b->rd(RegPbLo(ch));win.initPb[1]=b->rd(RegPbHi(ch));win.initPb[2]=b->rd(RegPbValid(ch));win.initPb[3]=b->rd(RegPbClient(ch));
        win.win0Iso=b->rd(RegArm(ChidWndw(0),c57e::SET_CONTEXT_DMA_ISO0));win.checkWin0Iso=!coexist;
        for(uint32_t i=0;i<kReplayCount;++i)win.replayVal[i]=b->rd(RegArm(ChidWndw(0),kReplayMthds[i])); // GOP's working window state
        BuildHeadWindowPushes(cfg,win);
        if(win.ring.overflow){Msg("error","window push overflow");return;}
        abort=Plan{};BuildHeadWindowShow(cfg,win,abort);
        st.windowLive=true;                              // from here on aborts include the window
        Io io{*this,"show"};const uint32_t at=Run(abort,0,abort.count,io);
        if(at!=abort.count||io.reg.violation){
            Msg("failed",io.reg.violation?"write outside the head-1 allow-list":"show: window abort (head 1 stays enabled)");
            WindowAbort(2000);return;}
        Msg("shown","head-1 surface on conn2 (blue, white bar at the top); head1 hide or head1 disable when done");Status();
    }
    bool Hide(){
        if(!st.windowLive){Msg("refused","no window shown on head 1");return false;}
        abort=Plan{};BuildHeadWindowHide(cfg,win,abort);
        Io io{*this,"hide"};const uint32_t at=Run(abort,0,abort.count,io);
        if(at!=abort.count||io.reg.violation){Msg("failed","hide: window abort");WindowAbort(2000);return st.windowLive==false;}
        st.windowLive=false;Msg("hidden","window 2 off, channel restored; head 1 still enabled");Status();return true;
    }
    // Driver: the flipper already pushed ISO 0; disconnect and restore the channel.
    bool WindowFini(){
        if(!st.windowLive)return true;
        abort=Plan{};BuildHeadWindowHide(cfg,win,abort,false);
        Io io{*this,"window-fini"};const uint32_t at=Run(abort,0,abort.count,io);
        if(at!=abort.count||io.reg.violation){Msg("failed","window fini: window abort");WindowAbort(2000);return false;}
        st.windowLive=false;return true;
    }
    void WindowAbort(uint32_t ms){
        if(!st.windowLive)return;
        abort=Plan{};BuildHeadWindowAbort(cfg,win,abort,ms,!winExternal);
        Io io{*this,"window-abort"};(void)Run(abort,0,abort.count,io,true);
        st.windowLive=false;
    }
    void RunAbort(HPhase reached,uint32_t ms){
        WindowAbort(ms);                                 // window first: it scans out through head 1
        abort=Plan{};BuildHeadAbort(cfg,reached,pushes,abort,ms,st.kicked);
        Io io{*this,"abort"};(void)Run(abort,0,abort.count,io,true);
        st.live=st.enabled=st.paused=st.preflightOk=false;++st.aborts;DpIdle(false);
        Msg("aborted","head abort list ran");Status();
    }
    void Abort(bool force){
        if(!st.live&&!st.paused&&!force){Msg("refused","nothing live (use abort force)");return;}
        if(!plan.count&&!Build())return;
        RunAbort(st.live||st.paused?PhaseOf(st.cursor):HPhase::Disable,2000); // manual abort: 2 s flush rounds (review N6)
    }
    void Quiesce(){if(st.live||st.paused)RunAbort(PhaseOf(st.cursor),50);}
    void PrintPlan(){
        if(st.live){Msg("refused","a head-1 run is live");return;}
        if(!Build())return;
        for(uint32_t i=0;i<plan.count;++i){Io io{*this,"plan"};io.log(i,plan.steps[i],0,true);}
        Msg("plan","end");
    }
    static bool Word(const char*& p,const char* w){
        while(*p==' ')++p;const char* q=p;while(*w){if(*q!=*w)return false;++q;++w;}
        if(*q&&*q!=' ')return false;p=q;return true;}
    static bool Num(const char*& p,uint32_t& v){
        while(*p==' ')++p;if(*p<'0'||*p>'9')return false;v=0;
        while(*p>='0'&&*p<='9'){v=v*10+uint32_t(*p-'0');++p;if(v>1000000)return false;}return true;}
    void Command(const char* p){
        if(Word(p,"status")){Status();return;}
        if(Word(p,"plan")){PrintPlan();return;}
        if(Word(p,"preflight")){Preflight();return;}
        if(Word(p,"disable")){Disable();return;}
        if(Word(p,"unblank")){Unblank();return;}
        if(Word(p,"show")){Show();return;}
        if(Word(p,"hide")){(void)Hide();return;}
        if(Word(p,"continue")){Continue();return;}
        if(Word(p,"abort")){Abort(Word(p,"force"));return;}
        if(Word(p,"enable")){
            bool skip=false,fe=false;uint32_t stop=~0u,named=0;
            for(;;){while(*p==' ')++p;if(!*p)break;
                if(Word(p,"skip-step3"))skip=true;
                else if(Word(p,"fe"))fe=true;
                else if(Word(p,"stop")){if(Word(p,"rung"))named=1;else if(Word(p,"kick"))named=2;
                    else if(!Num(p,stop)){Msg("usage","stop <step>|rung|kick");return;}}
                else{Msg("usage","unknown argument");return;}}
            Enable(skip,stop,fe,named);return;}
        Msg("usage","nv-disp head1 plan|preflight|enable [skip-step3] [fe] [stop N|rung|kick]|disable|show|hide|unblank|continue|abort [force]|status");
    }
};
} // namespace brook::nvdisp
