/* Minimal real Wayland wire-protocol smoke test for the guarded desktop. */
#include <stdint.h>
static long call3(long n,long a,long b,long c) {
    long r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;
}
struct UnixAddress {uint16_t family;char path[108];};
struct PollFd {int fd;short events,revents;};
struct Timespec {long sec,nsec;};
__attribute__((used,noinline,noreturn)) static void client_main(void) {
    static const char ok[]="GUARD_WAYLAND_CLIENT_PASS\n",fail[]="GUARD_WAYLAND_CLIENT_FAIL\n";
    struct UnixAddress address={1,"/tmp/wayland-0"};struct Timespec delay={0,100000000};long fd=-1;
    for(unsigned i=0;i<100;++i) {
        fd=call3(41,1,1,0);
        if(fd>=0 && call3(42,fd,(long)&address,sizeof(address))==0)break;
        if(fd>=0)call3(3,fd,0,0);
        fd=-1;call3(35,(long)&delay,0,0);
    }
    int passed=0;
    if(fd>=0) {
        /* wl_display.get_registry(new_id=2), then sync(new_id=3). */
        const uint32_t request[]={1,(12u<<16)|1u,2,1,12u<<16,3};
        if(call3(1,fd,(long)request,sizeof(request))==(long)sizeof(request)) {
            uint8_t data[8192];unsigned used=0,offset=0,globals=0;
            for(unsigned tries=0;tries<100 && used<sizeof(data);++tries) {
                struct PollFd p={(int)fd,1,0};long ready=call3(7,(long)&p,1,100);
                if(ready<0)break;
                if(!ready)continue;
                long bytes=call3(0,fd,(long)(data+used),sizeof(data)-used);if(bytes<=0)break;used+=(unsigned)bytes;
                while(used-offset>=8) {
                    uint32_t object=0,header=0;
                    for(unsigned j=0;j<4;++j){object|=(uint32_t)data[offset+j]<<(8*j);header|=(uint32_t)data[offset+4+j]<<(8*j);}
                    unsigned size=header>>16,opcode=header&0xffff;
                    if(size<8 || (size&3) || size>sizeof(data)){offset=used;break;}
                    if(used-offset<size)break;
                    if(object==2 && opcode==0)++globals;
                    if(object==3 && opcode==0 && size==12 && globals)passed=1;
                    offset+=size;
                }
                if(passed)break;
            }
        }
        call3(3,fd,0,0);
    }
    call3(1,1,(long)(passed?ok:fail),passed?sizeof(ok)-1:sizeof(fail)-1);
    call3(60,passed?0:1,0,0);__builtin_unreachable();
}
__attribute__((naked,noreturn)) void _start(void) {
    __asm__ volatile("andq $-16, %rsp\n\tcall client_main\n\tud2");
}
