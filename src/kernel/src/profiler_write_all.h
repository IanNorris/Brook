#pragma once
#include <stdint.h>

namespace brook {
// A VFS write can be short. Keep the first error and reject no progress or an
// invalid count so a full output filesystem cannot look like a completed dump.
template<class Writer>
int ProfilerWriteAll(Writer write, const char* data, uint32_t size)
{
    uint32_t done=0;
    while(done<size) {
        const int n=write(data+done,size-done);
        if(n<0)return n;
        if(n==0 || static_cast<uint32_t>(n)>size-done)return -5;
        done+=static_cast<uint32_t>(n);
    }
    return 0;
}
}
