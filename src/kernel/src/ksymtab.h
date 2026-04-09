#pragma once

#include <stdint.h>

// Kernel symbol export table — allows dynamically loaded modules to call
// kernel functions by name.
//
// Usage (in any kernel .cpp or .h):
//
//   EXPORT_SYMBOL(kmalloc);          // exports the function 'kmalloc'
//   EXPORT_SYMBOL_ALIAS(kfree, kfree); // explicit name
//
// The linker script collects all exported symbols between __start_ksymtab
// and __stop_ksymtab.  KsymLookup() does a linear scan (the table is small).
//
// NOTE: EXPORT_SYMBOL must be used in a .cpp file (not a header) to avoid
// duplicate definitions across translation units.

namespace brook {

struct KernelSymbol {
    const char*  name;
    const void*  addr;
};

// Look up a symbol by name.  Returns its address, or nullptr if not found.
const void* KsymLookup(const char* name);

// Print all exported symbols to serial (for debugging).
void KsymDump();

} // namespace brook

// ---- Export macros ----

// Export a symbol using its C++ linkage name.
// The __ksym_ prefix ensures the KernelSymbol struct variable doesn't
// collide with the function it describes.
#define EXPORT_SYMBOL(sym) \
    __attribute__((section(".ksymtab"), used)) \
    static const brook::KernelSymbol __ksym_##sym = { #sym, reinterpret_cast<const void*>(&sym) }

// Export a symbol with an explicit string name (useful when the C++ mangled
// name differs, or when exporting a non-function object).
#define EXPORT_SYMBOL_NAMED(sym, str_name) \
    __attribute__((section(".ksymtab"), used)) \
    static const brook::KernelSymbol __ksym_named_##sym = { str_name, reinterpret_cast<const void*>(&sym) }
