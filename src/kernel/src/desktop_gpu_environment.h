#pragma once
#include <stdint.h>
namespace brook {
// Kernel-owned immutable launch environments. Mesa's virtio-specific driver
// and ICD overrides must not constrain machines without a virtio GPU backend.
template<uint32_t N> struct DesktopGpuEnvironment { const char* entries[N+4]{}; };
struct DesktopMesaPaths { const char* dri; const char* gbm; const char* egl; };
constexpr bool DesktopTrustedGpuPath(uint32_t uid,uint16_t mode,bool isDir,bool isSymlink,bool expectDir) {
    return uid==0 && !(mode&0022) && !isSymlink && isDir==expectDir;
}
constexpr bool DesktopEnvPrefix(const char* value,const char* prefix) {
    while(*prefix)if(*value++!=*prefix++)return false;
    return true;
}
template<uint32_t N> constexpr DesktopGpuEnvironment<N> DesktopBuildGpuEnvironment(
    const char* const (&source)[N],bool virtio,bool direct,const DesktopMesaPaths* mesa=nullptr,const char* intelIcd=nullptr) {
    DesktopGpuEnvironment<N> out{};uint32_t count=0;
    for(uint32_t i=0;i<N && source[i];++i) {
        if(!virtio && (DesktopEnvPrefix(source[i],"MESA_LOADER_DRIVER_OVERRIDE=") ||
                       DesktopEnvPrefix(source[i],"VK_DRIVER_FILES=")))continue;
        if(!virtio && mesa) {
            if(DesktopEnvPrefix(source[i],"LIBGL_DRIVERS_PATH=")){out.entries[count++]=mesa->dri;continue;}
            if(DesktopEnvPrefix(source[i],"GBM_BACKENDS_PATH=")){out.entries[count++]=mesa->gbm;continue;}
            if(DesktopEnvPrefix(source[i],"__EGL_VENDOR_LIBRARY_DIRS=")){out.entries[count++]=mesa->egl;continue;}
        }
        out.entries[count++]=source[i];
    }
    if(!virtio && intelIcd){out.entries[count++]=intelIcd;out.entries[count++]="BROOK_I915_PARTIAL_CAPTURE=1";}
    out.entries[count++]=direct?"MESA_VK_WSI_DEBUG=":"MESA_VK_WSI_DEBUG=sw,noshm";
    out.entries[count++]=direct?"BROOK_VULKAN_PRESENTATION=direct":"BROOK_VULKAN_PRESENTATION=staged";
    return out;
}
}
