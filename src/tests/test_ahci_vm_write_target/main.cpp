#include "ahci_vm_write_target.h"
#include <cassert>
#include <cstring>
#include <initializer_list>

using namespace brook;

struct VirtualCpu {
    bool present;
    uint32_t maxLeaf;
    char vendor[13];

    void operator()(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) const {
        a = b = c = d = 0;
        if (leaf == 1) {
            if (present) c = 1u << 31;
        } else {
            assert(leaf == 0x40000000u);
            a = maxLeaf;
            std::memcpy(&b, vendor, 4);
            std::memcpy(&c, vendor + 4, 4);
            std::memcpy(&d, vendor + 8, 4);
        }
    }
};

int main()
{
    uint64_t sectors = 0;
    const auto parse = [&](const char* record) { return AhciVmWriteSectorCount(record, std::strlen(record), sectors); };
    assert(parse("BRKAHCIVM1\n262144\n") && sectors == 262144);
    assert(parse("BRKAHCIVM1\n281474976710656\n") && sectors == (1ull << 48));
    for (const char* malformed : {"BRKAHCI1\n262144\n", "BRKAHCIVM2\n262144\n", "BRKAHCIVM1\n",
         "BRKAHCIVM1\n67\n", "BRKAHCIVM1\n0262144\n", "BRKAHCIVM1\n262144", "BRKAHCIVM1\n+262144\n",
         "BRKAHCIVM1\n262144\r\n", "BRKAHCIVM1\n281474976710657\n", "BRKAHCIVM1\n262144\nextra\n"}) {
        assert(!parse(malformed) && sectors == 0);
    }
    constexpr char hidden[] = "BRKAHCIVM1\n262144\n\0trailing";
    assert(!AhciVmWriteSectorCount(hidden, sizeof(hidden)-1, sectors));
    assert(!AhciVmWriteSectorCount(nullptr, 10, sectors));

    constexpr VirtualCpu vmware{true, 0x40000010u, "VMwareVMware"};
    constexpr VirtualCpu kvm{true, 0x40000001u, "KVMKVMKVM\0\0"};
    constexpr VirtualCpu tcg{true, 0x40000001u, "TCGTCGTCGTCG"};
    assert(AhciVmWritePlatform(vmware) && AhciVmWritePlatform(kvm) && AhciVmWritePlatform(tcg));
    assert(!AhciVmWritePlatform(VirtualCpu{false, 0x40000010u, "VMwareVMware"}));
    assert(!AhciVmWritePlatform(VirtualCpu{true, 0x40000000u, "Microsoft Hv"}));
    assert(!AhciVmWritePlatform(VirtualCpu{true, 0x3fffffffu, "VMwareVMware"}));
    assert(!AhciVmWritePlatform(VirtualCpu{true, 0x40000001u, "KVMKVMKVMxxx"}));
    assert(!AhciVmWritePlatform(VirtualCpu{true, 0x40000001u, "TCGTCGTCGTxx"}));

    AtaIdentifyInfo disk{};
    disk.sectors = 262144;
    disk.logicalSectorBytes = disk.physicalSectorBytes = 512;
    assert(AhciVmWriteGeometry(262144, disk, true));
    assert(!AhciVmWriteGeometry(262143, disk, true));
    assert(!AhciVmWriteGeometry(262144, disk, false));
    disk.logicalSectorBytes = disk.physicalSectorBytes = 4096;
    assert(!AhciVmWriteGeometry(262144, disk, true));
    disk.logicalSectorBytes = 512;
    assert(!AhciVmWriteGeometry(262144, disk, true));
}
