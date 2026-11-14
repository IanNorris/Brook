#include "boot_timing.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace brook;

static void put16(std::vector<uint8_t>& v,size_t at,uint16_t x){v[at]=x&255;v[at+1]=x>>8;}
static void put32(std::vector<uint8_t>& v,size_t at,uint32_t x){for(int i=0;i<4;++i)v[at+i]=uint8_t(x>>(8*i));}
static void put64(std::vector<uint8_t>& v,size_t at,uint64_t x){for(int i=0;i<8;++i)v[at+i]=uint8_t(x>>(8*i));}

static std::vector<uint8_t> fpdt(uint64_t fbpt,bool withS3First) {
    std::vector<uint8_t> t(36+(withS3First?16:0)+16,0);
    memcpy(t.data(),"FPDT",4);put32(t,4,uint32_t(t.size()));
    size_t at=36;
    if(withS3First){put16(t,at,1);t[at+2]=16;t[at+3]=1;put64(t,at+8,0xdead000);at+=16;}
    put16(t,at,0);t[at+2]=16;t[at+3]=1;put64(t,at+8,fbpt);
    return t;
}
static std::vector<uint8_t> fbpt(uint64_t reset,uint64_t load,uint64_t start,uint64_t ebsIn,uint64_t ebsOut) {
    std::vector<uint8_t> t(8+48,0);
    memcpy(t.data(),"FBPT",4);put32(t,4,uint32_t(t.size()));
    put16(t,8,2);t[10]=48;t[11]=2;
    put64(t,16,reset);put64(t,24,load);put64(t,32,start);put64(t,40,ebsIn);put64(t,48,ebsOut);
    return t;
}

int main() {
    // Median calibration, tolerant of two outliers, bounded.
    assert(BootTimingMedianKhz({2194000,2194100,2193900,9000000,1000})==2194000);
    assert(BootTimingMedianKhz({0,0,0,1,1})==0);
    assert(BootTimingMedianKhz({30000000,30000000,30000000,1,1})==0);

    // TSC -> ms.
    uint64_t ms=0;
    assert(BootTscToMs(1000,1000+2194000ull*1500,2194000,ms) && ms==1500);
    assert(!BootTscToMs(10,5,2194000,ms));
    assert(!BootTscToMs(0,5,0,ms));
    assert(BootTscToMs(0,~uint64_t(0),1000,ms) && ms==~uint64_t(0)/1000);

    // FPDT pointer record, with an S3 record before it.
    auto table=fpdt(0x7f000000,true);
    assert(BootFpdtFbptAddress(table.data(),uint32_t(table.size()))==0x7f000000);
    table=fpdt(0x1234000,false);
    assert(BootFpdtFbptAddress(table.data(),uint32_t(table.size()))==0x1234000);
    assert(BootFpdtFbptAddress(table.data(),uint32_t(table.size())-1)==0); // length mismatch
    table[36+2]=0; // zero record length must not loop
    assert(BootFpdtFbptAddress(table.data(),uint32_t(table.size()))==0);

    // FBPT basic boot record.
    FpdtBootRecord rec{};
    auto basic=fbpt(1000000,3000000000ull,3100000000ull,4000000000ull,4001000000ull);
    assert(BootParseFbpt(basic.data(),uint32_t(basic.size()),rec) && rec.valid);
    assert(rec.startImageStart==3100000000ull && rec.exitBootServicesExit==4001000000ull);
    assert(!BootParseFbpt(basic.data(),uint32_t(basic.size())-1,rec)); // truncated
    auto zero=fbpt(0,0,0,0,0);
    assert(!BootParseFbpt(zero.data(),uint32_t(zero.size()),rec));
    auto reversed=fbpt(0,1,5000,4000,4500);
    assert(!BootParseFbpt(reversed.data(),uint32_t(reversed.size()),rec));

    const uint32_t khz=2000000; // 2 GHz: 2,000,000 ticks per ms
    auto T=[&](uint64_t msValue){return msValue*khz;};

    // New loader, TSC-only firmware time, wait ran.
    BootTimingInputs in{};
    in.protocolVersion=5;in.loaderTimingPresent=true;in.tscKhz=khz;
    in.loader.flags=BootTimingValid|BootTimingWaitRan;in.loader.waitConfiguredMs=500;
    in.loader.loaderEntryTsc=T(3200);in.loader.waitStartTsc=T(3300);in.loader.waitEndTsc=T(3800);
    in.loader.exitBootServicesTsc=T(3990);in.loader.kernelJumpTsc=T(3999);
    in.kernelEntryTsc=T(4000);in.desktopReadyTsc=T(8100);
    auto s=BootTimingCompute(in);
    assert(s.firmware.valid && s.firmware.ms==3200 && !strcmp(s.firmwareSource,"tsc"));
    assert(s.wait.valid && s.wait.ms==500);
    assert(s.loader.valid && s.loader.ms==300);
    assert(s.kernel.valid && s.kernel.ms==4100);
    assert(s.total.valid && s.total.ms==8100);
    char row[128];
    BootTimingRow(row,sizeof(row),s,false);
    assert(!strcmp(row,"Firmware 3.2 s | Menu 0.5 s | Loader 0.3 s | Kernel 4.1 s"));
    BootTimingRow(row,sizeof(row),s,true);
    assert(!strcmp(row,"Firmware 3.2 s | Menu 0.5 s | Loader+kernel 4.4 s"));
    BootTimingHeadline(row,sizeof(row),s);
    assert(!strcmp(row,"Desktop ready in 8.10 s from power-on"));
    char log[160];
    static_assert(sizeof(BootTiming)==56);
    BootTimingLogLine(log,sizeof(log),s,in);
    assert(!strcmp(log,"BOOT_TIMING firmware_ms=3200 src=tsc wait_ms=500 loader_ms=300 kernel_ms=4100 total_ms=8100 tsc_khz=2000000 proto=5 ext=1"));
    BootTimingDetailLine(log,sizeof(log),s,in);
    assert(!strcmp(log,"BOOT_TIMING_DETAIL tsc_fw_ms=3200 fpdt_fw_ms=na fpdt_reset_ms=na fpdt_ebs_ms=na flags=0x3 wait_cfg_ms=500 wait_keys=0 last_key=0x00000000"));

    // FPDT preferred when present.
    in.fpdt={true,1000000,2900000000ull,2950000000ull,3900000000ull,3901000000ull};
    s=BootTimingCompute(in);
    assert(s.firmware.ms==2950 && !strcmp(s.firmwareSource,"fpdt") && s.tscFirmware.ms==3200);
    in.fpdt={};

    // Menu disabled: zero wait, loader still measured.
    in.loader.flags=BootTimingValid;in.loader.waitStartTsc=in.loader.waitEndTsc=0;
    s=BootTimingCompute(in);
    assert(s.wait.valid && s.wait.ms==0 && s.loader.ms==800);

    // Implausible TSC-since-reset (warm start, no reset): firmware n/a.
    BootTimingInputs warm=in;
    warm.loader.loaderEntryTsc=T(20ull*60*1000);warm.kernelEntryTsc=T(20ull*60*1000+500);warm.desktopReadyTsc=warm.kernelEntryTsc+T(4000);
    s=BootTimingCompute(warm);
    assert(!s.firmware.valid && s.loader.valid && s.kernel.ms==4000 && !s.total.valid);
    BootTimingRow(row,sizeof(row),s,false);
    assert(!strcmp(row,"Firmware n/a | Menu 0.0 s | Loader 0.5 s | Kernel 4.0 s"));
    BootTimingHeadline(row,sizeof(row),s);
    assert(!strcmp(row,"Desktop ready in 4.00 s from kernel start"));

    // Handoff marker: the loader stays at version 5 and announces the timing
    // block through `extensions`; an older loader zero-initialises it.
    static_assert(BootProtocolVersion==5, "existing kernels accept only version 5 handoffs");
    static_assert(BootProtocolV5Size==__builtin_offsetof(BootProtocol,timing));
    {
        BootProtocol fresh{};fresh.magic=BootProtocolMagic;fresh.version=BootProtocolVersion;
        fresh.timing.flags=BootTimingValid;fresh.timing.loaderEntryTsc=123; // stale stack bytes
        assert(!BootHasTiming(&fresh));            // older loader: marker 0
        fresh.extensions=BootExtensionTimingMagic;
        assert(BootHasTiming(&fresh));             // new loader
        fresh.version=4;assert(!BootHasTiming(&fresh));
        assert(!BootHasTiming(nullptr));
    }
    // Inputs with the marker absent but a non-zero `loader` block (it is only
    // copied when present, but the computation must not trust it either way).
    {
        BootTimingInputs noMarker=in;noMarker.loaderTimingPresent=false;
        s=BootTimingCompute(noMarker);
        assert(!s.loaderStamps && !s.firmware.valid && !s.wait.valid && !s.loader.valid);
    }

    // Older loader (no extension): no stamps read even if memory past the struct is garbage.
    BootTimingInputs old{};
    old.protocolVersion=5;old.tscKhz=khz;old.kernelEntryTsc=T(4000);old.desktopReadyTsc=T(9000);
    s=BootTimingCompute(old);
    assert(!s.loaderStamps && !s.firmware.valid && !s.wait.valid && !s.loader.valid && s.kernel.ms==5000);
    BootTimingRow(row,sizeof(row),s,false);
    assert(!strcmp(row,"Firmware n/a | Menu n/a | Loader n/a | Kernel 5.0 s"));
    BootTimingRow(row,sizeof(row),s,true);
    assert(!strcmp(row,"Firmware n/a | Menu n/a | Kernel 5.0 s"));
    BootTimingLogLine(log,sizeof(log),s,old);
    assert(!strcmp(log,"BOOT_TIMING firmware_ms=na src=none wait_ms=na loader_ms=na kernel_ms=5000 total_ms=na tsc_khz=2000000 proto=5 ext=0"));

    // Unknown TSC rate: kernel falls back to the kernel clock.
    old.tscKhz=0;old.fallbackKernelValid=true;old.fallbackKernelMs=3456;
    s=BootTimingCompute(old);
    assert(s.kernel.valid && s.kernel.ms==3456);

    // Stamps after kernel entry (TSC not shared/reset in between) are rejected.
    BootTimingInputs bad=in;bad.loader.loaderEntryTsc=bad.kernelEntryTsc+1;
    s=BootTimingCompute(bad);
    assert(!s.loaderStamps && !s.firmware.valid && !s.loader.valid);

    // Rounding and truncation.
    BootText t(row,8);t.Add("0123456789");assert(!strcmp(row,"0123456"));
    BootText r(row,sizeof(row));r.Seconds(1949,1);assert(!strcmp(row,"1.9"));
    BootText r2(row,sizeof(row));r2.Seconds(1950,1);assert(!strcmp(row,"2.0"));
    BootText r3(row,sizeof(row));r3.Seconds(1005,2);assert(!strcmp(row,"1.01"));
    return 0;
}
