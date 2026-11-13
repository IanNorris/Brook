#pragma once
#include <stdint.h>

namespace brook {
enum class AcpiEcResult { Ok, Busy, UnexpectedOutput, TimedOut, Unavailable };

// ACPI EC byte protocol. The caller must serialize use of this instance and
// obtain ownership of the EC interface (including any firmware Global Lock)
// before calling it. The adapter supplies status, command, and data access,
// monotonic microseconds, and a bounded delay between polls.
class AcpiEmbeddedController {
public:
    template<class Io> AcpiEcResult Read(Io& io,uint8_t address,uint8_t& value) {
        return Transfer(io,address,&value,0);
    }
    template<class Io> AcpiEcResult Write(Io& io,uint8_t address,uint8_t value) {
        return Transfer(io,address,nullptr,value);
    }
    bool Quarantined() const {return quarantined_;}

private:
    static constexpr uint8_t OutputFull=1,InputFull=2,Burst=16;
    static constexpr uint64_t TimeoutUs=50000;
    static constexpr unsigned PollLimit=5000;
    bool quarantined_=false;

    template<class Io> AcpiEcResult Wait(Io& io,uint64_t start,unsigned& remaining,bool output) {
        while(remaining && io.nowUs()-start<TimeoutUs) {
            --remaining;
            const uint8_t status=io.status();
            if(status==0xff || (status&Burst))return AcpiEcResult::Unavailable;
            if(status&OutputFull)
                return output?AcpiEcResult::Ok:AcpiEcResult::UnexpectedOutput;
            if(!output && !(status&InputFull))return AcpiEcResult::Ok;
            io.delayPoll();
        }
        return AcpiEcResult::TimedOut;
    }
    template<class Io> AcpiEcResult Transfer(Io& io,uint8_t address,uint8_t* read,uint8_t write) {
        if(quarantined_)return AcpiEcResult::Unavailable;
        const uint64_t start=io.nowUs();
        unsigned remaining=PollLimit;
        auto result=Wait(io,start,remaining,false);
        if(result!=AcpiEcResult::Ok)
            return result==AcpiEcResult::TimedOut?AcpiEcResult::Busy:result;
        io.command(read?0x80:0x81);
        result=Wait(io,start,remaining,false);
        if(result==AcpiEcResult::Ok) {
            io.writeData(address);
            result=Wait(io,start,remaining,read!=nullptr);
            if(result==AcpiEcResult::Ok) {
                if(read)*read=io.readData();
                else {
                    io.writeData(write);
                    result=Wait(io,start,remaining,false);
                }
            }
        }
        // A command has already been issued. Its late completion or remaining
        // bytes cannot safely be mistaken for a later transaction's response.
        if(result!=AcpiEcResult::Ok)quarantined_=true;
        return result;
    }
};
}
