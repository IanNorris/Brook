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

// ------------------------------------------------------------ synthetic VBIOS
struct Rom{
    std::vector<uint8_t> d=std::vector<uint8_t>(0x8000,0);
    void w8(uint32_t o,uint8_t v){d[o]=v;} void w16(uint32_t o,uint16_t v){d[o]=uint8_t(v);d[o+1]=uint8_t(v>>8);}
    void w32(uint32_t o,uint32_t v){w16(o,uint16_t(v));w16(o+2,uint16_t(v>>16));}
    Vbios V()const{return Vbios{d.data(),uint32_t(d.size())};}
};
static uint8_t CfgDc(uint32_t i){return uint8_t(0x10+i);}
static uint8_t CfgPe(uint32_t i){return uint8_t(i*2);}
static uint8_t CfgPu(uint32_t i){return uint8_t(2+i%5);}
static Rom MakeRom(){
    Rom r;
    // BIT at 0x100: signature, entry size 6 at +9, 2 entries at +10, entries from +12 (bit.c 27-49)
    const uint32_t b=0x100;r.w8(b,0xff);r.w8(b+1,0xb8);r.w8(b+2,'B');r.w8(b+3,'I');r.w8(b+4,'T');
    r.w8(b+9,6);r.w8(b+10,2);
    r.w8(b+12,'C');r.w8(b+13,2);r.w16(b+14,4);r.w16(b+16,0x400);   // 'C' v2 -> u32 at 0x400
    r.w8(b+18,'d');r.w8(b+19,1);r.w16(b+20,2);r.w16(b+22,0x420);   // 'd' v1 -> u16 at 0x420
    // PLL limits v0x50 at 0x1000: hdr 4, len 19, cnt 3 (a VPLL for heads 0, 1 and a decoy)
    r.w32(0x400,0x1000);r.w8(0x1000,0x50);r.w8(0x1001,4);r.w8(0x1002,19);r.w8(0x1003,3);
    const uint8_t types[]={0x01,0x80,0x81};
    for(uint32_t i=0;i<3;++i){const uint32_t e=0x1004+i*19;r.w8(e,types[i]);
        r.w16(e+1,i==0?100:27);r.w16(e+5,800);r.w16(e+7,1620);r.w16(e+9,13);r.w16(e+11,38);
        r.w8(e+13,1);r.w8(e+14,1);r.w8(e+15,20);r.w8(e+16,255);r.w8(e+17,1);r.w8(e+18,31);}
    // DP table v0x42 at 0x2000: hdr 13, ptr len 2, cnt 2; dpout hdr 0x13; 4 cfg tables x 10 entries x 3 B
    r.w16(0x420,0x2000);const uint32_t t=0x2000;
    r.w8(t,0x42);r.w8(t+1,13);r.w8(t+2,2);r.w8(t+3,2);r.w8(t+4,0x13);r.w8(t+5,4);r.w8(t+6,3);r.w8(t+7,10);
    r.w16(t+13,0x2200);r.w16(t+15,0x2300);
    const uint32_t cfg=t+13+2*2;
    for(uint32_t i=0;i<40;++i){r.w8(cfg+i*3,CfgDc(i));r.w8(cfg+i*3+1,CfgPe(i));r.w8(cfg+i*3+2,CfgPu(i));}
    // dpout[0]: another output (SOR1-B, hashm 0x0f82); dpout[1]: ours (0x0006 / 0x0f44), cfg index 0
    r.w16(0x2200,0x0006);r.w16(0x2202,0x0f82);r.w8(0x2200+0x11,1);
    r.w16(0x2300,0x0006);r.w16(0x2302,0x0f44);r.w8(0x2304,0x2);r.w16(0x2305,0x611f);r.w16(0x2307,0x5bec);
    r.w16(0x2309,0x6107);r.w16(0x230b,0x62ab);r.w16(0x230d,0x629a);r.w16(0x230f,0x5c55);r.w8(0x2311,0);
    // DCB 4.1 at 0x3000 (ptr at 0x36): hdr 0x23, cnt 5, len 8, signature at +6 (dcb.c 27-60)
    r.w16(0x36,0x3000);const uint32_t dcb=0x3000;
    r.w8(dcb,0x41);r.w8(dcb+1,0x23);r.w8(dcb+2,5);r.w8(dcb+3,8);r.w32(dcb+6,0x4edcbdcb);
    auto entry=[&](uint32_t i,uint32_t conn,uint32_t conf){r.w32(dcb+0x23+i*8,conn);r.w32(dcb+0x23+i*8+4,conf);};
    entry(0,0x02000f66,0x0f400020);              // SOR1 (or 2), conn 0, CCB 6, DP, link B
    entry(4,0x04002f76,0x0f400010);              // SOR2 (or 4), conn 2, heads f, CCB 7, DP, link A, 5.4 G x4
    const uint8_t rates[]={0x1e,0x14,0x0a,0x06};const uint16_t scripts[]={0x616a,0x617b,0x618c,0x619d}; // dpout lnkcmp list
    for(uint32_t i=0;i<4;++i){r.w8(0x6107+i*3,rates[i]);r.w16(0x6108+i*3,scripts[i]);}
    return r;
}

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
static void TestCoreWords(){
    HeadConfig c;HeadPushes hp;BuildHeadPushes(c,hp);
    assert(hp.core.words[0]==PushHeader(0x208,1)&&hp.core.words[1]==kHandleSync);  // ours, first
    // replay: every documented core method once, in runs; then the rung (corec37d_update + ntfy)
    {uint32_t n=0;for(uint32_t i=2;i<hp.replayEnd/4;){const uint32_t k=(hp.core.words[i]>>18)&0x3ff;n+=k;i+=1+k;}
     assert(n==kCoreReplayCount&&kCoreReplayCount==3+16+40+4*110);}
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

// ------------------------------------------------------------- fake DP sink
struct Sink{
    uint8_t dpcd[0x1000]{};uint32_t needVs=2,needPe=1;bool deadCr=false;
    Sink(){dpcd[0]=0x12;dpcd[1]=0x14;dpcd[2]=0xc4;dpcd[0x0e]=0;}
    uint32_t Lanes()const{return dpcd[0x101]&0x1f;}
    void Update(){
        const uint8_t tp=dpcd[0x102]&0x0f;uint8_t lane[4]{};bool allEq=true;
        for(uint32_t i=0;i<Lanes();++i){
            const uint32_t vs=dpcd[0x103+i]&3,pe=(dpcd[0x103+i]>>3)&3;
            uint8_t st=0,adj=0;
            const bool cr=!deadCr&&vs>=needVs;if(cr&&tp)st|=1;
            if(tp==2||tp==3){if(cr&&pe>=needPe)st|=6;else allEq=false;}
            adj=uint8_t((cr?vs:needVs)|((tp>=2?needPe:pe)<<2));
            lane[i]=st;dpcd[0x206+(i>>1)]=uint8_t((dpcd[0x206+(i>>1)]&~(0xf<<((i&1)*4)))|(adj<<((i&1)*4)));
        }
        dpcd[0x202]=uint8_t(lane[0]|lane[1]<<4);dpcd[0x203]=uint8_t(lane[2]|lane[3]<<4);
        dpcd[0x204]=(tp>=2&&allEq)?1:0;
    }
};

// -------------------------------------------------- fake core display engine
struct Core{
    std::map<uint32_t,uint32_t> reg;std::map<uint64_t,std::vector<uint8_t>> pages;
    const InstImage* img=nullptr;const HeadPushes* hp=nullptr;const Vbios* vb=nullptr;
    HeadConfig cfg;DpOut dpout{};Sink sink;DpTrainResult tr;
    std::set<uint32_t> written;std::vector<std::string> trace;
    uint32_t cur=0,stage=0,supers=0,diffs=0,pauses=0,updates=0;uint32_t pendingPut=0;bool superOnHead0=false;
    uint32_t ntfyOff=0;
    uint32_t& R(uint32_t a){return reg[a];}
    uint8_t* V(uint64_t a){auto& p=pages[a&~0xfffull];if(p.empty())p.assign(4096,0);return p.data()+(a&0xfff);}
    uint32_t vrd(uint64_t a){uint32_t v;memcpy(&v,V(a),4);return v;}
    void vwr(uint64_t a,uint32_t v){memcpy(V(a),&v,4);}
    void Gop(){
        const auto& g=cfg.base.gop;
        R(kRegInstTarget)=1;R(kRegInstAddr)=0;R(kRegPraminBase)=0x1fff0;
        R(RegChnCtl(0))=g.ctlCore;R(RegChnStatus(0))=0x20070000;
        for(uint32_t base:{0x680000u,0x688000u}){
            R(base+0x2064)=g.headRaster;R(base+0x200c)=g.headPclk;R(base+0x2004)=0xfc000040;
            for(uint32_t s=0;s<4;++s)R(base+0x300+s*0x20)=s==1?g.sorCtl:0x100;
            for(uint32_t w=0;w<8;++w)R(base+0x1000+w*0x80)=w==0?0:0xf;
            R(base+0x1004)=g.coreFmtUsage;R(base+0x1010)=g.coreUsage;
            for(uint32_t h=1;h<4;++h)R(base+0x2064+h*0x400)=0x00050008; // idle 8x5 raster
        }
        R(RegPbLo(0))=1;R(RegPbValid(0))=1;R(0x612508)=0x603;R(0x61d004)=0x10000000;R(0x611cf0)=0x1ff;
        R(0xef04)=0x00033a01;R(0xef10)=0x18000000;                       // GOP VPLL0 = 533.25 MHz
        R(0x612200)=0x00800040;R(0x612b00)=0x03d10040;
    }
    bool Lookup(uint32_t handle,uint32_t chid,uint64_t& base){
        const uint64_t inst=uint64_t(R(kRegInstAddr))<<16;
        for(uint32_t n=0;n<kHashEntries;++n){const uint32_t s=(RamhtHash(handle,chid)+n)&(kHashEntries-1);
            const uint32_t h=vrd(inst+s*8),ctx=vrd(inst+s*8+4);
            if(ctx&&h==handle&&(ctx>>25)==chid){const uint32_t off=((ctx>>14)&0x7ff)<<5;
                base=(uint64_t(vrd(inst+off+4))|uint64_t(vrd(inst+off+8))<<32)<<8;return true;}}
        return false;
    }
    uint32_t HeadMask(uint32_t h){
        uint32_t m=0;
        for(uint32_t o=0x2000+h*0x400;o<0x2400+h*0x400;o+=4)if(reg.count(0x680000+o)&&R(0x680000+o)!=R(0x688000+o))m|=0x11000;
        for(uint32_t s=0;s<4;++s){const uint32_t a=R(0x680300+s*0x20),b=R(0x688300+s*0x20);
            if(a!=b&&(((a|b)&0xff)>>h)&1)m|=0x1000;}
        return m;
    }
    bool hideCtrlDisp=false,exceptOnModeset=false,resetOnConnect=true;uint32_t evt=0,flags=0,notes=0;
    void Raise(uint32_t st){stage=st;if(!hideCtrlDisp)R(kRegCtrlDisp)|=1u<<(st-1);evt|=1u<<(st-1);++supers;
        for(uint32_t h=0;h<4;++h){R(RegSuperHead(h))=HeadMask(h);if(h==0&&R(RegSuperHead(0)))superOnHead0=true;}}
    void Finish(){
        for(auto& kv:std::map<uint32_t,uint32_t>(reg))if(kv.first>=0x680200&&kv.first<0x683000)R(kv.first+0x8000)=kv.second;
        uint64_t nb;assert(Lookup(R(0x680208),0,nb));vwr(nb+ntfyOff,kNtfyFinished<<kNtfyStatusShift);
        R(RegGet(0))=pendingPut;stage=0;
    }
    void Execute(uint32_t put){
        const uint64_t pb=(uint64_t(R(RegPbLo(0))&~1u)<<8);
        while(cur!=put){
            const uint32_t hdr=vrd(pb+cur);cur+=4;
            if((hdr>>29)==1){cur=hdr&0xffc;continue;}
            const uint32_t n=(hdr>>18)&0x3ff,m=hdr&0x3ffc;
            for(uint32_t i=0;i<n;++i){const uint32_t d=vrd(pb+cur);cur+=4;const uint32_t mm=m+i*4;
                if(mm>=0x2000&&mm<0x2400)assert(d==R(0x688000+mm));  // head-0 methods only as their ARM value (replay)
                R(0x680000+mm)=d;
                if(mm==c57d::SET_NOTIFIER_CONTROL&&(d&0x1000))ntfyOff=d&0xff0;
                if(mm==c57d::UPDATE){++updates;pendingPut=put;
                    if(exceptOnModeset&&R(0x680340)==0x802){R(kRegExcOther)|=1;R(0x611020)=0x80004080;exceptOnModeset=false;return;}
                    bool any=false;for(uint32_t h=0;h<4;++h)if(HeadMask(h))any=true;
                    if(any){Raise(1);return;} Finish();}
            }
        }
        if(!stage)R(RegGet(0))=put;
    }
    // ---- AUX controller model (auxgm200 register layout), channel 4 only
    uint32_t auxDefers=0,auxNack=0,auxXfers=0;bool auxSink=true;
    static constexpr uint32_t kAux=0xd950+4*0x50;
    bool AuxReg(uint32_t a)const{return a>=0xd930&&a<0xd930+8*0x50;}
    uint32_t AuxRd(uint32_t a){
        if(a<0xd930+4*0x50||a>=0xd930+5*0x50)return 0; // other channels: idle, no sink
        if(a==kAux+0x08)return (R(a)&~0x10000000u)|(auxSink?0x10000000u:0);
        return R(a);}
    void AuxWr(uint32_t a,uint32_t v){
        if(a==kAux+0x04){ // ctrl
            uint32_t c=v;if((c&0x00700000)==0x00100000)c=(c&~0x07000000u)|0x01000000u; // grant
            if(!(c&0x00700000))c&=~0x07000000u;
            if(c&0x00010000){ // transaction
                ++auxXfers;const uint32_t type=(c>>12)&0xf,size=(c&0x1ff)==0x100?0:(c&0xf)+1,addr=R(kAux);
                uint32_t st=0x10000000u;
                if(auxDefers){--auxDefers;st|=0x2u<<16;}
                else if(auxNack){--auxNack;st|=0x1u<<16;}
                else if(type==brook::kAuxNativeRead){sink.Update();uint8_t buf[16]{};memcpy(buf,sink.dpcd+addr,size);
                    for(uint32_t i=0;i<16;i+=4){uint32_t w;memcpy(&w,buf+i,4);R(kAux-0x10+i)=w;}st|=size;}
                else if(type==brook::kAuxNativeWrite){for(uint32_t i=0;i<size;++i)sink.dpcd[addr+i]=uint8_t(R(kAux-0x20+(i&~3u))>>(8*(i&3)));}
                R(kAux+0x08)=st;c&=~0x00010000u;}
            R(a)=c;return;}
        if(a==kAux+0x08){R(a)&=~v;return;} // W1C
        R(a)=v;}
    uint32_t rd(uint32_t a){
        if(AuxReg(a))return AuxRd(a);
        if(a>=kPramin&&a<kPramin+0x100000)return vrd((uint64_t(R(kRegPraminBase))<<16)+(a-kPramin));
        if(a==kRegSuperAck)return evt;
        if(a==0x612508){const uint32_t v=R(a);return (v&~0x80u)|((((v>>8)&0xf)>6)?0x80:0);} // pad-cal comparator: target 6
        return R(a);
    }
    void wr(uint32_t a,uint32_t v){
        written.insert(a);
        if(AuxReg(a)){AuxWr(a,v);return;}
        if(a==kRegBar1Flush){R(a)=0;return;}
        if(a==kRegInstTarget&&(v&8))assert(R(kRegInstAddr)!=0);
        if(a==0x61d004||a==0x61d034){R(a)=v&~0x80000000u;return;}   // sequencers complete at once
        if(a==RegChnCtl(0)){const uint32_t old=R(a);R(a)=v;
            if((v&kCtlRunning)==kCtlRunning&&!(old&kCtlConnect)){assert((R(RegPbLo(0))>>8)&&R(RegPbClient(0))==kClientId);
                R(RegChnStatus(0))=0x000b0000;R(RegGet(0))=0;cur=0;
                if(resetOnConnect){R(0x682068)=0;R(0x680320)=0x100;R(0x681004)=0;R(0x682404)=0x123;}} // connect resets core ASSY
            return;}
        if(a==RegPut(0)&&(R(RegChnCtl(0))&kCtlRunning)==kCtlRunning){R(a)=v;Execute(v);return;}
        if(a==kRegSuperAck){R(kRegCtrlDisp)&=~v;evt&=~v;return;}
        if(a==kRegExcOther){R(a)&=~v;return;}
        if(a==0x611020&&v==kExceptAck){R(a)=0;return;}
        if(a==kRegSuperStat&&v==kSuperRelease&&stage){
            assert(!(evt&(1u<<(stage-1))));                         // acked before release
            if(stage==2&&R(RegSuperHead(1))==0){}                    // masks cleared by the plan
            if(stage<3)Raise(stage+1);else Finish();
            return;}
        R(a)=v;
    }
    // ---- executor Io
    void vcopy(uint64_t v,Blob b,uint32_t bytes){for(uint32_t i=0;i<bytes;i+=4)vwr(v+i,blob(b,i));}
    uint32_t blob(Blob b,uint32_t off){
        if(b==Blob::Inst)return img->words[off/4];
        if(b==Blob::CorePush)return off/4<hp->core.count?hp->core.words[off/4]:0;
        return 0;}
    void fence(){} bool expired(uint32_t ms,uint32_t& t){return ++t>ms;}
    void except(uint32_t,uint32_t,uint32_t,uint32_t){} void diff(uint32_t,uint32_t,uint32_t){++diffs;}
    void pause(const char*){++pauses;} void udelay(uint32_t){}
    void flag(uint32_t,const Step&,uint32_t){++flags;} void note(const char*,uint32_t){++notes;}
    void dump(uint32_t,const uint32_t*,uint32_t){}
    void fill(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){}
    bool call(uint32_t id){
        if(id==kCallSuperFlush||id==kCallSuperFlushVpll){(void)SupervisorFlush(*this,cfg,id==kCallSuperFlushVpll,100);return true;}
        if(id!=kCallDpTrain)return false;
        DpLink l=cfg.dp;memcpy(l.dpcd,sink.dpcd,16);
        struct TIo{Core& c;
            uint32_t rd(uint32_t a){return c.rd(a);} void wr(uint32_t a,uint32_t v){c.wr(a,v);}
            void udelay(uint32_t){}
            int aux(bool read,uint32_t addr,uint8_t* b,uint32_t n){
                if(read){c.sink.Update();memcpy(b,c.sink.dpcd+addr,n);}else memcpy(c.sink.dpcd+addr,b,n);return 0;}
            bool drive(uint32_t,uint8_t pc,uint8_t vs,uint8_t pe,DpCfg& o){return DpDrive(*c.vb,c.dpout,pc,vs,pe,o);}
        } io{*this};
        return DpTrain(io,l,tr);
    }
    bool verbose=false;
    void log(uint32_t i,const Step& s,uint32_t got,bool ok){if(verbose||!ok)fprintf(stderr,"  %3u %-5s %s got %08x\n",i,ok?"ok":"FAIL",s.what,got);}
};

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
    assert(at<p.phaseBegin[uint32_t(HPhase::Vram)]&&c.written.empty()&&c.diffs==1);
    // A documented one is simply replayed with its ARM value: no refusal.
    Core d;static InstImage i2;static HeadPushes h2;static Plan p2;Vbios v2;i2=InstImage{};h2=HeadPushes{};p2=Plan{};
    Setup(d,i2,h2,p2,v2);d.R(0x680000+0x2068)=0x0004001e;
    assert(Run(p2,0,p2.count,d)==p2.count&&d.flags==0&&d.R(0x68a068)==d.R(0x682068));
}
// An undocumented method the core connect resets cannot be replayed: the diffs log it
// without failing, and if it matters the rung UPDATE raises a supervisor, so the run
// stops there, before any head-1 state, and the abort flushes the stages cleanly.
static void TestUndocumentedResetLogged(){
    Core c;static InstImage img;static HeadPushes hp;static Plan p,ab;Vbios v;img=InstImage{};hp=HeadPushes{};p=Plan{};ab=Plan{};
    Setup(c,img,hp,p,v);
    const uint32_t k=p.phaseBegin[uint32_t(HPhase::CoreCheck)];
    assert(Run(p,0,k,c)==k);c.R(0x680000+0x2038)=9;                   // as if the connect reset it
    const uint32_t at=Run(p,k,p.count,c);
    assert(at==KickIndex(p,hp.rungEnd)+1&&c.diffs>=2&&c.stage==1);     // logged twice, then stuck at the rung
    BuildHeadAbort(c.cfg,HPhase::CoreCheck,hp,ab,100,Kicked::Rung);
    (void)Run(ab,0,ab.count,c,true);
    assert(c.stage==0&&c.supers==3&&(c.R(RegChnCtl(0))&0x13)==0x01&&c.R(kRegInstTarget)==1&&(c.R(0x688340)&0xff)==0);
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
struct HBackend{
    Core& c;uint64_t t=0;uint32_t hooks=0;bool s3busy=false,s3ok=false;std::vector<std::string> lines;
    explicit HBackend(Core& core):c(core){}
    int dpIdle=0;void dpIdleSuspend(bool s){dpIdle+=s?1:-1;assert(dpIdle==0||dpIdle==1);}
    uint32_t rd(uint32_t a){t+=1;return c.rd(a);} void wr(uint32_t a,uint32_t v){t+=1;c.wr(a,v);}
    uint32_t vrd(uint64_t a){return c.vrd(a);} void vwr(uint64_t a,uint32_t v){c.vwr(a,v);}
    void fill(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){}
    void fence(){} uint64_t nowUs(){return t+=5;} void relax(){}
    void emit(const char* l){lines.push_back(l);if(getenv("NV_HEAD_VERBOSE"))fputs(l,stderr);} void hookQuiesce(){++hooks;}
    bool step3Busy(){return s3busy;} bool step3Succeeded(){return s3ok;}
    size_t Count(const char* n)const{size_t k=0;for(auto& l:lines)if(l.find(n)!=std::string::npos)++k;return k;}
};
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
        "PRAMIN","FENCE","KICK","EXPNOT","LOG","COPY","ASSYARM","PAUSE","DELAY","ANDNIF","COPYMASK","CALL","FILL","PRAMINR","DUMP","POLLOR","EXPSOFT","EXPVPLL"};
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
    TestHeadPlan();TestRefusesWhenHead0Pending();TestUndocumentedResetLogged();TestHead0FlagFinishesStages();TestAbortUnkicked();
    TestStagesFromEventStatus();TestCoreExceptionAtModeset();TestVpll0Gate();TestTrainingFailure();
    puts("test_nv_disp_head: ok");return 0;
}
