/* Bounded, guest-only PID pressure probe. No libc, network or host processes.
 * Children block on one pipe, exit when the parent closes it, then are waited.
 * Run only on an isolated VM: at most 1536 children and a two-second hold.
 */
typedef unsigned long u64;
static long call(long n,long a,long b,long c,long d,long e,long f) {
    register long r10 __asm__("r10")=d;
    register long r8 __asm__("r8")=e;
    register long r9 __asm__("r9")=f;
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
    return result;
}
static void text(const char* s){u64 n=0;while(s[n])++n;call(1,1,(long)s,n,0,0,0);}
static void number(u64 n){char a[24],out[24];unsigned count=0;do{a[count++]=(char)('0'+n%10);n/=10;}while(n);for(unsigned i=0;i<count;++i)out[i]=a[count-i-1];call(1,1,(long)out,count,0,0,0);}
__attribute__((noreturn)) static void finish(int code){call(60,code,0,0,0,0,0);__builtin_unreachable();}
static long wait(long pid,int* status){long r;do{r=call(61,pid,(long)status,0,0,0,0);}while(r==-4);return r;}
static int children[1536];
__attribute__((force_align_arg_pointer,noreturn)) void _start(void) {
    int pipe[2];if(call(22,(long)pipe,0,0,0,0,0)<0)finish(2);
    unsigned count=0;long error=0;
    for(;count<1536;++count) {
        long pid=call(57,0,0,0,0,0,0);
        if(pid<0){error=pid;break;}
        if(!pid){char byte;call(3,pipe[1],0,0,0,0,0);long n;do{n=call(0,pipe[0],(long)&byte,1,0,0,0);}while(n==-4);finish(n==0?0:3);}
        children[count]=(int)pid;
    }
    text("PID_CAPACITY_REACHED children=");number(count);text(" errno=");number((u64)-error);text("\n");
    struct {long sec,nsec;} hold={2,0};call(35,(long)&hold,0,0,0,0,0);
    call(3,pipe[1],0,0,0,0,0);call(3,pipe[0],0,0,0,0,0);
    unsigned failures=0;
    for(unsigned i=0;i<count;++i){int status=-1;if(wait(children[i],&status)!=children[i]||status)++failures;}
    if(count<800||count==1536||error!=-12)++failures; // Brook currently maps failed fork to ENOMEM.
    for(unsigned i=0;i<64;++i) {
        long pid=call(57,0,0,0,0,0,0);
        if(pid<0){++failures;break;}
        if(!pid)finish(0);
        int status=-1;if(wait(pid,&status)!=pid||status)++failures;
    }
    text("PID_CAPACITY_RESULT children=");number(count);text(" reaped=");number(count);text(" recovery_forks=64 failures=");number(failures);text("\n");
    finish(failures?1:0);
}
