#pragma once
#include <stdint.h>

namespace brook {

// Opaque deterministic catalog keys, not authentication tokens. Catalog builds
// reject duplicate IDs/collisions before publication; admission also binds the
// generation. Hash namespaced exact bytes, with explicit field separators.
class LauncherIdentity {
    uint64_t first=14695981039346656037ULL,second=1099511628211ULL;
    void byte(uint8_t value) {first=(first^value)*1099511628211ULL;second=(second^value)*1099511628211ULL;}
public:
    void field(const char* value) {if(value)while(*value)byte(static_cast<uint8_t>(*value++));byte(0);}
    void finish(char out[33]) const {
        const char* digits="0123456789abcdef";
        for(unsigned i=0;i<16;++i){out[i]=digits[(first>>((15-i)*4))&15];out[16+i]=digits[(second>>((15-i)*4))&15];}
        out[32]=0;
    }
};

// Manifest commands use the shell's run syntax. Validate the actual program,
// not a supported run option. UID is bounded to the shell's signed override.
inline const char* LauncherExecutableStart(const char* command)
{
    if (!command) return nullptr;
    auto space = [](char c) { return c == ' ' || c == '\t'; };
    while (space(*command)) ++command;
    if (command[0] == '-' && command[1] == '-' && command[2] == 'u' &&
        command[3] == 'i' && command[4] == 'd' &&
        (space(command[5]) || command[5] == 0))
    {
        command += 5;
        while (space(*command)) ++command;
        if (*command < '0' || *command > '9') return nullptr;
        uint32_t uid = 0;
        while (*command >= '0' && *command <= '9')
        {
            const uint32_t digit = *command++ - '0';
            if (uid > (0x7fffffffu - digit) / 10) return nullptr;
            uid = uid * 10 + digit;
        }
        if (!space(*command)) return nullptr;
        while (space(*command)) ++command;
    }
    return *command ? command : nullptr;
}

} // namespace brook
