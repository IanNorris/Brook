#include "../../kernel/src/busybox_applets.h"
#include <cstdio>
#include <initializer_list>

int main()
{
    for (const char* name : {"sh", "env", "ls", "cat", "true", "false"}) {
        if (!BusyboxHasApplet(name)) {
            std::fprintf(stderr, "Missing BusyBox applet: %s\n", name);
            return 1;
        }
    }
    // PATH search must reach real Git/LLVM tools instead of a fake applet.
    for (const char* name : {"", "git", "git-remote-https", "git-submodule",
                            "clang", "cmake", "ls-extra", "not-a-command"}) {
        if (BusyboxHasApplet(name)) {
            std::fprintf(stderr, "False BusyBox applet: %s\n", name);
            return 1;
        }
    }
}
