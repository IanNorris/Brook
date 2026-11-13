#pragma once
#include <stdint.h>

namespace brook {
// Policy for an OS-owned scanout backend, not a register interface. The caller
// serializes access and retains each opaque, non-reused buffer identity until
// this policy explicitly returns it. The firmware framebuffer must be retained
// separately until the backend proves its replacement is live.
struct ScanoutLifetime {
    uint64_t display=0,active=0,pending=0;
    bool issued=false,uncertain=false;
};
// The backend must have already validated format/geometry and acquired an OS
// reference to the buffer. Rendering and all producer fences must be complete.
inline bool ScanoutPrepare(ScanoutLifetime& s,uint64_t display,uint64_t buffer,bool producerComplete) {
    if(!display || !buffer || !producerComplete || s.pending || s.uncertain ||
       (s.display && s.display!=display) || s.active==buffer)return false;
    s.display=display;s.pending=buffer;s.issued=false;return true;
}
// Once a plane update may have reached hardware, both buffers remain pinned.
inline bool ScanoutIssue(ScanoutLifetime& s) {
    if(!s.pending || s.issued || s.uncertain)return false;
    s.issued=true;return true;
}
// A failed attempt before any device write may return its prepared buffer.
inline uint64_t ScanoutCancelUnissued(ScanoutLifetime& s) {
    if(!s.pending || s.issued || s.uncertain)return 0;
    const uint64_t old=s.pending;s.pending=0;return old;
}
// 'live' must come from a backend-specific, race-free completion proof, not a
// command acknowledgement or an arbitrary observation of a register.
// The returned previous active buffer may be released only by the caller.
inline bool ScanoutComplete(ScanoutLifetime& s,uint64_t display,uint64_t live,uint64_t& retired) {
    if(!s.issued || !s.pending || s.uncertain || display!=s.display || live!=s.pending)return false;
    retired=s.active;s.active=s.pending;s.pending=0;s.issued=false;return true;
}
// A timeout, mode change or failed write after issue does not establish which
// buffer is scanned out. Do not recycle either on renderer exit or lease loss.
inline void ScanoutUncertain(ScanoutLifetime& s) {if(s.display)s.uncertain=true;}
// Only call after a backend-specific proof that the display can no longer read
// either buffer. This does not by itself prove render-engine work is drained.
inline bool ScanoutQuiesced(ScanoutLifetime& s,uint64_t& active,uint64_t& pending) {
    if(!s.display)return false;
    active=s.active;pending=s.pending;s={};return true;
}
}
