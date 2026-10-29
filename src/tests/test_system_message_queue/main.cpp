#include <cassert>
#include <thread>
#include <cstdio>
#include <cstring>
#include "../../kernel/src/system_message.h"
using namespace brook;
int main() {
    BrookSystemMessage event{};event.version=1;event.size=sizeof(event);
    event.code=BROOK_MESSAGE_APP_CLOSED_MEMORY;event.victim_uid=1000;
    std::strcpy(event.app_name,"GIMP");BrookSystemMessage copy{};
    assert(!SystemMessageReadAfter(0,1000,&copy));
    assert(SystemMessagePublish(event));
    assert(!SystemMessageReadAfter(0,1001,&copy));
    assert(SystemMessageReadAfter(0,1000,&copy)&&copy.sequence==1);
    assert(!std::strcmp(copy.app_name,"GIMP"));
    event.victim_uid=1001;assert(SystemMessagePublish(event));
    assert(SystemMessageReadAfter(1,0,&copy)&&copy.sequence==2);
    assert(!SystemMessageReadAfter(1,1000,&copy));
    event.victim_uid=UINT32_MAX;assert(SystemMessagePublish(event));
    assert(SystemMessageReadAfter(1,1000,&copy)&&copy.sequence==3);
    event.version=2;assert(!SystemMessagePublish(event));event.version=1;
    std::memset(event.app_name,'x',sizeof(event.app_name));assert(!SystemMessagePublish(event));
    event.app_name[0]=0;
    auto publish=[&] {
        for(unsigned i=0;i<100;++i){auto record=event;record.requested_bytes=i;assert(SystemMessagePublish(record));}
    };
    std::thread a(publish),b(publish),c(publish),d(publish);a.join();b.join();c.join();d.join();
    assert(SystemMessageLastSequence()==403);
    uint64_t cursor=0;unsigned count=0;
    while(SystemMessageReadAfter(cursor,1000,&copy)) {
        assert(copy.sequence>cursor && copy.sequence>=388 && copy.requested_bytes<100);
        cursor=copy.sequence;++count;
    }
    assert(count==16 && cursor==403);
    assert(!SystemMessageReadAfter(UINT64_MAX,0,&copy));
    assert(!SystemMessageReadAfter(0,0,nullptr));
    std::puts("system messages: bounded retention, UID isolation, broadcast and concurrent publication PASS");
}
