// Reopen exact regular-file capabilities, never their diagnostic pathname.
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CK(x) do { ++checks; if (!(x)) { fprintf(stderr,"PREGFD FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while(0)
static void name(int fd,char *out){snprintf(out,64,"/proc/self/fd/%d",fd);}
static void same(int a,int b){struct stat x,y;CK(!fstat(a,&x)&&!fstat(b,&y));CK(x.st_dev==y.st_dev&&x.st_ino==y.st_ino&&S_ISREG(y.st_mode));}
static int reopening(int source){char p[64];name(source,p);int f=open(p,O_RDONLY|O_CLOEXEC);CK(f>=0);same(source,f);CK(fcntl(f,F_GETFD)==FD_CLOEXEC);CK(lseek(f,0,SEEK_CUR)==0);char b=0;CK(read(f,&b,1)==1&&b=='A');return f;}
static unsigned stopped,observations;static char race[64];
static void *reader(void *unused){
 (void)unused;
 while(!__atomic_load_n(&stopped,__ATOMIC_ACQUIRE)){
  int f=open(race,O_RDONLY);if(f<0){if(errno!=ENOENT){perror("PREGFD race");exit(1);}continue;}
  char b=0;if(read(f,&b,1)!=1||(b!='A'&&b!='B')||close(f)){fputs("PREGFD invalid race data\n",stderr);exit(1);}
  __atomic_add_fetch(&observations,1,__ATOMIC_RELEASE);
 }
 return NULL;
}
int main(int argc,char **argv){
 setbuf(stdout,NULL);alarm(60);CK(geteuid()!=0);
 if(argc==3&&!strcmp(argv[1],"--inherited")){int source=atoi(argv[2]);int f=reopening(source);CK(!close(f));return 0;}
 int brook=argc==2&&!strcmp(argv[1],"--brook");CK(argc==1||brook);
 char base[]="/tmp/pregfd-XXXXXX";CK(mkdtemp(base));int cwd=open(".",O_RDONLY|O_DIRECTORY);CK(cwd>=0&&!chdir(base));
 int source=open("original",O_CREAT|O_EXCL|O_RDWR,0600);CK(source>=0&&write(source,"ABC",3)==3);
 int f=reopening(source);CK(lseek(source,0,SEEK_CUR)==3);CK(!close(f));
 char exe[512];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);CK(n>0);exe[n]=0;
 pid_t child=fork();CK(child>=0);if(!child){char fd[32];snprintf(fd,sizeof(fd),"%d",source);execl(exe,exe,"--inherited",fd,(char *)NULL);_exit(99);}
 int status;CK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 CK(!rename("original","renamed"));int replacement=open("original",O_CREAT|O_EXCL|O_RDWR,0600);CK(replacement>=0&&write(replacement,"BBB",3)==3);
 f=reopening(source);CK(!close(f));CK(!unlink("renamed"));f=reopening(source);CK(!close(f));
 char p[80];name(source,p);struct stat st;CK(!stat(p,&st)&&S_ISREG(st.st_mode));CK(!access(p,R_OK));
 errno=0;CK(open(p,O_RDONLY|O_DIRECTORY)==-1&&errno==ENOTDIR);errno=0;CK(open(p,O_RDONLY|O_NOFOLLOW)==-1&&errno==ELOOP);
 int metadata=open(p,O_PATH);CK(metadata>=0);same(source,metadata);f=reopening(metadata);CK(!close(f)&&!close(metadata));
 metadata=open(p,O_PATH|O_NOFOLLOW);CK(metadata>=0&&!fstat(metadata,&st)&&S_ISLNK(st.st_mode)&&!close(metadata));
 size_t len=strlen(p);strcpy(p+len,"/child");errno=0;CK(open(p,O_RDONLY)==-1&&errno==ENOTDIR);p[len]=0;
 CK(!fchmod(source,0400));errno=0;CK(open(p,O_WRONLY)==-1&&errno==EACCES);CK(!fchmod(source,0600));
 int readonly=open(p,O_RDONLY);CK(readonly>=0);char rp[64];name(readonly,rp);int writable=open(rp,O_WRONLY);CK(writable>=0&&!close(writable)&&!close(readonly));
 int slot=fcntl(source,F_DUPFD,200);CK(slot>=0);name(slot,race);pthread_t thread;CK(!pthread_create(&thread,NULL,reader,NULL));
 while(!__atomic_load_n(&observations,__ATOMIC_ACQUIRE))sched_yield();
 for(unsigned i=0;i<100;++i){CK(dup2(source,slot)==slot&&!close(slot));CK(dup2(replacement,slot)==slot&&!close(slot));}
 __atomic_store_n(&stopped,1,__ATOMIC_RELEASE);CK(!pthread_join(thread,NULL));
 f=reopening(source);CK(!close(source));char b=0;CK(read(f,&b,1)==1&&b=='B'&&!close(f));errno=0;CK(open(p,O_RDONLY)==-1&&errno==ENOENT);
 CK(!close(replacement)&&!unlink("original")&&!fchdir(cwd)&&!close(cwd)&&!rmdir(base));
 printf("PREGFD PASS checks=%u race_observations=%u\n",checks,observations);return 0;
}
