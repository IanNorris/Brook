// Exercise an actual SDL Wayland GL swap through the compositor.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"SDL_GL_FAIL line=%d\n",__LINE__);return 1;} } while(0)
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    void *lib=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_LOCAL);CHECK(lib);
    int (*init)(unsigned)=dlsym(lib,"SDL_Init");
    void *(*window)(const char*,int,int,int,int,unsigned)=dlsym(lib,"SDL_CreateWindow");
    void *(*context)(void*)=dlsym(lib,"SDL_GL_CreateContext");
    void *(*getproc)(const char*)=dlsym(lib,"SDL_GL_GetProcAddress");
    void (*swap)(void*)=dlsym(lib,"SDL_GL_SwapWindow");
    void (*clearError)(void)=dlsym(lib,"SDL_ClearError");
    const char *(*getError)(void)=dlsym(lib,"SDL_GetError");
    void (*destroyContext)(void*)=dlsym(lib,"SDL_GL_DeleteContext");
    void (*destroyWindow)(void*)=dlsym(lib,"SDL_DestroyWindow");
    void (*quit)(void)=dlsym(lib,"SDL_Quit");
    CHECK(init && window && context && getproc && swap && clearError && getError && destroyContext && destroyWindow && quit);
    CHECK(!init(0x20));puts("stage=window");
    void *w=window("Brook Intel GL probe",0x2fff0000,0x2fff0000,320,240,2|4);CHECK(w);
    puts("stage=context");void *ctx=context(w);CHECK(ctx);
    const unsigned char *(*getString)(unsigned)=getproc("glGetString");
    void (*clearColor)(float,float,float,float)=getproc("glClearColor");
    void (*clear)(unsigned)=getproc("glClear");void (*finish)(void)=getproc("glFinish");
    CHECK(getString && clearColor && clear && finish);
    printf("renderer=%s\n",getString(0x1f01));
    clearColor(0,1,0,1);clear(0x4000);finish();puts("stage=swap");clearError();swap(w);
    const char *error=getError();if(error && *error){fprintf(stderr,"SDL swap: %s\n",error);return 1;}
    puts("SDL_GL_SWAP_RETURNED");
    sleep(8);destroyContext(ctx);destroyWindow(w);quit();return 0;
}
