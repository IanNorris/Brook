#pragma once
// Gated live runner for the step-4 head-1 plan (nv_disp_head.h): head 1 ->
// SOR2 DP link A -> conn2 next to the GOP's head 0. Pure; the kernel glue
// (nv_disp_live.cpp) supplies the Backend. Verbs (bridge hardware nv-disp head1 ...):
//   plan                    print the op list, touch nothing
//   preflight               read-only phase; arms one enable this boot
//   enable [skip-step3] [stop N]  SOR power, DP train, core UPDATE + supervisors, verify;
//                           parks "enabled" (head 1 lit, run still live)
//   disable                 detach, supervisors, DisableLT, core fini (from "enabled")
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

// Registers the head-1 plan must never write (head 0, its OR SOR1, VPLL0, window 0).
struct Range{uint32_t lo,hi;};
constexpr Range kHead0Guard[]={
    {0x00ef00,0x00ef40},   // VPLL0
    {0x612200,0x612300},   // head 0 rgclk block
    {0x612400,0x612500},   // route macro 1 (SOR1)
    {0x612b00,0x612c00},   // SOR1 clock
    {0x616000,0x616800},   // head 0 RG/SF block
    {0x61c800,0x61d000},   // SOR1
    {0x682000,0x682400},{0x68a000,0x68a400}, // head-0 core ASSY/ARM (MMIO)
    {0x6104e4,0x6104e8},{0x610b30,0x610b40},{0x690000,0x691000}, // window 0 channel
};
inline bool Head0Guarded(uint32_t a){for(const auto& r:kHead0Guard)if(a>=r.lo&&a<r.hi)return true;return false;}
// Core methods that would touch head 0 / SOR1 / window 0 if pushed.
inline bool Head0Method(uint32_t m){return (m>=0x2000&&m<0x2400)||(m>=0x320&&m<0x340)||(m>=0x1000&&m<0x1080);}
inline bool PushTouchesHead0(const Push& p,uint32_t words){
    for(uint32_t i=0;i<words;){const uint32_t hdr=p.words[i++];if(hdr>>29){continue;}
        const uint32_t n=(hdr>>18)&0x3ff,m=hdr&0x3ffc;
        for(uint32_t k=0;k<n;++k)if(Head0Method(m+k*4))return true;
        i+=n;}
    return false;
}

struct HeadState{
    bool preflightOk=false,live=false,enabled=false,paused=false,hooked=false;
    uint32_t cursor=0,stopAt=~0u,failAt=~0u,runs=0,aborts=0,guardHits=0;
};

// Backend: as Runner's (rd, wr, vrd, vwr, fill, fence, nowUs, relax, emit, hookQuiesce)
// plus bool step3Busy(), bool step3Succeeded().
template<typename B> struct HeadRunner{
    B* b=nullptr;
    HeadConfig cfg{};DpOut dpout{};Vbios vb{};uint32_t auxCh=4;bool resolved=false;
    InstImage img{};HeadPushes pushes{};Plan plan{},abort{};HeadState st{};DpTrainResult tr{};

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
        void wr(uint32_t a,uint32_t v){if(Head0Guarded(a)){violation=true;++r.st.guardHits;r.Guard(a,v);return;}r.b->wr(a,v);}
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
            return 0;}
        void vcopy(uint64_t v,Blob k,uint32_t bytes){for(uint32_t i=0;i<bytes;i+=4)r.b->vwr(v+i,blob(k,i));}
        void fill(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){}
        void fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);r.b->fence();}
        bool expired(uint32_t ms,uint32_t& t){const uint64_t now=r.b->nowUs();if(!t){t=uint32_t(now)|1;return false;}
            r.b->relax();return uint32_t(now)-t>uint64_t(ms)*1000;}
        void udelay(uint32_t us){reg.udelay(us);}
        void except(uint32_t,uint32_t,uint32_t,uint32_t){}
        void diff(uint32_t m,uint32_t a,uint32_t c){
            Line l;l.raw("{\"nv_head1_diff\":\"");l.hex(m);l.raw("\",\"assy\":\"");l.hex(a);l.raw("\",\"arm\":\"");l.hex(c);
            l.raw("\",\"ours\":");l.dec(r.plan.Rewritten(m)?1:0);l.raw("}\n");r.b->emit(l.t);}
        void pause(const char*){}
        bool call(uint32_t id){return id==kCallDpTrain&&r.Train(reg);}
        void log(uint32_t i,const Step& s,uint32_t got,bool ok){
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
        Line m;m.raw("{\"nv_head1_dp\":1,\"cr\":");m.dec(tr.cr);m.raw(",\"eq\":");m.dec(tr.eq);m.raw(",\"cr_tries\":");m.dec(tr.crTries);
        m.raw(",\"eq_tries\":");m.dec(tr.eqTries);m.raw(",\"conf\":\"");m.hex(uint32_t(tr.conf[0])|tr.conf[1]<<8|tr.conf[2]<<16|uint32_t(tr.conf[3])<<24);
        m.raw("\",\"lanes\":\"");m.hex(uint32_t(tr.stat[0])|tr.stat[1]<<8|tr.stat[2]<<16);m.raw("\"}\n");b->emit(m.t);
        return ok;
    }

    bool Build(){
        if(!resolved){Msg("error","VBIOS tables not resolved (DCB[4]/DP table/PLL)");return false;}
        cfg.pauseAfterModeset=false; // "enabled" is the observer point
        img=InstImage{};pushes=HeadPushes{};plan=Plan{};
        if(BuildHeadPlan(cfg,img,pushes,plan)!=Err::Ok){Msg("error","head plan build failed");return false;}
        if(PushTouchesHead0(pushes.core,pushes.core.count)){Msg("error","core push touches head 0 / SOR1 / window 0");return false;}
        return true;
    }
    static bool ReadOnly(Op o){return o==Op::Expect||o==Op::ExpectNot||o==Op::Log||o==Op::AssyArmDiff;}
    void Status(){
        Line l;l.raw("{\"nv_head1_status\":1,\"preflight_ok\":");l.dec(st.preflightOk);l.raw(",\"live\":");l.dec(st.live);
        l.raw(",\"enabled\":");l.dec(st.enabled);l.raw(",\"paused\":");l.dec(st.paused);l.raw(",\"cursor\":");l.dec(st.cursor);
        l.raw(",\"runs\":");l.dec(st.runs);l.raw(",\"aborts\":");l.dec(st.aborts);l.raw(",\"guard_hits\":");l.dec(st.guardHits);
        l.raw(",\"vpll\":\"");l.hex((cfg.vpll.P<<16)|(cfg.vpll.N<<8)|cfg.vpll.M);l.raw("\",\"fN\":\"");l.hex(cfg.vpll.fN);
        l.raw("\",\"watermark\":");l.dec(cfg.sf.watermark);l.raw("}\n");b->emit(l.t);
    }
    bool Busy(){if(st.live||st.paused){Msg("refused","a head-1 run is live");return true;}
        if(b->step3Busy()){Msg("refused","a step-3 run is live");return true;}return false;}
    void Preflight(){
        if(Busy()||!Build())return;
        const uint32_t end=plan.phaseBegin[uint32_t(HPhase::Vram)];
        for(uint32_t i=0;i<end;++i)if(!ReadOnly(plan.steps[i].op)){Msg("error","preflight contains a write op");return;}
        Io io{*this,"preflight"};
        st.preflightOk=Run(plan,0,end,io)==end;
        Msg(st.preflightOk?"preflight":"preflight-failed",st.preflightOk?"ok: head1 enable armed for this boot":"mismatch: nothing armed");
    }
    void Enable(bool skipStep3,uint32_t stopAt){
        if(Busy())return;
        if(!st.preflightOk){Msg("refused","run head1 preflight first (this boot)");return;}
        if(!skipStep3&&!b->step3Succeeded()){Msg("refused","a step-3 noop/flip must succeed first this boot (or skip-step3)");return;}
        if(!Build())return;
        st.cursor=0;st.stopAt=stopAt;st.failAt=~0u;st.preflightOk=false;st.enabled=false;++st.runs;
        RunTo(plan.phaseBegin[uint32_t(HPhase::Disable)],true);
    }
    void Disable(){
        if(!st.enabled){Msg("refused","head 1 is not enabled");return;}
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
            const uint32_t i=st.cursor;const Step& s=plan.steps[i];
            if(resume)resume=false;
            else if(i==st.stopAt||s.op==Op::Pause){st.paused=true;Msg("paused",s.op==Op::Pause?s.what:"stop point");Status();return;}
            if(s.op==Op::Pause)continue;
            if(i>=vramBegin&&!st.live){st.live=true;if(!st.hooked){b->hookQuiesce();st.hooked=true;}}
            Io io{*this,"run"};
            const bool ok=Run(plan,i,i+1,io)==i+1&&!io.reg.violation;
            if(!ok){st.failAt=i;Msg("failed",io.reg.violation?"head-0 write guard":"auto-abort");RunAbort(PhaseOf(i),100);return;}
        }
        if(end==plan.count){st.live=false;Msg("done","head 1 disabled; run head1 preflight again before the next enable");}
        else{st.enabled=true;Msg("enabled","head 1 lit on conn2; head1 disable when done");}
        Status();
    }
    void RunAbort(HPhase reached,uint32_t ms){
        abort=Plan{};BuildHeadAbort(cfg,reached,pushes,abort,ms);
        Io io{*this,"abort"};(void)Run(abort,0,abort.count,io,true);
        st.live=st.enabled=st.paused=st.preflightOk=false;++st.aborts;
        Msg("aborted","head abort list ran");Status();
    }
    void Abort(bool force){
        if(!st.live&&!st.paused&&!force){Msg("refused","nothing live (use abort force)");return;}
        if(!plan.count&&!Build())return;
        RunAbort(st.live||st.paused?PhaseOf(st.cursor):HPhase::Disable,100);
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
        if(Word(p,"continue")){Continue();return;}
        if(Word(p,"abort")){Abort(Word(p,"force"));return;}
        if(Word(p,"enable")){
            bool skip=false;uint32_t stop=~0u;
            for(;;){while(*p==' ')++p;if(!*p)break;
                if(Word(p,"skip-step3"))skip=true;
                else if(Word(p,"stop")){if(!Num(p,stop)){Msg("usage","stop <step>");return;}}
                else{Msg("usage","unknown argument");return;}}
            Enable(skip,stop);return;}
        Msg("usage","nv-disp head1 plan|preflight|enable [skip-step3] [stop N]|disable|continue|abort [force]|status");
    }
};
} // namespace brook::nvdisp
