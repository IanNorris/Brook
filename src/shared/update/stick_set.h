#pragma once
/* Boot-stick file sets (RAM-only smoke stick, remote self-update).
 *
 * Shared by the UEFI loader, the in-guest update helper, the kernel and host
 * tests. SET.BIN (root set) and PREV/SET.BIN (previous set) each describe one
 * kernel + initrd pair by length and SHA-256; the loader boots a set only when
 * both files verify. Little-endian, no packed native structures.
 *
 *   magic "BRKSET01"[8] kernel_bytes u64 initrd_bytes u64
 *   kernel_sha256[32] initrd_sha256[32] payload_id[48] (NUL padded)
 *   crc32 u32 over the preceding 136 bytes                     = 140 bytes
 */
#include "payload.h"

#define BROOK_STICK_SET_BYTES 140u
#define BROOK_STICK_SET_KERNEL_MAX (32u * 1024u * 1024u)
#define BROOK_STICK_SET_INITRD_MAX (128u * 1024u * 1024u)
#define BROOK_STICK_PAYLOAD_ID_BYTES 48u

static inline int BrookStickSetValid(const uint8_t* r) {
    static const uint8_t magic[8] = {'B','R','K','S','E','T','0','1'};
    if (!BrookUpdateEqual(r, magic, 8)) return 0;
    const uint64_t kb = BrookUpdateGet64(r + 8), ib = BrookUpdateGet64(r + 16);
    if (kb < 64 || kb > BROOK_STICK_SET_KERNEL_MAX || ib < 512 || ib > BROOK_STICK_SET_INITRD_MAX) return 0;
    /* payload id: printable [A-Za-z0-9._-], at least one byte, then NULs only */
    unsigned length = 0;
    while (length < BROOK_STICK_PAYLOAD_ID_BYTES && r[88 + length]) {
        const uint8_t c = r[88 + length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) return 0;
        ++length;
    }
    if (!length || length == BROOK_STICK_PAYLOAD_ID_BYTES) return 0;
    for (unsigned i = length; i < BROOK_STICK_PAYLOAD_ID_BYTES; ++i) if (r[88 + i]) return 0;
    return BrookUpdateGet32(r + 136) == BrookUpdateCrc(r, 136);
}

static inline uint64_t BrookStickSetKernelBytes(const uint8_t* r) { return BrookUpdateGet64(r + 8); }
static inline uint64_t BrookStickSetInitrdBytes(const uint8_t* r) { return BrookUpdateGet64(r + 16); }
static inline const uint8_t* BrookStickSetKernelSha(const uint8_t* r) { return r + 24; }
static inline const uint8_t* BrookStickSetInitrdSha(const uint8_t* r) { return r + 56; }
static inline const char* BrookStickSetPayloadId(const uint8_t* r) { return (const char*)(r + 88); }

/* Both files must have exactly the recorded length and digest. */
static inline int BrookStickSetMatches(const uint8_t* r, const uint8_t* kernel, uint64_t kernelBytes,
                                       const uint8_t* initrd, uint64_t initrdBytes) {
    if (!BrookStickSetValid(r) || kernelBytes != BrookStickSetKernelBytes(r) ||
        initrdBytes != BrookStickSetInitrdBytes(r)) return 0;
    uint8_t h[32];
    BrookUpdateHash(kernel, (BrookUpdateSize)kernelBytes, h);
    if (!BrookUpdateEqual(h, BrookStickSetKernelSha(r), 32)) return 0;
    BrookUpdateHash(initrd, (BrookUpdateSize)initrdBytes, h);
    return BrookUpdateEqual(h, BrookStickSetInitrdSha(r), 32);
}

/* Loader decision. trialPending: TRIAL.BIN exists (root set never booted).
 * trialRun: TRIAL.RUN exists (root set booted once and the host has not
 * confirmed it). Returns the order to try: 1 = root first, 2 = PREV first. */
enum { BROOK_STICK_ROOT = 1, BROOK_STICK_PREV = 2 };
enum { BROOK_STICK_TRIAL_NONE = 0, BROOK_STICK_TRIAL_STARTED = 1,
       BROOK_STICK_TRIAL_UNCONFIRMED = 2, BROOK_STICK_TRIAL_MARK_FAILED = 3 };
static inline int BrookStickFirstSet(int trialPending, int trialRun) {
    return (!trialPending && trialRun) ? BROOK_STICK_PREV : BROOK_STICK_ROOT;
}
