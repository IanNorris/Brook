/* Two independent GPU surfaces with identical titles. The observer samples
 * only fixture-owned pixels after the compositor has had time to converge. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"SAME_TITLE_GL_FAIL line=%d\n",__LINE__);return 1;} } while(0)
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t mark;
static void stop(int signal_number){(void)signal_number;stopping=1;}
static void marker(int signal_number){(void)signal_number;mark=1;}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGTERM,&action,NULL));
    action.sa_handler=marker;CHECK(!sigaction(SIGUSR2,&action,NULL));
    void *lib=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_LOCAL);CHECK(lib);
    int (*init)(unsigned)=dlsym(lib,"SDL_Init");
    void *(*window)(const char*,int,int,int,int,unsigned)=dlsym(lib,"SDL_CreateWindow");
    void *(*context)(void*)=dlsym(lib,"SDL_GL_CreateContext");
    int (*current)(void*,void*)=dlsym(lib,"SDL_GL_MakeCurrent");
    void *(*getproc)(const char*)=dlsym(lib,"SDL_GL_GetProcAddress");
    void (*swap)(void*)=dlsym(lib,"SDL_GL_SwapWindow");
    void (*pump)(void)=dlsym(lib,"SDL_PumpEvents");
    const char *(*getError)(void)=dlsym(lib,"SDL_GetError");
    void (*destroyContext)(void*)=dlsym(lib,"SDL_GL_DeleteContext");
    void (*destroyWindow)(void*)=dlsym(lib,"SDL_DestroyWindow");
    void (*quit)(void)=dlsym(lib,"SDL_Quit");
    CHECK(init && window && context && current && getproc && swap && pump && getError && destroyContext && destroyWindow && quit);
    CHECK(!init(0x20));
    void *w[2],*ctx[2];
    for(int i=0;i<2;++i){
        w[i]=window("Brook same title GPU fixture",0x2fff0000,0x2fff0000,320,240,2|4);
        if(!w[i])fprintf(stderr,"SDL_CreateWindow[%d]: %s\n",i,getError());
        CHECK(w[i]);
        ctx[i]=context(w[i]);CHECK(ctx[i]);
    }
    const unsigned char *(*getString)(unsigned)=getproc("glGetString");
    void (*clearColor)(float,float,float,float)=getproc("glClearColor");
    void (*clear)(unsigned)=getproc("glClear");
    CHECK(getString && clearColor && clear);
    const char *renderer=(const char *)getString(0x1f01);CHECK(renderer);
    printf("SAME_TITLE_GL_RENDERER %s\n",renderer);
    CHECK(strstr(renderer,"Intel") && !strstr(renderer,"llvmpipe"));
    for(unsigned frame=0;!stopping;++frame){
        for(int i=0;i<2;++i){CHECK(!current(w[i],ctx[i]));
            clearColor(i?0.0f:1.0f,0.0f,i?1.0f:0.0f,1.0f);clear(0x4000);swap(w[i]);}
        if(mark){struct timespec t={0};CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));mark=0;
            printf("SAME_TITLE_GL_MARK frames=%u ns=%llu\n",frame+1,(unsigned long long)t.tv_sec*1000000000ull+(unsigned long long)t.tv_nsec);}
        pump();if(frame==3)puts("SAME_TITLE_GL_READY red=first blue=second");usleep(50000);
    }
    for(int i=0;i<2;++i){destroyContext(ctx[i]);destroyWindow(w[i]);}
    quit();puts("SAME_TITLE_GL_PASS");return 0;
}
