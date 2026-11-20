#include "debug_bridge_v3.h"
#include "debug_bridge_v3_keys.h"
#include "debug_bridge_v3_policy.h"

#include "apic.h"
#include "build_info.h"
#include "debug_epoch.h"
#include "debug_presence.h"
#include "kprintf.h"
#include "monocypher.h"
#include "net.h"
#include "scheduler.h"
#include "strong_random.h"
#include "string.h"
#include "update/bridge_v3.h"
#include "update/payload.h"

namespace brook {
namespace {

constexpr uint64_t RequestTimeoutMs = 10000;
constexpr uint64_t ReplyTimeoutMs = 10000;
constexpr uint32_t ReplyBytes = 4096;
constexpr uint32_t ReplyLimit = 2 * 1024 * 1024;
constexpr char ClientMagic[] = "BRKAUTH3";
constexpr char Refusal[] = "BRKREFU3";
constexpr char Banner[] = "=== Brook OS Debug Console ===\r\n";
constexpr char Hex[] = "0123456789abcdef";

struct Operator {
    uint8_t key[32];
    uint8_t id[32];
};
constexpr unsigned OperatorCount = (sizeof(BrookBridgeV3OperatorKeys) - 1) / 65;
static_assert(OperatorCount <= 2 && (sizeof(BrookBridgeV3OperatorKeys) - 1) % 65 == 0);
Operator operators[2]{};
uint8_t bootId[16]{};
char policySha[65]{};
char initrdSha[65]{};
bool authAvailable = false;

void EncodeHex(char* out, const uint8_t* bytes, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        out[i * 2] = Hex[bytes[i] >> 4];
        out[i * 2 + 1] = Hex[bytes[i] & 0xf];
    }
    out[count * 2] = '\0';
}

unsigned HexDigit(char digit) { return digit <= '9' ? digit - '0' : digit - 'a' + 10; }

void HashHex(char (&out)[65], const uint8_t* bytes, uint64_t length)
{
    uint8_t hash[32];
    BrookUpdateHash(bytes, length, hash);
    EncodeHex(out, hash, sizeof(hash));
}

bool Nonzero(const uint8_t* bytes, unsigned length)
{
    unsigned any = 0;
    for (unsigned i = 0; i < length; ++i) any |= bytes[i];
    return any != 0;
}

bool ReadExact(int socket, void* destination, uint32_t bytes, uint64_t deadline)
{
    auto* out = static_cast<uint8_t*>(destination);
    while (bytes && ApicTickCount() < deadline) {
        const int got = SockRecv(socket, out, bytes, true);
        if (got == -11) {
            SchedulerSleepMs(2);
            continue;
        }
        if (got <= 0 || static_cast<uint32_t>(got) > bytes) return false;
        out += got;
        bytes -= got;
    }
    return !bytes;
}

bool WriteExact(int socket, const void* source, uint32_t bytes, uint64_t deadline, bool local = false)
{
    auto* out = static_cast<const uint8_t*>(source);
    while (bytes && ApicTickCount() < deadline) {
        const int sent = local ? SockSendUntil(socket, out, bytes, deadline) :
                                 SockSendBefore(socket, out, bytes, deadline);
        if (sent <= 0 || static_cast<uint32_t>(sent) > bytes) return false;
        out += sent;
        bytes -= sent;
    }
    return !bytes;
}

struct RawReply {
    int socket;
    uint64_t deadline;
    bool local;
};

int WriteRawReply(void* context, const char* bytes, uint32_t length)
{
    const auto& reply = *static_cast<const RawReply*>(context);
    return WriteExact(reply.socket, bytes, length, reply.deadline, reply.local) ? int(length) : -5;
}

void ServePublicCaps(int socket, char first, bool local, uint64_t deadline)
{
    char command[DebugBridgeV3CommandBytes];
    command[0] = first;
    uint32_t length = 1;
    char next = 0;
    do {
        if (!ReadExact(socket, &next, 1, deadline)) return;
        if (next == '\n') break;
        if (length == sizeof(command) - 1) return;
        command[length++] = next;
    } while (true);
    if (length && command[length - 1] == '\r') --length;
    command[length] = '\0';

    if (!DebugLocalRequestPolicy(command, length).allowed ||
        !WriteExact(socket, Banner, sizeof(Banner) - 1, deadline, local)) return;
    RawReply reply{socket, deadline, local};
    if (!DebugChannelSetReplyWriter(WriteRawReply, &reply, false)) return;
    DebugHandleCommand(command, length);
    DebugChannelClearReplyWriter();
}

struct Session {
    explicit Session(int descriptor) : socket(descriptor) {}
    ~Session()
    {
        crypto_wipe(&incoming, sizeof(incoming));
        crypto_wipe(&outgoing, sizeof(outgoing));
        crypto_wipe(frame, sizeof(frame));
    }

    int socket;
    BrookBridgeV3Stream incoming{};
    BrookBridgeV3Stream outgoing{};
    uint64_t replyDeadline = 0;
    uint8_t frame[BrookBridgeV3HeaderBytes + ReplyBytes + BrookBridgeV3TagBytes];
    uint32_t buffered = 0;
    bool failed = false;

    uint8_t* Body() { return frame + BrookBridgeV3HeaderBytes; }

    bool Send(uint8_t type, uint32_t count, uint64_t deadline)
    {
        if (failed) return false;
        auto* body = Body();
        failed = !BrookBridgeV3Seal(&outgoing, frame, body, body + count, type, body, count) ||
                 !WriteExact(socket, frame, BrookBridgeV3HeaderBytes + count + BrookBridgeV3TagBytes,
                             deadline);
        return !failed;
    }

    bool Flush()
    {
        if (!buffered) return !failed;
        if (!replyDeadline) replyDeadline = ApicTickCount() + ReplyTimeoutMs;
        const uint32_t count = buffered;
        buffered = 0;
        return Send(BrookBridgeV3Data, count, replyDeadline);
    }

    bool Append(const char* bytes, uint32_t length)
    {
        if (outgoing.bytes + buffered + length > ReplyLimit) failed = true;
        while (length && !failed) {
            const uint32_t available = ReplyBytes - buffered;
            const uint32_t count = length < available ? length : available;
            memcpy(Body() + buffered, bytes, count);
            buffered += count;
            length -= count;
            bytes += count;
            if (buffered == ReplyBytes) Flush();
        }
        return !failed;
    }

    bool Read(uint8_t type, void* plain, uint32_t capacity, uint32_t& length, uint64_t deadline)
    {
        return ReadExact(socket, frame, BrookBridgeV3HeaderBytes, deadline) && frame[0] == type &&
            BrookBridgeV3RecordSize(&incoming, frame, &length) && length <= capacity &&
            ReadExact(socket, Body(), length + BrookBridgeV3TagBytes, deadline) &&
            BrookBridgeV3Open(&incoming, static_cast<uint8_t*>(plain), frame, Body(), length, Body() + length);
    }
};

int WriteEncryptedReply(void* context, const char* bytes, uint32_t length)
{
    return static_cast<Session*>(context)->Append(bytes, length) ? int(length) : -5;
}

struct EphemeralKey {
    uint8_t bytes[32]{};
    ~EphemeralKey() { crypto_wipe(bytes, sizeof(bytes)); }
};

bool Authenticate(Session& session, const uint8_t (&client)[BrookBridgeV3ClientBytes],
                  BrookBridgeV3ClientHello& request, uint64_t deadline)
{
    if (!BrookBridgeV3DecodeClient(&request, client) ||
        request.protocol != BrookBridgeV3Command) return false;

    const auto refused = [&]() {
        WriteExact(session.socket, Refusal, sizeof(Refusal) - 1, deadline);
        return false;
    };
    if (!authAvailable) return refused();
    const Operator* allowed = nullptr;
    for (unsigned i = 0; i < OperatorCount; ++i)
        if (BrookUpdateEqual(operators[i].id, request.operator_id, sizeof(request.operator_id)))
            allowed = &operators[i];
    if (!allowed) return refused();
    const bool currentBoot = !Nonzero(request.boot_id, sizeof(request.boot_id)) ||
        BrookUpdateEqual(bootId, request.boot_id, sizeof(bootId));

    EphemeralKey privateKey;
    BrookBridgeV3ServerHello challenge{};
    if (!StrongRandomBytes(privateKey.bytes, sizeof(privateKey.bytes)) ||
        !Nonzero(privateKey.bytes, sizeof(privateKey.bytes)) ||
        !StrongRandomBytes(challenge.nonce, sizeof(challenge.nonce)) ||
        !Nonzero(challenge.nonce, sizeof(challenge.nonce))) return refused();

    memcpy(challenge.boot_id, bootId, sizeof(bootId));
    crypto_x25519_public_key(challenge.ephemeral, privateKey.bytes);
    uint8_t server[BrookBridgeV3ServerBytes], proof[BrookBridgeV3ProofBytes];
    BrookBridgeV3EncodeServer(server, &challenge);
    // A stale pin may learn the current boot ID from the untrusted challenge,
    // but it cannot get an authenticated READY record or dispatch a command.
    return WriteExact(session.socket, server, sizeof(server), deadline) && currentBoot &&
        ReadExact(session.socket, proof, sizeof(proof), deadline) &&
        BrookBridgeV3Verify(proof, allowed->key, client, server) &&
        BrookBridgeV3Start(&session.incoming, &session.outgoing, privateKey.bytes, request.ephemeral,
                           client, server, proof);
}

}

void DebugBridgeV3Initialize()
{
    HashHex(policySha, reinterpret_cast<const uint8_t*>(BrookBridgeV3OperatorKeys),
            sizeof(BrookBridgeV3OperatorKeys) - 1);
    for (unsigned i = 0; i < OperatorCount; ++i) {
        auto& entry = operators[i];
        const char* hex = BrookBridgeV3OperatorKeys + i * 65;
        for (unsigned j = 0; j < sizeof(entry.key); ++j)
            entry.key[j] = static_cast<uint8_t>((HexDigit(hex[j * 2]) << 4) | HexDigit(hex[j * 2 + 1]));
        BrookBridgeV3OperatorId(entry.id, entry.key);
    }
    if (StrongRandomBytes(bootId, sizeof(bootId)) && Nonzero(bootId, sizeof(bootId))) {
        char epoch[33];
        EncodeHex(epoch, bootId, sizeof(bootId));
        authAvailable = DebugEpochInitialize(epoch) && DebugEpochMatches(epoch) && OperatorCount;
    }
    KPrintf("BRIDGE_V3_LOCAL build=%s policy_sha256=%s operators=%u ready=%u\n",
            BuildGitHash(), policySha, OperatorCount, authAvailable ? 1u : 0u);
}

void DebugBridgeV3MeasureInitrd(const uint8_t* bytes, uint64_t length)
{
    if (bytes && length) HashHex(initrdSha, bytes, length);
}

unsigned DebugBridgeV3OperatorKeyCount() { return OperatorCount; }
bool DebugBridgeV3Available() { return authAvailable; }
const char* DebugBridgeV3PolicySha256() { return policySha; }
const char* DebugBridgeV3InitrdSha256() { return initrdSha; }

void DebugBridgeV3Serve(int socket, uint32_t peerAddress, bool local, uint64_t helloDeadline)
{
    uint8_t client[BrookBridgeV3ClientBytes];
    if (!ReadExact(socket, client, 1, helloDeadline)) return;
    if (client[0] != ClientMagic[0]) {
        if (client[0] == '@' || client[0] == 'b')
            ServePublicCaps(socket, static_cast<char>(client[0]), local, helloDeadline);
        return;
    }
    constexpr uint32_t magicBytes = sizeof(ClientMagic) - 1;
    if (local || !ReadExact(socket, client + 1, magicBytes - 1, helloDeadline) ||
        memcmp(client, ClientMagic, magicBytes) ||
        !ReadExact(socket, client + magicBytes, sizeof(client) - magicBytes, helloDeadline)) return;

    Session session(socket);
    BrookBridgeV3ClientHello request{};
    if (!Authenticate(session, client, request, helloDeadline) ||
        !session.Send(BrookBridgeV3Ready, 0, helloDeadline)) return;

    const uint64_t requestDeadline = ApicTickCount() + RequestTimeoutMs;
    char command[DebugBridgeV3CommandBytes];
    uint32_t length = 0, endLength = 0;
    if (!session.Read(BrookBridgeV3Data, command, sizeof(command), length, requestDeadline) ||
        !DebugBridgeV3CommandAllowed(command, length, request.operation) ||
        !session.Read(BrookBridgeV3End, nullptr, 0, endLength, requestDeadline)) return;
    command[--length] = '\0';

    if (!DebugChannelSetReplyWriter(WriteEncryptedReply, &session, true)) return;
    if (request.operation == 1) DebugPresenceRequestBegin(peerAddress);
    DebugHandleCommand(command, length);
    DebugChannelClearReplyWriter();
    if (request.operation == 1) DebugPresenceRequestEnd();
    if (session.Flush()) session.Send(BrookBridgeV3End, 0, session.replyDeadline);
    crypto_wipe(command, sizeof(command));
}

}
