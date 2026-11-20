#include "debug_bridge_v3.h"
#include "net.h"
#include "update/bridge_v3.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
std::string incoming, outgoing;
char bootEpoch[33]{};
char console[512]{};
uint64_t now=1;
bool idle=false, hasEntropy=true;

std::string Hello(bool known, uint8_t protocol=BrookBridgeV3Command)
{
    constexpr std::array<uint8_t,32> key{
        0x03,0xa1,0x07,0xbf,0xf3,0xce,0x10,0xbe,0x1d,0x70,0xdd,0x18,0xe7,0x4b,0xc0,0x99,
        0x67,0xe4,0xd6,0x30,0x9b,0xa5,0x0d,0x5f,0x1d,0xdc,0x86,0x64,0x12,0x55,0x31,0xb8,
    };
    BrookBridgeV3ClientHello hello{};
    hello.protocol=protocol;
    hello.operation=1;
    hello.boot_id[0]=1;
    hello.transaction_id[0]=1;
    hello.nonce[0]=1;
    hello.ephemeral[0]=1;
    if(known)BrookBridgeV3OperatorId(hello.operator_id,key.data());
    else hello.operator_id[0]=1;
    uint8_t wire[BrookBridgeV3ClientBytes];
    BrookBridgeV3EncodeClient(wire,&hello);
    return {reinterpret_cast<const char*>(wire),sizeof(wire)};
}

std::string Serve(std::string wire,bool local=false)
{
    incoming=wire;
    outgoing.clear();
    brook::DebugBridgeV3Serve(1,2,local,now+5);
    return outgoing;
}
}

bool RdrandU64(uint64_t* out)
{
    if(!hasEntropy)return false;
    *out=0x0101010101010101;
    return true;
}

namespace brook {
uint64_t ApicTickCount(){return now;}
extern "C" void SchedulerSleepMs(uint32_t ms){now+=ms;}
int SockRecv(int,void* out,uint32_t len,bool)
{
    if(incoming.empty())return idle?-11:0;
    const auto count=std::min<size_t>(len,incoming.size());
    std::memcpy(out,incoming.data(),count);
    incoming.erase(0,count);
    return static_cast<int>(count);
}
int SockSendBefore(int,const void* source,uint32_t len,uint64_t)
{
    outgoing.append(static_cast<const char*>(source),len);
    return static_cast<int>(len);
}
int SockSendUntil(int sock,const void* source,uint32_t len,uint64_t deadline)
{
    return SockSendBefore(sock,source,len,deadline);
}
bool DebugEpochInitialize(const char* proposal){std::memcpy(bootEpoch,proposal,sizeof(bootEpoch));return true;}
bool DebugEpochMatches(const char* expected){return std::strcmp(bootEpoch,expected)==0;}
bool DebugChannelSetReplyWriter(DebugReplyWriter,void*,bool){assert(false);return false;}
void DebugChannelClearReplyWriter(){assert(false);}
void DebugPresenceRequestBegin(uint32_t){assert(false);}
void DebugPresenceRequestEnd(){assert(false);}
void DebugHandleCommand(const char*,uint32_t){assert(false);}
const char* BuildGitHash(){return "review-test";}
extern "C" void KPrintf(const char* format,...)
{
    va_list args;
    va_start(args,format);
    const size_t used=std::strlen(console);
    std::vsnprintf(console+used,sizeof(console)-used,format,args);
    va_end(args);
}
}

int main()
{
    hasEntropy=false;
    brook::DebugBridgeV3Initialize();
    brook::DebugBridgeV3LogBoot();
    assert(std::strstr(console,"operators=1 ready=0\n"));
    assert(std::strstr(console,"initrd_sha256=unavailable\n"));
    assert(Serve(Hello(true))=="BRKREFU3");
    assert(Serve(Hello(true,BrookBridgeV3Bulk)).empty());
    hasEntropy=true;
    brook::DebugBridgeV3Initialize();
    const uint8_t bootImage[]={'t','e','s','t'};
    brook::DebugBridgeV3MeasureInitrd(bootImage,sizeof(bootImage));
    console[0]=0;
    brook::DebugBridgeV3LogBoot();
    const std::string expected=
        "BRIDGE_V3_LOCAL build=review-test boot_id=01010101010101010101010101010101 operators=1 ready=1\n"
        "BRIDGE_V3_LOCAL policy_sha256="+std::string(brook::DebugBridgeV3PolicySha256())+
        "\nBRIDGE_V3_LOCAL initrd_sha256=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08\n";
    assert(console==expected);
    assert(Serve(Hello(false))=="BRKREFU3");
    auto invalid=Hello(false);
    invalid[9]=BrookBridgeV3DeviceIdentitySuite;
    assert(Serve(invalid).empty());
    assert(Serve(Hello(true),true).empty());

    hasEntropy=false;
    assert(Serve(Hello(true))=="BRKREFU3");
    hasEntropy=true;
    // Use the current boot pin so this malformed proof is read and rejected
    // after the challenge, without a refusal marker or authenticated READY.
    auto proof=Hello(true);
    std::fill_n(proof.begin()+16,16,'\x01');
    proof.append(BrookBridgeV3ProofBytes,'\0');
    const auto challenge=Serve(proof);
    assert(challenge.size()==BrookBridgeV3ServerBytes && challenge.compare(0,7,"BRKSRV3")==0);

    idle=true;
    const uint64_t started=now;
    assert(Serve("BRK").empty());
    assert(now>=started+5 && now<=started+6);
}
