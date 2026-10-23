#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static int checks;
#define CK(x) do { ++checks; if(!(x)){fprintf(stderr,"RLFD FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} }while(0)
static atomic_int stopped,failed,observations;
static void *reader(void *unused){(void)unused;while(!atomic_load(&stopped)){char b[8];ssize_t n=readlinkat(100,"",b,sizeof(b));if(n==1&&(b[0]=='A'||b[0]=='B'))atomic_fetch_add(&observations,1);else if(n!=-1||errno!=EBADF)atomic_store(&failed,1);}return NULL;}
int main(int argc,char **argv){
 char path[512];CK(snprintf(path,sizeof(path),"%s/readlinkfd-XXXXXX",argc>1?argv[1]:"/tmp")>0);CK(mkdtemp(path)!=NULL);int d=open(path,O_RDONLY|O_DIRECTORY);CK(d>=0);
 CK(symlinkat("alpha",d,"link")==0);int a=openat(d,"link",O_PATH|O_NOFOLLOW);CK(a>=0);char b[32];memset(b,'!',sizeof(b));CK(readlinkat(a,"",b,2)==2&&!memcmp(b,"al!",3));CK(readlinkat(a,"",b,sizeof(b))==5&&!memcmp(b,"alpha",5));
 errno=0;CK(syscall(SYS_readlinkat,a,"",b,0)==-1&&errno==EINVAL);errno=0;CK(readlinkat(-1,"",b,sizeof(b))==-1&&errno==EBADF);errno=0;CK(readlinkat(AT_FDCWD,"",b,sizeof(b))==-1&&errno==ENOENT);errno=0;CK(readlink("",b,sizeof(b))==-1&&errno==ENOENT);
 int f=openat(d,"file",O_CREAT|O_RDWR,0600);CK(f>=0);errno=0;CK(readlinkat(f,"",b,sizeof(b))==-1&&errno==ENOENT);errno=0;CK(readlinkat(d,"",b,sizeof(b))==-1&&errno==ENOENT);
 CK(renameat(d,"link",d,"moved")==0);CK(symlinkat("replacement",d,"link")==0);CK(readlinkat(a,"",b,sizeof(b))==5&&!memcmp(b,"alpha",5));CK(unlinkat(d,"moved",0)==0);CK(readlinkat(a,"",b,sizeof(b))==5&&!memcmp(b,"alpha",5));CK(close(a)==0);
 CK(symlinkat("A",d,"a")==0);CK(symlinkat("B",d,"b")==0);a=openat(d,"a",O_PATH|O_NOFOLLOW);int c=openat(d,"b",O_PATH|O_NOFOLLOW);CK(a>=0&&c>=0);CK(dup2(a,100)==100);pthread_t thread;CK(pthread_create(&thread,NULL,reader,NULL)==0);
 for(int i=0;i<2000;i++){CK(close(100)==0);int r=-1;for(int retry=0;retry<10000;retry++){r=dup2(i&1?a:c,100);if(r==100)break;CK(errno==EBUSY);sched_yield();}CK(r==100);}
 atomic_store(&stopped,1);CK(pthread_join(thread,NULL)==0);CK(!atomic_load(&failed));CK(atomic_load(&observations)>0);CK(close(100)==0);CK(close(a)==0);CK(close(c)==0);CK(close(f)==0);
 for(int i=0;i<4;i++){const char *names[]={"file","link","a","b"};CK(unlinkat(d,names[i],0)==0);}CK(close(d)==0);CK(rmdir(path)==0);
 printf("RLFD PASS checks=%d observations=%d\n",checks,atomic_load(&observations));return 0;
}
