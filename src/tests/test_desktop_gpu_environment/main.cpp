#include "desktop_gpu_environment.h"
#include <cassert>
#include <cstring>
using namespace brook;
static bool has(const char* const* env,const char* value) {
    for(;*env;++env)if(!std::strcmp(*env,value))return true;
    return false;
}
int main() {
    constexpr const char* base[]={"HOME=/home/test","MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu",
        "VK_DRIVER_FILES=/mesa/virtio.json","LIBGL_DRIVERS_PATH=/mesa/dri",
        "GBM_BACKENDS_PATH=/mesa/gbm","__EGL_VENDOR_LIBRARY_DIRS=/mesa/egl",nullptr};
    constexpr DesktopMesaPaths alternate{"LIBGL_DRIVERS_PATH=/other/dri",
        "GBM_BACKENDS_PATH=/other/gbm","__EGL_VENDOR_LIBRARY_DIRS=/other/egl"};
    constexpr auto virtio=DesktopBuildGpuEnvironment(base,true,true);
    constexpr auto staged=DesktopBuildGpuEnvironment(base,true,false);
    constexpr auto other=DesktopBuildGpuEnvironment(base,false,false);
    constexpr auto dell=DesktopBuildGpuEnvironment(base,false,false,&alternate);
    constexpr auto intelVulkan=DesktopBuildGpuEnvironment(base,false,false,&alternate,"VK_DRIVER_FILES=/system/intel.json");
    constexpr auto virtioWithIntel=DesktopBuildGpuEnvironment(base,true,true,&alternate,"VK_DRIVER_FILES=/system/intel.json");
    constexpr auto virtioWithAlternate=DesktopBuildGpuEnvironment(base,true,true,&alternate);
    assert(has(virtio.entries,"MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu"));
    assert(has(virtio.entries,"VK_DRIVER_FILES=/mesa/virtio.json"));
    assert(has(virtio.entries,"MESA_VK_WSI_DEBUG="));
    assert(has(virtio.entries,"BROOK_VULKAN_PRESENTATION=direct"));
    assert(has(staged.entries,"VK_DRIVER_FILES=/mesa/virtio.json"));
    assert(has(staged.entries,"MESA_VK_WSI_DEBUG=sw,noshm"));
    assert(!has(other.entries,"MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu"));
    assert(!has(other.entries,"VK_DRIVER_FILES=/mesa/virtio.json"));
    assert(has(other.entries,"LIBGL_DRIVERS_PATH=/mesa/dri"));
    assert(has(other.entries,"HOME=/home/test"));
    assert(has(other.entries,"BROOK_VULKAN_PRESENTATION=staged"));
    assert(has(dell.entries,"LIBGL_DRIVERS_PATH=/other/dri"));
    assert(has(dell.entries,"GBM_BACKENDS_PATH=/other/gbm"));
    assert(has(dell.entries,"__EGL_VENDOR_LIBRARY_DIRS=/other/egl"));
    assert(!has(dell.entries,"LIBGL_DRIVERS_PATH=/mesa/dri"));
    assert(!has(dell.entries,"VK_DRIVER_FILES=/mesa/virtio.json"));
    assert(has(intelVulkan.entries,"VK_DRIVER_FILES=/system/intel.json"));
    assert(has(intelVulkan.entries,"BROOK_I915_PARTIAL_CAPTURE=1"));
    assert(has(intelVulkan.entries,"LIBGL_DRIVERS_PATH=/other/dri"));
    assert(!has(virtioWithIntel.entries,"VK_DRIVER_FILES=/system/intel.json"));
    assert(!has(virtioWithIntel.entries,"BROOK_I915_PARTIAL_CAPTURE=1"));
    assert(DesktopTrustedGpuPath(0,0755,true,false,true));
    assert(DesktopTrustedGpuPath(0,0644,false,false,false));
    assert(!DesktopTrustedGpuPath(1000,0644,false,false,false));
    assert(!DesktopTrustedGpuPath(0,0664,false,false,false));
    assert(!DesktopTrustedGpuPath(0,0755,false,true,false));
    assert(!DesktopTrustedGpuPath(0,0755,false,false,true));
    assert(has(virtioWithAlternate.entries,"LIBGL_DRIVERS_PATH=/mesa/dri"));
}
