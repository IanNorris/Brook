#pragma once

#include <stdint.h>

namespace brook {

// ===========================================================================
// Strongly-typed address wrappers
// ===========================================================================
// These prevent silently mixing physical and virtual addresses, which is one
// of the most common OS-dev footguns.  Construct via the explicit constructor
// and extract via .raw().

struct PhysicalAddress
{
    uint64_t value;

    constexpr PhysicalAddress() : value(0) {}
    constexpr explicit PhysicalAddress(uint64_t v) : value(v) {}

    uint64_t raw() const { return value; }
    explicit operator bool() const { return value != 0; }

    PhysicalAddress operator+(uint64_t offset) const { return PhysicalAddress(value + offset); }
    PhysicalAddress operator-(uint64_t offset) const { return PhysicalAddress(value - offset); }
    PhysicalAddress& operator+=(uint64_t offset) { value += offset; return *this; }
    uint64_t operator-(PhysicalAddress o) const { return value - o.value; }

    bool operator==(PhysicalAddress o) const { return value == o.value; }
    bool operator!=(PhysicalAddress o) const { return value != o.value; }
    bool operator< (PhysicalAddress o) const { return value <  o.value; }
    bool operator<=(PhysicalAddress o) const { return value <= o.value; }
    bool operator> (PhysicalAddress o) const { return value >  o.value; }
    bool operator>=(PhysicalAddress o) const { return value >= o.value; }
};

struct VirtualAddress
{
    uint64_t value;

    constexpr VirtualAddress() : value(0) {}
    constexpr explicit VirtualAddress(uint64_t v) : value(v) {}

    uint64_t raw() const { return value; }
    explicit operator bool() const { return value != 0; }

    VirtualAddress operator+(uint64_t offset) const { return VirtualAddress(value + offset); }
    VirtualAddress operator-(uint64_t offset) const { return VirtualAddress(value - offset); }
    VirtualAddress& operator+=(uint64_t offset) { value += offset; return *this; }
    uint64_t operator-(VirtualAddress o) const { return value - o.value; }

    bool operator==(VirtualAddress o) const { return value == o.value; }
    bool operator!=(VirtualAddress o) const { return value != o.value; }
    bool operator< (VirtualAddress o) const { return value <  o.value; }
    bool operator<=(VirtualAddress o) const { return value <= o.value; }
    bool operator> (VirtualAddress o) const { return value >  o.value; }
    bool operator>=(VirtualAddress o) const { return value >= o.value; }
};

// Strongly-typed PML4 reference.  Wraps a PhysicalAddress.
struct PageTable
{
    PhysicalAddress pml4;

    constexpr PageTable() : pml4() {}
    constexpr explicit PageTable(PhysicalAddress phys) : pml4(phys) {}

    explicit operator bool() const { return bool(pml4); }
    bool operator==(const PageTable& o) const { return pml4 == o.pml4; }
    bool operator!=(const PageTable& o) const { return pml4 != o.pml4; }
};

// Sentinel: resolves to the kernel PML4 (CR3 captured at boot).
inline constexpr PageTable KernelPageTable{};

} // namespace brook
