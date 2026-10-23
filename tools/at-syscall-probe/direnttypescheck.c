#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"DIRENT FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
struct entry { uint64_t ino; int64_t off; unsigned short len; unsigned char type; char name[]; };
static void fixture(const char *parent) {
 char path[512];CHECK(snprintf(path,sizeof(path),"%s/direnttypes-XXXXXX",parent)>0);CHECK(mkdtemp(path)!=NULL);
 int d=open(path,O_RDONLY|O_DIRECTORY);CHECK(d>=0);
 int f=openat(d,"file",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(f>=0);CHECK(write(f,"ELF",3)==3);CHECK(close(f)==0);
 CHECK(mkdirat(d,"dir",0700)==0);CHECK(symlinkat("file",d,"libone.so")==0);CHECK(symlinkat("libone.so",d,"libone.so.0")==0);CHECK(symlinkat("dir",d,"dir-link")==0);CHECK(symlinkat("missing",d,"dangling")==0);
 char buf[4096];int found=0;long n;
 while ((n=syscall(SYS_getdents64,d,buf,sizeof(buf)))>0) {
  for(long off=0;off<n;) {
   struct entry *e=(struct entry*)(buf+off);CHECK(e->len>=20&&e->len<=n-off);CHECK(memchr(e->name,0,e->len-19)!=NULL);
   if(strcmp(e->name,".")&&strcmp(e->name,"..")) {
    struct stat st;CHECK(fstatat(d,e->name,&st,AT_SYMLINK_NOFOLLOW)==0);
    unsigned expected=S_ISLNK(st.st_mode)?DT_LNK:S_ISDIR(st.st_mode)?DT_DIR:DT_REG;
    CHECK(e->type==DT_UNKNOWN||e->type==expected);++found;
   }
   off+=e->len;
  }
 }
 CHECK(n==0);CHECK(found==6);CHECK(close(d)==0);
 DIR *stream=opendir(path);CHECK(stream!=NULL);struct dirent *e;int links=0;
 while((e=readdir(stream))) {if(e->d_type==DT_LNK)++links;else if(e->d_type==DT_UNKNOWN) {struct stat st;CHECK(fstatat(dirfd(stream),e->d_name,&st,AT_SYMLINK_NOFOLLOW)==0);if(S_ISLNK(st.st_mode))++links;}}
 CHECK(links==4);CHECK(closedir(stream)==0);
 d=open(path,O_RDONLY|O_DIRECTORY);CHECK(d>=0);
 for(size_t i=0;i<5;i++) {const char *names[]={"file","libone.so","libone.so.0","dir-link","dangling"};CHECK(unlinkat(d,names[i],0)==0);}
 CHECK(unlinkat(d,"dir",AT_REMOVEDIR)==0);CHECK(close(d)==0);CHECK(rmdir(path)==0);
}
int main(int argc,char **argv) {fixture(argc>1?argv[1]:"/tmp");printf("DIRENT PASS checks=%d\n",checks);return 0;}
