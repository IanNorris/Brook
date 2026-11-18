/* Host check (real Intel GPU, not run by nix): Mesa renders a coordinate
 * pattern into LINEAR/X/Y-tiled GBM buffers; src/shared/intel_tiling.h must
 * detile each exactly. Build: gcc -O2 intel-tiling-host-test.c $(pkg-config
 * --cflags --libs egl glesv2 gbm libdrm). Passed on UHD 630 (Gen9.5), 18 Nov. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include "../../src/shared/intel_tiling.h"
static GLuint sh(GLenum t,const char*s){GLuint h=glCreateShader(t);glShaderSource(h,1,&s,0);glCompileShader(h);return h;}
int main(){
  const int W=777,H=301;
  int fd=open("/dev/dri/renderD128",O_RDWR);struct gbm_device*g=gbm_create_device(fd);
  PFNEGLGETPLATFORMDISPLAYEXTPROC gp=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d=gp(EGL_PLATFORM_GBM_MESA,g,NULL);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext c=eglCreateContext(d,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,ca);eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c);
  PFNEGLCREATEIMAGEKHRPROC ci=(void*)eglGetProcAddress("eglCreateImageKHR");
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC et=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  GLuint p=glCreateProgram();
  glAttachShader(p,sh(GL_VERTEX_SHADER,"attribute vec2 a;void main(){gl_Position=vec4(a,0,1);}"));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,"precision highp float;void main(){vec2 f=floor(gl_FragCoord.xy);"
    "gl_FragColor=vec4(mod(f.x,256.0),floor(f.x/256.0)+mod(f.y,64.0)*4.0,mod(f.y,256.0),floor(f.y/256.0)+16.0)/255.0;}"));
  glBindAttribLocation(p,0,"a");glLinkProgram(p);glUseProgram(p);
  float q[]={-1,-1,1,-1,-1,1,1,1};glVertexAttribPointer(0,2,GL_FLOAT,0,0,q);glEnableVertexAttribArray(0);
  uint64_t mods[]={DRM_FORMAT_MOD_LINEAR,I915_FORMAT_MOD_X_TILED,I915_FORMAT_MOD_Y_TILED};int fails=0;
  for(int k=0;k<3;k++){
    struct gbm_bo*bo=gbm_bo_create_with_modifiers2(g,W,H,GBM_FORMAT_ARGB8888,&mods[k],1,GBM_BO_USE_RENDERING);
    int dfd=gbm_bo_get_fd(bo);uint32_t pitch=gbm_bo_get_stride(bo);
    EGLint at[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_LINUX_DRM_FOURCC_EXT,DRM_FORMAT_ARGB8888,EGL_DMA_BUF_PLANE0_FD_EXT,dfd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)pitch,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,(EGLint)(mods[k]&0xffffffff),EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,(EGLint)(mods[k]>>32),EGL_NONE};
    EGLImageKHR img=ci(d,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,at);
    GLuint tex,fb;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);et(GL_TEXTURE_2D,img);
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    glViewport(0,0,W,H);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();
    int tiling=brook_tiling_from_modifier(mods[k]);uint64_t bytes=brook_tiled_bytes(tiling,pitch,H);
    uint8_t*m=mmap(0,(bytes+4095)&~4095ull,PROT_READ,MAP_SHARED,dfd,0);if(m==MAP_FAILED){perror("mmap");return 1;}
    uint32_t*lin=malloc((size_t)W*H*4);brook_detile32(lin,W,m,tiling,pitch,W,H);
    int bad=0;for(int y=0;y<H;y++)for(int x=0;x<W;x++){
      /* GL origin bottom-left in FBO: row y in memory = fragcoord y. */
      uint32_t px=lin[(size_t)y*W+x];
      uint32_t b=px&255,gg=(px>>8)&255,r=(px>>16)&255,a=px>>24;
      uint32_t ex=r|((gg&3)<<8),ey=b|((a-16)<<8);
      if(ex!=(uint32_t)x || ey!=(uint32_t)y){if(bad<3)printf("  mismatch at %d,%d got %u,%u\n",x,y,ex,ey);bad++;}
    }
    printf("modifier=0x%016llx pitch=%u bytes=%llu mismatches=%d\n",(unsigned long long)mods[k],pitch,(unsigned long long)bytes,bad);
    fails+=bad!=0;munmap(m,(bytes+4095)&~4095ull);close(dfd);
  }
  printf(fails?"TILING_CHECK_FAIL\n":"TILING_CHECK_PASS\n");return fails;
}
