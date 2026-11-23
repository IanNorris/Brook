// Host test for nv_disp_head.h (RTX 2080 step 4: head 1 on SOR2-A).
// - VBIOS parsing against a synthetic image built here in nouveau's layouts
//   (BIT 'C' PLL v0x50, BIT 'd' DP v0x42, DCB 4.1); optionally also against a
//   real dump named by $BROOK_TEST_VBIOS (machine data is never committed).
// - Goldens: VPLL coefficients and register words (devinit/tu102.c 52-58,
//   gt215_pll_calc), DP SF values (nv50.c 1156-1260), head/SOR core words.
// - DP training state machine against a fake sink (dp.c 131-366).
// - The whole head plan against a fake core channel with supervisor stages,
//   including the head-0-untouched guards.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <string>
#include "nv_disp_head.h"
#include "nv_disp_runner.h" // GopFlipConfig
#include "nv_disp_head_runner.h"
using namespace brook::nvdisp;

#include "head_fake.h"

static void TestVbios(){
    const Rom rom=MakeRom();const Vbios v=rom.V();
    assert(v.Bit()==0x100);
    PllLimits l{};assert(ParsePll(v,kPllVpll0+1,l));
    assert(l.refclk==27000&&l.vcoMin==800000&&l.vcoMax==1620000&&l.inMin==13000&&l.inMax==38000);
    assert(l.mMin==1&&l.mMax==1&&l.nMin==20&&l.nMax==255&&l.pMin==1&&l.pMax==31);
    assert(!ParsePll(v,0x83,l));
    DcbOutp o{};assert(ParseDcb(v,4,o));
    assert(o.type==6&&o.orMask==4&&o.link==1&&o.heads==0xf&&o.connector==2&&o.ccb==7&&o.linkBw==0x14&&o.linkNr==4);
    assert(o.hasht==0x0006&&o.hashm==0x0f44);                       // as nouveau's DCB[4]
    DcbOutp g{};assert(ParseDcb(v,0,g)&&g.hashm==0x0f82&&g.link==2);
    DpOut dp{};assert(FindDpOut(v,o.hasht,o.hashm,dp));
    assert(dp.entry==0x2300&&dp.beforeLt==0x611f&&dp.lnkcmp==0x6107&&dp.disableLt==0x5c55&&dp.cfgIdx==0);
    // dpcfg index = pc*10 + vsoff[vs] + pe (dp.c 206-232)
    DpCfg c{};
    assert(DpDrive(v,dp,0,0,0,c)&&c.dc==CfgDc(0)&&c.pe==CfgPe(0)&&c.pu==CfgPu(0));
    assert(DpDrive(v,dp,0,2,1,c)&&c.dc==CfgDc(8));                  // vsoff[2]=7, +1
    assert(DpDrive(v,dp,1,3,0,c)&&c.dc==CfgDc(19));                 // 10 + 9
    DpOut other{};assert(FindDpOut(v,0x0006,0x0f82,other)&&other.cfgIdx==1);
    assert(DpDrive(v,other,0,0,0,c)&&c.dc==CfgDc(10));              // + cfgIdx*10 on v0x42
}
// Optional: the real dump (rtx2080-step4-design.md 1 / 7.1 values).
static void TestRealVbios(){
    const char* path=getenv("BROOK_TEST_VBIOS");if(!path)return;
    FILE* f=fopen(path,"rb");if(!f){printf("BROOK_TEST_VBIOS unreadable\n");return;}
    std::vector<uint8_t> d(1<<20);const size_t n=fread(d.data(),1,d.size(),f);fclose(f);
    const Vbios v{d.data(),uint32_t(n)};
    assert(v.Bit()==0x1b0);
    PllLimits l{};assert(ParsePll(v,kPllVpll0+1,l)&&l.refclk==27000&&l.vcoMax==1620000&&l.mMax==1&&l.pMax==31);
    DcbOutp o{};assert(ParseDcb(v,4,o)&&o.orMask==4&&o.link==1&&o.ccb==7&&o.hashm==0x0f44);
    DpOut dp{};assert(FindDpOut(v,o.hasht,o.hashm,dp)&&dp.entry==0x60e1&&dp.beforeLt==0x611f&&dp.disableLt==0x5c55);
    DpCfg c{};assert(DpDrive(v,dp,0,0,0,c)&&c.dc==0x15&&c.pe==0x01&&c.pu==0x2);
    HeadConfig hc;hc.base=GopFlipConfig(3840,2160,16384,0xcf,256ull<<20);DpOut out{};
    assert(VbiosHash(Vbios{d.data(),131072})==0xbb33ca0a);                        // the image the scripts came from
    assert(ResolveHeadConfig(Vbios{d.data(),131072},4,hc,out,0xbb33ca0a)&&hc.vpll.N==58&&hc.vpll.fN==0x1800&&hc.vpll.P==3);
    assert(LnkcmpScript(v,out.lnkcmp,0x14)==0x617b&&hc.vpll0.refclk==27000);
    printf("test_nv_disp_head: real VBIOS checks passed\n");
}
static void TestPllAndSf(){
    const PllLimits l{27000,800000,1620000,13000,38000,1,1,20,255,1,31};
    PllCoef c{};assert(Gt215PllCalc(l,533250,c));
    assert(c.N==58&&c.fN==0x1800&&c.M==1&&c.P==3);                  // (58 + 10240/8192) * 27 / 3 = 533.25
    Plan p;AddVpll(p,1,c);
    assert(p.steps[0].reg==0xef50&&p.steps[0].value==0x18000000);
    assert(p.steps[1].reg==0xef44&&p.steps[1].value==0x00033a01);
    assert(p.steps[2].reg==0xef4c&&p.steps[2].value==0x900&&p.steps[3].reg==0xef40&&p.steps[3].value==0x02000014);
    // Integer clocks take the fractional path too: 148.5 MHz.
    assert(Gt215PllCalc(l,148500,c)&&c.P==10&&uint64_t(27000)*(c.N*8192+c.fN+4096)/8192/c.P==148500);
    HeadMode m;DpSf s{};assert(DpSymbols(m,0x14,4,true,s));
    assert(s.h==148&&s.v==2149&&s.watermark==18);                  // nouveau's formula
    assert(!DpSymbols(m,0x06,1,true,s));                            // 4K60 does not fit RBR x1
}
static void TestUnblankSegment(){
    HeadConfig c;HeadPushes hp;BuildHeadPushes(c,hp);
    const uint32_t* w=hp.core.words+hp.enableEnd/4;
    assert(w[0]==PushHeader(0x240+c.head*4,1)&&w[1]==2&&hp.unblankEnd>hp.enableEnd&&hp.disableEnd>hp.unblankEnd);
    for(uint32_t i=0;i<hp.enableEnd/4;){const uint32_t x=hp.core.words[i],n=(x>>18)&0x3ff,m=x&0x3ffc;
        assert(!(m<0x250&&m+n*4>0x240));i+=1+n;}                       // nothing before it touches blanking ctrl
}
static void TestCoreWords(){
    HeadConfig c;HeadPushes hp;BuildHeadPushes(c,hp);
    assert(hp.core.words[0]==PushHeader(0x208,1)&&hp.core.words[1]==kHandleSync);  // ours, first
    // replay: every documented core method once, in runs; then the rung (corec37d_update + ntfy)
    {uint32_t n=0;for(uint32_t i=2;i<hp.replayEnd/4;){const uint32_t k=(hp.core.words[i]>>18)&0x3ff;n+=k;i+=1+k;}
     assert(n==kCoreReplayCount&&kCoreReplayCount==3+16+40+4*111);}
    {const uint32_t* r=hp.core.words+hp.replayEnd/4;const uint32_t g[]={0x0004020c,0x1000,0x00080218,0,0,0x00040200,1,0x0004020c,0};
     assert(hp.rungEnd-hp.replayEnd==sizeof(g)&&!memcmp(r,g,sizeof(g)));}
    assert(hp.core.count<=kPushMaxWords);
    const uint32_t* w=hp.core.words+hp.rungEnd/4;
    assert(w[0]==PushHeader(0x340,1)&&w[1]==0x802);                 // SOR2: owner head 1, DP_A
    w+=2;
    assert(w[0]==PushHeader(0x2400,4)&&w[1]==0&&w[2]==0xfc000040&&w[3]==0&&w[4]==533250000);
    // raster block at 0x2464: 4000x2222 total, sync end 31/4, blank end 111/58, blank start 3951/2218
    const uint32_t* q=hp.core.words;uint32_t i=hp.rungEnd/4;
    while(i<hp.core.count&&q[i]!=PushHeader(0x2464,5))++i;
    assert(i<hp.enableEnd/4&&q[i+1]==0x08ae0fa0&&q[i+2]==0x0004001f&&q[i+3]==0x003a006f&&q[i+4]==0x08aa0f6f&&q[i+5]==1);
    while(i<hp.core.count&&q[i]!=PushHeader(0x1100,1))++i;
    assert(i<hp.enableEnd/4&&q[i+1]==1);                            // window 2 -> head 1
    assert(hp.enableEnd&&hp.disableEnd>hp.enableEnd);
}

static Rom g_rom;
static void Setup(Core& c,InstImage& img,HeadPushes& hp,Plan& p,Vbios& v){
    g_rom=MakeRom();v=g_rom.V();
    c.cfg.base=GopFlipConfig(3840,2160,16384,0xcf,256ull<<20);
    assert(ResolveHeadConfig(v,4,c.cfg,c.dpout));
    assert(c.cfg.sf.watermark==0x15&&c.cfg.sf.h==148);             // GOP's watermark by default
    c.vb=&v;c.img=&img;c.hp=&hp;c.Gop();
    ReadCoreReplay(c.cfg,[&](uint32_t a){return c.rd(a);});
    assert(BuildHeadPlan(c.cfg,img,hp,p)==Err::Ok);
}
static void TestHeadPlan(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};
    Setup(c,img,hp,p,v);
    assert(Run(p,0,p.count,c)==p.count);
    assert(c.tr.cr&&c.tr.eq&&c.updates==3&&c.supers==6&&!c.superOnHead0&&c.pauses==1&&c.flags==0);
    // The core connect reset head-0/SOR1/window-0 ASSY; the replay restored it before any UPDATE.
    assert(c.R(0x682068)==c.R(0x68a068)&&c.R(0x680320)==0x901&&c.R(0x681004)==c.R(0x689004));
    assert(c.R(kRegExcOther)==0&&c.R(0x612b00)==0x03d10040&&c.R(0x612200)==0x00800040); // head-0 clock regs untouched
    // Training ended on the drive the sink asked for, from the VBIOS table.
    assert((c.tr.conf[0]&3)==2&&((c.tr.conf[0]>>3)&3)==1);
    assert((c.R(0x61d118)&0xff)==CfgDc(8)&&(c.R(0x61d120)&0xff)==CfgPe(8));
    assert(((c.R(0x61d130)>>8)&0xf)==CfgPu(8));
    // Supervisor 2 work landed: VPLL1, head-1 SF, rgclk, SOR clock.
    assert(c.R(0xef44)==0x00033a01&&c.R(0xef50)==0x18000000&&c.R(0xef40)==0x02000014);
    assert((c.R(0x616d68)&0xffff)==148&&(c.R(0x61656c+0x800)&0xffffff)==2149&&(c.R(0x616d50)&0x0c00003f)==0x08000015);
    // Pad calibration converged on the comparator's target and was copied.
    assert(((c.R(0x612508)>>8)&0xf)==6&&((c.R(0x612588)>>8)&0xf)==6);
    // Head 0 never changed; head 1 was ARMed and detached again.
    for(uint32_t i=0;i<hp.enableEnd/4;){const uint32_t w=hp.core.words[i],n=(w>>18)&0x3ff,m=w&0x3ffc; // no blanking ctrl pushed by default
        assert(!(m<0x250&&m+n*4>0x240));i+=1+n;}
    assert(c.R(0x68a064)==c.cfg.base.gop.headRaster&&c.R(0x68a00c)==c.cfg.base.gop.headPclk&&c.R(0x688320)==0x901);
    assert(c.R(0x68a464)==c.cfg.mode.rasterSize&&(c.R(0x688340)&0xff)==0&&(c.R(0x689100)&0xf)==0xf);
    assert((c.R(0x616d40)&1)==0);                                    // OffInt2 cleared the head SF enable
    // Back to the firmware state.
    assert((c.R(RegChnCtl(0))&0x6013)==0x01&&c.R(RegPbLo(0))==1&&c.R(kRegInstTarget)==1&&c.R(kRegInstAddr)==0);
    assert((c.R(0x612508)&0x1f)==0x03);                               // route left as the VBIOS set it
    // Writes: nothing to head-0 core regs, PLLs other than head 1, or SORs other than SOR2.
    for(uint32_t a:c.written){
        assert(!(a>=0xef00&&a<0xef40));
        assert(!(a>=0x61c000&&a<0x61d000)&&!(a>=0x612300&&a<0x612400)); // SOR0/SOR1 registers
        assert(!(a>=0x616540&&a<0x616570));                           // head-0 SF block
    }
}

// Head 0 dirtied after the last strict diff: every stage still finishes with the
// head-1 work (sticky flag, never an abort), VPLL1 is programmed, then disable.
static uint32_t KickIndex(const Plan& p,uint32_t put){
    for(uint32_t i=0;i<p.count;++i)if(p.steps[i].op==Op::Kick&&p.steps[i].value==put)return i;
    assert(false);return 0;}
static void TestRefusesWhenHead0Pending(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};
    Setup(c,img,hp,p,v);
    c.R(0x680000+0x2038)=7;                                            // undocumented head-0 method: cannot be replayed
    const uint32_t at=Run(p,0,p.count,c);
    assert(at<p.phaseBegin[uint32_t(HPhase::Vram)]&&c.written.empty()&&c.diffs==1); // blanking ctrl read-backs are skipped
    // A documented one is simply replayed with its ARM value: no refusal.
    Core d;static InstImage i2;static HeadPushes h2;static Plan p2;Vbios v2;i2=InstImage{};h2=HeadPushes{};p2=Plan{};
    Setup(d,i2,h2,p2,v2);d.R(0x680000+0x2068)=0x0004001e;
    assert(Run(p2,0,p2.count,d)==p2.count&&d.flags==0&&d.R(0x68a068)==d.R(0x682068));
}
static uint32_t StrictIndex(const Plan& p){
    for(uint32_t i=0;i<p.count;++i)if(p.steps[i].op==Op::AssyArmDiff&&p.steps[i].x==1)return i;
    assert(false);return 0;}
// N2: an undocumented method the core connect resets cannot be replayed, so the strict
// post-replay diff fails on it (any difference not ours), before the first UPDATE.
static void TestUndocumentedResetLogged(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p,ab;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};ab=Plan{};
    Setup(c,img,hp,p,v);
    const uint32_t k=p.phaseBegin[uint32_t(HPhase::CoreCheck)];
    assert(Run(p,0,k,c)==k);c.R(0x680000+0x2038)=9;                   // as if the connect reset it
    const uint32_t at=Run(p,k,p.count,c);
    assert(at>=StrictIndex(p)&&at<=StrictIndex(p)+2&&p.steps[at].op==Op::AssyArmDiff&&c.updates==0);
    BuildHeadAbort(c.cfg,HPhase::CoreCheck,hp,ab,100,Kicked::Replay);
    for(uint32_t i=0;i<ab.count;++i)assert(ab.steps[i].op!=Op::Kick&&ab.steps[i].op!=Op::Call);
    (void)Run(ab,0,ab.count,c,true);
    assert(c.updates==0&&(c.R(RegChnCtl(0))&0x13)==0x01&&c.R(kRegInstTarget)==1);
    // 0x2074 is replayed (nouveau pushes it raw): its connect reset (the fake resets it) is repaired.
    assert(c.R(0x682074)==1);
}
// N4: a supervisor during the rung fails its wait at once (no 2 s timeout); the abort flushes it.
static void TestRungSupervisorFailsFast(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p,ab;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};ab=Plan{};
    Setup(c,img,hp,p,v);
    const uint32_t k=KickIndex(p,hp.rungEnd);
    assert(Run(p,0,k,c)==k);c.R(0x680000+0x2068)=0x0004001e;          // head 0 dirtied after the strict diff
    const uint32_t at=Run(p,k,p.count,c);
    assert(at==k+1&&c.supers==1&&c.stage==1);
    BuildHeadAbort(c.cfg,HPhase::CoreCheck,hp,ab,100,Kicked::Rung);(void)Run(ab,0,ab.count,c,true);
    assert(c.stage==0&&c.supers==3&&(c.R(RegChnCtl(0))&0x13)==0x01);
}
// N1: a stop right after the stage-2 ack (the stage is open: acked, not released) and an
// abort: the flush treats the open stage as seen, does the stage-2 work (VPLL1) and releases it.
static void TestOpenStageReleased(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p,ab;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};ab=Plan{};
    Setup(c,img,hp,p,v);
    uint32_t ack2=0;for(uint32_t i=0;i<p.count;++i)if(p.steps[i].op==Op::Write&&p.steps[i].reg==kRegSuperAck&&p.steps[i].value==2){ack2=i;break;}
    assert(ack2&&Run(p,0,ack2+1,c)==ack2+1&&c.openStage==2&&c.stage==2&&!(c.evt&2));
    BuildHeadAbort(c.cfg,HPhase::Modeset,hp,ab,100,Kicked::Enable);(void)Run(ab,0,ab.count,c,true);
    assert(c.R(0xef44)==0x00033a01&&c.R(0xef50)==0x18000000&&c.stage==0&&c.openStage==0&&(c.R(0x688340)&0xff)==0);
}
static void TestHead0FlagFinishesStages(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};
    Setup(c,img,hp,p,v);
    const uint32_t k=KickIndex(p,hp.enableEnd);
    assert(Run(p,0,k,c)==k);
    c.R(0x680000+0x2068)=0x0004001e;
    assert(Run(p,k,p.count,c)==p.count);
    assert(c.superOnHead0&&c.flags>=3&&c.supers==6&&c.R(0xef44)==0x00033a01&&(c.R(0x688340)&0xff)==0);
}
// Abort from a stop before the enable kick: no supervisor flush, no disable kick.
static void TestAbortUnkicked(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p,ab;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};ab=Plan{};
    Setup(c,img,hp,p,v);
    const uint32_t k=KickIndex(p,hp.enableEnd);
    assert(Run(p,0,k,c)==k&&c.updates==1);                             // the rung only
    BuildHeadAbort(c.cfg,HPhase::Modeset,hp,ab,100,Kicked::Rung);
    for(uint32_t i=0;i<ab.count;++i)assert(!(ab.steps[i].op==Op::Kick));
    (void)Run(ab,0,ab.count,c,true);
    assert(c.updates==1&&c.stage==0&&(c.R(RegChnCtl(0))&0x13)==0x01&&c.R(kRegInstTarget)==1);
}
// Stages visible only in the event status 0x611860 are still serviced.
static void TestStagesFromEventStatus(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};
    Setup(c,img,hp,p,v);c.hideCtrlDisp=true;
    assert(Run(p,0,p.count,c)==p.count&&c.supers==6);
}
// A core exception on the modeset UPDATE fails the supervisor-1 wait at once; the
// abort logs/acks it (0x611854, 0x611020) and the flush + detach still complete.
static void TestCoreExceptionAtModeset(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p,ab;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};ab=Plan{};
    Setup(c,img,hp,p,v);c.exceptOnModeset=true;
    const uint32_t at=Run(p,0,p.count,c);
    assert(p.steps[at].op==Op::PollOr&&p.steps[at].reg==kRegCtrlDisp&&(c.R(kRegExcOther)&1));
    BuildHeadAbort(c.cfg,HPhase::Modeset,hp,ab,100,Kicked::Enable);
    (void)Run(ab,0,ab.count,c,true);
    assert(!(c.R(kRegExcOther)&1)&&c.R(0x611020)==0&&c.stage==0&&(c.R(0x688340)&0xff)==0);
}
// The VPLL0 frequency gate refuses a PLL encoding that does not give head 0's clock.
static void TestVpll0Gate(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};
    Setup(c,img,hp,p,v);c.R(0xef04)=0x00033a02;                       // M=2: half the clock
    const uint32_t at=Run(p,0,p.count,c);
    assert(p.steps[at].op==Op::ExpectVpll&&c.written.empty());
}
static void TestTrainingFailure(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p,ab;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};ab=Plan{};
    Setup(c,img,hp,p,v);c.sink.deadCr=true;
    const uint32_t at=Run(p,0,p.count,c);
    assert(at==p.phaseBegin[uint32_t(HPhase::DpTrain)]&&!c.tr.cr&&c.tr.crTries>=4&&c.updates==1);
    BuildHeadAbort(c.cfg,HPhase::DpTrain,hp,ab,100,Kicked::Rung);(void)Run(ab,0,ab.count,c,true);
    assert((c.R(RegChnCtl(0))&0x13)==0x01&&(c.R(0x61d130)&0xf)==0&&c.R(kRegInstTarget)==1);
    // Sinks the plan does not handle are refused by the trainer before any link write.
    Core d;static InstImage i2;static HeadPushes h2;static Plan p2;Vbios v2;i2=InstImage{};h2=HeadPushes{};p2=Plan{};
    Setup(d,i2,h2,p2,v2);d.sink.dpcd[2]&=0x7f;                       // no enhanced framing
    assert(Run(p2,0,p2.count,d)==p2.phaseBegin[uint32_t(HPhase::DpTrain)]&&d.tr.err==-8);
    Core e;static InstImage i3;static HeadPushes h3;static Plan p3;Vbios v3;i3=InstImage{};h3=HeadPushes{};p3=Plan{};
    Setup(e,i3,h3,p3,v3);e.sink.dpcd[0x0d]=1;                        // ASSR (eDP)
    assert(Run(p3,0,p3.count,e)==p3.phaseBegin[uint32_t(HPhase::DpTrain)]&&e.tr.err==-9);
}
// ---- AUX transfer (nv_aux_xfer.h) against the controller model
static void TestAux(){
    Core c;struct Io{Core& c;uint32_t rd(uint32_t a){return c.rd(a);} void wr(uint32_t a,uint32_t v){c.wr(a,v);} void udelay(uint32_t){}} io{c};
    brook::NvAux<Io> ax{io,4};
    assert(ax.Acquire()==0&&(c.R(Core::kAux+4)&0x07000000)==0x01000000);
    uint8_t caps[16]{};assert(ax.Dpcd(true,0,caps,16)==0&&caps[0]==0x12&&caps[1]==0x14&&caps[2]==0xc4);
    c.auxDefers=2;uint8_t one=0;assert(ax.Dpcd(true,1,&one,1)==0&&one==0x14);      // DEFER retried
    c.auxNack=1;assert(ax.Dpcd(true,1,&one,1)==-6);                                // NACK reported
    uint8_t lc[2]={0x14,0x84};assert(ax.Dpcd(false,0x100,lc,2)==0&&c.sink.dpcd[0x100]==0x14&&c.sink.dpcd[0x101]==0x84);
    uint8_t big[20]{};assert(ax.Dpcd(true,0,big,20)==0&&big[0]==0x12);              // two chunks
    ax.Release();assert(!(c.R(Core::kAux+4)&0x00710000));
    c.auxSink=false;assert(ax.Dpcd(true,0,caps,1)==-6);                             // no sink detected
    brook::NvAux<Io> other{io,3};uint8_t x;assert(other.Dpcd(true,0,&x,1)==-6);
}
// ---- head-1 runner against the fake core, AUX controller and sink
struct HRig{
    Core c;HBackend b{c};HeadRunner<HBackend>* r;
    HRig(){
        g_rom=MakeRom();
        r=new HeadRunner<HBackend>();r->b=&b;r->vb=g_rom.V();
        r->cfg.base=GopFlipConfig(3840,2160,16384,0xcf,256ull<<20);
        r->resolved=ResolveHeadConfig(r->vb,4,r->cfg,r->dpout);assert(r->resolved);
        {HeadConfig t=r->cfg;DpOut d{};assert(!ResolveHeadConfig(r->vb,4,t,d,0x12345678));} // wrong image hash refused
        c.cfg=r->cfg;c.vb=&r->vb;c.dpout=r->dpout;c.img=&r->img;c.hp=&r->pushes;c.Gop();
    }
    ~HRig(){delete r;}
};
static void TestHeadRunner(){
    auto phaseStart=[](HPhase ph){ // stop points from the plan itself
        static InstImage i2;static HeadPushes h2;static Plan p2;i2=InstImage{};h2=HeadPushes{};p2=Plan{};
        static Rom own;own=MakeRom();Vbios v=own.V();HeadConfig c;c.base=GopFlipConfig(3840,2160,16384,0xcf,256ull<<20);DpOut d{};
        assert(ResolveHeadConfig(v,4,c,d));c.pauseAfterModeset=false;assert(BuildHeadPlan(c,i2,h2,p2)==Err::Ok);
        return p2.phaseBegin[uint32_t(ph)];};
    {HRig g;g.r->Command("enable");assert(g.b.Count("\"nv_head1\":\"refused\"")==1&&g.c.written.empty());
     g.r->Command("preflight");assert(g.r->st.preflightOk&&g.c.written.empty());
     g.r->Command("enable");assert(g.b.Count("step-3 noop/flip must succeed")==1&&g.c.written.empty());
     g.b.s3busy=true;g.r->Command("enable");assert(g.b.Count("a step-3 run is live")==1);g.b.s3busy=false;
     g.b.s3ok=true;g.r->Command("enable");assert(g.b.dpIdle==1);
     assert(g.r->st.enabled&&g.r->st.live&&g.b.Count("\"nv_head1\":\"enabled\"")==1&&g.b.hooks==1);
     assert(g.b.Count("\"nv_head1_dp\":1,\"cr\":1,\"eq\":1")==1&&g.c.auxXfers>10);
     assert(g.c.R(0x68a464)==g.r->cfg.mode.rasterSize&&(g.c.R(0x688340)&0xff)==2);  // head 1 ARMed on SOR2
     assert(g.c.R(0x68a064)==g.r->cfg.base.gop.headRaster&&g.c.R(0x688320)==0x901);  // head 0 / SOR1 untouched
     g.r->Command("enable");assert(g.b.Count("a head-1 run is live")==1);
     g.r->Command("disable");
     assert(!g.r->st.live&&!g.r->st.enabled&&g.b.Count("\"nv_head1\":\"done\"")==1&&g.r->st.guardHits==0&&g.b.dpIdle==0);
     assert(g.b.Count("VPLL1 left running")==1);
     assert((g.c.R(0x688340)&0xff)==0&&(g.c.R(RegChnCtl(0))&0x6013)==0x01&&g.c.R(kRegInstTarget)==1);
     g.r->Command("enable skip-step3");assert(g.b.Count("run head1 preflight first")==2);}
    {HRig g;g.c.resetOnConnect=false;g.r->Command("preflight");g.r->Command("enable skip-step3");   // clean connect: replay skipped
     assert(g.r->st.enabled&&g.c.vrd(g.r->cfg.base.corePushVram+kReplaySkipWord*4)==PushJump(g.r->pushes.replayEnd));
     const uint32_t u0=g.c.updates;g.r->Command("unblank");
     assert(g.b.Count("\"nv_head1\":\"unblanked\"")==1&&g.c.updates==u0+1&&g.c.R(0x680244)==2&&g.r->st.enabled);
     g.r->Command("disable");assert(!g.r->st.live&&g.r->st.aborts==0&&g.r->st.softWarnings==0&&g.r->st.guardHits==0);}
    {HRig g;g.r->Command("preflight");g.r->Command("enable skip-step3");assert(g.r->st.enabled);  // connect resets: replay runs
     assert(g.c.vrd(g.r->cfg.base.corePushVram+kReplaySkipWord*4)!=PushJump(g.r->pushes.replayEnd));
     g.r->Command("disable");assert(!g.r->st.live&&g.r->st.aborts==0);}
    {HRig g;g.r->Command("unblank");assert(g.b.Count("head 1 is not enabled")==1&&g.c.written.empty());}
    // head1 show / hide: window 2 on head 1 with a fresh surface; window 0 / head 0 untouched.
    {HRig g;g.r->Command("show");assert(g.b.Count("head 1 is not enabled")==1&&g.c.written.empty());
     g.r->Command("preflight");g.r->Command("enable skip-step3");assert(g.r->st.enabled);
     const uint32_t win0=g.c.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0));
     g.r->Command("show");
     const Surface hs=HeadSurface(g.r->cfg);
     assert(g.b.Count("\"nv_head1\":\"shown\"")==1&&g.r->st.windowLive&&g.c.wupdatesCh[3]==1&&g.r->st.guardHits==0);
     assert(g.c.R(RegArm(3,c57e::SET_CONTEXT_DMA_ISO0))==kHandleIsoPitch&&g.c.R(RegArm(3,c57e::SET_OFFSET0))==uint32_t(hs.vram>>8));
     assert(hs.vram>=g.r->cfg.base.surf[1].vram+uint64_t(hs.pitch)*hs.height&&hs.vram+uint64_t(hs.pitch)*hs.height<=g.r->cfg.base.instVram);
     assert(g.c.R(RegArm(1,c57e::SET_CONTEXT_DMA_ISO0))==win0&&g.c.R(0x689000)==0);
     for(uint32_t a:g.c.written)assert(!(a>=0x690000&&a<0x691000)&&a!=RegChnCtl(1)&&!(a>=0x610b30&&a<0x610b40)); // window 0 untouched
     g.r->Command("show");assert(g.b.Count("already shown")==1);
     g.r->Command("hide");
     assert(!g.r->st.windowLive&&g.c.R(RegArm(3,c57e::SET_CONTEXT_DMA_ISO0))==0&&g.c.R(RegChnCtl(3))==0&&g.c.R(RegPbLo(3))==0);
     g.r->Command("show");assert(g.r->st.windowLive);
     g.r->Command("disable");                                                     // hides the window first
     assert(!g.r->st.windowLive&&!g.r->st.live&&g.r->st.aborts==0&&g.c.R(RegArm(3,c57e::SET_CONTEXT_DMA_ISO0))==0);
     assert(g.c.R(RegChnCtl(3))==0&&(g.c.R(0x688340)&0xff)==0&&g.r->st.guardHits==0);}
    {HRig g;g.r->Command("preflight");g.r->Command("enable skip-step3");g.r->Command("show");
     g.r->Command("abort");                                                       // window abort, then the head abort
     assert(!g.r->st.windowLive&&!g.r->st.live&&g.c.R(RegChnCtl(3))==0&&(g.c.R(RegChnCtl(0))&0x13)==0x01&&g.c.R(kRegInstTarget)==1);}
    {HRig g;g.r->Command("preflight");g.r->Command("enable skip-step3");g.r->Command("show");
     g.r->Quiesce();assert(!g.r->st.windowLive&&!g.r->st.live&&g.c.R(RegChnCtl(3))==0);}
    {HRig g;g.r->Command("preflight");g.r->Command("enable skip-step3");g.c.wexcept=true;g.r->Command("show"); // exception on the UPDATE
     assert(g.b.Count("show: window abort")==1&&!g.r->st.windowLive&&g.r->st.enabled&&g.c.R(RegChnCtl(3))==0&&(g.c.R(kRegExcWin)&4)==0);
     g.r->Command("disable");assert(!g.r->st.live&&g.r->st.aborts==0);}
    {HRig g;g.r->Command("preflight");g.r->Command("enable skip-step3");assert(g.r->st.enabled);
     g.r->Quiesce();                                                                  // reset while lit
     assert(!g.r->st.live&&g.r->st.aborts==1&&(g.c.R(0x688340)&0xff)==0&&(g.c.R(kRegCtrlDisp)&7)==0);
     assert((g.c.R(RegChnCtl(0))&0x13)==0x01&&g.c.R(kRegInstTarget)==1&&(g.c.R(0x61d130)&0xf)==0);}
    {HRig g;g.c.sink.deadCr=true;g.r->Command("preflight");g.r->Command("enable skip-step3");
     assert(!g.r->st.live&&g.r->st.aborts==1&&g.b.Count("\"nv_head1_dp\":1,\"cr\":0")==1&&g.c.updates==1);}
    {HRig g;g.r->Command("preflight");const uint32_t vs=phaseStart(HPhase::Vram)+1;
     g.r->Command(("enable skip-step3 stop "+std::to_string(vs)).c_str());           // stop in the VRAM phase
     assert(g.r->st.paused&&g.r->st.cursor==vs);g.r->Command("continue");assert(g.r->st.enabled);
     g.r->Command("abort");assert(!g.r->st.live&&(g.c.R(0x688340)&0xff)==0);}

    auto kickStart=[](){ // index of the enable kick in the runner's plan
        static InstImage i2;static HeadPushes h2;static Plan p2;i2=InstImage{};h2=HeadPushes{};p2=Plan{};
        static Rom own;own=MakeRom();Vbios v=own.V();HeadConfig c;c.base=GopFlipConfig(3840,2160,16384,0xcf,256ull<<20);DpOut d{};
        assert(ResolveHeadConfig(v,4,c,d));c.pauseAfterModeset=false;assert(BuildHeadPlan(c,i2,h2,p2)==Err::Ok);
        return KickIndex(p2,h2.enableEnd);};
    {HRig g;g.r->Command("preflight");
     g.r->Command(("enable skip-step3 stop "+std::to_string(kickStart())).c_str());   // stop at the enable kick
     assert(g.r->st.paused&&g.r->st.kicked==Kicked::None);g.c.R(0x680000+0x2068)=0x0004001e; // head 0 dirtied behind our back
     g.r->Command("continue");
     assert(g.r->st.head0Flagged&&g.b.Count("\"nv_head1\":\"WARNING\"")>=4&&g.r->st.aborts==0);
     assert(g.b.Count("skipping verify")==1&&g.b.Count("\"nv_head1\":\"done\"")==1&&!g.r->st.live);
     assert(g.c.supers==6&&g.c.R(0xef44)==0x00033a01&&(g.c.R(0x688340)&0xff)==0&&g.b.dpIdle==0);}
    {HRig g;g.r->Command("preflight");
     g.r->Command(("enable skip-step3 stop "+std::to_string(kickStart())).c_str());
     g.r->Command("abort");                                                          // unkicked enable: no flush, no detach
     assert(g.c.updates==1&&g.c.stage==0&&g.r->st.aborts==1&&g.b.Count("supervisor flush")==0&&g.b.dpIdle==0);}
    {HRig g;g.r->Command("preflight");g.r->Command("enable skip-step3 stop rung");    // the truncated run (needs no sink)
     assert(g.r->st.paused&&g.r->st.kicked==Kicked::Replay&&g.c.updates==0&&g.b.Count("nv_head1_diff")>=1);
     g.r->Command("abort");
     assert(!g.r->st.live&&g.r->st.aborts==1&&g.c.updates==0&&g.b.Count("supervisor flush")==0);
     assert((g.c.R(RegChnCtl(0))&0x6013)==0x01&&g.c.R(RegPbLo(0))==1&&g.c.R(kRegInstTarget)==1&&g.c.R(kRegPraminBase)==0x1fff0);
     assert(g.c.R(0x68a064)==g.r->cfg.base.gop.headRaster&&g.c.auxXfers==0&&g.r->st.guardHits==0&&g.b.dpIdle==0);}
    {HRig g;HeadRunner<HBackend>::RegIo io{*g.r};                                     // the write guard
     for(uint32_t a:{0x61c804u,0xef04u,0x616540u,0x612488u,0x612b00u,0x6104e4u,0x690000u}){
        io.violation=false;io.wr(a,1);assert(io.violation&&!g.c.written.count(a));}
     io.violation=false;io.wr(0x61d004,0x80000001);assert(!io.violation&&g.c.written.count(0x61d004));
     HeadPushes hp;ReadCoreReplay(g.r->cfg,[&](uint32_t a){return g.c.rd(a);});BuildHeadPushes(g.r->cfg,hp);
     assert(PushHead0MatchesArm(g.r->cfg,hp.core));
     Push bad;bad.Mthd(0x2064,1u);assert(!PushHead0MatchesArm(g.r->cfg,bad));         // head-0 raster != ARM
     Push ok;ok.Mthd(0x2064,g.c.R(0x68a064));assert(PushHead0MatchesArm(g.r->cfg,ok));
     Push undoc;undoc.Mthd(0x2038,1u);assert(!PushHead0MatchesArm(g.r->cfg,undoc));}  // not a replayable method
}
static void Dump(){
    static InstImage img;static HeadPushes hp;static Plan p,ab;
    g_rom=MakeRom();Vbios v=g_rom.V();HeadConfig c;c.base=GopFlipConfig(3840,2160,16384,0xcf,256ull<<20);DpOut d{};
    assert(ResolveHeadConfig(v,4,c,d));c.pauseAfterModeset=false;assert(BuildHeadPlan(c,img,hp,p)==Err::Ok);
    static const char* op[]={"WR","MASK","EXPECT","POLL","POLLSOFT","ACK","VCOPY","VWR","VEXPECT","VPOLL","VPOLLANY","ACKEXC",
        "PRAMIN","FENCE","KICK","EXPNOT","LOG","COPY","ASSYARM","PAUSE","DELAY","ANDNIF","COPYMASK","CALL","FILL","PRAMINR","DUMP","POLLOR","EXPSOFT","EXPVPLL","WRITEIF","EXPOR","REPLAYSKIP"};
    auto line=[&](uint32_t i,const Step& s){
        if(s.op==Op::VramCopy||s.op==Op::VramWrite||s.op==Op::VramPoll)printf("%3u %-8s vram %08llx val %08x %s\n",i,op[uint32_t(s.op)],(unsigned long long)s.vram,s.value,s.what);
        else if(s.op==Op::AssyArmDiff)printf("%3u %-8s chid %u mthd %04x..%04x %s\n",i,op[uint32_t(s.op)],s.reg,s.mask,s.value-4,s.what);
        else if(s.op==Op::CopyMask)printf("%3u %-8s %06x&%08x -> %06x (keep %08x)%s %s\n",i,op[uint32_t(s.op)],s.reg,s.mask,s.value,s.x,s.y?" if cond":"",s.what);
        else if(s.op==Op::Delay)printf("%3u %-8s %u us\n",i,op[uint32_t(s.op)],s.value);
        else printf("%3u %-8s %06x mask %08x val %08x%s %s\n",i,op[uint32_t(s.op)],s.reg,s.mask,s.value,s.timeoutMs?" (2000 ms)":"",s.what);};
    uint32_t ph=0;
    for(uint32_t i=0;i<p.count;++i){
        while(ph<uint32_t(HPhase::Count)&&p.phaseBegin[ph]==i){printf("## %s\n",HPhaseName(HPhase(ph)));++ph;}
        while(ph<uint32_t(HPhase::Count)&&p.phaseBegin[ph]<i)++ph;
        line(i,p.steps[i]);}
    printf("## core pushbuffer (%u words; enable ends at %u, disable at %u bytes)\n",hp.core.count,hp.enableEnd,hp.disableEnd);
    for(uint32_t i=0;i<hp.core.count;++i)printf("%s%08x%s",i%8?"":"  ",hp.core.words[i],i%8==7||i+1==hp.core.count?"\n":" ");
    BuildHeadAbort(c,HPhase::Modeset,hp,ab);printf("## abort (from modeset or later)\n");
    for(uint32_t i=0;i<ab.count;++i)line(i,ab.steps[i]);
}
int main(int argc,char** argv){
    if(argc>1&&!strcmp(argv[1],"--dump")){Dump();return 0;}
    TestAux();TestHeadRunner();
    TestVbios();TestRealVbios();TestPllAndSf();TestCoreWords();
    TestUnblankSegment();TestHeadPlan();TestRefusesWhenHead0Pending();TestUndocumentedResetLogged();TestRungSupervisorFailsFast();TestOpenStageReleased();TestHead0FlagFinishesStages();TestAbortUnkicked();
    TestStagesFromEventStatus();TestCoreExceptionAtModeset();TestVpll0Gate();TestTrainingFailure();
    puts("test_nv_disp_head: ok");return 0;
}
