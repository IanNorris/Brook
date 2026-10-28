#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc,char** argv){assert(argc==4);int fd=open(argv[1],O_RDONLY);assert(fd>=0);unsigned char bytes[65536];uint64_t off=strtoull(argv[2],0,10),expected=strtoull(argv[3],0,16);assert(pread(fd,bytes,sizeof(bytes),off)==-1 && errno==EIO);assert(pread(fd,bytes,sizeof(bytes),off)==sizeof(bytes));uint64_t hash=14695981039346656037ULL;for(unsigned i=0;i<sizeof(bytes);++i)hash=(hash^bytes[i])*1099511628211ULL;assert(hash==expected && close(fd)==0);puts("COALESCED_READ_EIO_RETRY_PASS");}
