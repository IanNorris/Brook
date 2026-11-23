/* Boot-stick self-update receiver for the RAM-only smoke stick (HWRTXUP.ELF).
 *
 *   HWRTXUP.ELF --status            read-only: loader set/trial and gate state
 *   HWRTXUP.ELF --confirm           the running root set booted: drop TRIAL.RUN
 *   HWRTXUP.ELF BOOT_ID TRANSACTION receive one signed file set on TCP 1235
 *
 * The upload is a v3 operator-authenticated stream (protocol UPDATE,
 * operation STICK) bound to this boot and transaction. The manifest must carry
 * an Ed25519 signature from the update key whose public half is
 * /etc/rtx-update-signer. Every file is hashed in RAM before the kernel gate
 * (BROOK_IOCTL_BOOT_STICK_ARM) is asked for write access; the gate decides
 * which volume that is. No shell, no paths from the wire, no retries.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/brook_boot_stick.h"
#include "rtx_stick_update.h"
#include "../src/third_party/monocypher/monocypher-ed25519.h"
#include "bridge_v3_posix.h"

#define STICK_OPERATION 3u
static BridgeV3Posix transport;
static uint64_t deadline;
static int gate = -1;

static int gate_status(BrookBootStickStatus* status) {
    memset(status, 0, sizeof(*status));
    return ioctl(gate, BROOK_IOCTL_BOOT_STICK_STATUS, status);
}
static int arm(int mode) { return ioctl(gate, BROOK_IOCTL_BOOT_STICK_ARM, (unsigned long)mode); }
static int disarm(void) { return ioctl(gate, BROOK_IOCTL_BOOT_STICK_DISARM, 0); }

static const char* set_name(uint32_t set) { return set == 1 ? "root" : set == 2 ? "PREV" : "legacy"; }
static const char* trial_name(uint32_t trial) {
    return trial == 1 ? "started" : trial == 2 ? "unconfirmed" : trial == 3 ? "mark_failed" : "none";
}

static int replied; /* a final failed/updated line was sent */
static int send_line(const char* text) {
    if (strstr(text, "\"state\":\"failed\"") || strstr(text, "\"state\":\"updated\"")) replied = 1;
    return BridgeV3PosixWrite(&transport, text, strlen(text), deadline);
}

static int read_signer(uint8_t key[32]) {
    const int fd = open("/etc/rtx-update-signer", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    char text[66] = {0};
    const ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n != 64 && !(n == 65 && text[64] == '\n')) return -1;
    text[64] = 0;
    return BridgeV3PosixHex(text, key, 32) ? 0 : -1;
}

static int status_main(void) {
    BrookBootStickStatus status;
    if (gate_status(&status)) { printf("HWRTXUP_STATUS enabled=0 errno=%d\n", errno); return 3; }
    char prev[256] = "", trial[8] = "";
    if (!arm(BROOK_BOOT_STICK_RO)) {
        uint64_t n = 0;
        uint8_t* set = RtxReadFile(BROOK_BOOT_STICK_MOUNT "/PREV/SET.BIN", BROOK_STICK_SET_BYTES, &n);
        if (set && n == BROOK_STICK_SET_BYTES && BrookStickSetValid(set)) snprintf(prev, sizeof(prev), "%s", BrookStickSetPayloadId(set));
        free(set);
        set = RtxReadFile(BROOK_BOOT_STICK_MOUNT "/SET.BIN", BROOK_STICK_SET_BYTES, &n);
        printf("HWRTXUP_ROOT_PAYLOAD %s\n", set && n == BROOK_STICK_SET_BYTES && BrookStickSetValid(set) ? BrookStickSetPayloadId(set) : "-");
        free(set);
        snprintf(trial, sizeof(trial), "%s%s", access(BROOK_BOOT_STICK_MOUNT "/TRIAL.BIN", F_OK) ? "" : "B",
                 access(BROOK_BOOT_STICK_MOUNT "/TRIAL.RUN", F_OK) ? "" : "R");
        disarm();
        printf("HWRTXUP_STATUS enabled=1 set=%s trial=%s markers=%s prev_payload=%s gate=ok\n",
               set_name(status.set), trial_name(status.trial), trial[0] ? trial : "-", prev[0] ? prev : "-");
        return 0;
    }
    gate_status(&status);
    printf("HWRTXUP_STATUS enabled=1 set=%s trial=%s gate=refused code=%d\n", set_name(status.set),
           trial_name(status.trial), status.last_refusal);
    return 4;
}

static int confirm_main(void) {
    BrookBootStickStatus status;
    if (gate_status(&status)) return 3;
    if (status.set != BROOK_STICK_ROOT) {
        printf("HWRTXUP_CONFIRM_REFUSED set=%s (only a root-set boot can be confirmed)\n", set_name(status.set));
        return 5;
    }
    if (arm(BROOK_BOOT_STICK_RW)) { printf("HWRTXUP_CONFIRM_REFUSED gate errno=%d\n", errno); return 4; }
    if (gate_status(&status) || status.running_location != BROOK_STICK_ROOT) {
        disarm();
        printf("HWRTXUP_CONFIRM_REFUSED the root set on the stick is not the running set\n");
        return 5;
    }
    int result = 0;
    if (unlink(BROOK_BOOT_STICK_MOUNT "/TRIAL.RUN") && errno != ENOENT) result = 6;
    if (unlink(BROOK_BOOT_STICK_MOUNT "/TRIAL.BIN") && errno != ENOENT) result = 6;
    sync();
    if (disarm()) result = 7;
    if (result) printf("HWRTXUP_CONFIRM_FAILED code=%d\n", result);
    else printf("HWRTXUP_CONFIRMED trial=%s\n", trial_name(status.trial));
    return result;
}

static int receive(const char* boot, const char* tx) {
    uint8_t header[72];
    if (BridgeV3PosixRead(&transport, header, sizeof(header), deadline) || memcmp(header, "BRRTXUP1", 8) ||
        memcmp(header + 8, boot, 32) || memcmp(header + 40, tx, 32)) return 10;
    BrookBootStickStatus status;
    if (gate_status(&status)) return 11;
    if (status.set != BROOK_STICK_ROOT && status.set != BROOK_STICK_PREV) return 12;
    char line[256];
    snprintf(line, sizeof(line), "{\"state\":\"ready\",\"running_set\":\"%s\",\"trial\":\"%s\",\"max_bytes\":%u}\n",
             set_name(status.set), trial_name(status.trial), RTX_TOTAL_MAX);
    if (send_line(line)) return 13;

    uint8_t manifest[RTX_MANIFEST_BYTES], signature[64], length[4];
    if (BridgeV3PosixRead(&transport, length, 4, deadline) || BrookUpdateGet32(length) != RTX_MANIFEST_BYTES ||
        BridgeV3PosixRead(&transport, manifest, sizeof(manifest), deadline) ||
        BridgeV3PosixRead(&transport, signature, 64, deadline)) return 14;
    uint8_t signer[32];
    if (read_signer(signer)) return 15;
    uint8_t* message = malloc(RTX_SIGN_DOMAIN_BYTES + sizeof(manifest));
    if (!message) return 17;
    memcpy(message, RTX_SIGN_DOMAIN, RTX_SIGN_DOMAIN_BYTES);
    memcpy(message + RTX_SIGN_DOMAIN_BYTES, manifest, sizeof(manifest));
    const int bad = crypto_ed25519_check(signature, signer, message, RTX_SIGN_DOMAIN_BYTES + sizeof(manifest));
    free(message);
    if (bad) { fputs("HWRTXUP_SIGNATURE_REFUSED\n", stderr); return RTX_E_SIGNATURE; }
    RtxManifest m;
    int result = RtxManifestParse(manifest, sizeof(manifest), &m);
    if (result) { fprintf(stderr, "HWRTXUP_MANIFEST_REFUSED code=%d\n", result); return result; }
    snprintf(line, sizeof(line), "{\"state\":\"manifest_ok\",\"payload_id\":\"%s\"}\n", m.payload_id);
    if (send_line(line)) return 13;

    uint8_t* files[RTX_FILE_COUNT] = {0};
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) {
        files[i] = malloc((size_t)m.bytes[i]);
        if (!files[i]) { result = 17; goto done; }
        if (BridgeV3PosixRead(&transport, files[i], (size_t)m.bytes[i], deadline)) { result = 18; goto done; }
    }
    result = RtxContentValid(&m, files);
    if (result) { fprintf(stderr, "HWRTXUP_CONTENT_REFUSED code=%d\n", result); goto done; }
    if (send_line("{\"state\":\"staged\",\"storage_modified\":false}\n")) { result = 13; goto done; }
    uint8_t command[8];
    if (BridgeV3PosixRead(&transport, command, 8, deadline) || memcmp(command, "ACTIVATE", 8) ||
        BridgeV3PosixRequireEnd(&transport, deadline)) { result = 22; goto done; }

    if (arm(BROOK_BOOT_STICK_RW)) {
        gate_status(&status);
        fprintf(stderr, "HWRTXUP_GATE_REFUSED code=%d errno=%d\n", status.last_refusal, errno);
        snprintf(line, sizeof(line), "{\"state\":\"failed\",\"code\":23,\"gate_refusal\":%d,\"storage_modified\":false}\n",
                 status.last_refusal);
        (void)send_line(line);
        result = 23; goto done;
    }
    RtxReceipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    if (gate_status(&status) || (status.running_location != BROOK_STICK_ROOT && status.running_location != BROOK_STICK_PREV)) {
        disarm(); result = 12; goto done;
    }
    /* The running pair is the known-good one: PREV is rebuilt from it only
     * when it is the root set; when it already is PREV/ it is kept. */
    result = RtxApply(BROOK_BOOT_STICK_MOUNT, &m, files, (int)status.running_location, &receipt);
    const int disarmed = disarm();
    int verified = 0;
    if (!result && !disarmed && !arm(BROOK_BOOT_STICK_RO)) {
        /* Fresh read-only mount: every byte comes back from the stick. */
        verified = RtxVerifyInstalled(BROOK_BOOT_STICK_MOUNT, &m) == RTX_OK;
        disarm();
    }
    if (result || disarmed || !verified) {
        fprintf(stderr, "HWRTXUP_INSTALL_FAILED code=%d step=%s disarm=%d verified=%d\n", result,
                receipt.failed_step, disarmed, verified);
        snprintf(line, sizeof(line), "{\"state\":\"failed\",\"code\":%d,\"step\":\"%s\",\"verified\":%s,"
                 "\"storage_modified\":true}\n", result ? result : 24, receipt.failed_step, verified ? "true" : "false");
        (void)send_line(line);
        if (!result) result = 24;
        goto done;
    }
    printf("HWRTXUP_UPDATED payload=%s prev=%s written=%u unchanged=%u loader_updated=%d prev_rebuilt=%d "
           "transaction=%s reboot_required=1\n", m.payload_id, receipt.prev_payload_id[0] ? receipt.prev_payload_id : "-",
           receipt.written, receipt.unchanged, receipt.loader_updated, receipt.prev_rebuilt, tx);
    fflush(stdout);
    snprintf(line, sizeof(line), "{\"state\":\"updated\",\"payload_id\":\"%s\",\"prev_payload_id\":\"%s\",\"written\":%u,"
             "\"unchanged\":%u,\"loader_updated\":%s,\"prev_rebuilt\":%s,\"verified\":true,\"trial\":\"pending\"}\n",
             m.payload_id, receipt.prev_payload_id, receipt.written, receipt.unchanged,
             receipt.loader_updated ? "true" : "false", receipt.prev_rebuilt ? "true" : "false");
    (void)send_line(line);
done:
    for (unsigned i = 0; i < RTX_FILE_COUNT; ++i) free(files[i]);
    return result;
}

/* Live-store slot write (BOOT TX --store): one signed store manifest, then the
 * raw image streamed into a store slot through the kernel gate, which decides
 * whether the slot is acceptable (boot-stick disk, not the boot volume, not
 * the running store). Readback SHA-256 by the kernel before success. The
 * running store and /etc/live-store are never modified here. */
#define STORE_MANIFEST_BYTES 112u
#define STORE_SIGN_DOMAIN "BROOK-RTX-STORE-MANIFEST-V1"
#define STORE_SIGN_DOMAIN_BYTES 28u
static uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v; }
static int receive_store(const char* boot, const char* tx) {
    uint8_t header[72];
    if (BridgeV3PosixRead(&transport, header, sizeof(header), deadline) || memcmp(header, "BRRTXST1", 8) ||
        memcmp(header + 8, boot, 32) || memcmp(header + 40, tx, 32)) return 10;
    if (send_line("{\"state\":\"ready\",\"mode\":\"store\"}\n")) return 13;
    uint8_t manifest[STORE_MANIFEST_BYTES], signature[64], signer[32];
    if (BridgeV3PosixRead(&transport, manifest, sizeof(manifest), deadline) ||
        BridgeV3PosixRead(&transport, signature, 64, deadline)) return 14;
    if (read_signer(signer)) return 15;
    uint8_t message[STORE_SIGN_DOMAIN_BYTES + STORE_MANIFEST_BYTES];
    memcpy(message, STORE_SIGN_DOMAIN, STORE_SIGN_DOMAIN_BYTES);
    memcpy(message + STORE_SIGN_DOMAIN_BYTES, manifest, sizeof(manifest));
    if (crypto_ed25519_check(signature, signer, message, sizeof(message))) {
        fputs("HWRTXUP_STORE_SIGNATURE_REFUSED\n", stderr); return RTX_E_SIGNATURE;
    }
    if (memcmp(manifest, "BRKSTOR1", 8)) return 16;
    BrookStoreExtent extent = {get64(manifest + 8), get64(manifest + 16)};
    const uint64_t bytes = get64(manifest + 24);
    const uint8_t* want = manifest + 32;
    char id[49] = {0}; memcpy(id, manifest + 64, 48);
    if (!bytes || bytes % 4096 || bytes > extent.sectors * 512) return 16;
    if (ioctl(gate, BROOK_IOCTL_BOOT_STICK_STORE_OPEN, &extent)) {
        BrookBootStickStatus status; gate_status(&status);
        char line[160];
        snprintf(line, sizeof(line), "{\"state\":\"failed\",\"code\":23,\"gate_refusal\":%d,\"storage_modified\":false}\n",
                 status.last_refusal);
        (void)send_line(line);
        return 23;
    }
    char line[256];
    snprintf(line, sizeof(line), "{\"state\":\"open\",\"store\":\"%s\",\"first_lba\":%" PRIu64 ",\"sectors\":%" PRIu64 "}\n",
             id, extent.first_lba, extent.sectors);
    int result = send_line(line) ? 13 : 0;
    uint8_t* chunk = result ? NULL : malloc(BROOK_STORE_WRITE_MAX);
    if (!result && !chunk) result = 17;
    uint64_t done = 0;
    while (!result && done < bytes) {
        const uint32_t n = bytes - done < BROOK_STORE_WRITE_MAX ? (uint32_t)(bytes - done) : BROOK_STORE_WRITE_MAX;
        if (BridgeV3PosixRead(&transport, chunk, n, deadline)) { result = 18; break; }
        BrookStoreWrite w = {done, (uint64_t)(uintptr_t)chunk, n, 0};
        if (ioctl(gate, BROOK_IOCTL_BOOT_STICK_STORE_WRITE, &w)) { result = 24; break; }
        done += n;
    }
    free(chunk);
    BrookStoreHash h = {bytes, {0}};
    if (!result && ioctl(gate, BROOK_IOCTL_BOOT_STICK_STORE_HASH, &h)) result = 25;
    if (!result && memcmp(h.sha256, want, 32)) result = 26;
    const int closed = ioctl(gate, BROOK_IOCTL_BOOT_STICK_STORE_CLOSE, 0);
    if (!result && closed) result = 27;
    if (result) {
        snprintf(line, sizeof(line), "{\"state\":\"failed\",\"code\":%d,\"written\":%" PRIu64 ",\"slot_modified\":%s}\n",
                 result, done, done ? "true" : "false");
        (void)send_line(line);
        return result;
    }
    snprintf(line, sizeof(line), "{\"state\":\"updated\",\"mode\":\"store\",\"store\":\"%s\",\"bytes\":%" PRIu64 ",\"verified\":true}\n",
             id, bytes);
    (void)send_line(line);
    printf("HWRTXUP_STORE_WRITTEN store=%s first_lba=%" PRIu64 " sectors=%" PRIu64 " bytes=%" PRIu64 " verified=1\n",
           id, extent.first_lba, extent.sectors, bytes);
    return 0;
}

static int hex32(const char* p) {
    if (strlen(p) != 32) return 0;
    for (unsigned i = 0; i < 32; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    return 1;
}

int main(int argc, char** argv) {
    gate = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (gate < 0) return 3;
    if (argc == 2 && !strcmp(argv[1], "--status")) return status_main();
    if (geteuid() != 0) return 3;
    if (argc == 2 && !strcmp(argv[1], "--confirm")) return confirm_main();
    const int store = argc == 4 && !strcmp(argv[3], "--store");
    if ((argc != 3 && !store) || !hex32(argv[1]) || !hex32(argv[2])) {
        fputs("usage: HWRTXUP.ELF --status | --confirm | BOOT_ID32 TRANSACTION_ID32 [--store]\n", stderr);
        return 2;
    }
    const int lock = open("/tmp/hardware-update.lock", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) return 4;
    close(lock);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), client = -1, result = 5;
    int reuse = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(1235), .sin_addr = {.s_addr = htonl(INADDR_ANY)}};
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
        bind(listener, (struct sockaddr*)&address, sizeof(address)) || listen(listener, 1)) goto out;
    puts("HWUPDATE_LISTENING port=1235 boot_stick=1");
    fflush(stdout);
    struct pollfd wait = {listener, POLLIN, 0};
    if (poll(&wait, 1, 60000) <= 0) { result = 6; goto out; }
    client = accept(listener, NULL, NULL);
    if (client < 0) { result = 7; goto out; }
    if (BridgeV3PosixAccept(&transport, client, BrookBridgeV3Update, 1u << STICK_OPERATION, argv[1], argv[2])) {
        fputs("HWRTXUP_AUTH_REFUSED version=3\n", stderr);
        result = 10; goto out;
    }
    deadline = BridgeV3PosixDeadline(store ? 3600000 : 1800000); /* host bounds its wait by size */
    result = store ? receive_store(argv[1], argv[2]) : receive(argv[1], argv[2]);
    if (result && !replied) {
        char error[96];
        snprintf(error, sizeof(error), "{\"state\":\"failed\",\"code\":%d,\"storage_modified\":false}\n", result);
        (void)send_line(error);
    }
    (void)BridgeV3PosixReplyEnd(&transport, deadline);
out:
    BridgeV3PosixClear(&transport);
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    unlink("/tmp/hardware-update.lock");
    if (result) fprintf(stderr, "HWRTXUP_FAILED code=%d\n", result);
    return result;
}
