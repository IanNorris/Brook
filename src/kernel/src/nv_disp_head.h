#pragma once
// RTX 2080 (TU104) step 4: light a second head (head 1 -> SOR2 DP link A ->
// conn2, AUX ch 4) next to the GOP's head 0, without NVIDIA firmware.
// Pure, allocation-free plan builder in the style of nv_disp_plan.h: VBIOS
// table parsing (DCB, PLL limits, DP drive table), VPLL coefficients, DP
// link training (a state machine over an Io with DPCD access), the core
// channel bring-up, head-1/SOR2 core methods, a first core UPDATE that must
// leave head 0 untouched, and the supervisor handshake.
//
// Clean-room reimplementation after nouveau (MIT), Linux master 22 Nov 2026:
//   H1 nvkm/subdev/bios/bit.c 27-49, base.c 189 (BIT)       H2 bios/pll.c 80-97, 184-205, 367-380
//   H3 nvkm/subdev/clk/pllgt215.c 29-87 (gt215_pll_calc)    H4 nvkm/subdev/devinit/tu102.c 29-67
//   H5 bios/dcb.c 27-60, 109-195 (DCB, hasht/hashm)         H6 bios/dp.c 28-232 (dpout, dpcfg)
//   H7 nvkm/engine/disp/dp.c 91-366 (sense/drive/pattern/cr/eq/link)
//   H8 disp/gm200.c 33-55 (drive), 98-113 (route); g94.c 101-118 (lane power);
//      gm107.c 31-53 (pattern); nv50.c 202-228 (SOR power); tu102.c 58-83 (links)
//   H9 disp/nv50.c 1137-1362 (supervisor 1/2/3, 2_2_dp math); gv100.c 52-70 (SF regs),
//      835-891 (super ack), 936-947 (CTRL_DISP); gf119.c 286-301, 416-420
//   H10 dispnv50/headc57d.c, headc37d.c, sorc37d.c, corec37d.c 52-79, disp.c 2101-2149
// VBIOS scripts (BeforeLinkTraining, lnkcmp, AfterLinkTraining, DisableLT,
// OnInt2/OffInt2 for DCB[4]/IEDT[11]) are hard-coded from the board's VBIOS
// 90.04.0b.00.86 (disassembled offline, rtx2080-step4-design.md 7.2) and
// guarded by the DP table entry match (type 0x0006, mask 0x0f44).
#include "nv_disp_plan.h"

namespace brook::nvdisp {

// ------------------------------------------------------------------ VBIOS
struct Vbios{
    const uint8_t* d=nullptr;uint32_t n=0;
    uint8_t  r8(uint32_t o)const{return o<n?d[o]:0;}
    uint16_t r16(uint32_t o)const{return uint16_t(r8(o)|r8(o+1)<<8);}
    uint32_t r32(uint32_t o)const{return uint32_t(r16(o))|uint32_t(r16(o+2))<<16;}
    uint32_t Bit()const{ // "\xff\xb8BIT" (H1 base.c 189)
        for(uint32_t i=0;i+5<=n;++i)if(d[i]==0xff&&d[i+1]==0xb8&&d[i+2]=='B'&&d[i+3]=='I'&&d[i+4]=='T')return i;
        return 0;}
    bool BitEntry(uint8_t id,uint8_t& ver,uint16_t& len,uint16_t& off)const{ // H1 27-49
        const uint32_t b=Bit();if(!b)return false;
        uint8_t entries=r8(b+10);uint32_t e=b+12;
        while(entries--){if(r8(e)==id){ver=r8(e+1);len=r16(e+2);off=r16(e+4);return true;}e+=r8(b+9);}
        return false;}
};
struct PllLimits{uint32_t refclk,vcoMin,vcoMax,inMin,inMax;uint8_t mMin,mMax,nMin,nMax,pMin,pMax;};
constexpr uint8_t kPllVpll0=0x80; // + head (bios/pll.h PLL_VPLL0)
// BIT 'C' v2 -> limits table v0x50, matched by type (H2).
inline bool ParsePll(const Vbios& v,uint8_t type,PllLimits& out){
    uint8_t ver;uint16_t len,off;if(!v.BitEntry('C',ver,len,off)||ver!=2||len<4)return false;
    const uint32_t t=v.r32(off);if(!t||v.r8(t)!=0x50)return false;
    const uint8_t hdr=v.r8(t+1),elen=v.r8(t+2),cnt=v.r8(t+3);
    for(uint32_t i=0,e=t+hdr;i<cnt;++i,e+=elen){
        if(v.r8(e)!=type)continue;
        out={uint32_t(v.r16(e+1))*1000,uint32_t(v.r16(e+5))*1000,uint32_t(v.r16(e+7))*1000,
             uint32_t(v.r16(e+9))*1000,uint32_t(v.r16(e+11))*1000,
             v.r8(e+13),v.r8(e+14),v.r8(e+15),v.r8(e+16),v.r8(e+17),v.r8(e+18)};
        return out.refclk!=0;
    }
    return false;
}
// DCB 4.x output entry (H5): or, link, heads, hasht/hashm, CCB (i2c) index.
struct DcbOutp{uint8_t type,orMask,link,heads,location,connector,ccb;uint16_t hasht,hashm;uint8_t linkBw,linkNr;};
inline bool ParseDcb(const Vbios& v,uint32_t idx,DcbOutp& o){
    const uint32_t dcb=v.r16(0x36);if(!dcb||v.r8(dcb)<0x40||v.r8(dcb)>=0x42||v.r32(dcb+6)!=0x4edcbdcb)return false;
    const uint8_t hdr=v.r8(dcb+1),cnt=v.r8(dcb+2),len=v.r8(dcb+3);if(idx>=cnt)return false;
    const uint32_t e=dcb+hdr+idx*len,conn=v.r32(e),conf=v.r32(e+4);
    o.orMask=uint8_t((conn>>24)&0xf);o.location=uint8_t((conn>>20)&3);o.connector=uint8_t((conn>>12)&0xf);
    o.heads=uint8_t((conn>>8)&0xf);o.ccb=uint8_t((conn>>4)&0xf);o.type=uint8_t(conn&0xf);o.link=0;o.linkBw=o.linkNr=0;
    if(o.type==6){ // DP
        static const uint8_t bw[]={0x06,0x0a,0x14,0x1e};o.linkBw=bw[((conf>>21)&7)>3?3:(conf>>21)&7];
        const uint32_t nr=(conf>>24)&0xf;o.linkNr=uint8_t(nr==0xf||nr==4?4:nr==3||nr==2?2:1);
    }
    if(o.type==6||o.type==2||o.type==3)o.link=uint8_t((conf>>4)&3);
    o.hasht=uint16_t((o.location<<4)|o.type);  // extdev 0 for on-chip
    o.hashm=uint16_t((o.heads<<8)|(o.link<<6)|o.orMask);
    return true;
}
// DP info table (BIT 'd' v1 -> v0x40..0x42) output entry and drive config (H6).
struct DpOut{uint32_t entry;uint8_t ver,hdr;uint16_t type,mask;uint16_t beforeLt,afterLt,lnkcmp,enSpread,disSpread,disableLt;uint8_t cfgIdx;};
struct DpCfg{uint8_t dc,pe,pu,pc;};
inline uint32_t DpTable(const Vbios& v,uint8_t& ver,uint8_t& hdr,uint8_t& len,uint8_t& cnt){
    uint8_t bver;uint16_t blen,off;if(!v.BitEntry('d',bver,blen,off)||bver!=1||blen<2)return 0;
    const uint32_t t=v.r16(off);if(!t)return 0;ver=v.r8(t);
    if(ver<0x40||ver>0x42)return 0;
    hdr=v.r8(t+1);len=v.r8(t+2);cnt=v.r8(t+3);return t;
}
inline bool FindDpOut(const Vbios& v,uint16_t hasht,uint16_t hashm,DpOut& o){
    uint8_t ver,hdr,len,cnt;const uint32_t t=DpTable(v,ver,hdr,len,cnt);if(!t)return false;
    for(uint32_t i=0;i<cnt;++i){
        const uint32_t e=v.r16(t+hdr+i*len);if(!e)continue;
        o={e,ver,v.r8(t+4),v.r16(e),v.r16(e+2),v.r16(e+5),v.r16(e+7),v.r16(e+9),v.r16(e+0x0b),v.r16(e+0x0d),v.r16(e+0x0f),v.r8(e+0x11)};
        if(o.type==hasht&&(o.mask&hashm)==hashm)return true;
    }
    return false;
}
// nvbios_dpcfg_match (H6 206-232): idx = pc*10 + vsoff[vs] + pe (+ dpout[0x11]*10 on v0x42).
inline bool DpDrive(const Vbios& v,const DpOut& o,uint8_t pc,uint8_t vs,uint8_t pe,DpCfg& c){
    static const uint8_t vsoff[]={0,4,7,9};
    uint8_t ver,hdr,len,cnt;const uint32_t t=DpTable(v,ver,hdr,len,cnt);if(!t)return false;
    uint32_t idx=pc*10u+vsoff[vs&3]+(pe&3);
    if(ver>=0x42)idx+=o.cfgIdx*10u;else if(o.hdr>=0x12)idx+=o.cfgIdx*40u;
    const uint32_t base=t+hdr+uint32_t(len)*cnt,clen=v.r8(t+6),ccnt=uint32_t(v.r8(t+7))*v.r8(t+5);
    if(idx>=ccnt)return false;
    const uint32_t e=base+idx*clen;
    if(ver==0x42)c={v.r8(e),v.r8(e+1),v.r8(e+2),0};else c={v.r8(e+1),v.r8(e+2),v.r8(e+3),v.r8(e)};
    return true;
}

// -------------------------------------------------------------- clocking
struct PllCoef{uint32_t N,fN,M,P,khz;};
// gt215_pll_calc with the fractional path (H3 29-87): returns false if no M fits.
inline bool Gt215PllCalc(const PllLimits& l,uint32_t khz,PllCoef& c){
    if(!khz||!l.refclk||!l.inMin||!l.inMax)return false;
    uint32_t P=l.vcoMax/khz;if(P>l.pMax)P=l.pMax;if(P<l.pMin)P=l.pMin;
    uint32_t lM=(l.refclk+l.inMax)/l.inMax;if(lM<l.mMin)lM=l.mMin;
    uint32_t hM=(l.refclk+l.inMin)/l.inMin;if(hM>l.mMax)hM=l.mMax;if(lM>hM)lM=hM;
    for(uint32_t M=lM;M<=hM;++M){
        const uint32_t tmp=khz*P*M;uint32_t N=tmp/l.refclk;
        if(tmp%l.refclk<l.refclk/2)--N;
        const uint32_t fN=tmp-N*l.refclk;
        if(N<l.nMin)continue;
        if(N>l.nMax)break;
        c={N,((((fN<<13)+l.refclk/2)/l.refclk)-4096)&0xffff,M,P,khz};
        return true;
    }
    return false;
}
// tu102_devinit_pll_set register writes (H4 52-58), in nouveau's order.
constexpr uint32_t RegVpll(uint32_t head,uint32_t o){return 0x00ef00+head*0x40+o;}
inline void AddVpll(Plan& p,uint32_t head,const PllCoef& c){
    p.Add(Op::Write,RegVpll(head,0x10),~0u,c.fN<<16,0,"VPLL fN<<16 (devinit/tu102.c 52)");
    p.Add(Op::Write,RegVpll(head,0x04),~0u,(c.P<<16)|(c.N<<8)|c.M,0,"VPLL P<<16|N<<8|M (53-55)");
    p.Add(Op::Write,RegVpll(head,0x0c),~0u,0x00000900,0,"VPLL magic 0x900 (57)");
    p.Add(Op::Write,RegVpll(head,0x00),~0u,0x02000014,0,"VPLL magic enable 0x02000014 (58)");
}

// ---------------------------------------------------------------- the mode
struct HeadMode{ // core HEAD_SET_* values (raw method words, as in the head-0 ARM readback)
    uint32_t outputResource=0xfc000040; // 0x2004: depth 24_444, ext packet none, polarities
    uint32_t pclkHz=533250000;          // 0x200c and _MAX 0x2028
    uint32_t rasterSize=0x08ae0fa0, syncEnd=0x0004001f, blankEnd=0x003a006f, blankStart=0x08aa0f6f;
    uint32_t vertBlank2=0x00000001;     // raw 0x2074 (C37D VERT_BLANK2, nouveau headc57d mode)
    uint32_t viewport=0x08700f00;       // SIZE_IN / SIZE_OUT
    uint32_t outputScaler=0x11, maxOutputScale=0x04000400, usageBounds=0x1110, stallLock=0x1000, dither=0;
    uint32_t Htotal()const{return rasterSize&0xffff;}  uint32_t Vtotal()const{return rasterSize>>16;}
    uint32_t Hblanke()const{return blankEnd&0xffff;}   uint32_t Vblanke()const{return blankEnd>>16;}
    uint32_t Hblanks()const{return blankStart&0xffff;} uint32_t Vblanks()const{return blankStart>>16;}
    uint32_t Depth()const{switch((outputResource>>4)&0xf){case 5:return 30;case 4:return 24;default:return 18;}}
};
// nv50_disp_super_2_2_dp (H9 nv50.c 1156-1260) without .activesym (TU102): TU = 64.
struct DpSf{uint32_t h,v,watermark;};
inline bool DpSymbols(const HeadMode& m,uint32_t bw,uint32_t nr,bool ef,DpSf& out){
    const uint64_t khz=m.pclkHz/1000,linkKBps=uint64_t(bw)*27000,symbol=100000;
    if(!khz||!nr)return false;
    uint64_t h=uint64_t(m.Hblanke()+m.Htotal()-m.Hblanks()-7)*linkKBps/khz;
    h=h-3*(ef?1:0)-12/nr;
    uint64_t v=uint64_t(m.Vblanks()-m.Vblanke()-25)*linkKBps/khz;
    v=v-((36/nr)+3)-1;
    const uint64_t linkData=(khz*m.Depth()/8)/nr;
    const uint64_t ratio=linkData*symbol/linkKBps;
    if(ratio>=symbol)return false; // mode does not fit the link
    uint64_t unk=(symbol-ratio)*64;unk*=ratio;unk/=symbol;unk/=symbol;unk+=6;
    out={uint32_t(h),uint32_t(v),uint32_t(unk)};
    return true;
}

// ------------------------------------------------------- DP link training
// DPCD addresses used (DP 1.2).
constexpr uint32_t kDpcdRev=0x000,kDpcdMaxLanes=0x002,kDpcdRdInterval=0x00e,kDpcdLinkBw=0x100,kDpcdLaneCount=0x101,
    kDpcdTpSet=0x102,kDpcdLaneSet=0x103,kDpcdPc2Set=0x10f,kDpcdLaneStatus=0x202,kDpcdAdjust=0x206,kDpcdPc2Adjust=0x20c,
    kDpcdPower=0x600;
struct DpLink{
    uint32_t sor=2, link=1;    // link 1 = A, 2 = B (nouveau asy.link)
    uint32_t bw=0x14, nr=4; bool ef=true;
    uint8_t dpcd[16]{};        // receiver caps 0x000-0x00f, read before training
    uint32_t Soff()const{return sor*0x800;}
    uint32_t Loff()const{return sor*0x800+(link==2?0x80:0);}
    bool Tps3()const{return dpcd[0]>=0x12&&(dpcd[2]&0x40);}
};
struct DpTrainResult{bool cr=false,eq=false;uint8_t stat[6]{},conf[4]{},pc2conf[2]{};uint32_t crTries=0,eqTries=0,drives=0;int err=0;};
// Io for training: rd/wr (BAR0), aux(read,addr,buf,n) -> 0 ok, udelay(us),
// and drive(lane,pc,vs,pe,DpCfg&) -> VBIOS table lookup (false = no entry).
template<typename Io> struct DpTrainer{
    Io& io;const DpLink& l;DpTrainResult& r;uint8_t pc2stat=0;
    void Mask(uint32_t a,uint32_t m,uint32_t v){io.wr(a,(io.rd(a)&~m)|v);}
    // gm107_sor_dp_pattern (soff-based: 0x110 link A, 0x12c link B) + DPCD 0x102 (H7 202-226)
    void Pattern(uint8_t p){
        static const uint32_t d[]={0x10101010,0x01010101,0x02020202,0x03030303,0x1b1b1b1b};
        Mask(((l.link&1)?0x61c110:0x61c12c)+l.Soff(),0x1f1f1f1f,d[p]);
        uint8_t tp=0;io.aux(true,kDpcdTpSet,&tp,1);
        tp=uint8_t((tp&~0x0f)|(p!=4?p:7));
        if(p)tp|=0x20;else tp&=uint8_t(~0x20);
        io.aux(false,kDpcdTpSet,&tp,1);
    }
    // nvkm_dp_train_drive (H7 131-200) + gm200_sor_dp_drive (H8 33-55)
    int Drive(bool pc){
        r.pc2conf[0]=r.pc2conf[1]=0;++r.drives;
        for(uint32_t i=0;i<l.nr;++i){
            const uint8_t lane=uint8_t((r.stat[4+(i>>1)]>>((i&1)*4))&0xf);
            // Post-cursor2 forced to 0 (review 23 Nov): drive strictly from the VBIOS pc=0 table.
            uint8_t lpc2=0,lpre=uint8_t((lane&0x0c)>>2),lvsw=uint8_t(lane&3),hivs=uint8_t(3-lpre);
            (void)pc2stat;
            if(lpre>=3){lpre=3|0x4;lvsw=hivs=uint8_t(3-(lpre&3));}
            else if(lvsw>=hivs)lvsw=hivs|0x4;
            r.conf[i]=uint8_t((lpre<<3)|lvsw);
            r.pc2conf[i>>1]|=uint8_t(lpc2<<((i&1)*4));
            DpCfg c{};if(!io.drive(i,uint8_t(lpc2&3),uint8_t(lvsw&3),uint8_t(lpre&3),c)){r.err=-7;return -1;} // no table entry: fail
            const uint32_t sh=i*8,lo=l.Loff();
            const uint32_t pu=c.pu&0xf;
            uint32_t d0=io.rd(0x61c118+lo)&~(0xffu<<sh),d1=io.rd(0x61c120+lo)&~(0xffu<<sh),d2=io.rd(0x61c130+lo);
            if((d2&0xf00)<(pu<<8)||i==0)d2=(d2&~0xf00u)|(pu<<8);
            io.wr(0x61c118+lo,d0|(uint32_t(c.dc)<<sh));io.wr(0x61c120+lo,d1|(uint32_t(c.pe)<<sh));io.wr(0x61c130+lo,d2);
            const uint32_t d3=io.rd(0x61c13c+lo)&~(0xffu<<sh);io.wr(0x61c13c+lo,d3|(uint32_t(c.pc)<<sh));
        }
        if(io.aux(false,kDpcdLaneSet,r.conf,4))return -1;
        if(pc&&io.aux(false,kDpcdPc2Set,r.pc2conf,2))return -1;
        return 0;
    }
    int Sense(bool pc,uint32_t us){ // H7 91-129
        io.udelay(us);
        if(io.aux(true,kDpcdLaneStatus,r.stat,3))return -1;
        if(io.aux(true,kDpcdAdjust,r.stat+4,2))return -1;
        if(pc&&io.aux(true,kDpcdPc2Adjust,&pc2stat,1))pc2stat=0;
        return 0;
    }
    bool LanesHave(uint8_t bits)const{
        for(uint32_t i=0;i<l.nr;++i)if((((r.stat[i>>1]>>((i&1)*4))&0xf)&bits)!=bits)return false;return true;}
    bool Cr(){ // nvkm_dp_train_cr (H7 275-310)
        uint8_t voltage=r.conf[0]&3;uint32_t tries=0;bool done=false,abort=false;
        Pattern(1);
        const uint32_t us=l.dpcd[0]<0x14?(l.dpcd[0x0e]&0x7f)*4000u:0;
        do{
            if(Drive(false)||Sense(false,us?us:100)){r.err=-5;break;}
            done=true;
            for(uint32_t i=0;i<l.nr;++i)if(!(((r.stat[i>>1]>>((i&1)*4))&0xf)&1)){done=false;if(r.conf[i]&0x4)abort=true;break;}
            if((r.conf[0]&3)!=voltage){voltage=r.conf[0]&3;tries=0;}
            ++r.crTries;
        }while(!done&&!abort&&++tries<5);
        return done;
    }
    bool Eq(){ // nvkm_dp_train_eq (H7 228-273); TPS4 needs DPCD 1.4, not this sink
        const bool pc=l.Tps3();bool eq=false,cr=true;uint32_t tries=0;
        Pattern(l.Tps3()?3:2);
        const uint32_t us=(l.dpcd[0x0e]&0x7f)*4000u;
        do{
            if((tries&&Drive(pc))||Sense(pc,us?us:400)){r.err=-5;break;}
            eq=(r.stat[2]&1)!=0;
            for(uint32_t i=0;i<l.nr&&eq;++i){const uint8_t ln=uint8_t((r.stat[i>>1]>>((i&1)*4))&0xf);
                if(!(ln&1))cr=false;
                if(!(ln&2)||!(ln&4))eq=false;}
            ++r.eqTries;
        }while(!eq&&cr&&++tries<=5);
        return eq;
    }
    bool Run(){ // nvkm_dp_train_link (H7 312-366), sink only (no LTTPRs on this path)
        // The plan programs EF and skips the ASSR branch: refuse sinks that differ.
        if(!(l.dpcd[2]&0x80)){r.err=-8;return false;}            // DPCD 0x002 bit7 ENHANCED_FRAME_CAP
        if(l.dpcd[0x0d]&1){r.err=-9;return false;}               // DPCD 0x00d bit0 ASSR (eDP) not handled
        uint8_t d0=1;if(io.aux(false,kDpcdPower,&d0,1)){r.err=-6;return false;}
        io.udelay(1000);
        // Spread: mirror only the sink's MSA downspread bit (DPCD 0x107 bit4) to the
        // source state the GOP left in 0x00ea80 bit4; never write 0xea80.
        {uint8_t ds=0;if(io.aux(true,0x107,&ds,1)){r.err=-6;return false;}
         const bool on=(io.rd(0x00ea80)>>4)&1&&(l.dpcd[3]&1);ds=uint8_t((ds&~0x10)|(on?0x10:0));
         if(io.aux(false,0x107,&ds,1)){r.err=-6;return false;}}
        uint8_t sink[2]={uint8_t(l.bw),uint8_t(l.nr|(l.ef?0x80:0))};
        if(io.aux(false,kDpcdLinkBw,sink,2)){r.err=-6;return false;}
        r.cr=Cr();if(r.cr)r.eq=Eq();
        Pattern(0);
        return r.cr&&r.eq;
    }
};
template<typename Io> inline bool DpTrain(Io& io,const DpLink& l,DpTrainResult& r){
    r=DpTrainResult{};DpTrainer<Io> t{io,l,r};return t.Run();
}

// ------------------------------------------------------- SOR/DP op lists
// nv50_sor_power(normal, pu) (H8 nv50.c 202-228)
inline void AddSorPower(Plan& p,uint32_t sor){
    const uint32_t s=sor*0x800;
    p.Add(Op::Poll,0x61c004+s,0x80000000,0,kTimeoutMs,"SOR power sequencer idle");
    p.Add(Op::Mask,0x61c004+s,0x80000001,0x80000001,0,"SOR power: normal PU");
    p.Add(Op::Poll,0x61c004+s,0x80000000,0,kTimeoutMs,"SOR power done");
    p.Add(Op::Poll,0x61c030+s,0x10000000,0,kTimeoutMs,"SOR sequencer not busy");
}
// gm200_sor_route_set (H8 98-113): macro = __ffs(DCB or-mask), link A -> 0x612308+moff.
inline uint32_t RegRoute(uint32_t orMask,uint32_t link){
    const uint32_t moff=uint32_t(__builtin_ctz(orMask))*0x100;return (link&1?0x612308:0x612388)+moff;}
inline uint32_t RouteValue(uint32_t sor){return (0u<<4)|(sor+1);} // link-A sublink 0
inline void AddRoute(Plan& p,const DcbOutp& o,uint32_t value,const char* what){
    p.Add(Op::Mask,RegRoute(o.orMask,o.link),0x1f,value&0x1f,0,what);
}
// VBIOS dpout[4] BeforeLinkTraining 0x611f (subs 0x5afd PHY, 0x42e4 pad cal, 0x5b65 lanes).
inline void AddBeforeLt(Plan& p,const DpLink& l){
    const uint32_t s=l.Soff(),lo=l.Loff();
    p.Add(Op::Mask,0x61c200+s,0x2000,0x2000,0,"BeforeLT PHY: 0x61c200 |= 0x2000");
    p.Add(Op::Mask,0x61c1b0+s,0x00800000,0,0,"BeforeLT PHY");
    p.Add(Op::Mask,0x612300+s,0x03000000,0x03000000,0,"BeforeLT PHY: SOR clk [25:24] = 3");
    p.Add(Op::Mask,0x61c138+s,0x00000ff3,0x00000442,0,"BeforeLT PHY");
    p.Add(Op::Write,0x61c00c+s,~0u,0x05050310,0,"BeforeLT PHY");
    p.Add(Op::Write,0x61c010+s,~0u,0x0000012f,0,"BeforeLT PHY");
    p.Add(Op::Write,0x61c1b8+s,~0u,0x38000500,0,"BeforeLT PHY");
    p.Add(Op::Write,0x61c014+s,~0u,0x00020000,0,"BeforeLT PHY");
    p.Add(Op::Delay,0,0,50,0,"50 us");
    p.Add(Op::Write,0x61c138+s,~0u,0x38000440,0,"BeforeLT PHY");
    p.Add(Op::Delay,0,0,1000,0,"1 ms");
    // pad calibration (0x42e4): fixed macro-2 registers, successive approximation on [11:8]
    p.Add(Op::Mask,0x612588,0x20,0,0,"pad cal: 0x612588 &= ~0x20");
    p.Add(Op::Mask,0x612508,0xf00,0x800,0,"pad cal: [11:8] = 8");
    for(uint32_t bit=0x800;bit>=0x100;bit>>=1){
        if(bit!=0x800)p.Add(Op::Mask,0x612508,bit,bit,0,"pad cal: set next bit");
        p.Add(Op::Delay,0,0,100,0,"100 us");
        p.Add(Op::AndNotIf,0x612508,bit,0,0,"pad cal: clear bit if comparator (bit7) high",0,0x80);
    }
    p.Add(Op::CopyMask,0x612508,0xf00,0x612588,0,"pad cal: copy [11:8] to 0x612588 if (0x625488 & 0x40000)==0",0,0xfffff0ff,0x625488,0x40000);
    p.Add(Op::Mask,0x61c10c+lo,0x4000,0x4000,0,"lanes: 0x61c10c |= 0x4000");
    p.Add(Op::Mask,0x61c130+s,0x004000ff,0x004000ff,0,"lanes");
    p.Add(Op::Mask,0x61c1b0+s,0x00c000ff,0x00c000ff,0,"lanes");
    p.Add(Op::Mask,0x61c034+s,0x80110000,0x80000000,0,"lanes: sequencer go");
    p.Add(Op::Poll,0x61c034+s,0x80000000,0,kTimeoutMs,"lanes: sequencer done");
    p.Add(Op::Delay,0,0,10,0,"10 us");
    p.Add(Op::Mask,0x61c130+s,0xf0,0,0,"lanes");
    p.Add(Op::Mask,0x61c1b0+s,0xf0,0,0,"lanes");
    p.Add(Op::Mask,0x61c110+s,0x1f1f1f1f,0,0,"patterns off");
    p.Add(Op::Mask,0x61c12c+s,0x1f1f1f1f,0,0,"patterns off");
    // ASSR branch (DPCD 0x00d bit0, eDP only) intentionally not taken: DP sink.
}
// lnkcmp (HBR2 0x617b) + tu102_sor_dp_links (H8 tu102.c 58-83) + g94_sor_dp_power (H8 g94.c 101-118)
inline void AddDpLinks(Plan& p,const DpLink& l){
    const uint32_t s=l.Soff(),lo=l.Loff();
    p.Add(Op::Mask,0x61c10c+lo,0x2,0x2,0,"lnkcmp: 0x61c10c |= 2");
    p.Add(Op::Mask,0x612300+s,0x00030000,0x00020000,0,"lnkcmp: SOR clk [17:16] = 2");
    p.Add(Op::Mask,0x61c130+lo,0x0000f000,0x00002000,0,"lnkcmp: [15:12] = 2");
    p.Add(Op::Mask,0x612300+s,0x007c0000,l.bw<<18,0,"links: SOR clk link bw (tu102.c 74)");
    p.Add(Op::Delay,0,0,40000,0,"40 ms (tu102.c 77)");
    p.Add(Op::Mask,0x612300+s,0x00030000,0x00010000,0,"links: [17:16] = 1");
    p.Add(Op::Mask,0x61c10c+lo,0x3,0x1,0,"links: dpctrl enable");
    p.Add(Op::Mask,0x61c10c+lo,0x401f4000,(((1u<<l.nr)-1)<<16)|(l.ef?0x4000u:0),0,"links: lanes + EF");
    p.Add(Op::Mask,0x61c130+lo,0xf,(1u<<l.nr)-1,0,"power: lane mask");
    p.Add(Op::Mask,0x61c034+s,0x80000000,0x80000000,0,"power: go");
    p.Add(Op::Poll,0x61c034+s,0x80000000,0,kTimeoutMs,"power: done");
}
// AfterLinkTraining 0x5bec (the DPCD 0x102 &= 0xd0 is done by the trainer's pattern 0)
inline void AddAfterLt(Plan& p,const DpLink& l){
    const uint32_t s=l.Soff();
    p.Add(Op::Mask,0x61c110+s,0x1f1f1f1f,0x10101010,0,"AfterLT patterns");
    p.Add(Op::Mask,0x61c12c+s,0x1f1f1f1f,0x10101010,0,"AfterLT patterns");
    p.Add(Op::Mask,0x61c1b8+s,0x80000000,0x80000000,0,"AfterLT 0x61c1b8 |= bit31");
}
// DisableLT 0x5c55
inline void AddDisableLt(Plan& p,const DpLink& l){
    const uint32_t s=l.Soff();
    p.Add(Op::Mask,0x61c130+s,0xf,0,0,"DisableLT lanes off");
    p.Add(Op::Mask,0x61c1b0+s,0xf,0,0,"DisableLT");
    p.Add(Op::Mask,0x61c034+s,0x80110000,0x80110000,0,"DisableLT sequencer");
    p.Add(Op::PollSoft,0x61c034+s,0x80000000,0,kTimeoutMs,"DisableLT sequencer done");
    p.Add(Op::Mask,0x61c10c+s,0x1,0,0,"DisableLT dpctrl link A off");
    p.Add(Op::Mask,0x61c18c+s,0x1,0,0,"DisableLT dpctrl link B off");
    p.Add(Op::Mask,0x612300+s,0x00030000,0x00020000,0,"DisableLT SOR clk");
    p.Add(Op::Mask,0x612300+s,0x037f0000,0x00280000,0,"DisableLT SOR clk bw 0x0a");
    p.Add(Op::Mask,0x61c014+s,0x01c00000,0x01c00000,0,"DisableLT");
    p.Add(Op::Mask,0x61c00c+s,0x1,0x1,0,"DisableLT");
}

// ---------------------------------------------------------- core methods
namespace c57d {
constexpr uint32_t HeadM(uint32_t h,uint32_t m){return 0x2000+h*0x400+m;}
constexpr uint32_t SOR_PROTOCOL_DP_A=8, SOR_PROTOCOL_DP_B=9;
}
// Head enable, the nouveau head flush (headc57d/headc37d view/mode/dither/procamp/or).
inline void PushHeadEnable(Push& p,uint32_t h,const HeadMode& m){
    using namespace c57d;
    p.Mthd(HeadM(h,0x000),0u,m.outputResource,0u,m.pclkHz);          // PROCAMP, OUTPUT_RESOURCE, CONTROL, PCLK
    p.Mthd(HeadM(h,0x014),m.outputScaler,m.dither);                   // OUTPUT_SCALER, DITHER
    p.Mthd(HeadM(h,0x028),m.pclkHz,m.maxOutputScale,m.usageBounds,m.stallLock); // PCLK_MAX, MAX_OUT_SCALE, USAGE, STALL
    p.Mthd(HeadM(h,0x048),0u,m.viewport);                            // VIEWPORT_POINT_IN, SIZE_IN
    p.Mthd(HeadM(h,0x058),m.viewport);                               // VIEWPORT_SIZE_OUT
    p.Mthd(HeadM(h,0x064),m.rasterSize,m.syncEnd,m.blankEnd,m.blankStart,m.vertBlank2);
}
// Core state a core UPDATE promotes and that connecting the channel may reset
// (the window connect did, live 23 Nov). The replay segment re-pushes every
// documented core method with its ARM value before any UPDATE: global, 4 SORs,
// 8 windows, 4 heads. Left out: UPDATE, the notifier pair (ours), the GET ports
// (GET_RG_SCAN_LINE 0x220-0x23c, SET_GET_BLANKING_CTRL 0x240-0x25c) and the head
// data port OUTPUT_SCALER_COEFF_VALUE (+0x214). Head context-DMA methods (CRC
// +0x180, OLUT +0x288) are replayed only as 0 (preflight requires ARM 0).
constexpr uint16_t kCoreHeadMthds[]={ // clc57d.h HEAD_SET_* offsets for head 0
    0x000,0x004,0x008,0x00c,0x010,0x014,0x018,0x01c,0x028,0x02c,0x030,0x034,0x040,0x044,0x048,0x04c,0x058,0x05c,
    0x064,0x068,0x06c,0x070,0x078,0x07c,0x080,0x098,0x09c,0x0a0,0x180,0x184,0x18c,0x194,0x198,0x19c,0x1a0,0x1a8,
    0x218,0x220,0x224,0x228,0x22c,0x238,0x23c,0x240,0x244,0x248,0x24c,0x250,0x254,0x258,0x25c,0x260,0x264,0x268,
    0x26c,0x270,0x280,0x284,0x288,0x28c,0x29c,0x2a0,0x2a4,0x2a8,0x2ac,0x2b0,0x2b4,0x2b8,0x2bc,0x2c0,0x2c4,0x2c8,
    0x2cc,0x2d0,0x2d4,0x2d8,0x2dc,0x2e0,0x2e4,0x2e8,0x2ec,0x2f0,0x2f4,0x2f8,0x2fc,0x300,0x304,0x308,0x30c,0x310,
    0x314,0x318,0x31c,0x320,0x324,0x328,0x32c,0x330,0x334,0x338,0x33c,0x340,0x344,0x348,0x34c,0x350,0x354,0x358,
    0x35c,0x360};
constexpr uint32_t kCoreHeadCount=sizeof(kCoreHeadMthds)/sizeof(kCoreHeadMthds[0]);
constexpr uint32_t kCoreReplayCount=3+4*4+8*5+4*kCoreHeadCount;
inline uint32_t CoreReplayMthd(uint32_t i){
    static constexpr uint16_t glob[]={0x210,0x218,0x21c};
    if(i<3)return glob[i];i-=3;
    if(i<16)return 0x300+(i/4)*0x20+(i%4)*4;i-=16;
    if(i<40)return 0x1000+(i/5)*0x80+(i%5)*4;i-=40;
    return 0x2000+(i/kCoreHeadCount)*0x400+kCoreHeadMthds[i%kCoreHeadCount];
}
inline bool CoreHeadCtxDma(uint32_t m){const uint32_t o=(m-0x2000)%0x400;return m>=0x2000&&m<0x3000&&(o==0x180||o==0x288);}

struct HeadConfig{
    Config base{};                 // step-3 layout (instmem, core pushbuffer, sync) and GOP state; coreChannel forced on
    uint32_t head=1, sor=2, window=2;
    DcbOutp dcb{};                 // DCB[4]: or-mask 4, link A, heads 0xf, CCB 7, hasht 0x0006, hashm 0x0f44
    DpLink dp{};
    HeadMode mode{};
    PllCoef vpll{};
    PllLimits vpll0{};             // VBIOS limits of VPLL0, for the VPLL0 frequency gate
    DpSf sf{};
    uint32_t windowFmtUsage=0xf, windowUsage=0x00117fff; // corec57d_init values
    uint32_t routeRestore=0x03;    // 0x612508[4:0] the VBIOS left (probe 058a8cd8: 0x603 -> SOR2, already routed)
    int32_t watermarkOverride=0x15;// GOP's head-0 DP watermark for the identical mode/link (0x616550 = 0x08000015);
                                   // nouveau's formula gives 18 ("close to vbios numbers, but not right", nv50.c 1252). -1 = formula
    bool pauseAfterModeset=true;
    uint32_t coreReplayVal[kCoreReplayCount]{}; // ARM values of CoreReplayMthd(i), read when the plan is built
};
template<typename Rd> inline void ReadCoreReplay(HeadConfig& c,Rd rd){
    for(uint32_t i=0;i<kCoreReplayCount;++i){const uint32_t m=CoreReplayMthd(i);c.coreReplayVal[i]=rd(RegArm(kChidCore,m));}
}
// Core ring segments: [replay] [rung: notifier-only UPDATE] [enable] [disable].
struct HeadPushes{Push core;uint32_t replayEnd=0,rungEnd=0,enableEnd=0,disableEnd=0;};
inline uint32_t SorCtl(const HeadConfig& c){
    return (1u<<c.head)|((c.dp.link==2?c57d::SOR_PROTOCOL_DP_B:c57d::SOR_PROTOCOL_DP_A)<<8);}
inline void BuildHeadPushes(const HeadConfig& c,HeadPushes& out){
    Push& p=out.core;
    p.Mthd(c57d::SET_CONTEXT_DMA_NOTIFIER,kHandleSync);                // ours (corec57d_init 40)
    for(uint32_t i=0;i<kCoreReplayCount;){                            // replay, consecutive methods share a header
        uint32_t j=i+1;while(j<kCoreReplayCount&&CoreReplayMthd(j)==CoreReplayMthd(j-1)+4)++j;
        p.Put(PushHeader(CoreReplayMthd(i),j-i));
        for(uint32_t k=i;k<j;++k)p.Put(CoreHeadCtxDma(CoreReplayMthd(k))?0u:c.coreReplayVal[k]);
        i=j;
    }
    out.replayEnd=p.count*4;
    PushCoreUpdate(p,0,0,true);                                       // rung: nothing changes, notifier only
    out.rungEnd=p.count*4;
    p.Mthd(c57d::SorSetControl(c.sor),SorCtl(c));                    // sorc37d_ctrl
    PushHeadEnable(p,c.head,c.mode);
    p.Mthd(c57d::WindowSetControl(c.window),c.head);                 // window -> head
    p.Mthd(c57d::WindowFormatUsage(c.window),c.windowFmtUsage,0u);
    p.Mthd(c57d::WindowUsage(c.window),c.windowUsage);
    PushCoreUpdate(p,0,0,true);                                       // corec37d_update, notifier at 0
    out.enableEnd=p.count*4;
    p.Mthd(c57d::SorSetControl(c.sor),0u);                           // detach OR
    p.Mthd(c57d::WindowSetControl(c.window),c57d::OWNER_NONE);
    PushCoreUpdate(p,0,0,true);
    out.disableEnd=p.count*4;
}
// Head-0 / SOR1 / window-0 core methods may appear only with their ARM value (the replay).
inline bool Head0MethodRange(uint32_t m){return (m>=0x2000&&m<0x2400)||(m>=0x320&&m<0x340)||(m>=0x1000&&m<0x1080);}
inline bool PushHead0MatchesArm(const HeadConfig& c,const Push& p){
    for(uint32_t i=0;i<p.count;){const uint32_t hdr=p.words[i++];if(hdr>>29)continue;
        const uint32_t n=(hdr>>18)&0x3ff,m=hdr&0x3ffc;
        for(uint32_t k=0;k<n&&i<p.count;++k,++i){const uint32_t mm=m+k*4;if(!Head0MethodRange(mm))continue;
            bool found=false;
            for(uint32_t r=0;r<kCoreReplayCount;++r)if(CoreReplayMthd(r)==mm){
                found=true;if(p.words[i]!=(CoreHeadCtxDma(mm)?0u:c.coreReplayVal[r]))return false;}
            if(!found)return false;}
    }
    return true;
}

// ----------------------------------------------------------- supervisor
constexpr uint32_t kRegSuperAck=0x611860, kRegSuperStat=0x6107a8, kSuperRelease=0x80000000;
constexpr uint32_t RegSuperHead(uint32_t h){return 0x6107ac+h*4;}
enum CallId:uint32_t{kCallDpTrain=1,kCallSuperFlush=2,kCallSuperFlushVpll=3};
// Stage-2 head-1 work (2.1 VPLL with readback, 2.2 OnInt2/rgclk/DP SF/SOR clock).
inline void AddStage2Work(Plan& p,const HeadConfig& c){
    const uint32_t h=c.head,hoff=h*0x800,s=c.sor*0x800,lo=c.dp.Loff();
    p.Add(Op::Expect,RegAssy(kChidCore,c57d::HeadM(h,0x00c)),~0u,c.mode.pclkHz,0,"2.1: ASSY head pclk as planned");
    AddVpll(p,h,c.vpll);
    p.Add(Op::Expect,RegVpll(h,0x04),~0u,(c.vpll.P<<16)|(c.vpll.N<<8)|c.vpll.M,0,"2.1: VPLL coefficients read back");
    p.Add(Op::Expect,RegVpll(h,0x10),~0u,c.vpll.fN<<16,0,"2.1: VPLL fN read back");
    p.Add(Op::CopyMask,0x61c10c+lo,0x40ff4001,0x616540+hoff,0,"2.2 OnInt2: head SF <- dpctrl (IEDT[11])",0,0xbf00bffe);
    p.Add(Op::Mask,0x612200+hoff,0xf,0,0,"2.2 rgclk div 0 (gf119_head_rgclk)");
    p.Add(Op::Mask,0x616568+hoff,0xffff,c.sf.h,0,"2.2 DP audio sym h (gv100.c 66)");
    p.Add(Op::Mask,0x61656c+hoff,0xffffff,c.sf.v,0,"2.2 DP audio sym v (gv100.c 67)");
    p.Add(Op::Mask,0x616550+hoff,0x0c00003f,0x08000000|c.sf.watermark,0,"2.2 DP watermark (gv100.c 58)");
    p.Add(Op::Mask,0x612300+s,0x707,0,0,"2.2 SOR clock div (gf119_sor_clock, DP link A)");
}
// One stage (gv100_disp_super 835-891, intr_ctrl_disp 942-947): pending in CTRL_DISP
// 0x611c30 or the event status 0x611860; a core exception (0x611854 bit0) fails the
// wait at once. Head 0 flagged is a sticky warning: the stage still finishes.
template<typename F> inline void AddSuper(Plan& p,uint32_t stage,const char* name,F work){
    const uint32_t bit=1u<<(stage-1);
    p.Add(Op::PollOr,kRegCtrlDisp,bit,bit,kTimeoutMs,name,0,kRegSuperAck,kRegExcOther,1);
    p.Add(Op::Write,kRegSuperAck,~0u,bit,0,"ack supervisor (0x611860)");
    p.Add(Op::Log,kRegSuperStat,0,0,0,"supervisor status 0x6107a8");
    p.Add(Op::Log,RegSuperHead(1),0,0,0,"head 1 supervisor mask");
    p.Add(Op::ExpectSoft,RegSuperHead(0),0x00011000,0,0,"HEAD 0 FLAGGED by supervisor (sticky warning; stage still completes)");
    work();
    for(uint32_t h=0;h<4;++h)p.Add(Op::Write,RegSuperHead(h),~0u,0,0,"clear head mask");
    p.Add(Op::Write,kRegSuperStat,~0u,kSuperRelease,0,"release supervisor");
}
inline void AddSupervisorsEnable(Plan& p,const HeadConfig& c){
    AddSuper(p,1,"supervisor 1",[&]{ /* 1.0 OffInt1: nothing ARMed on head 1 */ });
    AddSuper(p,2,"supervisor 2",[&]{AddStage2Work(p,c);});
    AddSuper(p,3,"supervisor 3",[&]{ /* 3.0 OnInt3: empty for IEDT[11] */ });
}
inline void AddSupervisorsDisable(Plan& p,const HeadConfig& c){
    const uint32_t hoff=c.head*0x800;
    AddSuper(p,1,"supervisor 1 (disable)",[&]{});
    AddSuper(p,2,"supervisor 2 (disable)",[&]{
        p.Add(Op::Mask,0x616540+hoff,0x1,0,0,"2.0 OffInt2: head SF enable off (IEDT[11])");});
    AddSuper(p,3,"supervisor 3 (disable)",[&]{});
}
// Abort flush (dynamic; called through Op::Call): up to 3 rounds, each waits for
// any stage pending in 0x611c30 | 0x611860, then acks and releases only the stages
// seen. Stage 2 during an enable gets the head-1 stage-2 work (VPLL1 included).
// Io: rd, wr, udelay, log(const char*,uint32_t).
template<typename Io> inline uint32_t SupervisorFlush(Io& io,const HeadConfig& c,bool stage2Work,uint32_t ms){
    uint32_t handled=0;
    for(uint32_t round=0;round<3;++round){
        uint32_t bits=0;
        for(uint32_t t=0;t<ms*10;++t){bits=(io.rd(kRegCtrlDisp)|io.rd(kRegSuperAck))&7;if(bits)break;io.udelay(100);}
        if(!bits){io.note("supervisor flush: nothing pending; 0x6107a8",io.rd(kRegSuperStat));break;}
        for(uint32_t bit=1;bit<=4;bit<<=1){
            if(!(bits&bit))continue;
            io.wr(kRegSuperAck,bit);io.note("supervisor flush: acked stage",bit);
            if(io.rd(RegSuperHead(0))&0x11000)io.note("supervisor flush: HEAD 0 FLAGGED",io.rd(RegSuperHead(0)));
            if(bit==2&&stage2Work){Plan w;AddStage2Work(w,c);(void)Run(w,0,w.count,io,true);}
            for(uint32_t h=0;h<4;++h)io.wr(RegSuperHead(h),0);
            io.wr(kRegSuperStat,kSuperRelease);++handled;
        }
    }
    return handled;
}

// ----------------------------------------------------------------- plan
enum class HPhase:uint32_t{Preflight,Vram,Instmem,Core,CoreCheck,Sor,DpPre,DpTrain,DpPost,Modeset,Verify,Disable,DpOff,CoreOff,Count};
inline const char* HPhaseName(HPhase p){
    static const char* n[]={"preflight","vram","instmem","core","core-check","sor","dp-pre","dp-train","dp-post","modeset","verify","disable","dp-off","core-off"};
    return n[uint32_t(p)];}
// Every core method a core UPDATE promotes: global, SORs, windows, heads.
// strict=1: fail on any known method that differs (except ours); unknown ones are logged.
inline void AddCoreDiff(Plan& p,const char* when,uint32_t strict=0){
    p.Add(Op::AssyArmDiff,kChidCore,0x200,0x400,0,when,0,strict);
    p.Add(Op::AssyArmDiff,kChidCore,0x1000,0x1400,0,when,0,strict);
    p.Add(Op::AssyArmDiff,kChidCore,0x2000,0x3000,0,when,0,strict);
}
inline void AddCoreKick(Plan& p,const HeadConfig& c,uint32_t put,const char* what){
    p.Add(Op::VramWrite,0,~0u,kNtfyNotBegun,0,"core notifier NOT_BEGUN",c.base.syncVram+0);
    p.Add(Op::Kick,RegPut(kChidCore),~0u,put,0,what);
}
// Allow-list of MMIO the head plan may write (review 23 Nov issue 11).
struct Range{uint32_t lo,hi;};
constexpr Range kHeadAllow[]={
    {0x001700,0x001704},{0x070000,0x070004},{0x610010,0x610018},{0x610078,0x61007c},
    {0x6104e0,0x6104e4},{0x610b20,0x610b30},{0x680000,0x680004},                  // instmem, FE, core channel
    {0x611020,0x611024},{0x611854,0x611858},{0x611860,0x611864},{0x6107a8,0x6107bc}, // core exception, supervisors
    {0x00ef40,0x00ef60},{0x612a00,0x612a04},{0x616d40,0x616d44},{0x616d50,0x616d54},{0x616d68,0x616d70}, // head 1
    {0x612508,0x61250c},{0x612588,0x61258c},{0x613300,0x613304},{0x61d000,0x61d800}, // SOR2, macro 2
    {0x00da70,0x00dac0},                                                           // AUX ch 4 (0xd930 + 4*0x50)
};
inline bool HeadAllowed(uint32_t a){for(const auto& r:kHeadAllow)if(a>=r.lo&&a<r.hi)return true;return false;}

inline Err BuildHeadPlan(HeadConfig c,InstImage& img,HeadPushes& pushes,Plan& p){
    c.base.coreChannel=true;
    if(c.head==0||c.head>3||c.sor>3||c.window>7||c.dp.nr<1||c.dp.nr>4)return Err::Bounds;
    if(Err e=Validate(c.base);e!=Err::Ok)return e;
    if(Err e=BuildInstImage(c.base,img);e!=Err::Ok)return e;
    BuildHeadPushes(c,pushes);if(pushes.core.overflow)return Err::PushFull;
    if(!PushHead0MatchesArm(c,pushes.core))return Err::Usage;
    p.MarkRewritten(pushes.core.words,pushes.enableEnd/4);
    for(uint32_t i=0;i<kCoreReplayCount;++i)Plan::Set(p.known,CoreReplayMthd(i));
    Plan::Set(p.known,0x208);Plan::Set(p.ours,0x208);Plan::Set(p.known,0x20c);Plan::Set(p.ours,0x20c);
    const auto& g=c.base.gop;const uint32_t h=c.head;
    auto B=[&](HPhase x){p.BeginN(uint32_t(x));};
    B(HPhase::Preflight);
    p.Add(Op::Expect,kRegDispOwner,0x2,0,0,"VBIOS ownership bit clear");
    p.Add(Op::Expect,kRegInstTarget,~0u,g.instTarget,0,"no display instmem (GOP)");
    p.Add(Op::Expect,RegChnCtl(kChidCore),0xffff,g.ctlCore,0,"core CHNCTL as left by GOP");
    p.Add(Op::Expect,RegChnStatus(kChidCore),kCoreStateMask,kCoreEfiOperation,0,"core STATE EFI_OPERATION (7)");
    p.Add(Op::Expect,kRegCtrlDisp,0x187,0,0,"no supervisor/error/awaken pending");
    p.Add(Op::Expect,kRegSuperAck,0x7,0,0,"no supervisor event pending (0x611860)");
    // RM_INTR_STAT latches with EN=0 when MSK routes the bit (survey: 0x611c00 = 7 with
    // 0x611d80 = 0), so polling CTRL_DISP needs the supervisor bits in its MSK (GOP: 0x1ff).
    p.Add(Op::Expect,0x611cf0,0x7,0x7,0,"CTRL_DISP MSK routes supervisors 1-3 (0x611c30 latches)");
    p.Add(Op::Log,0x611db0,0,0,0,"CTRL_DISP EN (expected 0: polled, no interrupt)");
    p.Add(Op::Expect,kRegExcOther,1,0,0,"no core exception latched");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::SorSetControl(c.sor)),0xff,0,0,"SOR2 owns no head");
    for(uint32_t s=0;s<4;++s)p.Add(Op::Expect,RegArm(kChidCore,c57d::SorSetControl(s)),1u<<h,0,0,"no SOR drives head 1");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::SorSetControl(g.sor)),~0u,g.sorCtl,0,"SOR1 still DP-B on head 0");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::WindowSetControl(c.window)),0xf,c57d::OWNER_NONE,0,"window owned by no head");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(0)),~0u,g.headRaster,0,"head 0 raster (GOP)");
    for(uint32_t hh=0;hh<4;++hh){
        p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadM(hh,0x180)),~0u,0,0,"head CRC ctxdma 0 (replayed as 0)");
        p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadM(hh,0x288)),~0u,0,0,"head OLUT ctxdma 0 (replayed as 0)");}
    if(c.vpll0.refclk)p.Add(Op::ExpectVpll,RegVpll(0,0),1,c.mode.pclkHz/1000,0,"VPLL0 runs head 0's pclk (our PLL encoding)",0,c.vpll0.refclk);
    p.Add(Op::Log,RegRoute(c.dcb.orMask,c.dcb.link),0,0,0,"route register before");
    {const uint32_t regs[]={RegVpll(h,0),RegVpll(h,4),RegVpll(h,0x10),0x610078u,0x625488u,0x61d004u,0x612a00u,0x612200u,0x612b00u,0x613300u,0x00ea80u};
     for(uint32_t r:regs)p.Add(Op::Log,r,0,0,0,"baseline");}
    AddCoreDiff(p,"core ASSY == ARM (heads, SORs, windows), or replayed");
    AddExceptionCheck(p,0);
    B(HPhase::Vram);
    p.Add(Op::VramCopy,uint32_t(Blob::Inst),kInstBytes,0,0,"instmem image",c.base.instVram);
    p.Add(Op::VramCopy,uint32_t(Blob::SyncInit),0x1000,0,0,"sync page zeroed",c.base.syncVram);
    p.Add(Op::VramCopy,uint32_t(Blob::CorePush),kPushBytes,0,0,"core pushbuffer (replay, rung, enable, disable)",c.base.corePushVram);
    p.Add(Op::Fence,0,0,0,0,"sfence");
    p.Add(Op::Write,kRegBar1Flush,~0u,1,0,"BAR1 write flush");
    p.Add(Op::Poll,kRegBar1Flush,0x2,0,kTimeoutMs,"flush done");
    if(c.base.praminCheck){
        p.Add(Op::Write,kRegPraminBase,~0u,uint32_t(c.base.instVram>>16),0,"PRAMIN -> instmem");
        for(uint32_t s=0;s<kHashEntries;++s)if(img.words[s*2+1])
            p.Add(Op::PraminRange,s*8,uint32_t(Blob::Inst),2,0,"RAMHT entry",0,s*8);
        p.Add(Op::PraminRange,ObjOffset(0),uint32_t(Blob::Inst),6,0,"sync ctxdma",0,ObjOffset(0));
        p.Add(Op::PraminRange,uint32_t(c.base.corePushVram-c.base.instVram),uint32_t(Blob::CorePush),pushes.core.count,0,"core push words",0,0);
        p.Add(Op::Write,kRegPraminBase,~0u,c.base.praminRestore,0,"PRAMIN restored");
    }
    B(HPhase::Instmem);
    p.Add(Op::Write,kRegInstAddr,~0u,uint32_t(c.base.instVram>>16),0,"instmem addr first");
    p.Add(Op::Write,kRegInstTarget,~0u,0x9,0,"instmem valid, VRAM");
    if(c.base.feEnable)p.Add(Op::Mask,kRegFeEnable,1,1,0,"fallback rung: 0x610078 |= 1 (tu102.c 240)");
    B(HPhase::Core);
    AddChannelInit(p,kChidCore,c.base.corePushVram);
    B(HPhase::CoreCheck);
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(0)),~0u,g.headRaster,0,"head 0 ARM survived connect");
    AddCoreDiff(p,"after core connect (log only; reset methods are replayed next)",2);
    p.Add(Op::Kick,RegPut(kChidCore),~0u,pushes.replayEnd,0,"core PUT: replay ARM state (methods only, no UPDATE)");
    p.Add(Op::PollOr,RegGet(kChidCore),~0xfffff003u|0xffc,pushes.replayEnd,kTimeoutMs,"core GET == PUT (replay fetched)",0,RegGet(kChidCore),kRegExcOther,1);
    AddCoreDiff(p,"after replay: ASSY == ARM (strict)",1);
    AddCoreKick(p,c,pushes.rungEnd,"core PUT: rung, notifier-only UPDATE");
    p.Add(Op::PollOr,RegGet(kChidCore),~0u,pushes.rungEnd,kTimeoutMs,"core GET == PUT (rung)",0,RegGet(kChidCore),kRegExcOther,1);
    p.Add(Op::VramPoll,0,3u<<kNtfyStatusShift,kNtfyFinished<<kNtfyStatusShift,kTimeoutMs,"rung notifier FINISHED",c.base.syncVram+0);
    p.Add(Op::Expect,kRegCtrlDisp,0x7,0,0,"rung raised no supervisor");
    p.Add(Op::Expect,kRegSuperAck,0x7,0,0,"rung raised no supervisor event");
    p.Add(Op::Expect,kRegExcOther,1,0,0,"rung raised no core exception");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(0)),~0u,g.headRaster,0,"head 0 raster unchanged by the rung");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadPixelClock(0)),~0u,g.headPclk,0,"head 0 pclk unchanged by the rung");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::SorSetControl(g.sor)),~0u,g.sorCtl,0,"SOR1 unchanged by the rung");
    B(HPhase::Sor);
    AddSorPower(p,c.sor);
    AddRoute(p,c.dcb,RouteValue(c.sor),"route macro -> SOR (gm200_sor_route_set)");
    B(HPhase::DpPre);
    AddBeforeLt(p,c.dp);
    AddDpLinks(p,c.dp);
    B(HPhase::DpTrain);
    p.Add(Op::Call,kCallDpTrain,0,0,0,"DP link training on SOR2-A (CR TPS1, EQ TPS3, VBIOS drive table)");
    B(HPhase::DpPost);
    AddAfterLt(p,c.dp);
    p.Add(Op::Expect,0x61c10c+c.dp.Loff(),0x001f4001,(((1u<<c.dp.nr)-1)<<16)|(c.dp.ef?0x4000u:0)|1,0,"dpctrl: lanes, EF, enabled");
    B(HPhase::Modeset);
    AddCoreDiff(p,"right before the modeset kick: ASSY == ARM (strict)",1);
    AddCoreKick(p,c,pushes.enableEnd,"core PUT: head 1 + SOR2 + window owner + UPDATE");
    AddSupervisorsEnable(p,c);
    p.Add(Op::PollOr,RegGet(kChidCore),~0u,pushes.enableEnd,kTimeoutMs,"core GET == PUT",0,RegGet(kChidCore),kRegExcOther,1);
    p.Add(Op::VramPoll,0,3u<<kNtfyStatusShift,kNtfyFinished<<kNtfyStatusShift,kTimeoutMs,"core notifier FINISHED",c.base.syncVram+0);
    p.Add(Op::Expect,kRegExcOther,1,0,0,"no core exception");
    {const uint32_t regs[]={0x612a00u,0x613300u,0x612200u,0x612b00u};
     for(uint32_t r:regs)p.Add(Op::Log,r,0,0,0,"after modeset (head-0 0x612200 / 0x612b00 must equal baseline)");}
    B(HPhase::Verify);
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(0)),~0u,g.headRaster,0,"head 0 raster unchanged");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadPixelClock(0)),~0u,g.headPclk,0,"head 0 pclk unchanged");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::SorSetControl(g.sor)),~0u,g.sorCtl,0,"SOR1 unchanged");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::SorSetControl(c.sor)),~0u,SorCtl(c),0,"SOR2 ARM = head 1, DP-A");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(h)),~0u,c.mode.rasterSize,0,"head 1 raster ARMed");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::WindowSetControl(c.window)),0xf,h,0,"window owned by head 1");
    p.Add(Op::Log,0x616330+h*0x800,0,0,0,"head 1 RG position (moving = scanning)");
    if(c.pauseAfterModeset)p.Add(Op::Pause,0,0,0,0,"observer: monitor on conn2 shows a (black) 4K60 signal, conn0 unchanged?");
    B(HPhase::Disable);
    AddCoreKick(p,c,pushes.disableEnd,"core PUT: detach SOR2, window owner none, UPDATE");
    AddSupervisorsDisable(p,c);
    p.Add(Op::PollOr,RegGet(kChidCore),~0u,pushes.disableEnd,kTimeoutMs,"core GET == PUT",0,RegGet(kChidCore),kRegExcOther,1);
    p.Add(Op::VramPoll,0,3u<<kNtfyStatusShift,kNtfyFinished<<kNtfyStatusShift,kTimeoutMs,"core notifier FINISHED",c.base.syncVram+0);
    B(HPhase::DpOff);
    AddDisableLt(p,c.dp);
    AddRoute(p,c.dcb,c.routeRestore,"route restored to the VBIOS value");
    B(HPhase::CoreOff);
    AddChannelFini(p,kChidCore,kTimeoutMs);
    AddPbRestore(p,kChidCore,g);
    if(c.base.feEnable)p.Add(Op::Write,kRegFeEnable,~0u,g.feEnable,0,"0x610078 = GOP value");
    p.Add(Op::Write,kRegInstTarget,~0u,g.instTarget,0,"instmem invalid");
    p.Add(Op::Write,kRegInstAddr,~0u,g.instAddr,0,"instmem addr 0");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(0)),~0u,g.headRaster,0,"head 0 still the GOP mode");
    p.EndN(uint32_t(HPhase::Count));
    return p.overflow?Err::TooManyOps:Err::Ok;
}
// Which core UPDATEs had been kicked when a run stopped (review issue 2).
enum class Kicked:uint8_t{None,Replay,Rung,Enable,Disable};
// Abort (best effort): log and ack a core exception; flush supervisors only if an
// UPDATE was kicked (stage-2 work with VPLL1 when it was the enable); detach only
// if the enable was kicked; then DisableLT, route restore, core fini, instmem off.
inline void BuildHeadAbort(const HeadConfig& c,HPhase reached,const HeadPushes& pushes,Plan& p,uint32_t ms=100,Kicked kicked=Kicked::Enable){
    p.BeginN(0);
    p.Add(Op::AckException,kChidCore,1,0,0,"log 0x611020/24/28, W1C 0x611854, ack 0x90000000 (gv100.c 992-994, 932)",0,kRegExcOther);
    const bool update=kicked==Kicked::Rung||kicked==Kicked::Enable||kicked==Kicked::Disable;
    if(update)p.Add(Op::Call,kicked==Kicked::Enable?kCallSuperFlushVpll:kCallSuperFlush,0,0,ms,"supervisor flush (stages seen only)");
    if(kicked==Kicked::Enable){
        p.Add(Op::Kick,RegPut(kChidCore),~0u,pushes.disableEnd,0,"core PUT: disable push");
        p.Add(Op::Call,kCallSuperFlush,0,0,ms,"supervisor flush after detach");}
    if(reached>=HPhase::DpPre){AddDisableLt(p,c.dp);}
    if(reached>=HPhase::Sor)AddRoute(p,c.dcb,c.routeRestore,"route restored to the VBIOS value");
    if(reached>=HPhase::Core){AddChannelFini(p,kChidCore,ms,false);AddPbRestore(p,kChidCore,c.base.gop);}
    if(reached>=HPhase::Instmem){
        if(c.base.feEnable)p.Add(Op::Write,kRegFeEnable,~0u,c.base.gop.feEnable,0,"0x610078 = GOP value");
        p.Add(Op::Write,kRegInstTarget,~0u,c.base.gop.instTarget,0,"instmem invalid");
        p.Add(Op::Write,kRegInstAddr,~0u,c.base.gop.instAddr,0,"instmem addr 0");}
    if(reached>=HPhase::Vram&&c.base.praminCheck)p.Add(Op::Write,kRegPraminBase,~0u,c.base.praminRestore,0,"PRAMIN restored");
    {const uint32_t regs[]={RegArm(kChidCore,c57d::HeadRasterSize(0)),RegChnStatus(kChidCore),kRegCtrlDisp,kRegSuperAck,kRegSuperStat,kRegExcOther};
     for(uint32_t r:regs)p.Add(Op::Log,r,0,0,0,"final state");}
    p.EndN(1);
}
// FNV-1a over the VBIOS image the scripts were disassembled from (128 KiB PROM read).
inline uint32_t VbiosHash(const Vbios& v){uint32_t h=2166136261u;for(uint32_t i=0;i<v.n;++i){h^=v.d[i];h*=16777619u;}return h;}
// HBR2 lnkcmp script (dp.c 400-416, v0x30+): skip while bw < entry rate.
inline uint32_t LnkcmpScript(const Vbios& v,uint32_t lnkcmp,uint32_t bw){
    if(!lnkcmp)return 0;for(uint32_t n=0;n<16&&bw<v.r8(lnkcmp);++n)lnkcmp+=3;return v.r16(lnkcmp+1);}
// Fill dcb/dp/vpll/sf from a VBIOS and the planned mode. False if any table is
// missing or the hard-coded scripts do not match this VBIOS (review issue 7).
// expectedHash: FNV-1a of the image (0 = skip, synthetic test images).
inline bool ResolveHeadConfig(const Vbios& v,uint32_t dcbIndex,HeadConfig& c,DpOut& dpout,uint32_t expectedHash=0){
    if(expectedHash&&VbiosHash(v)!=expectedHash)return false;
    if(!ParseDcb(v,dcbIndex,c.dcb)||c.dcb.type!=6||!(c.dcb.orMask&(1u<<c.sor)))return false;
    if(!FindDpOut(v,c.dcb.hasht,c.dcb.hashm,dpout))return false;
    if(dpout.beforeLt!=0x611f||dpout.afterLt!=0x5bec||dpout.disableLt!=0x5c55)return false;
    if(LnkcmpScript(v,dpout.lnkcmp,c.dp.bw)!=0x617b)return false;
    PllLimits lim{};if(!ParsePll(v,uint8_t(kPllVpll0+c.head),lim))return false;
    if(!ParsePll(v,kPllVpll0,c.vpll0))return false;
    if(!Gt215PllCalc(lim,c.mode.pclkHz/1000,c.vpll))return false;
    c.dp.sor=c.sor;c.dp.link=c.dcb.link==2?2:1;
    if(!DpSymbols(c.mode,c.dp.bw,c.dp.nr,c.dp.ef,c.sf))return false;
    if(c.watermarkOverride>=0)c.sf.watermark=uint32_t(c.watermarkOverride)&0x3f;
    return true;
}
} // namespace brook::nvdisp
