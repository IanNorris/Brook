/* Host benchmark (not run by nix): GL fill rate into LINEAR vs X/Y-tiled
 * GBM buffers. UHD 630, 3192x1632, 8 blended quads: LINEAR 48.6 ms,
 * X 15.3 ms, Y 14.5 ms per frame; 800x600: 4.8 / 2.0 / 2.0 ms. */
/* Render N textured, blended full-target quads into a GBM BO with a given
 * modifier (LINEAR / X_TILED / Y_TILED) via EGLImage FBO; report ms/frame. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static GLuint sh(GLenum t,const char*s){GLuint h=glCreateShader(t);glShaderSource(h,1,&s,0);glCompileShader(h);return h;}
int main(int argc,char**argv){
  int W=argc>1?atoi(argv[1]):3192,H=argc>2?atoi(argv[2]):1632,quads=argc>3?atoi(argv[3]):8,frames=60;
  int fd=open("/dev/dri/renderD128",O_RDWR);struct gbm_device*g=gbm_create_device(fd);
  PFNEGLGETPLATFORMDISPLAYEXTPROC gp=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d=gp(EGL_PLATFORM_GBM_MESA,g,NULL);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext c=eglCreateContext(d,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,ca);eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c);
  printf("renderer %s\n",glGetString(GL_RENDERER));
  PFNEGLCREATEIMAGEKHRPROC ci=(void*)eglGetProcAddress("eglCreateImageKHR");
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC et=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  /* source texture: optimal internal (driver chooses tiling) */
  GLuint src;glGenTextures(1,&src);glBindTexture(GL_TEXTURE_2D,src);
  unsigned char*px=malloc(1024*1024*4);for(int i=0;i<1024*1024*4;i++)px[i]=rand();
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1024,1024,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  GLuint p=glCreateProgram();
  glAttachShader(p,sh(GL_VERTEX_SHADER,"attribute vec2 a;varying vec2 v;uniform float o;void main(){v=a*0.5+0.5+o;gl_Position=vec4(a,0,1);}"));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,"precision mediump float;varying vec2 v;uniform sampler2D t;void main(){gl_FragColor=texture2D(t,v*1.7)*0.6;}"));
  glBindAttribLocation(p,0,"a");glLinkProgram(p);glUseProgram(p);GLint uo=glGetUniformLocation(p,"o");
  float q[]={-1,-1,1,-1,-1,1,1,1};glVertexAttribPointer(0,2,GL_FLOAT,0,0,q);glEnableVertexAttribArray(0);
  glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
  struct {const char*n;uint64_t m;} mods[]={{"LINEAR",DRM_FORMAT_MOD_LINEAR},{"X_TILED",I915_FORMAT_MOD_X_TILED},{"Y_TILED",I915_FORMAT_MOD_Y_TILED}};
  for(int k=0;k<3;k++){
    struct gbm_bo*bo=gbm_bo_create_with_modifiers2(g,W,H,GBM_FORMAT_XRGB8888,&mods[k].m,1,GBM_BO_USE_RENDERING);
    if(!bo){printf("%s: alloc failed\n",mods[k].n);continue;}
    int dfd=gbm_bo_get_fd(bo);
    EGLint at[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_LINUX_DRM_FOURCC_EXT,DRM_FORMAT_XRGB8888,EGL_DMA_BUF_PLANE0_FD_EXT,dfd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)gbm_bo_get_stride(bo),
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,(EGLint)(mods[k].m&0xffffffff),EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,(EGLint)(mods[k].m>>32),EGL_NONE};
    EGLImageKHR img=ci(d,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,at);
    GLuint tex,fb;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);et(GL_TEXTURE_2D,img);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){printf("%s fbo incomplete\n",mods[k].n);continue;}
    glViewport(0,0,W,H);glBindTexture(GL_TEXTURE_2D,src);
    for(int warm=0;warm<2;warm++){double t0=now();
      for(int f=0;f<frames;f++){glClear(GL_COLOR_BUFFER_BIT);for(int i=0;i<quads;i++){glUniform1f(uo,i*0.013f+f*0.001f);glDrawArrays(GL_TRIANGLE_STRIP,0,4);}glFinish();}
      if(warm)printf("%-8s modifier=0x%016llx stride=%u: %.2f ms/frame (%d blended %dx%d quads)\n",mods[k].n,(unsigned long long)gbm_bo_get_modifier(bo),gbm_bo_get_stride(bo),(now()-t0)/frames,quads,W,H);}
    glDeleteFramebuffers(1,&fb);glDeleteTextures(1,&tex);close(dfd);gbm_bo_destroy(bo);
  }
  return 0;
}
