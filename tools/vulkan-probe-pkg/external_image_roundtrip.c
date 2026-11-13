/* Intel-only offscreen test: import a Brook GEM dma-buf as a linear Vulkan
 * color image, clear it on the GPU and independently read back its pixels. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "gpu_output_abi.h"
#include "graphics_layers.h"
#include "graphics_image.h"
#include "graphics_pass.h"
#include "triangle_spv.h"

#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
struct gem_create { uint64_t size; uint32_t handle, pad; };
struct map_offset { uint32_t handle, pad; uint64_t offset, flags, extensions; };
struct prime { uint32_t handle, flags; int32_t fd; };
struct gem_domain { uint32_t handle, read, write; };
struct gem_close { uint32_t handle, pad; };
struct intel_context {uint32_t id,pad;};
struct intel_object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct intel_exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct intel_sync {uint32_t handle,flags;};
struct intel_exec_fence {uint32_t handle,flags;};
struct intel_sync_handle {uint32_t handle,flags;int32_t fd;uint32_t pad;};
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"EXTERNAL_IMAGE_FAIL line=%d errno=%d\n",__LINE__,errno); goto cleanup; } } while (0)
#define VKCHECK(x) do { VkResult r_=(x); if (r_!=VK_SUCCESS) { fprintf(stderr,"EXTERNAL_IMAGE_VK_FAIL line=%d result=%d\n",__LINE__,r_); goto cleanup; } } while (0)
#include "sample_background.h"
#include "external_sample.h"
#include "protected_graphics_pass.h"
#include "protected_image_import.h"

int main(int argc,char **argv)
{
    int large=0,alphaAudit=0,protectedImport=0,protectedWrite=0,clearTiming=0,noClear=0;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--large") && !large)large=1;
        else if(!strcmp(argv[i],"--alpha-audit") && !alphaAudit)alphaAudit=1;
        else if(!strcmp(argv[i],"--alpha-exact") && !alphaAudit)alphaAudit=2;
        else if(!strcmp(argv[i],"--alpha-layers") && !alphaAudit)alphaAudit=3;
        else if(!strcmp(argv[i],"--clear-timing") && !clearTiming)clearTiming=1;
        else if(!strcmp(argv[i],"--no-clear") && !noClear)noClear=1;
        else if(!strcmp(argv[i],"--frame-trace") && !traceFirstSample)traceFirstSample=1;
        else if(!strcmp(argv[i],"--geometry") && !sampleGeometry)sampleGeometry=1;
        else if(!strcmp(argv[i],"--graphics-pass") && !sampleGraphicsPass)sampleGraphicsPass=1;
        else if(!strcmp(argv[i],"--graphics-fence") && !sampleGraphicsFence)sampleGraphicsFence=1;
        else if(!strcmp(argv[i],"--graphics-multilayer") && !sampleGraphicsMultilayer)sampleGraphicsMultilayer=1;
        else if(!strcmp(argv[i],"--graphics-regions") && !sampleGraphicsRegions)sampleGraphicsRegions=1;
        else if(!strcmp(argv[i],"--protected-import") && !protectedImport)protectedImport=1;
        else if(!strcmp(argv[i],"--protected-write") && !protectedWrite)protectedWrite=1;
        else {fprintf(stderr,"usage: %s [--large] [--alpha-audit|--alpha-exact|--alpha-layers] [--clear-timing|--no-clear] [--frame-trace] [--geometry] [--graphics-pass] [--protected-import] [--protected-write]\n",argv[0]);return 2;}
    }
    if(clearTiming && (alphaAudit || protectedImport || protectedWrite)){fprintf(stderr,"--clear-timing cannot be combined with alpha or protected modes\n");return 2;}
    if(noClear && (clearTiming || alphaAudit==1 || protectedImport || protectedWrite)){fprintf(stderr,"--no-clear requires full output coverage\n");return 2;}
    if(sampleGeometry && alphaAudit!=3){fprintf(stderr,"--geometry requires --alpha-layers\n");return 2;}
    if(sampleGraphicsPass && (alphaAudit!=3 || !noClear || protectedImport || protectedWrite)){fprintf(stderr,"--graphics-pass requires --alpha-layers --no-clear\n");return 2;}
    if(sampleGraphicsFence && (!sampleGraphicsPass || sampleGeometry)){fprintf(stderr,"--graphics-fence requires --graphics-pass and no geometry override\n");return 2;}
    if(sampleGraphicsMultilayer && (!sampleGraphicsPass || sampleGraphicsFence)){fprintf(stderr,"--graphics-multilayer requires --graphics-pass and no fence override\n");return 2;}
    if(sampleGraphicsRegions && (!sampleGraphicsPass || sampleGraphicsFence)){fprintf(stderr,"--graphics-regions requires --graphics-pass and no fence override\n");return 2;}
    const unsigned Width=large?2560:64,Height=large?1440:64;
    int rc=1, drm=-1, fd=-1;
    void *mapped=MAP_FAILED;
    size_t mapSize=0;
    struct gem_create gem={0};
    VkInstance instance=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VKCHECK(vkCreateInstance(&ci,NULL,&instance));
    uint32_t count=0;
    VKCHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));
    CHECK(count>0 && count<=16);
    VkPhysicalDevice devices[16], physical=VK_NULL_HANDLE;
    VKCHECK(vkEnumeratePhysicalDevices(instance,&count,devices));
    for(uint32_t i=0;i<count;++i){
        VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(devices[i],&p);
        if(p.vendorID==0x8086 && p.deviceID==0x1616){physical=devices[i];break;}
    }
    CHECK(physical!=VK_NULL_HANDLE);
    uint32_t queues=0, family=UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&queues,NULL);
    CHECK(queues>0 && queues<=32);
    VkQueueFamilyProperties qp[32];vkGetPhysicalDeviceQueueFamilyProperties(physical,&queues,qp);
    for(uint32_t i=0;i<queues;++i)if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    CHECK(family!=UINT32_MAX);
    sampleGraphicsFamily=family;
    float priority=1.0f;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *extensions[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .enabledExtensionCount=3,.ppEnabledExtensionNames=extensions};
    VKCHECK(vkCreateDevice(physical,&di,NULL,&device));
    VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    if(protectedImport || protectedWrite)CHECK(protected_image_import(physical,device,queue,family,protectedWrite));
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={Width,Height,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(device,&ii,NULL,&image));
    VkMemoryRequirements needs;vkGetImageMemoryRequirements(device,image,&needs);
    VkImageSubresource sub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device,image,&sub,&layout);
    CHECK(needs.size>0 && needs.size<=64*1024*1024 && layout.rowPitch>=Width*4 &&
          layout.offset+(Height-1)*layout.rowPitch+Width*4<=needs.size);
    drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    gem.size=needs.size;CHECK(!ioctl(drm,IOC(0x5b,sizeof(gem)),&gem));
    mapSize=(size_t)gem.size;
    struct map_offset mo={.handle=gem.handle,.flags=2};CHECK(!ioctl(drm,IOC(0x64,sizeof(mo)),&mo));
    mapped=mmap(NULL,mapSize,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)mo.offset);CHECK(mapped!=MAP_FAILED);
    struct prime prime={.handle=gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
    CHECK(!ioctl(drm,IOC(0x2d,sizeof(prime)),&prime));fd=prime.fd;
    PFN_vkGetMemoryFdPropertiesKHR getFd=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR");
    CHECK(getFd!=NULL);
    VkMemoryFdPropertiesKHR fp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VKCHECK(getFd(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fd,&fp));
    uint32_t allowed=needs.memoryTypeBits&fp.memoryTypeBits,index=UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    for(uint32_t i=0;i<mp.memoryTypeCount;++i)if(allowed&(1u<<i)){index=i;break;}
    CHECK(index!=UINT32_MAX);
    VkImportMemoryFdInfoKHR import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=fd};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,
        .allocationSize=needs.size,.memoryTypeIndex=index};
    VkResult ar=vkAllocateMemory(device,&ai,NULL,&memory);
    if(ar==VK_SUCCESS)fd=-1; /* Vulkan owns a successfully imported fd. */
    CHECK(ar==VK_SUCCESS);
    VKCHECK(vkBindImageMemory(device,image,memory,0));
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VKCHECK(vkCreateCommandPool(device,&pi,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;VKCHECK(vkAllocateCommandBuffers(device,&ca,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKCHECK(vkBeginCommandBuffer(command,&begin));
    VkImageSubresourceRange range={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1};
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    const VkClearColorValue color={.float32={1.0f,0.0f,0.0f,1.0f}};
    vkCmdClearColorImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
    VKCHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VKCHECK(vkCreateFence(device,&fi,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    VKCHECK(vkQueueSubmit(queue,1,&submit,fence));
    VkResult waited=vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull);
    if(waited!=VK_SUCCESS){
        /* Completion is uncertain: retain the imported buffer and command
         * resources rather than allowing their backing to be reused. */
        fprintf(stderr,"EXTERNAL_IMAGE_COMPLETION_UNCERTAIN result=%d\n",waited);
        for(;;)pause();
    }
    struct gem_domain domain={.handle=gem.handle,.read=1,.write=0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(domain)),&domain));
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
        uint32_t pixel;memcpy(&pixel,(const uint8_t*)mapped+layout.offset+y*layout.rowPitch+x*4,4);
        if(pixel!=0xffff0000u){fprintf(stderr,"EXTERNAL_IMAGE_PIXEL_FAIL x=%u y=%u pixel=%08x\n",x,y,pixel);goto cleanup;}
    }
    printf("EXTERNAL_IMAGE_CLEAR_PASS size=%llu row_pitch=%llu pixels=%u\n",
           (unsigned long long)needs.size,(unsigned long long)layout.rowPitch,Width*Height);
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,.subresourceRange=range};
    VKCHECK(vkCreateImageView(device,&vi,NULL,&view));
    VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&ref};
    VkSubpassDependency dependency={.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,
        .srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
        .subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dependency};
    VKCHECK(vkCreateRenderPass(device,&rp,NULL,&pass));
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=1,.pAttachments=&view,.width=Width,.height=Height,.layers=1};
    VKCHECK(vkCreateFramebuffer(device,&fb,NULL,&framebuffer));
    VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(vertexCode),.pCode=vertexCode};
    VKCHECK(vkCreateShaderModule(device,&si,NULL,&vertex));
    si.codeSize=sizeof(fragmentCode);si.pCode=fragmentCode;VKCHECK(vkCreateShaderModule(device,&si,NULL,&fragment));
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VKCHECK(vkCreatePipelineLayout(device,&pli,NULL,&pipelineLayout));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,Width,Height,0,1};VkRect2D scissor={{0,0},{Width,Height}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vin,.pInputAssemblyState=&assembly,.pViewportState=&vp,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pipelineLayout,.renderPass=pass};
    VKCHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,NULL,&pipeline));
    VKCHECK(vkResetCommandBuffer(command,0));VKCHECK(vkResetFences(device,1,&fence));
    VKCHECK(vkBeginCommandBuffer(command,&begin));
    VkClearValue clear={.color={{0,0,0,1}}};
    VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,
        .renderArea={{0,0},{Width,Height}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(command,&rbi,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(command,3,1,0,0);
    vkCmdEndRenderPass(command);VKCHECK(vkEndCommandBuffer(command));
    VKCHECK(vkQueueSubmit(queue,1,&submit,fence));
    waited=vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull);
    if(waited!=VK_SUCCESS){fprintf(stderr,"EXTERNAL_IMAGE_COMPLETION_UNCERTAIN result=%d\n",waited);for(;;)pause();}
    CHECK(!ioctl(drm,IOC(0x5f,sizeof(domain)),&domain));
    const unsigned positions[][3]={{Width/2,Height/2,1},{Width/2,Height*5/8,1},{2,2,0},{Width-3,2,0},{2,Height-3,0},{Width-3,Height-3,0},{Width*10/64,Height*50/64,0}};
    for(unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);++i){
        const unsigned *p=positions[i];uint32_t pixel;
        memcpy(&pixel,(const uint8_t*)mapped+layout.offset+p[1]*layout.rowPitch+p[0]*4,4);
        if(pixel!=(p[2]?0xff00ff00u:0xff000000u)){
            fprintf(stderr,"EXTERNAL_IMAGE_TRIANGLE_FAIL x=%u y=%u pixel=%08x\n",p[0],p[1],pixel);goto cleanup;
        }
    }
    puts("EXTERNAL_IMAGE_TRIANGLE_PASS samples=7");
    if(sampleGraphicsPass){
        struct prime outputPrime={.handle=gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
        CHECK(!ioctl(drm,IOC(0x2d,sizeof(outputPrime)),&outputPrime));
        sampleGraphicsOutput=(struct BrookVkGraphicsBuffer){outputPrime.fd,-1,Width,Height,
            (uint32_t)(layout.offset/4u),(uint32_t)(layout.rowPitch/4u),gem.size};
        sampleGraphicsDrm=drm;sampleGraphicsOutputHandle=gem.handle;sampleGraphicsMap=mapped;
    }
    if(noClear){
        vkDestroyFramebuffer(device,framebuffer,NULL);framebuffer=VK_NULL_HANDLE;
        vkDestroyRenderPass(device,pass,NULL);pass=VK_NULL_HANDLE;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VKCHECK(vkCreateRenderPass(device,&rp,NULL,&pass));
        fb.renderPass=pass;VKCHECK(vkCreateFramebuffer(device,&fb,NULL,&framebuffer));
        puts("EXTERNAL_IMAGE_FULL_COVERAGE_NO_CLEAR");
    }
    for(unsigned test=clearTiming?1u:0u;test<(clearTiming?2u:alphaAudit==3?7u:alphaAudit?6u:5u);++test){
        const unsigned scale=test?2:1;
        const int gpuSource=test==2?1:test==3?2:0;
        const int xrgbSource=test==4;
        const int blendSource=test==5?alphaAudit:test==6?4:0;
        CHECK(sample_imported(physical,device,queue,pass,framebuffer,pool,command,fence,
                              Width/scale,Height/scale,Width,Height,gpuSource,blendSource,xrgbSource,clearTiming,
                              VK_NULL_HANDLE,VK_NULL_HANDLE,-1,NULL));
        CHECK(!ioctl(drm,IOC(0x5f,sizeof(domain)),&domain));
        uint64_t mismatches=0,checked=0;unsigned maxDelta=0;
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
            if(sampleGraphicsRegions && blendSource>=3 &&
               !((x>=2 && x<2+Width/3 && y>=3 && y<3+Height/3) ||
                 (x>=Width/2 && x<Width/2+Width/3 && y>=Height/2 && y<Height/2+Height/3)))continue;
            ++checked;
            uint32_t pixel;memcpy(&pixel,(const uint8_t*)mapped+layout.offset+y*layout.rowPitch+x*4,4);
            uint32_t sx=x/scale,sy=y/scale;
            uint32_t expected=gpuSource==1?0xff336699u:
                (0xff000000u|((sx&255u)<<16)|((sy&255u)<<8)|((sx^sy)&255u));
            if(clearTiming)expected=0xff000000u;
            if(gpuSource==2 && sx==0 && sy==0)expected=0xff13579bu;
            if(blendSource){
                const uint32_t br=blendSource>=3?(x*3u+17u)&255u:32u;
                const uint32_t bg=blendSource>=3?(y*5u+23u)&255u:64u;
                const uint32_t bb=blendSource>=3?(x*7u+y*11u)&255u:96u;
                const int covered=!(sampleGeometry && blendSource>=3) ||
                    (x>=4 && x<Width-5 && y>=7 && y<Height-7);
                if(sampleGeometry && blendSource>=3 && covered){
                    sx=2u+(uint32_t)(((uint64_t)(2u*(x+3u)+1u)*(Width/2u-4u))/(2u*(Width-2u)));
                    sy=3u+(uint32_t)(((uint64_t)(2u*(y-7u)+1u)*(Height/2u-6u))/(2u*(Height-14u)));
                }
                const uint32_t sourceAlpha=(sx*17u+sy*13u)&255u;
                const int producerPixel=sampleGraphicsFence && blendSource==3 && sx==0 && sy==0;
                const uint32_t effectiveAlpha=producerPixel?255u:sourceAlpha;
                const uint32_t alpha=blendSource>=3?(effectiveAlpha*173u+127u)/255u:sourceAlpha,inv=255u-alpha;
                uint32_t r,g,b;
                if(!covered){r=br;g=bg;b=bb;}
                else if(blendSource==4){
                    const uint32_t sr=((sx&255u)*sourceAlpha+127u)/255u;
                    const uint32_t sg=((sy&255u)*sourceAlpha+127u)/255u;
                    const uint32_t sb=(((sx^sy)&255u)*sourceAlpha+127u)/255u;
                    r=(sr*173u+127u)/255u+(br*inv+127u)/255u;
                    g=(sg*173u+127u)/255u+(bg*inv+127u)/255u;
                    b=(sb*173u+127u)/255u+(bb*inv+127u)/255u;
                    if(r>255u)r=255u;
                    if(g>255u)g=255u;
                    if(b>255u)b=255u;
                }else{
                    r=((producerPixel?19u:sx&255u)*alpha+br*inv+127u)/255u;
                    g=((producerPixel?87u:sy&255u)*alpha+bg*inv+127u)/255u;
                    b=((producerPixel?155u:(sx^sy)&255u)*alpha+bb*inv+127u)/255u;
                }
                expected=0xff000000u|(r<<16)|(g<<8)|b;
            }
            if(sampleGraphicsMultilayer && blendSource>=3 && x>=8 && x<Width-8 && y>=8 && y<Height-8){
                const uint32_t sr=(x*3u+17u)&255u,sg=(y*5u+23u)&255u,sb=(x*7u+y*11u)&255u;
                const uint32_t r=(sr*83u+((expected>>16)&255u)*172u+127u)/255u;
                const uint32_t g=(sg*83u+((expected>>8)&255u)*172u+127u)/255u;
                const uint32_t b=(sb*83u+(expected&255u)*172u+127u)/255u;
                expected=0xff000000u|(r<<16)|(g<<8)|b;
            }
            if(pixel!=expected){
                if(!blendSource){fprintf(stderr,"EXTERNAL_IMAGE_SAMPLE_FAIL scale=%u x=%u y=%u pixel=%08x expected=%08x\n",scale,x,y,pixel,expected);goto cleanup;}
                ++mismatches;
                for(unsigned shift=0;shift<32;shift+=8){
                    unsigned got=(pixel>>shift)&255u,want=(expected>>shift)&255u;
                    unsigned delta=got>want?got-want:want-got;if(delta>maxDelta)maxDelta=delta;
                }
            }
        }
        if(blendSource){
            fprintf(stderr,"EXTERNAL_IMAGE_ALPHA_EXACT mismatched_pixels=%llu max_channel_delta=%u\n",
                    (unsigned long long)mismatches,maxDelta);
            if(mismatches)goto cleanup;
        }
        printf("EXTERNAL_IMAGE_SAMPLE_PASS source=%s scale=%u width=%u height=%u pixels=%llu\n",
               clearTiming?"clear-only":gpuSource==1?"gpu-semaphore":gpuSource==2?"sync-file":blendSource?"cpu-alpha":xrgbSource?"cpu-xrgb":"cpu",
               scale,Width,Height,(unsigned long long)checked);
    }
    rc=0;
cleanup:
    if(sampleGraphicsOutput.fd>=0)close(sampleGraphicsOutput.fd);
    if(device && pipeline)vkDestroyPipeline(device,pipeline,NULL);
    if(device && pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,NULL);
    if(device && vertex)vkDestroyShaderModule(device,vertex,NULL);
    if(device && fragment)vkDestroyShaderModule(device,fragment,NULL);
    if(device && framebuffer)vkDestroyFramebuffer(device,framebuffer,NULL);
    if(device && pass)vkDestroyRenderPass(device,pass,NULL);
    if(device && view)vkDestroyImageView(device,view,NULL);
    if(device && fence)vkDestroyFence(device,fence,NULL);
    if(device && pool)vkDestroyCommandPool(device,pool,NULL);
    if(device && image)vkDestroyImage(device,image,NULL);
    if(device && memory)vkFreeMemory(device,memory,NULL);
    if(fd>=0)close(fd);
    if(mapped!=MAP_FAILED)munmap(mapped,mapSize);
    if(drm>=0 && gem.handle){struct gem_close c={gem.handle,0};(void)ioctl(drm,IOC(9,sizeof(c)),&c);}
    if(drm>=0)close(drm);
    if(device)vkDestroyDevice(device,NULL);
    if(instance)vkDestroyInstance(instance,NULL);
    return rc;
}
