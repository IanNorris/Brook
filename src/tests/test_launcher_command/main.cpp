#undef NDEBUG
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <string>
#include "../../kernel/src/launcher_command.h"

int main()
{
    using brook::LauncherExecutableStart;
    auto identity=[](const char* domain,const char* key){
        brook::LauncherIdentity id;id.field(domain);id.field(key);char out[33];id.finish(out);return std::string(out);
    };
    assert(identity("desktop","org.Editor")==identity("desktop","org.Editor"));
    assert(identity("desktop","org.Editor")!=identity("desktop","org.editor"));
    assert(identity("desktop","same")!=identity("shortcut","same"));
    assert(identity("ab","c")!=identity("a","bc"));
    assert(identity("desktop","old-command").size()==32);
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
