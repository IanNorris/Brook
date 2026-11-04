/* Disposable Brook VM regression for the optional diagnostic. Brook currently
 * ignores memfd names; Linux rejects invalid pointers, so this is not a Linux
 * compatibility oracle. Check serial separately for the 159-line cap. */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
 for (int i=0; i<4100; ++i) {
  const char *name=i==0?(const char *)(uintptr_t)0xffff800000000000ULL:i==1?(const char *)(uintptr_t)0x12345000:"pulse-test";
  long fd=syscall(SYS_memfd_create,name,1); if(fd<0){perror("memfd_create");return 1;} if(close(fd))return 2;
 }
 puts("MEMFD_TRACE_TEST_PASS");return 0;
}
