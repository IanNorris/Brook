#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static unsigned char data[32768],reply[32768],disk[65519];
static int fail(const char* why){printf("ECM_TEST_FAIL %s errno=%d\n",why,errno);return 1;}
static int exact(int fd,void* data,size_t size,int sending){
    size_t done=0;while(done<size){ssize_t n=sending?write(fd,(char*)data+done,size-done):read(fd,(char*)data+done,size-done);if(n<=0)return -1;done+=(size_t)n;}return 0;
}
static int storage(int fd,unsigned pass){
    const off_t offset=511+(pass*65519u)%(8*1024*1024-sizeof(disk)-511);
    if(pread(fd,disk,sizeof(disk),offset)!=(ssize_t)sizeof(disk))return -1;
    for(unsigned i=0;i<sizeof(disk);++i)if(disk[i]!=(unsigned char)(((offset+i)*17+5)%251))return -1;
    return 0;
}
int main(int argc,char** argv){
    setvbuf(stdout,NULL,_IONBF,0);if(argc!=2)return 2;
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons((unsigned short)atoi(argv[1]))};
    inet_pton(AF_INET,"10.0.2.100",&peer.sin_addr);
    int file=open("/media/usb/READTEST.BIN",O_RDONLY);if(file<0)return fail("USB file");
    int tcp=socket(AF_INET,SOCK_STREAM,0);if(tcp<0 || connect(tcp,(struct sockaddr*)&peer,sizeof(peer)))return fail("connect");
    // 14-byte Ethernet + 20-byte IPv4 + 20-byte TCP + 10 data = 64.
    char small[]="ecm-zlp-ok";if(exact(tcp,small,10,1) || exact(tcp,reply,10,0) || memcmp(small,reply,10))return fail("64-byte ZLP echo");
    usleep(1500000); // RX remains posted while idle.
    for(unsigned pass=0;pass<8;++pass){
        for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)((i*13+pass*7)%251);
        if(exact(tcp,data,sizeof(data),1) || exact(tcp,reply,sizeof(reply),0) || memcmp(data,reply,sizeof(data)))return fail("TCP integrity");
        if(storage(file,pass))return fail("concurrent USB read");
    }
    close(tcp);
    int udp=socket(AF_INET,SOCK_DGRAM,0);if(udp<0)return fail("UDP socket");
    // 14 Ethernet + 20 IPv4 + 8 UDP + 982 data = 1024 (also an SS packet).
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=peer.sin_port};
    if(bind(udp,(struct sockaddr*)&local,sizeof(local)))return fail("UDP bind");
    socklen_t length=sizeof(peer);struct pollfd wait={.fd=udp,.events=POLLIN};
    if(poll(&wait,1,10000)<=0)return fail("UDP readiness");
    if(recvfrom(udp,reply,sizeof(reply),0,(struct sockaddr*)&peer,&length)!=982 || memcmp(data,reply,982))return fail("1024-byte frame receive");
    if(sendto(udp,reply,982,0,(struct sockaddr*)&peer,length)!=982)return fail("1024-byte ZLP echo");
    close(udp);puts("ECM_NETWORK_INTEGRITY_OK bytes=262144 zlp_frames=64,1024");
    sleep(2);
    for(unsigned pass=0;pass<8;++pass)if(storage(file,pass))return fail("USB after disconnect");
    close(file);puts("ECM_USB_STILL_ALIVE");return 0;
}
