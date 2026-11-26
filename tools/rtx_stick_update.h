/* Boot-stick self-update core (RAM-only smoke stick). Header-only C shared by
 * the guest helper (tools/hardware_rtx_update.c) and host tests.
 *
 * Signed binary manifest ("BRKRTXM1"):
 *   header 128 B: magic[8] version u32(=1) count u32(=RTX_FILE_COUNT)
 *                 payload_id[48] fat_label[16] commit[48]   (NUL padded)
 *   entries count x 80 B: name[40] bytes u64 sha256[32], in RTX_NAMES order
 * Signature: Ed25519 over RTX_SIGN_DOMAIN || manifest (key held host-side).
 *
 * Install order under ROOT (the armed boot-stick mount):
 *   1. every changed file -> "<name>.new", fsync, re-read and hash
 *   2. running set is root: rebuild PREV/ as a verified copy of it, with
 *      PREV/SET.BIN removed first and written last (running set PREV: keep it)
 *   3. TRIAL.BIN created (one unconfirmed boot of the new set)
 *   4. rename *.new over the root files, SET.BIN after the kernel/initrd,
 *      EFI/BOOT/BOOTX64.EFI last and only when it changed
 * A failure in 1-2 leaves the root set untouched (the *.new files are
 * removed). A failure in 4 leaves TRIAL.BIN with a root set that no longer
 * verifies against SET.BIN, so the loader boots PREV/ (the running set).
 */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/update/payload.h"
#include "../src/shared/update/executable.h"
#include "../src/shared/update/boot_compression_magic.h"
#include "../src/shared/update/stick_set.h"

#define RTX_MANIFEST_HEADER 128u
#define RTX_MANIFEST_ENTRY 80u
#define RTX_FILE_COUNT 7u
#define RTX_MANIFEST_BYTES (RTX_MANIFEST_HEADER + RTX_FILE_COUNT * RTX_MANIFEST_ENTRY)
#define RTX_SIGN_DOMAIN "BROOK-RTX-STICK-MANIFEST-V1"
#define RTX_SIGN_DOMAIN_BYTES 28u /* including the terminating NUL */
#define RTX_TOTAL_MAX (192u * 1024u * 1024u)

enum { RTX_LOADER = 0, RTX_KERNEL, RTX_INITRD, RTX_CFG, RTX_README, RTX_MANIFEST_JSON, RTX_SET };
static const char* const RTX_NAMES[RTX_FILE_COUNT] = {
    "EFI/BOOT/BOOTX64.EFI", "KERNEL/BROOK.ELF", "INITRD.IMG", "BROOK.CFG", "README.TXT", "manifest.json", "SET.BIN"};
/* PREV/ copy of each root file (the loader reads PREV/BROOK.ELF, PREV/INITRD.IMG, PREV/SET.BIN). */
static const char* const RTX_PREV_NAMES[RTX_FILE_COUNT] = {
    "PREV/BOOTX64.EFI", "PREV/BROOK.ELF", "PREV/INITRD.IMG", "PREV/BROOK.CFG", "PREV/README.TXT",
    "PREV/manifest.json", "PREV/SET.BIN"};
/* Root swap order: SET.BIN after the pair it describes, loader last. */
static const unsigned RTX_SWAP_ORDER[RTX_FILE_COUNT] = {
    RTX_KERNEL, RTX_INITRD, RTX_SET, RTX_README, RTX_MANIFEST_JSON, RTX_CFG, RTX_LOADER};

typedef struct {
    char payload_id[49], label[17], commit[49];
    uint64_t bytes[RTX_FILE_COUNT];
    uint8_t sha[RTX_FILE_COUNT][32];
} RtxManifest;

typedef enum {
    RTX_OK = 0, RTX_E_MANIFEST = 40, RTX_E_SIGNATURE, RTX_E_LABEL, RTX_E_HASH, RTX_E_CONTENT,
    RTX_E_STAGE, RTX_E_PREV, RTX_E_TRIAL, RTX_E_SWAP, RTX_E_VERIFY, RTX_E_RUNNING
} RtxError;

typedef struct {
    unsigned written;            /* files staged and swapped */
    unsigned unchanged;          /* files already identical */
    int loader_updated;
    int prev_rebuilt;            /* 0: PREV kept (running set was PREV) */
    char failed_step[48];
    char prev_payload_id[49];
} RtxReceipt;

static inline int RtxText(const uint8_t* p, unsigned n, char* out, int allowEmpty) {
    unsigned len = 0;
    while (len < n && p[len]) {
        const uint8_t c = p[len];
        if (c < 0x21 || c > 0x7e) return 0;
        ++len;
    }
    if (len == n || (!len && !allowEmpty)) return 0;
    for (unsigned i = len; i < n; ++i) if (p[i]) return 0;
    memcpy(out, p, len); out[len] = 0;
    return 1;
}

static inline int RtxManifestParse(const uint8_t* m, size_t n, RtxManifest* out) {
    memset(out, 0, sizeof(*out));
    if (n != RTX_MANIFEST_BYTES || memcmp(m, "BRKRTXM1", 8) || BrookUpdateGet32(m + 8) != 1 ||
        BrookUpdateGet32(m + 12) != RTX_FILE_COUNT) return RTX_E_MANIFEST;
    if (!RtxText(m + 16, 48, out->payload_id, 0) || !RtxText(m + 64, 16, out->label, 0) ||
        !RtxText(m + 80, 48, out->commit, 1)) return RTX_E_MANIFEST;
    uint64_t total = 0;
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        const uint8_t* e = m + RTX_MANIFEST_HEADER + i * RTX_MANIFEST_ENTRY;
        char name[41];
        if (!RtxText(e, 40, name, 0) || strcmp(name, RTX_NAMES[i])) return RTX_E_MANIFEST;
        out->bytes[i] = BrookUpdateGet64(e + 40);
        memcpy(out->sha[i], e + 48, 32);
        if (!out->bytes[i] || out->bytes[i] > RTX_TOTAL_MAX) return RTX_E_MANIFEST;
        total += out->bytes[i];
    }
    if (total > RTX_TOTAL_MAX) return RTX_E_MANIFEST;
    if (strcmp(out->label, "BROOK_RTX")) return RTX_E_LABEL;
    return RTX_OK;
}

/* Content checks on the RAM copies, after their digests matched. */
static inline int RtxContentValid(const RtxManifest* m, uint8_t* const files[RTX_FILE_COUNT]) {
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        uint8_t h[32];
        BrookUpdateHash(files[i], (BrookUpdateSize)m->bytes[i], h);
        if (!BrookUpdateEqual(h, m->sha[i], 32)) return RTX_E_HASH;
    }
    if (m->bytes[RTX_LOADER] < 512 || files[RTX_LOADER][0] != 'M' || files[RTX_LOADER][1] != 'Z') return RTX_E_CONTENT;
    if (m->bytes[RTX_KERNEL] > BROOK_STICK_SET_KERNEL_MAX ||
        !BrookUpdateExecutable(files[RTX_KERNEL], (BrookUpdateSize)m->bytes[RTX_KERNEL])) return RTX_E_CONTENT;
    if (m->bytes[RTX_INITRD] < 512 || BrookBootCompressed(files[RTX_INITRD], m->bytes[RTX_INITRD]) ||
        files[RTX_INITRD][510] != 0x55 || files[RTX_INITRD][511] != 0xaa) return RTX_E_CONTENT;
    if (m->bytes[RTX_SET] != BROOK_STICK_SET_BYTES ||
        !BrookStickSetMatches(files[RTX_SET], files[RTX_KERNEL], m->bytes[RTX_KERNEL],
                              files[RTX_INITRD], m->bytes[RTX_INITRD]) ||
        strcmp(BrookStickSetPayloadId(files[RTX_SET]), m->payload_id)) return RTX_E_CONTENT;
    /* The new loader must keep booting file sets. */
    const uint8_t* cfg = files[RTX_CFG];
    const char* want = "STICK_SETS=1";
    int found = 0;
    for (uint64_t i = 0; i + 12 <= m->bytes[RTX_CFG]; ++i)
        if (!memcmp(cfg + i, want, 12) && (i == 0 || cfg[i - 1] == '\n')) found = 1;
    return found ? RTX_OK : RTX_E_CONTENT;
}

/* ---- file helpers on ROOT-relative paths ---- */
static inline void RtxPath(char* out, size_t cap, const char* root, const char* name, const char* suffix) {
    snprintf(out, cap, "%s/%s%s", root, name, suffix ? suffix : "");
}

static inline int RtxWriteFile(const char* path, const uint8_t* data, uint64_t length) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    int result = 0;
    while (length) {
        const size_t chunk = length > 65536 ? 65536 : (size_t)length;
        const ssize_t n = write(fd, data, chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = -1; break; }
        data += n; length -= (uint64_t)n;
    }
    if (!result && fsync(fd)) result = -1;
    if (close(fd)) result = -1;
    return result;
}

/* Fresh open, exact length, digest. 1 match, 0 differs or missing, -1 I/O. */
static inline int RtxFileMatches(const char* path, uint64_t length, const uint8_t sha[32]) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    BrookSha256 hash; BrookShaInit(&hash);
    static uint8_t chunk[65536];
    uint64_t total = 0; int result = 1;
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result = -1; break; }
        if (!n) break;
        total += (uint64_t)n;
        if (total > length) { result = 0; break; }
        BrookShaAdd(&hash, chunk, (BrookUpdateSize)n);
    }
    close(fd);
    if (result != 1) return result;
    if (total != length) return 0;
    uint8_t h[32]; BrookShaFinish(&hash, h);
    return BrookUpdateEqual(h, sha, 32);
}

/* Brook's FAT never replaces an existing target (no atomic replace on FAT):
 * remove it first. The gap is covered by the set fallback described above. */
static inline int RtxReplace(const char* from, const char* to) {
    if (unlink(to) && errno != ENOENT) return -1;
    return rename(from, to);
}

/* Whole file into RAM (bounded); caller frees. */
static inline uint8_t* RtxReadFile(const char* path, uint64_t limit, uint64_t* length) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > limit) { close(fd); return NULL; }
    uint8_t* data = (uint8_t*)malloc(st.st_size ? (size_t)st.st_size : 1);
    uint64_t done = 0;
    while (data && done < (uint64_t)st.st_size) {
        const ssize_t n = read(fd, data + done, (size_t)((uint64_t)st.st_size - done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); data = NULL; break; }
        done += (uint64_t)n;
    }
    close(fd);
    if (data) *length = done;
    return data;
}

#ifdef RTX_STICK_TEST
static inline int RtxFailAt(const char* step) {
    const char* v = getenv("RTX_TEST_FAIL_AT");
    return v && !strcmp(v, step);
}
#else
static inline int RtxFailAt(const char* step) { (void)step; return 0; }
#endif

static inline void RtxCleanupStaged(const char* root) {
    char path[256];
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        RtxPath(path, sizeof(path), root, RTX_NAMES[i], ".new"); unlink(path);
        RtxPath(path, sizeof(path), root, RTX_PREV_NAMES[i], ".new"); unlink(path);
    }
}

#define RTX_FAIL(code, step) do { snprintf(receipt->failed_step, sizeof(receipt->failed_step), "%s", step); \
    result = (code); goto fail; } while (0)

// Read only the discriminator. A short or unreadable running file cannot
// establish that the replacement loader will retain a bootable fallback.
static inline int RtxRawPayload(const char* path) {
    uint8_t prefix[BROOK_BOOT_COMPRESSED_MAGIC_BYTES];
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t length = 0;
    while (length < sizeof(prefix)) {
        const ssize_t got = read(fd, prefix + length, sizeof(prefix) - length);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        length += (size_t)got;
    }
    if (close(fd) || length != sizeof(prefix)) return -1;
    return !BrookBootCompressed(prefix, length);
}

/* runningSet: BROOK_STICK_ROOT or BROOK_STICK_PREV (the set the loader booted;
 * the caller's kernel gate has proven it is on this volume). */
static inline int RtxApply(const char* root, const RtxManifest* m, uint8_t* const files[RTX_FILE_COUNT],
                           int runningSet, RtxReceipt* receipt) {
    memset(receipt, 0, sizeof(*receipt));
    char path[256], dest[256];
    int changed[RTX_FILE_COUNT];
    int result = RTX_OK, swapping = 0;
    if (runningSet != BROOK_STICK_ROOT && runningSet != BROOK_STICK_PREV) return RTX_E_RUNNING;
    /* This receiver installs raw sets and cannot attest the incoming loader's
     * pinned key. Refuse before staging if its preserved fallback needs one. */
    for (unsigned i = RTX_KERNEL; i <= RTX_INITRD; ++i) {
        RtxPath(path, sizeof(path), root, runningSet == BROOK_STICK_ROOT ? RTX_NAMES[i] : RTX_PREV_NAMES[i], NULL);
        const int raw = RtxRawPayload(path);
        if (raw != 1) {
            snprintf(receipt->failed_step, sizeof(receipt->failed_step), "%s",
                     raw == 0 ? "compressed running set (offline update only)" : "read running payload prefix");
            return RTX_E_PREV;
        }
    }
    /* 1. stage */
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        RtxPath(path, sizeof(path), root, RTX_NAMES[i], NULL);
        const int same = RtxFileMatches(path, m->bytes[i], m->sha[i]);
        if (same < 0) RTX_FAIL(RTX_E_STAGE, "read current file");
        changed[i] = !same;
        if (same) { ++receipt->unchanged; continue; }
        RtxPath(path, sizeof(path), root, RTX_NAMES[i], ".new");
        if (RtxWriteFile(path, files[i], m->bytes[i]) || RtxFailAt("stage"))
            RTX_FAIL(RTX_E_STAGE, "write staged file");
        if (RtxFileMatches(path, m->bytes[i], m->sha[i]) != 1) RTX_FAIL(RTX_E_STAGE, "staged readback");
    }
    /* 2. PREV <- running root set (a copy; the root set stays bootable) */
    if (runningSet == BROOK_STICK_ROOT) {
        uint64_t setBytes = 0;
        RtxPath(path, sizeof(path), root, "SET.BIN", NULL);
        uint8_t* set = RtxReadFile(path, BROOK_STICK_SET_BYTES, &setBytes);
        if (!set || setBytes != BROOK_STICK_SET_BYTES || !BrookStickSetValid(set)) { free(set); RTX_FAIL(RTX_E_PREV, "root SET.BIN"); }
        snprintf(receipt->prev_payload_id, sizeof(receipt->prev_payload_id), "%s", BrookStickSetPayloadId(set));
        free(set);
        RtxPath(path, sizeof(path), root, "PREV", NULL);
        if (mkdir(path, 0755) && errno != EEXIST) RTX_FAIL(RTX_E_PREV, "mkdir PREV");
        RtxPath(path, sizeof(path), root, "PREV/SET.BIN", NULL);
        if (unlink(path) && errno != ENOENT) RTX_FAIL(RTX_E_PREV, "invalidate PREV");
        const unsigned order[RTX_FILE_COUNT] = {RTX_LOADER, RTX_KERNEL, RTX_INITRD, RTX_CFG, RTX_README,
                                                RTX_MANIFEST_JSON, RTX_SET};
        for (unsigned k = 0; k < RTX_FILE_COUNT; ++k) {
            const unsigned i = order[k];
            uint64_t length = 0;
            RtxPath(path, sizeof(path), root, RTX_NAMES[i], NULL);
            uint8_t* data = RtxReadFile(path, RTX_TOTAL_MAX, &length);
            if (!data) {
                if (i == RTX_README || i == RTX_MANIFEST_JSON) continue; /* optional in an old set */
                RTX_FAIL(RTX_E_PREV, "read running file");
            }
            uint8_t sha[32]; BrookUpdateHash(data, (BrookUpdateSize)length, sha);
            RtxPath(dest, sizeof(dest), root, RTX_PREV_NAMES[i], ".new");
            const int wrote = RtxWriteFile(dest, data, length);
            free(data);
            if (wrote || RtxFileMatches(dest, length, sha) != 1 || RtxFailAt("prev"))
                RTX_FAIL(RTX_E_PREV, "copy running file to PREV");
            RtxPath(path, sizeof(path), root, RTX_PREV_NAMES[i], NULL);
            if (RtxReplace(dest, path)) RTX_FAIL(RTX_E_PREV, "publish PREV file");
        }
        receipt->prev_rebuilt = 1;
    }
    /* 3. one trial boot of the new set */
    RtxPath(path, sizeof(path), root, "TRIAL.RUN", NULL); unlink(path);
    RtxPath(path, sizeof(path), root, "TRIAL.BIN", NULL);
    if (RtxWriteFile(path, (const uint8_t*)m->payload_id, strlen(m->payload_id)) || RtxFailAt("trial"))
        RTX_FAIL(RTX_E_TRIAL, "write TRIAL.BIN");
    /* 4. swap */
    for (unsigned k = 0; k < RTX_FILE_COUNT; ++k) {
        const unsigned i = RTX_SWAP_ORDER[k];
        if (!changed[i]) continue;
        if (i == RTX_SET && RtxFailAt("swap")) RTX_FAIL(RTX_E_SWAP, "rename staged file");
        RtxPath(path, sizeof(path), root, RTX_NAMES[i], ".new");
        RtxPath(dest, sizeof(dest), root, RTX_NAMES[i], NULL);
        swapping = 1; /* from here the root set may be partly new */
        if (RtxReplace(path, dest)) RTX_FAIL(RTX_E_SWAP, "rename staged file");
        ++receipt->written;
        if (i == RTX_LOADER) receipt->loader_updated = 1;
    }
    sync();
    /* 5. verify every root file from a fresh open */
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        RtxPath(path, sizeof(path), root, RTX_NAMES[i], NULL);
        if (RtxFileMatches(path, m->bytes[i], m->sha[i]) != 1) RTX_FAIL(RTX_E_VERIFY, "installed readback");
    }
    return RTX_OK;
fail:
    if (!swapping) {
        RtxCleanupStaged(root);
        /* Nothing in the root set changed; a stale TRIAL.BIN must not survive. */
        RtxPath(path, sizeof(path), root, "TRIAL.BIN", NULL); unlink(path);
    }
    sync();
    return result;
}

/* Re-check a finished install from a fresh (read-only) mount. */
static inline int RtxVerifyInstalled(const char* root, const RtxManifest* m) {
    char path[256];
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        RtxPath(path, sizeof(path), root, RTX_NAMES[i], NULL);
        if (RtxFileMatches(path, m->bytes[i], m->sha[i]) != 1) return RTX_E_VERIFY;
    }
    RtxPath(path, sizeof(path), root, "TRIAL.BIN", NULL);
    return access(path, F_OK) ? RTX_E_TRIAL : RTX_OK;
}
