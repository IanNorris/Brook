// Minimal X11 swapchain + GPU clear + X11 readback. No game, shaders or toolkit.
#define _POSIX_C_SOURCE 200809L
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define VK(call) do {VkResult r=(call);if(r!=VK_SUCCESS){fprintf(stderr,"WSI_FAIL stage=%s result=%d\n",#call,r);goto cleanup;}}while(0)
#define CK(value) do {if(!(value)){fprintf(stderr,"WSI_FAIL stage=%s\n",#value);goto cleanup;}}while(0)

int main(void) {
    int code=1;Display* display=NULL;Window window=0;
    VkInstance instance=VK_NULL_HANDLE;VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;VkSemaphore acquired=VK_NULL_HANDLE;
    VkSemaphore* rendered=NULL;uint32_t renderSemaphoreCount=0;
    VkFence fence=VK_NULL_HANDLE;VkImage* images=NULL;
    display=XOpenDisplay(NULL);CK(display);
    window=XCreateSimpleWindow(display,DefaultRootWindow(display),80,80,320,240,0,0,0);CK(window);
    XStoreName(display,window,"Brook Vulkan X11 WSI probe");XMapWindow(display,window);XSync(display,False);
    const char* extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Brook WSI probe",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,
        .enabledExtensionCount=2,.ppEnabledExtensionNames=extensions};
    VK(vkCreateInstance(&ici,NULL,&instance));
    VkXlibSurfaceCreateInfoKHR sci={.sType=VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,.dpy=display,.window=window};
    VK(vkCreateXlibSurfaceKHR(instance,&sci,NULL,&surface));
    uint32_t count=0;VK(vkEnumeratePhysicalDevices(instance,&count,NULL));CK(count);
    VkPhysicalDevice* physicals=calloc(count,sizeof(*physicals));CK(physicals);
    VkResult enumerated=vkEnumeratePhysicalDevices(instance,&count,physicals);
    VkPhysicalDevice physical=count?physicals[0]:VK_NULL_HANDLE;free(physicals);CK(enumerated==VK_SUCCESS && physical);
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
    printf("WSI_DEVICE %s\n",properties.deviceName);fflush(stdout);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,NULL);CK(families);
    VkQueueFamilyProperties* queues=calloc(families,sizeof(*queues));CK(queues);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues);
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families;++i) {
        VkBool32 present=VK_FALSE;
        VkResult r=vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&present);
        if(r==VK_SUCCESS && present && (queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}
    }
    free(queues);CK(family!=UINT32_MAX);
    const float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,
        .queueCount=1,.pQueuePriorities=&priority};
    const char* swapExtension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=&swapExtension};
    VK(vkCreateDevice(physical,&dci,NULL,&device));
    VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkSurfaceCapabilitiesKHR caps;VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,NULL));CK(count);
    VkSurfaceFormatKHR* formats=calloc(count,sizeof(*formats));CK(formats);
    VkResult formatResult=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats);
    if(formatResult!=VK_SUCCESS || !count){free(formats);goto cleanup;}
    VkSurfaceFormatKHR format=formats[0];
    for(uint32_t i=0;i<count;++i)if(formats[i].format==VK_FORMAT_B8G8R8A8_UNORM){format=formats[i];break;}
    free(formats);CK(formatResult==VK_SUCCESS);
    VkExtent2D extent=caps.currentExtent;
    if(extent.width==UINT32_MAX){extent.width=320;extent.height=240;}
    CK(extent.width && extent.height && (caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    uint32_t imageCount=caps.minImageCount+1;
    if(caps.maxImageCount && imageCount>caps.maxImageCount)imageCount=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(caps.supportedCompositeAlpha&alpha))alpha=(VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
    VkSwapchainCreateInfoKHR swap={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surface,
        .minImageCount=imageCount,.imageFormat=format.format,.imageColorSpace=format.colorSpace,.imageExtent=extent,
        .imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
        .preTransform=caps.currentTransform,.compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    VK(vkCreateSwapchainKHR(device,&swap,NULL,&swapchain));
    VK(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,NULL));CK(imageCount);
    images=calloc(imageCount,sizeof(*images));CK(images);VK(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,images));
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    VK(vkCreateCommandPool(device,&pci,NULL,&pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;VK(vkAllocateCommandBuffers(device,&cai,&command));
    VkSemaphoreCreateInfo sem={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK(vkCreateSemaphore(device,&sem,NULL,&acquired));
    // A queue-idle wait does not retire a presentation wait semaphore. Reuse
    // one only after the corresponding swapchain image has been reacquired.
    rendered=calloc(imageCount,sizeof(*rendered));CK(rendered);
    for(uint32_t i=0;i<imageCount;++i){VK(vkCreateSemaphore(device,&sem,NULL,&rendered[i]));++renderSemaphoreCount;}
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK(vkCreateFence(device,&fi,NULL,&fence));
    const uint32_t expected[]={0xff0000,0x00ff00,0x0000ff,0xffffff};
    for(unsigned frame=0;frame<4;++frame) {
        uint32_t image;VK(vkAcquireNextImageKHR(device,swapchain,2000000000ULL,acquired,VK_NULL_HANDLE,&image));
        CK(image<imageCount);VK(vkResetCommandBuffer(command,0));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VK(vkBeginCommandBuffer(command,&begin));
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=images[image],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearColorValue color={.float32={(expected[frame]&0xff0000)?1:0,(expected[frame]&0xff00)?1:0,(expected[frame]&0xff)?1:0,1}};
        vkCmdClearColorImage(command,images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
        VK(vkEndCommandBuffer(command));
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acquired,
            .pWaitDstStageMask=&waitStage,.commandBufferCount=1,.pCommandBuffers=&command,.signalSemaphoreCount=1,.pSignalSemaphores=&rendered[image]};
        VK(vkQueueSubmit(queue,1,&submit,fence));
        VkPresentInfoKHR present={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&rendered[image],
            .swapchainCount=1,.pSwapchains=&swapchain,.pImageIndices=&image};
        VK(vkQueuePresentKHR(queue,&present));
        VK(vkWaitForFences(device,1,&fence,VK_TRUE,2000000000ULL));VK(vkQueueWaitIdle(queue));
        uint32_t pixel=0;unsigned attempt;
        for(attempt=0;attempt<100;++attempt) {
            XImage* readback=XGetImage(display,window,(int)extent.width/2,(int)extent.height/2,1,1,AllPlanes,ZPixmap);
            CK(readback);pixel=(uint32_t)XGetPixel(readback,0,0)&0xffffff;XDestroyImage(readback);
            if(pixel==expected[frame])break;
            struct timespec pause={0,10000000};nanosleep(&pause,NULL);
        }
        printf("WSI_FRAME frame=%u expected=%06x actual=%06x polls=%u\n",frame,expected[frame],pixel,attempt);fflush(stdout);
        CK(pixel==expected[frame]);VK(vkResetFences(device,1,&fence));
    }
    puts("WSI_PASS");code=0;
cleanup:
    if(device){
        vkDeviceWaitIdle(device);
        if(fence)vkDestroyFence(device,fence,NULL);
        if(acquired)vkDestroySemaphore(device,acquired,NULL);
        for(uint32_t i=0;i<renderSemaphoreCount;++i)vkDestroySemaphore(device,rendered[i],NULL);
        if(pool)vkDestroyCommandPool(device,pool,NULL);
        if(swapchain)vkDestroySwapchainKHR(device,swapchain,NULL);
        vkDestroyDevice(device,NULL);
    }
    free(rendered);
    free(images);
    if(surface)vkDestroySurfaceKHR(instance,surface,NULL);
    if(instance)vkDestroyInstance(instance,NULL);
    if(window)XDestroyWindow(display,window);
    if(display)XCloseDisplay(display);
    return code;
}
