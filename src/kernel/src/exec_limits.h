#pragma once
#include <stdint.h>

namespace brook {

// Shared by execve's user-memory copy and initial stack construction. Counts
// exclude the terminating null pointer; byte budgets include string NULs.
static constexpr int EXEC_MAX_ARGS = 512;
static constexpr int EXEC_MAX_ENV = 256;
static constexpr uint32_t EXEC_ARG_BYTES = 64 * 1024;
static constexpr uint32_t EXEC_ENV_BYTES = 32 * 1024;

// For trusted kernel vectors only; execve validates user memory while copying.
inline bool ExecVectorFits(int count, const char* const* values,
                           int maxCount, uint32_t capacity)
{
    if (count < 0 || count > maxCount || (count && !values)) return false;
    uint32_t used = 0;
    for (int i = 0; i < count; ++i) {
        if (!values[i]) return false;
        uint32_t len = 0;
        while (len < capacity - used && values[i][len]) ++len;
        if (len == capacity - used) return false;
        used += len + 1;
    }
    return true;
}

inline bool ExecArgumentsFit(int argc, const char* const* argv,
                             int envc, const char* const* envp)
{
    return ExecVectorFits(argc, argv, EXEC_MAX_ARGS, EXEC_ARG_BYTES) &&
           ExecVectorFits(envc, envp, EXEC_MAX_ENV, EXEC_ENV_BYTES);
}

} // namespace brook
