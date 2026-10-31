#pragma once
#include <stdint.h>
#include "../../shared/update/payload.h"

namespace brook {
// This is an opt-in, digest-bound snapshot of one reviewed Dell DSDT layout,
// not an AML evaluator. Io must validate complete ranges against ACPI NVS and
// read NVS only. No reported address is mapped or used for device I/O here.
struct DellAudioFirmwareSnapshot {
    uint16_t osys=0,pchGeneration=0;
    uint8_t codecSelection=0,i2cMode=0,i2cIrq=0;
    uint32_t dspBase0=0,dspBase1=0,dspIrq=0,i2cBase0=0,i2cBase1=0;
};
enum class DellAudioFirmwareResult {Ok,Invalid,HashMismatch,Layout,Nvs,Unstable};
inline bool DellAudioFindUnique(const uint8_t* data,uint32_t bytes,const uint8_t* prefix,uint32_t n,uint32_t tail,uint32_t& found) {
    if(!data || n>bytes || tail>bytes-n)return false;
    unsigned matches=0;
    for(uint32_t i=0;i<=bytes-n-tail;++i) {
        uint32_t j=0;while(j<n && data[i+j]==prefix[j])++j;
        if(j==n){found=i+n;if(++matches>1)return false;}
    }
    return matches==1;
}
template<class Io> DellAudioFirmwareResult DellAudioFirmwareCapture(const uint8_t* dsdt,uint32_t size,
        const uint8_t expected[32],Io& io,DellAudioFirmwareSnapshot& output) {
    if(!dsdt || !expected || size<36 || size>1024*1024 || dsdt[0]!='D' || dsdt[1]!='S' || dsdt[2]!='D' || dsdt[3]!='T' ||
       BrookUpdateGet32(dsdt+4)!=size)return DellAudioFirmwareResult::Invalid;
    uint8_t digest[32];BrookUpdateHash(dsdt,size,digest);
    if(!BrookUpdateEqual(digest,expected,32))return DellAudioFirmwareResult::HashMismatch;
    static constexpr uint8_t pnbase[]={0x08,'P','N','V','B',0x0c};
    static constexpr uint8_t pnlen[]={0x08,'P','N','V','L',0x0b};
    static constexpr uint8_t pnregion[]={0x5b,0x80,'P','N','V','A',0x00,'P','N','V','B','P','N','V','L'};
    static constexpr uint8_t gnregion[]={0x5b,0x80,'G','N','V','S',0x00,0x0c};
    uint32_t b=0,l=0,r=0,g=0;
    if(!DellAudioFindUnique(dsdt,size,pnbase,sizeof(pnbase),4,b) ||
       !DellAudioFindUnique(dsdt,size,pnlen,sizeof(pnlen),2,l) ||
       !DellAudioFindUnique(dsdt,size,pnregion,sizeof(pnregion),0,r) ||
       !DellAudioFindUnique(dsdt,size,gnregion,sizeof(gnregion),7,g) ||
       dsdt[g+4]!=0x0b)return DellAudioFirmwareResult::Layout;
    const uint64_t pn=BrookUpdateGet32(dsdt+b),gn=BrookUpdateGet32(dsdt+g);
    const uint32_t plen=uint32_t(dsdt[l])|(uint32_t(dsdt[l+1])<<8);
    const uint32_t glen=uint32_t(dsdt[g+5])|(uint32_t(dsdt[g+6])<<8);
    if(!pn || !gn || plen!=0xe4 || glen!=0x35b || !io.containsNvs(pn,plen) || !io.containsNvs(gn,glen))
        return DellAudioFirmwareResult::Nvs;
    auto sample=[&](DellAudioFirmwareSnapshot& s)->bool {
        uint8_t a[2],p[2],c[1],m[1],irq[1],d0[4],d1[4],di[4],i0[4],i1[4];
        if(!io.readNvs(gn,a,2) || !io.readNvs(gn+284,c,1) || !io.readNvs(pn+6,p,2) ||
           !io.readNvs(pn+44,d0,4) || !io.readNvs(pn+48,d1,4) || !io.readNvs(pn+52,di,4) ||
           !io.readNvs(pn+114,m,1) || !io.readNvs(pn+122,irq,1) ||
           !io.readNvs(pn+133,i0,4) || !io.readNvs(pn+165,i1,4))return false;
        s.osys=uint16_t(a[0]|uint16_t(a[1])<<8);s.codecSelection=c[0];
        s.pchGeneration=uint16_t(p[0]|uint16_t(p[1])<<8);s.i2cMode=m[0];s.i2cIrq=irq[0];
        s.dspBase0=BrookUpdateGet32(d0);s.dspBase1=BrookUpdateGet32(d1);s.dspIrq=BrookUpdateGet32(di);
        s.i2cBase0=BrookUpdateGet32(i0);s.i2cBase1=BrookUpdateGet32(i1);return true;
    };
    DellAudioFirmwareSnapshot a{},z{};
    if(!sample(a) || !sample(z))return DellAudioFirmwareResult::Nvs;
    if(a.osys!=z.osys || a.pchGeneration!=z.pchGeneration || a.codecSelection!=z.codecSelection ||
       a.i2cMode!=z.i2cMode || a.i2cIrq!=z.i2cIrq || a.dspBase0!=z.dspBase0 || a.dspBase1!=z.dspBase1 ||
       a.dspIrq!=z.dspIrq || a.i2cBase0!=z.i2cBase0 || a.i2cBase1!=z.i2cBase1)return DellAudioFirmwareResult::Unstable;
    output=a;return DellAudioFirmwareResult::Ok;
}
}
