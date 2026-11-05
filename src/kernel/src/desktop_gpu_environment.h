#pragma once
#include <stdint.h>
namespace brook {
// Kernel-owned immutable launch environments. Mesa's virtio-specific driver
// and ICD overrides must not constrain machines without a virtio GPU backend.
template<uint32_t N> struct DesktopGpuEnvironment { const char* entries[N+2]{}; };
constexpr bool DesktopEnvPrefix(const char* value,const char* prefix) {
    while(*prefix)if(*value++!=*prefix++)return false;
    return true;
}
template<uint32_t N> constexpr DesktopGpuEnvironment<N> DesktopBuildGpuEnvironment(
    const char* const (&source)[N],bool virtio,bool direct) {
    DesktopGpuEnvironment<N> out{};uint32_t count=0;
    for(uint32_t i=0;i<N && source[i];++i) {
        if(!virtio && (DesktopEnvPrefix(source[i],"MESA_LOADER_DRIVER_OVERRIDE=") ||
                       DesktopEnvPrefix(source[i],"VK_DRIVER_FILES=")))continue;
        out.entries[count++]=source[i];
    }
    out.entries[count++]=direct?"MESA_VK_WSI_DEBUG=":"MESA_VK_WSI_DEBUG=sw,noshm";
    out.entries[count++]=direct?"BROOK_VULKAN_PRESENTATION=direct":"BROOK_VULKAN_PRESENTATION=staged";
    return out;
}
}
