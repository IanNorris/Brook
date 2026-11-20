#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BrookBridgeV3Version = 3,
    BrookBridgeV3OperatorSuite = 1,
    BrookBridgeV3DeviceIdentitySuite = 2, // Reserved. Phase 1 rejects it.
    BrookBridgeV3ClientBytes = 144,
    BrookBridgeV3ServerBytes = 96,
    BrookBridgeV3ProofBytes = 64,
    BrookBridgeV3IdBytes = 16,
    BrookBridgeV3KeyBytes = 32,
    BrookBridgeV3NonceBytes = 32,
    BrookBridgeV3HeaderBytes = 16,
    BrookBridgeV3TagBytes = 16,
    BrookBridgeV3MaxRecord = 65536,
    BrookBridgeV3MaxRecords = 1 << 24,
    BrookBridgeV3Ready = 1,
    BrookBridgeV3Data = 2,
    BrookBridgeV3End = 3,
};

typedef enum {
    BrookBridgeV3Command = 1,
    BrookBridgeV3Update = 2,
    BrookBridgeV3Bulk = 3,
    BrookBridgeV3Store = 4,
} BrookBridgeV3Protocol;

typedef struct {
    uint8_t protocol;
    uint8_t operation;
    uint8_t boot_id[16];
    uint8_t transaction_id[16];
    uint8_t operator_id[32];
    uint8_t nonce[32];
    uint8_t ephemeral[32];
} BrookBridgeV3ClientHello;

typedef struct {
    uint8_t boot_id[16];
    uint8_t nonce[32];
    uint8_t ephemeral[32];
} BrookBridgeV3ServerHello;

typedef struct {
    uint8_t key[32];
    uint8_t nonce_prefix[4];
    uint8_t transcript[32];
    uint64_t sequence;
    uint64_t bytes;
    uint8_t direction;
} BrookBridgeV3Stream;

void BrookBridgeV3EncodeClient(uint8_t out[144], const BrookBridgeV3ClientHello *hello);
int BrookBridgeV3DecodeClient(BrookBridgeV3ClientHello *out, const uint8_t bytes[144]);
void BrookBridgeV3EncodeServer(uint8_t out[96], const BrookBridgeV3ServerHello *hello);
int BrookBridgeV3DecodeServer(BrookBridgeV3ServerHello *out, const uint8_t bytes[96]);
void BrookBridgeV3OperatorId(uint8_t out[32], const uint8_t public_key[32]);
void BrookBridgeV3Sign(uint8_t proof[64], const uint8_t secret_key[64],
                       const uint8_t client[144], const uint8_t server[96]);
int BrookBridgeV3Verify(const uint8_t proof[64], const uint8_t public_key[32],
                       const uint8_t client[144], const uint8_t server[96]);
int BrookBridgeV3Start(BrookBridgeV3Stream *client_to_server, BrookBridgeV3Stream *server_to_client,
                      const uint8_t private_key[32], const uint8_t peer_ephemeral[32],
                      const uint8_t client[144], const uint8_t server[96], const uint8_t proof[64]);

// Validate a header before receiving its bounded body. Zero means invalid.
int BrookBridgeV3RecordSize(const BrookBridgeV3Stream *stream, const uint8_t header[16],
                            uint32_t *length);
int BrookBridgeV3Seal(BrookBridgeV3Stream *stream, uint8_t header[16],
                      uint8_t *ciphertext, uint8_t tag[16],
                      uint8_t type, const uint8_t *plain, uint32_t length);
int BrookBridgeV3Open(BrookBridgeV3Stream *stream, uint8_t *plain, const uint8_t header[16],
                      const uint8_t *ciphertext, uint32_t length, const uint8_t tag[16]);

#ifdef __cplusplus
}
#endif
