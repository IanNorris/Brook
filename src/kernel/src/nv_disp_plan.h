#pragma once
// RTX 2080 (TU104) display engine: dry-run bring-up plan for page flips on
// the head the firmware GOP already lit (step 3). Pure and allocation-free:
// the same op lists drive the host tests (with a fake register file) and,
// later, the gated live path, so the reviewed sequence is the executed one.
//
// Clean-room reimplementation from these MIT-licensed / public sources
// (line numbers are Linux master of 22 Nov 2026, NVIDIA open-gpu-doc and
// open-gpu-kernel-modules main of the same day):
//   N1 nvkm/engine/disp/gv100.c   dmac_idle 340, dmac_bind 353, dmac_fini 361,
//                                 dmac_init 373, wndw chids 537, core idle 722,
//                                 core fini 750, core init 760, exception 894,
//                                 intr 1078, disp_init 1125
//   N2 nvkm/engine/disp/tu102.c   disp_init 181 (instmem 243-251), ramht 0x2000 294
//   N3 nvkm/engine/disp/nv50.c    dmac_push 684 (target|addr>>8), oneinit 1637-1642
//   N4 nvkm/core/ramht.c          hash 27, update 60 (entry words 93-94), insert 107
//   N5 nvkm/engine/dma/usergv100.c bind 38 (ctxdma words), flags 104-116
//   N6 dispnv50/disp.c            dmac kick 133 (0x070000 flush 142-148), wind 168,
//                                 dmac max 263, sync/vram ctxdmas 291-311,
//                                 commit_core 2101, window owner 2309
//   N7 dispnv50/wndwc37e.c        image_clr 141, ntfy_set 224, update 270
//   N8 dispnv50/wndwc57e.c        image_set 33
//   N9 dispnv50/corec37d.c        wndw_owner 34, update 52, ntfy_init 94
//   N10 dispnv50/corec57d.c       init 31
//   N11 dispnv50/wndw.c           flush_set 150, ntfy_enable 187, ctxdma_new 58
//   N12 dispnv50/disp.h           NV50_DISP_SYNC/NTFY offsets 17-21
//   N13 include/nvif/pushc37b.h   method header; include/nvhw/class/clc37b.h 28-39
//   R1 open-gpu-kernel-modules src/common/inc/swref/published/disp/v03_00/dev_disp.h
//      (CHNCTL/CHNSTATUS fields, hash table layout NV_UDISP_HASH_*, NV_DMA_*)
//   R2 open-gpu-kernel-modules .../disp/inst_mem/arch/v03/disp_inst_mem_0300.c
//      (hash 105-121, entry 137-139, ctxdma commit 143-277)
//   R3 open-gpu-kernel-modules src/nvidia-modeset/src/nvkms-dma.c 37-66
//      (PUT is a byte offset; sfence before PUT; JUMP word 0x20000000)
//   R4 open-gpu-doc classes/display/clc57d.h, clc57e.h, clc37b.h (methods)
#include <stdint.h>
#include <stddef.h>

namespace brook::nvdisp {

// ---------------------------------------------------------------- registers
constexpr uint32_t kRegBar1Flush   =0x070000; // write 1, poll bit 1 clear (N6 142-148)
constexpr uint32_t kRegPraminBase  =0x001700; // BAR0 0x700000 window base, addr>>16, 1 MiB
constexpr uint32_t kPramin         =0x700000;
constexpr uint32_t kRegDispOwner   =0x6254e8; // bit1 = VBIOS owns display (N2 189)
constexpr uint32_t kRegInstTarget  =0x610010; // 0x8 valid | target (1 VRAM)
constexpr uint32_t kRegInstAddr    =0x610014; // inst >> 16
constexpr uint32_t kRegFeEnable    =0x610078; // nouveau |= 1 after the caps copies (N2 240)
constexpr uint32_t kRegIntrDispatch=0x611ec0; // bits 0-7 head timing, 9 EXC_WIN, 10 EXC_WINIM, 11 EXC_OTHER, 12 CTRL_DISP
constexpr uint32_t kRegExcWin      =0x61184c; // per window, W1C (N1 1037-1049)
constexpr uint32_t kRegExcWinim    =0x611850;
constexpr uint32_t kRegExcOther    =0x611854; // bit0 core, 16+n cursor n
constexpr uint32_t kRegCtrlDisp    =0x611c30; // supervisor 0-2, error 7, awaken 8
constexpr uint32_t kRegWinIntrEn   =0x611da4; // EXC_WIN enables (left 0: poll mode)
constexpr uint32_t RegHeadTiming(uint32_t h){return 0x611800+h*4;} // bit2 VBLANK, W1C
constexpr uint32_t RegExcept(uint32_t chid){return 0x611020+chid*12;} // stat,+4 data,+8 code
constexpr uint32_t kExceptAck      =0x90000000; // N1 932

constexpr uint32_t kChidCore=0;
constexpr uint32_t ChidWndw(uint32_t w){return 1+w;}
constexpr uint32_t ChidWimm(uint32_t w){return 33+w;}
constexpr uint32_t RegChnCtl(uint32_t chid){return 0x6104e0+chid*4;}
constexpr uint32_t RegChnStatus(uint32_t chid){return chid==0?0x610630:0x610664+(chid-1)*4;}
constexpr uint32_t RegPbHi(uint32_t chid){return 0x610b20+chid*0x10;}
constexpr uint32_t RegPbLo(uint32_t chid){return 0x610b24+chid*0x10;}
constexpr uint32_t RegPbValid(uint32_t chid){return 0x610b28+chid*0x10;}  // nouveau writes 1
constexpr uint32_t RegPbClient(uint32_t chid){return 0x610b2c+chid*0x10;} // nouveau writes 0x40
constexpr uint32_t RegUser(uint32_t chid){return chid==0?0x680000:0x690000+(chid-1)*0x1000;}
constexpr uint32_t RegPut(uint32_t chid){return RegUser(chid)+0;}
constexpr uint32_t RegGet(uint32_t chid){return RegUser(chid)+4;}
// Method state shadows: ASSY = pending (USER base + method), ARM = active
// (core +0x8000, others +0x800) (N1 .prev 511/711; R1 NV_UDISP_FE_CHN_*_BASEADR).
constexpr uint32_t RegAssy(uint32_t chid,uint32_t m){return RegUser(chid)+m;}
constexpr uint32_t RegArm(uint32_t chid,uint32_t m){return RegUser(chid)+(chid==0?0x8000:0x800)+m;}

// CHNCTL bits (R1) and channel states.
constexpr uint32_t kCtlAllocate=0x1, kCtlConnect=0x2, kCtlPutptrWrite=0x10, kCtlTrashAbort=0x4000;
constexpr uint32_t kCtlRunning=0x13; // N1 388/772
constexpr uint32_t kCoreStateMask=0x1f0000, kCoreIdle=0x0b0000, kCoreEfiOperation=0x070000;
constexpr uint32_t kWinStateMask =0x0f0000, kWinIdle =0x040000, kWinUnconnected=0x010000;
constexpr uint32_t kWinMethodExecBusy=0x80000000;

// ---------------------------------------------------------------- methods
namespace c57e { // window channel (R4 clc57e.h; N8)
constexpr uint32_t UPDATE=0x200, SET_CONTEXT_DMA_NOTIFIER=0x21c, SET_NOTIFIER_CONTROL=0x220,
    SET_SIZE=0x224, SET_STORAGE=0x228, SET_PARAMS=0x22c, SET_PLANAR_STORAGE0=0x230,
    SET_CONTEXT_DMA_ISO0=0x240, SET_OFFSET0=0x260, SET_POINT_IN0=0x290, SET_SIZE_IN=0x298,
    SET_SIZE_OUT=0x2a4, SET_PRESENT_CONTROL=0x308, SET_INTERLOCK_FLAGS=0x370,
    SET_WINDOW_INTERLOCK_FLAGS=0x374;
constexpr uint32_t FORMAT_A8R8G8B8=0xcf, FORMAT_X8R8G8B8=0xe6, LAYOUT_PITCH=1;
}
namespace c57d { // core channel (R4 clc57d.h; N9/N10)
constexpr uint32_t UPDATE=0x200, SET_CONTEXT_DMA_NOTIFIER=0x208, SET_NOTIFIER_CONTROL=0x20c,
    SET_INTERLOCK_FLAGS=0x218, SET_WINDOW_INTERLOCK_FLAGS=0x21c;
constexpr uint32_t WindowSetControl(uint32_t w){return 0x1000+w*0x80;}
constexpr uint32_t WindowFormatUsage(uint32_t w){return 0x1004+w*0x80;}
constexpr uint32_t WindowRotatedUsage(uint32_t w){return 0x1008+w*0x80;}
constexpr uint32_t WindowMaxInputScale(uint32_t w){return 0x100c+w*0x80;}
constexpr uint32_t WindowUsage(uint32_t w){return 0x1010+w*0x80;}
constexpr uint32_t SorSetControl(uint32_t s){return 0x300+s*0x20;}
constexpr uint32_t HeadRasterSize(uint32_t h){return 0x2064+h*0x400;}
constexpr uint32_t HeadPixelClock(uint32_t h){return 0x200c+h*0x400;}
constexpr uint32_t OWNER_NONE=0xf;
}
// Push buffer words (N13): METHOD opcode 0, count 27:18, offset 13:2; JUMP opcode 1.
constexpr uint32_t PushHeader(uint32_t mthd,uint32_t count){return (count<<18)|(mthd&0x3ffc);}
constexpr uint32_t PushJump(uint32_t byteOffset){return 0x20000000|(byteOffset&0xffc);}
constexpr uint32_t kPushBytes=0x1000, kPushMaxWords=0x1000/4-1; // N6 263 (GV100+: no -12)

// Notifier (clc37d.h NV_DISP_NOTIFIER, 16 bytes): word0 status 31:30.
constexpr uint32_t kNtfyStatusShift=30, kNtfyNotBegun=0, kNtfyBegun=1, kNtfyFinished=2;
// nouveau's sync page layout (N12): core ntfy 0x00, window w ntfy (1+w)*0x40+0x20 (^0x10 per flip).
constexpr uint32_t WindowNtfyOffset(uint32_t w,uint32_t parity){return (1+w)*0x40+0x20+(parity&1)*0x10;}

// ---------------------------------------------------------------- instmem
// 64 KiB, 64 KiB aligned; hash table at 0 (0x2000 = 1024 x 8 B), objects
// from 0x2000 at 32-byte granules (N3 1637-1642, R1 NV_UDISP_HASH/OBJ_MEM).
constexpr uint32_t kInstBytes=0x10000, kHashBytes=0x2000, kHashEntries=kHashBytes/8, kHashBits=10;
constexpr uint32_t kObjBase=0x2000, kCtxDmaBytes=0x20; // 24 B used, 32 B granule
constexpr uint32_t kClientId=0x40; // HASH_TBL_CLIENT_ID; = 0x610b2c value (N1 384, 357)
// Handles nouveau uses (dispnv50/handles.h) plus the GOP's own ISO handle.
constexpr uint32_t kHandleSync=0xf0000000, kHandleVram=0xf0000001, kHandleIsoPitch=0xfb000000,
                   kHandleGop=0x45564144; // "DAVE", window 0 ISO handle left by the GOP

// nouveau's hash (N4 27-38): xor-fold the handle in 10-bit chunks, ^ chid<<6.
constexpr uint32_t RamhtHash(uint32_t handle,uint32_t chid){
    uint32_t h=0;while(handle){h^=handle&(kHashEntries-1);handle>>=kHashBits;}
    return h^(chid<<(kHashBits-4));
}
// RM's documented hash with a client id (R2 105-121), for reference/mirroring.
constexpr uint32_t RamhtHashRm(uint32_t handle,uint32_t chid,uint32_t client){
    return ((handle>>0)&0x3ff)^((handle>>10)&0x3ff)^((handle>>20)&0x3ff)^
           (((client&0xff)<<2)|(handle>>30))^(((chid&0xf)<<6)|((client>>8)&0x3f))^((chid>>4)&0x7);
}
// Entry word 1: CHN 31:25 | INSTANCE 24:14 (byte offset >> 5) | CLIENT_ID 13:0.
// Identical to nouveau's (chid<<25)|0x40|(offset<<9) (N1 356-357, N4 88).
constexpr uint32_t RamhtContext(uint32_t chid,uint32_t objOffset){
    return (chid<<25)|((objOffset>>5)<<14)|kClientId;
}
// Ctxdma flags (N5 104-116; R1 NV_DMA_*): target 1:0 (1 VRAM), access bit2 rw,
// bit6 small-page (nouveau sets it for the untyped sync ctxdma), kind bit20 BL.
constexpr uint32_t kDmaVram=0x1, kDmaRw=0x4, kDmaPageSp=0x40, kDmaKindBl=0x100000;
constexpr uint32_t kFlagsIsoPitch=kDmaVram|kDmaRw;            // N11 106-108 (page LP, kind 0)
constexpr uint32_t kFlagsSync=kDmaVram|kDmaRw|kDmaPageSp;     // N6 291 via N5 98-101

struct CtxDma{uint32_t handle,flags;uint64_t base,limit;};
struct RamhtUse{uint32_t chid,handle,objIndex;};

// --------------------------------------------------------------- the plan
struct Surface{uint64_t vram;uint32_t width,height,pitch,format;};
// Window state that connecting the channel may reset in ASSY (live 23 Nov: SET_PARAMS
// read 0xe9 = A1R5G5B5 right after connect, GOP ARM 0xcf). Submission 1 replays every
// documented C57E method it does not otherwise set, with the value read from ARM when
// the plan is built, so the first UPDATE re-arms exactly the GOP's state. Data ports
// (INPUT_SCALER_COEFF_VALUE 0x2ac, EXT_PACKET_DATA 0x39c, CSC0LUT_ENTRY 0x4a8,
// CSC1LUT_ENTRY 0x588) are excluded; 0x350-0x36c are not in clc57e.h (software
// methods, never pushed; skipped by the diff).
constexpr uint16_t kReplayMthds[]={
    0x20c,0x210,0x214,0x218,                                    // semaphore control/acquire/release/ctxdma
    0x234,0x238,0x244,0x248,0x24c,0x250,0x254,                  // planes 1-2 pitch, ISO 1-5
    0x264,0x268,0x26c,0x270,0x274,0x294,                        // OFFSET 1-5, POINT_IN 1
    0x2a8,0x2ec,0x2f0,0x2f4,0x2f8,0x2fc,0x300,0x304,            // scaler, composition, keys
    0x340,0x344,0x348,0x34c,0x398,                              // timestamps, ext packet control
    0x400,0x404,0x408,0x40c,0x410,0x414,0x418,0x41c,0x420,0x424,0x428,0x42c, // FMT coefficients
    0x440,0x444,0x448,                                          // ILUT
    0x45c,0x460,0x464,0x468,0x46c,0x470,0x474,0x478,0x47c,0x480,0x484,0x488,0x48c, // CSC00
    0x4a0,0x4a4,                                                // CSC0LUT control/segment
    0x4bc,0x4c0,0x4c4,0x4c8,0x4cc,0x4d0,0x4d4,0x4d8,0x4dc,0x4e0,0x4e4,0x4e8,0x4ec, // CSC01
    0x500,0x508,0x50c,0x510,0x514,0x518,0x51c,0x528,0x52c,      // TMO
    0x53c,0x540,0x544,0x548,0x54c,0x550,0x554,0x558,0x55c,0x560,0x564,0x568,0x56c, // CSC10
    0x580,0x584,                                                // CSC1LUT control/segment
    0x59c,0x5a0,0x5a4,0x5a8,0x5ac,0x5b0,0x5b4,0x5b8,0x5bc,0x5c0,0x5c4,0x5c8,0x5cc, // CSC11
    0x5d0};                                                     // CLAMP_RANGE
constexpr uint32_t kReplayCount=sizeof(kReplayMthds)/sizeof(kReplayMthds[0]);
// Live state the GOP left (survey 4d60b483 / 555a7dbd); preflight checks these.
struct GopState{
    uint32_t instTarget=0x1, instAddr=0;
    uint32_t ctlCore=0x21, ctlWndw=0x01;
    uint32_t winSize=0x08700f00, winStorage=0x10, winParams=0xcf, winPlanar=0x100,
             winIso=kHandleGop, winOffset=0;
    uint32_t coreWinCtl=0, coreFmtUsage=0x3b7, coreRotUsage=0, coreMaxScale=0x04000400, coreUsage=0x110f00;
    uint32_t headRaster=0x08ae0fa0, headPclk=0x1fc8bfd0, sorCtl=0x901; uint32_t sor=1;
    uint32_t pbLo=0x1, pbHi=0, pbValid=0x1, pbClient=0; // 0x610b20.. per channel (555a7dbd)
    uint32_t feEnable=0;                                // 0x610078
};
struct Config{
    uint32_t window=0, head=0;
    Surface surf[2]{};          // [0] = the GOP surface (restore target), [1] = back buffer
    uint64_t instVram=0, corePushVram=0, wndwPushVram=0, syncVram=0;
    uint64_t isoLimit=0;        // last VRAM byte the ISO ctxdma may reach
    bool coreChannel=false;     // stage B: also connect the core channel (no core UPDATE)
    bool gopHandle=true;        // also bind "DAVE" -> surf[0] so restore re-arms the GOP handle
    bool mirrorRmHash=false;    // duplicate every RAMHT entry at the RM-hash slot too
    bool praminCheck=true;      // verify BAR1 == VRAM identity through PRAMIN before enabling
    uint32_t praminRestore=0x1fff0; // 0x1700 value the firmware left
    bool feEnable=false;        // stage A2: 0x610078 |= 1 like tu102_disp_init 240 (restored to 0)
    bool capsCopy=false;        // stage A2: tu102_disp_init caps copies into FE_SW 0x640000 (not restored)
    bool pauseForObserver=false;// first live noop: Pause between window disconnect and instmem off
    bool replay=true;           // submission 1 replays kReplayMthds with replayVal (ARM values)
    bool dumpAfterConnect=false;// log the whole window ASSY and ARM blocks right after connect
    uint32_t replayVal[kReplayCount]{};
    GopState gop{};
};

enum class Err:uint8_t{Ok,Align,Overlap,Bounds,Usage,Format,PushFull,HashFull,TooManyOps};

// ---- instance memory image
struct InstImage{
    uint32_t words[kInstBytes/4];
    CtxDma obj[4];uint32_t objCount;
    RamhtUse use[8];uint32_t useCount;
};
inline uint32_t ObjOffset(uint32_t i){return kObjBase+i*kCtxDmaBytes;}
inline void WriteCtxDma(uint32_t* w,uint32_t off,const CtxDma& d){
    uint32_t* o=w+off/4;const uint64_t b=d.base>>8,l=d.limit>>8;   // N5 50-54, R2 258-271
    o[0]=d.flags;o[1]=uint32_t(b);o[2]=uint32_t(b>>32);o[3]=uint32_t(l);o[4]=uint32_t(l>>32);o[5]=0;
}
// Linear probe from the hash, wrap at the table end (N4 115-126). Returns slot or -1.
inline int RamhtInsert(uint32_t* w,uint32_t handle,uint32_t chid,uint32_t objOffset,uint32_t start){
    for(uint32_t n=0;n<kHashEntries;++n){
        const uint32_t s=(start+n)&(kHashEntries-1);
        if(w[s*2+1]==0){w[s*2]=handle;w[s*2+1]=RamhtContext(chid,objOffset);return int(s);}
        if(w[s*2]==handle&&(w[s*2+1]>>25)==chid)return int(s);
    }
    return -1;
}
inline Err BuildInstImage(const Config& c,InstImage& img){
    for(auto& x:img.words)x=0;
    img.objCount=img.useCount=0;
    const uint32_t wch=ChidWndw(c.window);
    // Objects: 0 sync (4 KiB notifier page), 1 ISO pitch (VRAM 0..isoLimit), 2 GOP "DAVE".
    img.obj[img.objCount++]={kHandleSync,kFlagsSync,c.syncVram,c.syncVram+0xfff};
    img.obj[img.objCount++]={kHandleIsoPitch,kFlagsIsoPitch,0,c.isoLimit};
    if(c.gopHandle)img.obj[img.objCount++]={kHandleGop,kFlagsIsoPitch,c.surf[0].vram,
        c.surf[0].vram+uint64_t(c.surf[0].pitch)*c.surf[0].height-1};
    for(uint32_t i=0;i<img.objCount;++i)WriteCtxDma(img.words,ObjOffset(i),img.obj[i]);
    auto bind=[&](uint32_t chid,uint32_t obj)->bool{
        const uint32_t h=img.obj[obj].handle;
        if(RamhtInsert(img.words,h,chid,ObjOffset(obj),RamhtHash(h,chid))<0)return false;
        if(c.mirrorRmHash&&RamhtInsert(img.words,h,chid,ObjOffset(obj),RamhtHashRm(h,chid,kClientId))<0)return false;
        img.use[img.useCount++]={chid,h,obj};return true;
    };
    // Window channel: notifier + ISO (+ GOP handle). Core (stage B): notifier only.
    if(!bind(wch,0)||!bind(wch,1)||(c.gopHandle&&!bind(wch,2)))return Err::HashFull;
    if(c.coreChannel&&!bind(kChidCore,0))return Err::HashFull;
    return Err::Ok;
}

// ---- push buffers
struct Push{
    uint32_t words[kPushMaxWords+1];uint32_t count=0;bool overflow=false;
    void Put(uint32_t v){if(count<kPushMaxWords)words[count++]=v;else overflow=true;}
    // Consecutive methods share one header, as PUSH_MTHD does (N13).
    template<typename... D> void Mthd(uint32_t m,D... d){
        const uint32_t v[]={uint32_t(d)...};Put(PushHeader(m,sizeof...(D)));for(uint32_t x:v)Put(x);}
};
inline uint32_t SurfSize(const Surface& s){return (s.height<<16)|s.width;}
// Notifier: SET_CONTEXT_DMA_NOTIFIER + SET_NOTIFIER_CONTROL (N7 224-237, mode WRITE).
inline void PushWindowNtfy(Push& p,uint32_t ntfyOffset){
    p.Mthd(c57e::SET_CONTEXT_DMA_NOTIFIER,kHandleSync,(ntfyOffset>>4)<<4);
}
// Full image state, wndwc57e_image_set order (N8 33-79). NON_TEARING, interval 1
// (N11 153-156 for an interlocked/normal flip).
inline void PushWindowImage(Push& p,const Surface& s,uint32_t handle,uint32_t interval=1){
    p.Mthd(c57e::SET_PRESENT_CONTROL,interval&0xf);
    p.Mthd(c57e::SET_SIZE,SurfSize(s),c57e::LAYOUT_PITCH<<4,s.format&0xff,(s.pitch>>6)&0x1fff);
    p.Mthd(c57e::SET_CONTEXT_DMA_ISO0,handle);
    p.Mthd(c57e::SET_OFFSET0,uint32_t(s.vram>>8));
    p.Mthd(c57e::SET_POINT_IN0,0);
    p.Mthd(c57e::SET_SIZE_IN,SurfSize(s));
    p.Mthd(c57e::SET_SIZE_OUT,SurfSize(s));
}
// Flip: address plus size/storage/params/pitch on every flip, as nvkms does
// (nvkms-evo3.c EvoFlipC3Common; decision 23 Nov: 5 words removes an unknown).
inline void PushWindowFlip(Push& p,const Surface& s,uint32_t handle,uint32_t interval=1){
    p.Mthd(c57e::SET_PRESENT_CONTROL,interval&0xf);
    p.Mthd(c57e::SET_SIZE,SurfSize(s),c57e::LAYOUT_PITCH<<4,s.format&0xff,(s.pitch>>6)&0x1fff);
    p.Mthd(c57e::SET_CONTEXT_DMA_ISO0,handle);
    p.Mthd(c57e::SET_OFFSET0,uint32_t(s.vram>>8));
}
// wndwc37e_update (N7 270-287): no core/cursor interlock, window interlock = self.
inline void PushWindowUpdate(Push& p,uint32_t window,uint32_t coreInterlock=0){
    p.Mthd(c57e::SET_INTERLOCK_FLAGS,coreInterlock&1,1u<<window);
    p.Mthd(c57e::UPDATE,0x1);
}
// corec57d_init (N10 31-60). matchArm: usage bounds copied from the GOP's ARM
// state so a later core UPDATE would not change window limits.
inline void PushCoreInit(Push& p,const GopState& g,bool matchArm,uint32_t windows=8){
    p.Mthd(c57d::SET_CONTEXT_DMA_NOTIFIER,kHandleSync);
    for(uint32_t i=0;i<windows;++i){
        if(matchArm&&i==0){
            p.Mthd(c57d::WindowFormatUsage(i),g.coreFmtUsage,g.coreRotUsage);
            p.Mthd(c57d::WindowUsage(i),g.coreUsage);
        }else{
            p.Mthd(c57d::WindowFormatUsage(i),0xf,0);           // RGB_PACKED1/2/4/8BPP
            p.Mthd(c57d::WindowUsage(i),0x7fffu|(1u<<16)|(1u<<20)); // max 0x7fff, ILUT, TAPS_2
        }
    }
}
// corec37d_wndw_owner (N9 34-49): nouveau maps window i -> head i>>1. matchArm
// keeps the GOP mapping (window 0 -> head 0, the rest NONE).
inline void PushCoreWindowOwner(Push& p,bool matchArm,uint32_t windows=8){
    for(uint32_t i=0;i<windows;++i)
        p.Mthd(c57d::WindowSetControl(i),matchArm?(i==0?0u:c57d::OWNER_NONE):(i>>1));
}
// corec37d_update with notifier (N9 52-79).
inline void PushCoreUpdate(Push& p,uint32_t cursInterlock,uint32_t wndwInterlock,bool ntfy){
    if(ntfy)p.Mthd(c57d::SET_NOTIFIER_CONTROL,(1u<<12)|0);
    p.Mthd(c57d::SET_INTERLOCK_FLAGS,cursInterlock,wndwInterlock);
    p.Mthd(c57d::UPDATE,0x1);
    if(ntfy)p.Mthd(c57d::SET_NOTIFIER_CONTROL,0);
}

// ---------------------------------------------------------------- op lists
enum class Op:uint8_t{
    Write,      // reg = value
    Mask,       // reg = (reg & ~mask) | value
    Expect,     // (reg & mask) == value, else abort
    Poll,       // until (reg & mask) == value or timeoutMs, else abort
    PollSoft,   // like Poll but a timeout is only logged (nouveau ignores fini idle)
    AckIfSet,   // v = reg & mask; if v, reg = v (W1C)
    VramCopy,   // copy blob `value` (Blob) of `mask` bytes to VRAM `vram` via BAR1
    VramWrite,  // vram32 = value (notifier reset, N9 94-101)
    VramExpect, // (vram32 & mask) == value
    VramPoll,   // until (vram32 & mask) == value
    VramPollAny,// until (vram32 & mask) != 0
    AckException,// if (0x61184c & mask): log RegExcept(reg) stat/data/code, W1C 0x61184c,
                 // write 0x90000000 to RegExcept(reg) (N1 1040-1043, 894-933)
    PraminExpect,// (BAR0 kPramin + reg) == word `value` of blob `mask`, see PraminCheck
    Fence,      // sfence before PUT (R3 54)
    Kick,       // reg (PUT) = value, after Fence; GET must reach it next
    ExpectNot,  // (reg & mask) != value, else abort
    Log,        // read reg and log it; never fails
    Copy,       // reg `value` = read reg (caps copies, tu102_disp_init)
    AssyArmDiff,// chid `reg`: every method mask..value-4 must have ASSY == ARM unless the
                // first submission rewrites it (Plan::rewritten); diffs go to io.diff
    Pause,      // live: wait for the operator (io.pause); dry run: no-op
    Delay,      // io.udelay(value) (VBIOS TIME)
    AndNotIf,   // if ((reg & x) == x) reg &= ~mask (VBIOS CONDITION + ANDN_REG)
    CopyMask,   // if (!y || (rd(y) & z) == 0): reg `value` = (rd(value) & x) | (rd(reg) & mask) (COPY_NV_REG)
    Call,       // io.call(reg) runs a dynamic step (DP link training); false aborts
    SurfFill,   // io.fill(vram, bytes=value, pitch=mask, colour x, marker colour y, marker lines z)
    PraminRange,// value words at BAR0 kPramin+reg == blob(mask) words from byte x; got = first bad index
    Dump,       // read value words from reg and hand them to io.dump (diagnostics; never fails)
    PollOr,     // until ((reg | x) & mask) == value; fails at once if y && (y & z) (exception), else on timeout
    ExpectSoft, // (reg & mask) == value, else io.flag(step, got): a sticky warning, never fails
    ExpectVpll, // VPLL at reg (+4 P|N|M, +0x10 fN<<16) runs at value kHz +- mask with refclk x kHz
};
enum class Blob:uint32_t{Inst,WndwPush,CorePush,SyncInit};
struct Step{Op op;uint32_t reg,mask,value,timeoutMs;uint64_t vram;const char* what;uint32_t x,y,z;};
enum class Phase:uint8_t{Preflight,Vram,Instmem,Core,Window,FirstFlip,Flip,Restore,Teardown,Quiesce,Count};
inline const char* PhaseName(Phase p){
    static const char* n[]={"preflight","vram","instmem","core","window","first-flip","flip","restore","teardown","quiesce"};
    return n[uint32_t(p)];
}
struct Plan{
    static constexpr uint32_t kMax=768,kPhases=16,kMthds=0x4000/4;
    Step steps[kMax];uint32_t count=0;bool overflow=false;
    uint32_t rewritten[kMthds/32]{}; // methods the plan's own pushbuffer sets (excluded from AssyArmDiff)
    void MarkRewritten(const uint32_t* w,uint32_t n){
        for(uint32_t i=0;i<n;){const uint32_t hdr=w[i++];if(hdr>>29)continue;
            const uint32_t cnt=(hdr>>18)&0x3ff,m=hdr&0x3ffc;
            for(uint32_t k=0;k<cnt&&i<n;++k,++i){const uint32_t mm=(m+k*4)/4;if(mm<kMthds)rewritten[mm/32]|=1u<<(mm%32);}}}
    bool Rewritten(uint32_t mthd)const{const uint32_t mm=mthd/4;return mm<kMthds&&(rewritten[mm/32]>>(mm%32))&1;}
    // Strict diffs (AssyArmDiff with x=1): fail on any difference in a known method
    // except our own (ours); differences in methods not known are only logged.
    uint32_t ours[kMthds/32]{},known[kMthds/32]{};
    static void Set(uint32_t* b,uint32_t m){const uint32_t mm=m/4;if(mm<kMthds)b[mm/32]|=1u<<(mm%32);}
    static bool Has(const uint32_t* b,uint32_t m){const uint32_t mm=m/4;return mm<kMthds&&(b[mm/32]>>(mm%32))&1;}
    uint32_t phaseBegin[kPhases+1]{};
    void Begin(Phase p){phaseBegin[uint32_t(p)]=count;}
    void BeginN(uint32_t p){phaseBegin[p]=count;}
    void End(){phaseBegin[uint32_t(Phase::Count)]=count;}
    void EndN(uint32_t n){phaseBegin[n]=count;}
    void Add(Op op,uint32_t reg,uint32_t mask,uint32_t value,uint32_t ms,const char* what,uint64_t vram=0,
             uint32_t x=0,uint32_t y=0,uint32_t z=0){
        if(count<kMax)steps[count++]={op,reg,mask,value,ms,vram,what,x,y,z};else overflow=true;}
};
constexpr uint32_t kTimeoutMs=2000; // every nouveau poll in N1/N6 is 2000 ms

// The window ring holds every submission of the dry run back to back:
//   [0] first flip: notifier + full image state for surf[0] + update. The GOP
//       surface under our ISO ctxdma, so nothing visible changes.
//   [1] flip to surf[1] (address only)
//   [2] restore: surf[0] under the GOP's own "DAVE" handle when bound.
struct Ring{uint32_t put=0;}; // byte offset of the next free word
// Submissions: [0] first flip, [1..nflips] flips B/A/B..., [nflips+1] restore.
constexpr uint32_t kMaxFlips=16,kMaxSub=kMaxFlips+2;
struct Pushes{Push wndw;Push core;uint32_t count=0;uint32_t end[kMaxSub]{};uint32_t ntfy[kMaxSub]{};uint32_t handle[kMaxSub]{};uint64_t offset[kMaxSub]{};};
inline uint32_t RingAppend(Push& ring,Ring& r,const Push& p){
    uint32_t at=r.put/4;
    if(at+p.count+1>kPushMaxWords){ring.words[at]=PushJump(0);at=0;} // caller waits GET>0 (N6 173-186)
    for(uint32_t i=0;i<p.count;++i)ring.words[at+i]=p.words[i];
    if(at+p.count>ring.count)ring.count=at+p.count;
    if(p.overflow)ring.overflow=true;
    r.put=(at+p.count)*4;return r.put;
}
// Runtime flip: notifier slot `parity`, address-only state, update.
inline uint32_t BuildFlip(Push& ring,Ring& r,const Config& c,const Surface& s,uint32_t parity,uint32_t handle=kHandleIsoPitch){
    Push p;PushWindowNtfy(p,WindowNtfyOffset(c.window,parity));
    PushWindowFlip(p,s,handle);PushWindowUpdate(p,c.window);
    return RingAppend(ring,r,p);
}
// Replay as few headers as possible: consecutive methods share one.
inline void PushReplay(Push& p,const Config& c){
    for(uint32_t i=0;i<kReplayCount;){
        uint32_t j=i+1;while(j<kReplayCount&&kReplayMthds[j]==kReplayMthds[j-1]+4)++j;
        p.Put(PushHeader(kReplayMthds[i],j-i));for(uint32_t k=i;k<j;++k)p.Put(c.replayVal[k]);
        i=j;
    }
}
template<typename Rd> inline void ReadReplay(Config& c,Rd rd){
    for(uint32_t i=0;i<kReplayCount;++i)c.replayVal[i]=rd(RegArm(ChidWndw(c.window),kReplayMthds[i]));
}
inline uint32_t BuildFirstFlip(Push& ring,Ring& r,const Config& c){
    Push p;PushWindowNtfy(p,WindowNtfyOffset(c.window,0));
    if(c.replay)PushReplay(p,c);
    PushWindowImage(p,c.surf[0],kHandleIsoPitch);PushWindowUpdate(p,c.window);
    return RingAppend(ring,r,p);
}
inline uint32_t RestoreHandle(const Config& c){return c.gopHandle?kHandleGop:kHandleIsoPitch;}
inline void BuildPushes(const Config& c,Pushes& out,uint32_t nflips=1){
    Ring r;if(nflips>kMaxFlips)nflips=kMaxFlips;
    out.count=nflips+2;
    out.end[0]=BuildFirstFlip(out.wndw,r,c);out.ntfy[0]=WindowNtfyOffset(c.window,0);
    out.handle[0]=kHandleIsoPitch;out.offset[0]=c.surf[0].vram;
    for(uint32_t i=1;i<=nflips;++i){const Surface& s=c.surf[i&1];
        out.end[i]=BuildFlip(out.wndw,r,c,s,i&1);out.ntfy[i]=WindowNtfyOffset(c.window,i&1);
        out.handle[i]=kHandleIsoPitch;out.offset[i]=s.vram;}
    const uint32_t k=nflips+1;
    out.end[k]=BuildFlip(out.wndw,r,c,c.surf[0],k&1,RestoreHandle(c));out.ntfy[k]=WindowNtfyOffset(c.window,k&1);
    out.handle[k]=RestoreHandle(c);out.offset[k]=c.surf[0].vram;
}

inline uint64_t SurfBytes(const Surface& s){return uint64_t(s.pitch)*s.height;}
inline Err Validate(const Config& c){
    for(const auto& s:c.surf){
        if((s.vram&0xff)||(s.pitch&63)||!s.width||!s.height)return Err::Align;
        if(s.format!=c57e::FORMAT_A8R8G8B8&&s.format!=c57e::FORMAT_X8R8G8B8)return Err::Format;
        if(s.pitch<s.width*4u||(s.pitch>>6)>0x1fff)return Err::Bounds;
        if(s.vram+SurfBytes(s)-1>c.isoLimit)return Err::Bounds;
        if(s.width>(c.gop.coreUsage&0x7fff))return Err::Usage;      // MAX_PIXELS_FETCHED_PER_LINE
        if(!(c.gop.coreFmtUsage&0x4))return Err::Usage;              // RGB_PACKED4BPP
        if(SurfSize(s)!=c.gop.winSize||s.format!=c.gop.winParams)return Err::Usage; // keep the GOP mode
        if(((s.pitch>>6)&0x1fff)!=c.gop.winPlanar)return Err::Usage;
    }
    if((c.instVram&0xffff)||(c.corePushVram&0xfff)||(c.wndwPushVram&0xfff)||(c.syncVram&0xfff))return Err::Align;
    if(c.praminCheck&&((c.instVram&0xfffff)||((c.wndwPushVram^c.instVram)>>20)))return Err::Align; // one 1 MiB PRAMIN window
    struct R{uint64_t a,b;}r[6];uint32_t n=0;
    r[n++]={c.surf[0].vram,c.surf[0].vram+SurfBytes(c.surf[0])};
    r[n++]={c.surf[1].vram,c.surf[1].vram+SurfBytes(c.surf[1])};
    r[n++]={c.instVram,c.instVram+kInstBytes};
    r[n++]={c.wndwPushVram,c.wndwPushVram+kPushBytes};
    r[n++]={c.syncVram,c.syncVram+0x1000};
    if(c.coreChannel)r[n++]={c.corePushVram,c.corePushVram+kPushBytes};
    for(uint32_t i=0;i<n;++i){
        if(r[i].b-1>c.isoLimit)return Err::Bounds;
        for(uint32_t j=i+1;j<n;++j)if(r[i].a<r[j].b&&r[j].a<r[i].b)return Err::Overlap;}
    return Err::Ok;
}

// Channel bring-up, gv100_disp_dmac_init / core_init (N1 373-390, 760-774).
// Deviation: nouveau enables the channel's exception interrupt first
// (chan.c nvkm_disp_chan_init 145 -> gv100 intr 0x611da4/0x611dac bit);
// Brook polls the latched status instead and leaves every enable at 0.
inline void AddChannelInit(Plan& p,uint32_t chid,uint64_t pushVram){
    const uint64_t push=uint64_t(pushVram>>8)|1;  // VRAM target | addr>>8 (N3 694-701)
    p.Add(Op::Write,RegPbLo(chid),~0u,uint32_t(push),0,"pushbuffer lo = VRAM|addr>>8");
    p.Add(Op::Write,RegPbHi(chid),~0u,uint32_t(push>>32),0,"pushbuffer hi");
    p.Add(Op::Write,RegPbValid(chid),~0u,1,0,"pushbuffer valid (nouveau 1)");
    p.Add(Op::Write,RegPbClient(chid),~0u,kClientId,0,"pushbuffer client id 0x40 (= RAMHT client)");
    p.Add(Op::Mask,RegChnCtl(chid),kCtlPutptrWrite,kCtlPutptrWrite,0,"CHNCTL |= PUTPTR_WRITE");
    p.Add(Op::Write,RegPut(chid),~0u,0,0,"PUT = 0");
    p.Add(Op::Write,RegChnCtl(chid),~0u,kCtlRunning,0,"CHNCTL = ALLOCATE|CONNECT|PUTPTR_WRITE");
    if(chid==kChidCore)p.Add(Op::Poll,RegChnStatus(chid),kCoreStateMask,kCoreIdle,kTimeoutMs,"core STATE == IDLE (0xb)");
    else p.Add(Op::Poll,RegChnStatus(chid),kWinStateMask,kWinIdle,kTimeoutMs,"window STATE == IDLE (4)");
    p.Add(Op::Expect,RegGet(chid),0xffc,0,0,"GET == 0 (masked: reads 0x80000000 unconnected)");
}
// gv100_disp_dmac_fini / core_fini (N1 361-370, 750-757), plus RM's
// UNCONNECTED wait (kern_disp_channel_0300.c 408-418) for windows.
inline void AddChannelFini(Plan& p,uint32_t chid,uint32_t ms,bool check=true){
    p.Add(Op::Mask,RegChnCtl(chid),kCtlPutptrWrite,0,0,"CHNCTL &= ~PUTPTR_WRITE");
    if(chid==kChidCore)p.Add(Op::PollSoft,RegChnStatus(chid),kCoreStateMask,kCoreIdle,ms,"core idle");
    else p.Add(Op::PollSoft,RegChnStatus(chid),kWinStateMask,kWinIdle,ms,"window idle");
    p.Add(Op::Mask,RegChnCtl(chid),kCtlConnect,0,0,"CHNCTL &= ~CONNECT");
    if(chid!=kChidCore)p.Add(Op::PollSoft,RegChnStatus(chid),kWinStateMask,kWinUnconnected,ms,"window UNCONNECTED (1)");
    if(check)p.Add(Op::Expect,RegChnCtl(chid),0x6013,0x0001,0,"CHNCTL == ALLOCATE only");
}
// Pushbuffer registers back to what the GOP left (hi 0, lo 1, valid 1, client 0).
inline void AddPbRestore(Plan& p,uint32_t chid,const GopState& g){
    p.Add(Op::Write,RegPbLo(chid),~0u,g.pbLo,0,"pushbuffer lo = GOP value");
    p.Add(Op::Write,RegPbHi(chid),~0u,g.pbHi,0,"pushbuffer hi = GOP value");
    p.Add(Op::Write,RegPbValid(chid),~0u,g.pbValid,0,"pushbuffer valid = GOP value");
    p.Add(Op::Write,RegPbClient(chid),~0u,g.pbClient,0,"pushbuffer client = GOP value");
}
// tu102_disp_init caps copies (N2 198-240) into the FE_SW caps page.
inline void AddCapsCopy(Plan& p){
    p.Add(Op::Write,0x640008,~0u,0x21,0,"caps: lock pins (tu102.c 199-200)");
    for(uint32_t i=0;i<4;++i){
        p.Add(Op::Mask,0x640000,0x100u<<i,0x100u<<i,0,"caps: SOR present");
        p.Add(Op::Copy,0x61c000+i*0x800,0,0x640144+i*8,0,"caps: SOR cap");}
    for(uint32_t h=0;h<4;++h){
        p.Add(Op::Copy,0x616300+h*0x800,0,0x640048+h*0x20,0,"caps: head RG");
        for(uint32_t j=0;j<20;j+=4)p.Add(Op::Copy,0x616140+h*0x800+j,0,0x640680+h*0x20+j,0,"caps: head POSTCOMP");}
    for(uint32_t w=0;w<8;++w){
        p.Add(Op::Mask,0x640004,1u<<w,1u<<w,0,"caps: window present");
        for(uint32_t j=0;j<24;j+=4)p.Add(Op::Copy,0x630100+w*0x800+j,0,0x640780+w*0x20+j,0,"caps: window cap");
        p.Add(Op::Mask,0x64000c,0x100,0x100,0,"caps: window caps valid");}
    for(uint32_t i=0;i<3;++i)p.Add(Op::Copy,0x62e000+i*4,0,0x640010+i*4,0,"caps: IHUB");
}
inline void AddExceptionCheck(Plan& p,uint32_t window){
    p.Add(Op::Expect,kRegIntrDispatch,0x1e00,0,0,"no EXC_WIN/WINIM/OTHER/CTRL_DISP pending");
    p.Add(Op::Expect,kRegExcWin,1u<<window,0,0,"no window exception");
    p.Add(Op::Expect,kRegExcOther,1,0,0,"no core exception (0x611854 bit0)");
}
inline void AddKickAndWait(Plan& p,const Config& c,uint32_t putBytes,uint32_t handle,uint64_t offset,uint32_t ntfy){
    const uint32_t ch=ChidWndw(c.window);
    p.Add(Op::VramWrite,0,~0u,kNtfyNotBegun,0,"notifier slot reset NOT_BEGUN",c.syncVram+ntfy);
    p.Add(Op::Write,kRegBar1Flush,~0u,1,0,"BAR1 write flush (VRAM pushbuffer, N6 142-148)");
    p.Add(Op::Poll,kRegBar1Flush,0x2,0,kTimeoutMs,"flush done");
    p.Add(Op::Kick,RegPut(ch),~0u,putBytes,0,"sfence; PUT = byte offset of push end");
    p.Add(Op::Poll,RegGet(ch),~0u,putBytes,kTimeoutMs,"GET == PUT (methods fetched)");
    AddExceptionCheck(p,c.window);
    p.Add(Op::ExpectNot,RegChnStatus(ch),0xf00,0xb00,0,"UPD_STATE != EXCEPTION (0xb)");
    p.Add(Op::VramPollAny,0,3u<<kNtfyStatusShift,0,kTimeoutMs,"notifier BEGUN/FINISHED (flip latched)",c.syncVram+ntfy);
    p.Add(Op::Poll,RegArm(ch,c57e::SET_CONTEXT_DMA_ISO0),~0u,handle,kTimeoutMs,"ARM ISO handle");
    p.Add(Op::Poll,RegArm(ch,c57e::SET_OFFSET0),~0u,uint32_t(offset>>8),kTimeoutMs,"ARM SET_OFFSET");
}

// The whole step-3 dry run in execution order: preflight, VRAM objects,
// instmem, (core), window channel, first flip (no visible change), flip to
// surf[1], restore to the GOP surface, teardown back to the GOP state.
// B's contents before the first flip to it: a fast pattern fill (UC reads of A
// over BAR1 would take seconds): dark grey with an orange marker bar on top.
constexpr uint32_t kFillGrey=0xff303030,kFillMarker=0xffff8000,kFillMarkerLines=120;
inline Err BuildPlan(const Config& c,InstImage& img,Pushes& pushes,Plan& p,uint32_t nflips=1,uint32_t holdMs=0){
    if(Err e=Validate(c);e!=Err::Ok)return e;
    if(Err e=BuildInstImage(c,img);e!=Err::Ok)return e;
    if(nflips>kMaxFlips||holdMs>5000)return Err::Bounds;
    BuildPushes(c,pushes,nflips);
    if(pushes.wndw.overflow)return Err::PushFull;
    const auto& g=c.gop;const uint32_t ch=ChidWndw(c.window),w=c.window,h=c.head;
    p.Begin(Phase::Preflight);
    p.Add(Op::Expect,kRegDispOwner,0x2,0,0,"VBIOS ownership bit clear (else tu102_disp_init claim)");
    p.Add(Op::Expect,kRegInstTarget,~0u,g.instTarget,0,"no display instmem (GOP)");
    p.Add(Op::Expect,kRegInstAddr,~0u,g.instAddr,0,"instmem addr 0");
    p.Add(Op::Expect,RegChnCtl(kChidCore),0xffff,g.ctlCore,0,"core CHNCTL as left by GOP");
    p.Add(Op::Expect,RegChnStatus(kChidCore),kCoreStateMask,kCoreEfiOperation,0,"core STATE EFI_OPERATION (7)");
    p.Add(Op::Expect,RegChnCtl(ch),0xffff,g.ctlWndw,0,"window CHNCTL ALLOCATE only");
    p.Add(Op::Expect,RegChnStatus(ch),kWinStateMask,kWinUnconnected,0,"window STATE UNCONNECTED (1)");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::WindowSetControl(w)),0xf,g.coreWinCtl,0,"ARM window owner = head 0");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::WindowFormatUsage(w)),~0u,g.coreFmtUsage,0,"ARM format usage bounds");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::WindowUsage(w)),~0u,g.coreUsage,0,"ARM window usage bounds");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(h)),~0u,g.headRaster,0,"ARM head raster (mode unchanged)");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadPixelClock(h)),~0u,g.headPclk,0,"ARM pixel clock");
    p.Add(Op::Expect,RegArm(kChidCore,c57d::SorSetControl(g.sor)),~0u,g.sorCtl,0,"ARM SOR control (DP-B, head 0)");
    p.Add(Op::Expect,RegAssy(ch,c57e::SET_SIZE),~0u,g.winSize,0,"window size");
    p.Add(Op::Expect,RegAssy(ch,c57e::SET_STORAGE),~0u,g.winStorage,0,"window storage (pitch)");
    p.Add(Op::Expect,RegAssy(ch,c57e::SET_PARAMS),~0u,g.winParams,0,"window format");
    p.Add(Op::Expect,RegAssy(ch,c57e::SET_PLANAR_STORAGE0),~0u,g.winPlanar,0,"window pitch");
    p.Add(Op::Expect,RegAssy(ch,c57e::SET_CONTEXT_DMA_ISO0),~0u,g.winIso,0,"window ISO handle");
    p.Add(Op::Expect,RegArm(ch,c57e::SET_CONTEXT_DMA_ISO0),~0u,g.winIso,0,"window ARM ISO handle");
    p.Add(Op::Expect,RegArm(ch,c57e::SET_OFFSET0),~0u,g.winOffset,0,"window ARM offset");
    p.MarkRewritten(pushes.wndw.words,pushes.end[0]/4);
    p.Add(Op::AssyArmDiff,ch,0x200,0x800,0,"window ASSY 0x200-0x7fc == ARM, or rewritten by submission 1");
    p.Add(Op::Log,RegArm(kChidCore,0x2288+h*0x400),0,0,0,"head OLUT ctxdma (non-zero: firmware LUT in VRAM)");
    p.Add(Op::Log,RegArm(kChidCore,0x228c+h*0x400),0,0,0,"head OLUT offset");
    p.Add(Op::Expect,kRegFeEnable,~0u,g.feEnable,0,"0x610078 as left by GOP");
    AddExceptionCheck(p,w);
    p.Begin(Phase::Vram);
    p.Add(Op::VramCopy,uint32_t(Blob::Inst),kInstBytes,0,0,"instmem image (RAMHT + ctxdmas)",c.instVram);
    p.Add(Op::VramCopy,uint32_t(Blob::SyncInit),0x1000,0,0,"sync page zeroed (notifiers NOT_BEGUN)",c.syncVram);
    p.Add(Op::VramCopy,uint32_t(Blob::WndwPush),kPushBytes,0,0,"window pushbuffer (all submissions)",c.wndwPushVram);
    if(c.coreChannel)p.Add(Op::VramCopy,uint32_t(Blob::CorePush),kPushBytes,0,0,"core pushbuffer (empty)",c.corePushVram);
    p.Add(Op::Fence,0,0,0,0,"sfence");
    p.Add(Op::Write,kRegBar1Flush,~0u,1,0,"BAR1 write flush");
    p.Add(Op::Poll,kRegBar1Flush,0x2,0,kTimeoutMs,"flush done");
    if(c.praminCheck){
        p.Add(Op::Write,kRegPraminBase,~0u,uint32_t(c.instVram>>16),0,"PRAMIN window -> instmem (BAR1 identity check)");
        for(uint32_t s=0;s<kHashEntries;++s)if(img.words[s*2+1])
            p.Add(Op::PraminRange,s*8,uint32_t(Blob::Inst),2,0,"RAMHT entry (handle, context) at its VRAM address",0,s*8);
        p.Add(Op::PraminRange,kObjBase,uint32_t(Blob::Inst),img.objCount*kCtxDmaBytes/4,0,"ctxdma objects",0,kObjBase);
        p.Add(Op::PraminRange,uint32_t(c.wndwPushVram-c.instVram),uint32_t(Blob::WndwPush),pushes.wndw.count,0,"window pushbuffer words",0,0);
        p.Add(Op::Write,kRegPraminBase,~0u,c.praminRestore,0,"PRAMIN window restored");
    }
    p.Begin(Phase::Instmem);
    // Address first, so instmem never points at VRAM 0 (the GOP surface) while valid.
    p.Add(Op::Write,kRegInstAddr,~0u,uint32_t(c.instVram>>16),0,"instmem addr >> 16 (N2 251)");
    p.Add(Op::Write,kRegInstTarget,~0u,0x8|0x1,0,"instmem valid, VRAM (N2 250)");
    p.Add(Op::Expect,kRegInstTarget,~0u,0x9,0,"readback");
    p.Add(Op::Expect,kRegInstAddr,~0u,uint32_t(c.instVram>>16),0,"readback");
    if(c.capsCopy)AddCapsCopy(p);
    if(c.feEnable)p.Add(Op::Mask,kRegFeEnable,1,1,0,"0x610078 |= 1 (N2 240)");
    p.Begin(Phase::Core);
    if(c.coreChannel){ // stage B: connect only; no core methods, no core UPDATE
        AddChannelInit(p,kChidCore,c.corePushVram);
        p.Add(Op::Expect,RegArm(kChidCore,c57d::HeadRasterSize(h)),~0u,g.headRaster,0,"ARM head state survived connect");
        p.Add(Op::Expect,RegArm(kChidCore,c57d::WindowSetControl(w)),0xf,g.coreWinCtl,0,"ARM window owner survived connect");
    }
    p.Begin(Phase::Window);
    AddChannelInit(p,ch,c.wndwPushVram);
    // Connect may reset ASSY (live 23 Nov: 0x22c read 0xe9). Every diff is logged;
    // only methods submission 1 does not rewrite fail the run.
    if(c.dumpAfterConnect){
        p.Add(Op::Dump,RegAssy(ch,0x200),0,0x180,0,"window ASSY 0x200-0x7fc after connect");
        p.Add(Op::Dump,RegArm(ch,0x200),0,0x180,0,"window ARM 0x200-0x7fc after connect");}
    p.Add(Op::AssyArmDiff,ch,0x200,0x800,0,"after connect: ASSY == ARM, or rewritten by submission 1");
    p.Add(Op::Expect,RegArm(ch,c57e::SET_CONTEXT_DMA_ISO0),~0u,g.winIso,0,"ARM unchanged by connect");
    AddExceptionCheck(p,w);
    p.Begin(Phase::FirstFlip);
    AddKickAndWait(p,c,pushes.end[0],pushes.handle[0],pushes.offset[0],pushes.ntfy[0]);
    p.Add(Op::Expect,RegArm(ch,c57e::SET_SIZE),~0u,g.winSize,0,"ARM size unchanged");
    p.Add(Op::Expect,RegArm(ch,c57e::SET_PARAMS),~0u,g.winParams,0,"ARM format unchanged");
    p.Begin(Phase::Flip);
    if(nflips){
        p.Add(Op::SurfFill,0,c.surf[1].pitch,uint32_t(SurfBytes(c.surf[1])),0,"surface B: grey + marker bar (BAR1)",c.surf[1].vram,
              kFillGrey,kFillMarker,kFillMarkerLines);
        p.Add(Op::Fence,0,0,0,0,"sfence");
    }
    for(uint32_t i=1;i<=nflips;++i){
        AddKickAndWait(p,c,pushes.end[i],pushes.handle[i],pushes.offset[i],pushes.ntfy[i]);
        if(holdMs)p.Add(Op::Delay,0,0,holdMs*1000,0,"hold");
    }
    p.Begin(Phase::Restore);
    {const uint32_t k=nflips+1;AddKickAndWait(p,c,pushes.end[k],pushes.handle[k],pushes.offset[k],pushes.ntfy[k]);}
    p.Begin(Phase::Teardown);
    AddChannelFini(p,ch,kTimeoutMs);
    AddPbRestore(p,ch,g);
    if(c.coreChannel){AddChannelFini(p,kChidCore,kTimeoutMs);AddPbRestore(p,kChidCore,g);}
    if(c.pauseForObserver)p.Add(Op::Pause,0,0,0,0,"observer: screen unchanged with the window disconnected?");
    if(c.feEnable)p.Add(Op::Write,kRegFeEnable,~0u,g.feEnable,0,"0x610078 = GOP value");
    p.Add(Op::Write,kRegInstTarget,~0u,g.instTarget,0,"instmem invalid (GOP value) before the address");
    p.Add(Op::Write,kRegInstAddr,~0u,g.instAddr,0,"instmem addr (GOP value)");
    p.Add(Op::Expect,RegArm(ch,c57e::SET_CONTEXT_DMA_ISO0),~0u,RestoreHandle(c),0,"scanout handle is the GOP's");
    p.Add(Op::Expect,RegArm(ch,c57e::SET_OFFSET0),~0u,uint32_t(c.surf[0].vram>>8),0,"scanout is the GOP surface");
    AddExceptionCheck(p,w);
    p.Begin(Phase::Quiesce);
    p.End();
    return p.overflow?Err::TooManyOps:Err::Ok;
}

// Abort / pre-reset quiesce: best effort, bounded, from any partial state.
// `reached` = the highest phase that started. It does not try to flip
// (a broken channel cannot); callers that still have a working channel run
// the Restore submission first. A wedged window is trash-and-aborted like
// RM's kdispSetChannelTrashAndAbortAccel_v03_00 (TRASH_MODE=2, disconnect).
inline void BuildAbort(const Config& c,Phase reached,Plan& p,uint32_t ms=100){
    const uint32_t ch=ChidWndw(c.window);
    p.Begin(Phase::Quiesce);
    if(reached>=Phase::Window){
        // Trash first so nothing behind the faulting method runs once the
        // exception is acked (review 23 Nov; RM kdispSetChannelTrashAndAbortAccel).
        p.Add(Op::Mask,RegChnCtl(ch),kCtlTrashAbort,kCtlTrashAbort,0,"TRASH_AND_ABORT pending methods");
        p.Add(Op::AckException,ch,1u<<c.window,0,0,"log + W1C 0x61184c + ack 0x90000000 (N1 1040-1043, 932)");
        p.Add(Op::PollSoft,RegChnStatus(ch),kWinMethodExecBusy,0,ms,"METHOD_EXEC idle");
        AddChannelFini(p,ch,ms,false);
        p.Add(Op::Mask,RegChnCtl(ch),kCtlTrashAbort,0,0,"TRASH_MODE off");
        p.Add(Op::Expect,RegChnCtl(ch),0x6013,0x0001,0,"CHNCTL == ALLOCATE only");
        AddPbRestore(p,ch,c.gop);
    }
    if(reached>=Phase::Core&&c.coreChannel){
        p.Add(Op::AckException,kChidCore,1,0,0,"log 0x611020/24/28, W1C 0x611854, ack 0x90000000 (gv100.c 992-994)",0,kRegExcOther);
        AddChannelFini(p,kChidCore,ms);AddPbRestore(p,kChidCore,c.gop);}
    if(reached>=Phase::Instmem){
        if(c.feEnable)p.Add(Op::Write,kRegFeEnable,~0u,c.gop.feEnable,0,"0x610078 = GOP value");
        p.Add(Op::Write,kRegInstTarget,~0u,c.gop.instTarget,0,"instmem invalid");
        p.Add(Op::Write,kRegInstAddr,~0u,c.gop.instAddr,0,"instmem addr 0");
    }
    if(reached>=Phase::Vram&&c.praminCheck)p.Add(Op::Write,kRegPraminBase,~0u,c.praminRestore,0,"PRAMIN window restored");
    p.Add(Op::Log,RegArm(ch,c57e::SET_CONTEXT_DMA_ISO0),0,0,0,"final ARM ISO handle (on screen)");
    p.Add(Op::Log,RegArm(ch,c57e::SET_OFFSET0),0,0,0,"final ARM SET_OFFSET");
    p.Add(Op::Log,RegChnStatus(ch),0,0,0,"final window CHNSTATUS (STATE 19:16, UPD_STATE 11:8)");
    p.Add(Op::Log,RegChnStatus(kChidCore),0,0,0,"final core CHNSTATUS");
    p.Add(Op::Log,kRegIntrDispatch,0,0,0,"final 0x611ec0");
    p.End();
}

// Executor shared by the host tests and the live path. Io must provide:
//   uint32_t rd(uint32_t reg); void wr(uint32_t reg,uint32_t v);
//   uint32_t vrd(uint64_t vram); void vwr(uint64_t vram,uint32_t v);
//   void vcopy(uint64_t vram,Blob b,uint32_t bytes); void except(uint32_t chid,stat,data,code);
//   uint32_t blob(Blob b,uint32_t byteOffset); void fence(); bool expired(uint32_t ms,uint32_t& state);
//   void log(uint32_t index,const Step&,uint32_t got,bool ok);
//   void diff(uint32_t mthd,uint32_t assy,uint32_t arm); void pause(const char* what);
//   void udelay(uint32_t us); bool call(uint32_t id); void dump(uint32_t addr,const uint32_t* w,uint32_t n);
//   void flag(uint32_t index,const Step&,uint32_t got);
//   void fill(uint64_t vram,uint32_t bytes,uint32_t pitch,uint32_t colour,uint32_t marker,uint32_t lines);
// Returns `end` on success, else the index of the (first) failing step.
// bestEffort (abort/quiesce lists) logs a failure and carries on.
template<typename Io> uint32_t Run(const Plan& p,uint32_t begin,uint32_t end,Io& io,bool bestEffort=false){
    uint32_t firstFail=end;
    for(uint32_t i=begin;i<end;++i){
        const Step& s=p.steps[i];uint32_t got=0;bool ok=true;
        switch(s.op){
        case Op::Write:io.wr(s.reg,s.value);break;
        case Op::Mask:io.wr(s.reg,(io.rd(s.reg)&~s.mask)|s.value);break;
        case Op::Expect:got=io.rd(s.reg);ok=(got&s.mask)==s.value;break;
        case Op::Poll:case Op::PollSoft:{uint32_t t=0;
            while(((got=io.rd(s.reg))&s.mask)!=s.value)if(io.expired(s.timeoutMs,t)){ok=s.op==Op::PollSoft;break;}
            break;}
        case Op::AckIfSet:got=io.rd(s.reg)&s.mask;if(got)io.wr(s.reg,got);break;
        case Op::AckException:{const uint32_t st=s.x?s.x:kRegExcWin;got=io.rd(st)&s.mask; // x: status reg override (core: 0x611854)
            if(got){io.except(s.reg,io.rd(RegExcept(s.reg)),io.rd(RegExcept(s.reg)+4),io.rd(RegExcept(s.reg)+8));
                io.wr(st,got);io.wr(RegExcept(s.reg),kExceptAck);}
            break;}
        case Op::PollOr:{uint32_t t=0;
            for(;;){got=io.rd(s.reg)|io.rd(s.x);if((got&s.mask)==s.value)break;
                if(s.y&&(io.rd(s.y)&s.z)){ok=false;got=0xe0000000u|io.rd(s.y);break;}
                if(io.expired(s.timeoutMs,t)){ok=false;break;}}
            break;}
        case Op::ExpectSoft:got=io.rd(s.reg);if((got&s.mask)!=s.value)io.flag(i,s,got);break;
        case Op::ExpectVpll:{const uint32_t c=io.rd(s.reg+4),f=io.rd(s.reg+0x10);
            const uint32_t P=(c>>16)&0x3f,N=(c>>8)&0xff,M=c&0xff;const int32_t fN=int16_t(f>>16);
            if(!P||!M){ok=false;got=0;break;}
            const uint64_t khz=uint64_t(s.x)*(uint64_t(N)*8192+uint64_t(fN+4096))/8192/(uint64_t(M)*P);
            got=uint32_t(khz);ok=(khz>s.value?khz-s.value:s.value-khz)<=s.mask;break;}
        case Op::VramWrite:io.vwr(s.vram,s.value);break;
        case Op::VramCopy:io.vcopy(s.vram,Blob(s.reg),s.mask);break;
        case Op::VramExpect:got=io.vrd(s.vram);ok=(got&s.mask)==s.value;break;
        case Op::VramPoll:{uint32_t t=0;
            while(((got=io.vrd(s.vram))&s.mask)!=s.value)if(io.expired(s.timeoutMs,t)){ok=false;break;}
            break;}
        case Op::VramPollAny:{uint32_t t=0;
            while(!((got=io.vrd(s.vram))&s.mask))if(io.expired(s.timeoutMs,t)){ok=false;break;}
            break;}
        case Op::PraminExpect:got=io.rd(kPramin+s.reg);ok=got==s.value;break;
        case Op::Fence:io.fence();break;
        case Op::Kick:io.fence();io.wr(s.reg,s.value);break;
        case Op::ExpectNot:got=io.rd(s.reg);ok=(got&s.mask)!=s.value;break;
        case Op::Log:got=io.rd(s.reg);break;
        case Op::Copy:got=io.rd(s.reg);io.wr(s.value,got);break;
        case Op::AssyArmDiff:
            for(uint32_t m=s.mask;m<s.value;m+=4){
                if(m==0x200)continue;                                     // UPDATE trigger
                if(s.reg!=kChidCore&&m==0x208)continue;                   // window GET_LINE (live)
                if(s.reg!=kChidCore&&m>=0x350&&m<0x370)continue;          // window: not in clc57e.h (SW methods)
                if(s.reg==kChidCore&&m>=0x220&&m<0x240)continue;          // core GET_RG_SCAN_LINE (live)
                const uint32_t a=io.rd(RegAssy(s.reg,m)),b=io.rd(RegArm(s.reg,m));
                if(a!=b){io.diff(m,a,b);
                    // x: 0 = fail unless rewritten, 1 = strict (known and not ours), 2 = log only
                    const bool bad=s.x==2?false:s.x?(Plan::Has(p.known,m)&&!Plan::Has(p.ours,m)):!p.Rewritten(m);
                    if(bad){ok=false;if(!got)got=m;}}
            }
            break;
        case Op::Pause:io.pause(s.what);break;
        case Op::Delay:io.udelay(s.value);break;
        case Op::AndNotIf:got=io.rd(s.reg);if((got&s.x)==s.x)io.wr(s.reg,got&~s.mask);break;
        case Op::CopyMask:
            if(!s.y||(io.rd(s.y)&s.z)==0){got=io.rd(s.reg)&s.mask;io.wr(s.value,(io.rd(s.value)&s.x)|got);}
            break;
        case Op::Call:ok=io.call(s.reg);break;
        case Op::SurfFill:io.fill(s.vram,s.value,s.mask,s.x,s.y,s.z);break;
        case Op::PraminRange:
            for(uint32_t k=0;k<s.value;++k)if(io.rd(kPramin+s.reg+k*4)!=io.blob(Blob(s.mask),s.x+k*4)){ok=false;got=k;break;}
            break;
        case Op::Dump:{uint32_t w[8];
            for(uint32_t k=0;k<s.value;k+=8){const uint32_t n=s.value-k<8?s.value-k:8;
                for(uint32_t j=0;j<n;++j)w[j]=io.rd(s.reg+(k+j)*4);io.dump(s.reg+k*4,w,n);}
            break;}
        }
        io.log(i,s,got,ok);
        if(!ok){if(!bestEffort)return i;if(firstFail==end)firstFail=i;}
    }
    return firstFail;
}
} // namespace brook::nvdisp
