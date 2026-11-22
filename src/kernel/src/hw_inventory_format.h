#pragma once
#include <stdint.h>

// Read-only hardware inventory formatting. Pure functions over caller-supplied
// readers so the exact output can be host-tested. Nothing here writes device
// registers: PCI BARs are decoded from their current values and never sized
// (sizing would require writing all-ones), VPD and MSI-X tables are not read,
// and ACPI table bodies other than MCFG/DMAR are never examined.
namespace brook {

class HwText {
    char* buffer_;uint32_t capacity_;uint32_t length_=0;bool overflow_=false;
public:
    HwText(char* buffer,uint32_t capacity):buffer_(buffer),capacity_(capacity){if(capacity_)buffer_[0]=0;}
    void ch(char c) {
        if(!capacity_ || length_+1>=capacity_){overflow_=true;return;}
        buffer_[length_++]=c;buffer_[length_]=0;
    }
    void str(const char* s){while(s && *s)ch(*s++);}
    // Printable ASCII only; anything else becomes '.'.
    void raw(const char* s,uint32_t n){for(uint32_t i=0;i<n;++i){const char c=s[i];ch(c>=32 && c<127?c:'.');}}
    void hex(uint64_t v,unsigned digits=0) {
        char d[16];unsigned n=0;
        do{const unsigned x=unsigned(v&15);d[n++]=char(x<10?'0'+x:'a'+x-10);v>>=4;}while(v && n<16);
        while(n<digits && n<16)d[n++]='0';
        while(n)ch(d[--n]);
    }
    void hx(uint64_t v,unsigned digits=0){str("0x");hex(v,digits);}
    void dec(uint64_t v){char d[20];unsigned n=0;do{d[n++]=char('0'+v%10);v/=10;}while(v);while(n)ch(d[--n]);}
    void size(uint64_t bytes) {
        const char* unit[]={"B","KB","MB","GB","TB"};unsigned u=0;
        while(u<4 && bytes>=1024 && !(bytes&1023)){bytes>>=10;++u;}
        dec(bytes);str(unit[u]);
    }
    void end(){ch('\n');}
    uint32_t length() const {return length_;}
    bool overflow() const {return overflow_;}
    const char* data() const {return buffer_;}
};

inline const char* HwPciClassName(uint8_t cls,uint8_t sub) {
    switch(cls) {
    case 0x01: return sub==0x06?"sata":sub==0x08?"nvme":sub==0x04?"raid":"storage";
    case 0x02: return "network";
    case 0x03: return sub==0x00?"vga":sub==0x02?"3d":"display";
    case 0x04: return sub==0x03?"hda-audio":"multimedia";
    case 0x05: return "memory";
    case 0x06: return sub==0x00?"host-bridge":sub==0x01?"isa-bridge":sub==0x04?"pci-bridge":"bridge";
    case 0x07: return "comm";
    case 0x08: return "system";
    case 0x0c: return sub==0x03?"usb":sub==0x05?"smbus":sub==0x80?"serial-bus":"serial";
    case 0x0d: return "wireless";
    case 0x10: return "crypto";
    case 0x11: return "signal";
    case 0x12: return "accel";
    case 0x13: return "instr";
    default: return "other";
    }
}

inline const char* HwPcieTypeName(unsigned type) {
    switch(type){case 0:return "endpoint";case 1:return "legacy-endpoint";case 4:return "root-port";
        case 5:return "upstream";case 6:return "downstream";case 7:return "pcie-to-pci";
        case 8:return "pci-to-pcie";case 9:return "rc-endpoint";case 10:return "rc-event";default:return "reserved";}
}

inline const char* HwExtCapName(unsigned id) {
    switch(id){case 0x01:return "aer";case 0x02:return "vc";case 0x03:return "dsn";case 0x04:return "power-budget";
        case 0x05:return "rclink";case 0x0b:return "vsec";case 0x0d:return "acs";case 0x0e:return "ari";
        case 0x0f:return "ats";case 0x10:return "sriov";case 0x13:return "page-request";case 0x15:return "rebar";
        case 0x16:return "dpa";case 0x17:return "tph";case 0x18:return "ltr";case 0x19:return "secondary-pcie";
        case 0x1b:return "pasid";case 0x1d:return "dpc";case 0x1e:return "l1ss";case 0x1f:return "ptm";
        case 0x23:return "dvsec";case 0x24:return "vf-rebar";case 0x25:return "dlf";case 0x26:return "phy-16gt";
        case 0x27:return "lane-margin";case 0x2a:return "phy-32gt";default:return "ext";}
}

inline const char* HwCapName(unsigned id) {
    switch(id){case 0x01:return "pm";case 0x03:return "vpd";case 0x05:return "msi";case 0x09:return "vendor";
        case 0x0a:return "debug";case 0x0d:return "ssvid";case 0x10:return "pcie";case 0x11:return "msix";
        case 0x12:return "sata";case 0x13:return "af";case 0x14:return "ea";default:return "cap";}
}

// Read must provide uint32_t operator()(uint16_t dwordAlignedOffset). Offsets
// at or above 0x100 are requested only when extended is true.
template<class Read>
void HwFormatPciFunction(HwText& t,uint8_t bus,uint8_t dev,uint8_t fn,Read read,bool extended) {
    const uint32_t id=read(0x00),cmdsts=read(0x04),cls=read(0x08),misc=read(0x0c);
    const uint8_t header=uint8_t(misc>>16)&0x7f;
    t.str("pci ");t.hex(bus,2);t.ch(':');t.hex(dev,2);t.ch('.');t.hex(fn,1);
    t.ch(' ');t.hex(id&0xffff,4);t.ch(':');t.hex(id>>16,4);
    t.str(" rev ");t.hex(cls&0xff,2);t.str(" class ");t.hex(cls>>8,6);
    t.ch(' ');t.str(HwPciClassName(uint8_t(cls>>24),uint8_t(cls>>16)));
    t.str(" hdr ");t.hex(uint8_t(misc>>16),2);
    if(header==0){const uint32_t s=read(0x2c);t.str(" subsys ");t.hex(s&0xffff,4);t.ch(':');t.hex(s>>16,4);}
    t.str(" cmd ");t.hex(cmdsts&0xffff,4);t.str(" sts ");t.hex(cmdsts>>16,4);
    const uint32_t irq=read(0x3c);
    t.str(" intpin ");t.dec((irq>>8)&0xff);t.str(" intline ");t.dec(irq&0xff);
    t.end();

    const unsigned bars=header==0?6:header==1?2:0;
    for(unsigned i=0;i<bars;++i) {
        const uint32_t lo=read(uint16_t(0x10+i*4));
        if(!lo)continue; // unimplemented or unassigned 32-bit BAR
        t.str("  bar");t.dec(i);t.ch(' ');
        if(lo&1) {t.str("io base ");t.hx(lo&~3u,4);t.end();continue;}
        const unsigned type=(lo>>1)&3;const bool pref=(lo&8)!=0;
        uint64_t base=lo&~0xfull;
        if(type==2 && i+1<bars){base|=uint64_t(read(uint16_t(0x10+(i+1)*4)))<<32;}
        t.str(type==2?"mem64":type==1?"mem20":"mem32");t.str(pref?" pref":" nonpref");
        t.str(" base ");t.hx(base,type==2?16:8);
        if(base) {
            // Alignment bounds the size from above (BARs are naturally aligned).
            uint64_t align=base&(~base+1);t.str(" align_max ");t.size(align);
        } else t.str(" unassigned");
        t.end();
        if(type==2)++i;
    }
    if(header==0) {
        const uint32_t rom=read(0x30);
        if(rom&~0x7ffu){t.str("  rom base ");t.hx(rom&~0x7ffu,8);t.str(rom&1?" enabled":" disabled");t.end();}
    }
    if(header==1) {
        const uint32_t busses=read(0x18);
        t.str("  bridge pri ");t.hex(busses&0xff,2);t.str(" sec ");t.hex((busses>>8)&0xff,2);
        t.str(" sub ");t.hex((busses>>16)&0xff,2);t.end();
        const uint32_t io=read(0x1c),ioUpper=read(0x30);
        uint32_t ioBase=(io&0xf0)<<8,ioLimit=((io>>8)&0xf0)<<8|0xfff;
        if((io&0xf)==1){ioBase|=(ioUpper&0xffff)<<16;ioLimit|=(ioUpper>>16)<<16;}
        t.str("  window io ");
        if(ioBase<=ioLimit){t.hx(ioBase,4);t.ch('-');t.hx(ioLimit,4);}else t.str("disabled");
        t.end();
        const uint32_t mem=read(0x20);
        const uint32_t memBase=(mem&0xfff0)<<16,memLimit=((mem>>16)&0xfff0)<<16|0xfffff;
        t.str("  window mem ");
        if(memBase<=memLimit){t.hx(memBase,8);t.ch('-');t.hx(memLimit,8);t.str(" (");t.size(uint64_t(memLimit)-memBase+1);t.ch(')');}
        else t.str("disabled");
        t.end();
        const uint32_t pref=read(0x24);
        uint64_t prefBase=uint64_t(pref&0xfff0)<<16,prefLimit=uint64_t((pref>>16)&0xfff0)<<16|0xfffff;
        const bool pref64=(pref&0xf)==1;
        if(pref64){prefBase|=uint64_t(read(0x28))<<32;prefLimit|=uint64_t(read(0x2c))<<32;}
        t.str("  window pref");t.str(pref64?"64 ":"32 ");
        if(prefBase<=prefLimit){t.hx(prefBase,16);t.ch('-');t.hx(prefLimit,16);t.str(" (");t.size(prefLimit-prefBase+1);t.ch(')');}
        else t.str("disabled");
        t.end();
        t.str("  bridge control ");t.hex(read(0x3c)>>16,4);t.end();
    }

    if(cmdsts&(1u<<20)) {
        uint8_t ptr=uint8_t(read(header==2?0x14:0x34)&0xfc);
        for(unsigned n=0;ptr>=0x40 && n<48;++n) {
            const uint32_t h=read(ptr);const unsigned capId=h&0xff;const uint16_t control=uint16_t(h>>16);
            t.str("  cap ");t.hex(ptr,2);t.ch(' ');t.str(HwCapName(capId));t.str(" id ");t.hex(capId,2);
            if(capId==0x01){t.str(" ver ");t.dec(control&7);t.str(" state D");t.dec(read(uint16_t(ptr+4))&3);}
            else if(capId==0x05) {
                t.str(control&(1u<<7)?" 64bit":" 32bit");t.str(control&(1u<<8)?" maskable":"");
                t.str(" vectors ");t.dec(1u<<((control>>4)&7));t.ch('/');t.dec(1u<<((control>>1)&7));
                t.str(control&1?" enabled":" disabled");
            } else if(capId==0x11) {
                const uint32_t table=read(uint16_t(ptr+4)),pba=read(uint16_t(ptr+8));
                t.str(" table_size ");t.dec((control&0x7ff)+1u);
                t.str(" table bar");t.dec(table&7);t.str("+");t.hx(table&~7u);
                t.str(" pba bar");t.dec(pba&7);t.str("+");t.hx(pba&~7u);
                t.str(control&(1u<<15)?" enabled":" disabled");if(control&(1u<<14))t.str(" fn_masked");
            } else if(capId==0x10) {
                const unsigned type=(control>>4)&15;
                t.str(" v");t.dec(control&15);t.ch(' ');t.str(HwPcieTypeName(type));
                const uint32_t devCap=read(uint16_t(ptr+4)),devCtl=read(uint16_t(ptr+8));
                t.str(" mps_cap ");t.dec(128u<<(devCap&7));t.str(" mps ");t.dec(128u<<((devCtl>>5)&7));
                t.str(" mrrs ");t.dec(128u<<((devCtl>>12)&7));
                if(type!=9 && type!=10) {
                    const uint32_t linkCap=read(uint16_t(ptr+0x0c)),linkSta=read(uint16_t(ptr+0x10))>>16;
                    t.str(" link_cap gen");t.dec(linkCap&15);t.str(" x");t.dec((linkCap>>4)&63);
                    t.str(" link_sta gen");t.dec(linkSta&15);t.str(" x");t.dec((linkSta>>4)&63);
                    t.str(" aspm_cap ");t.dec((linkCap>>10)&3);
                }
                if(control&(1u<<8))t.str(" slot");
            } else if(capId==0x0d) {
                const uint32_t s=read(uint16_t(ptr+4));t.str(" subsys ");t.hex(s&0xffff,4);t.ch(':');t.hex(s>>16,4);
            }
            t.end();
            ptr=uint8_t((h>>8)&0xfc);
        }
    }

    if(!extended)return;
    uint16_t off=0x100;
    for(unsigned n=0;off>=0x100 && off<0x1000 && n<96;++n) {
        const uint32_t h=read(off);
        if(!h || h==0xffffffffu)break;
        const unsigned capId=h&0xffff;
        t.str("  ecap ");t.hex(off,3);t.ch(' ');t.str(HwExtCapName(capId));t.str(" id ");t.hex(capId,4);
        t.str(" v");t.dec((h>>16)&15);
        if(capId==0x15 || capId==0x24) {
            const unsigned count=(read(uint16_t(off+8))>>5)&7;
            t.str(" bars ");t.dec(count);t.end();
            for(unsigned i=0;i<count && i<6;++i) {
                const uint32_t cap=read(uint16_t(off+4+i*8)),ctl=read(uint16_t(off+8+i*8));
                // Supported sizes: cap bits 4..31 => 1MB..256TB; ctl bits 16..31 extend beyond.
                const uint64_t supported=uint64_t(cap>>4)|(uint64_t(ctl>>16)<<28);
                t.str("    rebar bar");t.dec(ctl&7);t.str(" current ");t.size(uint64_t(1)<<(20+((ctl>>8)&0x3f)));
                t.str(" supported_mask ");t.hx(supported);
                int lowBit=-1,highBit=-1;
                for(int b=0;b<44;++b)if(supported&(uint64_t(1)<<b)){if(lowBit<0)lowBit=b;highBit=b;}
                if(lowBit>=0){t.str(" range ");t.size(uint64_t(1)<<(20+lowBit));t.str("..");t.size(uint64_t(1)<<(20+highBit));}
                t.end();
            }
        } else if(capId==0x0d) {
            const uint32_t v=read(uint16_t(off+4));t.str(" cap ");t.hex(v&0xffff,4);t.str(" ctl ");t.hex(v>>16,4);t.end();
        } else if(capId==0x0b || capId==0x23) {
            const uint32_t v=read(uint16_t(off+4));t.str(" vsec_id ");t.hex(v&0xffff,4);t.str(" rev ");t.dec((v>>16)&15);
            t.str(" len ");t.dec(v>>20);t.end();
        } else if(capId==0x10) {
            const uint32_t total=read(uint16_t(off+0x0c));t.str(" initial_vfs ");t.dec(total&0xffff);
            t.str(" total_vfs ");t.dec(total>>16);t.end();
        } else t.end();
        const uint16_t next=uint16_t((h>>20)&0xffc);
        if(next<=off)break;
        off=next;
    }
}

struct HwMcfgEntry {uint64_t base;uint16_t segment;uint8_t startBus,endBus;};
// Parse at most capacity MCFG allocation entries from a copied table.
inline uint32_t HwParseMcfg(const uint8_t* table,uint32_t bytes,HwMcfgEntry* out,uint32_t capacity) {
    if(!table || bytes<44)return 0;
    uint32_t count=0;
    for(uint32_t o=44;o+16<=bytes && count<capacity;o+=16) {
        HwMcfgEntry e{};
        for(unsigned i=0;i<8;++i)e.base|=uint64_t(table[o+i])<<(8*i);
        e.segment=uint16_t(table[o+8]|table[o+9]<<8);e.startBus=table[o+10];e.endBus=table[o+11];
        if(e.endBus<e.startBus)continue;
        out[count++]=e;
    }
    return count;
}

inline uint64_t HwLe(const uint8_t* p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;}

inline void HwFormatDmarScopes(HwText& t,const uint8_t* p,uint32_t bytes) {
    for(uint32_t o=0;o+6<=bytes;) {
        const uint8_t type=p[o],length=p[o+1];
        if(length<6 || o+length>bytes)break;
        t.str(" scope:");
        t.str(type==1?"endpoint":type==2?"bridge":type==3?"ioapic":type==4?"hpet":type==5?"acpi-ns":"type?");
        t.ch('(');t.str("id ");t.dec(p[o+4]);t.str(" bus ");t.hex(p[o+5],2);
        for(uint32_t q=o+6;q+2<=uint32_t(o+length);q+=2){t.str(" path ");t.hex(p[q],2);t.ch('.');t.hex(p[q+1],1);}
        t.ch(')');o+=length;
    }
}

// DMAR: remapping hardware summary (DRHD, RMRR, ATSR, RHSA, ANDD, SATC).
inline void HwFormatDmar(HwText& t,const uint8_t* table,uint32_t bytes) {
    if(!table || bytes<48){t.str("dmar absent\n");return;}
    t.str("dmar len ");t.dec(bytes);t.str(" host_addr_width ");t.dec(table[36]+1u);
    t.str(" flags ");t.hx(table[37],2);
    if(table[37]&1)t.str(" intr_remap");if(table[37]&2)t.str(" x2apic_opt_out");if(table[37]&4)t.str(" dma_ctrl_opt_in");
    t.end();
    for(uint32_t o=48;o+4<=bytes;) {
        const uint16_t type=uint16_t(HwLe(table+o,2)),length=uint16_t(HwLe(table+o+2,2));
        if(length<4 || o+length>bytes){t.str("dmar malformed at ");t.dec(o);t.end();break;}
        const uint8_t* s=table+o;
        if(type==0 && length>=16) {
            t.str("dmar drhd flags ");t.hx(s[4],2);if(s[4]&1)t.str(" include_pci_all");
            t.str(" segment ");t.dec(HwLe(s+6,2));t.str(" base ");t.hx(HwLe(s+8,8),8);
            HwFormatDmarScopes(t,s+16,length-16u);
        } else if(type==1 && length>=24) {
            t.str("dmar rmrr segment ");t.dec(HwLe(s+6,2));t.str(" range ");t.hx(HwLe(s+8,8),8);t.ch('-');t.hx(HwLe(s+16,8),8);
            HwFormatDmarScopes(t,s+24,length-24u);
        } else if(type==2 && length>=8) {
            t.str("dmar atsr flags ");t.hx(s[4],2);t.str(" segment ");t.dec(HwLe(s+6,2));
            HwFormatDmarScopes(t,s+8,length-8u);
        } else if(type==3 && length>=20) {
            t.str("dmar rhsa base ");t.hx(HwLe(s+8,8),8);t.str(" proximity ");t.dec(HwLe(s+16,4));
        } else if(type==4 && length>=8) {
            t.str("dmar andd device ");t.dec(s[7]);t.str(" name ");t.raw(reinterpret_cast<const char*>(s+8),length-8u>32?32:length-8u);
        } else {
            t.str("dmar type ");t.dec(type);t.str(" len ");t.dec(length);
        }
        t.end();o+=length;
    }
}

inline const char* HwMemoryTypeName(uint32_t type) {
    static const char* names[]={"free","reserved","acpi-reclaim","acpi-nvs","mmio","unusable","kernel",
        "kernel-stack","boot-data","framebuffer","loader-code","initrd"};
    return type<sizeof(names)/sizeof(names[0])?names[type]:"type?";
}
}
