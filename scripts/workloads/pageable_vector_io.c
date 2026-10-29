#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static void cold(unsigned char* p,unsigned count){
    for(unsigned i=0;i<count;++i)assert(ctl(1,(long)(uintptr_t)(p+i*4096))==4096);
    assert(ctl(6,0)==0);
}
int main(void){
    char name[]="/tmp/brook-vector-XXXXXX";int fd=mkstemp(name);assert(fd>=0);
    int append=open(name,O_WRONLY|O_APPEND);assert(append>=0 && unlink(name)==0);
    int pipes[2],packet[2];assert(pipe(pipes)==0 && pipe2(packet,O_DIRECT)==0);
    enum { Pages=66, Count=64, Bytes=128 };
    long address=ctl(0,Pages*4096);assert(address>0);
    unsigned char* p=(void*)(uintptr_t)address;
    // Descriptor array straddles two pages; payloads cover more intervals than
    // the lease registry can list (overflow must conservatively cover the AS).
    struct iovec* vectors=(void*)(p+4096-sizeof(struct iovec));
    for(unsigned i=0;i<Count;++i){
        vectors[i]=(struct iovec){p+(i+2)*4096,Bytes};
        memset(vectors[i].iov_base,(int)(i+1),Bytes);
    }
    cold(p,Pages);
    assert(writev(fd,vectors,Count)==Count*Bytes && ctl(6,0)==0);
    assert(lseek(fd,0,SEEK_SET)==0);
    for(unsigned i=0;i<Count;++i)memset(vectors[i].iov_base,0,Bytes);
    cold(p,Pages);
    assert(readv(fd,vectors,Count)==Count*Bytes && ctl(6,0)==0);
    for(unsigned i=0;i<Count;++i)for(unsigned j=0;j<Bytes;++j)
        assert(((unsigned char*)vectors[i].iov_base)[j]==i+1);
    cold(p,Pages);
    assert(writev(append,vectors,Count)==Count*Bytes && ctl(6,0)==0);
    assert(lseek(fd,0,SEEK_END)==2*Count*Bytes);
    // First invalid output is rejected before consuming regular-file bytes.
    assert(lseek(fd,0,SEEK_SET)==0);
    assert(mprotect(p+8192,4096,PROT_READ)==0);
    errno=0;assert(readv(fd,vectors,1)==-1 && errno==EFAULT);
    assert(lseek(fd,0,SEEK_CUR)==0 && ctl(6,0)==0);
    assert(mprotect(p+8192,4096,PROT_READ|PROT_WRITE)==0);
    errno=0;assert(syscall(SYS_writev,fd,1L,1L)==-1 && errno==EFAULT);
    assert(ctl(6,0)==0);
    errno=0;assert(syscall(SYS_readv,fd,(long)(uintptr_t)vectors,1025L)==-1 && errno==EINVAL);
    assert(ctl(6,0)==0 && writev(fd,vectors,0)==0 && readv(fd,vectors,0)==0);
    for(unsigned kind=0;kind<2;++kind){
        int* pair=kind?packet:pipes;
        cold(p,Pages);
        assert(writev(pair[1],vectors,2)==2*Bytes && ctl(6,0)==0);
        memset(vectors[0].iov_base,0,Bytes);memset(vectors[1].iov_base,0,Bytes);
        cold(p,Pages);
        assert(readv(pair[0],vectors,2)==2*Bytes && ctl(6,0)==0);
        for(unsigned i=0;i<2;++i)for(unsigned j=0;j<Bytes;++j)
            assert(((unsigned char*)vectors[i].iov_base)[j]==i+1);
    }
    assert(munmap(p,Pages*4096)==0 && ctl(2,0)==0 && ctl(6,0)==0);
    close(fd);close(append);for(unsigned i=0;i<2;++i){close(pipes[i]);close(packet[i]);}
    puts("PAGEABLE_VECTOR_IO_PASS cold_descriptors 64_buffers generic append stream_packet_pipe readonly EFAULT lease_cleanup");
}
