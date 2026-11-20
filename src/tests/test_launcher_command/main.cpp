#undef NDEBUG
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "../../kernel/src/launcher_command.h"
#include "../../kernel/src/launcher_user_manifest.h"

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
    using namespace brook;
    auto record = [](const std::string& title, const std::string& command, const std::string& id,
                     const std::string& icon = "") {
        return title + "\t" + command + "\t" + icon + "\tNetwork;\t" + id;
    };
    UserLauncherEntry entry{};
    auto parse = [&](const std::string& line) { return ParseUserLauncherEntry(line.data(), line.size(), entry); };
    assert(UserLauncherManifestMode(0100600, DesktopUid));
    assert(UserLauncherManifestMode(0100644, DesktopUid));
    assert(!UserLauncherManifestMode(0100600, 0));
    for (auto mode : {0100666, 0100640, 0100700, 0101600, 0120600, 0010600, 0040600})
        assert(!UserLauncherManifestMode(mode, DesktopUid));

    assert(!parse(record("Web Café", "brook-chromium --app=https://example.org/#section",
                         "org.Brook.Web", "web.rgba")));
    assert(!std::strcmp(entry.command, "--uid 1000 /nix/profile/bin/brook-chromium --app=https://example.org/#section"));
    assert(!std::strcmp(entry.executable, "brook-chromium"));
    assert(!std::strcmp(entry.icon, "web.rgba"));
    assert(!parse(record("Flatpak", "/nix/profile/bin/flatpak run org.App.Test", "org.App.Test")));
    assert(!std::strcmp(entry.command, "--uid 1000 /nix/profile/bin/flatpak run org.App.Test"));

    for (const char* command : {"--uid 0 flatpak", "--gid 0 flatpak", "--as root flatpak",
         "--service flatpak", "-flatpak", "/nix/store/build/bin/flatpak", "/nix/bin/flatpak", "/data/home/flatpak",
         "./flatpak", "/nix/profile/bin/../flatpak", "/nix/profile/bin//flatpak", "/nix/profile/bin/sub/flatpak",
         "/nix/profile/bin/.", "/nix/profile/bin/..", "/nix/profile/bin/--once", "../flatpak",
         "flatpak --uid=0", "flatpak --gid 0", "flatpak --as=guest", "flatpak --strace",
         "flatpak --strace-errors", "flatpak --service", "flatpak --once",
         "flatpak --quiet", "flatpak --console", "flatpak #comment",
         "flatpak \"space argument\"", "flatpak 'space argument'", "flatpak slash\\escape",
         "flatpak tab\targ", "flatpak line\narg", "flatpak line\rarg", "flatpak \x1b",
         " flatpak", "flatpak ñ", "", ".", "#comment"}) {
        assert(parse(record("Invalid", command, "org.App.Bad")));
    }
    for (const char* icon : {".", "..", "a..b", "/home/icon.rgba", "../icon.rgba",
                             "sub/icon.rgba", "sub\\icon.rgba", "bad\x1b.rgba"}) {
        assert(parse(record("Invalid", "flatpak", "org.App.Bad", icon)));
    }
    assert(parse(record("Leading ", "flatpak", "org.App.Bad")));
    assert(parse(record("Invalid", "flatpak", "")));
    assert(parse(record("Invalid", "flatpak", "org.App.Bad/")));
    assert(parse(record("Invalid", "flatpak", std::string(48, 'A'))));
    assert(parse(record("Invalid", "flatpak", std::string(1025, 'A'))));
    assert(parse(record("Invalid", "flatpak", "org.App.Bad") + "\textra"));
    assert(parse("Malformed\tflatpak"));
    assert(parse(record("Invalid", std::string("flatpak\0 hidden", 15), "org.App.Bad")));
    assert(parse(record("Invalid", "flatpak " + std::string(120, 'a'), "org.App.Bad")));
    std::string manyArgs = "flatpak";
    for (unsigned i = 0; i < 28; ++i) manyArgs += " x";
    assert(!parse(record("Valid", manyArgs, "org.App.Good")));
    assert(parse(record("Invalid", manyArgs + " x", "org.App.Bad")));

    struct CatalogEntry { std::string title, id; };
    std::vector<CatalogEntry> items{{"Files", "org.gnome.Nautilus"}, {"Quake", "quake"}};
    auto merge = [&](const UserLauncherEntry& item) -> const char* {
        for (const auto& existing : items)
            if (UserLauncherConflicts(item, existing.title.c_str(), existing.id.c_str()))
                return "duplicate";
        if (std::strcmp(item.executable, "flatpak")) return "unavailable";
        items.push_back({item.title, item.appId});
        return nullptr;
    };
    const auto bytes = record("Invalid", "--uid 0 flatpak", "org.App.Bad") + "\n" +
        record("fIlEs", "flatpak", "org.App.New") + "\n" +
        record("Fake game", "flatpak", "QUAKE") + "\n" +
        record("Only in /nix/bin", "missing", "org.App.Missing") + "\n" +
        record("Accepted", "flatpak", "org.App.Good") + "\r\n" +
        record("Duplicate accepted ID", "flatpak", "org.App.Good") + "\n" +
        std::string(1025, 'A') + "\n" +
        record("Last", "flatpak", "org.App.Last");
    std::vector<unsigned> rejected;
    auto counts = LoadUserLauncherRecords(bytes.data(), bytes.size(), merge,
        [&](uint32_t line, const char*) { rejected.push_back(line); });
    assert(counts.entries == 2 && counts.rejected == 6);
    assert((rejected == std::vector<unsigned>{1, 2, 3, 4, 6, 7}));
    assert(items[0].title == "Files" && items[1].title == "Quake");
    assert(items[2].title == "Accepted" && items[3].title == "Last");

    std::string full;
    for (unsigned i = 0; i < UserLauncherEntryLimit + 3; ++i)
        full += record("App " + std::to_string(i), "flatpak", "org.App." + std::to_string(i)) + "\n";
    counts = LoadUserLauncherRecords(full.data(), full.size(), merge, [](uint32_t, const char*) {});
    assert(counts.entries == UserLauncherEntryLimit && counts.rejected == 3);

    UserLauncherReload reload;
    assert(!reload.Take(0));
    reload.Request();
    assert(reload.Take(0));
    reload.Request();
    reload.Request();
    assert(!reload.Take(999));
    assert(reload.Take(1000));
    assert(!reload.Take(2000));
}
