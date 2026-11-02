/* Verify that data accepted by send() reaches the peer before close's FIN. */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { TOTAL=256*1024, CHUNK=1024 };
static uint8_t payload[TOTAL];
int main(int argc,char** argv) {
    if(argc!=3)return 2;
    char* end;unsigned long port=strtoul(argv[2],&end,10);
    if(!*argv[2] || *end || !port || port>65535)return 2;
    signal(SIGPIPE,SIG_IGN);
    for(unsigned i=0;i<TOTAL;++i)payload[i]=(uint8_t)(i*17u+3u);
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){perror("socket");return 1;}
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if(inet_pton(AF_INET,argv[1],&peer.sin_addr)!=1 || connect(fd,(struct sockaddr*)&peer,sizeof(peer))){perror("connect");close(fd);return 1;}
    uint8_t ready;if(recv(fd,&ready,1,0)!=1 || ready!='B'){perror("ready");close(fd);return 1;}
    size_t sent=0;unsigned calls=0;
    while(sent<TOTAL){size_t want=TOTAL-sent<CHUNK?TOTAL-sent:CHUNK;
        ssize_t n=send(fd,payload+sent,want,MSG_NOSIGNAL);++calls;
        if(n<=0 || (size_t)n>want){perror("send");close(fd);return 1;}sent+=(size_t)n;
    }
    if(close(fd)){perror("close");return 1;}
    printf("TCP_CLOSE_AFTER_SEND_OK bytes=%zu calls=%u\n",sent,calls);return 0;
}
