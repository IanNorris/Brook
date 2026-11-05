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
        "VK_DRIVER_FILES=/mesa/virtio.json","LIBGL_DRIVERS_PATH=/mesa/dri",nullptr};
    constexpr auto virtio=DesktopBuildGpuEnvironment(base,true,true);
    constexpr auto staged=DesktopBuildGpuEnvironment(base,true,false);
    constexpr auto other=DesktopBuildGpuEnvironment(base,false,false);
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
}
