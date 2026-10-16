#pragma once
#include <stdint.h>

namespace brook {

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
