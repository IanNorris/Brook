#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
static unsigned checks;
#define CK(x) do{++checks;if(!(x)){fprintf(stderr,"PACKET_FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);exit(1);}}while(0)
static void empty(int r){char x;errno=0;CK(read(r,&x,1)==-1&&errno==EAGAIN);}
static void closepipe(int* p){CK(!close(p[0]));CK(!close(p[1]));}
int main(void){
 setbuf(stdout,NULL);alarm(60);int p[2];char out[16384],data[16384];memset(data,'p',sizeof(data));
 CK(!pipe2(p,O_DIRECT|O_NONBLOCK|O_CLOEXEC));
 CK((fcntl(p[0],F_GETFL)&(O_DIRECT|O_NONBLOCK|3))==O_NONBLOCK);
 CK((fcntl(p[1],F_GETFL)&(O_DIRECT|O_NONBLOCK|3))==(O_DIRECT|O_NONBLOCK|O_WRONLY));
 CK(fcntl(p[0],F_GETFD)==FD_CLOEXEC&&fcntl(p[1],F_GETFD)==FD_CLOEXEC);
 errno=0;CK(write(p[0],"x",1)==-1&&errno==EBADF);
 struct iovec bad={"x",1};errno=0;CK(writev(p[0],&bad,1)==-1&&errno==EBADF);
 errno=0;CK(read(p[1],out,1)==-1&&errno==EBADF);
 CK(write(p[1],"abc",3)==3&&write(p[1],"defg",4)==4);
 CK(read(p[0],out,0)==0);CK(read(p[0],out,1)==1&&out[0]=='a');
 CK(read(p[0],out,sizeof(out))==4&&!memcmp(out,"defg",4));empty(p[0]);
 CK(write(p[1],data,6000)==6000);
 CK(read(p[0],out,sizeof(out))==4096);CK(read(p[0],out,sizeof(out))==1904);empty(p[0]);
 struct iovec w[3]={{"he",2},{"",0},{"llo",3}};
 CK(writev(p[1],w,3)==5);
 struct iovec r[2]={{out,2},{out+2,10}};
 CK(readv(p[0],r,2)==5&&!memcmp(out,"hello",5));empty(p[0]);
 CK(write(p[1],"first",5)==5&&write(p[1],"next",4)==4);
 r[0].iov_len=1;r[1].iov_base=out+1;r[1].iov_len=1;
 CK(readv(p[0],r,2)==2&&!memcmp(out,"fi",2));CK(read(p[0],out,sizeof(out))==4&&!memcmp(out,"next",4));
 // Packets already queued remain framed after O_DIRECT is disabled.
 CK(write(p[1],"old",3)==3);CK(!fcntl(p[1],F_SETFL,O_NONBLOCK));
 CK(write(p[1],"stream",6)==6);
 CK(read(p[0],out,sizeof(out))==3&&!memcmp(out,"old",3));
 CK(read(p[0],out,sizeof(out))==6&&!memcmp(out,"stream",6));empty(p[0]);
 closepipe(p);
 // A large output buffer must not acquire a hidden 4K truncation boundary.
 for(int vector=0;vector<2;++vector){
  CK(!pipe2(p,O_NONBLOCK));CK(write(p[1],data,3000)==3000);
  CK(!fcntl(p[1],F_SETFL,O_NONBLOCK|O_DIRECT));CK(write(p[1],data,4000)==4000);
  if(vector){r[0].iov_base=out;r[0].iov_len=2000;r[1].iov_base=out+2000;r[1].iov_len=6000;CK(readv(p[0],r,2)==7000);}
  else CK(read(p[0],out,8000)==7000);
  empty(p[0]);closepipe(p);
 }
 // Every full packet is atomic under backpressure; don't assume Linux's
 // page-slot capacity is the same as Brook's byte-ring capacity.
 CK(!pipe2(p,O_DIRECT|O_NONBLOCK));unsigned packets=0;
 for(;;){ssize_t n=write(p[1],data,4096);if(n<0){CK(errno==EAGAIN);break;}CK(n==4096);CK(++packets<64);}
 for(unsigned i=0;i<packets;++i)CK(read(p[0],out,sizeof(out))==4096);
 empty(p[0]);closepipe(p);
 printf("PACKET_PIPE_PASS checks=%u framing/truncation/zero/large/vector/toggle/mixed/backpressure\n",checks);return 0;
}
