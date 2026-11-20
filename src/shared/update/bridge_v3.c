#include "bridge_v3.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "payload.h"

static const uint8_t ClientMagic[] = {'B','R','K','A','U','T','H','3'};
static const uint8_t ServerMagic[] = {'B','R','K','S','R','V','3',0};
static const uint8_t OperatorLabel[] = "Brook bridge v3 operator proof";
static const uint8_t ClientKeyLabel[] = "Brook bridge v3 client to server";
static const uint8_t ServerKeyLabel[] = "Brook bridge v3 server to client";
static const uint64_t MaxTraffic = 16ull * 1024 * 1024 * 1024 * 1024;

static void Copy(uint8_t *out, const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) out[i] = bytes[i];
}

static int Nonzero(const uint8_t *bytes, size_t length) {
    unsigned combined = 0;
    for (size_t i = 0; i < length; ++i) combined |= bytes[i];
    return combined != 0;
}

// Operation zero is read-only command status. All other operations carry a
// transaction and name the boot that the operator intends to reach.
static int ClientValid(const BrookBridgeV3ClientHello *hello) {
    if (!Nonzero(hello->operator_id, 32) || !Nonzero(hello->nonce, 32) ||
        !Nonzero(hello->ephemeral, 32)) return 0;

    switch (hello->protocol) {
    case BrookBridgeV3Command:
        if (hello->operation == 0) return 1;
        if (hello->operation != 1) return 0;
        break;
    case BrookBridgeV3Update:
        if (hello->operation < 1 || hello->operation > 2) return 0;
        break;
    case BrookBridgeV3Bulk:
        // PUT, GET, GETZ, FRAME, FRAMEZ, DISK.
        if (hello->operation < 1 || hello->operation > 6) return 0;
        break;
    case BrookBridgeV3Store:
        if (hello->operation != 1) return 0;
        break;
    default:
        return 0;
    }

    return Nonzero(hello->boot_id, 16) && Nonzero(hello->transaction_id, 16);
}

void BrookBridgeV3EncodeClient(uint8_t out[144], const BrookBridgeV3ClientHello *hello) {
    Copy(out, ClientMagic, 8);
    out[8] = BrookBridgeV3Version;
    out[9] = BrookBridgeV3OperatorSuite;
    out[10] = hello->protocol;
    out[11] = hello->operation;
    BrookUpdatePut32(out + 12, 1233u + hello->protocol);
    Copy(out + 16, hello->boot_id, 16);
    Copy(out + 32, hello->transaction_id, 16);
    Copy(out + 48, hello->operator_id, 32);
    Copy(out + 80, hello->nonce, 32);
    Copy(out + 112, hello->ephemeral, 32);
}

int BrookBridgeV3DecodeClient(BrookBridgeV3ClientHello *out, const uint8_t bytes[144]) {
    if (!BrookUpdateEqual(bytes, ClientMagic, 8) || bytes[8] != BrookBridgeV3Version ||
        bytes[9] != BrookBridgeV3OperatorSuite || BrookUpdateGet32(bytes + 12) != 1233u + bytes[10])
        return 0;

    BrookBridgeV3ClientHello hello;
    hello.protocol = bytes[10];
    hello.operation = bytes[11];
    Copy(hello.boot_id, bytes + 16, 16);
    Copy(hello.transaction_id, bytes + 32, 16);
    Copy(hello.operator_id, bytes + 48, 32);
    Copy(hello.nonce, bytes + 80, 32);
    Copy(hello.ephemeral, bytes + 112, 32);
    if (!ClientValid(&hello)) return 0;
    *out = hello;
    return 1;
}

void BrookBridgeV3EncodeServer(uint8_t out[96], const BrookBridgeV3ServerHello *hello) {
    Copy(out, ServerMagic, 8);
    BrookUpdatePut64(out + 8, BrookBridgeV3OperatorSuite);
    Copy(out + 16, hello->boot_id, 16);
    Copy(out + 32, hello->nonce, 32);
    Copy(out + 64, hello->ephemeral, 32);
}

int BrookBridgeV3DecodeServer(BrookBridgeV3ServerHello *out, const uint8_t bytes[96]) {
    if (!BrookUpdateEqual(bytes, ServerMagic, 8) ||
        BrookUpdateGet64(bytes + 8) != BrookBridgeV3OperatorSuite ||
        !Nonzero(bytes + 16, 16) || !Nonzero(bytes + 32, 32) ||
        !Nonzero(bytes + 64, 32)) return 0;

    Copy(out->boot_id, bytes + 16, 16);
    Copy(out->nonce, bytes + 32, 32);
    Copy(out->ephemeral, bytes + 64, 32);
    return 1;
}

void BrookBridgeV3OperatorId(uint8_t out[32], const uint8_t public_key[32]) {
    BrookUpdateHash(public_key, 32, out);
}

enum { ProofInputBytes = sizeof OperatorLabel - 1 + BrookBridgeV3ClientBytes + BrookBridgeV3ServerBytes };

static void ProofInput(uint8_t input[ProofInputBytes], const uint8_t client[144], const uint8_t server[96]) {
    const size_t label_length = sizeof OperatorLabel - 1;
    Copy(input, OperatorLabel, label_length);
    Copy(input + label_length, client, BrookBridgeV3ClientBytes);
    Copy(input + label_length + BrookBridgeV3ClientBytes, server, BrookBridgeV3ServerBytes);
}

void BrookBridgeV3Sign(uint8_t proof[64], const uint8_t secret_key[64],
                       const uint8_t client[144], const uint8_t server[96]) {
    uint8_t input[ProofInputBytes];
    ProofInput(input, client, server);
    crypto_ed25519_sign(proof, secret_key, input, sizeof input);
}

int BrookBridgeV3Verify(const uint8_t proof[64], const uint8_t public_key[32],
                       const uint8_t client[144], const uint8_t server[96]) {
    uint8_t operator_id[32], input[ProofInputBytes];
    BrookBridgeV3OperatorId(operator_id, public_key);
    if (!BrookUpdateEqual(operator_id, client + 48, 32)) return 0;
    ProofInput(input, client, server);
    return crypto_ed25519_check(proof, public_key, input, sizeof input) == 0;
}

// RFC 2104 over Brook's existing SHA-256. HKDF uses fixed 32-byte keys.
static void HmacSha256(uint8_t out[32], const uint8_t key[32],
                       const uint8_t *bytes, size_t length) {
    BrookSha256 state;
    uint8_t pad[64], inner[32];
    for (size_t i = 0; i < sizeof pad; ++i) pad[i] = 0x36;
    for (size_t i = 0; i < 32; ++i) pad[i] ^= key[i];
    BrookShaInit(&state);
    BrookShaAdd(&state, pad, sizeof pad);
    BrookShaAdd(&state, bytes, length);
    BrookShaFinish(&state, inner);

    for (size_t i = 0; i < sizeof pad; ++i) pad[i] ^= 0x36 ^ 0x5c;
    BrookShaInit(&state);
    BrookShaAdd(&state, pad, sizeof pad);
    BrookShaAdd(&state, inner, sizeof inner);
    BrookShaFinish(&state, out);
    crypto_wipe(pad, sizeof pad);
    crypto_wipe(inner, sizeof inner);
    crypto_wipe(&state, sizeof state);
}

static void Expand(BrookBridgeV3Stream *stream, const uint8_t prk[32],
                   const uint8_t transcript[32], const uint8_t *label, size_t label_length,
                   uint8_t direction) {
    uint8_t input[32 + sizeof ClientKeyLabel], second[32];
    Copy(input, label, label_length);
    input[label_length] = 1;
    HmacSha256(stream->key, prk, input, label_length + 1);
    Copy(input, stream->key, 32);
    Copy(input + 32, label, label_length);
    input[32 + label_length] = 2;
    HmacSha256(second, prk, input, 32 + label_length + 1);
    Copy(stream->nonce_prefix, second, 4);
    Copy(stream->transcript, transcript, 32);
    stream->sequence = 0;
    stream->bytes = 0;
    stream->direction = direction;
    crypto_wipe(input, sizeof input);
    crypto_wipe(second, sizeof second);
}

int BrookBridgeV3Start(BrookBridgeV3Stream *client_to_server, BrookBridgeV3Stream *server_to_client,
                      const uint8_t private_key[32], const uint8_t peer_ephemeral[32],
                      const uint8_t client[144], const uint8_t server[96], const uint8_t proof[64]) {
    uint8_t shared[32], transcript[32], prk[32];
    static const uint8_t zero[32] = {0};
    crypto_x25519(shared, private_key, peer_ephemeral);
    if (!crypto_verify32(shared, zero)) {
        crypto_wipe(shared, sizeof shared);
        return 0;
    }

    BrookSha256 hash;
    BrookShaInit(&hash);
    BrookShaAdd(&hash, client, BrookBridgeV3ClientBytes);
    BrookShaAdd(&hash, server, BrookBridgeV3ServerBytes);
    BrookShaAdd(&hash, proof, BrookBridgeV3ProofBytes);
    BrookShaFinish(&hash, transcript);
    HmacSha256(prk, transcript, shared, sizeof shared);
    Expand(client_to_server, prk, transcript, ClientKeyLabel, sizeof ClientKeyLabel - 1, 1);
    Expand(server_to_client, prk, transcript, ServerKeyLabel, sizeof ServerKeyLabel - 1, 2);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(prk, sizeof prk);
    return 1;
}

static int ValidRecord(const BrookBridgeV3Stream *stream, uint8_t type, uint32_t length) {
    if (stream->sequence >= BrookBridgeV3MaxRecords || length > BrookBridgeV3MaxRecord ||
        stream->bytes > MaxTraffic - length) return 0;
    if (type == BrookBridgeV3Ready)
        return stream->direction == 2 && stream->sequence == 0 && length == 0;
    if (type == BrookBridgeV3End) return length == 0;
    return type == BrookBridgeV3Data && length != 0;
}

int BrookBridgeV3RecordSize(const BrookBridgeV3Stream *stream, const uint8_t header[16],
                            uint32_t *length) {
    if (header[1] || header[2] || header[3] || BrookUpdateGet64(header + 8) != stream->sequence)
        return 0;
    uint32_t candidate = BrookUpdateGet32(header + 4);
    if (!ValidRecord(stream, header[0], candidate)) return 0;
    *length = candidate;
    return 1;
}

static void InitRecord(crypto_aead_ctx *context, uint8_t aad[49],
                       const BrookBridgeV3Stream *stream, const uint8_t header[16]) {
    uint8_t nonce[12];
    Copy(nonce, stream->nonce_prefix, 4);
    BrookUpdatePut64(nonce + 4, stream->sequence);
    Copy(aad, stream->transcript, 32);
    aad[32] = stream->direction;
    Copy(aad + 33, header, 16);
    crypto_aead_init_ietf(context, stream->key, nonce);
}

int BrookBridgeV3Seal(BrookBridgeV3Stream *stream, uint8_t header[16],
                      uint8_t *ciphertext, uint8_t tag[16],
                      uint8_t type, const uint8_t *plain, uint32_t length) {
    if (!ValidRecord(stream, type, length)) return 0;
    BrookUpdatePut32(header, type);
    BrookUpdatePut32(header + 4, length);
    BrookUpdatePut64(header + 8, stream->sequence);
    crypto_aead_ctx context;
    uint8_t aad[49];
    InitRecord(&context, aad, stream, header);
    crypto_aead_write(&context, ciphertext, tag, aad, sizeof aad, plain, length);
    crypto_wipe(&context, sizeof context);
    ++stream->sequence;
    stream->bytes += length;
    return 1;
}

int BrookBridgeV3Open(BrookBridgeV3Stream *stream, uint8_t *plain, const uint8_t header[16],
                      const uint8_t *ciphertext, uint32_t length, const uint8_t tag[16]) {
    uint32_t expected;
    if (!BrookBridgeV3RecordSize(stream, header, &expected) || expected != length) return 0;
    crypto_aead_ctx context;
    uint8_t aad[49];
    InitRecord(&context, aad, stream, header);
    int valid = crypto_aead_read(&context, plain, tag, aad, sizeof aad, ciphertext, length) == 0;
    crypto_wipe(&context, sizeof context);
    if (!valid) return 0;
    ++stream->sequence;
    stream->bytes += length;
    return 1;
}
