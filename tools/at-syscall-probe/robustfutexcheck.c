// Disposable Linux/Brook VM: real robust-list registration and owner-death paths.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
struct node {struct node* next;uint32_t word;};
struct head {struct node* next;long offset;struct node* pending;};
struct area {struct head head;struct node node;pthread_mutex_t mutex;};
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"ROBUST FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void waitok(pid_t child){int s;CK(waitpid(child,&s,0)==child);CK(WIFEXITED(s)&&WEXITSTATUS(s)==0);}
static void observe(pid_t child,int signal){siginfo_t i={0};CK(!waitid(P_PID,child,&i,WEXITED|WNOWAIT));CK(i.si_pid==child);CK(signal?i.si_code==CLD_KILLED&&i.si_status==signal:i.si_code==CLD_EXITED&&i.si_status==0);}
static void register_list(struct area* a,int pending,int foreign,int cycle){
 a->head.next=pending?(struct node*)&a->head:&a->node;a->head.offset=offsetof(struct node,word);a->head.pending=pending?&a->node:NULL;
 a->node.next=cycle?&a->node:(struct node*)&a->head;
 a->node.word=(uint32_t)syscall(SYS_gettid)+(foreign?1:0);a->node.word|=0x80000000u;
 CK(!syscall(SYS_set_robust_list,&a->head,sizeof(a->head)));
 void* head=NULL;size_t length=0;CK(!syscall(SYS_get_robust_list,0,&head,&length));CK(head==&a->head&&length==24);
}
static void deathcase(int kind){
 struct area* a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CK(a!=MAP_FAILED);
 int ready[2],release[2];CK(!pipe(ready)&&!pipe(release));
 pid_t p=fork();CK(p>=0);
 if(!p){alarm(10);close(ready[0]);close(release[1]);
  if(kind==4)CK(!syscall(SYS_set_robust_list,(void*)~(uintptr_t)0,24));
  else register_list(a,kind==1,kind==3,kind==2);
  CK(write(ready[1],"R",1)==1);char c;CK(read(release[0],&c,1)==1);syscall(SYS_exit,0);_exit(99);
 }
 close(ready[1]);close(release[0]);char c;CK(read(ready[0],&c,1)==1);uint32_t old=a->node.word;
 CK(write(release[1],"G",1)==1);observe(p,0);
 if(kind==3 || kind==4)CK(a->node.word==old);else CK(a->node.word==0xc0000000u);
 waitok(p);close(ready[0]);close(release[1]);CK(!munmap(a,4096));
}
static void killed(void){
 struct area* a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CK(a!=MAP_FAILED);
 int ready[2];CK(!pipe(ready));pid_t p=fork();CK(p>=0);
 if(!p){alarm(10);close(ready[0]);register_list(a,0,0,0);CK(write(ready[1],"R",1)==1);for(;;)pause();}
 close(ready[1]);char c;CK(read(ready[0],&c,1)==1);CK(!kill(p,SIGKILL));observe(p,SIGKILL);
 CK(a->node.word==0xc0000000u);int s;CK(waitpid(p,&s,0)==p&&WIFSIGNALED(s)&&WTERMSIG(s)==SIGKILL);close(ready[0]);CK(!munmap(a,4096));
}
struct group_args {struct area* a;int ready;};
static void* groupowner(void* raw){struct group_args* arg=raw;register_list(arg->a,0,0,0);CK(write(arg->ready,"R",1)==1);for(;;)pause();return NULL;}
static void groupcase(void){
 struct area* a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CK(a!=MAP_FAILED);
 int ready[2],release[2];CK(!pipe(ready)&&!pipe(release));pid_t p=fork();CK(p>=0);
 if(!p){alarm(10);close(ready[0]);close(release[1]);struct group_args arg={a,ready[1]};pthread_t thread;CK(!pthread_create(&thread,NULL,groupowner,&arg));char c;CK(read(release[0],&c,1)==1);syscall(SYS_exit_group,0);_exit(83);}
 close(ready[1]);close(release[0]);char c;CK(read(ready[0],&c,1)==1);CK(write(release[1],"G",1)==1);observe(p,0);CK(a->node.word==0xc0000000u);waitok(p);close(ready[0]);close(release[1]);CK(!munmap(a,4096));
}
static void* owner(void* arg){pthread_mutex_t* mutex=arg;int e=pthread_mutex_lock(mutex);if(e)_exit(80);return NULL;}
static void pthreadcase(void){
 pthread_mutex_t m;pthread_mutexattr_t attr;CK(!pthread_mutexattr_init(&attr));CK(!pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST));CK(!pthread_mutex_init(&m,&attr));
 pthread_t t;CK(!pthread_create(&t,NULL,owner,&m));CK(!pthread_join(t,NULL));
 struct timespec end;CK(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=3;
 CK(pthread_mutex_timedlock(&m,&end)==EOWNERDEAD);CK(!pthread_mutex_consistent(&m));CK(!pthread_mutex_unlock(&m));CK(!pthread_mutex_destroy(&m));CK(!pthread_mutexattr_destroy(&attr));
}
static void execcase(const char* self){
 struct area* a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CK(a!=MAP_FAILED);
 int ready[2],release[2];CK(!pipe(ready)&&!pipe(release));pid_t p=fork();CK(p>=0);
 if(!p){alarm(10);close(ready[0]);close(release[1]);register_list(a,0,0,0);CK(write(ready[1],"R",1)==1);
  char fd[24];snprintf(fd,sizeof(fd),"%d",release[0]);execl(self,self,"--exec-helper",fd,NULL);_exit(81);
 }
 close(ready[1]);close(release[0]);char c;CK(read(ready[0],&c,1)==1);
 struct timespec start,now;CK(!clock_gettime(CLOCK_MONOTONIC,&start));
 while(__atomic_load_n(&a->node.word,__ATOMIC_ACQUIRE)!=0xc0000000u){CK(!clock_gettime(CLOCK_MONOTONIC,&now));CK(now.tv_sec-start.tv_sec<5);usleep(1000);}
 // The exec image is waiting on this pipe: recovery precedes its eventual exit.
 CK(write(release[1],"G",1)==1);waitok(p);close(ready[0]);close(release[1]);CK(!munmap(a,4096));
}
static void cowcase(void){
 struct area* a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CK(a!=MAP_FAILED);
 int ready[2],release[2],result[2];CK(!pipe(ready)&&!pipe(release)&&!pipe(result));pid_t p=fork();CK(p>=0);
 if(!p){alarm(10);close(ready[0]);close(result[0]);register_list(a,0,0,0);uint32_t original=a->node.word;
  pid_t observer=fork();CK(observer>=0);
  if(!observer){close(release[1]);char c;CK(read(release[0],&c,1)==1);char ok=a->node.word==original?'Y':'N';CK(write(result[1],&ok,1)==1);_exit(0);}
  CK(write(ready[1],"R",1)==1);syscall(SYS_exit,0);_exit(82);
 }
 close(ready[1]);close(release[0]);close(result[1]);char c;CK(read(ready[0],&c,1)==1);observe(p,0);
 CK(write(release[1],"G",1)==1);CK(read(result[0],&c,1)==1&&c=='Y');waitok(p);
 close(ready[0]);close(release[1]);close(result[0]);CK(!munmap(a,4096));
}
// The lock's VMA is deliberately destroyed while its owner exits. There is no
// surviving mutex to recover; the contract is bounded cleanup without touching
// an unrelated replacement mapping or hanging exit under mapping churn.
struct churn_args {struct head head;void* page;int stop;unsigned rounds;};
static void* churnmapper(void* raw){
 struct churn_args* a=raw;
 while(!__atomic_load_n(&a->stop,__ATOMIC_ACQUIRE)){
  void* p=mmap(a->page,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
  if(p!=a->page)_exit(84);
  __atomic_add_fetch(&a->rounds,1,__ATOMIC_RELAXED);
  if(munmap(a->page,4096))_exit(85);
 }
 return NULL;
}
static void* churnowner(void* raw){
 struct churn_args* a=raw;
 CK(!syscall(SYS_set_robust_list,&a->head,24));return NULL;
}
static void churncase(void){
 struct churn_args a={0};a.page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CK(a.page!=MAP_FAILED);
 a.head.next=a.page;a.head.offset=offsetof(struct node,word);
 pthread_t mapper;CK(!pthread_create(&mapper,NULL,churnmapper,&a));
 while(!__atomic_load_n(&a.rounds,__ATOMIC_RELAXED))sched_yield();
 for(unsigned i=0;i<32;++i){pthread_t t;CK(!pthread_create(&t,NULL,churnowner,&a));CK(!pthread_join(t,NULL));}
 __atomic_store_n(&a.stop,1,__ATOMIC_RELEASE);CK(!pthread_join(mapper,NULL));CK(a.rounds>0);
 // munmap may already have been performed by the mapper.
 CK(!munmap(a.page,4096));
}
int main(int argc,char** argv){
 setbuf(stdout,NULL);alarm(120);
 if(argc==3&&!strcmp(argv[1],"--exec-helper")){char c;CK(read(atoi(argv[2]),&c,1)==1);return 0;}
 CK(argc==2&&!strcmp(argv[1],"--disposable-vm"));
 void* saved=NULL;size_t length=0;CK(!syscall(SYS_get_robust_list,0,&saved,&length)&&length==24);
 errno=0;CK(syscall(SYS_set_robust_list,saved,23)==-1&&errno==EINVAL);
 errno=0;CK(syscall(SYS_get_robust_list,0,(void*)1,&length)==-1&&errno==EFAULT);
 CK(!syscall(SYS_set_robust_list,saved,24));
 for(int kind=0;kind<5;++kind)deathcase(kind);
 killed();groupcase();pthreadcase();execcase(argv[0]);cowcase();churncase();
 printf("ROBUST_FUTEX_PASS checks=%u registration/get/normal/pending/cycle/foreign/malformed/kill/group/WNOWAIT/pthread/exec/COW/remap-churn\n",checks);return 0;
}
