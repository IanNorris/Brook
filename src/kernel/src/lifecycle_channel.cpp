#include "lifecycle_channel.h"
#include "shutdown.h"
#include "debug_epoch.h"
#include "fw_cfg.h"
#include "portio.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "vfs.h"

namespace brook {
namespace {
constexpr uint16_t Base=0x2f8; // COM2; COM1 remains the normal serial log.
LifecycleChannel channel;
char configuredCookie[33]{};
void Epoch(const char* proposal,char output[33]) {
    if(proposal)DebugEpochInitialize(proposal);
    DebugEpochCopy(output);
}
int Sync(){return VfsSync(nullptr);}
bool Send(const char* text) {
    for(;*text;++text) {
        // Disconnected or broken UART must not wedge a kernel worker.
        unsigned budget=100000;
        while(!(inb(Base+5)&0x20))if(!--budget)return false;
        outb(Base,static_cast<uint8_t>(*text));
    }
    return true;
}
void Worker(void*) {
    char line[160];unsigned length=0;bool discard=false;
    for(;;) {
        // Bound each turn even if the host floods malformed input.
        for(unsigned count=0;count<256;++count) {
            uint8_t status=inb(Base+5);
            if(status&0x1e)discard=true; // overrun/parity/framing/break
            if(!(status&1))break;
            char c=static_cast<char>(inb(Base));
            if(c=='\n') {
                if(!discard) {
                    line[length]=0;char response[160];
                    if(channel.Handle(line,response,Epoch,Sync,ShutdownCommand))Send(response);
                }
                length=0;discard=false;
            }else if(c<' '||c>'~'||length+1>=sizeof(line))discard=true;
            else if(!discard)line[length++]=c;
        }
        SchedulerSleepMs(2);
    }
}
}
// Read fw_cfg only before scheduling starts: its selector/data registers are
// shared with other fw_cfg readers, which are not a runtime transport.
void LifecycleChannelConfigure() {
    ShutdownConfigure();
    const auto n=FwCfgReadFile("opt/brook/lifecycle",configuredCookie,sizeof(configuredCookie));
    if(n!=32){configuredCookie[0]=0;return;}
    configuredCookie[32]=0;
    if(!channel.Init(configuredCookie)){configuredCookie[0]=0;return;}
    // Only touch COM2 with explicit QEMU bridge opt-in, never on real hardware
    // merely because another device happens to occupy this legacy port.
    outb(Base+1,0);outb(Base+3,0x80);outb(Base,1);outb(Base+1,0);
    outb(Base+3,3);outb(Base+2,0xc7);outb(Base+4,3);
}
void LifecycleChannelStart() {
    ShutdownStart();
    if(!configuredCookie[0])return;
    Process* worker=KernelThreadCreate("vm_lifecycle",Worker,nullptr,3);
    if(!worker){SerialPuts("lifecycle: worker allocation failed\n");return;}
    SchedulerAddProcess(worker);
    SerialPuts("lifecycle: COM2 maintenance channel ready (sync/status only)\n");
}
}
