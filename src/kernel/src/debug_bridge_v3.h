#pragma once

#include <stdint.h>

namespace brook {

void DebugBridgeV3Initialize();
void DebugBridgeV3MeasureInitrd(const uint8_t* bytes, uint64_t length);
void DebugBridgeV3LogBoot();
void DebugBridgeV3Serve(int socket, uint32_t peerAddress, bool local, uint64_t helloDeadline);

unsigned DebugBridgeV3OperatorKeyCount();
bool DebugBridgeV3Available();
const char* DebugBridgeV3PolicySha256();
const char* DebugBridgeV3InitrdSha256();

}
