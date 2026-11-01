#include "catpt_transport.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

using namespace brook::catpt;
struct Fake { uint8_t bytes[8192]{}; unsigned reads=0; bool fail=false; char failOp=0;char events[32]{};unsigned count=0; };
static bool read(void* ctx,uint32_t off,void* out,uint32_t len) {
    auto& fake=*static_cast<Fake*>(ctx);++fake.reads;
    fake.events[fake.count++]='r';
    if(fake.fail || fake.failOp=='r' || off>sizeof(fake.bytes) || len>sizeof(fake.bytes)-off)return false;
    std::memcpy(out,fake.bytes+off,len);return true;
}
static bool write(void* ctx,uint32_t off,const void* data,uint32_t len) {
    auto& f=*static_cast<Fake*>(ctx);f.events[f.count++]='w';
    if(f.failOp=='w' || off>sizeof(f.bytes)||len>sizeof(f.bytes)-off)return false;
    std::memcpy(f.bytes+off,data,len);return true;
}
static bool publish(void* ctx,uint32_t header) {auto& f=*static_cast<Fake*>(ctx);f.events[f.count++]='p';return f.failOp!='p' && (header&Mailbox::BusyBit)!=0;}
static bool ackDone(void* ctx) {auto& f=*static_cast<Fake*>(ctx);f.events[f.count++]='d';return f.failOp!='d';}
static bool ackBusy(void* ctx) {auto& f=*static_cast<Fake*>(ctx);f.events[f.count++]='b';return f.failOp!='b';}
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
    // The transport is not bound to hardware here: these callbacks are only a
    // mock, with exact payload/publication and reply/acknowledgement ordering.
    f.count=0;Transport transport;TransportIo io{&f,read,write,publish,ackDone,ackBusy};
    assert(transport.bind(valid,io));const uint8_t payload[]={1,2,3};
    assert(transport.begin(0x01000000,payload,sizeof(payload),3));
    assert(f.count==2 && f.events[0]=='w' && f.events[1]=='p');
    // Simultaneous BUSY is left unacknowledged until DONE has been handled.
    assert(!transport.delayed(Mailbox::BusyBit));assert(f.count==2);
    assert(transport.immediate((1u<<30)|5));assert(transport.state()==TransportState::WaitingDelayed);
    assert(transport.delayed(Mailbox::BusyBit));assert(transport.state()==TransportState::Complete);
    assert(f.count==5 && f.events[2]=='d' && f.events[3]=='r' && f.events[4]=='b');
    assert(transport.replySize()==3 && std::memcmp(transport.reply(),payload,3)==0);assert(transport.consume());
    assert(transport.begin(0,payload,sizeof(payload),0));transport.timeout();
    assert(transport.state()==TransportState::Failed);
    assert(!transport.immediate(1u<<30));assert(!transport.begin(0,payload,sizeof(payload),0));
    f.count=0;Transport direct;assert(direct.bind(valid,io));
    assert(!direct.begin(Mailbox::BusyBit,payload,3,0));assert(f.count==0);
    assert(!direct.begin(0,payload,257,0));assert(f.count==0);
    assert(direct.begin(0,payload,3,3));assert(direct.immediate(1u<<30));
    assert(f.count==4 && f.events[2]=='r' && f.events[3]=='d');assert(direct.reply()!=nullptr);
    assert(direct.consume());assert(direct.reply()==nullptr);
    f.count=0;assert(direct.begin(0,payload,3,3));assert(direct.immediate((1u<<30)|1));
    assert(direct.status()==1 && direct.reply()==nullptr && f.count==3 && f.events[2]=='d');
    for(char fail: {'w','p','r','d','b'}) {
        f.count=0;f.failOp=fail;Transport t;assert(t.bind(valid,io));
        bool started=t.begin(0,payload,3,3);
        if(fail=='w'||fail=='p')assert(!started);
        else {
            assert(started);
            if(fail=='b'){assert(t.immediate((1u<<30)|5));assert(!t.delayed(Mailbox::BusyBit));}
            else assert(!t.immediate(1u<<30));
        }
        assert(t.state()==TransportState::Failed);unsigned count=f.count;
        assert(!t.immediate(1u<<30) && !t.delayed(Mailbox::BusyBit));assert(f.count==count);
    }
}
