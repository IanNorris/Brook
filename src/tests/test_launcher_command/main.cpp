#undef NDEBUG
#include <cassert>
#include <cstring>
#include <initializer_list>
#include "../../kernel/src/launcher_command.h"

int main()
{
    using brook::LauncherExecutableStart;
    assert(!LauncherExecutableStart(nullptr));
    assert(!LauncherExecutableStart(" \t"));
    assert(!std::strcmp(LauncherExecutableStart("firefox --name firefox"), "firefox --name firefox"));
    assert(!std::strcmp(LauncherExecutableStart("--uid 1000 /nix/bin/brook-firefox --name firefox"),
                        "/nix/bin/brook-firefox --name firefox"));
    assert(!std::strcmp(LauncherExecutableStart(" \t--uid\t1000 \t\"/nix/stk-play.sh\""),
                        "\"/nix/stk-play.sh\""));
    assert(!std::strcmp(LauncherExecutableStart("--uid 2147483647 /app"), "/app"));
    for (const char* invalid : {"--uid", "--uid ", "--uid -1 /app", "--uid x /app",
         "--uid 1000", "--uid 1000/app", "--uid 2147483648 /app", "--uid 4294967296 /app"})
        assert(!LauncherExecutableStart(invalid));
}
