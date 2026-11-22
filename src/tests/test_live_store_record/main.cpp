#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "live_store_record.h"
using namespace brook;
int main()
{
    const char* good = "guid=00112233445566778899aabbccddeeff bytes=4096000 sha256="
                       "0001020304050607080910111213141516171819202122232425262728293031\n";
    LiveStoreRecord r{};
    assert(LiveStoreParse(good, strlen(good), r));
    assert(r.guid[0] == 0x00 && r.guid[15] == 0xff && r.bytes == 4096000 && r.sha256[0] == 0 && r.sha256[31] == 0x31);
    std::string s(good);
    const char* noNl = "guid=00112233445566778899aabbccddeeff bytes=8192 sha256="
                       "0001020304050607080910111213141516171819202122232425262728293031";
    assert(LiveStoreParse(noNl, strlen(noNl), r) && r.bytes == 8192);
    auto bad = [&](std::string t) { LiveStoreRecord x{}; return !LiveStoreParse(t.data(), t.size(), x); };
    assert(bad(std::string(good).replace(5, 1, "G")));                  // non-hex guid
    assert(bad(std::string(good).replace(5, 32, std::string(32, '0')))); // zero guid
    assert(bad(std::string(noNl).replace(44, 4, "8193")));               // not 4 KiB aligned
    assert(bad(std::string(noNl) + "x"));                                // trailing bytes
    assert(bad(std::string(noNl).substr(0, strlen(noNl) - 2)));          // short digest
    assert(bad("guid=00112233445566778899aabbccddeeff bytes= sha256=" + std::string(64, '0')));
    assert(bad("guid=00112233445566778899aabbccddeeff bytes=1234567890123456 sha256=" + std::string(64, '0')));
    assert(bad("bytes=4096 guid=00112233445566778899aabbccddeeff sha256=" + std::string(64, '0')));
    std::puts("live store record: PASS");
}
