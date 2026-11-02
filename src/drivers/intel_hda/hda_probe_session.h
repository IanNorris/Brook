#pragma once
#include "hda_probe.h"

namespace brook {
// One boot-time diagnostic transaction. The adapter must validate exact live
// PCI identity/resources/cache/ownership and create an exclusively owned,
// bounded UC mapping in prepare(), without accessing controller registers.
// It must write PCI command as a 32-bit value with a zero W1C status half.
enum class HdaSessionResult : uint8_t {
    Unprepared, CommandNotIdle, DecodeEnableFailed, InspectionFailed,
    CodecQueryFailed, CodecMismatch, Complete, RestoreFailed
};
struct HdaSessionReport {
    HdaSessionResult result=HdaSessionResult::Unprepared;
    HdaProbeResult probe=HdaProbeResult::Failed;
    HdaProbeController controller{};
    uint16_t before=0, enabled=0, after=0;
    uint32_t vendor=0, revision=0, subsystem=0;
    uint32_t values[12]{};uint8_t completed=0;
};
struct HdaProbeQuery {uint8_t nid;uint16_t verb;uint8_t payload;};
static constexpr HdaProbeQuery HdaDellSpeakerQueries[] = {
    {0x14,0xf00,0x09},{0x14,0xf00,0x0c},{0x14,0xf1c,0},
    {0x14,0xf00,0x0e},{0x14,0xf02,0},{0x14,0xf01,0},
    {0x14,0xf07,0},{0x14,0xf0c,0},{0x0d,0xf00,0x09},
    {0x0d,0xf00,0x0e},{0x0d,0xf02,0},{0x03,0xf00,0x09}
};
static_assert(sizeof(HdaDellSpeakerQueries)/sizeof(HdaDellSpeakerQueries[0]) ==
              sizeof(HdaSessionReport::values)/sizeof(uint32_t), "HDA query/report capacity mismatch");
template<class Io> HdaSessionReport HdaProbeDellSession(Io& io) {
    HdaSessionReport r{};
    if(!io.prepare() || !io.valid())return r;
    r.before=io.command();
    if(r.before!=0){r.result=HdaSessionResult::CommandNotIdle;return r;}
    io.setCommand(2);r.enabled=io.command();
    if(r.enabled!=2)r.result=HdaSessionResult::DecodeEnableFailed;
    else {
        HdaReadOnlyProbe<Io> probe(io);
        r.probe=probe.inspect(r.controller);
        if(r.probe!=HdaProbeResult::Ready)r.result=HdaSessionResult::InspectionFailed;
        else {
            r.result=HdaSessionResult::CodecQueryFailed;
            if((r.probe=probe.read(0,0,0xf00,0,r.vendor))==HdaProbeResult::Ready &&
               (r.probe=probe.read(0,0,0xf00,2,r.revision))==HdaProbeResult::Ready &&
               (r.probe=probe.read(0,1,0xf20,0,r.subsystem))==HdaProbeResult::Ready) {
                if(r.vendor!=0x10ec0288 || r.subsystem!=0x10280665)r.result=HdaSessionResult::CodecMismatch;
                else {
                    r.result=HdaSessionResult::Complete;
                    for(const auto& q:HdaDellSpeakerQueries) {
                        r.probe=probe.read(0,q.nid,q.verb,q.payload,r.values[r.completed]);
                        if(r.probe!=HdaProbeResult::Ready) {
                            r.result=HdaSessionResult::CodecQueryFailed;break;
                        }
                        ++r.completed;
                    }
                }
            }
        }
    }
    // Command was initially zero. A failed readback still attempts restoration;
    // failure is terminal and cannot be mistaken for a successful probe.
    io.setCommand(0);r.after=io.command();
    if(r.after!=0)r.result=HdaSessionResult::RestoreFailed;
    return r;
}
}
