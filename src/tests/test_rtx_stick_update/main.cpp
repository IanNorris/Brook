// Boot-stick self-update core: manifest parsing, RAM verification, the
// install order with injected failures, and the loader's set choice.
#define RTX_STICK_TEST 1
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "../../../tools/rtx_stick_update.h"
#include "../../third_party/monocypher/monocypher-ed25519.h"

using Bytes = std::vector<uint8_t>;

static Bytes Elf(uint8_t tag) {
    Bytes e(4096, tag);
    memset(e.data(), 0, 120);
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    memcpy(e.data(), ident, sizeof(ident));
    e[16] = 2; e[18] = 62; BrookUpdatePut32(&e[20], 1);
    BrookUpdatePut64(&e[24], 0xffffffff80001000ull); // entry
    BrookUpdatePut64(&e[32], 64);                      // phoff
    e[52] = 64; e[54] = 56; e[56] = 1;
    uint8_t* p = &e[64];
    BrookUpdatePut32(p, 1); BrookUpdatePut32(p + 4, 5);
    BrookUpdatePut64(p + 8, 0); BrookUpdatePut64(p + 16, 0xffffffff80000000ull);
    BrookUpdatePut64(p + 32, 4096); BrookUpdatePut64(p + 40, 0x10000);
    return e;
}

static Bytes Set(const Bytes& k, const Bytes& i, const char* id) {
    Bytes r(BROOK_STICK_SET_BYTES, 0);
    memcpy(r.data(), "BRKSET01", 8);
    BrookUpdatePut64(&r[8], k.size()); BrookUpdatePut64(&r[16], i.size());
    BrookUpdateHash(k.data(), k.size(), &r[24]); BrookUpdateHash(i.data(), i.size(), &r[56]);
    memcpy(&r[88], id, strlen(id));
    BrookUpdatePut32(&r[136], BrookUpdateCrc(r.data(), 136));
    return r;
}

struct FileSet {
    std::string id;
    Bytes files[RTX_FILE_COUNT];
};

static FileSet MakeSet(const char* id, uint8_t tag, uint8_t loaderTag = 1) {
    FileSet s; s.id = id;
    s.files[RTX_LOADER] = Bytes(2048, loaderTag); s.files[RTX_LOADER][0] = 'M'; s.files[RTX_LOADER][1] = 'Z';
    s.files[RTX_KERNEL] = Elf(tag);
    s.files[RTX_INITRD] = Bytes(8192, tag); s.files[RTX_INITRD][510] = 0x55; s.files[RTX_INITRD][511] = 0xaa;
    const std::string cfg = "TARGET=KERNEL\\BROOK.ELF\nSTICK_SETS=1\n";
    s.files[RTX_CFG] = Bytes(cfg.begin(), cfg.end());
    s.files[RTX_README] = Bytes(100, 'r');
    const std::string json = std::string("{\"payload_id\":\"") + id + "\"}\n";
    s.files[RTX_MANIFEST_JSON] = Bytes(json.begin(), json.end());
    s.files[RTX_SET] = Set(s.files[RTX_KERNEL], s.files[RTX_INITRD], id);
    return s;
}

static Bytes Manifest(const FileSet& s, const char* label = "BROOK_RTX") {
    Bytes m(RTX_MANIFEST_BYTES, 0);
    memcpy(m.data(), "BRKRTXM1", 8); BrookUpdatePut32(&m[8], 1); BrookUpdatePut32(&m[12], RTX_FILE_COUNT);
    memcpy(&m[16], s.id.data(), s.id.size()); memcpy(&m[64], label, strlen(label)); memcpy(&m[80], "abc", 3);
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        uint8_t* e = &m[RTX_MANIFEST_HEADER + i * RTX_MANIFEST_ENTRY];
        memcpy(e, RTX_NAMES[i], strlen(RTX_NAMES[i]));
        BrookUpdatePut64(e + 40, s.files[i].size());
        BrookUpdateHash(s.files[i].data(), s.files[i].size(), e + 48);
    }
    return m;
}

static std::string Root() {
    char tmpl[] = "/tmp/rtx-stick-XXXXXX";
    assert(mkdtemp(tmpl));
    return tmpl;
}

static void Put(const std::string& path, const Bytes& data) {
    FILE* f = fopen(path.c_str(), "wb"); assert(f);
    assert(fwrite(data.data(), 1, data.size(), f) == data.size()); fclose(f);
}

static Bytes Get(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    Bytes out; int c;
    while ((c = fgetc(f)) != EOF) out.push_back(uint8_t(c));
    fclose(f);
    return out;
}

static bool Exists(const std::string& path) { struct stat st; return !stat(path.c_str(), &st); }

static void Install(const std::string& root, const FileSet& s) {
    mkdir((root + "/EFI").c_str(), 0755); mkdir((root + "/EFI/BOOT").c_str(), 0755); mkdir((root + "/KERNEL").c_str(), 0755);
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) Put(root + "/" + RTX_NAMES[i], s.files[i]);
}

// What STICK_SETS loader code would boot (mirror of stick_sets.cpp).
static std::string LoaderChoice(const std::string& root) {
    const bool pending = Exists(root + "/TRIAL.BIN"), run = Exists(root + "/TRIAL.RUN");
    const int first = BrookStickFirstSet(pending, run);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool prev = (attempt == 0) == (first == BROOK_STICK_PREV);
        const Bytes set = Get(root + (prev ? "/PREV/SET.BIN" : "/SET.BIN"));
        const Bytes k = Get(root + (prev ? "/PREV/BROOK.ELF" : "/KERNEL/BROOK.ELF"));
        const Bytes i = Get(root + (prev ? "/PREV/INITRD.IMG" : "/INITRD.IMG"));
        if (set.size() == BROOK_STICK_SET_BYTES && !k.empty() && !i.empty() &&
            BrookStickSetMatches(set.data(), k.data(), k.size(), i.data(), i.size()))
            return std::string(prev ? "PREV:" : "root:") + BrookStickSetPayloadId(set.data());
    }
    return "none";
}

static int Apply(const std::string& root, const FileSet& s, int running, RtxReceipt* r, const char* fail = nullptr) {
    if (fail) setenv("RTX_TEST_FAIL_AT", fail, 1); else unsetenv("RTX_TEST_FAIL_AT");
    Bytes m = Manifest(s);
    RtxManifest parsed;
    assert(RtxManifestParse(m.data(), m.size(), &parsed) == RTX_OK);
    uint8_t* files[RTX_FILE_COUNT];
    Bytes copy[RTX_FILE_COUNT];
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) { copy[i] = s.files[i]; files[i] = copy[i].data(); }
    assert(RtxContentValid(&parsed, files) == RTX_OK);
    return RtxApply(root.c_str(), &parsed, files, running, r);
}

int main() {
    const FileSet a = MakeSet("brook-rtx-smoke-aaaa", 0x11), b = MakeSet("brook-rtx-smoke-bbbb", 0x22);

    // Manifest structure, label and names.
    {
        RtxManifest m;
        Bytes good = Manifest(b);
        assert(RtxManifestParse(good.data(), good.size(), &m) == RTX_OK && m.payload_id == std::string("brook-rtx-smoke-bbbb"));
        Bytes wrong = Manifest(b, "BROOK");
        assert(RtxManifestParse(wrong.data(), wrong.size(), &m) == RTX_E_LABEL);
        Bytes renamed = good; renamed[RTX_MANIFEST_HEADER + RTX_MANIFEST_ENTRY + 1] = 'X';
        assert(RtxManifestParse(renamed.data(), renamed.size(), &m) == RTX_E_MANIFEST);
        assert(RtxManifestParse(good.data(), good.size() - 1, &m) == RTX_E_MANIFEST);
        Bytes huge = good; BrookUpdatePut64(&huge[RTX_MANIFEST_HEADER + 40], RTX_TOTAL_MAX + 1ull);
        assert(RtxManifestParse(huge.data(), huge.size(), &m) == RTX_E_MANIFEST);
    }
    // RAM verification: digests, kernel format, SET.BIN consistency, STICK_SETS.
    {
        Bytes m = Manifest(b); RtxManifest parsed; RtxManifestParse(m.data(), m.size(), &parsed);
        for (unsigned bad = 0; bad < RTX_FILE_COUNT; ++bad) {
            Bytes copy[RTX_FILE_COUNT]; uint8_t* files[RTX_FILE_COUNT];
            for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) { copy[i] = b.files[i]; files[i] = copy[i].data(); }
            copy[bad][copy[bad].size() / 2] ^= 0x40;
            assert(RtxContentValid(&parsed, files) == RTX_E_HASH);
        }
        FileSet noSets = b; const std::string cfg = "TARGET=KERNEL\\BROOK.ELF\n";
        noSets.files[RTX_CFG] = Bytes(cfg.begin(), cfg.end());
        Bytes m2 = Manifest(noSets); RtxManifestParse(m2.data(), m2.size(), &parsed);
        uint8_t* files[RTX_FILE_COUNT];
        for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) files[i] = noSets.files[i].data();
        assert(RtxContentValid(&parsed, files) == RTX_E_CONTENT);
        FileSet mismatch = b; mismatch.files[RTX_SET] = a.files[RTX_SET];
        Bytes m3 = Manifest(mismatch); RtxManifestParse(m3.data(), m3.size(), &parsed);
        for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) files[i] = mismatch.files[i].data();
        assert(RtxContentValid(&parsed, files) == RTX_E_CONTENT);
    }
    // Signature domain: Ed25519 over domain || manifest; one flipped bit fails.
    {
        uint8_t seed[32]; for (unsigned i = 0; i < 32; ++i) seed[i] = uint8_t(i * 7 + 1);
        uint8_t secret[64], pub[32], sig[64];
        crypto_ed25519_key_pair(secret, pub, seed);
        Bytes m = Manifest(b), msg(reinterpret_cast<const uint8_t*>(RTX_SIGN_DOMAIN), reinterpret_cast<const uint8_t*>(RTX_SIGN_DOMAIN) + RTX_SIGN_DOMAIN_BYTES);
        assert(msg.size() == 28 && msg.back() == 0);
        msg.insert(msg.end(), m.begin(), m.end());
        crypto_ed25519_sign(sig, secret, msg.data(), msg.size());
        assert(!crypto_ed25519_check(sig, pub, msg.data(), msg.size()));
        msg[40] ^= 1; assert(crypto_ed25519_check(sig, pub, msg.data(), msg.size()));
    }
    // Loader decision table.
    assert(BrookStickFirstSet(0, 0) == BROOK_STICK_ROOT && BrookStickFirstSet(1, 0) == BROOK_STICK_ROOT &&
           BrookStickFirstSet(0, 1) == BROOK_STICK_PREV && BrookStickFirstSet(1, 1) == BROOK_STICK_ROOT);

    // Successful update from a root boot: PREV = old set, trial pending.
    {
        const std::string root = Root(); Install(root, a);
        assert(LoaderChoice(root) == "root:brook-rtx-smoke-aaaa");
        RtxReceipt r;
        assert(Apply(root, b, BROOK_STICK_ROOT, &r) == RTX_OK);
        assert(r.prev_rebuilt && !r.loader_updated && r.unchanged == 3 /* loader, cfg, readme */ &&
               r.prev_payload_id == std::string("brook-rtx-smoke-aaaa"));
        assert(Get(root + "/PREV/BROOK.ELF") == a.files[RTX_KERNEL] && Get(root + "/PREV/BOOTX64.EFI") == a.files[RTX_LOADER]);
        assert(Exists(root + "/TRIAL.BIN") && !Exists(root + "/KERNEL/BROOK.ELF.new"));
        assert(LoaderChoice(root) == "root:brook-rtx-smoke-bbbb");
        RtxManifest m; Bytes raw = Manifest(b); RtxManifestParse(raw.data(), raw.size(), &m);
        assert(RtxVerifyInstalled(root.c_str(), &m) == RTX_OK);
        // Loader consumes the marker; an unconfirmed trial falls back to PREV.
        rename((root + "/TRIAL.BIN").c_str(), (root + "/TRIAL.RUN").c_str());
        assert(LoaderChoice(root) == "PREV:brook-rtx-smoke-aaaa");
        // From that PREV boot a further update keeps PREV (the good set).
        const FileSet c = MakeSet("brook-rtx-smoke-cccc", 0x33, 2);
        assert(Apply(root, c, BROOK_STICK_PREV, &r) == RTX_OK && !r.prev_rebuilt && r.loader_updated);
        assert(Get(root + "/PREV/BROOK.ELF") == a.files[RTX_KERNEL] && !Exists(root + "/TRIAL.RUN"));
        assert(LoaderChoice(root) == "root:brook-rtx-smoke-cccc");
        assert(Get(root + "/EFI/BOOT/BOOTX64.EFI") == c.files[RTX_LOADER]);
    }
    // Failures before the swap leave the root set and loader untouched.
    for (const char* step : {"stage", "prev", "trial"}) {
        const std::string root = Root(); Install(root, a);
        RtxReceipt r;
        const FileSet c = MakeSet("brook-rtx-smoke-cccc", 0x33, 2);
        assert(Apply(root, c, BROOK_STICK_ROOT, &r, step) != RTX_OK && r.failed_step[0]);
        for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) assert(Get(root + "/" + RTX_NAMES[i]) == a.files[i]);
        assert(!Exists(root + "/TRIAL.BIN") && !Exists(root + "/KERNEL/BROOK.ELF.new"));
        assert(LoaderChoice(root) == "root:brook-rtx-smoke-aaaa");
    }
    // A failure in the swap (new kernel+initrd, old SET.BIN): loader boots PREV,
    // which is the running set; the old loader is still in place.
    {
        const std::string root = Root(); Install(root, a);
        RtxReceipt r;
        const FileSet c = MakeSet("brook-rtx-smoke-cccc", 0x33, 2);
        assert(Apply(root, c, BROOK_STICK_ROOT, &r, "swap") == RTX_E_SWAP);
        assert(Get(root + "/EFI/BOOT/BOOTX64.EFI") == a.files[RTX_LOADER] && Exists(root + "/TRIAL.BIN"));
        assert(LoaderChoice(root) == "PREV:brook-rtx-smoke-aaaa");
    }
    unsetenv("RTX_TEST_FAIL_AT");
    puts("RTX_STICK_UPDATE_PASS");
}
