#include "debug_hardware.h"
#include "bridge_reply.h"
#include "shutdown.h"
#include "fs/compressed_vfs.h"
#include "vfs.h"
#include <cassert>
#include <cstring>
#include <string>
std::string reply,dispatched;
unsigned powerCalls=0;std::string powerVerb;
namespace brook {
AppSnapshotBootInfo snapshot;
Vnode binaryNode{},markerNode{};
bool binaryPresent=false,markerPresent=false,directory=false,statError=false;
unsigned openNodes=0;
AppSnapshotBootInfo CompressedVfsAppSnapshotInfo(){return snapshot;}
extern "C" Vnode* VfsOpen(const char* path,int){
    if(!std::strcmp(path,"/boot/HWBULK.ELF")&&binaryPresent){++openNodes;return &binaryNode;}
    if(!std::strcmp(path,"/etc/hardware-bulk")&&markerPresent){++openNodes;return &markerNode;}
    return nullptr;
}
int VfsStat(Vnode*,VnodeStat* out){*out={};out->size=1;out->isDir=directory;return statError?-5:0;}
extern "C" void VfsClose(Vnode*){assert(openNodes);--openNodes;}

void DebugChannelSend(const char* s){reply+=s;}
void DebugEpochCopy(char out[33]){std::memset(out,'a',32);out[32]=0;}
bool DebugEpochMatches(const char* s){return std::string(s)==std::string(32,'a');}
void DebugHandleCommand(const char* s,uint32_t n){assert(n==std::strlen(s));dispatched=s;}
const char* BuildGitHash(){return "test+build";}
const char* BuildDate(){return "now";}
bool AudioAvailable(){return false;}
ShutdownPowerCapabilities ShutdownGetPowerCapabilities(){return {true,true,false,"acpi-io8","manual","firmware","S5 unavailable"};}
const char* ShutdownRequestStatus(const char*){return "Safe to power off manually";}
void ShutdownCommand(const char*,const char* verb,char state[24],int* error){++powerCalls;powerVerb=verb;std::strcpy(state,"ready");*error=std::strcmp(verb,"commit")?0:-95;}
uint32_t SmpGetCpuCount(){return 4;}
int DebugOverlayReadRange(uint32_t* cursor,char* out,uint32_t limit,uint32_t stride,uint32_t* total,uint32_t* dropped){
    *total=2;*dropped=0;if(*cursor>2)return -22;
    unsigned count=2-*cursor;if(count>limit)count=limit;
    for(unsigned i=0;i<count;++i)std::strcpy(out+i*stride,i?"next":"quote\" slash\\ newline\n");
    *cursor+=count;return count;
}
}
bool handle(const std::string& command){reply.clear();dispatched.clear();return brook::DebugHardwareHandleCommand(command.c_str(),command.size());}
int main(){
    assert(!handle("bridge unrelated"));
    assert(handle("bridge hardware caps") && reply.find("\"hardware_api\":1")!=std::string::npos);
    assert(reply.find("\"panic_transport\":false")!=std::string::npos);
    assert(reply.find("\"power_api\":1")!=std::string::npos && reply.find("\"power_off\":false")!=std::string::npos);
    assert(reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::binaryPresent=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::markerPresent=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":1")!=std::string::npos && !brook::openNodes);
    brook::directory=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::directory=false;brook::statError=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::statError=false;brook::snapshot.configured=true;brook::snapshot.sourceClosed=true;
    brook::snapshot.error=brook::AppSnapshotBootError::Hash;
    assert(handle("bridge hardware caps") && reply.find("\"ready\":false")!=std::string::npos);
    brook::snapshot.error=brook::AppSnapshotBootError::None;brook::snapshot.ready=true;brook::snapshot.bytes=1234;
    assert(handle("bridge hardware caps") && reply.find("\"ready\":true")!=std::string::npos && reply.find("\"bytes\":1234")!=std::string::npos);
    const std::string request(32,'1'),boot(32,'a');
    assert(handle("bridge hardware power "+request+" shutdown "+boot) && powerCalls==1 && powerVerb=="shutdown");
    assert(handle("bridge hardware power "+request+" commit "+boot) && powerCalls==2 && reply.find("\"error\":-95")!=std::string::npos);
    for(const auto& bad : {"bridge hardware power short shutdown "+boot,"bridge hardware power "+request+" exec "+boot,"bridge hardware power "+request+" restart "+std::string(32,'b'),"bridge hardware power "+request+" restart "+boot+" more"}){
        assert(handle(bad) && powerCalls==2);
    }
    assert(handle("bridge guard "+std::string(32,'a')+" mem") && dispatched=="mem");
    for(const auto& command:{std::string("bridge guard short"),"bridge guard "+std::string(32,'b')+" mem","bridge guard "+std::string(32,'a')+" @1 mem","bridge guard "+std::string(32,'a')+" bridge guard "+std::string(32,'a')+" mem"}){
        assert(handle(command));assert(dispatched.empty());assert(reply.find("boot_error")!=std::string::npos);
    }
    assert(handle("bridge hardware logs 0 64"));
    assert(reply.find("quote\\\" slash\\\\ newline\\u000a")!=std::string::npos);
    assert(reply.find("\"next_cursor\":2")!=std::string::npos);
    for(const auto* bad:{"0 0","0 65","4294967296 1","2 1 more","3 1","-1 2"}){
        assert(handle(std::string("bridge hardware logs ")+bad));assert(reply.find("logs_error")!=std::string::npos);
    }
    reply.clear();brook::BridgeReplyBuffer<brook::DebugChannelSend> r;
    const std::string longText(2000,'x');r.quoted(longText.c_str());r.send();assert(reply=='"'+longText+'"');
}
