/* Check the actual environment passed to a non-virtio guest service. */
static long syscall3(long n,long a,long b,long c) {
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return result;
}
static int prefix(const char* text,const char* expected) {
    while(*expected)if(*text++!=*expected++)return 0;
    return 1;
}
__attribute__((used,noinline,noreturn)) static void inspect(char** env) {
    int driver=0,icd=0,paths=0,gbm=0,egl=0,staged=0;
    for(;*env;++env) {
        driver|=prefix(*env,"MESA_LOADER_DRIVER_OVERRIDE=");
        icd|=prefix(*env,"VK_DRIVER_FILES=");
        paths|=prefix(*env,
#ifdef EXPECT_ALTERNATE_MESA
                      "LIBGL_DRIVERS_PATH=/nix/store/9kypdl7xqn01sis0ly591wmnsa7hm509-mesa-26.0.6/lib/dri");
        gbm|=prefix(*env,"GBM_BACKENDS_PATH=/nix/store/9kypdl7xqn01sis0ly591wmnsa7hm509-mesa-26.0.6/lib/gbm");
        egl|=prefix(*env,"__EGL_VENDOR_LIBRARY_DIRS=/nix/store/9kypdl7xqn01sis0ly591wmnsa7hm509-mesa-26.0.6/share/glvnd/egl_vendor.d");
#else
                      "LIBGL_DRIVERS_PATH=/nix/store/b793hmy0383vxaax7hiaslzmizzd3pmg-mesa-26.0.6/lib/dri");
        gbm|=prefix(*env,"GBM_BACKENDS_PATH=/nix/store/b793hmy0383vxaax7hiaslzmizzd3pmg-mesa-26.0.6/lib/gbm");
        egl|=prefix(*env,"__EGL_VENDOR_LIBRARY_DIRS=/nix/store/b793hmy0383vxaax7hiaslzmizzd3pmg-mesa-26.0.6/share/glvnd/egl_vendor.d");
#endif
        staged|=prefix(*env,"BROOK_VULKAN_PRESENTATION=staged");
    }
    static const char yes[]="GPU_ENV_NONVIRTIO_PASS\n",no[]="GPU_ENV_NONVIRTIO_FAIL\n";
    int ok=!driver && !icd && paths && gbm && egl && staged;
    syscall3(1,1,(long)(ok?yes:no),ok?sizeof(yes)-1:sizeof(no)-1);
    syscall3(60,ok?0:1,0,0);
    __builtin_unreachable();
}
__attribute__((naked,noreturn)) void _start(void) {
    __asm__ volatile("mov (%%rsp), %%rdi\n\tlea 16(%%rsp,%%rdi,8), %%rdi\n\tandq $-16, %%rsp\n\tcall inspect\n\tud2" ::: "memory");
}
