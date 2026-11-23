#include "../../kernel/src/hw_inventory_format.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using namespace brook;

// Read-only fake config space: the formatter only ever receives a reader.
struct Space {
    uint32_t d[1024]{};unsigned reads=0,extendedReads=0;
    uint32_t operator()(uint16_t offset){assert(!(offset&3) && offset<4096);++reads;if(offset>=0x100)++extendedReads;return d[offset/4];}
};
static std::string Format(Space& s,bool extended,uint8_t bus=1,uint8_t dev=0,uint8_t fn=0) {
    static char buffer[16384];HwText t(buffer,sizeof(buffer));
    HwFormatPciFunction(t,bus,dev,fn,[&](uint16_t o){return s(o);},extended);
    assert(!t.overflow());return std::string(t.data(),t.length());
}
static bool Has(const std::string& text,const char* needle){return text.find(needle)!=std::string::npos;}

int main() {
    // RTX 3080-like endpoint: 32-bit BAR0, 64-bit prefetchable BAR1 and BAR3, I/O BAR5.
    Space gpu;
    gpu.d[0]=0x220610de;gpu.d[1]=0x00100007;gpu.d[2]=0x030000a1;gpu.d[3]=0x00800000;
    gpu.d[4]=0xf6000000;gpu.d[5]=0xe000000c;gpu.d[6]=0;gpu.d[7]=0xf000000c;gpu.d[8]=0;gpu.d[9]=0xe001;
    gpu.d[0x2c/4]=0x147e1043;gpu.d[0x34/4]=0x60;gpu.d[0x3c/4]=0x000001ff;
    gpu.d[0x60/4]=0x00036801;gpu.d[0x64/4]=0x00000008;           // PM D0, next 0x68
    gpu.d[0x68/4]=0x01817805;                                     // MSI 64-bit, maskable(0x100) count bits, next 0x78
    gpu.d[0x78/4]=0x00020010;gpu.d[0x7c/4]=0x00000001;gpu.d[0x80/4]=0x00002020;
    gpu.d[0x84/4]=0x00000104;gpu.d[0x88/4]=0x01040000;            // link cap gen4 x16, status gen4 x16
    gpu.d[0x100/4]=0x2501000b;gpu.d[0x104/4]=0x02810001;          // VSEC -> 0x250
    gpu.d[0x250/4]=0x00010015;gpu.d[0x254/4]=0x00003ff0;gpu.d[0x258/4]=0x00000821; // ReBAR bar1 current 256MB, sizes 1MB..512MB, 1 bar
    auto text=Format(gpu,true);
    assert(Has(text,"pci 01:00.0 10de:2206 rev a1 class 030000 vga hdr 80 subsys 1043:147e cmd 0007 sts 0010"));
    assert(Has(text,"  bar0 mem32 nonpref base 0xf6000000 align_max 32MB\n"));
    assert(Has(text,"  bar1 mem64 pref base 0x00000000e0000000 align_max 512MB\n"));
    assert(Has(text,"  bar3 mem64 pref base 0x00000000f0000000 align_max 256MB\n"));
    assert(Has(text,"  bar5 io base 0xe000\n"));
    assert(!Has(text,"  bar2") && !Has(text,"  bar4"));
    assert(Has(text,"cap 60 pm id 01 ver 3 state D0"));
    assert(Has(text,"cap 68 msi id 05 64bit maskable vectors 1/1 enabled"));
    assert(Has(text,"cap 78 pcie id 10 v2 endpoint mps_cap 256 mps 256 mrrs 512 link_cap gen4 x16 link_sta gen4 x16"));
    assert(Has(text,"ecap 100 vsec id 000b v1 vsec_id 0001 rev 1 len 40"));
    assert(Has(text,"ecap 250 rebar id 0015 v1 bars 1\n    rebar bar1 current 256MB supported_mask 0x3ff range 1MB..512MB\n"));
    const unsigned all=gpu.reads;(void)all;
    Space legacy=gpu;legacy.reads=legacy.extendedReads=0;auto narrow=Format(legacy,false);
    assert(legacy.extendedReads==0 && !Has(narrow,"ecap"));

    // MSI-X endpoint (audio function) with table/PBA pointers only.
    Space audio;audio.d[0]=0x1aef10de;audio.d[1]=0x00100006;audio.d[2]=0x040300a1;audio.d[3]=0;
    audio.d[4]=0xf7080000;audio.d[0x34/4]=0x40;audio.d[0x40/4]=0x80070011;audio.d[0x44/4]=0x00002000;audio.d[0x48/4]=0x00003000;
    text=Format(audio,false,1,0,1);
    assert(Has(text,"pci 01:00.1 10de:1aef rev a1 class 040300 hda-audio hdr 00"));
    assert(Has(text,"cap 40 msix id 11 table_size 8 table bar0+0x2000 pba bar0+0x3000 enabled"));
    assert(Has(text,"  bar0 mem32 nonpref base 0xf7080000 align_max 512KB\n"));
    assert(!Has(text,"  bar1") && !Has(text,"unassigned"));

    // Root port bridge windows, including 64-bit prefetchable window and disabled I/O.
    Space bridge;bridge.d[0]=0x19018086;bridge.d[1]=0x00100007;bridge.d[2]=0x06040007;bridge.d[3]=0x00810000;
    bridge.d[0x18/4]=0x00010100;bridge.d[0x1c/4]=0x000000f0;bridge.d[0x20/4]=0xf700f600;
    bridge.d[0x24/4]=0xf1f1e001;bridge.d[0x28/4]=0;bridge.d[0x2c/4]=0;bridge.d[0x3c/4]=0x001800ff;
    text=Format(bridge,false,0,1,0);
    assert(Has(text,"pci 00:01.0 8086:1901 rev 07 class 060400 pci-bridge hdr 81 cmd 0007"));
    assert(!Has(text,"subsys"));
    assert(Has(text,"  bridge pri 00 sec 01 sub 01\n"));
    assert(Has(text,"  window io disabled\n"));
    assert(Has(text,"  window mem 0xf6000000-0xf70fffff (17MB)\n"));
    assert(Has(text,"  window pref64 0x00000000e0000000-0x00000000f1ffffff (288MB)\n"));
    assert(Has(text,"  bridge control 0018\n"));

    // Capability loops terminate on cycles and bad extended headers.
    Space loop;loop.d[0]=0x12348086;loop.d[1]=0x00100000;loop.d[0x34/4]=0x40;loop.d[0x40/4]=0x00004009;
    loop.d[0x100/4]=0x10000001;
    text=Format(loop,true);
    unsigned caps=0;for(size_t at=0;(at=text.find("cap 40 vendor",at))!=std::string::npos;++at)++caps;
    assert(caps==48);
    assert(Has(text,"ecap 100 aer id 0001 v0\n"));

    // MCFG parse and DMAR summary.
    uint8_t mcfg[60]{};mcfg[4]=60;
    const uint64_t base=0xe0000000;for(int i=0;i<8;++i)mcfg[44+i]=uint8_t(base>>(8*i));mcfg[54]=0;mcfg[55]=0x3f;
    HwMcfgEntry entries[4];assert(HwParseMcfg(mcfg,sizeof(mcfg),entries,4)==1 && entries[0].base==base && entries[0].endBus==0x3f);
    assert(HwParseMcfg(mcfg,43,entries,4)==0);
    std::vector<uint8_t> dmar(48);dmar[36]=38;dmar[37]=5;
    auto put=[&](std::initializer_list<uint8_t> bytes){dmar.insert(dmar.end(),bytes);};
    // DRHD for 00:02.0 at 0xfed90000
    put({0,0,24,0, 0,0,0,0, 0x00,0x00,0xd9,0xfe,0,0,0,0, 1,8,0,0,0,0,2,0});
    // DRHD include-all at 0xfed91000
    put({0,0,16,0, 1,0,0,0, 0x00,0x10,0xd9,0xfe,0,0,0,0});
    // RMRR for 00:14.0
    put({1,0,32,0, 0,0,0,0, 0,0,0x80,0x3e,0,0,0,0, 0xff,0xff,0x9f,0x3e,0,0,0,0, 1,8,0,0,0,0,0x14,0});
    static char buffer[4096];HwText t(buffer,sizeof(buffer));HwFormatDmar(t,dmar.data(),uint32_t(dmar.size()));
    std::string d(t.data());
    assert(Has(d,"dmar len 120 host_addr_width 39 flags 0x05 intr_remap dma_ctrl_opt_in\n"));
    assert(Has(d,"dmar drhd flags 0x00 segment 0 base 0xfed90000 scope:endpoint(id 0 bus 00 path 02.0)\n"));
    assert(Has(d,"dmar drhd flags 0x01 include_pci_all segment 0 base 0xfed91000\n"));
    assert(Has(d,"dmar rmrr segment 0 range 0x3e800000-0x3e9fffff scope:endpoint(id 0 bus 00 path 14.0)\n"));
    dmar[50]=200;HwText bad(buffer,sizeof(buffer));HwFormatDmar(bad,dmar.data(),uint32_t(dmar.size()));
    assert(Has(bad.data(),"dmar malformed at 48"));

    // Bounded output never overruns.
    char tiny[8];HwText small(tiny,sizeof(tiny));small.str("0123456789");
    assert(small.overflow() && small.length()==7 && !std::strcmp(tiny,"0123456"));
    char sz[64];HwText sizes(sz,sizeof(sz));sizes.size(0x400000000ull);sizes.ch(' ');sizes.size(4096);sizes.ch(' ');sizes.size(1000);
    assert(!std::strcmp(sz,"16GB 4KB 1000B"));
    return 0;
}
