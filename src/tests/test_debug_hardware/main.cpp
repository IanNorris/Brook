#include "debug_hardware.h"
#include "bridge_reply.h"
#include <cassert>
#include <cstring>
#include <string>
std::string reply,dispatched;
namespace brook {
void DebugChannelSend(const char* s){reply+=s;}
void DebugEpochCopy(char out[33]){std::memset(out,'a',32);out[32]=0;}
bool DebugEpochMatches(const char* s){return std::string(s)==std::string(32,'a');}
void DebugHandleCommand(const char* s,uint32_t n){assert(n==std::strlen(s));dispatched=s;}
const char* BuildGitHash(){return "test+build";}
const char* BuildDate(){return "now";}
bool AudioAvailable(){return false;}
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
