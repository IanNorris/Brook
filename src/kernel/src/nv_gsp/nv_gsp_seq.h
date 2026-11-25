#pragma once
// Interpreter for GSP-RM's RUN_CPU_SEQUENCER event (0x1002): a list of
// register writes/modifies/polls/delays/stores plus GSP core control that
// the CPU must replay during boot (the 570.144 sequence on this card resets
// the GSP core, PIO-loads its boot ROM stub, starts it, waits for halt and
// resumes it with SEC2).
//
// Clean-room from Linux nouveau (MIT) nvkm/subdev/gsp/rm/r535/gsp.c
// 1009-1135 (r535_gsp_msg_run_cpu_sequencer) and r535/nvrm/gsp.h 350-415.
// Differences, on purpose: Brook bounds-checks every command against
// cmdIndex/bufferSizeDWord and the message length, rejects store slots >= 8
// and unknown opcodes before executing anything, and treats a poll timeout
// as a failure (nouveau logs nothing and carries on).
#include <stdint.h>
#include <stddef.h>
#include "nvrm_r570.h"

namespace brook::nvgsp {

enum class SeqStatus : uint8_t { Ok, Truncated, BadOpcode, BadSlot, PollTimeout, CoreFailed };
inline const char* SeqStatusName(SeqStatus s) {
    switch (s) {
    case SeqStatus::Ok: return "ok";
    case SeqStatus::Truncated: return "truncated";
    case SeqStatus::BadOpcode: return "bad-opcode";
    case SeqStatus::BadSlot: return "bad-slot";
    case SeqStatus::PollTimeout: return "poll-timeout";
    case SeqStatus::CoreFailed: return "core-failed";
    }
    return "?";
}

struct SeqResult {
    SeqStatus status = SeqStatus::Ok;
    uint32_t commands = 0;           // executed
    uint32_t fail_index = 0;         // dword index of the failing command
    uint32_t writes = 0, modifies = 0, polls = 0, delays = 0, stores = 0, core_ops = 0;
    uint32_t save[8] = {};
};

// Pass 1: validate the whole buffer. `words` points at commandBuffer,
// `avail` = dwords present in the message after the 40-byte header.
inline SeqStatus ValidateSequencer(const uint32_t* words, uint32_t avail, uint32_t cmd_index,
                                   uint32_t buffer_dwords, uint32_t* fail) {
    if (cmd_index > avail || cmd_index > buffer_dwords) { *fail = 0; return SeqStatus::Truncated; }
    for (uint32_t p = 0; p < cmd_index;) {
        const uint32_t op = words[p];
        if (op > rm570::GSP_SEQ_BUF_OPCODE_CORE_RESUME) { *fail = p; return SeqStatus::BadOpcode; }
        const uint32_t n = 1 + rm570::GspSeqPayloadDwords(op);
        if (p + n > cmd_index) { *fail = p; return SeqStatus::Truncated; }
        if (op == rm570::GSP_SEQ_BUF_OPCODE_REG_STORE && words[p + 2] >= 8) { *fail = p; return SeqStatus::BadSlot; }
        p += n;
    }
    return SeqStatus::Ok;
}

// Bus: rd32(addr), wr32(addr, v), udelay(us), now_us(), seq_step(opcode)
// (called before each command; tracing only), and the core hooks
// core_reset(), core_start(), core_wait_halt(), core_resume() -> bool.
template <class Bus>
inline SeqResult RunSequencer(Bus& bus, const uint8_t* msg_payload, uint32_t payload_len) {
    SeqResult r;
    if (payload_len < sizeof(rm570::rpc_run_cpu_sequencer_v17_00)) { r.status = SeqStatus::Truncated; return r; }
    rm570::rpc_run_cpu_sequencer_v17_00 h;
    __builtin_memcpy(&h, msg_payload, sizeof(h));
    const uint32_t avail = (payload_len - uint32_t(sizeof(h))) / 4;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(msg_payload + sizeof(h));
    r.status = ValidateSequencer(w, avail, h.cmdIndex, h.bufferSizeDWord, &r.fail_index);
    if (r.status != SeqStatus::Ok) return r;
    for (uint32_t p = 0; p < h.cmdIndex;) {
        const uint32_t op = w[p];
        const uint32_t* a = w + p + 1;
        bus.seq_step(op);
        switch (op) {
        case rm570::GSP_SEQ_BUF_OPCODE_REG_WRITE:
            bus.wr32(a[0], a[1]); ++r.writes; break;
        case rm570::GSP_SEQ_BUF_OPCODE_REG_MODIFY: {
            const uint32_t v = bus.rd32(a[0]);
            bus.wr32(a[0], (v & ~a[1]) | a[2]); ++r.modifies; break;
        }
        case rm570::GSP_SEQ_BUF_OPCODE_REG_POLL: {
            const uint32_t us = a[3] ? a[3] : 4000000;
            (void)bus.rd32(a[0]);                       // nouveau's priming read
            const uint64_t t0 = bus.now_us();
            bool ok = false;
            for (;;) {
                if ((bus.rd32(a[0]) & a[1]) == a[2]) { ok = true; break; }
                if (bus.now_us() - t0 >= us) break;
                bus.udelay(1);
            }
            ++r.polls;
            if (!ok) { r.status = SeqStatus::PollTimeout; r.fail_index = p; return r; }
            break;
        }
        case rm570::GSP_SEQ_BUF_OPCODE_DELAY_US:
            bus.udelay(a[0]); ++r.delays; break;
        case rm570::GSP_SEQ_BUF_OPCODE_REG_STORE:
            r.save[a[1]] = bus.rd32(a[0]); ++r.stores; break;
        case rm570::GSP_SEQ_BUF_OPCODE_CORE_RESET:
        case rm570::GSP_SEQ_BUF_OPCODE_CORE_START:
        case rm570::GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT:
        case rm570::GSP_SEQ_BUF_OPCODE_CORE_RESUME: {
            const bool ok = op == rm570::GSP_SEQ_BUF_OPCODE_CORE_RESET ? bus.core_reset() :
                            op == rm570::GSP_SEQ_BUF_OPCODE_CORE_START ? bus.core_start() :
                            op == rm570::GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT ? bus.core_wait_halt() :
                            bus.core_resume();
            ++r.core_ops;
            if (!ok) { r.status = SeqStatus::CoreFailed; r.fail_index = p; return r; }
            break;
        }
        }
        ++r.commands;
        p += 1 + rm570::GspSeqPayloadDwords(op);
    }
    return r;
}

} // namespace brook::nvgsp
