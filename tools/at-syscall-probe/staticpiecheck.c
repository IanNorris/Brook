// Interpreter-less PIE: preserve data/auxv while reserving Wine's low arena.
// Build: cc -O2 -nostdlib -static-pie -fPIE -fno-stack-protector staticpiecheck.c -o staticpiecheck
#include <stdint.h>
#include <elf.h>
static volatile uint64_t initialized = UINT64_C(0x2468ace013579bdf);
static volatile unsigned char zeroed[4096];
static long call6(long n,long a,long b,long c,long d,long e,long f) {
 register long r10 __asm__("r10")=d,r8 __asm__("r8")=e,r9 __asm__("r9")=f;long r;
 __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return r;
}
static void say(const char*s){unsigned n=0;while(s[n])++n;call6(1,1,(long)s,n,0,0,0);}
static void address(const char*label,uintptr_t v){char b[19];say(label);b[0]='0';b[1]='x';for(unsigned i=0;i<16;i++)b[2+i]="0123456789abcdef"[(v>>(60-4*i))&15];b[18]='\n';call6(1,1,(long)b,sizeof(b),0,0,0);}
void _start(void);
__attribute__((used)) static long check(uintptr_t*sp) {
 uintptr_t*env=sp+sp[0]+2;while(*env)++env;uintptr_t*aux=env+1;uintptr_t entry=0,phdr=0,phnum=0,phent=0,base=0,page=0;
 for(unsigned i=0;aux[0]&&i<64;i++,aux+=2)switch(aux[0]){case AT_ENTRY:entry=aux[1];break;case AT_PHDR:phdr=aux[1];break;case AT_PHNUM:phnum=aux[1];break;case AT_PHENT:phent=aux[1];break;case AT_BASE:base=aux[1];break;case AT_PAGESZ:page=aux[1];break;}
 address("STATICPIE_ENTRY ",entry);
 if(entry!=(uintptr_t)_start||base||!phdr||!phnum||phnum>64||phent!=sizeof(Elf64_Phdr)||page!=4096){say("STATICPIE_FAIL auxv\n");return 1;}
 const Elf64_Ehdr*h=(const Elf64_Ehdr*)(phdr-sizeof(Elf64_Ehdr));
 if(h->e_ident[0]!=0x7f||h->e_ident[1]!='E'||h->e_ident[2]!='L'||h->e_ident[3]!='F'||h->e_type!=ET_DYN||h->e_phoff!=sizeof(*h)){say("STATICPIE_FAIL header\n");return 2;}
 uintptr_t bias=entry-h->e_entry;const Elf64_Phdr*ph=(const Elf64_Phdr*)phdr;
 for(unsigned i=0;i<phnum;i++) {
  if(ph[i].p_type==PT_INTERP){say("STATICPIE_FAIL interpreter present\n");return 3;}
  if(ph[i].p_type==PT_LOAD&&ph[i].p_align>1&&bias%ph[i].p_align){say("STATICPIE_FAIL alignment\n");return 4;}
 }
 if(entry<UINT64_C(0x100000000)){say("STATICPIE_LOW_PLACEMENT\n");return 42;}
 for(unsigned i=0;i<sizeof(zeroed);i++)if(zeroed[i]){say("STATICPIE_FAIL bss\n");return 5;}
 long heap=call6(12,0,0,0,0,0,0);address("STATICPIE_BRK ",heap);
 if(heap<=0||call6(12,heap+8192,0,0,0,0,0)!=heap+8192){say("STATICPIE_FAIL brk grow\n");return 6;}
 *(volatile unsigned char*)heap=0x5a;if(*(volatile unsigned char*)heap!=0x5a||call6(12,heap,0,0,0,0,0)!=heap){say("STATICPIE_FAIL brk restore\n");return 7;}
 long anon=call6(9,0,4096,3,0x22,-1,0);address("STATICPIE_MMAP ",anon);
 if(anon<0){say("STATICPIE_FAIL mmap\n");return 8;}*(volatile unsigned char*)anon=0x6b;
 if(*(volatile unsigned char*)anon!=0x6b||call6(11,anon,4096,0,0,0,0)){say("STATICPIE_FAIL munmap\n");return 9;}
 long reserved=call6(9,0x110000,0x67ef0000,0,0x4032,-1,0);
 if(reserved!=0x110000){say("STATICPIE_FAIL low reservation\n");return 10;}
 if(initialized!=UINT64_C(0x2468ace013579bdf)||zeroed[4095]||h->e_type!=ET_DYN){say("STATICPIE_FAIL data after reservation\n");return 11;}
 if(call6(11,0x110000,0x67ef0000,0,0,0,0)){say("STATICPIE_FAIL reservation cleanup\n");return 12;}
 say("STATICPIE_RESERVE_PASS auxv data bss brk mmap low-fixed-reservation\n");return 0;
}
__attribute__((naked,noreturn)) void _start(void) {
 __asm__ volatile("mov %rsp,%rdi\n\tand $-16,%rsp\n\tcall check\n\tmov %rax,%rdi\n\tmov $60,%eax\n\tsyscall\n\tud2");
}
