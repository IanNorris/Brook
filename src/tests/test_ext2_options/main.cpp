// Host test for ext2_options.h (write-behind size/age from fw_cfg or /etc markers).
#include <cassert>
#include <cstdio>
#include <cstring>
#include "ext2_options.h"
using namespace brook;
static uint32_t P(const char* s, uint32_t lo, uint32_t hi, uint32_t fb) { return Ext2ParseBounded(s, uint32_t(std::strlen(s)), lo, hi, fb); }
int main() {
    assert(P("16", 0, 16, 0) == 16);
    assert(P("16\n", 0, 16, 0) == 16);       // /etc marker written by the builder
    assert(P("8\n", 0, 16, 0) == 8);
    assert(P("17\n", 0, 16, 0) == 0);        // out of range -> off
    assert(P("", 0, 16, 0) == 0);
    assert(P("\n", 0, 16, 0) == 0);
    assert(P("1 6", 0, 16, 0) == 0);
    assert(P("16\n\n", 0, 16, 0) == 0);      // only one trailing newline
    assert(P("123456", 0, 99999, 7) == 7);   // too long
    assert(P("1000\n", 250, 10000, 1000) == 1000);
    assert(P("100", 250, 10000, 1000) == 1000);
    std::puts("test_ext2_options: ok");
    return 0;
}
