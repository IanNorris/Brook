/* Sample a second independently imported GEM image into the shared target. */
#include "external_sample_spv.h"
static int traceFirstSample;
static int sampleGeometry;
static int sampleGraphicsPass;
static int sampleGraphicsFence;
static int sampleGraphicsMultilayer;
static int sampleGraphicsRegions;
static uint32_t sampleGraphicsFamily;
static struct BrookVkGraphicsBuffer sampleGraphicsOutput={.fd=-1,.syncFd=-1};
static int sampleGraphicsDrm=-1;
static uint32_t sampleGraphicsOutputHandle;
static void *sampleGraphicsMap;
static uint64_t sample_tsc(void){uint32_t lo,hi;__asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");return ((uint64_t)hi<<32)|lo;}
static BrookVkResult sample_graphics_submit(BrookVkGraphicsPass *pass,
        const struct BrookVkGraphicsBuffer *bg,const struct BrookVkGraphicsBuffer *fg,
        const struct BrookVkGraphicsBuffer *overlay,const struct BrookVkGraphicsBuffer *out,
        const struct BrookVkGraphicsLayer *layer)
{
    if(!overlay && !sampleGraphicsRegions)return brook_vk_graphics_pass_submit(pass,bg,fg,out,layer);
    const size_t count=overlay?3:2;
    uint32_t scene[BROOK_VK_SCENE_HEADER+3*BROOK_VK_LAYER_WORDS]={out->width,out->height,(uint32_t)count,0};
    struct BrookVkSceneLayer base={bg->offsetWords,bg->strideWords,bg->width,bg->height,0,0,bg->width,bg->height,
        0,0,bg->width,bg->height,0,0,bg->width,bg->height,255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE};
    struct BrookVkSceneLayer front={fg->offsetWords,fg->strideWords,fg->width,fg->height,
        layer->sourceX,layer->sourceY,layer->sourceWidth,layer->sourceHeight,layer->x,layer->y,layer->width,layer->height,
        layer->clipX,layer->clipY,layer->clipWidth,layer->clipHeight,layer->opacity|(layer->premultiplied?0:BROOK_VK_STRAIGHT_ALPHA)};
    memcpy(scene+BROOK_VK_SCENE_HEADER,&base,sizeof(base));
    memcpy(scene+BROOK_VK_SCENE_HEADER+BROOK_VK_LAYER_WORDS,&front,sizeof(front));
    struct BrookVkGraphicsBuffer sources[3]={*bg,*fg,{.fd=-1,.syncFd=-1}};
    if(overlay){
        struct BrookVkSceneLayer top={overlay->offsetWords,overlay->strideWords,overlay->width,overlay->height,
            0,0,overlay->width,overlay->height,0,0,out->width,out->height,8,8,out->width-16,out->height-16,83|BROOK_VK_STRAIGHT_ALPHA};
        memcpy(scene+BROOK_VK_SCENE_HEADER+2*BROOK_VK_LAYER_WORDS,&top,sizeof(top));sources[2]=*overlay;
    }
    if(sampleGraphicsRegions){
        const struct BrookVkGraphicsRegion regions[2]={{2,3,out->width/3,out->height/3},
            {out->width/2,out->height/2,out->width/3,out->height/3}};
        struct BrookVkGraphicsRegion invalid={out->width,0,1,1};
        if(brook_vk_graphics_pass_submit_regions(pass,scene,BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS,sources,count,out,&invalid,1)!=BROOK_VK_REJECTED){
            fprintf(stderr,"GRAPHICS_PASS_REGION_REJECTION_UNCERTAIN\n");for(;;)pause();}
        return brook_vk_graphics_pass_submit_regions(pass,scene,BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS,sources,count,out,regions,2);
    }
    return brook_vk_graphics_pass_submit_scene(pass,scene,BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS,sources,count,out);
}
static int sample_imported(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                           VkRenderPass pass,VkFramebuffer framebuffer,VkCommandPool pool,
                           VkCommandBuffer command,VkFence fence,unsigned width,unsigned height,
                           unsigned OutWidth,unsigned OutHeight,int gpuSource,int blendSource,int xrgbSource,int clearOnly,
                           VkImage protectedImage,VkBuffer protectedReadback,int protectedFd,long *protectedTicket)
{
    if(!width || !height || width>OutWidth || height>OutHeight)return 0;
    int passed=0,drm=-1,fd=-1;void *mapped=MAP_FAILED;size_t mapSize=0;
    struct gem_create gem={0};VkImage source=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    struct SampleBackground bg={0},overlay={0};int overlayFd=-1;
    VkImageView view=VK_NULL_HANDLE;VkSampler sampler=VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl=VK_NULL_HANDLE;VkDescriptorPool dp=VK_NULL_HANDLE;
    VkPipelineLayout pl=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkQueryPool timestamps=VK_NULL_HANDLE;uint32_t timestampBits=0;
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
    uint32_t queueCount=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,NULL);
    if(queueCount && queueCount<=32){
        VkQueueFamilyProperties families[32];vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,families);
        for(uint32_t i=0;i<queueCount;++i)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){timestampBits=families[i].timestampValidBits;break;}
    }
    VkSemaphore ready=VK_NULL_HANDLE;VkCommandBuffer producer=VK_NULL_HANDLE;
    struct gem_create batch={0};struct intel_context context={0};struct intel_sync sync={0};
    uint32_t *batchMap=MAP_FAILED;int syncFd=-1,producerOutstanding=0;
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|(gpuSource==1?VK_IMAGE_USAGE_TRANSFER_DST_BIT:0),
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED};
    VKCHECK(vkCreateImage(device,&ii,NULL,&source));
    VkMemoryRequirements needs;vkGetImageMemoryRequirements(device,source,&needs);
    VkImageSubresource sub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device,source,&sub,&layout);
    CHECK(needs.size>0 && needs.size<=64*1024*1024 && layout.rowPitch>=width*4 &&
          layout.offset+(height-1)*layout.rowPitch+width*4<=needs.size);
    drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    if(blendSource>=3)CHECK(sample_background_create(physical,device,drm,OutWidth,OutHeight,&bg));
    gem.size=needs.size;
    /* Match the sampled Quake producer's 4 MiB allocation, which is larger
     * than the linear image's minimum requirement. */
    if(gpuSource==2 && width==1280 && height==720 && gem.size<4u*1024u*1024u)gem.size=4u*1024u*1024u;
    CHECK(!(layout.offset&3u) && !(layout.rowPitch&3u) && layout.offset/4u<=UINT32_MAX &&
          layout.rowPitch/4u<=UINT32_MAX &&
          brook_vk_graphics_linear_layout_valid(width,height,(uint32_t)(layout.offset/4u),
              (uint32_t)(layout.rowPitch/4u),gem.size,layout.offset,layout.rowPitch,needs.size));
    CHECK(!ioctl(drm,IOC(0x5b,sizeof(gem)),&gem));mapSize=(size_t)gem.size;
    struct map_offset mo={.handle=gem.handle,.flags=2};CHECK(!ioctl(drm,IOC(0x64,sizeof(mo)),&mo));
    mapped=mmap(NULL,mapSize,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)mo.offset);CHECK(mapped!=MAP_FAILED);
    if(gpuSource!=1){
        struct gem_domain domain={.handle=gem.handle,.read=1,.write=1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(domain)),&domain));
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            const uint32_t alpha=(blendSource||xrgbSource)?((x*17u+y*13u)&255u):255u;
            uint32_t r=x&255u,g=y&255u,b=(x^y)&255u;
            if(blendSource==4){r=(r*alpha+127u)/255u;g=(g*alpha+127u)/255u;b=(b*alpha+127u)/255u;}
            uint32_t pixel=(alpha<<24)|(r<<16)|(g<<8)|b;
            memcpy((uint8_t*)mapped+layout.offset+y*layout.rowPitch+x*4,&pixel,4);
        }
    }
    struct prime prime={.handle=gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
    CHECK(!ioctl(drm,IOC(0x2d,sizeof(prime)),&prime));fd=prime.fd;
    vkDestroyImage(device,source,NULL);source=VK_NULL_HANDLE;
    struct BrookVkGraphicsImage imported={0};
    CHECK(brook_vk_graphics_image_import(physical,device,fd,width,height,(uint32_t)(layout.offset/4u),
        (uint32_t)(layout.rowPitch/4u),gem.size,ii.usage,&imported));
    source=imported.image;memory=imported.memory;view=imported.view;
    VkImageSubresourceRange range={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1};
    if(sampleGraphicsPass && blendSource>=3){
        struct prime backgroundPrime={.handle=bg.gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
        CHECK(!ioctl(drm,IOC(0x2d,sizeof(backgroundPrime)),&backgroundPrime));
        int bgfd=backgroundPrime.fd;
        VkImageSubresource backgroundSub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout backgroundLayout;
        vkGetImageSubresourceLayout(device,bg.image,&backgroundSub,&backgroundLayout);
        struct BrookVkGraphicsBuffer background={bgfd,-1,OutWidth,OutHeight,(uint32_t)(backgroundLayout.offset/4u),
            (uint32_t)(backgroundLayout.rowPitch/4u),bg.size};
        struct BrookVkGraphicsBuffer foreground={fd,-1,width,height,(uint32_t)(layout.offset/4u),
            (uint32_t)(layout.rowPitch/4u),gem.size};
        struct BrookVkGraphicsBuffer third={.fd=-1,.syncFd=-1};
        if(sampleGraphicsMultilayer){
            CHECK(sample_background_create(physical,device,drm,OutWidth,OutHeight,&overlay));
            struct prime op={.handle=overlay.gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
            CHECK(!ioctl(drm,IOC(0x2d,sizeof(op)),&op));overlayFd=op.fd;
            VkSubresourceLayout ol;vkGetImageSubresourceLayout(device,overlay.image,&backgroundSub,&ol);
            third=(struct BrookVkGraphicsBuffer){overlayFd,-1,OutWidth,OutHeight,(uint32_t)(ol.offset/4u),(uint32_t)(ol.rowPitch/4u),overlay.size};
        }
        struct BrookVkGraphicsLayer layer={173,blendSource==4,0,0,OutWidth,OutHeight,0,0,OutWidth,OutHeight,0,0,width,height};
        if(sampleGeometry){layer.x=-3;layer.y=7;layer.width=OutWidth-2;layer.height=OutHeight-14;
            layer.clipX=4;layer.clipY=5;layer.clipWidth=OutWidth-9;layer.clipHeight=OutHeight-8;
            layer.sourceX=2;layer.sourceY=3;layer.sourceWidth=width-4;layer.sourceHeight=height-6;}
        BrookVkGraphicsPass *passState=brook_vk_graphics_pass_create(physical,device,queue,sampleGraphicsFamily);
        if(!passState){close(bgfd);goto cleanup;}
        struct BrookVkGraphicsBuffer aliased=sampleGraphicsOutput;aliased.fd=foreground.fd;
        if(brook_vk_graphics_pass_submit(passState,&background,&foreground,&aliased,&layer)!=BROOK_VK_REJECTED){
            fprintf(stderr,"GRAPHICS_PASS_ALIAS_UNCERTAIN\n");for(;;)pause();}
        struct BrookVkGraphicsLayer invalid=layer;invalid.sourceX=UINT32_MAX;
        if(brook_vk_graphics_pass_submit(passState,&background,&foreground,&sampleGraphicsOutput,&invalid)!=BROOK_VK_REJECTED){
            fprintf(stderr,"GRAPHICS_PASS_INVALID_UNCERTAIN\n");for(;;)pause();}
        if(sampleGraphicsFence && blendSource==3){
            CHECK(!ioctl(drm,IOC(0x6d,sizeof(context)),&context) && context.id);
            batch.size=4096;CHECK(!ioctl(drm,IOC(0x5b,sizeof(batch)),&batch));
            struct map_offset bm={.handle=batch.handle,.flags=2};CHECK(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
            batchMap=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);CHECK(batchMap!=MAP_FAILED);
            CHECK(!ioctl(drm,IOC(0xbf,sizeof(sync)),&sync) && sync.handle);
            batchMap[0]=0x10000002;batchMap[1]=0x40011000;batchMap[2]=0;batchMap[3]=0xff13579b;
            batchMap[4]=0x05000000;batchMap[5]=0;
            struct intel_object objects[]={{gem.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
            struct intel_exec_fence signal={sync.handle,2};
            struct intel_exec request={(uintptr_t)objects,2,0,24,0,0,1,(uintptr_t)&signal,(1u<<11)|(1u<<19),context.id,0};
            if(ioctl(drm,IOC(0x69,sizeof(request)),&request)){fprintf(stderr,"GRAPHICS_PASS_PRODUCER_UNCERTAIN\n");for(;;)pause();}
            producerOutstanding=1;
            struct intel_sync_handle exported={sync.handle,1,-1,0};
            if(ioctl(drm,IOC(0xc1,sizeof(exported)),&exported) || exported.fd<0){fprintf(stderr,"GRAPHICS_PASS_FENCE_UNCERTAIN\n");for(;;)pause();}
            syncFd=exported.fd;struct pollfd pollfd={.fd=syncFd,.events=POLLIN};int ready=poll(&pollfd,1,0);
            CHECK(ready>=0);fprintf(stderr,"GRAPHICS_PASS_SOURCE_FENCE ready=%d revents=%d\n",ready,pollfd.revents);
            foreground.syncFd=syncFd;
        }
        BrookVkResult status=sample_graphics_submit(passState,&background,&foreground,sampleGraphicsMultilayer?&third:NULL,&sampleGraphicsOutput,&layer);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"GRAPHICS_PASS_UNCERTAIN submit\n");for(;;)pause();}
        if(status!=BROOK_VK_PENDING){brook_vk_graphics_pass_destroy(passState);close(bgfd);goto cleanup;}
        if(brook_vk_graphics_pass_submit(passState,&background,&foreground,&sampleGraphicsOutput,&layer)!=BROOK_VK_REJECTED){
            fprintf(stderr,"GRAPHICS_PASS_REENTRY_UNCERTAIN\n");for(;;)pause();}
        if(brook_vk_graphics_pass_destroy(passState)){fprintf(stderr,"GRAPHICS_PASS_PENDING_DESTROY_UNSAFE\n");for(;;)pause();}
        for(unsigned i=0;i<10000 && status==BROOK_VK_PENDING;++i){usleep(1000);status=brook_vk_graphics_pass_poll(passState);}
        if(status!=BROOK_VK_COMPLETE){fprintf(stderr,"GRAPHICS_PASS_UNCERTAIN wait=%d\n",status);for(;;)pause();}
        producerOutstanding=0;
        if(sampleGraphicsFence && blendSource==3){
            struct gem_domain outputDomain={.handle=sampleGraphicsOutputHandle,.read=1,.write=0};
            if(ioctl(sampleGraphicsDrm,IOC(0x5f,sizeof(outputDomain)),&outputDomain)){
                brook_vk_graphics_pass_destroy(passState);close(bgfd);goto cleanup;}
            for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x){
                const uint32_t br=(x*3u+17u)&255u,bgcolor=(y*5u+23u)&255u,bb=(x*7u+y*11u)&255u;
                const uint32_t r=(19u*173u+br*82u+127u)/255u,g=(87u*173u+bgcolor*82u+127u)/255u;
                const uint32_t b=(155u*173u+bb*82u+127u)/255u,expected=0xff000000u|(r<<16)|(g<<8)|b;
                uint32_t pixel;memcpy(&pixel,(const uint8_t*)sampleGraphicsMap+
                    (sampleGraphicsOutput.offsetWords+(uint64_t)y*sampleGraphicsOutput.strideWords+x)*4u,4);
                if(pixel!=expected){fprintf(stderr,"GRAPHICS_PASS_FIRST_FRAME_FAIL x=%u y=%u got=%08x expected=%08x\n",x,y,pixel,expected);
                    brook_vk_graphics_pass_destroy(passState);close(bgfd);goto cleanup;}
            }
            puts("GRAPHICS_PASS_FIRST_PENDING_FENCE_PIXELS_PASS");
        }
        foreground.syncFd=-1;
        status=sample_graphics_submit(passState,&background,&foreground,sampleGraphicsMultilayer?&third:NULL,&sampleGraphicsOutput,&layer);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"GRAPHICS_PASS_UNCERTAIN resubmit\n");for(;;)pause();}
        if(status!=BROOK_VK_PENDING){brook_vk_graphics_pass_destroy(passState);close(bgfd);goto cleanup;}
        for(unsigned i=0;i<10000 && status==BROOK_VK_PENDING;++i){usleep(1000);status=brook_vk_graphics_pass_poll(passState);}
        if(status!=BROOK_VK_COMPLETE){fprintf(stderr,"GRAPHICS_PASS_UNCERTAIN reuse=%d\n",status);for(;;)pause();}
        if(!brook_vk_graphics_pass_destroy(passState)){fprintf(stderr,"GRAPHICS_PASS_UNCERTAIN destroy\n");for(;;)pause();}
        close(bgfd);
        puts("GRAPHICS_PASS_ASYNC_REUSE_COMPLETE");passed=1;goto cleanup;
    }
    VkSamplerCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,
        .minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};
    VKCHECK(vkCreateSampler(device,&sci,NULL,&sampler));
    VkDescriptorSetLayoutBinding binding[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo dli={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=blendSource>=3?2:1,.pBindings=binding};
    VKCHECK(vkCreateDescriptorSetLayout(device,&dli,NULL,&dsl));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,blendSource>=3?2u:1u};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VKCHECK(vkCreateDescriptorPool(device,&dpi,NULL,&dp));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,
        .descriptorSetCount=1,.pSetLayouts=&dsl};VkDescriptorSet set;VKCHECK(vkAllocateDescriptorSets(device,&dai,&set));
    VkDescriptorImageInfo descriptor={.sampler=sampler,.imageView=view,.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&descriptor};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    if(blendSource>=3){
        VkDescriptorImageInfo background={.sampler=sampler,.imageView=bg.view,.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        write.dstBinding=1;write.pImageInfo=&background;vkUpdateDescriptorSets(device,1,&write,0,NULL);
    }
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT,
        .size=blendSource==2?4*sizeof(uint32_t):sizeof(struct BrookVkGraphicsLayer)};
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl,
        .pushConstantRangeCount=blendSource>=2?1:0,.pPushConstantRanges=blendSource>=2?&push:NULL};
    VKCHECK(vkCreatePipelineLayout(device,&pli,NULL,&pl));
    VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(sample_vertexCode),.pCode=sample_vertexCode};
    VKCHECK(vkCreateShaderModule(device,&si,NULL,&vertex));
    si.codeSize=blendSource>=3?sizeof(sample_layersCode):blendSource==2?sizeof(sample_exactCode):xrgbSource?sizeof(sample_xrgbCode):sizeof(sample_fragmentCode);
    si.pCode=blendSource>=3?sample_layersCode:blendSource==2?sample_exactCode:xrgbSource?sample_xrgbCode:sample_fragmentCode;
    VKCHECK(vkCreateShaderModule(device,&si,NULL,&fragment));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,OutWidth,OutHeight,0,1};VkRect2D scissor={{0,0},{OutWidth,OutHeight}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.blendEnable=blendSource==1?VK_TRUE:VK_FALSE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp=VK_BLEND_OP_ADD,.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,.alphaBlendOp=VK_BLEND_OP_ADD,.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vin,.pInputAssemblyState=&assembly,.pViewportState=&vp,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pl,.renderPass=pass};
    VKCHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,NULL,&pipeline));
    if(timestampBits && properties.limits.timestampComputeAndGraphics){
        VkQueryPoolCreateInfo q={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        VKCHECK(vkCreateQueryPool(device,&q,NULL,&timestamps));
    }
    VKCHECK(vkResetCommandBuffer(command,0));VKCHECK(vkResetFences(device,1,&fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKCHECK(vkBeginCommandBuffer(command,&begin));
    if(timestamps){vkCmdResetQueryPool(command,timestamps,0,2);vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps,0);}
    if(gpuSource!=1){
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_PREINITIALIZED,
            .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=source,.subresourceRange=range};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
    }
    if(blendSource>=3){
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_PREINITIALIZED,
            .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=bg.image,.subresourceRange=range};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
    }
    VkClearValue clear={.color={{0,0,0,1}}};
    if(blendSource){clear.color.float32[0]=32.0f/255.0f;clear.color.float32[1]=64.0f/255.0f;clear.color.float32[2]=96.0f/255.0f;}
    VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,
        .renderArea={{0,0},{OutWidth,OutHeight}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(command,&rbi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&set,0,NULL);
    if(blendSource==2){const uint32_t bg[4]={32,64,96,255};vkCmdPushConstants(command,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(bg),bg);}
    if(blendSource>=3){
        struct BrookVkGraphicsLayer layer={173,blendSource==4,0,0,OutWidth,OutHeight,0,0,OutWidth,OutHeight,0,0,width,height};
        if(sampleGeometry){
            CHECK(width>4 && height>6 && OutWidth>9 && OutHeight>14);
            layer.x=-3;layer.y=7;layer.width=OutWidth-2;layer.height=OutHeight-14;
            layer.clipX=4;layer.clipY=5;layer.clipWidth=OutWidth-9;layer.clipHeight=OutHeight-8;
            layer.sourceX=2;layer.sourceY=3;layer.sourceWidth=width-4;layer.sourceHeight=height-6;
        }
        CHECK(brook_vk_graphics_layer_valid(&layer,OutWidth,OutHeight,width,height));
        vkCmdPushConstants(command,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(layer),&layer);
    }
    if(!clearOnly)vkCmdDraw(command,3,1,0,0);
    vkCmdEndRenderPass(command);
    if(protectedReadback){
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={OutWidth,OutHeight,1}};
        vkCmdCopyImageToBuffer(command,protectedImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,protectedReadback,1,&copy);
        VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    }
    if(timestamps)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,1);
    VKCHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo producerSubmit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if(gpuSource==1){
        VkSemaphoreCreateInfo semaphore={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(device,&semaphore,NULL,&ready));
        VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
            .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VKCHECK(vkAllocateCommandBuffers(device,&alloc,&producer));VKCHECK(vkBeginCommandBuffer(producer,&begin));
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_PREINITIALIZED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=source,.subresourceRange=range};
        vkCmdPipelineBarrier(producer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearColorValue color={.float32={51.0f/255.0f,102.0f/255.0f,153.0f/255.0f,1.0f}};
        vkCmdClearColorImage(producer,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(producer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VKCHECK(vkEndCommandBuffer(producer));
        producerSubmit.commandBufferCount=1;producerSubmit.pCommandBuffers=&producer;
        producerSubmit.signalSemaphoreCount=1;producerSubmit.pSignalSemaphores=&ready;
        submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&ready;submit.pWaitDstStageMask=&waitStage;
    }
    if(gpuSource==2){
        VkSemaphoreCreateInfo semaphore={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(device,&semaphore,NULL,&ready));
        CHECK(!ioctl(drm,IOC(0x6d,sizeof(context)),&context) && context.id);
        batch.size=4096;CHECK(!ioctl(drm,IOC(0x5b,sizeof(batch)),&batch));
        struct map_offset bm={.handle=batch.handle,.flags=2};CHECK(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
        batchMap=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);CHECK(batchMap!=MAP_FAILED);
        CHECK(!ioctl(drm,IOC(0xbf,sizeof(sync)),&sync) && sync.handle);
        batchMap[0]=0x10000002;batchMap[1]=0x40011000;batchMap[2]=0;batchMap[3]=0xff13579b;
        batchMap[4]=0x05000000;batchMap[5]=0;
        struct intel_object objects[]={{gem.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
        struct intel_exec_fence signal={sync.handle,2};
        struct intel_exec request={(uintptr_t)objects,2,0,24,0,0,1,(uintptr_t)&signal,(1u<<11)|(1u<<19),context.id,0};
        if(ioctl(drm,IOC(0x69,sizeof(request)),&request)){fprintf(stderr,"EXTERNAL_IMAGE_PRODUCER_UNCERTAIN\n");for(;;)pause();}
        producerOutstanding=1;
        struct intel_sync_handle exported={sync.handle,1,-1,0};
        if(ioctl(drm,IOC(0xc1,sizeof(exported)),&exported) || exported.fd<0){fprintf(stderr,"EXTERNAL_IMAGE_FENCE_UNCERTAIN\n");for(;;)pause();}
        syncFd=exported.fd;struct pollfd p={.fd=syncFd,.events=POLLIN};
        int readyAtImport=poll(&p,1,0);CHECK(readyAtImport>=0);
        fprintf(stderr,"EXTERNAL_IMAGE_SYNCFILE_BEFORE_IMPORT ready=%d revents=%d\n",readyAtImport,p.revents);
        PFN_vkImportSemaphoreFdKHR importFd=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(device,"vkImportSemaphoreFdKHR");
        CHECK(importFd!=NULL);
        VkImportSemaphoreFdInfoKHR imp={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=ready,
            .flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=syncFd};
        VkResult imported=importFd(device,&imp);if(imported==VK_SUCCESS)syncFd=-1;CHECK(imported==VK_SUCCESS);
        waitStage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&ready;submit.pWaitDstStageMask=&waitStage;
    }
    if(protectedFd>=0){
        CHECK(protectedReadback && protectedImage && protectedTicket);
        *protectedTicket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0L,0L,0L,0L);
        CHECK(*protectedTicket>0);
    }
    const int tracing=traceFirstSample;traceFirstSample=0;
    if(tracing){long result=syscall(500,3,1500,0,0,0,0);if(result){fprintf(stderr,"EXTERNAL_IMAGE_FRAME_TRACE_START_FAIL result=%ld errno=%d\n",result,errno);goto cleanup;}}
    uint64_t submitStartTsc=sample_tsc(),submitEndTsc=0,waitEndTsc=0;
    struct timespec started={0},submitted={0},finished={0};
    CHECK(clock_gettime(CLOCK_MONOTONIC,&started)==0);
    if(gpuSource==1)VKCHECK(vkQueueSubmit(queue,1,&producerSubmit,VK_NULL_HANDLE));
    VkResult queued=vkQueueSubmit(queue,1,&submit,fence);
    if(queued!=VK_SUCCESS && gpuSource){fprintf(stderr,"EXTERNAL_IMAGE_COMPLETION_UNCERTAIN submit=%d\n",queued);for(;;)pause();}
    CHECK(queued==VK_SUCCESS);
    submitEndTsc=sample_tsc();
    if(clock_gettime(CLOCK_MONOTONIC,&submitted)!=0){fprintf(stderr,"EXTERNAL_IMAGE_COMPLETION_UNCERTAIN clock\n");for(;;)pause();}
    VkResult waited=vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull);
    if(waited!=VK_SUCCESS){fprintf(stderr,"EXTERNAL_IMAGE_COMPLETION_UNCERTAIN result=%d\n",waited);for(;;)pause();}
    waitEndTsc=sample_tsc();
    producerOutstanding=0;
    CHECK(clock_gettime(CLOCK_MONOTONIC,&finished)==0);
    if(tracing){
        long result=syscall(500,1,0,0,0,0,0);
        fprintf(stderr,"EXTERNAL_IMAGE_FRAME_TRACE pid=%ld submit_start=%llu submit_end=%llu wait_end=%llu stop=%ld\n",
                (long)getpid(),(unsigned long long)submitStartTsc,(unsigned long long)submitEndTsc,
                (unsigned long long)waitEndTsc,result);
    }
    const int64_t elapsed=(int64_t)(finished.tv_sec-started.tv_sec)*1000000000ll+finished.tv_nsec-started.tv_nsec;
    const int64_t submitNs=(int64_t)(submitted.tv_sec-started.tv_sec)*1000000000ll+submitted.tv_nsec-started.tv_nsec;
    const int64_t waitNs=(int64_t)(finished.tv_sec-submitted.tv_sec)*1000000000ll+finished.tv_nsec-submitted.tv_nsec;
    CHECK(elapsed>=0);
    CHECK(submitNs>=0 && waitNs>=0);
    printf("EXTERNAL_IMAGE_SAMPLE_SUBMITTED source_pitch=%llu allocation=%llu elapsed_ns=%lld\n",
           (unsigned long long)layout.rowPitch,(unsigned long long)gem.size,(long long)elapsed);
    printf("EXTERNAL_IMAGE_CPU_TIMING submit_ns=%lld fence_wait_ns=%lld\n",(long long)submitNs,(long long)waitNs);
    if(timestamps){
        uint64_t ticks[2]={0};VKCHECK(vkGetQueryPoolResults(device,timestamps,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT));
        const uint64_t mask=timestampBits>=64?UINT64_MAX:((1ull<<timestampBits)-1);
        const uint64_t delta=(ticks[1]-ticks[0])&mask;
        printf("EXTERNAL_IMAGE_GPU_INTERVAL_NS %.0f valid_bits=%u\n",(double)delta*properties.limits.timestampPeriod,timestampBits);
    }
    passed=1;
cleanup:
    if(producerOutstanding){fprintf(stderr,"EXTERNAL_IMAGE_PRODUCER_UNCERTAIN resources retained\n");for(;;)pause();}
    if(!passed && protectedTicket && *protectedTicket>0){
        (void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,*protectedTicket,0L,0L,0L,0L);
        *protectedTicket=-1;
    }
    if(producer)vkFreeCommandBuffers(device,pool,1,&producer);
    if(ready)vkDestroySemaphore(device,ready,NULL);
    if(timestamps)vkDestroyQueryPool(device,timestamps,NULL);
    if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
    if(vertex)vkDestroyShaderModule(device,vertex,NULL);
    if(fragment)vkDestroyShaderModule(device,fragment,NULL);
    if(pl)vkDestroyPipelineLayout(device,pl,NULL);
    if(dp)vkDestroyDescriptorPool(device,dp,NULL);
    if(dsl)vkDestroyDescriptorSetLayout(device,dsl,NULL);
    if(sampler)vkDestroySampler(device,sampler,NULL);
    if(view)vkDestroyImageView(device,view,NULL);
    if(overlayFd>=0)close(overlayFd);
    sample_background_destroy(device,drm,&overlay);
    sample_background_destroy(device,drm,&bg);
    if(source)vkDestroyImage(device,source,NULL);
    if(memory)vkFreeMemory(device,memory,NULL);
    if(fd>=0)close(fd);
    if(syncFd>=0)close(syncFd);
    if(batchMap!=MAP_FAILED)munmap(batchMap,4096);
    if(drm>=0 && sync.handle)(void)ioctl(drm,IOC(0xc0,sizeof(sync)),&sync);
    if(drm>=0 && batch.handle){struct gem_close c={batch.handle,0};(void)ioctl(drm,IOC(9,sizeof(c)),&c);}
    if(drm>=0 && context.id){struct intel_context c={context.id,0};(void)ioctl(drm,IOC(0x6e,sizeof(c)),&c);}
    if(mapped!=MAP_FAILED)munmap(mapped,mapSize);
    if(drm>=0 && gem.handle){struct gem_close c={gem.handle,0};(void)ioctl(drm,IOC(9,sizeof(c)),&c);}
    if(drm>=0)close(drm);
    return passed;
}
