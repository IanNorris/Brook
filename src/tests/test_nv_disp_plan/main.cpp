// Host test for nv_disp_plan.h (RTX 2080 step-3 dry-run builder).
// Goldens are hand-derived from nouveau (MIT); each block cites the source.
// A small fake display engine executes the plan: it parses the window
// pushbuffer from fake VRAM, resolves ctxdma handles through the RAMHT image
// the way the hardware does, and latches ASSY -> ARM on UPDATE.
// `test_nv_disp_plan --dump` prints the full sequence for the review packet.
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <string>
#include "nv_disp_plan.h"
using namespace brook::nvdisp;

static Config DellConfig(){
    Config c;
    const Surface a{0x0000000,3840,2160,16384,c57e::FORMAT_A8R8G8B8};
    const Surface b{0x2200000,3840,2160,16384,c57e::FORMAT_A8R8G8B8};
    c.surf[0]=a;c.surf[1]=b;
    c.instVram=0x0ff00000;c.wndwPushVram=0x0ff10000;c.corePushVram=0x0ff11000;c.syncVram=0x0ff12000;
    c.isoLimit=0x0fffffff; // BAR1 aperture (256 MiB), BAR1 == VRAM identity checked via PRAMIN
    return c;
}

// --- nouveau's ramht hash, transcribed independently (nvkm/core/ramht.c 27-38)
static uint32_t NouveauHash(uint32_t handle,int chid){
    const int bits=10;uint32_t hash=0;
    while(handle){hash^=(handle&((1u<<bits)-1));handle>>=bits;}
    hash^=uint32_t(chid)<<(bits-4);return hash;
}
static void TestHash(){
    // Hand-computed: 0xf0000000 -> chunks 0,0,0x300,3 -> 0x303; ^ (1<<6).
    assert(RamhtHash(0xf0000000,0)==0x303);
    assert(RamhtHash(0xf0000000,1)==0x343);
    for(uint32_t h:{0xf0000000u,0xf0000001u,0xfb000000u,0x45564144u,0x12345678u,1u})
        for(int ch=0;ch<9;++ch){
            assert(RamhtHash(h,ch)==NouveauHash(h,ch));
            assert(RamhtHashRm(h,ch,0)==NouveauHash(h,ch)); // RM == nouveau with client 0, chid < 16
        }
    // Entry word 1: nouveau (chid<<25)|0x40 | inst<<9 (gv100.c 356-357, ramht.c 88)
    for(uint32_t ch:{0u,1u,8u})for(uint32_t off:{0x2000u,0x2020u,0x2040u,0xffe0u})
        assert(RamhtContext(ch,off)==((ch<<25)|0x40u|(off<<9)));
    assert(RamhtContext(1,0x2000)==0x02400040);
}
static void TestHashFix(){ // pin the 0xfb000000 value exactly
    uint32_t v=0xfb000000;uint32_t e=(v&0x3ff)^((v>>10)&0x3ff)^((v>>20)&0x3ff)^(v>>30);
    assert(e==(0x3b0^0x3)); // (0xfb0 & 0x3ff) = 0x3b0
    assert(RamhtHash(0xfb000000,1)==(e^0x40));
}

// --- ctxdma words: gv100_dmaobj_bind (usergv100.c 50-54), flags 104-116
static void TestInst(){
    Config c=DellConfig();static InstImage img;assert(BuildInstImage(c,img)==Err::Ok);
    assert(img.objCount==3&&img.useCount==3);
    const uint32_t* w=img.words;
    // obj0 sync @0x2000: VRAM|rw|page-SP, start 0x0ff12000>>8, limit +0xfff
    assert(w[0x2000/4+0]==0x45&&w[0x2000/4+1]==0x0ff120&&w[0x2000/4+2]==0&&w[0x2000/4+3]==0x0ff12f&&w[0x2000/4+4]==0);
    // obj1 ISO pitch @0x2020: VRAM|rw, 0 .. 0x0fffffff
    assert(w[0x2020/4+0]==0x5&&w[0x2020/4+1]==0&&w[0x2020/4+3]==0x0fffff);
    // obj2 GOP "DAVE" @0x2040: surface 0, 16384*2160 bytes
    assert(w[0x2040/4+0]==0x5&&w[0x2040/4+1]==0&&w[0x2040/4+3]==((16384u*2160-1)>>8));
    // RAMHT slots at the nouveau hash, chid 1 (window 0)
    auto slot=[&](uint32_t h,uint32_t ch){return RamhtHash(h,ch);};
    assert(w[slot(kHandleSync,1)*2]==kHandleSync&&w[slot(kHandleSync,1)*2+1]==RamhtContext(1,0x2000));
    assert(w[slot(kHandleIsoPitch,1)*2]==kHandleIsoPitch&&w[slot(kHandleIsoPitch,1)*2+1]==RamhtContext(1,0x2020));
    assert(w[slot(kHandleGop,1)*2]==kHandleGop&&w[slot(kHandleGop,1)*2+1]==RamhtContext(1,0x2040));
    // Every other word of the hash table is empty.
    uint32_t used=0;for(uint32_t i=0;i<kHashEntries;++i)if(w[i*2+1])++used;
    assert(used==3);
    // Collision: same slot twice -> linear probe to the next slot (ramht.c 115-126)
    static uint32_t t[kInstBytes/4];memset(t,0,sizeof(t));
    assert(RamhtInsert(t,0x400,0,0x2000,RamhtHash(0x400,0))==1);   // 0x400 -> chunks 0,1 -> slot 1
    assert(RamhtInsert(t,0x1,0,0x2020,RamhtHash(0x1,0))==2);       // slot 1 taken -> 2
    // Mirror option writes a second entry at the RM slot.
    c.mirrorRmHash=true;assert(BuildInstImage(c,img)==Err::Ok);
    used=0;for(uint32_t i=0;i<kHashEntries;++i)if(img.words[i*2+1])++used;
    assert(used==6);
    // Stage B adds the core's notifier binding.
    c=DellConfig();c.coreChannel=true;assert(BuildInstImage(c,img)==Err::Ok&&img.useCount==4);
    assert(img.words[RamhtHash(kHandleSync,0)*2+1]==RamhtContext(0,0x2000));
}

// --- push buffers vs nouveau
static void TestPush(){
    // pushc37b.h PUSH_HDR / clc37b.h 28-39: count<<18 | mthd.
    assert(PushHeader(0x200,1)==0x00040200&&PushHeader(0x224,4)==0x00100224&&PushJump(0)==0x20000000);
    Config c=DellConfig();
    // wndwc57e_image_set (wndwc57e.c 33-79) reserves 17 words, all used.
    {Push p;PushWindowImage(p,c.surf[0],kHandleIsoPitch);assert(p.count==17);
     const uint32_t g[]={0x00040308,0x00000001,
        0x00100224,0x08700f00,0x00000010,0x000000cf,0x00000100,
        0x00040240,0xfb000000, 0x00040260,0x00000000, 0x00040290,0x00000000,
        0x00040298,0x08700f00, 0x000402a4,0x08700f00};
     assert(!memcmp(p.words,g,sizeof(g)));}
    // wndwc37e_ntfy_set (wndwc37e.c 224-237): 3 words; offset NV50_DISP_WNDW_NTFY(0)=0x60 (disp.h 21)
    {Push p;PushWindowNtfy(p,WindowNtfyOffset(0,0));assert(p.count==3);
     assert(p.words[0]==0x0008021c&&p.words[1]==0xf0000000&&p.words[2]==0x60);
     assert(WindowNtfyOffset(0,1)==0x70&&WindowNtfyOffset(3,0)==0x120);}
    // wndwc37e_update (wndwc37e.c 270-287): 5 words; window interlock = own bit (wndw.c 182).
    {Push p;PushWindowUpdate(p,0);assert(p.count==5);
     const uint32_t g[]={0x00080370,0,1,0x00040200,1};assert(!memcmp(p.words,g,sizeof(g)));
     Push q;PushWindowUpdate(q,5,1);assert(q.words[1]==1&&q.words[2]==0x20);}
    // corec57d_init (corec57d.c 31-60): PUSH_WAIT(2 + windows*5) exactly.
    {Push p;PushCoreInit(p,c.gop,false);assert(p.count==2+8*5);
     assert(p.words[0]==0x00040208&&p.words[1]==0xf0000000);
     for(uint32_t i=0;i<8;++i){const uint32_t* q=p.words+2+i*5;
        assert(q[0]==PushHeader(0x1004+i*0x80,2)&&q[1]==0xf&&q[2]==0);
        assert(q[3]==PushHeader(0x1010+i*0x80,1)&&q[4]==0x00117fff);}
     Push m;PushCoreInit(m,c.gop,true);assert(m.count==42&&m.words[3]==0x3b7&&m.words[6]==0x110f00);}
    // corec37d_wndw_owner (corec37d.c 34-49): windows*2 words, owner i>>1.
    {Push p;PushCoreWindowOwner(p,false);assert(p.count==16);
     for(uint32_t i=0;i<8;++i)assert(p.words[i*2]==PushHeader(0x1000+i*0x80,1)&&p.words[i*2+1]==(i>>1));
     Push m;PushCoreWindowOwner(m,true);assert(m.words[1]==0&&m.words[3]==0xf);}
    // corec37d_update with ntfy (corec37d.c 52-79): PUSH_WAIT(2*2 + 5).
    {Push p;PushCoreUpdate(p,0,1,true);assert(p.count==9);
     const uint32_t g[]={0x0004020c,0x1000,0x00080218,0,1,0x00040200,1,0x0004020c,0};
     assert(!memcmp(p.words,g,sizeof(g)));}
}

// --- fake display engine
struct Fake{
    std::map<uint32_t,uint32_t> reg;
    std::map<uint64_t,std::vector<uint8_t>> pages; // 4 KiB VRAM pages
    const InstImage* img=nullptr;const Pushes* push=nullptr;
    std::set<uint32_t> written;std::vector<std::string> trace;
    uint32_t wndwCur=0;          // HW fetch pointer, bytes
    bool idleOnConnect=true,breakRamht=false;
    uint32_t exceptions=0,flips=0;
    uint32_t& R(uint32_t a){return reg[a];}
    uint8_t* V(uint64_t a){auto& p=pages[a&~0xfffull];if(p.empty())p.assign(4096,0);return p.data()+(a&0xfff);}
    uint32_t vrd(uint64_t a){uint32_t v;memcpy(&v,V(a),4);return v;}
    void vwr(uint64_t a,uint32_t v){memcpy(V(a),&v,4);}
    void Gop(const Config& c){
        const auto& g=c.gop;const uint32_t ch=ChidWndw(c.window);
        R(kRegInstTarget)=g.instTarget;R(kRegInstAddr)=g.instAddr;R(kRegPraminBase)=c.praminRestore;
        R(RegChnCtl(0))=g.ctlCore;R(RegChnStatus(0))=0x20070000;R(RegChnCtl(ch))=g.ctlWndw;R(RegChnStatus(ch))=0x24010100;
        R(RegArm(0,c57d::WindowSetControl(0)))=g.coreWinCtl;R(RegArm(0,c57d::WindowFormatUsage(0)))=g.coreFmtUsage;
        R(RegArm(0,c57d::WindowUsage(0)))=g.coreUsage;R(RegArm(0,c57d::HeadRasterSize(0)))=g.headRaster;
        R(RegArm(0,c57d::HeadPixelClock(0)))=g.headPclk;R(RegArm(0,c57d::SorSetControl(1)))=g.sorCtl;
        for(uint32_t base:{RegAssy(ch,0),RegArm(ch,0)}){
            R(base+c57e::SET_SIZE)=g.winSize;R(base+c57e::SET_STORAGE)=g.winStorage;R(base+c57e::SET_PARAMS)=g.winParams;
            R(base+c57e::SET_PLANAR_STORAGE0)=g.winPlanar;R(base+c57e::SET_CONTEXT_DMA_ISO0)=g.winIso;R(base+c57e::SET_OFFSET0)=g.winOffset;}
        R(RegPut(ch))=0x80000000;
    }
    // Hardware-style handle lookup: probe from the hash over the whole table.
    bool Lookup(uint32_t handle,uint32_t chid,uint64_t& base,uint64_t& limit){
        const uint64_t inst=uint64_t(R(kRegInstAddr))<<16;
        if(!(R(kRegInstTarget)&8))return false;
        for(uint32_t n=0;n<kHashEntries;++n){
            const uint32_t s=(RamhtHash(handle,chid)+n)&(kHashEntries-1);
            const uint32_t h=vrd(inst+s*8),ctx=vrd(inst+s*8+4);
            if(!ctx)continue;
            if(h==handle&&(ctx>>25)==chid&&(ctx&0x3fff)==R(RegPbClient(chid))){
                const uint32_t off=((ctx>>14)&0x7ff)<<5;assert(off>=kObjBase&&off<kInstBytes);
                const uint32_t f=vrd(inst+off);assert((f&3)==1); // VRAM target
                base=(uint64_t(vrd(inst+off+4))|uint64_t(vrd(inst+off+8))<<32)<<8;
                limit=((uint64_t(vrd(inst+off+12))|uint64_t(vrd(inst+off+16))<<32)<<8)|0xff;
                return !breakRamht;
            }
        }
        return false;
    }
    void Except(uint32_t chid,uint32_t mthd){
        ++exceptions;R(kRegExcWin)|=1u<<(chid-1);R(kRegIntrDispatch)|=0x200;
        R(RegExcept(chid))=0x80000000|(7u<<12)|(mthd>>2); // UNRESOLVABLE_HANDLE
        R(RegChnStatus(chid))=(R(RegChnStatus(chid))&~0xf00u)|0xb00; // UPD_STATE EXCEPTION
    }
    void Execute(uint32_t chid,uint32_t put){
        const uint64_t pb=(uint64_t(R(RegPbLo(chid))&~1u)<<8)|(uint64_t(R(RegPbHi(chid)))<<40);
        assert((R(RegPbLo(chid))&3)==1); // VRAM target
        while(wndwCur!=put){
            const uint32_t hdr=vrd(pb+wndwCur);wndwCur+=4;
            if((hdr>>29)==1){wndwCur=hdr&0xffc;continue;}
            assert((hdr>>29)==0);
            const uint32_t n=(hdr>>18)&0x3ff,m=hdr&0x3ffc;
            for(uint32_t i=0;i<n;++i){const uint32_t d=vrd(pb+wndwCur);wndwCur+=4;
                const uint32_t mm=m+i*4;R(RegAssy(chid,mm))=d;
                if(mm==c57e::UPDATE){
                    uint64_t ib,il,nb,nl;const uint32_t iso=R(RegAssy(chid,c57e::SET_CONTEXT_DMA_ISO0));
                    if(!Lookup(iso,chid,ib,il)){Except(chid,mm);return;}
                    if(!Lookup(R(RegAssy(chid,c57e::SET_CONTEXT_DMA_NOTIFIER)),chid,nb,nl)){Except(chid,mm);return;}
                    const uint64_t off=uint64_t(R(RegAssy(chid,c57e::SET_OFFSET0)))<<8;
                    const uint32_t pitch=(R(RegAssy(chid,c57e::SET_PLANAR_STORAGE0))&0x1fff)<<6;
                    const uint32_t hgt=R(RegAssy(chid,c57e::SET_SIZE))>>16;
                    assert(ib+off+uint64_t(pitch)*hgt-1<=il); // surface within ctxdma limit
                    assert(R(RegAssy(chid,c57e::SET_INTERLOCK_FLAGS))==0); // no core interlock in step 3
                    for(uint32_t x=0x200;x<0x800;x+=4)if(reg.count(RegAssy(chid,x)))R(RegArm(chid,x))=R(RegAssy(chid,x));
                    const uint32_t ntfy=R(RegAssy(chid,c57e::SET_NOTIFIER_CONTROL))&0xff0;
                    assert(nb+ntfy+15<=nl);
                    vwr(nb+ntfy,kNtfyBegun<<kNtfyStatusShift);++flips;
                }
            }
        }
        R(RegGet(chid))=put;
    }
    uint32_t rd(uint32_t a){
        if(a>=kPramin&&a<kPramin+0x100000)return vrd((uint64_t(R(kRegPraminBase))<<16)+(a-kPramin));
        return R(a);
    }
    void wr(uint32_t a,uint32_t v){
        written.insert(a);
        if(a==kRegBar1Flush){R(a)=0;return;}
        if(a==kRegExcWin){R(a)&=~v;if(!R(a))R(kRegIntrDispatch)&=~0x200u;return;}
        for(uint32_t ch=0;ch<=8;++ch){
            if(a==RegChnCtl(ch)){
                const uint32_t old=R(a);R(a)=v;
                if((v&kCtlRunning)==kCtlRunning&&(old&kCtlConnect)==0){
                    assert(R(RegPbLo(ch))&1);assert(R(RegPbClient(ch))==kClientId);
                    assert((R(kRegInstTarget)&8)&&R(kRegInstAddr));     // instmem before connect
                    if(idleOnConnect)R(RegChnStatus(ch))=ch==0?0x000b0000:0x04040100;
                    R(RegGet(ch))=0;wndwCur=0;}
                if(!(v&kCtlConnect)&&(old&kCtlConnect))R(RegChnStatus(ch))=ch==0?0x00080000:0x01010100;
                return;}
            if(a==RegPut(ch)&&(R(RegChnCtl(ch))&kCtlRunning)==kCtlRunning){R(a)=v;if(ch)Execute(ch,v);return;}
            if(a==RegExcept(ch)&&v==kExceptAck){R(a)=0;R(RegChnStatus(ch))&=~0xf00u;R(RegChnStatus(ch))|=0x100;return;}
        }
        R(a)=v;
    }
    void vcopy(uint64_t vram,Blob b,uint32_t bytes){for(uint32_t i=0;i<bytes;i+=4)vwr(vram+i,blob(b,i));}
    uint32_t blob(Blob b,uint32_t off){
        switch(b){
        case Blob::Inst:return img->words[off/4];
        case Blob::WndwPush:return off/4<push->wndw.count?push->wndw.words[off/4]:0;
        case Blob::CorePush:return off/4<push->core.count?push->core.words[off/4]:0;
        default:return 0;}
    }
    void fence(){}
    bool expired(uint32_t ms,uint32_t& t){return ++t>ms;}
    void except(uint32_t chid,uint32_t s,uint32_t d,uint32_t c){trace.push_back("exception chid "+std::to_string(chid));(void)s;(void)d;(void)c;}
    bool verbose=false;
    void log(uint32_t i,const Step& s,uint32_t got,bool ok){
        if(verbose||!ok)printf("  %3u %-12s %s got %08x\n",i,ok?"ok":"FAIL",s.what,got);}
};
// PraminExpect values must be the image words at those offsets.
static void FixPramin(Plan& p,const InstImage& img){
    for(uint32_t i=0;i<p.count;++i)if(p.steps[i].op==Op::PraminExpect)assert(p.steps[i].value==img.words[p.steps[i].reg/4]);
}

static void TestPlanRun(bool core){
    Config c=DellConfig();c.coreChannel=core;
    static InstImage img;static Pushes pushes;static Plan plan;pushes=Pushes{};plan=Plan{};
    assert(BuildPlan(c,img,pushes,plan)==Err::Ok);FixPramin(plan,img);
    Fake f;f.img=&img;f.push=&pushes;f.Gop(c);
    assert(Run(plan,0,plan.count,f)==plan.count);
    assert(f.flips==3&&f.exceptions==0);
    const uint32_t ch=ChidWndw(0);
    // Final state == the GOP state the survey recorded (except core CHNCTL in stage B).
    assert(f.R(kRegInstTarget)==1&&f.R(kRegInstAddr)==0&&f.R(kRegPraminBase)==0x1fff0);
    assert((f.R(RegChnCtl(ch))&0xffff)==0x01&&(f.R(RegChnStatus(ch))&kWinStateMask)==kWinUnconnected);
    assert(f.R(RegArm(ch,c57e::SET_CONTEXT_DMA_ISO0))==kHandleGop&&f.R(RegArm(ch,c57e::SET_OFFSET0))==0);
    assert(f.R(RegArm(ch,c57e::SET_SIZE))==0x08700f00&&f.R(RegArm(ch,c57e::SET_PARAMS))==0xcf);
    if(!core)assert(f.R(RegChnCtl(0))==0x21);
    // Registers the plan may write: nothing in head/SOR/core method space.
    std::set<uint32_t> allowed={kRegBar1Flush,kRegPraminBase,kRegInstTarget,kRegInstAddr,
        RegPbLo(ch),RegPbHi(ch),RegPbValid(ch),RegPbClient(ch),RegChnCtl(ch),RegPut(ch)};
    if(core)for(uint32_t r:{RegPbLo(0),RegPbHi(0),RegPbValid(0),RegPbClient(0),RegChnCtl(0),RegPut(0)})allowed.insert(r);
    for(uint32_t a:f.written){if(!allowed.count(a)){printf("unexpected write %06x\n",a);assert(false);}}
    // Phase markers are monotonic and cover the list.
    for(uint32_t i=0;i<uint32_t(Phase::Count);++i)assert(plan.phaseBegin[i]<=plan.phaseBegin[i+1]);
    assert(pushes.end[0]==25*4&&pushes.end[1]==pushes.end[0]+14*4&&pushes.end[2]==pushes.end[1]+14*4);
}
static void TestPreflightRefuses(){
    Config c=DellConfig();static InstImage img;static Pushes pushes;static Plan plan;pushes=Pushes{};plan=Plan{};
    assert(BuildPlan(c,img,pushes,plan)==Err::Ok);FixPramin(plan,img);
    Fake f;f.img=&img;f.push=&pushes;f.Gop(c);f.R(kRegInstTarget)=0x9; // someone already owns instmem
    const uint32_t at=Run(plan,0,plan.count,f);
    assert(at<plan.phaseBegin[uint32_t(Phase::Vram)]&&f.written.empty()); // nothing written
}
static void TestAbortWindowNeverIdle(){
    Config c=DellConfig();static InstImage img;static Pushes pushes;static Plan plan,abort;pushes=Pushes{};plan=Plan{};abort=Plan{};
    assert(BuildPlan(c,img,pushes,plan)==Err::Ok);FixPramin(plan,img);
    Fake f;f.img=&img;f.push=&pushes;f.Gop(c);f.idleOnConnect=false;
    f.R(RegChnStatus(1))=0x02010100; // stuck in INIT1 (core still EFI-owned)
    const uint32_t at=Run(plan,0,plan.count,f);
    assert(at>=plan.phaseBegin[uint32_t(Phase::Window)]&&at<plan.phaseBegin[uint32_t(Phase::FirstFlip)]);
    BuildAbort(c,Phase::Window,abort);
    assert(Run(abort,0,abort.count,f,true)==abort.count); // soft polls only log
    assert((f.R(RegChnCtl(1))&0x6013)==0x01&&f.R(kRegInstTarget)==1&&f.R(kRegInstAddr)==0&&f.R(kRegPraminBase)==0x1fff0);
    // ARM never changed: the GOP keeps scanning out its own surface.
    assert(f.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0))==kHandleGop&&f.flips==0);
}
static void TestAbortUnresolvableHandle(){
    Config c=DellConfig();static InstImage img;static Pushes pushes;static Plan plan,abort;pushes=Pushes{};plan=Plan{};abort=Plan{};
    assert(BuildPlan(c,img,pushes,plan)==Err::Ok);FixPramin(plan,img);
    Fake f;f.img=&img;f.push=&pushes;f.Gop(c);f.breakRamht=true;
    const uint32_t at=Run(plan,0,plan.count,f);
    assert(at>=plan.phaseBegin[uint32_t(Phase::FirstFlip)]&&at<plan.phaseBegin[uint32_t(Phase::Flip)]);
    assert(f.exceptions==1&&(f.R(kRegIntrDispatch)&0x200));
    BuildAbort(c,Phase::FirstFlip,abort);assert(Run(abort,0,abort.count,f,true)==abort.count);
    assert(f.trace.size()==1&&!(f.R(kRegExcWin)&1)&&!(f.R(kRegIntrDispatch)&0x200));
    assert((f.R(RegChnCtl(1))&0x6013)==0x01&&f.R(kRegInstTarget)==1);
    assert(f.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0))==kHandleGop);
}
static void TestValidate(){
    Config c=DellConfig();assert(Validate(c)==Err::Ok);
    c.surf[1].vram=0x1000000;assert(Validate(c)==Err::Overlap);          // overlaps surface 0
    c=DellConfig();c.surf[1].vram+=0x80;assert(Validate(c)==Err::Align);
    c=DellConfig();c.instVram=0x0ff08000;assert(Validate(c)==Err::Align);
    c=DellConfig();c.surf[1].width=1920;assert(Validate(c)==Err::Usage);   // step 3 keeps the GOP mode
    c=DellConfig();c.surf[1].vram=0x0f000000;assert(Validate(c)==Err::Bounds); // beyond BAR1
    c=DellConfig();c.wndwPushVram=0x0ff12000;assert(Validate(c)==Err::Overlap);
    c=DellConfig();c.surf[0].format=0xd5;assert(Validate(c)==Err::Format);
}
static void TestRingWrap(){
    Config c=DellConfig();static Push ring;ring=Push{};Ring r;
    uint32_t last=0,jumps=0;
    for(uint32_t i=0;i<200;++i){
        const uint32_t before=r.put;last=BuildFlip(ring,r,c,c.surf[i&1],i&1);
        assert(last%4==0&&last/4<=kPushMaxWords);
        if(last<before){++jumps;assert(ring.words[before/4]==PushJump(0));}
    }
    assert(jumps>=2&&!ring.overflow);
}

static void Dump(bool core){
    Config c=DellConfig();c.coreChannel=core;
    static InstImage img;static Pushes pushes;static Plan plan;pushes=Pushes{};plan=Plan{};
    assert(BuildPlan(c,img,pushes,plan)==Err::Ok);FixPramin(plan,img);
    static const char* op[]={"WR","MASK","EXPECT","POLL","POLLSOFT","ACK","VCOPY","VWR","VEXPECT","VPOLL","VPOLLANY","ACKEXC","PRAMIN","FENCE","KICK"};
    uint32_t ph=0;
    for(uint32_t i=0;i<plan.count;++i){
        while(ph<uint32_t(Phase::Count)&&plan.phaseBegin[ph]<=i&&(ph+1>=uint32_t(Phase::Count)||plan.phaseBegin[ph+1]<=i))++ph;
        for(uint32_t q=0;q<uint32_t(Phase::Count);++q)if(plan.phaseBegin[q]==i&&(q+1==uint32_t(Phase::Count)||plan.phaseBegin[q+1]>i))printf("## %s\n",PhaseName(Phase(q)));
        const Step& s=plan.steps[i];
        if(s.op==Op::VramCopy||s.op==Op::VramWrite||s.op==Op::VramPoll||s.op==Op::VramPollAny)
            printf("%3u %-8s vram %08llx mask %08x val %08x %s\n",i,op[uint32_t(s.op)],(unsigned long long)s.vram,s.mask,s.value,s.what);
        else printf("%3u %-8s %06x mask %08x val %08x%s %s\n",i,op[uint32_t(s.op)],s.reg,s.mask,s.value,
            s.timeoutMs?(" ("+std::to_string(s.timeoutMs)+" ms)").c_str():"",s.what);
    }
    printf("## window pushbuffer (%u words)\n",pushes.wndw.count);
    for(uint32_t i=0;i<pushes.wndw.count;++i)printf("%s%08x%s",i%8?"":"  ",pushes.wndw.words[i],i%8==7||i+1==pushes.wndw.count?"\n":" ");
    printf("## submissions end at %u %u %u bytes\n## RAMHT\n",pushes.end[0],pushes.end[1],pushes.end[2]);
    for(uint32_t i=0;i<kHashEntries;++i)if(img.words[i*2+1])printf("  slot %03x: %08x %08x\n",i,img.words[i*2],img.words[i*2+1]);
    for(uint32_t o=0;o<img.objCount;++o){const uint32_t* w=img.words+ObjOffset(o)/4;
        printf("  obj @%04x: %08x %08x %08x %08x %08x %08x\n",ObjOffset(o),w[0],w[1],w[2],w[3],w[4],w[5]);}
    static Plan abort;abort=Plan{};BuildAbort(c,Phase::Restore,abort);
    printf("## abort/quiesce (from any phase >= window)\n");
    for(uint32_t i=0;i<abort.count;++i){const Step& s=abort.steps[i];
        printf("%3u %-8s %06x mask %08x val %08x %s\n",i,op[uint32_t(s.op)],s.reg,s.mask,s.value,s.what);}
}

int main(int argc,char** argv){
    if(argc>1&&!strcmp(argv[1],"--dump")){Dump(argc>2&&!strcmp(argv[2],"core"));return 0;}
    TestHashFix();
    TestPush();TestInst();
    TestPlanRun(false);TestPlanRun(true);
    TestPreflightRefuses();TestAbortWindowNeverIdle();TestAbortUnresolvableHandle();
    TestValidate();TestRingWrap();
    TestHash();
    puts("test_nv_disp_plan: ok");return 0;
}
