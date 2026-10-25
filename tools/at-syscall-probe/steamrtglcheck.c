#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int shell(const char *overlay){
 pid_t p=fork();if(p<0)return 1;
 if(!p){if(overlay)setenv("LD_PRELOAD",overlay,1);else unsetenv("LD_PRELOAD");execl("/bin/bash","bash","-c","exit 0",(char*)0);perror("exec bash");_exit(127);}
 int s;if(waitpid(p,&s,0)!=p)return 1;printf("GLCHECK bash overlay=%d status=%d\n",overlay!=NULL,s);return WIFEXITED(s)&&WEXITSTATUS(s)==0?0:1;
}
int main(int argc,char **argv){
 setbuf(stdout,NULL);alarm(45);if(argc!=4)return 2;
 const char *names[]={"LD_LIBRARY_PATH","LD_PRELOAD","__GLX_VENDOR_LIBRARY_NAME",NULL};
 for(unsigned i=0;names[i];++i)printf("GLCHECK env %s=%s\n",names[i],getenv(names[i])?getenv(names[i]):"<unset>");
 const char *paths[]={argv[1],argv[2],"/overrides/lib/x86_64-linux-gnu/libGL.so.1","/usr/lib/pressure-vessel/overrides/lib/x86_64-linux-gnu/libGL.so.1","/etc/ld.so.cache",NULL};
 for(unsigned i=0;paths[i];++i){struct stat s;int r=stat(paths[i],&s);printf("GLCHECK stat path=%s result=%d errno=%d bytes=%lld\n",paths[i],r,r?errno:0,r?0:(long long)s.st_size);}
 char link[512];ssize_t n=readlink("/etc/ld.so.cache",link,sizeof(link)-1);if(n>=0){link[n]=0;printf("GLCHECK cache_link=%s\n",link);}
 int in=open("/etc/ld.so.cache",O_RDONLY),out=open(argv[3],O_CREAT|O_EXCL|O_WRONLY,0600);int copied=0;
 if(in>=0&&out>=0){char buffer[4096];ssize_t count;while((count=read(in,buffer,sizeof(buffer)))>0){if(write(out,buffer,count)!=count)break;copied+=count;}}
 if(in>=0)close(in);
 if(out>=0)close(out);
 printf("GLCHECK cache_copied=%d\n",copied);
 int plain=shell(NULL),overlay=shell(argv[1]);
 const char* previous=getenv("LD_LIBRARY_PATH");char path[8192];snprintf(path,sizeof(path),"/overrides/lib/x86_64-linux-gnu:%s",previous?previous:"");setenv("LD_LIBRARY_PATH",path,1);
 int override=shell(argv[1]);printf("GLCHECK result plain=%d overlay=%d override=%d\n",plain,overlay,override);return plain||override;
}
