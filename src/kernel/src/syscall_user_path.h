#pragma once

#include "path_buffer.h"
#include "user_copy.h"

namespace brook {

inline int CopyUserPath(uint64_t address, PathBuffer& path)
{
    if (!path.allocate()) return -12;
    return CopyUserCStringResult(address, path, path.capacity());
}

}
