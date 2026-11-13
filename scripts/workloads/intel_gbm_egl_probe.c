// Direct Intel render-node GBM/EGL diagnostic; requires Mesa and libglvnd.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"INTEL_GBM_EGL_FAIL line=%d\n",__LINE__);return 1;}} while(0)
typedef void* Ptr;typedef unsigned Int;typedef int EInt;
static void fault(int signal,siginfo_t* info,void* opaque){
    ucontext_t* context=opaque;
#ifdef REG_RIP
    fprintf(stderr,"fault signal=%d address=%p rip=%llx\n",signal,info->si_addr,
            (unsigned long long)context->uc_mcontext.gregs[REG_RIP]);
    Dl_info details={0};if(dladdr((void*)(uintptr_t)context->uc_mcontext.gregs[REG_RIP],&details))
        fprintf(stderr,"fault object=%s base=%p symbol=%s\n",details.dli_fname,details.dli_fbase,details.dli_sname?details.dli_sname:"?");
    void* frames[24];int count=backtrace(frames,24);backtrace_symbols_fd(frames,count,2);
#else
    (void)context;fprintf(stderr,"fault signal=%d address=%p\n",signal,info->si_addr);
#endif
    _exit(128+signal);
}
int main(int argc, char** argv){
    const int cacheTest=argc==2 && !strcmp(argv[1],"--cache-test");
    if(argc!=1 && !cacheTest)return 2;
    setvbuf(stdout,0,_IONBF,0);
    struct sigaction handler={0};handler.sa_sigaction=fault;handler.sa_flags=SA_SIGINFO;
    sigemptyset(&handler.sa_mask);sigaction(SIGSEGV,&handler,0);sigaction(SIGILL,&handler,0);sigaction(SIGABRT,&handler,0);
    int fd=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    Ptr gbm=dlopen("libgbm.so.1",RTLD_NOW|RTLD_LOCAL),egl=dlopen("libEGL.so.1",RTLD_NOW|RTLD_LOCAL);
    if(!gbm || !egl)fprintf(stderr,"dlopen: %s\n",dlerror());
    CHECK(gbm && egl);
    Ptr (*create)(int)=dlsym(gbm,"gbm_create_device");void (*destroy)(Ptr)=dlsym(gbm,"gbm_device_destroy");
    Ptr (*getproc)(const char*)=dlsym(egl,"eglGetProcAddress");CHECK(create && destroy && getproc);
    Ptr (*platform)(Int,Ptr,const EInt*)=getproc("eglGetPlatformDisplayEXT");
    Int (*initialize)(Ptr,EInt*,EInt*)=dlsym(egl,"eglInitialize");
    Int (*choose)(Ptr,const EInt*,Ptr*,EInt,EInt*)=dlsym(egl,"eglChooseConfig");
    Int (*bind)(Int)=dlsym(egl,"eglBindAPI");Ptr (*context)(Ptr,Ptr,Ptr,const EInt*)=dlsym(egl,"eglCreateContext");
    Int (*current)(Ptr,Ptr,Ptr,Ptr)=dlsym(egl,"eglMakeCurrent");Int (*error)(void)=dlsym(egl,"eglGetError");
    CHECK(platform && initialize && choose && bind && context && current && error);
    Ptr device=create(fd);CHECK(device);Ptr display=platform(0x31d7,device,0);CHECK(display);
    EInt major=0,minor=0;if(!initialize(display,&major,&minor)){fprintf(stderr,"eglInitialize error=0x%x\n",error());return 1;}
    EInt configAttrs[]={0x3040,4,0x3024,8,0x3023,8,0x3022,8,0x3038};Ptr config=0;EInt count=0;
    CHECK(choose(display,configAttrs,&config,1,&count) && count && bind(0x30a0));
    EInt contextAttrs[]={0x3098,2,0x3038};Ptr ctx=context(display,config,0,contextAttrs);
    if(!ctx || !current(display,0,0,ctx)){fprintf(stderr,"eglContext error=0x%x\n",error());return 1;}
    const unsigned char* (*string)(Int)=getproc("glGetString");void (*clearColor)(float,float,float,float)=getproc("glClearColor");
    void (*clear)(Int)=getproc("glClear");void (*finish)(void)=getproc("glFinish");
    void (*read)(int,int,int,int,Int,Int,void*)=getproc("glReadPixels");CHECK(string && clearColor && clear && finish && read);
    printf("EGL %d.%d renderer=%s\n",major,minor,string(0x1f01));
    void (*genTextures)(int,Int*)=getproc("glGenTextures");void (*bindTexture)(Int,Int)=getproc("glBindTexture");
    void (*image)(Int,int,int,int,int,int,Int,Int,const void*)=getproc("glTexImage2D");
    void (*genFramebuffers)(int,Int*)=getproc("glGenFramebuffers");void (*bindFramebuffer)(Int,Int)=getproc("glBindFramebuffer");
    void (*attach)(Int,Int,Int,Int,int)=getproc("glFramebufferTexture2D");Int (*status)(Int)=getproc("glCheckFramebufferStatus");
    CHECK(genTextures && bindTexture && image && genFramebuffers && bindFramebuffer && attach && status);
    Int texture=0,fb=0;puts("stage=texture");genTextures(1,&texture);bindTexture(0x0de1,texture);
    puts("stage=image");image(0x0de1,0,0x1908,16,16,0,0x1908,0x1401,0);
    puts("stage=framebuffer");genFramebuffers(1,&fb);bindFramebuffer(0x8d40,fb);attach(0x8d40,0x8ce0,0x0de1,texture,0);CHECK(status(0x8d40)==0x8cd5);
    puts("stage=clear");clearColor(1,0,0,1);clear(0x4000);puts("stage=finish");finish();
    puts("stage=read");unsigned char pixel[4]={};read(0,0,1,1,0x1908,0x1401,pixel);
    printf("pixel=%u,%u,%u,%u\n",pixel[0],pixel[1],pixel[2],pixel[3]);
    CHECK(pixel[0]>240 && pixel[1]<16 && pixel[2]<16 && pixel[3]>240);
    puts("INTEL_GBM_EGL_PASS");
    Int (*createShader)(Int)=getproc("glCreateShader");
    void (*shaderSource)(Int,int,const char* const*,const int*)=getproc("glShaderSource");
    void (*compileShader)(Int)=getproc("glCompileShader");void (*shaderiv)(Int,Int,int*)=getproc("glGetShaderiv");
    Int (*createProgram)(void)=getproc("glCreateProgram");void (*attachShader)(Int,Int)=getproc("glAttachShader");
    void (*linkProgram)(Int)=getproc("glLinkProgram");void (*programiv)(Int,Int,int*)=getproc("glGetProgramiv");
    void (*useProgram)(Int)=getproc("glUseProgram");void (*vertexAttribPointer)(Int,int,Int,unsigned char,int,const void*)=getproc("glVertexAttribPointer");
    void (*enableVertexAttribArray)(Int)=getproc("glEnableVertexAttribArray");
    void (*viewport)(int,int,int,int)=getproc("glViewport");void (*drawArrays)(Int,int,int)=getproc("glDrawArrays");
    CHECK(createShader && shaderSource && compileShader && shaderiv && createProgram && attachShader && linkProgram && programiv && useProgram && vertexAttribPointer && enableVertexAttribArray && viewport && drawArrays);
    const char* vs="attribute vec2 p; void main(){gl_Position=vec4(p,0.0,1.0);}";
    const char* fs="precision mediump float; void main(){gl_FragColor=vec4(0.0,1.0,0.0,1.0);}";
    Int vertex=createShader(0x8b31),fragment=createShader(0x8b30);int ok=0;
    shaderSource(vertex,1,&vs,0);compileShader(vertex);shaderiv(vertex,0x8b81,&ok);CHECK(ok);
    shaderSource(fragment,1,&fs,0);compileShader(fragment);shaderiv(fragment,0x8b81,&ok);CHECK(ok);
    Int program=createProgram();attachShader(program,vertex);attachShader(program,fragment);
    void (*bindAttribLocation)(Int,Int,const char*)=getproc("glBindAttribLocation");CHECK(bindAttribLocation);
    bindAttribLocation(program,0,"p");linkProgram(program);programiv(program,0x8b82,&ok);CHECK(ok);useProgram(program);
    const float vertices[]={-1,-1,3,-1,-1,3};viewport(0,0,16,16);
    vertexAttribPointer(0,2,0x1406,0,0,vertices);enableVertexAttribArray(0);
    puts("stage=triangle");drawArrays(0x0004,0,3);finish();
    read(8,8,1,1,0x1908,0x1401,pixel);printf("triangle_pixel=%u,%u,%u,%u\n",pixel[0],pixel[1],pixel[2],pixel[3]);
    CHECK(pixel[0]<16 && pixel[1]>240 && pixel[2]<16 && pixel[3]>240);
    puts("INTEL_GBM_TRIANGLE_PASS");
    if(cacheTest){
        void (*parameter)(Int,Int,int)=getproc("glTexParameteri");
        void (*subImage)(Int,int,int,int,int,int,Int,Int,const void*)=getproc("glTexSubImage2D");
        Int (*glError)(void)=getproc("glGetError");
        CHECK(parameter && subImage && glError && !glError());
        Int src=0,out=0,second=0;
        genTextures(1,&src);genTextures(1,&out);genFramebuffers(1,&second);
        unsigned char expected[16*16*4]={0},actual[sizeof(expected)];
        const Int textures[]={src,texture,out};
        for(unsigned i=0;i<3;++i){
            bindTexture(0x0de1,textures[i]);
            parameter(0x0de1,0x2800,0x2600); // nearest magnification
            parameter(0x0de1,0x2801,0x2600); // nearest minification
            if(i!=1)image(0x0de1,0,0x1908,16,16,0,0x1908,0x1401,i==0?expected:NULL);
        }
        bindFramebuffer(0x8d40,second);
        attach(0x8d40,0x8ce0,0x0de1,out,0);CHECK(status(0x8d40)==0x8cd5);
        const char* sample="precision mediump float; uniform sampler2D image;"
            "void main(){gl_FragColor=texture2D(image,gl_FragCoord.xy/16.0);}";
        Int sampleShader=createShader(0x8b30);ok=0;
        shaderSource(sampleShader,1,&sample,0);compileShader(sampleShader);shaderiv(sampleShader,0x8b81,&ok);CHECK(ok);
        Int sampleProgram=createProgram();attachShader(sampleProgram,vertex);attachShader(sampleProgram,sampleShader);
        bindAttribLocation(sampleProgram,0,"p");linkProgram(sampleProgram);programiv(sampleProgram,0x8b82,&ok);CHECK(ok);
        useProgram(sampleProgram);
        for(unsigned round=1;round<=24;++round){
            for(unsigned i=0;i<16*16;++i){
                expected[4*i]=(unsigned char)(i*17+round*19);
                expected[4*i+1]=(unsigned char)(i*5+round*53);
                expected[4*i+2]=(unsigned char)(i*29+round*37);
                expected[4*i+3]=255;
            }
            // CPU uploads a new pattern to the same texture. The next
            // submission samples the first draw's reused render target.
            bindFramebuffer(0x8d40,fb);
            bindTexture(0x0de1,src);subImage(0x0de1,0,0,0,16,16,0x1908,0x1401,expected);
            drawArrays(0x0004,0,3);finish();CHECK(!glError());
            bindFramebuffer(0x8d40,second);
            bindTexture(0x0de1,texture);drawArrays(0x0004,0,3);finish();
            read(0,0,16,16,0x1908,0x1401,actual);CHECK(!glError());
            if(memcmp(actual,expected,sizeof(expected))){
                unsigned offset=0;while(actual[offset]==expected[offset])++offset;
                fprintf(stderr,"INTEL_GBM_TEXTURE_CACHE_FAIL round=%u byte=%u got=%u expected=%u\n",
                        round,offset,actual[offset],expected[offset]);return 1;
            }
        }
        puts("INTEL_GBM_TEXTURE_CACHE_PASS rounds=24 pixels_per_round=256");
    }
    destroy(device);close(fd);return 0;
}
