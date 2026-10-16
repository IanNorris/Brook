#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"PROCFDCHECK FAIL line=%d errno=%d\n",__LINE__,errno); ++failures; } } while (0)
static int reopen(int fd, int flags, int numeric) {
 char name[80];
 if(numeric) snprintf(name,sizeof(name),"/proc/%d/fd/%d",getpid(),fd);
 else snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);
 return open(name,flags);
}
static void expect_fault(volatile char *address, int writing) {
 pid_t child=fork();CHECK(child>=0);
 if(!child){if(writing)*address='!';else{volatile char value=*address;(void)value;}_exit(4);}
 if(child>0){int status;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);}
}
static void protection_splits(int capacity) {
 int fd=memfd_create("procfd-splits",0);CHECK(fd>=0);if(fd<0)return;
 CHECK(ftruncate(fd,12288)==0);
 char *mapping=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 CHECK(mapping!=MAP_FAILED);if(mapping==MAP_FAILED){close(fd);return;}
 if(capacity){
  enum { FILL_COUNT=768 };void *fill[FILL_COUNT];unsigned count=0;
  // Exercise metadata splits beyond the retired 256-entry table limit.
  mapping[0]='L';mapping[4096]='M';mapping[8192]='R';
  while(count<FILL_COUNT){void *p=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0);if(p==MAP_FAILED)break;fill[count++]=p;}
  CHECK(count==FILL_COUNT);
  for(unsigned i=0;i<count;i+=17)CHECK(*(volatile char*)fill[i]=='L');
  CHECK(!mprotect(mapping+4096,4096,PROT_NONE));
  CHECK(mapping[0]=='L'&&mapping[8192]=='R');
  printf("PROCFDCHECK dynamic mappings=%u\n",count);
  while(count)CHECK(!munmap(fill[--count],4096));
 }else{
  // Outside pages remain cold until after the middle permission change.
  CHECK(!mprotect(mapping+4096,4096,PROT_NONE));
  mapping[0]='L';mapping[8192]='R';CHECK(mapping[0]=='L'&&mapping[8192]=='R');
 }
 expect_fault(mapping+4096,0);
 CHECK(!mprotect(mapping+4096,4096,PROT_READ));
 CHECK(mapping[4096]==(capacity?'M':0));expect_fault(mapping+4096,1);
 CHECK(!mprotect(mapping+4096,4096,PROT_READ|PROT_WRITE));mapping[4096]='C';
 CHECK(mapping[0]=='L'&&mapping[4096]=='C'&&mapping[8192]=='R');
 CHECK(!munmap(mapping,12288));CHECK(!close(fd));
}
int main(int argc, char **argv) {
 int capacity=argc==2&&!strcmp(argv[1],"--brook-capacity");
 if(argc>1&&!capacity)return 2;
 syscall(SYS_alarm,30);
 int original=memfd_create("procfd-fixture",MFD_CLOEXEC);
 if(original<0) {perror("memfd");return 2;}
 CHECK((fcntl(original,F_GETFL)&O_ACCMODE)==O_RDWR);
 CHECK(ftruncate(original,4096)==0);
 CHECK(write(original,"abcdef",6)==6);
 int ro=reopen(original,O_RDONLY|O_CLOEXEC,0), rw=reopen(original,O_RDWR,1);
 if(ro<0||rw<0) {perror("procfd reopen");return 2;}
 struct stat a,b,c;
 CHECK(!fstat(original,&a)&&!fstat(ro,&b)&&!fstat(rw,&c));
 CHECK(a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_dev==c.st_dev&&a.st_ino==c.st_ino);
 CHECK((fcntl(ro,F_GETFL)&O_ACCMODE)==O_RDONLY);
 CHECK(fcntl(ro,F_GETFD)&FD_CLOEXEC);
 char bytes[8]={0}; CHECK(read(ro,bytes,3)==3&&!memcmp(bytes,"abc",3));
 CHECK(lseek(original,0,SEEK_CUR)==6&&lseek(rw,0,SEEK_CUR)==0);
 CHECK(pread(ro,bytes,3,3)==3&&!memcmp(bytes,"def",3));
 CHECK(lseek(ro,0,SEEK_CUR)==3);
 errno=0;CHECK(write(ro,"X",1)==-1&&errno==EBADF);
 errno=0;CHECK(pwrite(ro,"X",1,0)==-1&&errno==EBADF);
 errno=0;CHECK(ftruncate(ro,0)==-1&&errno==EINVAL);
 errno=0;void* denied=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,ro,0);
 CHECK(denied==MAP_FAILED&&errno==EACCES);if(denied!=MAP_FAILED)munmap(denied,4096);
 char *readmap=mmap(NULL,4096,PROT_READ,MAP_SHARED,ro,0);
 char *writemap=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,rw,0);
 if(readmap==MAP_FAILED||writemap==MAP_FAILED){perror("mapping");return 2;}
 CHECK(!memcmp(readmap,"abcdef",6));
 errno=0;CHECK(mprotect(readmap,4096,PROT_READ|PROT_WRITE)==-1&&errno==EACCES);
 CHECK(pwrite(rw,"Z",1,0)==1);CHECK(readmap[0]=='Z'&&writemap[0]=='Z');
 writemap[1]='Y';CHECK(pread(ro,bytes,2,0)==2&&!memcmp(bytes,"ZY",2));
 int copy=dup(ro);CHECK(copy>=0);errno=0;CHECK(write(copy,"X",1)==-1&&errno==EBADF);
 CHECK(close(original)==0);
 int wo=reopen(rw,O_WRONLY,0);CHECK(wo>=0);
 errno=0;CHECK(read(wo,bytes,1)==-1&&errno==EBADF);
 errno=0;denied=mmap(NULL,4096,PROT_READ,MAP_SHARED,wo,0);
 CHECK(denied==MAP_FAILED&&errno==EACCES);if(denied!=MAP_FAILED)munmap(denied,4096);
 pid_t child=fork();CHECK(child>=0);
 if(!child){errno=0;int ok=pread(copy,bytes,2,0)==2&&!memcmp(bytes,"ZY",2)&&write(copy,"X",1)==-1&&errno==EBADF;_exit(ok?0:3);}
 if(child>0){int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 CHECK(close(ro)==0&&close(rw)==0&&close(copy)==0&&close(wo)==0);
 CHECK(readmap[0]=='Z'&&writemap[1]=='Y');
 CHECK(munmap(readmap,4096)==0&&munmap(writemap,4096)==0);
 protection_splits(0);
 if(capacity)protection_splits(1);
 char shmname[96];snprintf(shmname,sizeof(shmname),"/dev/shm/brook-procfdcheck-%d",getpid());
 int shm=open(shmname,O_CREAT|O_RDWR|O_CLOEXEC,0600);CHECK(shm>=0);
 if(shm>=0){
  CHECK((fcntl(shm,F_GETFL)&O_ACCMODE)==O_RDWR);CHECK(fcntl(shm,F_GETFD)&FD_CLOEXEC);
  CHECK(ftruncate(shm,4096)==0&&pwrite(shm,"S",1,0)==1);
  int sr=open(shmname,O_RDONLY);CHECK(sr>=0);CHECK(close(shm)==0);
  if(sr>=0){CHECK((fcntl(sr,F_GETFL)&O_ACCMODE)==O_RDONLY);
   CHECK(pread(sr,bytes,1,0)==1&&bytes[0]=='S');errno=0;CHECK(write(sr,"X",1)==-1&&errno==EBADF);CHECK(close(sr)==0);}
  // Cleanup request only: namespace unlink behavior is not asserted here.
  unlink(shmname);
 }
 printf("PROCFDCHECK %s failures=%d\n",failures?"FAIL":"PASS",failures);
 return failures!=0;
}
