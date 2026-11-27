// Host test for inject_parse.h (the debug channel's `inject button` / `inject rel` verbs).
#include <cassert>
#include <cstdio>
#include "inject_parse.h"
using namespace brook;
int main() {
    int32_t dx = 0, dy = 0;
    assert(InjectParseRel("15 -7", &dx, &dy) && dx == 15 && dy == -7);
    assert(InjectParseRel("-4096 +4096", &dx, &dy) && dx == -4096 && dy == 4096);
    assert(!InjectParseRel("4097 0", &dx, &dy));          // out of range
    assert(!InjectParseRel("5", &dx, &dy));               // missing dy
    assert(!InjectParseRel("5 6 7", &dx, &dy));           // trailing junk
    assert(!InjectParseRel("- 6", &dx, &dy));             // sign without digits
    assert(!InjectParseRel("99999999 1", &dx, &dy));      // overflow guard
    uint8_t b = 9; bool down = false;
    assert(InjectParseButton("1 down", &b, &down) && b == 1 && down);
    assert(InjectParseButton("2 up", &b, &down) && b == 2 && !down);
    assert(InjectParseButton("0 down", &b, &down) && b == 0 && down);
    assert(!InjectParseButton("3 down", &b, &down));      // only left/right/middle
    assert(!InjectParseButton("1 press", &b, &down));
    assert(!InjectParseButton("1 downx", &b, &down));
    assert(!InjectParseButton("1", &b, &down));
    std::puts("test_inject_parse: ok");
    return 0;
}
