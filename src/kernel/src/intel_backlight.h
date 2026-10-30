#pragma once
#include <stdint.h>

namespace brook {
inline bool DisplayIdleSecondsParse(const char* text,uint32_t bytes,uint32_t& seconds){
    if(!text || !bytes || bytes>6)return false;if(text[bytes-1]=='\n')--bytes;if(!bytes)return false;
    uint32_t value=0;
    for(uint32_t i=0;i<bytes;++i){if(text[i]<'0'||text[i]>'9')return false;value=value*10+text[i]-'0';if(value>86400)return false;}
    seconds=value;return true;
}
// Broadwell/LPT register layout. Reference: Linux's MIT-licensed
// drivers/gpu/drm/i915/display/intel_backlight{,_regs.h}. Preserve firmware
// routing, frequency and polarity; this is not a modesetting/panel-link driver.
struct BacklightRegisters {
    uint32_t cpuControl=0,cpuDuty=0,pchControl=0,pchDuty=0;
};
struct BacklightIo {
    void* context=nullptr;
    uint32_t (*read)(void*,uint32_t)=nullptr;
    void (*write)(void*,uint32_t,uint32_t)=nullptr;
};
class IntelBacklight {
public:
    static constexpr uint32_t CpuControl=0x48250,CpuDuty=0x48254;
    static constexpr uint32_t PchControl=0xc8250,PchDuty=0xc8254;
    static constexpr uint32_t Enable=1u<<31,Override=1u<<30;
    BacklightRegisters original{};
    bool ready=false,cpuRoute=false,on=true,touched=false,uncertain=false;
    const char* reason="No supported backlight device";

    BacklightRegisters Read(const BacklightIo& io) const {
        if(!io.read)return {};
        return {io.read(io.context,CpuControl),io.read(io.context,CpuDuty),
                io.read(io.context,PchControl),io.read(io.context,PchDuty)};
    }
    bool Probe(const BacklightIo& io) {
        original=Read(io);ready=false;on=true;touched=false;uncertain=false;
        if(!io.read || !io.write){reason="Backlight access unavailable";return false;}
        const auto& r=original;
        if(r.cpuControl==0xffffffffu || r.cpuDuty==0xffffffffu ||
           r.pchControl==0xffffffffu || r.pchDuty==0xffffffffu){reason="Invalid backlight register read";return false;}
        if(!(r.pchControl&Enable)){reason="Firmware PCH PWM is not enabled";return false;}
        cpuRoute=!(r.pchControl&Override) && (r.cpuControl&Enable);
        if(!cpuRoute && !(r.pchControl&Override)){reason="Firmware PWM route is ambiguous";return false;}
        const uint32_t period=r.pchDuty>>16;
        const uint32_t duty=(cpuRoute?r.cpuDuty:r.pchDuty)&0xffff;
        if(!period || !duty || duty>period){reason="Invalid firmware PWM period or duty";return false;}
        ready=true;reason=cpuRoute?"Firmware CPU PWM":"Firmware PCH override PWM";return true;
    }
    bool Compatible(const BacklightRegisters& r) const {
        return ready && r.cpuControl==original.cpuControl && r.pchControl==original.pchControl &&
            (r.pchDuty&0xffff0000u)==(original.pchDuty&0xffff0000u) &&
            (!cpuRoute || (r.cpuDuty&0xffff0000u)==(original.cpuDuty&0xffff0000u));
    }
    bool SetOn(const BacklightIo& io,bool wantOn) {
        if(!ready)return false;
        const auto r=Read(io);
        if(!Compatible(r)){reason="Firmware PWM routing changed";return false;}
        const auto offset=cpuRoute?CpuDuty:PchDuty;
        const uint32_t live=cpuRoute?r.cpuDuty:r.pchDuty;
        const uint32_t saved=cpuRoute?original.cpuDuty:original.pchDuty;
        // Do not override an external writer or adopt a brighter new setting.
        if((!uncertain || !wantOn) && (live&0xffff)!=(on?(saved&0xffff):0u)){
            reason="Firmware PWM duty changed";return false;
        }
        if(wantOn==on && !uncertain)return true;
        const uint32_t value=wantOn?saved:(live&0xffff0000u);
        // Publish before the write so panic can restore even if this CPU is
        // halted between the MMIO operation and its readback/state update.
        __atomic_store_n(&touched,true,__ATOMIC_SEQ_CST);
        io.write(io.context,offset,value);
        if(io.read(io.context,offset)!=value){uncertain=true;reason="PWM write readback failed";return false;}
        on=wantOn;uncertain=false;
        reason=cpuRoute?"Firmware CPU PWM":"Firmware PCH override PWM";return true;
    }
    // Other CPUs are stopped. Immutable original state only; no normal lock,
    // allocator, page-table operation, delay or panel sequencing is permitted.
    void PanicRestore(const BacklightIo& io) const {
        if(!ready || !__atomic_load_n(&touched,__ATOMIC_ACQUIRE) || !Compatible(Read(io)))return;
        const auto offset=cpuRoute?CpuDuty:PchDuty;
        io.write(io.context,offset,cpuRoute?original.cpuDuty:original.pchDuty);
        (void)io.read(io.context,offset);
    }
};

// Serialized by the display-power lock. Input producers only publish their
// existing sequence; no display MMIO is performed from an input IRQ.
struct DisplayIdlePolicy {
    uint64_t lastInput=0,lastActivityMs=0,restoreAtMs=0;
    uint32_t timeoutSeconds=0;
    bool initialized=false;
    bool Observe(uint64_t now,uint64_t sequence) {
        const bool changed=initialized && sequence!=lastInput;
        if(!initialized || changed || now<lastActivityMs)lastActivityMs=now;
        initialized=true;lastInput=sequence;
        return changed || (restoreAtMs && now>=restoreAtMs);
    }
    void Reset(uint64_t now,uint64_t sequence) {
        initialized=true;lastActivityMs=now;lastInput=sequence;restoreAtMs=0;
    }
    bool Expired(uint64_t now) const {
        return initialized && timeoutSeconds && now>=lastActivityMs &&
            now-lastActivityMs>=uint64_t(timeoutSeconds)*1000;
    }
};
struct DisplayBacklightController {
    IntelBacklight pwm;
    DisplayIdlePolicy idle;
    bool SetOn(const BacklightIo& io,bool on,uint32_t restoreSeconds,uint64_t now,uint64_t sequence) {
        if(restoreSeconds>60 || (on&&restoreSeconds) || !pwm.SetOn(io,on))return false;
        idle.Reset(now,sequence);
        if(!on && restoreSeconds)idle.restoreAtMs=now+uint64_t(restoreSeconds)*1000;
        return true;
    }
    bool SetIdle(const BacklightIo& io,uint32_t seconds,uint64_t now,uint64_t sequence) {
        if(seconds>86400 || !pwm.SetOn(io,true))return false;
        idle.timeoutSeconds=seconds;idle.Reset(now,sequence);return true;
    }
    void Tick(const BacklightIo& io,uint64_t now,uint64_t sequence) {
        if(!pwm.ready)return;
        const bool wake=idle.Observe(now,sequence);
        if(wake || pwm.uncertain){
            if(pwm.uncertain)idle.timeoutSeconds=0;
            if(pwm.SetOn(io,true))idle.Reset(now,sequence);
        } else if(pwm.on && idle.Expired(now))pwm.SetOn(io,false);
    }
};
}
