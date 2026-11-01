#include "catpt_mailbox.h"
#include <cassert>
#include <cstdint>
#include <cstring>

using namespace brook::catpt;
struct Fake { uint8_t bytes[8192]{}; unsigned reads=0; bool fail=false; };
static bool read(void* ctx,uint32_t off,void* out,uint32_t len) {
    auto& fake=*static_cast<Fake*>(ctx);++fake.reads;
    if(fake.fail || off>sizeof(fake.bytes) || len>sizeof(fake.bytes)-off)return false;
    std::memcpy(out,fake.bytes+off,len);return true;
}
static void put(Fake& f,unsigned at,uint32_t value) {
    for(unsigned i=0;i<4;++i)f.bytes[at+i]=static_cast<uint8_t>(value>>(i*8));
}
static void config(Fake& f) { put(f,64,512);put(f,68,1024);put(f,72,256);put(f,76,256);put(f,80,100); }
int main() {
    Fake f{};Mailbox absent;
    assert(!absent.admit(0,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));
    assert(!absent.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));assert(f.reads==0);
    assert(!absent.admit(UINT64_MAX-4,32,0,32,read,&f));assert(f.reads==0);
    config(f);Mailbox valid;assert(valid.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));assert(f.reads==0);
    assert(!valid.firmwareReady(Mailbox::ReadyBit|8));assert(f.reads==0);
    assert(valid.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));assert(f.reads==1);
    auto c=valid.config();assert(c.inboxOffset==512 && c.outboxOffset==1024 && c.inboxSize==256 && c.outboxSize==256 && c.infoSize==100);
    assert(!valid.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));assert(f.reads==1);
    Mailbox bounds;assert(bounds.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));
    assert(!bounds.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|1020));assert(bounds.state()==MailboxState::Failed);assert(f.reads==1);
    Mailbox malformed;assert(malformed.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));put(f,80,101);
    assert(!malformed.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));assert(malformed.state()==MailboxState::Failed);
    config(f);put(f,68,8190);Mailbox range;assert(range.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));
    assert(!range.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));
    config(f);f.fail=true;Mailbox error;assert(error.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));
    assert(!error.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));assert(error.state()==MailboxState::Failed);
    f.fail=false;config(f);put(f,72,0);Mailbox zero;assert(zero.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));
    assert(!zero.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));
    config(f);put(f,76,4097);Mailbox huge;assert(huge.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));
    assert(!huge.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));
    config(f);put(f,68,768);Mailbox edge;assert(edge.admit(0x1000,sizeof(f.bytes),0,1024,read,&f));
    assert(edge.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));
    config(f);put(f,68,600);Mailbox overlap;assert(overlap.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));
    assert(!overlap.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));
    Mailbox max;assert(max.admit(0x1000,sizeof(f.bytes),0,sizeof(f.bytes),read,&f));unsigned reads=f.reads;
    assert(!max.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|((1u<<29)-1)));assert(f.reads==reads);
    assert(!max.firmwareReady(Mailbox::ReadyBit|Mailbox::BusyBit|8));assert(f.reads==reads);
}
