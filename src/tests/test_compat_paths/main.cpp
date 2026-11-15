#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "compat_paths.h"
using namespace brook;

namespace {
// Fake namespace: directories (with entry counts), files and mounts.
struct Fake {
    std::set<std::string> dirs{"/"}, files, mounts;
    std::map<std::string, std::string> links;
    std::map<std::string, int> failMount, failSymlink;
    std::vector<std::string> events;
    bool HasEntries(const std::string& d) const {
        const std::string prefix = d + "/";
        for (auto* set : {&dirs, &files})
            for (auto& p : *set) if (p.rfind(prefix, 0) == 0) return true;
        for (auto& [p, t] : links) if (p.rfind(prefix, 0) == 0) return true;
        return false;
    }
};
Fake& F(void* c) { return *static_cast<Fake*>(c); }
int Probe(void* c, const char* p) {
    auto& f = F(c);
    if (f.dirs.count(p)) return 0;
    if (f.files.count(p) || f.links.count(p)) return -20;
    return -2;
}
int Mkdir(void* c, const char* p) { F(c).dirs.insert(p); return 0; }
int Entries(void* c, const char* p) { return F(c).HasEntries(p) ? 1 : 0; }
int Mount(void* c, const char* p) {
    auto& f = F(c);
    if (f.failMount.count(p)) return f.failMount[p];
    assert(f.dirs.count(p) && !f.HasEntries(p));
    f.mounts.insert(p);
    return 0;
}
int Symlink(void* c, const char* t, const char* p) {
    auto& f = F(c);
    if (f.failSymlink.count(p)) return f.failSymlink[p];
    assert(!f.links.count(p));
    f.links[p] = t;
    return 0;
}
void Report(void* c, const char* p, const char* e, int) { F(c).events.push_back(std::string(p) + " " + e); }
CompatPathOps Ops(Fake& f) { return {&f, Probe, Mkdir, Entries, Mount, Symlink, Report}; }
} // namespace

int main() {
    // Shipped table on the Dell/QEMU initrd layout: /usr exists, nothing else.
    {
        Fake f;
        f.dirs.insert("/usr");
        f.dirs.insert("/nix");
        auto s = CompatPathsProvision(kCompatPathLinks, kCompatPathLinkCount, Ops(f));
        assert(s.mounted == 2 && s.links == 2 && !s.skipped && !s.failed);
        assert(f.mounts == (std::set<std::string>{"/bin", "/usr/bin"}));
        assert(f.links.at("/bin/sh") == "/nix/profile/bin/sh");
        assert(f.links.at("/usr/bin/env") == "/nix/profile/bin/env");
    }
    // Missing ancestors are created; rows sharing a directory share one mount;
    // an existing empty directory is reused.
    {
        const CompatPathLink table[] = {
            {"/a/b/c", "x", "/t/x"}, {"/d", "y", "/t/y"}, {"/a/b/c", "z", "/t/z"}};
        Fake f;
        f.dirs.insert("/d");
        auto s = CompatPathsProvision(table, 3, Ops(f));
        assert(s.mounted == 2 && s.links == 3 && !s.failed && !s.skipped);
        assert(f.dirs.count("/a") && f.dirs.count("/a/b") && f.dirs.count("/a/b/c"));
        assert(f.links.at("/a/b/c/z") == "/t/z" && f.links.at("/d/y") == "/t/y");
    }
    // Never mask real content: a non-empty directory or a non-directory is kept.
    {
        const CompatPathLink table[] = {
            {"/lib64", "ld.so", "/t/ld"}, {"/bin", "sh", "/t/sh"}, {"/opt/bin", "x", "/t/x"}};
        Fake f;
        f.dirs.insert("/lib64");
        f.files.insert("/lib64/ld.so");
        f.files.insert("/bin");
        f.files.insert("/opt");
        auto s = CompatPathsProvision(table, 3, Ops(f));
        assert(!s.mounted && !s.links && s.skipped == 3 && !s.failed);
        assert(f.mounts.empty() && f.links.empty());
    }
    // Invalid rows are rejected without side effects; failures stay local.
    {
        const CompatPathLink table[] = {
            {"bin", "sh", "/t"}, {"/", "sh", "/t"}, {"/a/../b", "sh", "/t"}, {"/c/", "sh", "/t"},
            {"/e", "x/y", "/t"}, {"/e", "..", "/t"}, {"/e", "ok", "relative"}, {"/e", "fine", "/t/f"},
            {"/m", "sh", "/t"}, {"/s", "sh", "/t"}, {"/s", "env", "/t/env"}};
        Fake f;
        f.failMount["/m"] = -12;
        f.failSymlink["/s/sh"] = -28;
        auto s = CompatPathsProvision(table, 11, Ops(f));
        // /e mounts with one good link; /s mounts with one of two; /m fails.
        assert(s.mounted == 2 && s.links == 2 && !s.skipped);
        assert(s.failed == 4 + 1 + 1 + 1);
        assert(f.links.size() == 2 && f.links.at("/e/fine") == "/t/f" && f.links.at("/s/env") == "/t/env");
        assert(!f.dirs.count("/a") && !f.dirs.count("/b"));
    }
    std::puts("test_compat_paths: ok");
    return 0;
}
