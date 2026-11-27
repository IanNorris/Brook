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
enum { QUAD_MODE_STRAIGHT=1, QUAD_MODE_OPAQUE=2, QUAD_MODE_OVER_ZERO=4, QUAD_MODE_SAMPLER=8, QUAD_MODE_DIVIDE=16, QUAD_MODE_READ=32,
       QUAD_MODE_UP=256, QUAD_MODE_BOX=512 };
enum { QUAD_PIPELINE_STRAIGHT=0, QUAD_PIPELINE_PREMULTIPLIED=1, QUAD_PIPELINE_NONE=2, QUAD_PIPELINE_GLASS=3,
       QUAD_PIPELINE_CAPTURE=4, QUAD_PIPELINE_FILTER=5, QUAD_PIPELINES=6 };
struct QuadPush {int32_t rect[4];float invHalf[2];int32_t dst[2];uint32_t dstSize[2],src[2],srcSize[2];float scale[2];uint32_t opacity,mode;};
_Static_assert(sizeof(struct QuadPush)==72,"quad push constant layout");
struct QuadGlassPush {
    struct QuadPush quad;
    uint32_t padding[2];
    struct QuadRect screen,atlas;
    uint32_t radius;
};
_Static_assert(offsetof(struct QuadGlassPush,screen)==80 && sizeof(struct QuadGlassPush)==116,"glass push layout");
/* quad_backdrop.frag's push block (within the guaranteed 128 bytes). */
struct QuadBackdropPush {
    struct QuadPush quad;
    int32_t origin[2],limit[2],clip0[2],clip1[2],hole0[2],hole1[2];
    uint32_t block;
};
_Static_assert(offsetof(struct QuadBackdropPush,origin)==72 && sizeof(struct QuadBackdropPush)==124,"backdrop push layout");
enum { QUAD_TIMESTAMP_COUNT=4+QUAD_BACKDROP_TILES*8 };

/* One imported linear BGRA8 image. live: slot holds an import. set: the sampled
 * source descriptor, or for an output its input-attachment descriptor. */
struct QuadImage {
    dev_t dev;ino_t ino;uint64_t bytes,lastUse;
    uint32_t width,height,offsetWords,strideWords;VkImageUsageFlags usage;
    int pin,live;uint64_t modifier;
    VkImage image;VkDeviceMemory memory;VkImageView view;VkDescriptorSet set;VkFramebuffer framebuffer;
    VkBuffer upload;
    VkDeviceMemory uploadMemory;
    VkDescriptorSet linear; /* sampled with linearSampler (backdrop capture), made on first use */
};
struct BrookVkQuadPass {
    VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
    VkDescriptorSetLayout setLayout,inputLayout;VkDescriptorPool pool;int fixedBlend;VkPipelineLayout layout;VkRenderPass renderPass;
    VkShaderModule vertex,fragment,glassFragment,captureFragment,filterFragment;VkSampler sampler,atlasSampler,linearSampler;VkPipeline pipeline[QUAD_PIPELINES];
    int linearSources; /* linear-tiled BGRA8 supports filtering: the capture may box 2x2 with one tap */
    VkCommandPool commandPool;VkCommandBuffer command;VkFence fence;
    VkQueryPool timestamps;uint32_t timestampBits;float timestampPeriod;uint64_t gpuNs;int gpuValid;
    PFN_vkGetMemoryFdPropertiesKHR getFdProperties;PFN_vkImportSemaphoreFdKHR importSemaphore;int modifiers;
    uint64_t sourceModifiers[BROOK_VK_MAX_LAYERS];
    struct QuadImage cache[QUAD_CACHE_SLOTS],frame[QUAD_FRAME_SLOTS];unsigned frameCount;
    int optimalOutput;
    struct QuadImage optimalTarget;
    VkBuffer frameOutput;
    VkDeviceMemory frameOutputMemory;
    VkSemaphore wait[BROOK_VK_MAX_LAYERS];unsigned waitCount;
    /* The backdrop atlas: atlas holds mip 0's view, input-attachment set and
     * framebuffer; atlasLevel and atlasFramebuffer the other mips';
     * atlasSample[m] samples mip m alone (linear). atlasBuilt lists the keys
     * of the tiles whose texels are current (the last glass frame's). */
    struct QuadImage atlas;
    VkImageView atlasLevel[QUAD_BACKDROP_MIPS];VkFramebuffer atlasFramebuffer[QUAD_BACKDROP_MIPS];
    VkDescriptorSet atlasSample[QUAD_BACKDROP_MIPS];
    uint64_t atlasBuilt[QUAD_BACKDROP_TILES];unsigned atlasBuiltCount;int atlasInitialized;
    /* Timestamps for the blur only (BROOK_RENDER_BLUR without
     * BROOK_RENDER_GPU_TIMESTAMPS): no per-chrome-draw pairs. */
    int timingBlurOnly;
    unsigned timestampNext;
    uint8_t timestampClass[QUAD_TIMESTAMP_COUNT/2];
    int atlasTimed,chromeTimed,timingOverflow;
    struct BrookVkBackdropStats backdropStats;
    uint64_t clock;int pending,uncertain;VkResult lastResult;
    struct BrookVkQuadStats stats;
};

static void image_destroy(BrookVkQuadPass *p,struct QuadImage *m){
    if(m->framebuffer)vkDestroyFramebuffer(p->device,m->framebuffer,NULL);
    if(m->set)vkFreeDescriptorSets(p->device,p->pool,1,&m->set);
    if(m->linear)vkFreeDescriptorSets(p->device,p->pool,1,&m->linear);
    if(m->view)vkDestroyImageView(p->device,m->view,NULL);
    if(m->image)vkDestroyImage(p->device,m->image,NULL);
    if(m->memory)vkFreeMemory(p->device,m->memory,NULL);
    if(m->upload)vkDestroyBuffer(p->device,m->upload,NULL);
    if(m->uploadMemory)vkFreeMemory(p->device,m->uploadMemory,NULL);
    if(m->pin>=0)close(m->pin);
    *m=(struct QuadImage){.pin=-1};
}
static void frame_cleanup(BrookVkQuadPass *p){
    if(p->frameOutput)vkDestroyBuffer(p->device,p->frameOutput,NULL);
    if(p->frameOutputMemory)vkFreeMemory(p->device,p->frameOutputMemory,NULL);
    p->frameOutput=VK_NULL_HANDLE;
    p->frameOutputMemory=VK_NULL_HANDLE;
    for(unsigned i=0;i<p->frameCount;++i)image_destroy(p,&p->frame[i]);
    p->frameCount=0;
    for(unsigned i=0;i<p->waitCount;++i)if(p->wait[i])vkDestroySemaphore(p->device,p->wait[i],NULL);
    p->waitCount=0;
}

static void atlas_destroy(BrookVkQuadPass *p){
    for(unsigned m=0;m<QUAD_BACKDROP_MIPS;++m){
        if(p->atlasSample[m])vkFreeDescriptorSets(p->device,p->pool,1,&p->atlasSample[m]);
        if(m && p->atlasFramebuffer[m])vkDestroyFramebuffer(p->device,p->atlasFramebuffer[m],NULL);
        if(m && p->atlasLevel[m])vkDestroyImageView(p->device,p->atlasLevel[m],NULL);
        p->atlasSample[m]=VK_NULL_HANDLE;p->atlasFramebuffer[m]=VK_NULL_HANDLE;p->atlasLevel[m]=VK_NULL_HANDLE;
    }
    image_destroy(p,&p->atlas);
    if(p->atlasSampler)vkDestroySampler(p->device,p->atlasSampler,NULL);
    if(p->linearSampler)vkDestroySampler(p->device,p->linearSampler,NULL);
    p->atlasSampler=p->linearSampler=VK_NULL_HANDLE;p->atlasBuiltCount=0;p->atlasInitialized=0;
}

int brook_vk_quad_pass_destroy(BrookVkQuadPass *p){
    if(!p)return 1;
    if(p->pending || p->uncertain)return 0;
    frame_cleanup(p);
    image_destroy(p,&p->optimalTarget);
    atlas_destroy(p);
    if(p->glassFragment)vkDestroyShaderModule(p->device,p->glassFragment,NULL);
    if(p->captureFragment)vkDestroyShaderModule(p->device,p->captureFragment,NULL);
    if(p->filterFragment)vkDestroyShaderModule(p->device,p->filterFragment,NULL);
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

static BrookVkQuadPass *quad_pass_create(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,int optimalOutput){
    if(!physical || !device || !queue)return NULL;
    VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_B8G8R8A8_UNORM,&format);
    const VkFormatFeatureFlags attachmentFeatures=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    const VkFormatFeatureFlags tiling=optimalOutput?format.optimalTilingFeatures:format.linearTilingFeatures;
    const VkFormatFeatureFlags needed=attachmentFeatures|(optimalOutput?VK_FORMAT_FEATURE_TRANSFER_SRC_BIT:0);
    const VkFormatFeatureFlags sampled=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        (optimalOutput?VK_FORMAT_FEATURE_TRANSFER_DST_BIT:0);
    if((tiling&sampled)!=sampled || (tiling&needed)!=needed)return NULL;
    BrookVkQuadPass *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->atlas.pin=p->optimalTarget.pin=-1;
    p->optimalOutput=optimalOutput;
    p->physical=physical;p->device=device;p->queue=queue;p->family=family;
    for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)p->cache[i].pin=-1;
    for(unsigned i=0;i<QUAD_FRAME_SLOTS;++i)p->frame[i].pin=-1;
#define OK(x) do { if((x)!=VK_SUCCESS)goto fail; } while(0)
    p->getFdProperties=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR");
    p->importSemaphore=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(device,"vkImportSemaphoreFdKHR");
    if(!p->getFdProperties)goto fail;
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
    if(props.limits.maxPushConstantsSize<sizeof(struct QuadGlassPush))goto fail;
    const char *blur=getenv("BROOK_RENDER_BLUR"),*blurTiming=getenv("BROOK_RENDER_BLUR_TIMING");
    p->timingBlurOnly=!getenv("BROOK_RENDER_GPU_TIMESTAMPS") && blur && !strcmp(blur,"1") && !(blurTiming && blurTiming[0]=='0');
    if(getenv("BROOK_RENDER_GPU_TIMESTAMPS") || p->timingBlurOnly){
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,NULL);
        if(!n || n>64 || family>=n)goto fail;
        VkQueueFamilyProperties q[64];vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q);
        p->timestampBits=q[family].timestampValidBits;p->timestampPeriod=props.limits.timestampPeriod;
        if(p->timestampBits && p->timestampBits<=64 && p->timestampPeriod>0){
            VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=QUAD_TIMESTAMP_COUNT};
            OK(vkCreateQueryPool(device,&query,NULL,&p->timestamps));
        }
    }
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    OK(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->setLayout));
    binding.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    OK(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->inputLayout));
    /* Each image: a sampled or input set, and a linear set for the backdrop. */
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*(QUAD_CACHE_SLOTS+QUAD_FRAME_SLOTS)+1+QUAD_BACKDROP_MIPS},
                                   {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,QUAD_CACHE_SLOTS+QUAD_FRAME_SLOTS+2}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets=2*(QUAD_CACHE_SLOTS+QUAD_FRAME_SLOTS)+2+QUAD_BACKDROP_MIPS,.poolSizeCount=2,.pPoolSizes=sizes};
    OK(vkCreateDescriptorPool(device,&dp,NULL,&p->pool));
    p->fixedBlend=getenv("BROOK_RENDER_QUAD_FIXED_BLEND")!=NULL;
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,.offset=0,.size=sizeof(struct QuadGlassPush)};
    const VkDescriptorSetLayout setLayouts[3]={p->setLayout,p->inputLayout,p->setLayout};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=3,.pSetLayouts=setLayouts,
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
    sm.codeSize=sizeof(quadBlurCode);sm.pCode=quadBlurCode;
    OK(vkCreateShaderModule(device,&sm,NULL,&p->glassFragment));
    sm.codeSize=sizeof(quadBackdropCaptureCode);sm.pCode=quadBackdropCaptureCode;
    OK(vkCreateShaderModule(device,&sm,NULL,&p->captureFragment));
    sm.codeSize=sizeof(quadBackdropFilterCode);sm.pCode=quadBackdropFilterCode;
    OK(vkCreateShaderModule(device,&sm,NULL,&p->filterFragment));
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
        {.blendEnable=VK_FALSE,.colorWriteMask=15},
        {.blendEnable=VK_FALSE,.colorWriteMask=15},
        {.blendEnable=VK_FALSE,.colorWriteMask=15},
        {.blendEnable=VK_FALSE,.colorWriteMask=15}};
    VkDynamicState dynamicStates[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamicStates};
    for(unsigned i=0;i<QUAD_PIPELINES;++i){
        stages[1].module=i==QUAD_PIPELINE_GLASS?p->glassFragment:i==QUAD_PIPELINE_CAPTURE?p->captureFragment:
            i==QUAD_PIPELINE_FILTER?p->filterFragment:p->fragment;
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

BrookVkQuadPass *brook_vk_quad_pass_create(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family){
    return quad_pass_create(physical,device,queue,family,0);
}

BrookVkQuadPass *brook_vk_quad_pass_create_optimal_output(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family){
    return quad_pass_create(physical,device,queue,family,1);
}

static int image_allocate_optimal(BrookVkQuadPass *p,uint32_t width,uint32_t height,
                                  VkImageUsageFlags usage,struct QuadImage *image){
    const VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage handle=VK_NULL_HANDLE;
    if(vkCreateImage(p->device,&info,NULL,&handle)!=VK_SUCCESS)return 0;
    image->image=handle;
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(p->device,image->image,&needs);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(p->physical,&memory);
    uint32_t type=0;
    while(type<memory.memoryTypeCount && (!(needs.memoryTypeBits&(1u<<type)) ||
          !(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
    if(type==memory.memoryTypeCount)return 0;
    const VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=needs.size,.memoryTypeIndex=type};
    VkDeviceMemory allocated=VK_NULL_HANDLE;
    if(vkAllocateMemory(p->device,&allocate,NULL,&allocated)!=VK_SUCCESS)return 0;
    image->memory=allocated;
    if(vkBindImageMemory(p->device,image->image,image->memory,0)!=VK_SUCCESS)return 0;
    const VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image->image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=info.format,
        .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    VkImageView imageView=VK_NULL_HANDLE;
    if(vkCreateImageView(p->device,&view,NULL,&imageView)!=VK_SUCCESS)return 0;
    image->view=imageView;
    return 1;
}

static struct QuadImage *optimal_target(BrookVkQuadPass *p,uint32_t width,uint32_t height){
    struct QuadImage *target=&p->optimalTarget;
    if(target->framebuffer && target->width==width && target->height==height)return target;
    image_destroy(p,target);
    if(!image_allocate_optimal(p,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,target))goto fail;
    VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,
        .descriptorSetCount=1,.pSetLayouts=&p->inputLayout};
    VkDescriptorSet set=VK_NULL_HANDLE;
    if(vkAllocateDescriptorSets(p->device,&ds,&set)!=VK_SUCCESS)goto fail;
    target->set=set;
    const VkDescriptorImageInfo input={.imageView=target->view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=target->set,.dstBinding=0,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=&input};
    vkUpdateDescriptorSets(p->device,1,&write,0,NULL);
    const VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=p->renderPass,.attachmentCount=1,
        .pAttachments=&target->view,.width=width,.height=height,.layers=1};
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    if(vkCreateFramebuffer(p->device,&fb,NULL,&framebuffer)!=VK_SUCCESS)goto fail;
    target->framebuffer=framebuffer;
    target->width=width;
    target->height=height;
    return target;
fail:
    image_destroy(p,target);return NULL;
}

/* On failure the caller destroys any handles published here. */
static int import_transfer_buffer(BrookVkQuadPass *p,const struct BrookVkGraphicsBuffer *externalBuffer,
                                  VkBufferUsageFlags usage,VkBuffer *outBuffer,VkDeviceMemory *outMemory){
    const off_t size=lseek(externalBuffer->fd,0,SEEK_END);
    if(lseek(externalBuffer->fd,0,SEEK_SET)!=0 || size<0 || (uint64_t)size!=externalBuffer->allocationBytes)return 0;
    const VkPhysicalDeviceExternalBufferInfo externalInfo={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage=usage,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkExternalBufferProperties properties={.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(p->physical,&externalInfo,&properties);
    const VkExternalMemoryFeatureFlags features=properties.externalMemoryProperties.externalMemoryFeatures;
    if(!(features&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))return 0;
    const VkExternalMemoryBufferCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&external,.size=externalBuffer->allocationBytes,
        .usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer=VK_NULL_HANDLE;
    if(vkCreateBuffer(p->device,&info,NULL,&buffer)!=VK_SUCCESS)return 0;
    *outBuffer=buffer;
    VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 needs={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dedicated};
    const VkBufferMemoryRequirementsInfo2 requirements={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,.buffer=buffer};
    vkGetBufferMemoryRequirements2(p->device,&requirements,&needs);
    if(needs.memoryRequirements.size>externalBuffer->allocationBytes)return 0;
    VkMemoryFdPropertiesKHR fdProperties={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if(p->getFdProperties(p->device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,externalBuffer->fd,&fdProperties)!=VK_SUCCESS)return 0;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(p->physical,&memory);
    const uint32_t allowed=needs.memoryRequirements.memoryTypeBits&fdProperties.memoryTypeBits;
    uint32_t type=0;
    while(type<memory.memoryTypeCount && !(allowed&(1u<<type)))++type;
    if(type==memory.memoryTypeCount)return 0;
    const int fd=fcntl(externalBuffer->fd,F_DUPFD_CLOEXEC,0);
    if(fd<0)return 0;
    const VkMemoryDedicatedAllocateInfo dedicatedInfo={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=buffer};
    const VkImportMemoryFdInfoKHR import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext=(dedicated.requiresDedicatedAllocation || (features&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT))?&dedicatedInfo:NULL,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=fd};
    const VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,
        .allocationSize=externalBuffer->allocationBytes,.memoryTypeIndex=type};
    VkDeviceMemory allocated=VK_NULL_HANDLE;
    if(vkAllocateMemory(p->device,&allocate,NULL,&allocated)!=VK_SUCCESS){close(fd);return 0;}
    *outMemory=allocated;
    return vkBindBufferMemory(p->device,buffer,allocated,0)==VK_SUCCESS;
}

/* Import a linear BGRA8 DMA-BUF. The image is created at the buffer's
 * width, or, if the driver's natural row pitch differs, at the stride width
 * (the shader never samples outside the used crop). The data offset becomes
 * the memory binding offset. */
void brook_vk_quad_pass_set_modifiers(BrookVkQuadPass *p,int enabled){if(p)p->modifiers=enabled!=0;}
int brook_vk_quad_pass_set_source_modifiers(BrookVkQuadPass *p,const uint64_t *modifiers,size_t count){
    if(!p || count>BROOK_VK_MAX_LAYERS)return 0;
    memset(p->sourceModifiers,0,sizeof(p->sourceModifiers));
    for(size_t i=0;i<count;++i){if(modifiers[i] && !p->modifiers)return 0;p->sourceModifiers[i]=modifiers[i];}
    return 1;
}
static int image_create(BrookVkQuadPass *p,int fd,const struct BrookVkGraphicsBuffer *b,uint64_t modifier,uint32_t width,
                        VkImageUsageFlags usage,struct QuadImage *m){
    /* Intel X/Y-tiled sources use an explicit DRM format modifier layout
     * (one plane, pitch = stride, data offset = memory binding offset). */
    const int tiled=modifier!=0;
    if(tiled && (!p->modifiers || (usage&~VK_IMAGE_USAGE_SAMPLED_BIT) || width!=b->width))return 0;
    const VkSubresourceLayout plane={.offset=0,.rowPitch=(VkDeviceSize)b->strideWords*4u};
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicitModifier={.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier=modifier,.drmFormatModifierPlaneCount=1,.pPlaneLayouts=&plane};
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier=modifier,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    const VkImageTiling vkTiling=tiled?VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT:VK_IMAGE_TILING_LINEAR;
    VkPhysicalDeviceExternalImageFormatInfo externalFormat={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext=tiled?&modifierInfo:NULL,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 format={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&externalFormat,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.type=VK_IMAGE_TYPE_2D,.tiling=vkTiling,.usage=usage};
    VkExternalImageFormatProperties externalProperties={.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&externalProperties};
    if(vkGetPhysicalDeviceImageFormatProperties2(p->physical,&format,&properties)!=VK_SUCCESS ||
       !(externalProperties.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
       width>properties.imageFormatProperties.maxExtent.width || b->height>properties.imageFormatProperties.maxExtent.height)return 0;
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext=tiled?&explicitModifier:NULL,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo create={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={width,b->height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=vkTiling,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    if(vkCreateImage(p->device,&create,NULL,&m->image)!=VK_SUCCESS){m->image=VK_NULL_HANDLE;return 0;}
    VkImageSubresource subresource={.aspectMask=tiled?VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT:VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(p->device,m->image,&subresource,&layout);
    VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dedicated};
    VkImageMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=m->image};
    vkGetImageMemoryRequirements2(p->device,&info,&requirements);
    const uint64_t bind=(uint64_t)b->offsetWords*4u,need=requirements.memoryRequirements.size;
    const uint64_t tileRows=modifier==0x0100000000000001ull?8u:32u;
    const uint64_t last=tiled?bind+(((uint64_t)b->height+tileRows-1u)/tileRows*tileRows)*b->strideWords*4u:
        bind+((uint64_t)(b->height-1u)*b->strideWords+b->width)*4u;
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
static int image_import(BrookVkQuadPass *p,const struct BrookVkGraphicsBuffer *b,uint64_t modifier,const struct stat *id,
                        VkImageUsageFlags usage,int pin,struct QuadImage *m){
    *m=(struct QuadImage){.pin=-1};
    int ok;
    if(p->optimalOutput){
        const uint64_t last=((uint64_t)b->offsetWords+(uint64_t)(b->height-1)*b->strideWords+b->width)*4;
        if(modifier || usage!=VK_IMAGE_USAGE_SAMPLED_BIT || last>b->allocationBytes)return 0;
        ok=import_transfer_buffer(p,b,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&m->upload,&m->uploadMemory) &&
            image_allocate_optimal(p,b->width,b->height,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,m);
    }else{
        off_t size=lseek(b->fd,0,SEEK_END);
        if(lseek(b->fd,0,SEEK_SET)!=0 || size<0 || (uint64_t)size!=b->allocationBytes)return 0;
        ok=image_create(p,b->fd,b,modifier,b->width,usage,m);
        if(!ok && !modifier && b->strideWords!=b->width){image_destroy(p,m);ok=image_create(p,b->fd,b,0,b->strideWords,usage,m);}
    }
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
    m->offsetWords=b->offsetWords;m->strideWords=b->strideWords;m->usage=usage;m->live=1;m->modifier=modifier;
    ++p->stats.imports;return 1;
}
static int image_matches(const struct QuadImage *m,const struct BrookVkGraphicsBuffer *b,uint64_t modifier,const struct stat *id,VkImageUsageFlags usage){
    return m->live && m->modifier==modifier && m->dev==id->st_dev && m->ino==id->st_ino && m->bytes==b->allocationBytes && m->width==b->width &&
           m->height==b->height && m->offsetWords==b->offsetWords && m->strideWords==b->strideWords && m->usage==usage;
}
/* Find or import; returns NULL on failure. Cached imports outlive the
 * frame; others are destroyed when it retires. */
static struct QuadImage *image_get(BrookVkQuadPass *p,const struct BrookVkQuadSource *s,uint64_t modifier,const struct stat *id,VkImageUsageFlags usage){
    const struct BrookVkGraphicsBuffer *b=&s->buffer;
    for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)if(image_matches(&p->cache[i],b,modifier,id,usage)){
        p->cache[i].lastUse=p->clock;++p->stats.cacheHits;return &p->cache[i];}
    for(unsigned i=0;i<p->frameCount;++i)if(image_matches(&p->frame[i],b,modifier,id,usage))return &p->frame[i];
    if(s->cache){
        int slot=-1;
        for(unsigned i=0;i<QUAD_CACHE_SLOTS && slot<0;++i)if(!p->cache[i].live)slot=(int)i;
        if(slot<0){ /* Evict the least recently used entry not used by this frame. */
            for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)if(p->cache[i].lastUse!=p->clock &&
                (slot<0 || p->cache[i].lastUse<p->cache[slot].lastUse))slot=(int)i;
            if(slot>=0){image_destroy(p,&p->cache[slot]);++p->stats.evictions;}
        }
        if(slot>=0){
            if(!image_import(p,b,modifier,id,usage,1,&p->cache[slot]))return NULL;
            p->cache[slot].lastUse=p->clock;return &p->cache[slot];
        }
    }
    if(p->frameCount==QUAD_FRAME_SLOTS || !image_import(p,b,modifier,id,usage,0,&p->frame[p->frameCount]))return NULL;
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

static VkRect2D quad_vk_rect(struct QuadRect r){
    return (VkRect2D){{r.x0,r.y0},{(uint32_t)(r.x1-r.x0),(uint32_t)(r.y1-r.y0)}};
}

int brook_vk_quad_pass_enable_backdrop(BrookVkQuadPass *p)
{
    if(!p)return 0;
    if(p->atlas.framebuffer)return 1;
    struct QuadImage *a=&p->atlas;
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(p->physical,VK_FORMAT_B8G8R8A8_UNORM,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))return 0;
    const VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={QUAD_BACKDROP_WIDTH,QUAD_BACKDROP_HEIGHT,1},.mipLevels=QUAD_BACKDROP_MIPS,
        .arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    if(vkCreateImage(p->device,&info,NULL,&a->image)!=VK_SUCCESS){a->image=VK_NULL_HANDLE;goto fail;}
    VkMemoryRequirements requirements;vkGetImageMemoryRequirements(p->device,a->image,&requirements);
    VkPhysicalDeviceMemoryProperties props;vkGetPhysicalDeviceMemoryProperties(p->physical,&props);
    uint32_t type=0;
    while(type<props.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) ||
          !(props.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
    if(type==props.memoryTypeCount)goto fail;
    const VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size,.memoryTypeIndex=type};
    if(vkAllocateMemory(p->device,&allocate,NULL,&a->memory)!=VK_SUCCESS){a->memory=VK_NULL_HANDLE;goto fail;}
    if(vkBindImageMemory(p->device,a->image,a->memory,0)!=VK_SUCCESS)goto fail;
    const VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,
        .minFilter=VK_FILTER_LINEAR,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if(vkCreateSampler(p->device,&sampler,NULL,&p->atlasSampler)!=VK_SUCCESS){p->atlasSampler=VK_NULL_HANDLE;goto fail;}
    /* Unnormalized bilinear taps of scene layers at 2x2 corners. */
    const VkFormatFeatureFlags sourceTiling=p->optimalOutput?properties.optimalTilingFeatures:properties.linearTilingFeatures;
    p->linearSources=(sourceTiling&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)!=0;
    const VkSamplerCreateInfo linear={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .unnormalizedCoordinates=VK_TRUE};
    if(vkCreateSampler(p->device,&linear,NULL,&p->linearSampler)!=VK_SUCCESS){p->linearSampler=VK_NULL_HANDLE;goto fail;}
    /* One single-level view per mip: a pass draws one level while sampling
     * its neighbour, so no view spans the attachment. */
    for(unsigned m=0;m<QUAD_BACKDROP_MIPS;++m){
        const VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=a->image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=info.format,
            .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.baseMipLevel=m,.levelCount=1,.layerCount=1}};
        VkImageView *v=m?&p->atlasLevel[m]:&a->view;
        if(vkCreateImageView(p->device,&view,NULL,v)!=VK_SUCCESS){*v=VK_NULL_HANDLE;goto fail;}
        p->atlasLevel[m]=*v;
        const VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=p->renderPass,
            .attachmentCount=1,.pAttachments=v,.width=QUAD_BACKDROP_WIDTH>>m,.height=QUAD_BACKDROP_HEIGHT>>m,.layers=1};
        VkFramebuffer *f=m?&p->atlasFramebuffer[m]:&a->framebuffer;
        if(vkCreateFramebuffer(p->device,&fb,NULL,f)!=VK_SUCCESS){*f=VK_NULL_HANDLE;goto fail;}
        p->atlasFramebuffer[m]=*f;
        const VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,
            .descriptorSetCount=1,.pSetLayouts=&p->setLayout};
        if(vkAllocateDescriptorSets(p->device,&ds,&p->atlasSample[m])!=VK_SUCCESS){p->atlasSample[m]=VK_NULL_HANDLE;goto fail;}
        const VkDescriptorImageInfo sample={.sampler=p->atlasSampler,.imageView=*v,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->atlasSample[m],.descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&sample};
        vkUpdateDescriptorSets(p->device,1,&write,0,NULL);
    }
    const VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,
        .descriptorSetCount=1,.pSetLayouts=&p->inputLayout};
    if(vkAllocateDescriptorSets(p->device,&ds,&a->set)!=VK_SUCCESS){a->set=VK_NULL_HANDLE;goto fail;}
    const VkDescriptorImageInfo read={.imageView=a->view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=a->set,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=&read};
    vkUpdateDescriptorSets(p->device,1,&write,0,NULL);
    return 1;
fail:
    atlas_destroy(p);
    return 0;
}

static void quad_blend_barrier(BrookVkQuadPass *p)
{
    const VkMemoryBarrier self={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_DEPENDENCY_BY_REGION_BIT,1,&self,0,NULL,0,NULL);
}

static int quad_chrome_begin(BrookVkQuadPass *p,unsigned category)
{
    if(!p->timestamps)return 0;
    p->chromeTimed=1;
    if(p->timestampNext+1>=QUAD_TIMESTAMP_COUNT){p->timingOverflow=1;return 0;}
    p->timestampClass[p->timestampNext/2]=(uint8_t)category;
    vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timestamps,p->timestampNext++);
    return 1;
}
static void quad_chrome_end(BrookVkQuadPass *p,int recorded)
{
    if(recorded)vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timestamps,p->timestampNext++);
}
/* A tile's texels at mip m. */
static struct QuadRect atlas_level_rect(const struct QuadBackdropTile *t,unsigned m)
{
    const int32_t x=t->atlas.x0>>m,y=t->atlas.y0>>m,s=1<<m;
    return (struct QuadRect){x,y,x+(t->atlas.x1-t->atlas.x0+s-1)/s,y+(t->atlas.y1-t->atlas.y0+s-1)/s};
}
static struct QuadPush quad_layer_push(const uint32_t *scene,const uint32_t *l,struct QuadRect rect,uint32_t mode)
{
    return (struct QuadPush){
        {rect.x0,rect.y0,rect.x1,rect.y1},{2.0f/(float)scene[0],2.0f/(float)scene[1]},
        {(int32_t)l[8],(int32_t)l[9]},{l[10],l[11]},{l[4],l[5]},{l[6],l[7]},
        {(float)l[6]/(float)l[10],(float)l[7]/(float)l[11]},l[16]&255u,
        ((l[16]&BROOK_VK_STRAIGHT_ALPHA)?QUAD_MODE_STRAIGHT:0u)|
        ((l[16]&BROOK_VK_OPAQUE_SOURCE)?QUAD_MODE_OPAQUE:0u)|mode|fetch_mode(l)};
}
static void quad_draw_rect(BrookVkQuadPass *p,const uint32_t *scene,const uint32_t *l,struct QuadRect rect,
                           unsigned kind,const struct QuadBackdropTile *glass)
{
    const int blending=kind==QUAD_KIND_STRAIGHT || kind==QUAD_KIND_PREMULTIPLIED;
    const int exact=glass || !p->fixedBlend;
    const struct QuadGlassPush push={.quad=quad_layer_push(scene,l,rect,
        (kind==QUAD_KIND_OVER_ZERO?QUAD_MODE_OVER_ZERO:0u)|((blending && exact) || glass?QUAD_MODE_READ:0u)),
        .screen=glass?glass->capture:(struct QuadRect){0},
        .atlas=glass?atlas_level_rect(glass,QUAD_BACKDROP_FINAL):(struct QuadRect){0},
        .radius=glass?1u<<(glass->base+QUAD_BACKDROP_FINAL):0u};
    vkCmdPushConstants(p->command,p->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
        0,sizeof(push),&push);
    vkCmdDraw(p->command,4,1,0,0);
}

static void atlas_barrier(BrookVkQuadPass *p)
{
    const VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=p->atlas.image,.subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=QUAD_BACKDROP_MIPS,.layerCount=1}};
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&b);
}
/* A pass over mip m; area bounds the texels it draws. */
static void atlas_begin(BrookVkQuadPass *p,unsigned m,unsigned pipeline,struct QuadRect area)
{
    const uint32_t w=QUAD_BACKDROP_WIDTH>>m,h=QUAD_BACKDROP_HEIGHT>>m;
    const VkRenderPassBeginInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=p->renderPass,
        .framebuffer=p->atlasFramebuffer[m],.renderArea=quad_vk_rect(area)};
    vkCmdBeginRenderPass(p->command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport view={0,0,(float)w,(float)h,0,1};
    vkCmdSetViewport(p->command,0,1,&view);
    vkCmdBindPipeline(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipeline[pipeline]);
}

/* The image's linear set, or VK_NULL_HANDLE (the capture then uses the
 * exact per-pixel path). */
static VkDescriptorSet image_linear(BrookVkQuadPass *p,struct QuadImage *m)
{
    if(m->linear || !p->linearSources || m->modifier)return m->linear;
    const VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,
        .descriptorSetCount=1,.pSetLayouts=&p->setLayout};
    if(vkAllocateDescriptorSets(p->device,&ds,&m->linear)!=VK_SUCCESS)return m->linear=VK_NULL_HANDLE;
    const VkDescriptorImageInfo image={.sampler=p->linearSampler,.imageView=m->view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=m->linear,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
    vkUpdateDescriptorSets(p->device,1,&write,0,NULL);
    return m->linear;
}

/* Capture, downsample and upsample the tiles marked in rebuild (see
 * quad_backdrop.h). Tiles never overlap in the atlas, so the others keep
 * their texels. */
static void quad_capture_backdrop(BrookVkQuadPass *p,const uint32_t *scene,
        struct QuadImage *const *images,const struct QuadBackdrop *backdrop,const struct QuadRect *holes,
        const uint8_t *rebuild)
{
    unsigned deepest=0;
    struct QuadRect area[QUAD_BACKDROP_MIPS];
    for(unsigned m=0;m<QUAD_BACKDROP_MIPS;++m)area[m]=(struct QuadRect){INT32_MAX,INT32_MAX,0,0};
    VkClearRect clears[QUAD_BACKDROP_TILES];unsigned n=0;
    for(unsigned t=0;t<backdrop->count;++t)if(rebuild[t]){
        const struct QuadBackdropTile *tile=&backdrop->tiles[t];
        clears[n++]=(VkClearRect){quad_vk_rect(tile->atlas),0,1};
        if(tile->mips>deepest)deepest=tile->mips;
        for(unsigned m=0;m<=tile->mips;++m){
            const struct QuadRect r=atlas_level_rect(tile,m);struct QuadRect *u=&area[m];
            if(r.x0<u->x0)u->x0=r.x0;
            if(r.y0<u->y0)u->y0=r.y0;
            if(r.x1>u->x1)u->x1=r.x1;
            if(r.y1>u->y1)u->y1=r.y1;
        }
    }
    atlas_begin(p,0,QUAD_PIPELINE_CAPTURE,area[0]);
    {
        const VkClearAttachment clear={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.clearValue={.color={{0,0,0,0}}}};
        vkCmdClearAttachments(p->command,1,&clear,n,clears);
    }
    vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,1,1,&p->atlas.set,0,NULL);
    const float half[2]={2.0f/QUAD_BACKDROP_WIDTH,2.0f/QUAD_BACKDROP_HEIGHT};
    for(unsigned t=0;t<backdrop->count;++t){
        if(!rebuild[t])continue;
        const struct QuadBackdropTile *tile=&backdrop->tiles[t];
        const struct QuadRect c=tile->capture,a=tile->atlas;
        const int32_t block=1<<tile->base;
        const VkRect2D scissor=quad_vk_rect(a);
        vkCmdSetScissor(p->command,0,1,&scissor);
        for(unsigned i=quad_backdrop_bottom(scene,holes,tile);i<tile->below;++i){
            const uint32_t *l=quad_backdrop_layer(scene,i);
            const struct QuadRect r=quad_rect_intersect(quad_layer_bounds(l,scene[0],scene[1]),c);
            if(!images[i] || quad_rect_empty(r) || (holes && quad_rect_contains(holes[i],r)))continue;
            const struct QuadRect hole=holes?quad_rect_intersect(holes[i],r):(struct QuadRect){0};
            /* Every texel whose block touches the piece. */
            const struct QuadRect texels={a.x0+(r.x0-c.x0)/block,a.y0+(r.y0-c.y0)/block,
                a.x0+(r.x1-c.x0+block-1)/block,a.y0+(r.y1-c.y0+block-1)/block};
            struct QuadBackdropPush push={.quad=quad_layer_push(scene,l,texels,0),
                .origin={c.x0-a.x0*block,c.y0-a.y0*block},.limit={c.x1-1,c.y1-1},
                .clip0={r.x0,r.y0},.clip1={r.x1,r.y1},.block=(uint32_t)block};
            if(!quad_rect_empty(hole)){push.hole0[0]=hole.x0;push.hole0[1]=hole.y0;push.hole1[0]=hole.x1;push.hole1[1]=hole.y1;}
            push.quad.invHalf[0]=half[0];push.quad.invHalf[1]=half[1];
            /* Unscaled opaque or premultiplied layers: covered 2x2 blocks take
             * one bilinear tap (straight alpha would weight colour wrongly). */
            const VkDescriptorSet linear=block==2 && !fetch_mode(l) &&
                (!(l[16]&BROOK_VK_STRAIGHT_ALPHA) || (l[16]&BROOK_VK_OPAQUE_SOURCE))?image_linear(p,images[i]):VK_NULL_HANDLE;
            if(linear)push.quad.mode|=QUAD_MODE_BOX;
            quad_blend_barrier(p);
            const VkDescriptorSet sets[3]={images[i]->set,p->atlas.set,linear?linear:images[i]->set};
            vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,0,3,sets,0,NULL);
            vkCmdPushConstants(p->command,p->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
            vkCmdDraw(p->command,4,1,0,0);
        }
    }
    vkCmdEndRenderPass(p->command);
    /* Down to each tile's depth, then back up to the mip the glass samples. */
    for(unsigned pass=0;pass+QUAD_BACKDROP_FINAL<2*deepest;++pass){
        const int up=pass>=deepest;
        const unsigned m=up?2*deepest-1-pass:pass+1,from=up?m+1:m-1;
        atlas_barrier(p);
        atlas_begin(p,m,QUAD_PIPELINE_FILTER,area[m]);
        vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,0,1,&p->atlasSample[from],0,NULL);
        for(unsigned t=0;t<backdrop->count;++t){
            const struct QuadBackdropTile *tile=&backdrop->tiles[t];
            if(!rebuild[t] || (up?tile->mips<=m:tile->mips<m))continue;
            const struct QuadRect dst=atlas_level_rect(tile,m),src=atlas_level_rect(tile,from);
            const struct QuadBackdropPush push={.quad={.rect={dst.x0,dst.y0,dst.x1,dst.y1},
                    .invHalf={2.0f/(float)(QUAD_BACKDROP_WIDTH>>m),2.0f/(float)(QUAD_BACKDROP_HEIGHT>>m)},
                    .mode=up?QUAD_MODE_UP:0u},
                .origin={src.x0,src.y0},.limit={src.x1,src.y1},.clip0={dst.x0,dst.y0},
                .clip1={QUAD_BACKDROP_WIDTH>>from,QUAD_BACKDROP_HEIGHT>>from}};
            const VkRect2D scissor=quad_vk_rect(dst);
            vkCmdSetScissor(p->command,0,1,&scissor);
            vkCmdPushConstants(p->command,p->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
            vkCmdDraw(p->command,4,1,0,0);
        }
        vkCmdEndRenderPass(p->command);
    }
    const VkImageMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=p->atlas.image,.subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=QUAD_BACKDROP_MIPS,.layerCount=1}};
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,NULL,0,NULL,1,&ready);
}

static int atlas_built(const BrookVkQuadPass *p,uint64_t key)
{
    for(unsigned i=0;i<p->atlasBuiltCount;++i)if(p->atlasBuilt[i]==key)return 1;
    return 0;
}

static BrookVkResult quad_pass_submit(BrookVkQuadPass *,const uint32_t *,size_t,
        const struct BrookVkQuadSource *,size_t,const struct BrookVkQuadSource *,const struct QuadPlan *,
        const struct QuadBackdrop *,const struct QuadRect *);

BrookVkResult brook_vk_quad_pass_submit_backdrop(BrookVkQuadPass *p,const uint32_t *scene,size_t words,
        const struct BrookVkQuadSource *sources,size_t count,const struct BrookVkQuadSource *output,
        const struct QuadPlan *plan,const struct QuadBackdrop *backdrop,const struct QuadRect *holes)
{
    const BrookVkResult result=quad_pass_submit(p,scene,words,sources,count,output,plan,backdrop,holes);
    if(p)memset(p->sourceModifiers,0,sizeof(p->sourceModifiers));
    return result;
}
BrookVkResult brook_vk_quad_pass_submit(BrookVkQuadPass *p,const uint32_t *scene,size_t words,
        const struct BrookVkQuadSource *sources,size_t count,const struct BrookVkQuadSource *output,const struct QuadPlan *plan)
{
    return brook_vk_quad_pass_submit_backdrop(p,scene,words,sources,count,output,plan,NULL,NULL);
}

static BrookVkResult quad_pass_submit(BrookVkQuadPass *p,const uint32_t *scene,size_t words,
        const struct BrookVkQuadSource *sources,size_t count,const struct BrookVkQuadSource *output,
        const struct QuadPlan *plan,const struct QuadBackdrop *backdrop,const struct QuadRect *holes)
{
    if(!p || p->pending || p->uncertain || !scene || !sources || !output || !plan || !count || count>BROOK_VK_MAX_LAYERS ||
       words<BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS || scene[2]!=count || plan->width!=scene[0] || plan->height!=scene[1] ||
       !buffer_valid(&output->buffer) || output->buffer.syncFd!=-1 || output->buffer.offsetWords ||
       output->buffer.width!=scene[0] || output->buffer.height!=scene[1] || (!plan->drawCount && !plan->clearCount) ||
       plan->drawCount>QUAD_PLAN_MAX_DRAWS || plan->clearCount>QUAD_PLAN_MAX_CLEARS ||
       !rect_inside(plan->bounds,scene[0],scene[1]))return BROOK_VK_REJECTED;
    if(p->optimalOutput && (plan->regionCount!=1 || plan->bounds.x0 || plan->bounds.y0 ||
        (uint32_t)plan->bounds.x1!=scene[0] || (uint32_t)plan->bounds.y1!=scene[1]))return BROOK_VK_REJECTED;
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
    if(backdrop && backdrop->count && (!backdrop->key || backdrop->count>QUAD_BACKDROP_TILES))return BROOK_VK_REJECTED;
    const int glass=backdrop && backdrop->count;
    if(glass && !brook_vk_quad_pass_enable_backdrop(p))return BROOK_VK_REJECTED;
    /* Recapture only tiles whose texels are not already current. */
    uint8_t rebuildTile[QUAD_BACKDROP_TILES]={0};unsigned rebuildCount=0;
    for(unsigned t=0;glass && t<backdrop->count;++t)
        if(!atlas_built(p,backdrop->tiles[t].key)){rebuildTile[t]=1;++rebuildCount;}
    const int rebuild=rebuildCount!=0;
    if(rebuild)for(size_t i=0;i<count;++i)used[i]|=backdrop->used[i];
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
    /* Uncached target (zero-copy ring): no other render-target import may
     * stay alive in this device (see brook_vk_quad_pass_target_imports). */
    if(!output->cache)for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)
        if(p->cache[i].live && (p->cache[i].usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)){
            image_destroy(p,&p->cache[i]);++p->stats.targetEvictions;}
    struct QuadImage *target=p->optimalOutput?optimal_target(p,scene[0],scene[1]):
        image_get(p,output,0,&outputId,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    struct QuadImage *images[BROOK_VK_MAX_LAYERS]={0};
    if(!target || (p->optimalOutput && !import_transfer_buffer(p,&output->buffer,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       &p->frameOutput,&p->frameOutputMemory)))goto reject;
    for(size_t i=0;i<count;++i)if(used[i] && !(images[i]=image_get(p,&sources[i],p->sourceModifiers[i],&ids[i],VK_IMAGE_USAGE_SAMPLED_BIT)))goto reject;
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
        waits[p->waitCount]=semaphore;
        stages[p->waitCount++]=p->optimalOutput?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    if(vkResetCommandBuffer(p->command,0)!=VK_SUCCESS || vkResetFences(p->device,1,&p->fence)!=VK_SUCCESS)goto reject;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(vkBeginCommandBuffer(p->command,&begin)!=VK_SUCCESS)goto reject;
    p->gpuValid=0;p->atlasTimed=p->chromeTimed=p->timingOverflow=0;p->timestampNext=4;
    if(p->timestamps){vkCmdResetQueryPool(p->command,p->timestamps,0,QUAD_TIMESTAMP_COUNT);
        vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,p->timestamps,0);}
    /* Acquire every image from its external owner (the producer, the CPU
     * or the presenter). External memory images are created UNDEFINED; like
     * other dma-buf compositors, treat the external contents as GENERAL so
     * the acquire preserves them. */
    VkImageMemoryBarrier barriers[BROOK_VK_MAX_LAYERS+2];uint32_t barrierCount=0;
    struct QuadImage *distinct[BROOK_VK_MAX_LAYERS+1];uint32_t distinctCount=0;
    distinct[distinctCount++]=target;
    for(size_t i=0;i<count;++i)if(images[i]){int seen=0;
        for(uint32_t j=0;j<distinctCount;++j)if(distinct[j]==images[i])seen=1;
        if(!seen)distinct[distinctCount++]=images[i];}
    VkBufferMemoryBarrier uploads[BROOK_VK_MAX_LAYERS];
    unsigned uploadCount=0;
    for(uint32_t i=0;i<distinctCount;++i){struct QuadImage *m=distinct[i];const int out=m==target;
        const int upload=p->optimalOutput && !out;
        barriers[barrierCount++]=(VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=0,.dstAccessMask=out?VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
                VK_ACCESS_INPUT_ATTACHMENT_READ_BIT:upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT,
            .oldLayout=p->optimalOutput?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL,
            .newLayout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=p->optimalOutput?VK_QUEUE_FAMILY_IGNORED:VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex=p->optimalOutput?VK_QUEUE_FAMILY_IGNORED:p->family,.image=m->image,
            .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
        if(upload)uploads[uploadCount++]=(VkBufferMemoryBarrier){.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex=p->family,.buffer=m->upload,.size=VK_WHOLE_SIZE};
    }
    if(rebuild && !p->atlasInitialized)barriers[barrierCount++]=(VkImageMemoryBarrier){
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=p->atlas.image,.subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=QUAD_BACKDROP_MIPS,.layerCount=1}};
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT};
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
            (uploadCount?VK_PIPELINE_STAGE_TRANSFER_BIT:0),0,1,&host,uploadCount,uploads,barrierCount,barriers);
    for(uint32_t i=0;i<uploadCount;++i){
        const struct QuadImage *m=distinct[i+1];
        const VkBufferImageCopy copy={.bufferOffset=(uint64_t)m->offsetWords*4u,.bufferRowLength=m->strideWords,
            .imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.imageExtent={m->width,m->height,1}};
        vkCmdCopyBufferToImage(p->command,m->upload,m->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        uploads[i].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        uploads[i].dstAccessMask=0;
        uploads[i].srcQueueFamilyIndex=p->family;
        uploads[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
        barriers[i+1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i+1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barriers[i+1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i+1].newLayout=VK_IMAGE_LAYOUT_GENERAL;
    }
    if(uploadCount){
        vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,0,NULL,0,NULL,uploadCount,barriers+1);
        vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,0,NULL,uploadCount,uploads,0,NULL);
    }
    if(rebuild){
        if(p->timestamps)vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timestamps,2);
        quad_capture_backdrop(p,scene,images,backdrop,holes,rebuildTile);
        if(p->timestamps)vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timestamps,3);
        p->atlasTimed=1;
    }
    const struct QuadRect bounds=plan->bounds;
    VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=p->renderPass,.framebuffer=target->framebuffer,
        .renderArea={{bounds.x0,bounds.y0},{(uint32_t)(bounds.x1-bounds.x0),(uint32_t)(bounds.y1-bounds.y0)}}};
    vkCmdBeginRenderPass(p->command,&render,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport={0,0,(float)scene[0],(float)scene[1],0,1};
    vkCmdSetViewport(p->command,0,1,&viewport);vkCmdSetScissor(p->command,0,1,&render.renderArea);
    if(p->optimalOutput || plan->clearCount){
        VkClearAttachment clear={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=0,.clearValue={.color={{0,0,0,0}}}};
        VkClearRect rects[QUAD_PLAN_MAX_CLEARS];
        for(uint32_t i=0;i<plan->clearCount;++i){const struct QuadRect r=plan->clears[i];
            rects[i]=(VkClearRect){{{r.x0,r.y0},{(uint32_t)(r.x1-r.x0),(uint32_t)(r.y1-r.y0)}},0,1};}
        if(p->optimalOutput)rects[0]=(VkClearRect){render.renderArea,0,1};
        vkCmdClearAttachments(p->command,1,&clear,p->optimalOutput?1u:plan->clearCount,rects);
    }
    vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,1,1,&target->set,0,NULL);
    if(glass)vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,2,1,&p->atlasSample[QUAD_BACKDROP_FINAL],0,NULL);
    int boundPipeline=-1,boundLayer=-1,written=p->optimalOutput || plan->clearCount!=0;
    for(uint32_t i=0;i<plan->drawCount;++i){
        const struct QuadDraw *d=&plan->draws[i];
        const uint32_t *l=quad_backdrop_layer(scene,d->layer);
        const int blend=d->kind==QUAD_KIND_STRAIGHT || d->kind==QUAD_KIND_PREMULTIPLIED;
        struct QuadRect ordinary[QUAD_PLAN_MAX_PIECES]={d->rect};
        struct QuadRect effects[QUAD_BACKDROP_TILES];
        const struct QuadBackdropTile *tiles[QUAD_BACKDROP_TILES];
        unsigned n=1,effectCount=0;
        for(unsigned t=0;glass && t<backdrop->count;++t){
            const struct QuadBackdropTile *tile=&backdrop->tiles[t];
            if(tile->layer!=d->layer)continue;
            const struct QuadRect overlap=quad_rect_intersect(tile->screen,d->rect);
            if(quad_rect_empty(overlap))continue;
            if(!quad_subtract(ordinary,&n,overlap)){ordinary[0]=d->rect;n=1;effectCount=0;break;}
            effects[effectCount]=overlap;tiles[effectCount++]=tile;
        }
        if((int)d->layer!=boundLayer){
            if(written && (effectCount || (blend && (!p->fixedBlend || glass))))quad_blend_barrier(p);
            vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,0,1,&images[d->layer]->set,0,NULL);
            boundLayer=d->layer;
        }
        if(effectCount){
            if(boundPipeline!=QUAD_PIPELINE_GLASS)vkCmdBindPipeline(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipeline[QUAD_PIPELINE_GLASS]);
            boundPipeline=QUAD_PIPELINE_GLASS;
            const int timing=quad_chrome_begin(p,QUAD_GPU_GLASS);
            for(unsigned t=0;t<effectCount;++t)quad_draw_rect(p,scene,l,effects[t],d->kind,tiles[t]);
            quad_chrome_end(p,timing);
        }
        if(n){
            const int pipeline=!p->fixedBlend || !blend?QUAD_PIPELINE_NONE:d->kind==QUAD_KIND_STRAIGHT?
                QUAD_PIPELINE_STRAIGHT:QUAD_PIPELINE_PREMULTIPLIED;
            if(pipeline!=boundPipeline){vkCmdBindPipeline(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipeline[pipeline]);boundPipeline=pipeline;}
            const unsigned category=backdrop?backdrop->chromeClass[d->layer]:0;
            const int timing=category && !p->timingBlurOnly?quad_chrome_begin(p,category):0;
            for(unsigned k=0;k<n;++k)quad_draw_rect(p,scene,l,ordinary[k],d->kind,NULL);
            quad_chrome_end(p,timing);
        }
        written=1;
    }
    vkCmdEndRenderPass(p->command);
    if(p->optimalOutput){
        VkImageMemoryBarrier color=barriers[0];
        color.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        color.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        color.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
        color.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferMemoryBarrier transfer={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=p->family,.buffer=p->frameOutput,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,1,&transfer,1,&color);
        const VkBufferImageCopy copy={.bufferRowLength=output->buffer.strideWords,
            .imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.imageExtent={scene[0],scene[1],1}};
        vkCmdCopyImageToBuffer(p->command,target->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,p->frameOutput,1,&copy);
        transfer.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        transfer.dstAccessMask=0;
        transfer.srcQueueFamilyIndex=p->family;
        transfer.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
        vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,0,NULL,1,&transfer,0,NULL);
    }
    const uint32_t first=p->optimalOutput?distinctCount:0u;
    for(uint32_t i=first;i<distinctCount;++i){const int out=distinct[i]==target;
        barriers[i].srcAccessMask=out?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:0;barriers[i].dstAccessMask=0;
        barriers[i].oldLayout=VK_IMAGE_LAYOUT_GENERAL;barriers[i].newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex=p->family;barriers[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;}
    if(distinctCount>first)
        vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,distinctCount-first,barriers+first);
    if(p->timestamps)vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timestamps,1);
    if(vkEndCommandBuffer(p->command)!=VK_SUCCESS)goto reject;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=p->waitCount,.pWaitSemaphores=waits,
        .pWaitDstStageMask=stages,.commandBufferCount=1,.pCommandBuffers=&p->command};
    {const VkResult submitted=vkQueueSubmit(p->queue,1,&submit,p->fence);
     if(submitted!=VK_SUCCESS){p->lastResult=submitted;p->uncertain=1;return BROOK_VK_UNCERTAIN;}}
    if(glass){
        if(rebuild)++p->backdropStats.rebuilds;else ++p->backdropStats.reuses;
        p->backdropStats.tileRebuilds+=rebuildCount;p->backdropStats.tileReuses+=backdrop->count-rebuildCount;
        p->atlasInitialized|=rebuild;
        p->atlasBuiltCount=backdrop->count;
        for(unsigned t=0;t<backdrop->count;++t)p->atlasBuilt[t]=backdrop->tiles[t].key;
    }
    ++p->stats.submits;p->pending=1;return BROOK_VK_PENDING;
reject:
    /* Cached imports stay valid for the next submission. */
    frame_cleanup(p);return BROOK_VK_REJECTED;
}

static BrookVkResult retire(BrookVkQuadPass *p,VkResult status){
    if(status==VK_NOT_READY || status==VK_TIMEOUT)return BROOK_VK_PENDING;
    p->pending=0;
    if(status!=VK_SUCCESS){p->lastResult=status;p->uncertain=1;return BROOK_VK_UNCERTAIN;}
    if(p->timestamps){
        uint64_t ticks[2]={0};
        if(vkGetQueryPoolResults(p->device,p->timestamps,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
            const uint64_t mask=p->timestampBits==64?UINT64_MAX:(UINT64_C(1)<<p->timestampBits)-1;
            const double ns=(double)((ticks[1]-ticks[0])&mask)*p->timestampPeriod;
            if(ns>=0 && ns<(double)UINT64_MAX){p->gpuNs=(uint64_t)ns;p->gpuValid=1;}
        }
    }
    if(p->timestamps && (p->chromeTimed || p->atlasTimed) && !p->timingOverflow){
        uint64_t ticks[QUAD_TIMESTAMP_COUNT]={0};
        const uint64_t mask=p->timestampBits==64?UINT64_MAX:(UINT64_C(1)<<p->timestampBits)-1;
        const int atlasValid=!p->atlasTimed || vkGetQueryPoolResults(p->device,p->timestamps,2,2,2*sizeof(uint64_t),
            ticks+2,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS;
        const unsigned draws=p->timestampNext-4;
        const int glassValid=!draws || vkGetQueryPoolResults(p->device,p->timestamps,4,draws,draws*sizeof(uint64_t),
            ticks+4,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS;
        if(atlasValid && glassValid){
            uint64_t sums[QUAD_GPU_CLASSES]={0};
            sums[QUAD_GPU_ATLAS]=(uint64_t)((double)((ticks[3]-ticks[2])&mask)*p->timestampPeriod);
            for(unsigned i=4;i<p->timestampNext;i+=2){
                const unsigned category=p->timestampClass[i/2];
                if(category && category<QUAD_GPU_CLASSES)
                    sums[category]+=(uint64_t)((double)((ticks[i+1]-ticks[i])&mask)*p->timestampPeriod);
            }
            p->backdropStats.atlasNs+=sums[QUAD_GPU_ATLAS];p->backdropStats.glassNs+=sums[QUAD_GPU_GLASS];
            p->backdropStats.atlasTimedFrames+=p->atlasTimed;
            p->backdropStats.frameNs+=sums[QUAD_GPU_FRAME];p->backdropStats.shellNs+=sums[QUAD_GPU_SHELL];
            ++p->backdropStats.timedFrames;
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
int brook_vk_quad_pass_last_result(const BrookVkQuadPass *p){return p?(int)p->lastResult:0;}
unsigned brook_vk_quad_pass_target_imports(const BrookVkQuadPass *p,uint64_t *inodes,unsigned max){
    unsigned n=0;if(!p)return 0;
    for(unsigned i=0;i<QUAD_CACHE_SLOTS;++i)if(p->cache[i].live && (p->cache[i].usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)){
        if(inodes && n<max)inodes[n]=(uint64_t)p->cache[i].ino;
        ++n;}
    for(unsigned i=0;i<p->frameCount;++i)if(p->frame[i].live && (p->frame[i].usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)){
        if(inodes && n<max)inodes[n]=(uint64_t)p->frame[i].ino;
        ++n;}
    return n;
}
int brook_vk_quad_pass_gpu_timestamp_ns(const BrookVkQuadPass *p,uint64_t *ns){
    if(!p || !ns || !p->gpuValid)return 0;
    *ns=p->gpuNs;return 1;
}
struct BrookVkQuadStats brook_vk_quad_pass_stats(const BrookVkQuadPass *p){
    return p?p->stats:(struct BrookVkQuadStats){0};
}

struct BrookVkBackdropStats brook_vk_quad_pass_backdrop_stats(const BrookVkQuadPass *p){
    return p?p->backdropStats:(struct BrookVkBackdropStats){0};
}
