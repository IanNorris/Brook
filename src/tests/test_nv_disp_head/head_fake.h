#pragma once
// Shared fakes for the head-1 and resident-driver host tests: a synthetic VBIOS, a DP
// sink, a fake display engine (core channel with supervisor stages, window channels
// 1 and 3, AUX ch 4) and a runner backend.
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
        for(uint32_t base:{0x682000u,0x68a000u}){R(base+0x38)=1;R(base+0x3c)=5;R(base+0x74)=1;R(base+0x20c)=1;} // probes2 non-zero head-0 values
        R(0x680240)=2;R(0x680244)=1;R(0x680248)=1;R(0x68024c)=1;        // live 35265134: GOP-pending blanking ctrl (ARM 0)
        // window 0 as the GOP leaves it (step 3 preflight; survey 058a8cd8)
        R(RegChnCtl(1))=g.ctlWndw;R(RegChnStatus(1))=0x24010100;
        for(uint32_t base:{RegAssy(1,0),RegArm(1,0)}){
            R(base+c57e::SET_SIZE)=g.winSize;R(base+c57e::SET_STORAGE)=g.winStorage;R(base+c57e::SET_PARAMS)=g.winParams;
            R(base+c57e::SET_PLANAR_STORAGE0)=g.winPlanar;R(base+c57e::SET_CONTEXT_DMA_ISO0)=g.winIso;R(base+c57e::SET_OFFSET0)=g.winOffset;
            R(base+0x370)=1;R(base+0x374)=1;}
        R(RegPbLo(1))=g.pbLo;R(RegPbHi(1))=g.pbHi;R(RegPbValid(1))=g.pbValid;R(RegPbClient(1))=g.pbClient;
        R(RegPut(1))=0x80000000;R(RegGet(1))=0x80000000;
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
    bool hideCtrlDisp=false,exceptOnModeset=false,resetOnConnect=true;uint32_t evt=0,flags=0,notes=0,openStage=0;
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
    // ---- window 2 channel (chid 3), for head1 show/hide
    // window channels (chid 1 = window 0, chid 3 = window 2), latched ASSY -> ARM on UPDATE
    std::map<uint32_t,uint32_t> wcurs;uint32_t wupdates=0,wupdatesCh[8]{};bool wexcept=false;std::set<uint32_t> wstall;
    void ExecuteW(uint32_t ch,uint32_t put){
        if(wstall.count(ch))return;                                    // channel wedged: GET stays
        uint32_t& wcur=wcurs[ch];
        const uint64_t pb=(uint64_t(R(RegPbLo(ch))&~1u)<<8);
        while(wcur!=put){
            const uint32_t hdr=vrd(pb+wcur);wcur+=4;
            if((hdr>>29)==1){wcur=hdr&0xffc;continue;}
            const uint32_t n=(hdr>>18)&0x3ff,m=hdr&0x3ffc;
            for(uint32_t i=0;i<n;++i){const uint32_t d=vrd(pb+wcur);wcur+=4;const uint32_t mm=m+i*4;R(RegAssy(ch,mm))=d;
                if(mm==c57e::UPDATE){
                    uint64_t b=0,nb=0;const uint32_t iso=R(RegAssy(ch,c57e::SET_CONTEXT_DMA_ISO0)),nh=R(RegAssy(ch,c57e::SET_CONTEXT_DMA_NOTIFIER));
                    if(wexcept||(iso&&!Lookup(iso,ch,b))||(nh&&!Lookup(nh,ch,nb))){
                        R(kRegExcWin)|=1u<<(ch-1);R(RegExcept(ch))=0x80000000|(mm>>2);R(RegGet(ch))=wcur;return;}
                    if(ch==3)assert(R(0x689100)==1);                        // window 2 owned by head 1
                    if(ch==1)assert((R(0x689000)&0xf)==0);                  // window 0 owned by head 0
                    for(uint32_t x=0x200;x<0x800;x+=4)if(reg.count(RegAssy(ch,x)))R(RegArm(ch,x))=R(RegAssy(ch,x));
                    if(nh)vwr(nb+(R(RegAssy(ch,c57e::SET_NOTIFIER_CONTROL))&0xff0),kNtfyBegun<<kNtfyStatusShift);
                    ++wupdates;++wupdatesCh[ch];}
            }
        }
        R(RegGet(ch))=put;
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
                if(resetOnConnect){R(0x682068)=0;R(0x680320)=0x100;R(0x681004)=0;R(0x682404)=0x123;R(0x682074)=0;}} // connect resets core ASSY
            return;}
        if(a==RegPut(0)&&(R(RegChnCtl(0))&kCtlRunning)==kCtlRunning){R(a)=v;Execute(v);return;}
        for(uint32_t ch:{1u,3u}){
            if(a==RegChnCtl(ch)){const uint32_t old=R(a);R(a)=v;
                if((v&kCtlRunning)==kCtlRunning&&!(old&kCtlConnect)){assert(R(RegPbClient(ch))==kClientId&&(R(kRegInstTarget)&8));
                    R(RegChnStatus(ch))=0x04040100;R(RegGet(ch))=0;wcurs[ch]=0;}
                if(!(v&kCtlConnect)&&(old&kCtlConnect))R(RegChnStatus(ch))=0x01010100;
                return;}
            if(a==RegPut(ch)&&(R(RegChnCtl(ch))&kCtlRunning)==kCtlRunning&&!(R(RegChnCtl(ch))&kCtlTrashAbort)){R(a)=v;ExecuteW(ch,v);return;}
            if(a==RegExcept(ch)&&v==kExceptAck){R(a)=0;return;}}
        if(a==kRegExcWin){R(a)&=~v;return;}
        if(a>=0x611800&&a<0x611820){R(a)&=~v;return;}                   // EVT_STAT_HEAD_TIMING W1C
        if(a==kRegSuperAck){R(kRegCtrlDisp)&=~v;evt&=~v;openStage=v&7;return;}
        if(a==kRegExcOther){R(a)&=~v;return;}
        if(a==0x611020&&v==kExceptAck){R(a)=0;return;}
        if(a==kRegSuperStat&&v==kSuperRelease)openStage=0;
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
    bool call(uint32_t id,uint32_t ms){
        if(id==kCallSuperFlush||id==kCallSuperFlushVpll){(void)SupervisorFlush(*this,cfg,id==kCallSuperFlushVpll,ms?ms:100,openStage);return true;}
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
