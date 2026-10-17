#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
struct window_result {uint32_t id,stride;uint64_t pixels;};
#define CHECK(c) do {if(!(c)){printf("WMTERMINALRESIZECHECK FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void) {
    // Launch from a Brook Terminal, so this window owner is a descendant of
    // its pinned bash child. Resizing this GUI must not resize that terminal.
    struct winsize before={0},after={0};
    CHECK(ioctl(1,TIOCGWINSZ,&before)==0 && before.ws_row && before.ws_col);
    struct window_result window={0};
    CHECK(syscall(506,360,180,"Resize ownership test",&window,0)==0);
    for(unsigned round=0;round<12;++round) {
        CHECK(syscall(515,window.id,1)==0);
        CHECK(ioctl(1,TIOCGWINSZ,&after)==0);
        if(after.ws_row!=before.ws_row || after.ws_col!=before.ws_col) {
            (void)syscall(507,window.id);
            printf("WMTERMINALRESIZECHECK FAIL parent terminal changed %ux%u -> %ux%u\n",before.ws_row,before.ws_col,after.ws_row,after.ws_col);
            return 1;
        }
        CHECK(syscall(515,window.id,0)==0);
        CHECK(ioctl(1,TIOCGWINSZ,&after)==0 && after.ws_row==before.ws_row && after.ws_col==before.ws_col);
    }
    CHECK(syscall(507,window.id)==0);
    printf("WMTERMINALRESIZECHECK PASS rounds=12 parent=%ux%u\n",before.ws_row,before.ws_col);
    return 0;
}
