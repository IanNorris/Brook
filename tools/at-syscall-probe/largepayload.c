#include <stdio.h>
#include <string.h>
extern const unsigned char large_payload[40*1024*1024];
int main(int argc, char **argv) {
    unsigned sum=0;
    for (unsigned i=0;i<sizeof(large_payload);i+=4096) {
        if (large_payload[i]!=0xa5) return 1;
        sum+=large_payload[i];
    }
    if (large_payload[sizeof(large_payload)-1]!=0xa5) return 2;
    printf("LARGE_PAYLOAD_PASS mode=%s bytes=%zu sum=%u\n",argc>1?argv[1]:"shell",sizeof(large_payload),sum);
    return 37;
}
