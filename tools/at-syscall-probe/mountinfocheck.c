// Identical Linux/Brook disposable-VM probe for namespace mount metadata.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"MINFO FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
struct row {unsigned id,parent,major,minor;char root[512],point[512],flags[256],type[64];};
static struct row rows[512];static unsigned count;
static void decode(char* text){char* p=text;for(char* s=text;*s;) {if(s[0]=='\\'&&s[1]>='0'&&s[1]<='7'&&s[2]>='0'&&s[2]<='7'&&s[3]>='0'&&s[3]<='7'){*p++=(s[1]-'0')*64+(s[2]-'0')*8+s[3]-'0';s+=4;}else *p++=*s++;}*p=0;}
static void readinfo(void){
 FILE* f=fopen("/proc/self/mountinfo","r");CK(f);char line[8192];count=0;
 while(fgets(line,sizeof(line),f)) {
  CK(strchr(line,'\n'));CK(count<512);struct row* r=&rows[count++];char root[2048],point[2048];
  CK(sscanf(line,"%u %u %u:%u %2047s %2047s %255s",&r->id,&r->parent,&r->major,&r->minor,root,point,r->flags)==7);
  decode(root);decode(point);CK(strlen(root)<sizeof(r->root)&&strlen(point)<sizeof(r->point));strcpy(r->root,root);strcpy(r->point,point);
  char* sep=strstr(line," - ");CK(sep&&sscanf(sep+3,"%63s",r->type)==1);
  CK(r->id);for(unsigned j=0;j+1<count;++j)CK(rows[j].id!=r->id);
 }
 CK(!ferror(f)&&!fclose(f)&&count);
 for(unsigned i=0;i<count;++i){int found=!strcmp(rows[i].point,"/");for(unsigned j=0;j<count;++j)if(rows[i].parent==rows[j].id)found=1;CK(found);}
}
static struct row find(const char* point){struct row* r=NULL;for(unsigned i=0;i<count;++i)if(!strcmp(rows[i].point,point))r=&rows[i];if(!r){fprintf(stderr,"MINFO missing point=[%s] rows=%u\n",point,count);for(unsigned i=0;i<count;++i)fprintf(stderr,"ROW %u %u [%s] [%s]\n",rows[i].id,rows[i].parent,rows[i].root,rows[i].point);}CK(r);return *r;}
static void map(const char* p,unsigned id){char t[64];int n=snprintf(t,sizeof(t),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,t,n)==n);CK(!close(f));}
static void waitok(pid_t p){int s;CK(waitpid(p,&s,0)==p);CK(WIFEXITED(s)&&!WEXITSTATUS(s));}
static void same(struct row r,const char* physical,const char* logical){
 CK(!strcmp(r.root,physical)&&!strcmp(r.point,logical)&&!strcmp(r.type,"tmpfs"));
 CK(strstr(r.flags,"nosuid")&&strstr(r.flags,"nodev"));struct stat st;CK(!stat(logical,&st));CK(r.major==major(st.st_dev)&&r.minor==minor(st.st_dev));
}
static void linkequals(int fd,const char* expected){char name[64],value[512];snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);ssize_t n=readlink(name,value,sizeof(value)-1);CK(n>=0);value[n]=0;CK(!strcmp(value,expected));}
static int slot,stopReader;static unsigned linkReads;static char linkName[64],allowedA[128],allowedB[128];
static void* readlinks(void* unused){
 (void)unused;char value[512];
 while(!__atomic_load_n(&stopReader,__ATOMIC_ACQUIRE)) {
  ssize_t n=readlink(linkName,value,sizeof(value)-1);
  if(n<0){if(errno!=ENOENT){perror("racing readlink");exit(1);}continue;}
  value[n]=0;if(strcmp(value,allowedA)&&strcmp(value,allowedB)){fprintf(stderr,"MINFO bad racing fd path [%s]\n",value);exit(1);}
  __atomic_add_fetch(&linkReads,1,__ATOMIC_RELEASE);
 }
 return NULL;
}
int main(int argc,char** argv){
 setbuf(stdout,NULL);alarm(60);CK(argc==2&&!strcmp(argv[1],"--disposable-vm"));CK(geteuid()!=0);
 char base[]="/tmp/mi-XXXXXX";CK(mkdtemp(base));uid_t uid=geteuid();gid_t gid=getegid();
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){waitok(child);CK(!rmdir(base));printf("MINFO PARENT PASS checks=%u\n",checks);return 0;}
 map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0&&write(f,"deny\n",5)==5&&!close(f));map("/proc/self/gid_map",gid);
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));CK(!mount("tmpfs",base,"tmpfs",MS_NODEV|MS_NOSUID,NULL));CK(!chdir(base));
 const char* source="src \t\\\n";const char* target="dst \t\\\n";const char* renamed="new \t\\\n";
 CK(!mkdir(source,0700)&&!mkdir(target,0700)&&!mkdir("other",0700));char nested[64];snprintf(nested,sizeof(nested),"%s/sub",source);CK(!mkdir(nested,0700));
 CK(!mount("tmpfs",nested,"tmpfs",MS_NODEV|MS_NOSUID,NULL));
 int held=open(target,O_RDONLY|O_DIRECTORY),sourcefd=open(source,O_PATH|O_DIRECTORY);CK(held>=0&&sourcefd>=0);CK(!mount(source,target,NULL,MS_BIND|MS_REC,NULL));
 char logical[128],physical[128],sub[128];snprintf(logical,sizeof(logical),"%s/%s",base,target);snprintf(physical,sizeof(physical),"/%s",source);snprintf(sub,sizeof(sub),"%s/sub",logical);
 readinfo();struct row bound=find(logical),nestedRow=find(sub);same(bound,physical,logical);same(nestedRow,"/",sub);CK(nestedRow.parent==bound.id);
 CK(!rename(source,renamed));snprintf(physical,sizeof(physical),"/%s",renamed);readinfo();struct row moved=find(logical);CK(moved.id==bound.id);same(moved,physical,logical);
 char sourceLabel[128];snprintf(sourceLabel,sizeof(sourceLabel),"%s/%s",base,renamed);linkequals(sourcefd,sourceLabel);linkequals(held,logical);CK(!close(sourcefd));
 pid_t copy=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(copy>=0);
 if(!copy){readinfo();struct row cloned=find(logical);CK(cloned.id!=bound.id);same(cloned,physical,logical);puts("MINFO COPY PASS");_exit(0);}waitok(copy);
 readinfo();CK(find(logical).id==bound.id);
 int otherfd=open("other",O_PATH|O_DIRECTORY);CK(otherfd>=0);slot=dup(held);CK(slot>=0);snprintf(linkName,sizeof(linkName),"/proc/self/fd/%d",slot);strcpy(allowedA,logical);snprintf(allowedB,sizeof(allowedB),"%s/other",base);
 pthread_t reader;CK(!pthread_create(&reader,NULL,readlinks,NULL));while(!__atomic_load_n(&linkReads,__ATOMIC_ACQUIRE))sched_yield();
 for(unsigned i=0;i<500;++i){CK(dup2(held,slot)==slot);CK(!close(slot));CK(dup2(otherfd,slot)==slot);CK(!close(slot));}
 __atomic_store_n(&stopReader,1,__ATOMIC_RELEASE);CK(!pthread_join(reader,NULL));CK(!close(otherfd));printf("MINFO FD RACE PASS reads=%u\n",linkReads);
 char other[128];snprintf(other,sizeof(other),"%s/other",base);CK(!fchdir(held));CK(!mount(other,".",NULL,MS_BIND,NULL));CK(!chdir(base));
 readinfo();struct row top=find(logical);CK(top.id!=bound.id&&top.parent==bound.id);same(top,"/other",logical);
 CK(!umount2(target,MNT_DETACH));readinfo();CK(find(logical).id==bound.id);
 CK(!umount2(target,MNT_DETACH));CK(!close(held));CK(!chdir("/"));CK(!umount2(base,MNT_DETACH));
 printf("MINFO FIXTURE PASS checks=%u\n",checks);_exit(0);
}
