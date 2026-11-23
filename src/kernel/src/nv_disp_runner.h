#pragma once
// Gated live runner for the RTX 2080 step-3 plan (nv_disp_plan.h), kept pure so
// the host tests drive it against the fake display engine. The kernel side
// (nv_disp_live.cpp) only supplies the Backend: BAR0/BAR1 access, time, and
// the JSON sink. Verbs (bridge hardware nv-disp ...):
//   plan [flips n] [a|a2|b]       print the op list, touch nothing
//   preflight                     phase 0 only (reads); arms noop/flip for this boot
//   noop [a|a2|b] [pause] [stop N] bring-up, first flip (no visible change), restore, teardown
//   flip <n> <ms> [a|a2|b] [stop N] noop plus n A/B flips held <ms> each
//   continue                      resume after a PAUSE or stop
//   abort [force]                 run the abort list (only when live, unless force)
//   status                        report the runner state
// Rules: nothing runs at boot; noop/flip refuse unless preflight passed this
// boot and no run is live; any failed expectation/timeout auto-aborts (with a
// restore attempt when a flip had already worked); every run re-arms only
// after a new preflight.
#include "nv_disp_plan.h"

namespace brook::nvdisp {

// Layout for a GOP surface at VRAM 0, with BAR1 a VRAM identity map of bar1Size bytes.
inline Config GopFlipConfig(uint32_t width,uint32_t height,uint32_t pitch,uint32_t format,uint64_t bar1Size){
    Config c;const uint64_t bytes=uint64_t(pitch)*height,two=2ull<<20;
    c.surf[0]={0,width,height,pitch,format};
    c.surf[1]={(bytes+two-1)&~(two-1),width,height,pitch,format};
    c.instVram=(bar1Size-(1ull<<20))&~0xfffffull;
    c.wndwPushVram=c.instVram+0x10000;c.corePushVram=c.instVram+0x11000;c.syncVram=c.instVram+0x12000;
    c.isoLimit=bar1Size-1;
    return c;
}

enum class Stage:uint8_t{A,A2,B};
struct RunState{
    bool preflightOk=false;   // this boot, since the last run
    bool live=false;          // a write-phase step has run and the teardown/abort has not finished
    bool paused=false;
    bool hooked=false;        // quiesce hook registered
    bool firstFlipOk=false;
    uint32_t cursor=0,stopAt=~0u,failAt=~0u,runs=0,aborts=0,done=0; // done: runs that reached teardown's end
    uint32_t nflips=0,holdMs=0;Stage stage=Stage::A;bool observer=false;
};

// Backend: uint32_t rd(uint32_t); void wr(uint32_t,uint32_t); uint32_t vrd(uint64_t);
// void vwr(uint64_t,uint32_t); void fill(...) (see Op::SurfFill); uint64_t nowUs();
// void relax(); void emit(const char* line); void hookQuiesce();
// Constant-initialisable (kernel globals have no .init_array): set `b` before use.
template<typename B> struct Runner{
    B* b=nullptr;Config base{};RunState st{};
    InstImage img{};Pushes pushes{};Plan plan{},abort{};Config cfg{};

    // ---- JSON helpers (fixed buffer, no allocation)
    struct Line{char t[320];uint32_t n=0;
        void s(const char* x){while(*x&&n+2<sizeof(t)){char ch=*x++;t[n++]=ch=='"'||ch=='\\'?'\'':ch;}t[n]=0;}
        void raw(const char* x){while(*x&&n+2<sizeof(t))t[n++]=*x++;t[n]=0;}
        void hex(uint64_t v){char d[17];int k=0;do{d[k++]="0123456789abcdef"[v&15];v>>=4;}while(v&&k<16);while(k)t[n<sizeof(t)-2?n++:n]=d[--k];t[n]=0;}
        void dec(uint32_t v){char d[11];int k=0;do{d[k++]=char('0'+v%10);v/=10;}while(v);while(k)t[n<sizeof(t)-2?n++:n]=d[--k];t[n]=0;}
    };
    static const char* OpName(Op o){
        static const char* n[]={"WR","MASK","EXPECT","POLL","POLLSOFT","ACK","VCOPY","VWR","VEXPECT","VPOLL","VPOLLANY","ACKEXC",
            "PRAMIN","FENCE","KICK","EXPNOT","LOG","COPY","ASSYARM","PAUSE","DELAY","ANDNIF","COPYMASK","CALL","FILL"};
        return uint32_t(o)<sizeof(n)/sizeof(n[0])?n[uint32_t(o)]:"?";
    }
    Phase PhaseOf(uint32_t i)const{Phase ph=Phase::Preflight;
        for(uint32_t k=0;k<uint32_t(Phase::Count);++k)if(plan.phaseBegin[k]<=i&&plan.phaseBegin[k+1]>i)ph=Phase(k);
        return ph;}
    void Msg(const char* kind,const char* text){Line l;l.raw("{\"nv_disp\":\"");l.s(kind);l.raw("\",\"msg\":\"");l.s(text);l.raw("\"}\n");b->emit(l.t);}

    // ---- Io adapter for brook::nvdisp::Run
    struct Io{
        Runner& r;const Plan& p;const char* tag;
        uint32_t rd(uint32_t a){return r.b->rd(a);}
        void wr(uint32_t a,uint32_t v){r.b->wr(a,v);}
        uint32_t vrd(uint64_t a){return r.b->vrd(a);}
        void vwr(uint64_t a,uint32_t v){r.b->vwr(a,v);}
        uint32_t blob(Blob k,uint32_t off){
            switch(k){case Blob::Inst:return r.img.words[off/4];
            case Blob::WndwPush:return off/4<r.pushes.wndw.count?r.pushes.wndw.words[off/4]:0;
            case Blob::CorePush:return off/4<r.pushes.core.count?r.pushes.core.words[off/4]:0;
            default:return 0;}}
        void vcopy(uint64_t vram,Blob k,uint32_t bytes){for(uint32_t i=0;i<bytes;i+=4)r.b->vwr(vram+i,blob(k,i));}
        void fill(uint64_t vram,uint32_t bytes,uint32_t pitch,uint32_t c,uint32_t m,uint32_t lines){r.b->fill(vram,bytes,pitch,c,m,lines);}
        void fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);r.b->fence();}
        bool expired(uint32_t ms,uint32_t& t){ // t = low 32 bits of the start time in us, 0 = unset
            const uint64_t now=r.b->nowUs();if(!t){t=uint32_t(now)|1;return false;}
            r.b->relax();return uint32_t(now)-t>uint64_t(ms)*1000;}
        void udelay(uint32_t us){const uint64_t end=r.b->nowUs()+us;while(r.b->nowUs()<end)r.b->relax();}
        void except(uint32_t chid,uint32_t s,uint32_t d,uint32_t c){
            Line l;l.raw("{\"nv_disp_exc\":1,\"chid\":");l.dec(chid);l.raw(",\"stat\":\"");l.hex(s);l.raw("\",\"data\":\"");
            l.hex(d);l.raw("\",\"code\":\"");l.hex(c);l.raw("\"}\n");r.b->emit(l.t);}
        void diff(uint32_t m,uint32_t a,uint32_t c){
            Line l;l.raw("{\"nv_disp_diff\":\"");l.hex(m);l.raw("\",\"assy\":\"");l.hex(a);l.raw("\",\"arm\":\"");l.hex(c);
            l.raw("\",\"rewritten\":");l.dec(r.plan.Rewritten(m)?1:0);l.raw("}\n");r.b->emit(l.t);}
        void pause(const char*){}
        bool call(uint32_t){return false;} // no dynamic steps in step 3
        void log(uint32_t i,const Step& s,uint32_t got,bool ok){
            Line l;l.raw("{\"nv_disp_step\":");l.dec(i);l.raw(",\"list\":\"");l.s(tag);l.raw("\",\"op\":\"");l.s(OpName(s.op));
            l.raw("\",\"reg\":\"");l.hex(s.op==Op::VramCopy||s.op==Op::VramWrite||s.op==Op::VramPoll||s.op==Op::VramPollAny||s.op==Op::SurfFill?s.vram:s.reg);
            l.raw("\",\"exp\":\"");l.hex(s.value);l.raw("\",\"got\":\"");l.hex(got);l.raw("\",\"ok\":");l.dec(ok?1:0);
            l.raw(",\"what\":\"");l.s(s.what);l.raw("\"}\n");r.b->emit(l.t);}
    };

    // ---- building
    bool Build(Stage stage,uint32_t nflips,uint32_t holdMs,bool observer){
        cfg=base;cfg.coreChannel=stage==Stage::B;cfg.feEnable=stage==Stage::A2;cfg.capsCopy=false;cfg.pauseForObserver=observer;
        pushes=Pushes{};plan=Plan{};
        const Err e=BuildPlan(cfg,img,pushes,plan,nflips,holdMs);
        if(e!=Err::Ok){Msg("error","plan build failed (layout/bounds)");return false;}
        return true;
    }
    static bool ReadOnly(Op o){return o==Op::Expect||o==Op::ExpectNot||o==Op::Log||o==Op::AssyArmDiff||o==Op::Poll||o==Op::PollSoft;}

    // ---- verbs
    void Status(){
        Line l;l.raw("{\"nv_disp_status\":1,\"preflight_ok\":");l.dec(st.preflightOk);l.raw(",\"live\":");l.dec(st.live);
        l.raw(",\"paused\":");l.dec(st.paused);l.raw(",\"cursor\":");l.dec(st.cursor);l.raw(",\"fail_at\":");l.dec(st.failAt==~0u?0:st.failAt+1);
        l.raw(",\"runs\":");l.dec(st.runs);l.raw(",\"aborts\":");l.dec(st.aborts);l.raw(",\"stage\":\"");
        l.s(st.stage==Stage::A?"a":st.stage==Stage::A2?"a2":"b");l.raw("\"}\n");b->emit(l.t);
    }
    void PrintPlan(Stage stage,uint32_t nflips){
        if(st.live){Msg("refused","a run is live; plan would rebuild its buffers");return;}
        if(!Build(stage,nflips,0,false))return;
        for(uint32_t i=0;i<plan.count;++i){Io io{*this,plan,"plan"};io.log(i,plan.steps[i],0,true);}
        Msg("plan","end");
    }
    bool Preflight(){
        if(st.live||st.paused){Msg("refused","a run is live");return false;}
        if(!Build(Stage::A,0,0,false))return false;
        const uint32_t end=plan.phaseBegin[uint32_t(Phase::Vram)];
        for(uint32_t i=0;i<end;++i)if(!ReadOnly(plan.steps[i].op)){Msg("error","preflight contains a write op");return false;}
        Io io{*this,plan,"preflight"};
        const uint32_t at=brook::nvdisp::Run(plan,0,end,io);
        st.preflightOk=at==end;
        Msg(st.preflightOk?"preflight":"preflight-failed",st.preflightOk?"ok: noop/flip armed for this boot":"mismatch: nothing armed");
        return st.preflightOk;
    }
    // noop = flips 0
    void Start(Stage stage,uint32_t nflips,uint32_t holdMs,bool observer,uint32_t stopAt){
        if(!st.preflightOk){Msg("refused","run preflight first (this boot)");return;}
        if(st.live||st.paused){Msg("refused","a run is live; continue or abort it");return;}
        if(!Build(stage,nflips,holdMs,observer))return;
        st.stage=stage;st.nflips=nflips;st.holdMs=holdMs;st.observer=observer;st.stopAt=stopAt;
        st.cursor=0;st.failAt=~0u;st.firstFlipOk=false;st.preflightOk=false;++st.runs;
        Continue(true);
    }
    // Run from the cursor. A stop point or PAUSE step parks the run (state
    // stays live; the nv-aux lock is released by the caller); `continue`
    // resumes at exactly that step without stopping on it again.
    void Continue(bool fresh=false){
        if(!fresh&&!st.paused){Msg("refused","nothing paused");return;}
        bool resume=!fresh;st.paused=false;
        const uint32_t vramBegin=plan.phaseBegin[uint32_t(Phase::Vram)];
        for(;st.cursor<plan.count;++st.cursor){
            const uint32_t i=st.cursor;const Step& s=plan.steps[i];
            if(resume)resume=false;
            else if(i==st.stopAt||s.op==Op::Pause){
                Io io{*this,plan,"run"};if(s.op==Op::Pause)io.log(i,s,0,true);
                st.paused=true;Msg("paused",s.op==Op::Pause?s.what:"stop point");Status();return;
            }
            if(s.op==Op::Pause)continue;
            if(i>=vramBegin&&!st.live){st.live=true;if(!st.hooked){b->hookQuiesce();st.hooked=true;}}
            Io io{*this,plan,"run"};
            if(brook::nvdisp::Run(plan,i,i+1,io)!=i+1){st.failAt=i;Fail(i);return;}
            if(i+1==plan.phaseBegin[uint32_t(Phase::Flip)])st.firstFlipOk=true;
        }
        st.live=false;++st.done;Msg("done","teardown complete; run preflight again before the next run");Status();
    }
    // On failure: when a flip had already worked, try the restore submission
    // first (so the GOP surface is on screen), then the abort list.
    void Fail(uint32_t i){
        const Phase ph=PhaseOf(i);
        Msg("failed","auto-abort");
        if(st.firstFlipOk&&ph>=Phase::Flip&&ph<=Phase::Restore&&pushes.count){
            Plan rp;const uint32_t k=pushes.count-1,ch=ChidWndw(cfg.window);
            rp.Add(Op::AckException,ch,1u<<cfg.window,0,0,"ack window exception before restore");
            rp.Add(Op::Kick,RegPut(ch),~0u,pushes.end[k],0,"restore submission");
            rp.Add(Op::PollSoft,RegArm(ch,c57e::SET_OFFSET0),~0u,uint32_t(cfg.surf[0].vram>>8),500,"ARM back on the GOP surface");
            Io io{*this,rp,"restore"};(void)brook::nvdisp::Run(rp,0,rp.count,io,true);
        }
        RunAbort(ph,100);
    }
    void RunAbort(Phase reached,uint32_t ms){
        abort=Plan{};BuildAbort(cfg,reached,abort,ms);
        Io io{*this,abort,"abort"};(void)brook::nvdisp::Run(abort,0,abort.count,io,true);
        st.live=false;st.paused=false;st.preflightOk=false;++st.aborts;
        Msg("aborted","abort list ran; run preflight again before the next run");Status();
    }
    void Abort(bool force){
        if(!st.live&&!st.paused&&!force){Msg("refused","nothing live (use abort force)");return;}
        if(!st.live&&!st.paused&&force&&!plan.count&&!Build(Stage::B,0,0,false))return;
        RunAbort(st.live||st.paused?PhaseOf(st.cursor):Phase::Restore,100);
    }
    // Pre-reset quiesce hook: abort only if a run is live (paused counts).
    void Quiesce(){if(st.live||st.paused)RunAbort(PhaseOf(st.cursor),50);}

    // ---- command parsing ("plan ...", "preflight", ...)
    static bool Word(const char*& p,const char* w){
        while(*p==' ')++p;const char* q=p;while(*w){if(*q!=*w)return false;++q;++w;}
        if(*q&&*q!=' ')return false;p=q;return true;}
    static bool Num(const char*& p,uint32_t& v){
        while(*p==' ')++p;if(*p<'0'||*p>'9')return false;v=0;
        while(*p>='0'&&*p<='9'){v=v*10+uint32_t(*p-'0');++p;if(v>1000000)return false;}return true;}
    void Command(const char* p){
        if(Word(p,"status")){Status();return;}
        if(Word(p,"preflight")){(void)Preflight();return;}
        if(Word(p,"continue")){Continue();return;}
        if(Word(p,"abort")){Abort(Word(p,"force"));return;}
        bool isPlan=false,isNoop=false,isFlip=false;
        if(Word(p,"plan"))isPlan=true;else if(Word(p,"noop"))isNoop=true;else if(Word(p,"flip"))isFlip=true;
        else{Msg("usage","nv-disp plan|preflight|noop|flip <n> <ms>|continue|abort [force]|status");return;}
        uint32_t n=0,ms=0,stop=~0u;Stage stage=Stage::A;bool observer=false;
        if(isFlip&&(!Num(p,n)||!Num(p,ms)||!n||n>kMaxFlips||ms>5000)){Msg("usage","flip <1..16> <0..5000 ms>");return;}
        for(;;){
            while(*p==' ')++p;if(!*p)break;
            if(Word(p,"a"))stage=Stage::A;else if(Word(p,"a2"))stage=Stage::A2;else if(Word(p,"b"))stage=Stage::B;
            else if(Word(p,"pause"))observer=true;
            else if(isPlan&&Word(p,"flips")){if(!Num(p,n)||n>kMaxFlips){Msg("usage","flips 0..16");return;}}
            else if(Word(p,"stop")){if(!Num(p,stop)){Msg("usage","stop <step>");return;}}
            else{Msg("usage","unknown argument");return;}
        }
        if(isPlan){PrintPlan(stage,n);return;}
        Start(stage,isNoop?0:n,ms,observer,stop);
    }
};
} // namespace brook::nvdisp
