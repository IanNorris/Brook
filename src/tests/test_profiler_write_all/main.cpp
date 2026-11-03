#include "profiler_write_all.h"
#include <cassert>
#include <string>

int main()
{
    using brook::ProfilerWriteAll;
    const char input[]="abcdef";
    std::string output;
    int rc=ProfilerWriteAll([&](const char* p,uint32_t n) {
        const auto take=n>2?2u:n; output.append(p,take);return static_cast<int>(take);
    },input,6);
    assert(rc==0 && output=="abcdef");
    int calls=0;
    rc=ProfilerWriteAll([&](const char*,uint32_t) { return ++calls==1?2:-28; },input,6);
    assert(rc==-28 && calls==2);
    calls=0;
    rc=ProfilerWriteAll([&](const char*,uint32_t) { ++calls; return 0; },input,6);
    assert(rc==-5 && calls==1);
    rc=ProfilerWriteAll([](const char*,uint32_t n) { return static_cast<int>(n+1); },input,6);
    assert(rc==-5);
    calls=0;
    rc=ProfilerWriteAll([&](const char*,uint32_t) { ++calls; return 0; },input,0);
    assert(rc==0 && calls==0);
}
