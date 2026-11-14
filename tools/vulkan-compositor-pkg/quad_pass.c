#define _POSIX_C_SOURCE 200809L
#include "quad_pass.h"
#include "quad_pass_spv.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* An entry unused for QUAD_CACHE_MAX_AGE submissions (about two seconds of
 * frames) is dropped, so a replaced buffer is not pinned for long while the
 * wallpaper survives long runs of game-only frames that never sample it. */
enum { QUAD_CACHE_SLOTS=48, QUAD_CACHE_MAX_AGE=120, QUAD_FRAME_SLOTS=BROOK_VK_MAX_LAYERS+1 };
enum { QUAD_MODE_STRAIGHT=1, QUAD_MODE_OPAQUE=2, QUAD_MODE_OVER_ZERO=4, QUAD_MODE_SAMPLER=8, QUAD_MODE_DIVIDE=16, QUAD_MODE_READ=32 };
enum { QUAD_PIPELINE_STRAIGHT=0, QUAD_PIPELINE_PREMULTIPLIED=1, QUAD_PIPELINE_NONE=2, QUAD_PIPELINES=3 };
struct QuadPush {int32_t rect[4];float invHalf[2];int32_t dst[2];uint32_t dstSize[2],src[2],srcSize[2];float scale[2];uint32_t opacity,mode;};
_Static_assert(sizeof(struct QuadPush)==72,"quad push constant layout");

/* One imported linear BGRA8 image. live: slot holds an import. set: the sampled
 * source descriptor, or for an output its input-attachment descriptor. */
struct QuadImage {
    dev_t dev;ino_t ino;uint64_t bytes,lastUse;
    uint32_t width,height,offsetWords,strideWords;VkImageUsageFlags usage;
    int pin,live;
    VkImage image;VkDeviceMemory memory;VkImageView view;VkDescriptorSet set;VkFramebuffer framebuffer;
};
struct BrookVkQuadPass {
    VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
    VkDescriptorSetLayout setLayout,inputLayout;VkDescriptorPool pool;int fixedBlend;VkPipelineLayout layout;VkRenderPass renderPass;
    VkShaderModule vertex,fragment;VkSampler sampler;VkPipeline pipeline[QUAD_PIPELINES];
    VkCommandPool commandPool;VkCommandBuffer command;VkFence fence;
    VkQueryPool timestamps;uint32_t timestampBits;float timestampPeriod;uint64_t gpuNs;int gpuValid;
    PFN_vkGetMemoryFdPropertiesKHR getFdProperties;PFN_vkImportSemaphoreFdKHR importSemaphore;
    struct QuadImage cache[QUAD_CACHE_SLOTS],frame[QUAD_FRAME_SLOTS];unsigned frameCount;
    VkSemaphore wait[BROOK_VK_MAX_LAYERS];unsigned waitCount;
    uint64_t clock;int pending,uncertain;
    struct BrookVkQuadStats stats;
};

static void image_destroy(BrookVkQuadPass *p,struct QuadImage *m){
    if(m->framebuffer)vkDestroyFramebuffer(p->device,m->framebuffer,NULL);
    if(m->set)vkFreeDescriptorSets(p->device,p->pool,1,&m->set);
    if(m->view)vkDestroyImageView(p->device,m->view,NULL);
    if(m->image)vkDestroyImage(p->device,m->image,NULL);
    if(m->memory)vkFreeMemory(p->device,m->memory,NULL);
    if(m->pin>=0)close(m->pin);
    *m=(struct QuadImage){.pin=-1};
}
static void frame_cleanup(BrookVkQuadPass *p){
    for(unsigned i=0;i<p->frameCount;++i)image_destroy(p,&p->frame[i]);
    p->frameCount=0;
    for(unsigned i=0;i<p->waitCount;++i)if(p->wait[i])vkDestroySemaphore(p->device,p->wait[i],NULL);
    p->waitCount=0;
}

int brook_vk_quad_pass_destroy(BrookVkQuadPass *p){
    if(!p)return 1;
    if(p->pending || p->uncertain)return 0;
    frame_cleanup(p);
    for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)image_destroy(p,&p->cache[i]);
    if(p->fence)vkDestroyFence(p->device,p->fence,NULL);
    if(p->timestamps)vkDestroyQueryPool(p->device,p->timestamps,NULL);
    if(p->commandPool)vkDestroyCommandPool(p->device,p->commandPool,NULL);
    for(unsigned i=0;i<QUAD_PIPELINES;++i)if(p->pipeline[i])vkDestroyPipeline(p->device,p->pipeline[i],NULL);
    if(p->vertex)vkDestroyShaderModule(p->device,p->vertex,NULL);
    if(p->fragment)vkDestroyShaderModule(p->device,p->fragment,NULL);
    if(p->layout)vkDestroyPipelineLayout(p->device,p->layout,NULL);
    if(p->pool)vkDestroyDescriptorPool(p->device,p->pool,NULL);
    if(p->setLayout)vkDestroyDescriptorSetLayout(p->device,p->setLayout,NULL);
    if(p->inputLayout)vkDestroyDescriptorSetLayout(p->device,p->inputLayout,NULL);
    if(p->sampler)vkDestroySampler(p->device,p->sampler,NULL);
    if(p->renderPass)vkDestroyRenderPass(p->device,p->renderPass,NULL);
    free(p);return 1;
}

BrookVkQuadPass *brook_vk_quad_pass_create(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family){
    if(!physical || !device || !queue)return NULL;
    VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_B8G8R8A8_UNORM,&format);
    const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    if((format.linearTilingFeatures&needed)!=needed)return NULL;
    BrookVkQuadPass *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->physical=physical;p->device=device;p->queue=queue;p->family=family;
    for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)p->cache[i].pin=-1;
    for(unsigned i=0;i<QUAD_FRAME_SLOTS;++i)p->frame[i].pin=-1;
#define OK(x) do { if((x)!=VK_SUCCESS)goto fail; } while(0)
    p->getFdProperties=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR");
    p->importSemaphore=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(device,"vkImportSemaphoreFdKHR");
    if(!p->getFdProperties)goto fail;
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
    if(props.limits.maxPushConstantsSize<sizeof(struct QuadPush))goto fail;
    if(getenv("BROOK_RENDER_GPU_TIMESTAMPS")){
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,NULL);
        if(!n || n>64 || family>=n)goto fail;
        VkQueueFamilyProperties q[64];vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q);
        p->timestampBits=q[family].timestampValidBits;p->timestampPeriod=props.limits.timestampPeriod;
        if(p->timestampBits && p->timestampBits<=64 && p->timestampPeriod>0){
            VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
            OK(vkCreateQueryPool(device,&query,NULL,&p->timestamps));
        }
    }
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    OK(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->setLayout));
    binding.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    OK(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->inputLayout));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,QUAD_CACHE_SLOTS+QUAD_FRAME_SLOTS},
                                   {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,QUAD_CACHE_SLOTS+QUAD_FRAME_SLOTS}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets=QUAD_CACHE_SLOTS+QUAD_FRAME_SLOTS,.poolSizeCount=2,.pPoolSizes=sizes};
    OK(vkCreateDescriptorPool(device,&dp,NULL,&p->pool));
    p->fixedBlend=getenv("BROOK_RENDER_QUAD_FIXED_BLEND")!=NULL;
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,.offset=0,.size=sizeof(struct QuadPush)};
    const VkDescriptorSetLayout setLayouts[2]={p->setLayout,p->inputLayout};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=2,.pSetLayouts=setLayouts,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push};
    OK(vkCreatePipelineLayout(device,&pl,NULL,&p->layout));
    /* Unnormalized nearest sampling: texel floor(coordinate) on each axis. */
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .unnormalizedCoordinates=VK_TRUE};
    OK(vkCreateSampler(device,&sampler,NULL,&p->sampler));
    VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    /* The output is both the colour attachment and an input attachment, so
     * the shader can blend exactly; a by-region self-dependency orders a
     * layer's reads after the previous layers' writes. */
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.inputAttachmentCount=1,.pInputAttachments=&ref,
        .colorAttachmentCount=1,.pColorAttachments=&ref};
    VkSubpassDependency deps[3]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_INPUT_ATTACHMENT_READ_BIT},
        {.srcSubpass=0,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT}};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
        .subpassCount=1,.pSubpasses=&sub,.dependencyCount=3,.pDependencies=deps};
    OK(vkCreateRenderPass(device,&rp,NULL,&p->renderPass));
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(quadVertexCode),.pCode=quadVertexCode};
    OK(vkCreateShaderModule(device,&sm,NULL,&p->vertex));
    sm.codeSize=sizeof(quadFragmentCode);sm.pCode=quadFragmentCode;
    OK(vkCreateShaderModule(device,&sm,NULL,&p->fragment));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=p->vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=p->fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    /* composite.comp's blends: straight colour is s*a+d*(1-a), alpha and
     * premultiplied colour are s+d*(1-a); UNORM rounding matches its
     * (x+127)/255 for the integer-valued inputs the shader produces. */
    const VkPipelineColorBlendAttachmentState blends[QUAD_PIPELINES]={
        {.blendEnable=VK_TRUE,.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp=VK_BLEND_OP_ADD,.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .alphaBlendOp=VK_BLEND_OP_ADD,.colorWriteMask=15},
        {.blendEnable=VK_TRUE,.srcColorBlendFactor=VK_BLEND_FACTOR_ONE,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp=VK_BLEND_OP_ADD,.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .alphaBlendOp=VK_BLEND_OP_ADD,.colorWriteMask=15},
        {.blendEnable=VK_FALSE,.colorWriteMask=15}};
    VkDynamicState dynamicStates[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamicStates};
    for(unsigned i=0;i<QUAD_PIPELINES;++i){
        VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blends[i]};
        VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
            .pVertexInputState=&vin,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,
            .pMultisampleState=&ms,.pColorBlendState=&cb,.pDynamicState=&dynamic,.layout=p->layout,.renderPass=p->renderPass};
        OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline,NULL,&p->pipeline[i]));
    }
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    OK(vkCreateCommandPool(device,&pool,NULL,&p->commandPool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=p->commandPool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    OK(vkAllocateCommandBuffers(device,&ca,&p->command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};OK(vkCreateFence(device,&fi,NULL,&p->fence));
#undef OK
    return p;
fail:
    brook_vk_quad_pass_destroy(p);return NULL;
}

/* Import a linear BGRA8 DMA-BUF. The image is created at the buffer's
 * width, or, if the driver's natural row pitch differs, at the stride width
 * (the shader never samples outside the used crop). The data offset becomes
 * the memory binding offset. */
static int image_create(BrookVkQuadPass *p,int fd,const struct BrookVkGraphicsBuffer *b,uint32_t width,
                        VkImageUsageFlags usage,struct QuadImage *m){
    VkPhysicalDeviceExternalImageFormatInfo externalFormat={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 format={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&externalFormat,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_LINEAR,.usage=usage};
    VkExternalImageFormatProperties externalProperties={.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&externalProperties};
    if(vkGetPhysicalDeviceImageFormatProperties2(p->physical,&format,&properties)!=VK_SUCCESS ||
       !(externalProperties.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
       width>properties.imageFormatProperties.maxExtent.width || b->height>properties.imageFormatProperties.maxExtent.height)return 0;
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo create={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={width,b->height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_LINEAR,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    if(vkCreateImage(p->device,&create,NULL,&m->image)!=VK_SUCCESS){m->image=VK_NULL_HANDLE;return 0;}
    VkImageSubresource subresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(p->device,m->image,&subresource,&layout);
    VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dedicated};
    VkImageMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=m->image};
    vkGetImageMemoryRequirements2(p->device,&info,&requirements);
    const uint64_t bind=(uint64_t)b->offsetWords*4u,need=requirements.memoryRequirements.size;
    const uint64_t last=bind+((uint64_t)(b->height-1u)*b->strideWords+b->width)*4u;
    const int dedicatedOnly=dedicated.requiresDedicatedAllocation ||
        (externalProperties.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
    if(layout.offset || layout.rowPitch!=(uint64_t)b->strideWords*4u || !need ||
       bind>b->allocationBytes || need>b->allocationBytes-bind || last>b->allocationBytes ||
       (requirements.memoryRequirements.alignment && bind%requirements.memoryRequirements.alignment) ||
       (dedicatedOnly && bind))return 0;
    int copy=fcntl(fd,F_DUPFD_CLOEXEC,0);if(copy<0)return 0;
    VkMemoryFdPropertiesKHR fdProperties={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if(p->getFdProperties(p->device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,copy,&fdProperties)!=VK_SUCCESS){close(copy);return 0;}
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(p->physical,&memory);
    const uint32_t allowed=fdProperties.memoryTypeBits&requirements.memoryRequirements.memoryTypeBits;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)if(allowed&(1u<<i)){type=i;break;}
    if(type==UINT32_MAX){close(copy);return 0;}
    VkMemoryDedicatedAllocateInfo dedicatedInfo={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=m->image};
    VkImportMemoryFdInfoKHR import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,.pNext=dedicatedOnly?&dedicatedInfo:NULL,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=copy};
    VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,
        .allocationSize=b->allocationBytes,.memoryTypeIndex=type};
    if(vkAllocateMemory(p->device,&allocate,NULL,&m->memory)!=VK_SUCCESS){m->memory=VK_NULL_HANDLE;close(copy);return 0;}
    if(vkBindImageMemory(p->device,m->image,m->memory,bind)!=VK_SUCCESS)return 0;
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=m->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    if(vkCreateImageView(p->device,&view,NULL,&m->view)!=VK_SUCCESS){m->view=VK_NULL_HANDLE;return 0;}
    return 1;
}
static int image_import(BrookVkQuadPass *p,const struct BrookVkGraphicsBuffer *b,const struct stat *id,
                        VkImageUsageFlags usage,int pin,struct QuadImage *m){
    *m=(struct QuadImage){.pin=-1};
    off_t size=lseek(b->fd,0,SEEK_END);
    if(lseek(b->fd,0,SEEK_SET)!=0 || size<0 || (uint64_t)size!=b->allocationBytes)return 0;
    int ok=image_create(p,b->fd,b,b->width,usage,m);
    if(!ok && b->strideWords!=b->width){image_destroy(p,m);ok=image_create(p,b->fd,b,b->strideWords,usage,m);}
    if(ok && (usage&VK_IMAGE_USAGE_SAMPLED_BIT)){
        VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,
            .descriptorSetCount=1,.pSetLayouts=&p->setLayout};
        if(vkAllocateDescriptorSets(p->device,&ds,&m->set)!=VK_SUCCESS){m->set=VK_NULL_HANDLE;ok=0;}
        else {
            VkDescriptorImageInfo image={.sampler=p->sampler,.imageView=m->view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=m->set,.dstBinding=0,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
            vkUpdateDescriptorSets(p->device,1,&write,0,NULL);
        }
    }
    if(ok && (usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)){
        VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,
            .descriptorSetCount=1,.pSetLayouts=&p->inputLayout};
        if(vkAllocateDescriptorSets(p->device,&ds,&m->set)!=VK_SUCCESS){m->set=VK_NULL_HANDLE;ok=0;}
        else {
            VkDescriptorImageInfo image={.imageView=m->view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=m->set,.dstBinding=0,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=&image};
            vkUpdateDescriptorSets(p->device,1,&write,0,NULL);
        }
    }
    if(ok && (usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)){
        VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=p->renderPass,.attachmentCount=1,
            .pAttachments=&m->view,.width=b->width,.height=b->height,.layers=1};
        if(vkCreateFramebuffer(p->device,&fb,NULL,&m->framebuffer)!=VK_SUCCESS){m->framebuffer=VK_NULL_HANDLE;ok=0;}
    }
    if(ok && pin){m->pin=fcntl(b->fd,F_DUPFD_CLOEXEC,0);if(m->pin<0)ok=0;}
    if(!ok){image_destroy(p,m);return 0;}
    m->dev=id->st_dev;m->ino=id->st_ino;m->bytes=b->allocationBytes;m->width=b->width;m->height=b->height;
    m->offsetWords=b->offsetWords;m->strideWords=b->strideWords;m->usage=usage;m->live=1;
    ++p->stats.imports;return 1;
}
static int image_matches(const struct QuadImage *m,const struct BrookVkGraphicsBuffer *b,const struct stat *id,VkImageUsageFlags usage){
    return m->live && m->dev==id->st_dev && m->ino==id->st_ino && m->bytes==b->allocationBytes && m->width==b->width &&
           m->height==b->height && m->offsetWords==b->offsetWords && m->strideWords==b->strideWords && m->usage==usage;
}
/* Find or import; returns NULL on failure. Cached imports outlive the
 * frame; others are destroyed when it retires. */
static struct QuadImage *image_get(BrookVkQuadPass *p,const struct BrookVkQuadSource *s,const struct stat *id,VkImageUsageFlags usage){
    const struct BrookVkGraphicsBuffer *b=&s->buffer;
    for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)if(image_matches(&p->cache[i],b,id,usage)){
        p->cache[i].lastUse=p->clock;++p->stats.cacheHits;return &p->cache[i];}
    for(unsigned i=0;i<p->frameCount;++i)if(image_matches(&p->frame[i],b,id,usage))return &p->frame[i];
    if(s->cache){
        int slot=-1;
        for(unsigned i=0;i<QUAD_CACHE_SLOTS && slot<0;++i)if(!p->cache[i].live)slot=(int)i;
        if(slot<0){ /* Evict the least recently used entry not used by this frame. */
            for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)if(p->cache[i].lastUse!=p->clock &&
                (slot<0 || p->cache[i].lastUse<p->cache[slot].lastUse))slot=(int)i;
            if(slot>=0){image_destroy(p,&p->cache[slot]);++p->stats.evictions;}
        }
        if(slot>=0){
            if(!image_import(p,b,id,usage,1,&p->cache[slot]))return NULL;
            p->cache[slot].lastUse=p->clock;return &p->cache[slot];
        }
    }
    if(p->frameCount==QUAD_FRAME_SLOTS || !image_import(p,b,id,usage,0,&p->frame[p->frameCount]))return NULL;
    return &p->frame[p->frameCount++];
}
static int buffer_valid(const struct BrookVkGraphicsBuffer *b){
    return b->fd>=0 && b->syncFd>=-1 && b->width && b->height && b->width<=BROOK_VK_MAX_DIMENSION &&
           b->height<=BROOK_VK_MAX_DIMENSION && b->strideWords>=b->width && b->strideWords<=BROOK_VK_MAX_DIMENSION &&
           b->allocationBytes && !(b->allocationBytes&3u) && b->allocationBytes/4u<=SIZE_MAX;
}
static int rect_inside(struct QuadRect r,uint32_t width,uint32_t height){
    return r.x0>=0 && r.y0>=0 && r.x0<r.x1 && r.y0<r.y1 && (uint32_t)r.x1<=width && (uint32_t)r.y1<=height;
}
static uint32_t fetch_mode(const uint32_t *l){
    const uint32_t sw=l[6],sh=l[7],dw=l[10],dh=l[11];
    if(sw==dw && sh==dh)return 0;
    const int x=sw==dw || (dw%sw==0 && dw/sw<=64),y=sh==dh || (dh%sh==0 && dh/sh<=64);
    return x && y?QUAD_MODE_SAMPLER:QUAD_MODE_DIVIDE;
}

BrookVkResult brook_vk_quad_pass_submit(BrookVkQuadPass *p,const uint32_t *scene,size_t words,
        const struct BrookVkQuadSource *sources,size_t count,const struct BrookVkQuadSource *output,
        const struct QuadPlan *plan){
    if(!p || p->pending || p->uncertain || !scene || !sources || !output || !plan || !count || count>BROOK_VK_MAX_LAYERS ||
       words<BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS || scene[2]!=count || plan->width!=scene[0] || plan->height!=scene[1] ||
       !buffer_valid(&output->buffer) || output->buffer.syncFd!=-1 || output->buffer.offsetWords ||
       output->buffer.width!=scene[0] || output->buffer.height!=scene[1] || (!plan->drawCount && !plan->clearCount) ||
       plan->drawCount>QUAD_PLAN_MAX_DRAWS || plan->clearCount>QUAD_PLAN_MAX_CLEARS ||
       !rect_inside(plan->bounds,scene[0],scene[1]))return BROOK_VK_REJECTED;
    const uint64_t outputLast=((uint64_t)(output->buffer.height-1u)*output->buffer.strideWords+output->buffer.width)*4u;
    if(outputLast>output->buffer.allocationBytes)return BROOK_VK_REJECTED;
    size_t sizes[BROOK_VK_MAX_LAYERS];
    for(size_t i=0;i<count;++i){const struct BrookVkGraphicsBuffer *b=&sources[i].buffer;
        const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        if(!buffer_valid(b) || b->offsetWords!=l[0] || b->strideWords!=l[1] || b->width!=l[2] || b->height!=l[3])return BROOK_VK_REJECTED;
        sizes[i]=(size_t)(b->allocationBytes/4u);
    }
    if(!brook_vk_scene_valid_multi_output(scene,words,sizes,count))return BROOK_VK_REJECTED;
    for(uint32_t i=0;i<plan->drawCount;++i){const struct QuadDraw *d=&plan->draws[i];
        if(d->layer>=count || d->kind>QUAD_KIND_OVER_ZERO || !rect_inside(d->rect,scene[0],scene[1]) ||
           !quad_rect_contains(plan->bounds,d->rect) ||
           !quad_rect_contains(quad_layer_bounds(scene+BROOK_VK_SCENE_HEADER+d->layer*BROOK_VK_LAYER_WORDS,scene[0],scene[1]),d->rect))
            return BROOK_VK_REJECTED;
        if(d->kind==QUAD_KIND_REPLACE && !quad_layer_replaces(scene+BROOK_VK_SCENE_HEADER+d->layer*BROOK_VK_LAYER_WORDS))return BROOK_VK_REJECTED;
    }
    for(uint32_t i=0;i<plan->clearCount;++i)if(!rect_inside(plan->clears[i],scene[0],scene[1]) ||
        !quad_rect_contains(plan->bounds,plan->clears[i]))return BROOK_VK_REJECTED;
    uint8_t used[BROOK_VK_MAX_LAYERS]={0};
    for(uint32_t i=0;i<plan->drawCount;++i)used[plan->draws[i].layer]=1;
    struct stat ids[BROOK_VK_MAX_LAYERS],outputId;
    if(fstat(output->buffer.fd,&outputId) || !outputId.st_ino)return BROOK_VK_REJECTED;
    for(size_t i=0;i<count;++i)if(used[i]){
        if(fstat(sources[i].buffer.fd,&ids[i]) || !ids[i].st_ino)return BROOK_VK_REJECTED;
        if(ids[i].st_dev==outputId.st_dev && ids[i].st_ino==outputId.st_ino)return BROOK_VK_REJECTED;
        if(sources[i].buffer.syncFd>=0 && !p->importSemaphore)return BROOK_VK_REJECTED;
    }
    ++p->clock;
    /* Nothing is pending, so idle cache entries can be dropped. */
    for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)if(p->cache[i].live && p->cache[i].lastUse+QUAD_CACHE_MAX_AGE<p->clock){
        image_destroy(p,&p->cache[i]);++p->stats.evictions;}
    struct QuadImage *target=image_get(p,output,&outputId,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    struct QuadImage *images[BROOK_VK_MAX_LAYERS]={0};
    if(!target)goto reject;
    for(size_t i=0;i<count;++i)if(used[i] && !(images[i]=image_get(p,&sources[i],&ids[i],VK_IMAGE_USAGE_SAMPLED_BIT)))goto reject;
    VkSemaphore waits[BROOK_VK_MAX_LAYERS];VkPipelineStageFlags stages[BROOK_VK_MAX_LAYERS];
    for(size_t i=0;i<count;++i)if(used[i] && sources[i].buffer.syncFd>=0){
        int fd=fcntl(sources[i].buffer.syncFd,F_DUPFD_CLOEXEC,0);if(fd<0)goto reject;
        VkSemaphoreCreateInfo si={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore=VK_NULL_HANDLE;
        if(vkCreateSemaphore(p->device,&si,NULL,&semaphore)!=VK_SUCCESS){close(fd);goto reject;}
        p->wait[p->waitCount]=semaphore;
        VkImportSemaphoreFdInfoKHR sf={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=semaphore,
            .flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=fd};
        if(p->importSemaphore(p->device,&sf)!=VK_SUCCESS){close(fd);++p->waitCount;goto reject;}
        waits[p->waitCount]=semaphore;stages[p->waitCount++]=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    if(vkResetCommandBuffer(p->command,0)!=VK_SUCCESS || vkResetFences(p->device,1,&p->fence)!=VK_SUCCESS)goto reject;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(vkBeginCommandBuffer(p->command,&begin)!=VK_SUCCESS)goto reject;
    p->gpuValid=0;
    if(p->timestamps){vkCmdResetQueryPool(p->command,p->timestamps,0,2);
        vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,p->timestamps,0);}
    /* Acquire every image from its external owner (the producer, the CPU
     * or the presenter). External memory images are created UNDEFINED; like
     * other dma-buf compositors, treat the external contents as GENERAL so
     * the acquire preserves them. */
    VkImageMemoryBarrier barriers[BROOK_VK_MAX_LAYERS+1];uint32_t barrierCount=0;
    struct QuadImage *distinct[BROOK_VK_MAX_LAYERS+1];uint32_t distinctCount=0;
    distinct[distinctCount++]=target;
    for(size_t i=0;i<count;++i)if(images[i]){int seen=0;
        for(uint32_t j=0;j<distinctCount;++j)if(distinct[j]==images[i])seen=1;
        if(!seen)distinct[distinctCount++]=images[i];}
    for(uint32_t i=0;i<distinctCount;++i){struct QuadImage *m=distinct[i];const int out=m==target;
        barriers[barrierCount++]=(VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=0,.dstAccessMask=out?VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
                VK_ACCESS_INPUT_ATTACHMENT_READ_BIT:VK_ACCESS_SHADER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=p->family,.image=m->image,
            .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    }
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT};
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,1,&host,0,NULL,barrierCount,barriers);
    const struct QuadRect bounds=plan->bounds;
    VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=p->renderPass,.framebuffer=target->framebuffer,
        .renderArea={{bounds.x0,bounds.y0},{(uint32_t)(bounds.x1-bounds.x0),(uint32_t)(bounds.y1-bounds.y0)}}};
    vkCmdBeginRenderPass(p->command,&render,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport={0,0,(float)scene[0],(float)scene[1],0,1};
    vkCmdSetViewport(p->command,0,1,&viewport);vkCmdSetScissor(p->command,0,1,&render.renderArea);
    if(plan->clearCount){
        VkClearAttachment clear={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=0,.clearValue={.color={{0,0,0,0}}}};
        VkClearRect rects[QUAD_PLAN_MAX_CLEARS];
        for(uint32_t i=0;i<plan->clearCount;++i){const struct QuadRect r=plan->clears[i];
            rects[i]=(VkClearRect){{{r.x0,r.y0},{(uint32_t)(r.x1-r.x0),(uint32_t)(r.y1-r.y0)}},0,1};}
        vkCmdClearAttachments(p->command,1,&clear,plan->clearCount,rects);
    }
    vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,1,1,&target->set,0,NULL);
    int boundPipeline=-1,boundLayer=-1,written=plan->clearCount!=0;
    for(uint32_t i=0;i<plan->drawCount;++i){const struct QuadDraw *d=&plan->draws[i];
        const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+d->layer*BROOK_VK_LAYER_WORDS;
        const int blend=d->kind==QUAD_KIND_STRAIGHT || d->kind==QUAD_KIND_PREMULTIPLIED;
        const int pipeline=!p->fixedBlend || !blend?QUAD_PIPELINE_NONE:d->kind==QUAD_KIND_STRAIGHT?
            QUAD_PIPELINE_STRAIGHT:QUAD_PIPELINE_PREMULTIPLIED;
        if((int)d->layer!=boundLayer && blend && !p->fixedBlend && written){
            /* This layer reads pixels earlier layers (or clears) wrote. A
             * layer's own rectangles are disjoint, so one barrier suffices. */
            VkMemoryBarrier self={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
            vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT,1,&self,0,NULL,0,NULL);
        }
        written=1;
        if(pipeline!=boundPipeline){vkCmdBindPipeline(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipeline[pipeline]);boundPipeline=pipeline;}
        if((int)d->layer!=boundLayer){
            vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,0,1,&images[d->layer]->set,0,NULL);
            boundLayer=d->layer;
        }
        const struct QuadPush push={{d->rect.x0,d->rect.y0,d->rect.x1,d->rect.y1},{2.0f/(float)scene[0],2.0f/(float)scene[1]},
            {(int32_t)l[8],(int32_t)l[9]},{l[10],l[11]},{l[4],l[5]},{l[6],l[7]},
            {(float)l[6]/(float)l[10],(float)l[7]/(float)l[11]},l[16]&255u,
            ((l[16]&BROOK_VK_STRAIGHT_ALPHA)?QUAD_MODE_STRAIGHT:0u)|((l[16]&BROOK_VK_OPAQUE_SOURCE)?QUAD_MODE_OPAQUE:0u)|
            (d->kind==QUAD_KIND_OVER_ZERO?QUAD_MODE_OVER_ZERO:0u)|(blend && !p->fixedBlend?QUAD_MODE_READ:0u)|fetch_mode(l)};
        vkCmdPushConstants(p->command,p->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
        vkCmdDraw(p->command,4,1,0,0);
    }
    vkCmdEndRenderPass(p->command);
    for(uint32_t i=0;i<distinctCount;++i){const int out=distinct[i]==target;
        barriers[i].srcAccessMask=out?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:0;barriers[i].dstAccessMask=0;
        barriers[i].oldLayout=VK_IMAGE_LAYOUT_GENERAL;barriers[i].newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex=p->family;barriers[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;}
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,distinctCount,barriers);
    if(p->timestamps)vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timestamps,1);
    if(vkEndCommandBuffer(p->command)!=VK_SUCCESS)goto reject;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=p->waitCount,.pWaitSemaphores=waits,
        .pWaitDstStageMask=stages,.commandBufferCount=1,.pCommandBuffers=&p->command};
    if(vkQueueSubmit(p->queue,1,&submit,p->fence)!=VK_SUCCESS){p->uncertain=1;return BROOK_VK_UNCERTAIN;}
    ++p->stats.submits;p->pending=1;return BROOK_VK_PENDING;
reject:
    /* Cached imports stay valid for the next submission. */
    frame_cleanup(p);return BROOK_VK_REJECTED;
}

static BrookVkResult retire(BrookVkQuadPass *p,VkResult status){
    if(status==VK_NOT_READY || status==VK_TIMEOUT)return BROOK_VK_PENDING;
    p->pending=0;
    if(status!=VK_SUCCESS){p->uncertain=1;return BROOK_VK_UNCERTAIN;}
    if(p->timestamps){
        uint64_t ticks[2]={0};
        if(vkGetQueryPoolResults(p->device,p->timestamps,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
            const uint64_t mask=p->timestampBits==64?UINT64_MAX:(UINT64_C(1)<<p->timestampBits)-1;
            const double ns=(double)((ticks[1]-ticks[0])&mask)*p->timestampPeriod;
            if(ns>=0 && ns<(double)UINT64_MAX){p->gpuNs=(uint64_t)ns;p->gpuValid=1;}
        }
    }
    frame_cleanup(p);return BROOK_VK_COMPLETE;
}
BrookVkResult brook_vk_quad_pass_poll(BrookVkQuadPass *p){
    if(!p || !p->pending || p->uncertain)return BROOK_VK_REJECTED;
    return retire(p,vkGetFenceStatus(p->device,p->fence));
}
BrookVkResult brook_vk_quad_pass_wait(BrookVkQuadPass *p,uint64_t timeoutNs){
    if(!p || !p->pending || p->uncertain)return BROOK_VK_REJECTED;
    return retire(p,vkWaitForFences(p->device,1,&p->fence,VK_TRUE,timeoutNs));
}
int brook_vk_quad_pass_gpu_timestamp_ns(const BrookVkQuadPass *p,uint64_t *ns){
    if(!p || !ns || !p->gpuValid)return 0;
    *ns=p->gpuNs;return 1;
}
struct BrookVkQuadStats brook_vk_quad_pass_stats(const BrookVkQuadPass *p){
    return p?p->stats:(struct BrookVkQuadStats){0};
}
