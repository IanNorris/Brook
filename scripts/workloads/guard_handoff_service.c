/* Static, libc-free guest probe: exercise desktop-user writes after guarded handoff. */
static long call3(long n, long a, long b, long c) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}
__attribute__((noreturn)) void _start(void) {
    static const char path[]="/data/home/SERVICE.TXT";
    static const char data[]="guarded desktop service\n";
    static const char success[]="GUARD_DESKTOP_SERVICE_PASS\n";
    static const char failure[]="GUARD_DESKTOP_SERVICE_FAIL\n";
    long fd=call3(2,(long)path,577,0644); /* O_WRONLY|O_CREAT|O_TRUNC */
    int ok=fd>=0 && call3(102,0,0,0)==1000; /* getuid */
    if(ok)ok=call3(1,fd,(long)data,sizeof(data)-1)==sizeof(data)-1;
    if(ok)ok=call3(74,fd,0,0)==0;
    if(fd>=0 && call3(3,fd,0,0)!=0)ok=0;
    call3(1,1,(long)(ok?success:failure),ok?sizeof(success)-1:sizeof(failure)-1);
    call3(60,ok?0:1,0,0);
    __builtin_unreachable();
}
