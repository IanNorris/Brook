#include <cassert>
#include <cstring>

#include "debug_bridge_v3_policy.h"

int main()
{
    const auto allowed = [](const char* command, unsigned operation) {
        return brook::DebugBridgeV3CommandAllowed(command, std::strlen(command), operation);
    };
    assert(allowed("@17 bridge hardware caps\n", 0));
    assert(allowed("@17 bridge hardware caps\n", 1));
    assert(allowed("@17 bridge hardware logs 0 16\n", 1));
    assert(!allowed("@17 bridge hardware logs 0 16\n", 0));
    assert(!allowed("@17 bridge hardware caps \n", 0));
    assert(!allowed("@17 bridge hardware caps\n", 2));
    assert(!allowed("bridge hardware caps\n", 0));
    assert(!allowed("@17 bridge hardware caps", 0));
    assert(!allowed("@17 bridge hardware caps\r\n", 0));
    assert(!allowed("@17 bridge hardware caps\n@18 bridge hardware caps\n", 1));
    assert(!allowed("@17 @18 bridge hardware caps\n", 1));
    assert(!allowed("@17  bridge hardware caps\n", 1));
    assert(!allowed("@17 \n", 1));
    assert(!allowed("@x bridge hardware caps\n", 1));
    assert(allowed("@12345678901234567890123 bridge hardware caps\n", 0));
    assert(!allowed("@123456789012345678901234 bridge hardware caps\n", 0));
    constexpr char embeddedZero[] = "@17 bridge hardware caps\0\n";
    assert(!brook::DebugBridgeV3CommandAllowed(embeddedZero, sizeof(embeddedZero) - 1, 1));
    char largest[brook::DebugBridgeV3CommandBytes + 1];
    std::memset(largest, 'a', sizeof(largest));
    std::memcpy(largest, "@1 ", 3);
    largest[sizeof(largest) - 2] = '\n';
    assert(brook::DebugBridgeV3CommandAllowed(largest, sizeof(largest) - 1, 1));
    assert(!brook::DebugBridgeV3CommandAllowed(largest, sizeof(largest), 1));
}
