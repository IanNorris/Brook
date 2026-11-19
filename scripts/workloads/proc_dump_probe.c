#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static atomic_int waiting;

__attribute__((noinline)) static void* pipe_waiter(void* argument)
{
    const int fd=*(int*)argument;
    char byte=0;
    atomic_store(&waiting,1);
    (void)read(fd,&byte,1);
    return NULL;
}

__attribute__((noinline)) static void running_main(void)
{
    for(;;)__asm__ volatile("pause" ::: "memory");
}

int main(void)
{
    int pipe_fds[2];
    pthread_t thread;
    if(pipe(pipe_fds) || pthread_create(&thread,NULL,pipe_waiter,&pipe_fds[0]))return 1;
    int file=open("/boot/PROCDUMP.ELF",O_RDONLY);
    if(file<0)return 2;
    void* named=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,file,0);
    close(file);
    if(named==MAP_FAILED || *(const volatile unsigned char*)named!=0x7f)return 3;
    while(!atomic_load(&waiting))sched_yield();
    printf("PROC_DUMP_READY %u\n",(unsigned)getpid());
    fflush(stdout);
    running_main();
}
