#pragma once

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../src/shared/update/bridge_v3.h"
#include "../src/shared/update/payload.h"
#include "../src/third_party/monocypher/monocypher.h"

#if !defined(BROOK_BRIDGE_V3_TEST) && !defined(BROOK_BRIDGE_V3_POLICY_SHA256)
#error "v3 helpers require the SHA-256 of the boot operator policy"
#endif

typedef struct {
    int fd;
    int ready, failed, peer_end;
    uint8_t operation;
    BrookBridgeV3Stream input, output;
    uint8_t *plain, *cipher;
    uint32_t offset, length;
} BridgeV3Posix;

static inline uint64_t BridgeV3PosixNow(void) {
    struct timespec time;
    return clock_gettime(CLOCK_MONOTONIC, &time) ? UINT64_MAX :
        (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static inline uint64_t BridgeV3PosixDeadline(uint64_t millis) {
    const uint64_t now = BridgeV3PosixNow();
    return now > UINT64_MAX - millis ? 0 : now + millis;
}

static inline int BridgeV3PosixExact(int fd, void *incoming, const void *outgoing,
                                     size_t length, uint64_t deadline) {
    uint8_t *in = (uint8_t *)incoming;
    const uint8_t *out = (const uint8_t *)outgoing;
    while (length) {
        const uint64_t now = BridgeV3PosixNow();
        if (now >= deadline) return -1;
        const uint64_t left = deadline - now;
        struct pollfd wait = {fd, (short)(out ? POLLOUT : POLLIN), 0};
        const int result = poll(&wait, 1, left > 60000 ? 60000 : (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (!result) continue;
        if (BridgeV3PosixNow() >= deadline) return -1;
        const ssize_t n = out ? send(fd, out, length, MSG_DONTWAIT | MSG_NOSIGNAL) :
                                recv(fd, in, length, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        if (out) out += n;
        else in += n;
        length -= (size_t)n;
    }
    return 0;
}

static inline int BridgeV3PosixHex(const char *text, uint8_t *result, size_t size) {
    unsigned nonzero = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; ++j) {
            const char c = *text++;
            if (c >= '0' && c <= '9') value = value * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') value = value * 16 + (unsigned)(c - 'a' + 10);
            else return 0;
        }
        result[i] = (uint8_t)value;
        nonzero |= value;
    }
    return nonzero != 0;
}

static inline int BridgeV3PosixFindKey(const uint8_t operator_id[32], uint8_t key[32]) {
#ifdef BROOK_BRIDGE_V3_TEST
    const char *path = getenv("BROOK_BRIDGE_V3_TEST_ALLOWLIST");
    const char *policy = getenv("BROOK_BRIDGE_V3_TEST_POLICY_SHA256");
#else
    const char *path = "/etc/bridge-v3-operators";
    const char *policy = BROOK_BRIDGE_V3_POLICY_SHA256;
#endif
    uint8_t expected[32], actual[32];
    if (!path || !policy || strlen(policy) != 64 || !BridgeV3PosixHex(policy, expected, 32)) return -1;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    char text[131];
    size_t used = 0;
    int clean = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        !(st.st_mode & 0022) && (st.st_size == 0 || st.st_size == 65 || st.st_size == 130);
    while (clean && used < sizeof(text)) {
        const ssize_t n = read(fd, text + used, sizeof(text) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n < 0) clean = 0;
            break;
        }
        used += (size_t)n;
    }
    if (close(fd) || !clean || used != (size_t)st.st_size) return -1;
    BrookUpdateHash((const uint8_t *)text, used, actual);
    if (!BrookUpdateEqual(expected, actual, sizeof(expected))) return -1;
    uint8_t keys[2][32];
    int found = 0;
    for (size_t i = 0; i < used / 65; ++i) {
        if (text[i * 65 + 64] != '\n' || !BridgeV3PosixHex(text + i * 65, keys[i], 32) ||
            (i && BrookUpdateEqual(keys[i - 1], keys[i], 32))) return -1;
        uint8_t id[32];
        BrookBridgeV3OperatorId(id, keys[i]);
        if (BrookUpdateEqual(id, operator_id, 32)) {
            memcpy(key, keys[i], 32);
            found = 1;
        }
    }
    return found;
}

static inline int BridgeV3PosixEntropy(uint8_t random[64]) {
#ifdef BROOK_BRIDGE_V3_TEST
    const unsigned long flags = 0;
#else
    const unsigned long flags = 1UL << 30; // BROOK_GRND_STRONG_ONLY
    // Older kernels silently ignored getrandom flags and could use a weak
    // fallback. Require the strong-only ABI before accepting any entropy.
    if (syscall(SYS_getrandom, NULL, 0UL, flags | 1UL) != -1 || errno != EINVAL) return 0;
#endif
    return syscall(SYS_getrandom, random, 64UL, flags) == 64;
}

static inline int BridgeV3PosixWriteRecord(BridgeV3Posix *session, uint8_t type,
                                          const uint8_t *data, uint32_t length, uint64_t deadline) {
    uint8_t header[BrookBridgeV3HeaderBytes], tag[BrookBridgeV3TagBytes];
    if (session->failed || !BrookBridgeV3Seal(&session->output, header, session->cipher, tag, type, data, length) ||
        BridgeV3PosixExact(session->fd, NULL, header, sizeof(header), deadline) ||
        BridgeV3PosixExact(session->fd, NULL, session->cipher, length, deadline) ||
        BridgeV3PosixExact(session->fd, NULL, tag, sizeof(tag), deadline)) {
        session->failed = 1;
        return -1;
    }
    return 0;
}

static inline int BridgeV3PosixAccept(BridgeV3Posix *session, int fd, uint8_t protocol,
                                      uint32_t operations, const char *boot, const char *transaction) {
    const uint64_t deadline = BridgeV3PosixDeadline(4000);
    uint8_t wire[BrookBridgeV3ClientBytes], answer[BrookBridgeV3ServerBytes];
    uint8_t proof[BrookBridgeV3ProofBytes], public_key[32], random[64] = {0};
    uint8_t boot_id[16], transaction_id[16];
    BrookBridgeV3ClientHello client;
    BrookBridgeV3ServerHello server;
    session->fd = fd;
    if (!BridgeV3PosixHex(boot, boot_id, sizeof(boot_id)) ||
        !BridgeV3PosixHex(transaction, transaction_id, sizeof(transaction_id)) ||
        BridgeV3PosixExact(fd, wire, NULL, sizeof(wire), deadline) ||
        !BrookBridgeV3DecodeClient(&client, wire) || client.protocol != protocol ||
        client.operation > 31 || !(operations & (1u << client.operation)) ||
        !BrookUpdateEqual(client.boot_id, boot_id, sizeof(boot_id)) ||
        !BrookUpdateEqual(client.transaction_id, transaction_id, sizeof(transaction_id))) return -1;
    const int policy = BridgeV3PosixFindKey(client.operator_id, public_key);
    if (policy < 0) fputs("BRIDGE_V3_POLICY_REFUSED\n", stderr);
    if (policy <= 0 || !BridgeV3PosixEntropy(random)) {
        crypto_wipe(random, sizeof(random));
        return -1;
    }
    const uint8_t empty[32] = {0};
    if (BrookUpdateEqual(empty, random, 32) || BrookUpdateEqual(empty, random + 32, 32)) {
        crypto_wipe(random, sizeof(random));
        return -1;
    }
    memcpy(server.boot_id, boot_id, sizeof(boot_id));
    memcpy(server.nonce, random, sizeof(server.nonce));
    crypto_x25519_public_key(server.ephemeral, random + 32);
    BrookBridgeV3EncodeServer(answer, &server);
    const int verified =
        !BridgeV3PosixExact(fd, NULL, answer, sizeof(answer), deadline) &&
        !BridgeV3PosixExact(fd, proof, NULL, sizeof(proof), deadline) &&
        BrookBridgeV3Verify(proof, public_key, wire, answer) &&
        BrookBridgeV3Start(&session->input, &session->output, random + 32, client.ephemeral, wire, answer, proof);
    crypto_wipe(random, sizeof(random));
    if (!verified) return -1;
    session->plain = (uint8_t *)malloc(BrookBridgeV3MaxRecord);
    session->cipher = (uint8_t *)malloc(BrookBridgeV3MaxRecord);
    if (!session->plain || !session->cipher ||
        BridgeV3PosixWriteRecord(session, BrookBridgeV3Ready, session->cipher, 0, deadline)) return -1;
    session->operation = client.operation;
    session->ready = 1;
    return 0;
}

static inline int BridgeV3PosixNext(BridgeV3Posix *session, uint64_t deadline) {
    uint8_t header[BrookBridgeV3HeaderBytes], tag[BrookBridgeV3TagBytes];
    uint32_t length;
    if (!session->ready || session->failed || session->peer_end ||
        BridgeV3PosixExact(session->fd, header, NULL, sizeof(header), deadline) ||
        !BrookBridgeV3RecordSize(&session->input, header, &length) ||
        BridgeV3PosixExact(session->fd, session->cipher, NULL, length, deadline) ||
        BridgeV3PosixExact(session->fd, tag, NULL, sizeof(tag), deadline) ||
        !BrookBridgeV3Open(&session->input, session->plain, header, session->cipher, length, tag)) {
        session->failed = 1;
        return -1;
    }
    session->offset = 0;
    session->length = length;
    session->peer_end = header[0] == BrookBridgeV3End;
    return session->peer_end ? 0 : 1;
}

static inline int BridgeV3PosixRead(BridgeV3Posix *session, void *data, size_t length,
                                    uint64_t deadline) {
    uint8_t *out = (uint8_t *)data;
    const size_t requested = length;
    while (length) {
        if (session->offset == session->length) {
            const int next = BridgeV3PosixNext(session, deadline);
            if (next <= 0) {
                if (!next && length == requested) return -2; // clean END before any bytes
                session->failed = 1;
                return -1;
            }
        }
        const size_t available = session->length - session->offset;
        const size_t take = available < length ? available : length;
        memcpy(out, session->plain + session->offset, take);
        session->offset += (uint32_t)take;
        out += take;
        length -= take;
    }
    return 0;
}

static inline int BridgeV3PosixWrite(BridgeV3Posix *session, const void *data, size_t length,
                                     uint64_t deadline) {
    const uint8_t *bytes = (const uint8_t *)data;
    if (!session->ready) return -1;
    while (length) {
        const uint32_t n = length > BrookBridgeV3MaxRecord ? (uint32_t)BrookBridgeV3MaxRecord : (uint32_t)length;
        if (BridgeV3PosixWriteRecord(session, BrookBridgeV3Data, bytes, n, deadline)) return -1;
        bytes += n;
        length -= n;
    }
    return 0;
}

static inline int BridgeV3PosixRequireEnd(BridgeV3Posix *session, uint64_t deadline) {
    if (session->failed || session->offset != session->length ||
        (!session->peer_end && BridgeV3PosixNext(session, deadline) != 0)) return -1;
    return session->peer_end ? 0 : -1;
}

static inline int BridgeV3PosixReplyEnd(BridgeV3Posix *session, uint64_t deadline) {
    return session->ready ? BridgeV3PosixWriteRecord(session, BrookBridgeV3End, session->cipher, 0, deadline) : -1;
}

static inline void BridgeV3PosixClear(BridgeV3Posix *session) {
    if (session->plain) crypto_wipe(session->plain, BrookBridgeV3MaxRecord);
    if (session->cipher) crypto_wipe(session->cipher, BrookBridgeV3MaxRecord);
    free(session->plain);
    free(session->cipher);
    crypto_wipe(session, sizeof(*session));
}
