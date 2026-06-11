// dlopenllvm.c — minimal BRO-187 reproducer: dlopen libLLVM.so directly to
// isolate the global-constructor crash from Mesa. If this crashes the same way
// (user #PF in libLLVM .init_array), the bug is purely libLLVM-load on Brook.
#include <stdio.h>
#include <dlfcn.h>

#ifndef LLVM_SO
#define LLVM_SO "libLLVM.so.21.1"
#endif

int main(void)
{
    fprintf(stderr, "DLO: before dlopen %s\n", LLVM_SO);
    void* h = dlopen(LLVM_SO, RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "DLO: after dlopen -> %p\n", h);
    if (!h) { fprintf(stderr, "DLO: dlerror=%s\n", dlerror()); return 1; }
    fprintf(stderr, "DLO: DLOPEN_OK\n");
    return 0;
}
