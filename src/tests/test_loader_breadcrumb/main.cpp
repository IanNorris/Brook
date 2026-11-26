// Host test for the loader breadcrumb record (src/bootloader/src/loader_breadcrumb.h).
#include "loader_breadcrumb.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
using namespace brook::bootloader::breadcrumb;

int main() {
    char rec[kRecordBytes];
    State st; st.entries = 41; st.stage = "entry";
    Format(st, rec);
    assert(rec[kRecordBytes - 1] == '\n' && std::memcmp(rec, kMagic, std::strlen(kMagic)) == 0);
    const std::string t(rec, kRecordBytes);
    assert(t.find("entries=41\nstage=entry\ngop=none modes=0 setmode=none\n") != std::string::npos);
    assert(ParseEntries(rec, kRecordBytes) == 41);
    // gop stage with a failed SetMode keeps the current mode
    st.stage = "gop"; st.gop = true; st.gop_w = 3840; st.gop_h = 2160; st.gop_modes = 12;
    st.setmode_tried = true; st.setmode_failed = true; st.setmode_status = 0x8000000000000003ull;
    Format(st, rec);
    assert(std::string(rec, kRecordBytes).find("gop=3840x2160 modes=12 setmode=failed:0x8000000000000003\n") != std::string::npos);
    assert(ParseEntries(rec, kRecordBytes) == 41);
    // halt: ASCII-only message, clipped, still fixed size
    char msg[96]; const char16_t m16[] = u"No GOP framebuffer available é";
    Ascii(m16, msg, sizeof(msg));
    assert(std::strcmp(msg, "No GOP framebuffer available ?") == 0);
    st.stage = "halt"; st.halt = msg; st.halt_status = 0x8000000000000003ull;
    Format(st, rec);
    assert(std::string(rec, kRecordBytes).find("stage=halt\n") != std::string::npos);
    assert(std::string(rec, kRecordBytes).find("halt=0x8000000000000003 No GOP framebuffer available ?\n") != std::string::npos);
    char longmsg[400]; std::memset(longmsg, 'x', sizeof(longmsg) - 1); longmsg[sizeof(longmsg) - 1] = 0;
    st.halt = longmsg; Format(st, rec);
    assert(rec[kRecordBytes - 1] == '\n');
    // not ours / truncated / garbage -> 0 (the loader then starts counting at 1)
    assert(ParseEntries("", 0) == 0);
    assert(ParseEntries("hello", 5) == 0);
    Format(State{}, rec); rec[40] = 'z';
    char cut[30]; std::memcpy(cut, rec, sizeof(cut));
    assert(ParseEntries(cut, sizeof(cut)) == 0);
    const char big[] = "brook-loader-breadcrumb v1\nentries=99999999999\n";
    assert(ParseEntries(big, sizeof(big) - 1) == 0);
    std::printf("test_loader_breadcrumb: OK\n");
    return 0;
}
