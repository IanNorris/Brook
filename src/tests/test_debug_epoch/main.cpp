#include <cassert>
#include <string>
#include <mutex>
#include <thread>
#include <vector>
#include "debug_epoch.h"

namespace {
std::mutex mutex;
std::string reply;
}
namespace brook {
void DebugChannelSend(const char* value) {
    std::lock_guard<std::mutex> guard(mutex);reply+=value;
}
}
int main() {
    char token[33];brook::DebugEpochCopy(token);
    assert(std::string(token)==std::string(32,'0'));
    assert(!brook::DebugEpochMatches(token));
    assert(!brook::DebugEpochHandleCommand("bridge epochs"));
    for(const auto& bad:{"", "abc", "00000000000000000000000000000000", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa extra"}) {
        reply.clear();assert(brook::DebugEpochHandleCommand((std::string("bridge epoch init ")+bad).c_str()));
        assert(reply.find("epoch_error")!=std::string::npos);
    }
    std::vector<std::thread> threads;
    for(char c='a';c<='f';++c)threads.emplace_back([c]{
        const auto command=std::string("bridge epoch init ")+std::string(32,c);
        assert(brook::DebugEpochHandleCommand(command.c_str()));
        for(unsigned i=0;i<1000;++i) {
            char copy[33];brook::DebugEpochCopy(copy);
            assert(std::string(copy)==std::string(32,copy[0]));
            assert(brook::DebugEpochMatches(copy));
        }
    });
    for(auto& thread:threads)thread.join();
    brook::DebugEpochCopy(token);
    assert(brook::DebugEpochMatches(token));
    reply.clear();assert(brook::DebugEpochHandleCommand("bridge epoch"));
    assert(reply==std::string("{\"boot_id\":\"")+token+"\"}\n");
    const char different=token[0]=='a'?'b':'a';
    assert(!brook::DebugEpochMatches(std::string(32,different).c_str()));
    assert(brook::DebugEpochHandleCommand((std::string("bridge epoch init ")+std::string(32,different)).c_str()));
    assert(brook::DebugEpochMatches(token)); // First valid initializer wins.
}
