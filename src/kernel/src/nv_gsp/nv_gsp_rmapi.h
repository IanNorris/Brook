#pragma once
// RM API over the GSP RPC channel: GSP_RM_ALLOC, GSP_RM_CONTROL and FREE,
// as nouveau's r535 alloc.c / ctrl.c send them (one RPC each, reply
// awaited; asynchronous events seen while waiting are counted, and the last
// RC_TRIGGERED / MMU fault payload head is kept for the report).
// Clean-room from Linux nouveau (MIT): rm/r535/alloc.c 27-111, ctrl.c 38-93,
// rpc.c (r535_gsp_msg_recv event dispatch).
#include <stdint.h>
#include <stddef.h>
#include "nv_gsp_queue.h"
#include "nvrm_r570_obj.h"

namespace brook::nvgsp {

struct RmEvents {
    uint32_t total = 0, rc = 0, mmu_fault = 0, os_error = 0, post_event = 0, nocat = 0, libos = 0, other = 0;
    uint32_t last_other_fn = 0;
    uint8_t last_rc[32] = {};       // rpc_rc_triggered head (engine type, chid, gfid, exceptLevel, exceptType...)
    uint8_t last_os_error[32] = {};
};

constexpr uint32_t kRmApiBuf = 0x2000;

template <class Waiter, class Doorbell>
struct RmApi {
    RpcChannel* ch;
    uint8_t* rx;
    uint32_t rxcap;
    Waiter& wait;
    Doorbell& bell;
    RmEvents ev;
    QStatus q = QStatus::Ok;
    uint32_t rpc_result = 0, status = 0;   // header rpc_result and the payload's NV_STATUS
    uint32_t calls = 0;
    uint8_t* buf;                        // kRmApiBuf bytes of request scratch (kept off the stack)
    RmApi(RpcChannel* c, uint8_t* r, uint32_t cap, Waiter& w, Doorbell& d, uint8_t* scratch)
        : ch(c), rx(r), rxcap(cap), wait(w), bell(d), buf(scratch) {}

    bool OnEvent(const uint8_t* m, uint32_t len) {
        ++ev.total;
        const uint32_t fn = Get32(m + offsetof(rm570::rpc_message_header_v, function));
        const uint8_t* p = m + kRpcHdr;
        const uint32_t n = len > kRpcHdr ? len - kRpcHdr : 0;
        auto keep = [&](uint8_t* dst) { for (uint32_t i = 0; i < 32; ++i) dst[i] = i < n ? p[i] : 0; };
        switch (fn) {
            case rm570::NV_VGPU_MSG_EVENT_RC_TRIGGERED: ++ev.rc; keep(ev.last_rc); break;
            case rm570::NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED: ++ev.mmu_fault; break;
            case rm570::NV_VGPU_MSG_EVENT_OS_ERROR_LOG: ++ev.os_error; keep(ev.last_os_error); break;
            case rm570::NV_VGPU_MSG_EVENT_POST_EVENT: ++ev.post_event; break;
            case rm570::NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD: ++ev.nocat; break;
            case rm570::NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT: ++ev.libos; break;
            default: ++ev.other; ev.last_other_fn = fn; break;
        }
        return true;   // never abort a wait on an event
    }

    // Send `len` bytes of buf as `fn` and wait for its reply into rx.
    bool Call(uint32_t fn, uint32_t len, uint32_t* reply_len) {
        ++calls;
        rpc_result = 0; status = 0;
        auto on_event = [this](const uint8_t* m, uint32_t l) { return OnEvent(m, l); };
        q = ch->Send(fn, buf, len, wait, bell);
        if (q != QStatus::Ok) return false;
        q = ch->WaitFor(fn, rx, rxcap, reply_len, 0, wait, on_event);
        rpc_result = Get32(rx + offsetof(rm570::rpc_message_header_v, rpc_result));
        return q == QStatus::Ok || q == QStatus::RpcError;
    }

    // RM_ALLOC; the reply's params are copied back into `params` when given.
    bool Alloc(uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass, void* params, uint32_t size) {
        constexpr uint32_t h = sizeof(rm570::rpc_gsp_rm_alloc_v03_00);
        if (h + size > kRmApiBuf) { q = QStatus::TooBig; return false; }
        rm570::rpc_gsp_rm_alloc_v03_00 a = {};
        a.hClient = hClient; a.hParent = hParent; a.hObject = hObject; a.hClass = hClass; a.paramsSize = size;
        __builtin_memcpy(buf, &a, h);
        if (size) __builtin_memcpy(buf + h, params, size);
        uint32_t n = 0;
        if (!Call(rm570::NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, h + size, &n)) return false;
        if (n < kRpcHdr + h) { q = QStatus::BadElement; return false; }
        status = Get32(rx + kRpcHdr + offsetof(rm570::rpc_gsp_rm_alloc_v03_00, status));
        if (params && size && n >= kRpcHdr + h + size) __builtin_memcpy(params, rx + kRpcHdr + h, size);
        return q == QStatus::Ok && status == 0;
    }

    // RM_CONTROL, params in/out.
    bool Control(uint32_t hClient, uint32_t hObject, uint32_t cmd, void* params, uint32_t size) {
        constexpr uint32_t h = sizeof(rm570::rpc_gsp_rm_control_v03_00);
        if (h + size > kRmApiBuf) { q = QStatus::TooBig; return false; }
        rm570::rpc_gsp_rm_control_v03_00 c = {};
        c.hClient = hClient; c.hObject = hObject; c.cmd = cmd; c.paramsSize = size;
        __builtin_memcpy(buf, &c, h);
        if (size) __builtin_memcpy(buf + h, params, size);
        uint32_t n = 0;
        if (!Call(rm570::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, h + size, &n)) return false;
        if (n < kRpcHdr + h) { q = QStatus::BadElement; return false; }
        status = Get32(rx + kRpcHdr + offsetof(rm570::rpc_gsp_rm_control_v03_00, status));
        if (params && size && n >= kRpcHdr + h + size) __builtin_memcpy(params, rx + kRpcHdr + h, size);
        return q == QStatus::Ok && status == 0;
    }

    bool Free(uint32_t hClient, uint32_t hParent, uint32_t hObject) {
        rm570::rpc_free_v03_00 f = {};
        f.hRoot = hClient; f.hObjectParent = hParent; f.hObjectOld = hObject;
        __builtin_memcpy(buf, &f, sizeof(f));
        uint32_t n = 0;
        if (!Call(rm570::NV_VGPU_MSG_FUNCTION_FREE, sizeof(f), &n)) return false;
        if (n >= kRpcHdr + sizeof(f)) status = Get32(rx + kRpcHdr + offsetof(rm570::rpc_free_v03_00, status));
        return q == QStatus::Ok && status == 0;
    }
};

} // namespace brook::nvgsp
