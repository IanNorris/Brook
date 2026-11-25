// BROOK_NV_GSP (default OFF): compile-only unit for the TU104 GSP-RM M0
// groundwork. Nothing here is called at boot; it instantiates the header
// templates with a no-op bus so the freestanding kernel toolchain checks
// them (-ffreestanding -fno-builtin -fno-exceptions -Wshadow -Werror).
// The live MMIO bus, firmware loading and the bridge verbs arrive in M1.
#include "nv_gsp/nv_gsp_boot.h"
#include "nv_gsp/nv_gsp_queue.h"
#include "nv_gsp/nv_gsp_radix3.h"
#include "nv_gsp/nv_gsp_seq.h"

namespace brook::nvgsp {
namespace {

struct InertBus {
    uint32_t rd32(uint32_t) { return 0; }
    void wr32(uint32_t, uint32_t) {}
    void udelay(uint32_t) {}
    uint64_t now_us() { return t++ * 1000000; }   // every poll times out at once
    void mark(const char*, const char*) {}
    void seq_step(uint32_t) {}
    bool core_reset() { return false; }
    bool core_start() { return false; }
    bool core_wait_halt() { return false; }
    bool core_resume() { return false; }
    uint64_t t = 0;
};

struct NoWait { bool operator()() { return false; } };
struct NoBell { void operator()() {} };

}  // namespace

// Referenced by nothing; kept so the linker-visible symbol proves the build.
[[gnu::used]] int NvGspM0CompileCheck(const uint8_t* blob, size_t len) {
    const Span s{blob, len};
    GspElf elf;
    BooterImage booter;
    RiscvBootloader bl;
    Vbios vb;
    FwsecImage fw;
    int r = int(ParseGspElf(s, &elf)) + int(ParseBooter(s, &booter)) + int(ParseRiscvBootloader(s, &bl));
    if (ParseVbios(s, &vb) == FwStatus::Ok) r += int(ParseFwsec(vb, &fw));
    FbLayout L;
    FbLayoutInput in;
    in.fb_size = 8ull << 30; in.reg_625f04 = 0x20e09; in.elf_size = 28528288; in.boot_size = 0x1000;
    r += ComputeFbLayout(in, &L) ? 1 : 0;
    InertBus bus;
    r += int(RunPreflight(bus).status);
    r += int(RunFwsecFrts(bus, fw, nullptr, blob, 0, L).status);
    r += int(ResetGspRiscv(bus, 0));
    r += int(RunBooterLoad(bus, booter, blob, 0).status);
    r += int(GspHandshake(bus, 0));
    r += int(RunSequencer(bus, blob, uint32_t(len)).status);
    GspCoreOps<InertBus> ops{bus, 0, 0};
    r += ops.core_resume() ? 1 : 0;
    r += int(PlanRadix3(len).lvl2_pages);
    static uint8_t shm_unused[1];
    QueueShm q;
    r += InitQueueShm(shm_unused, 0, sizeof(shm_unused), &q) ? 1 : 0;
    RpcChannel* ch = nullptr;                  // instantiate only; 128 KiB, never allocated here
    if (ch) {
        NoWait w; NoBell b;
        uint8_t buf[64];
        uint32_t n = 0;
        auto ev = [](const uint8_t*, uint32_t) { return true; };
        r += int(ch->Send(rm570::NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, blob, uint32_t(len), w, b));
        r += int(ch->WaitFor(rm570::NV_VGPU_MSG_EVENT_GSP_INIT_DONE, buf, sizeof(buf), &n, 0, w, ev));
    }
    return r;
}

}  // namespace brook::nvgsp
