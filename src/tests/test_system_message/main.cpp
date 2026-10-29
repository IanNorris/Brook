#include <cassert>
#include <cstring>
#include <cstdio>
#include "../../../tools/brook-message-pkg/message_format.h"
#include "../../../tools/brook-message-pkg/message_pending.h"
static constexpr uint64_t GiB=1024ULL*1024*1024;
int main() {
    BrookSystemMessage m{};m.version=1;m.size=sizeof(m);
    m.code=BROOK_MESSAGE_APP_CLOSED_MEMORY;
    std::strcpy(m.app_name,"GIMP");std::strcpy(m.executable,"/nix/store/example/bin/gimp");
    char simple[512],technical[3072];
    auto format=[&] { return brook_system_message_format(&m,simple,sizeof(simple),technical,sizeof(technical)); };
    assert(format());
    assert(!std::strcmp(simple,"GIMP was closed because there wasn't enough memory for it to continue running."));
    assert(std::strstr(technical,m.executable) && std::strstr(technical,"Paging status is unavailable"));
    assert(!std::strstr(technical,"full") && !std::strstr(technical,"released"));
    m.flags=BROOK_MESSAGE_PHYSICAL_VALID|BROOK_MESSAGE_AFTER_VALID|BROOK_MESSAGE_PAGING_STATE_VALID;
    m.physical_total_bytes=16*GiB;m.physical_free_before_bytes=1*GiB;m.physical_free_after_bytes=6*GiB;
    assert(format());assert(std::strstr(technical,"6.0 GiB free of 16.0 GiB (62.5% used)"));
    assert(std::strstr(technical,"increased by 5.0 GiB during recovery"));
    assert(std::strstr(technical,"not configured") && !std::strstr(technical,"Measured memory released"));
    m.flags|=BROOK_MESSAGE_RELEASED_VALID;m.released_bytes=10*GiB;
    assert(format() && std::strstr(technical,"Measured memory released by application cleanup: 10.0 GiB"));
    m.flags|=BROOK_MESSAGE_PAGING_CONFIGURED|BROOK_MESSAGE_PAGING_USAGE_VALID|BROOK_MESSAGE_PAGING_FULL|BROOK_MESSAGE_PAGING_EXPERIMENTAL;
    m.paging_capacity_bytes=m.paging_used_bytes=2*GiB;
    assert(format() && std::strstr(technical,"Experimental paging backing: 2.0 GiB used of 2.0 GiB (full)"));
    --m.paging_used_bytes;assert(!format() && !simple[0] && !technical[0]);
    ++m.paging_used_bytes;m.flags&=~BROOK_MESSAGE_PAGING_STATE_VALID;assert(!format());
    m.flags=BROOK_MESSAGE_AFTER_VALID;assert(!format());
    m.flags=BROOK_MESSAGE_PHYSICAL_VALID;m.physical_free_before_bytes=17*GiB;assert(!format());
    m.flags=0;m.code=999;assert(!format());
    for(unsigned code=1;code<=5;++code){m.code=code;assert(format());}
    m.size--;assert(!format());m.size++;
    m.version=999;assert(!format());m.version=1;
    std::memset(m.app_name,'x',sizeof(m.app_name));assert(!format());m.app_name[0]=0;
    assert(format());assert(brook_system_message_format(&m,simple,1,technical,sizeof(technical))==0);
    assert(!simple[0] && !technical[0]);
    assert(brook_system_message_format(&m,simple,sizeof(simple),technical,1)==0);
    assert(!simple[0] && !technical[0]);
    assert(!brook_system_message_format(nullptr,simple,sizeof(simple),technical,sizeof(technical)));
    BrookPendingMessages queue{};
    m.flags=0;m.code=BROOK_MESSAGE_TERMINATION_REQUESTED;m.victim_uid=1000;
    m.victim_pid=10;m.victim_incarnation=100;m.sequence=1;
    brook_pending_message_add(&queue,&m);
    auto launch=m;launch.code=BROOK_MESSAGE_LAUNCH_MEMORY;launch.victim_pid=0;launch.victim_incarnation=0;
    std::strcpy(launch.executable,"/nix/store/other/bin/other");launch.sequence=2;
    brook_pending_message_add(&queue,&launch);
    m.code=BROOK_MESSAGE_APP_CLOSED_MEMORY;m.sequence=3;brook_pending_message_add(&queue,&m);
    assert(queue.count==2);BrookSystemMessage next{};
    assert(brook_pending_message_take(&queue,&next) && next.sequence==3);
    assert(brook_pending_message_take(&queue,&next) && next.sequence==2);
    assert(!brook_pending_message_take(&queue,&next));
    for(unsigned i=0;i<20;++i){m.victim_pid=i+2;m.victim_incarnation=i+1;brook_pending_message_add(&queue,&m);}
    assert(queue.count==BROOK_PENDING_MESSAGES);
    assert(brook_pending_message_take(&queue,&next) && next.victim_incarnation==5);
    std::puts("system message: wording, measured/unknown state, contradictory records and truncation PASS");
}
