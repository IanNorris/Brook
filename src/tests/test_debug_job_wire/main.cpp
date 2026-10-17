#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "debug_job_wire.h"

int main() {
    std::vector<uint8_t> bytes{'B','J','0','1',0xe8,3,0,0,2,0,0,0,1,0,0,0};
    for (const std::string value:{"/bin/test","quotes ' \" ; $()\n","/data/home","CUSTOM=hello\nworld"}) {
        bytes.insert(bytes.end(),value.begin(),value.end());bytes.push_back(0);
    }
    brook::DebugJobRequest request{};
    assert(brook::DebugJobDecode(bytes.data(),bytes.size(),&request));
    assert(request.uid==1000 && request.argc==2 && request.envc==1);
    assert(std::string(request.argv[1])=="quotes ' \" ; $()\n");
    assert(std::string(request.cwd)=="/data/home");
    assert(std::string(request.env[0])=="CUSTOM=hello\nworld");
    for (uint32_t size=0;size<bytes.size();++size)
        assert(!brook::DebugJobDecode(bytes.data(),size,&request));
    auto malformed=bytes;malformed.push_back(0);
    assert(!brook::DebugJobDecode(malformed.data(),malformed.size(),&request));
    for (uint32_t at:{0u,4u,8u,12u,16u}) {
        malformed=bytes;malformed[at]=255;
        assert(!brook::DebugJobDecode(malformed.data(),malformed.size(),&request));
    }
    malformed=bytes;
    malformed[malformed.size()-std::string("CUSTOM=hello\nworld").size()-1]='=';
    assert(!brook::DebugJobDecode(malformed.data(),malformed.size(),&request));
}
