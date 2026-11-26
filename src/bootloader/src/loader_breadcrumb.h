#pragma once
// Loader breadcrumbs on the loader's own boot volume (p1 on the RTX SSD): LOADER.TXT is
// rewritten at each stage of every boot (entry, gop, kernel, halt), HALT.TXT is written by
// Halt(). Each is one fixed 256-byte ASCII record (overwritten in place, never resized),
// so a write that is cut short leaves an unparsable record, never a longer file. The
// signed update and trial flow address their files by name and never list the root, so
// these two extra files cannot affect them. `entries` counts loader entries: if a hung
// boot does not increase it, the hang was before the loader (firmware POST).
// Pure formatting/parsing here (host-tested in src/tests/test_loader_breadcrumb).
#include <stdint.h>

namespace brook {
namespace bootloader {
namespace breadcrumb {

constexpr uint32_t kRecordBytes = 256;
constexpr const char kMagic[] = "brook-loader-breadcrumb v1\n";

struct State {
    uint32_t entries = 0;
    const char* stage = "entry";
    uint32_t gop_w = 0, gop_h = 0, gop_modes = 0;
    bool gop = false, setmode_tried = false, setmode_failed = false;
    uint64_t setmode_status = 0, halt_status = 0;
    const char* halt = nullptr;     // ASCII message (converted from the UTF-16 Halt text)
};

namespace detail {
struct Out {
    char* p; uint32_t n = 0, cap;
    void c(char ch) { if (n + 1 < cap) p[n++] = ch; }
    void s(const char* t) { while (t && *t) c(*t++); }
    void u(uint64_t v) { char b[24]; int i = 0; do { b[i++] = char('0' + v % 10); v /= 10; } while (v); while (i) c(b[--i]); }
    void x(uint64_t v) { s("0x"); char b[24]; int i = 0; do { b[i++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); while (i) c(b[--i]); }
};
}

// Fills exactly kRecordBytes: the fields, then spaces, then a final newline.
inline void Format(const State& st, char out[kRecordBytes]) {
    detail::Out o{out, 0, kRecordBytes};
    o.s(kMagic);
    o.s("entries="); o.u(st.entries); o.c('\n');
    o.s("stage="); o.s(st.stage); o.c('\n');
    o.s("gop=");
    if (st.gop) { o.u(st.gop_w); o.c('x'); o.u(st.gop_h); } else o.s("none");
    o.s(" modes="); o.u(st.gop_modes);
    o.s(" setmode=");
    if (!st.setmode_tried) o.s("none"); else if (st.setmode_failed) { o.s("failed:"); o.x(st.setmode_status); } else o.s("ok");
    o.c('\n');
    if (st.halt) {
        o.s("halt="); o.x(st.halt_status); o.c(' ');
        for (const char* m = st.halt; *m && o.n + 2 < kRecordBytes; ++m) o.c((*m >= 32 && *m < 127) ? *m : '?');
        o.c('\n');
    }
    while (o.n < kRecordBytes - 1) out[o.n++] = ' ';
    out[kRecordBytes - 1] = '\n';
}

// entries=N from a previous record; 0 when the record is missing or not ours.
inline uint32_t ParseEntries(const char* in, uint32_t len) {
    uint32_t m = 0;
    while (kMagic[m]) { if (m >= len || in[m] != kMagic[m]) return 0; ++m; }
    const char key[] = "entries=";
    uint32_t i = m, k = 0;
    while (key[k]) { if (i >= len || in[i] != key[k]) return 0; ++i; ++k; }
    uint64_t v = 0; bool any = false;
    while (i < len && in[i] >= '0' && in[i] <= '9' && v < 0xffffffffull) { v = v * 10 + uint32_t(in[i++] - '0'); any = true; }
    return (any && i < len && in[i] == '\n' && v < 0xffffffffull) ? uint32_t(v) : 0;
}

// UTF-16 -> ASCII for Halt messages ('?' for anything outside printable ASCII).
inline void Ascii(const char16_t* in, char* out, uint32_t cap) {
    uint32_t n = 0;
    while (in && in[n] && n + 1 < cap) { const char16_t c = in[n]; out[n] = (c >= 32 && c < 127) ? char(c) : '?'; ++n; }
    out[n] = 0;
}

}  // namespace breadcrumb
}  // namespace bootloader
}  // namespace brook
