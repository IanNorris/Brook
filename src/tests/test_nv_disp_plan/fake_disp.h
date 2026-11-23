#pragma once
// Fake TU104 display engine for the nv_disp_plan / nv_disp_runner host tests:
// parses the window pushbuffer from fake VRAM, resolves ctxdma handles through
// the RAMHT image as the hardware does, and latches ASSY -> ARM on UPDATE.
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <string>
#include "nv_disp_plan.h"
namespace fake_disp {
using namespace brook::nvdisp;
inline Config DellConfig(){
    Config c;
    const Surface a{0x0000000,3840,2160,16384,c57e::FORMAT_A8R8G8B8};
    const Surface b{0x2200000,3840,2160,16384,c57e::FORMAT_A8R8G8B8};
    c.surf[0]=a;c.surf[1]=b;
    c.instVram=0x0ff00000;c.wndwPushVram=0x0ff10000;c.corePushVram=0x0ff11000;c.syncVram=0x0ff12000;
    c.isoLimit=0x0fffffff; // BAR1 aperture (256 MiB), BAR1 == VRAM identity checked via PRAMIN
    return c;
}
// --- fake display engine
struct Fake{
    std::map<uint32_t,uint32_t> reg;
    std::map<uint64_t,std::vector<uint8_t>> pages; // 4 KiB VRAM pages
    const InstImage* img=nullptr;const Pushes* push=nullptr;
    std::set<uint32_t> written;std::vector<std::string> trace;
    uint32_t wndwCur=0;          // HW fetch pointer, bytes
    bool idleOnConnect=true,breakRamht=false;
    uint32_t exceptions=0,flips=0;int failUpdateAt=-1;bool resetOnConnect=true;
    // GET stall model (flip_4_1000): fetching stops at header offset stallAt; the next PUT
    // write resumes it (one-shot) unless stallSticky.
    uint32_t stallAt=~0u;bool stallSticky=false;
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
        // Non-default GOP window state the survey saw (0x690000+), mirrored in ARM.
        for(uint32_t base:{RegAssy(ch,0),RegArm(ch,0)}){
            R(base+0x2a8)=0x11;R(base+0x2ec)=0x10000;R(base+0x2f4)=1;R(base+0x360)=1;R(base+0x364)=0x1300;
            R(base+0x368)=2;R(base+0x370)=1;R(base+0x374)=1;R(base+0x460)=0x10000;R(base+0x4c0)=0x460d0;}
        for(uint32_t c2:{0u,ch}){R(RegPbLo(c2))=g.pbLo;R(RegPbHi(c2))=g.pbHi;R(RegPbValid(c2))=g.pbValid;R(RegPbClient(c2))=g.pbClient;}
        R(RegArm(ch,c57e::UPDATE))=0xacf3fbfb;R(RegAssy(ch,0x208))=0x554; // live 058a8cd8: UPDATE slot, GET_LINE
        R(kRegFeEnable)=g.feEnable;
        R(RegPut(ch))=0x80000000;R(RegGet(ch))=0x80000000;
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
            if(wndwCur==stallAt){R(RegGet(chid))=wndwCur;if(!stallSticky)stallAt=~0u-1;return;}
            if(stallAt==~0u-1)stallAt=~0u;
            const uint32_t hdr=vrd(pb+wndwCur);wndwCur+=4;
            if((hdr>>29)==1){wndwCur=hdr&0xffc;continue;}
            assert((hdr>>29)==0);
            const uint32_t n=(hdr>>18)&0x3ff,m=hdr&0x3ffc;
            for(uint32_t i=0;i<n;++i){const uint32_t d=vrd(pb+wndwCur);wndwCur+=4;
                const uint32_t mm=m+i*4;R(RegAssy(chid,mm))=d;
                if(mm==c57e::UPDATE){
                    if(failUpdateAt>=0&&flips==uint32_t(failUpdateAt)){failUpdateAt=-1;Except(chid,mm);return;} // one-shot
                    uint64_t ib,il,nb,nl;const uint32_t iso=R(RegAssy(chid,c57e::SET_CONTEXT_DMA_ISO0));
                    if(!Lookup(iso,chid,ib,il)){Except(chid,mm);return;}
                    const uint32_t nh=R(RegAssy(chid,c57e::SET_CONTEXT_DMA_NOTIFIER)); // 0 = no notifier
                    if(nh&&!Lookup(nh,chid,nb,nl)){Except(chid,mm);return;}
                    const uint64_t off=uint64_t(R(RegAssy(chid,c57e::SET_OFFSET0)))<<8;
                    const uint32_t pitch=(R(RegAssy(chid,c57e::SET_PLANAR_STORAGE0))&0x1fff)<<6;
                    const uint32_t hgt=R(RegAssy(chid,c57e::SET_SIZE))>>16;
                    assert(ib+off+uint64_t(pitch)*hgt-1<=il); // surface within ctxdma limit
                    assert(R(RegAssy(chid,c57e::SET_INTERLOCK_FLAGS))==0); // no core interlock in step 3
                    for(uint32_t x=0x200;x<0x800;x+=4)if(reg.count(RegAssy(chid,x)))R(RegArm(chid,x))=R(RegAssy(chid,x));
                    if(nh){const uint32_t ntfy=R(RegAssy(chid,c57e::SET_NOTIFIER_CONTROL))&0xff0;
                        assert(nb+ntfy+15<=nl);vwr(nb+ntfy,kNtfyBegun<<kNtfyStatusShift);}
                    ++flips;
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
        if(a==kRegInstTarget&&(v&8))assert(R(kRegInstAddr)!=0); // never valid at VRAM 0
        if(a==kRegExcWin){R(a)&=~v;if(!R(a))R(kRegIntrDispatch)&=~0x200u;return;}
        for(uint32_t ch=0;ch<=8;++ch){
            if(a==RegChnCtl(ch)){
                const uint32_t old=R(a);R(a)=v;
                if((v&kCtlRunning)==kCtlRunning&&(old&kCtlConnect)==0){
                    assert(R(RegPbLo(ch))&1);assert(R(RegPbClient(ch))==kClientId);
                    assert((R(kRegInstTarget)&8)&&R(kRegInstAddr));     // instmem before connect
                    assert(R(RegPbLo(ch))>1);                            // our pushbuffer, not the GOP's
                    if(idleOnConnect)R(RegChnStatus(ch))=ch==0?0x000b0000:0x04040100;
                    if(ch&&resetOnConnect){ // live 23 Nov: connect re-initialises ASSY (0x22c -> 0xe9)
                        R(RegAssy(ch,0x22c))=0xe9;R(RegAssy(ch,0x2ec))=0;R(RegAssy(ch,0x460))=0;R(RegAssy(ch,0x360))=0;}
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
    uint32_t fills=0;
    void fill(uint64_t vram,uint32_t bytes,uint32_t pitch,uint32_t colour,uint32_t marker,uint32_t lines){
        ++fills;assert(pitch&&bytes%pitch==0&&lines<bytes/pitch);
        vwr(vram,marker);vwr(vram+uint64_t(lines)*pitch,colour);vwr(vram+bytes-4,colour); // samples only
    }
    bool expired(uint32_t ms,uint32_t& t){return ++t>ms;}
    void except(uint32_t chid,uint32_t s,uint32_t d,uint32_t c){trace.push_back("exception chid "+std::to_string(chid));(void)s;(void)d;(void)c;}
    bool verbose=false;uint32_t diffs=0,pauses=0;
    void diff(uint32_t m,uint32_t a,uint32_t b){++diffs;if(verbose)printf("  diff %03x assy %08x arm %08x\n",m,a,b);}
    void pause(const char*){++pauses;}
    uint32_t flags=0;void flag(uint32_t,const Step&,uint32_t){++flags;} void note(const char*,uint32_t){}
    uint32_t dumped=0;void dump(uint32_t,const uint32_t*,uint32_t n){dumped+=n;}
    void udelay(uint32_t){}
    bool call(uint32_t,uint32_t){return true;}
    void log(uint32_t i,const Step& s,uint32_t got,bool ok){
        if(verbose||!ok)printf("  %3u %-12s %s got %08x\n",i,ok?"ok":"FAIL",s.what,got);}
};
} // namespace fake_disp
