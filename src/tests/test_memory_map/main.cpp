#include "test_framework.h"

TEST_MAIN("memory_map", {
    brook::BootProtocol* proto = brook::test::g_protocol;

    // Must have a valid memory map
    ASSERT_TRUE(proto->memoryMap != nullptr);
    ASSERT_TRUE(proto->memoryMapCount > 0);
    ASSERT_TRUE(proto->memoryMapCount < 1024);  // sanity cap

    // Must have at least one Free region
    bool foundFree = false;
    for (uint32_t i = 0; i < proto->memoryMapCount; i++) {
        if (proto->memoryMap[i].type == brook::MemoryType::Free) {
            foundFree = true;
            // Each free region must have at least 1 page
            ASSERT_TRUE(proto->memoryMap[i].pageCount > 0);
        }
        // physicalStart must be 4KB-aligned
        ASSERT_EQ(proto->memoryMap[i].physicalStart & 0xFFF, (uint64_t)0);
    }
    ASSERT_TRUE(foundFree);

    // No overlapping regions (assumes sorted by physicalStart)
    // (skip overlap check for now - bootloader may not sort)

    // ACPI RSDP must be non-zero
    ASSERT_TRUE(proto->acpi.rsdpPhysical != 0);

    // Framebuffer must be sensible
    ASSERT_TRUE(proto->framebuffer.width >= 640 && proto->framebuffer.width <= 7680);
    ASSERT_TRUE(proto->framebuffer.height >= 480 && proto->framebuffer.height <= 4320);
    ASSERT_TRUE(proto->framebuffer.physicalBase != 0);
})
