#define _POSIX_C_SOURCE 200809L
#include "graphics_pass.h"
#include "graphics_image.h"
#include "graphics_pass_spv.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct BrookVkGraphicsPass {
    VkPhysicalDevice physical;VkDevice device;VkQueue queue;
    VkDescriptorSetLayout layout;VkDescriptorPool descriptors;VkDescriptorSet set;
    VkPipelineLayout pipelineLayout;VkPipeline pipeline;VkRenderPass renderPass;
    VkShaderModule vertex,fragment;VkSampler sampler;
    VkCommandPool pool;VkCommandBuffer command;VkFence fence;VkFramebuffer framebuffer;
    VkSemaphore wait[BROOK_VK_GRAPHICS_MAX_LAYERS];
    struct BrookVkGraphicsImage image[BROOK_VK_GRAPHICS_MAX_LAYERS+1];
    VkBuffer metadata;VkDeviceMemory metadataMemory;void *metadataMap;VkMemoryPropertyFlags metadataFlags;
    VkQueryPool timestamps;uint32_t timestampBits;float timestampPeriod;uint64_t gpuNs;int gpuValid;
    int pending,uncertain;
};
static void frame_cleanup(BrookVkGraphicsPass *p)
{
    if(p->framebuffer)vkDestroyFramebuffer(p->device,p->framebuffer,NULL);
    p->framebuffer=VK_NULL_HANDLE;
    for(unsigned i=0;i<BROOK_VK_GRAPHICS_MAX_LAYERS;++i){if(p->wait[i])vkDestroySemaphore(p->device,p->wait[i],NULL);p->wait[i]=VK_NULL_HANDLE;}
    for(unsigned i=0;i<BROOK_VK_GRAPHICS_MAX_LAYERS+1;++i)brook_vk_graphics_image_destroy(p->device,&p->image[i]);
}
int brook_vk_graphics_pass_destroy(BrookVkGraphicsPass *p)
{
    if(!p)return 1;
    if(p->pending || p->uncertain)return 0;
    frame_cleanup(p);
    if(p->fence)vkDestroyFence(p->device,p->fence,NULL);
    if(p->timestamps)vkDestroyQueryPool(p->device,p->timestamps,NULL);
    if(p->pool)vkDestroyCommandPool(p->device,p->pool,NULL);
    if(p->pipeline)vkDestroyPipeline(p->device,p->pipeline,NULL);
    if(p->vertex)vkDestroyShaderModule(p->device,p->vertex,NULL);
    if(p->fragment)vkDestroyShaderModule(p->device,p->fragment,NULL);
    if(p->pipelineLayout)vkDestroyPipelineLayout(p->device,p->pipelineLayout,NULL);
    if(p->descriptors)vkDestroyDescriptorPool(p->device,p->descriptors,NULL);
    if(p->layout)vkDestroyDescriptorSetLayout(p->device,p->layout,NULL);
    if(p->sampler)vkDestroySampler(p->device,p->sampler,NULL);
    if(p->renderPass)vkDestroyRenderPass(p->device,p->renderPass,NULL);
    if(p->metadataMap)vkUnmapMemory(p->device,p->metadataMemory);
    if(p->metadata)vkDestroyBuffer(p->device,p->metadata,NULL);
    if(p->metadataMemory)vkFreeMemory(p->device,p->metadataMemory,NULL);
    free(p);return 1;
}

BrookVkGraphicsPass *brook_vk_graphics_pass_create(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family)
{
    if(!physical || !device || !queue)return NULL;
    BrookVkGraphicsPass *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->physical=physical;p->device=device;p->queue=queue;
#define OK(x) do { if((x)!=VK_SUCCESS)goto fail; } while(0)
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
    if(getenv("BROOK_RENDER_GPU_TIMESTAMPS")){
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,NULL);
        if(!n || n>64 || family>=n)goto fail;
        VkQueueFamilyProperties q[64];vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q);
        p->timestampBits=q[family].timestampValidBits;p->timestampPeriod=props.limits.timestampPeriod;
        if(!p->timestampBits || p->timestampBits>64 || !(p->timestampPeriod>0))goto fail;
        VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        OK(vkCreateQueryPool(device,&query,NULL,&p->timestamps));
    }
    if(props.limits.maxPerStageDescriptorSamplers<BROOK_VK_GRAPHICS_MAX_LAYERS ||
       props.limits.maxDescriptorSetSamplers<BROOK_VK_GRAPHICS_MAX_LAYERS ||
       props.limits.maxPerStageDescriptorSampledImages<BROOK_VK_GRAPHICS_MAX_LAYERS ||
       props.limits.maxDescriptorSetSampledImages<BROOK_VK_GRAPHICS_MAX_LAYERS ||
       props.limits.maxPerStageResources<BROOK_VK_GRAPHICS_MAX_LAYERS+1u)goto fail;
    const VkDeviceSize metadataBytes=(BROOK_VK_SCENE_HEADER+BROOK_VK_GRAPHICS_MAX_LAYERS*BROOK_VK_LAYER_WORDS)*4u;
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=metadataBytes,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    OK(vkCreateBuffer(device,&buffer,NULL,&p->metadata));
    VkMemoryRequirements need;vkGetBufferMemoryRequirements(device,p->metadata,&need);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((need.memoryTypeBits&(1u<<i)) &&
       (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){
        if(type==UINT32_MAX || (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))type=i;
        if(memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;
    }
    if(type==UINT32_MAX)goto fail;
    p->metadataFlags=memory.memoryTypes[type].propertyFlags;
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=need.size,.memoryTypeIndex=type};
    OK(vkAllocateMemory(device,&alloc,NULL,&p->metadataMemory));OK(vkBindBufferMemory(device,p->metadata,p->metadataMemory,0));
    OK(vkMapMemory(device,p->metadataMemory,0,VK_WHOLE_SIZE,0,&p->metadataMap));
    VkDescriptorSetLayoutBinding bindings[2]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=BROOK_VK_GRAPHICS_MAX_LAYERS,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    OK(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->layout));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,BROOK_VK_GRAPHICS_MAX_LAYERS}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    OK(vkCreateDescriptorPool(device,&dp,NULL,&p->descriptors));
    VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->descriptors,
        .descriptorSetCount=1,.pSetLayouts=&p->layout};
    OK(vkAllocateDescriptorSets(device,&ds,&p->set));
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&p->layout};
    OK(vkCreatePipelineLayout(device,&pl,NULL,&p->pipelineLayout));
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,
        .minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    OK(vkCreateSampler(device,&sampler,NULL,&p->sampler));
    VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
    VkSubpassDependency deps[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT}};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
        .subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=deps};
    OK(vkCreateRenderPass(device,&rp,NULL,&p->renderPass));
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(graphicsVertexCode),.pCode=graphicsVertexCode};
    OK(vkCreateShaderModule(device,&sm,NULL,&p->vertex));sm.codeSize=sizeof(graphicsFragmentCode);sm.pCode=graphicsFragmentCode;
    OK(vkCreateShaderModule(device,&sm,NULL,&p->fragment));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=p->vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=p->fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkDynamicState dynamicStates[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamicStates};
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vin,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pColorBlendState=&cb,.pDynamicState=&dynamic,.layout=p->pipelineLayout,.renderPass=p->renderPass};
    OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline,NULL,&p->pipeline));
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    OK(vkCreateCommandPool(device,&pool,NULL,&p->pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=p->pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    OK(vkAllocateCommandBuffers(device,&ca,&p->command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};OK(vkCreateFence(device,&fi,NULL,&p->fence));
#undef OK
    return p;
fail:
    brook_vk_graphics_pass_destroy(p);return NULL;
}

BrookVkResult brook_vk_graphics_pass_submit_regions(BrookVkGraphicsPass *p,const uint32_t *scene,size_t words,
        const struct BrookVkGraphicsBuffer *sources,size_t count,const struct BrookVkGraphicsBuffer *output,
        const struct BrookVkGraphicsRegion *regions,size_t regionCount)
{
    if(!p || p->pending || p->uncertain || !scene || !sources || !output || !count ||
       count>BROOK_VK_GRAPHICS_MAX_LAYERS || words<BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS ||
       scene[2]!=count || scene[3]!=0 || output->fd<0 || output->syncFd!=-1 ||
       output->width!=scene[0] || output->height!=scene[1] || regionCount>BROOK_VK_GRAPHICS_MAX_REGIONS ||
       (regionCount && !regions))return BROOK_VK_REJECTED;
    for(size_t i=0;i<regionCount;++i){
        const struct BrookVkGraphicsRegion *r=&regions[i];
        if(!r->width || !r->height || r->x>=output->width || r->y>=output->height ||
           r->width>output->width-r->x || r->height>output->height-r->y)return BROOK_VK_REJECTED;
    }
    size_t sizes[BROOK_VK_GRAPHICS_MAX_LAYERS];struct stat identities[BROOK_VK_GRAPHICS_MAX_LAYERS+1];
    for(size_t i=0;i<count;++i){
        const struct BrookVkGraphicsBuffer *b=&sources[i];const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        if(b->fd<0 || b->syncFd < -1 || !b->allocationBytes || (b->allocationBytes&3u) ||
           b->allocationBytes/4u>SIZE_MAX || b->offsetWords!=l[0] || b->strideWords!=l[1] ||
           b->width!=l[2] || b->height!=l[3])return BROOK_VK_REJECTED;
        sizes[i]=(size_t)(b->allocationBytes/4u);
    }
    if(!brook_vk_scene_valid_multi_output(scene,words,sizes,count) || !output->allocationBytes ||
       (output->allocationBytes&3u) || fstat(output->fd,&identities[count]) || !identities[count].st_ino)return BROOK_VK_REJECTED;
    for(size_t i=0;i<count;++i){
        if(fstat(sources[i].fd,&identities[i]) || !identities[i].st_ino)return BROOK_VK_REJECTED;
        for(size_t j=0;j<i;++j)if(identities[i].st_dev==identities[j].st_dev && identities[i].st_ino==identities[j].st_ino)return BROOK_VK_REJECTED;
        if(identities[i].st_dev==identities[count].st_dev && identities[i].st_ino==identities[count].st_ino)return BROOK_VK_REJECTED;
    }
    for(size_t i=0;i<=count;++i){
        const struct BrookVkGraphicsBuffer *b=i==count?output:&sources[i];
        const VkImageUsageFlags usage=i==count?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_SAMPLED_BIT;
        if(!brook_vk_graphics_image_import(p->physical,p->device,b->fd,b->width,b->height,
             b->offsetWords,b->strideWords,b->allocationBytes,usage,&p->image[i]))goto reject;
    }
    PFN_vkImportSemaphoreFdKHR importSync=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(p->device,"vkImportSemaphoreFdKHR");
    VkSemaphore waits[BROOK_VK_GRAPHICS_MAX_LAYERS];VkPipelineStageFlags stages[BROOK_VK_GRAPHICS_MAX_LAYERS];uint32_t waitCount=0;
    for(size_t i=0;i<count;++i)if(sources[i].syncFd>=0){
        if(!importSync)goto reject;
        int fd=fcntl(sources[i].syncFd,F_DUPFD_CLOEXEC,0);if(fd<0)goto reject;
        VkSemaphoreCreateInfo si={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if(vkCreateSemaphore(p->device,&si,NULL,&p->wait[i])!=VK_SUCCESS){close(fd);goto reject;}
        VkImportSemaphoreFdInfoKHR sf={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore=p->wait[i],.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=fd};
        if(getenv("BROOK_GRAPHICS_PASS_TRACE")){
            struct pollfd probe={.fd=fd,.events=POLLIN};int ready=poll(&probe,1,0);
            fprintf(stderr,"GRAPHICS_PASS_IMPORT_FENCE index=%zu ready=%d revents=%d\n",i,ready,probe.revents);
        }
        if(importSync(p->device,&sf)!=VK_SUCCESS){close(fd);goto reject;}
        waits[waitCount]=p->wait[i];stages[waitCount++]=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    const size_t metadataWords=BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS;
    memcpy(p->metadataMap,scene,metadataWords*4u);
    if(!(p->metadataFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
        VkMappedMemoryRange mr={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=p->metadataMemory,.size=VK_WHOLE_SIZE};
        if(vkFlushMappedMemoryRanges(p->device,1,&mr)!=VK_SUCCESS)goto reject;
    }
    VkDescriptorImageInfo descriptors[BROOK_VK_GRAPHICS_MAX_LAYERS];
    for(unsigned i=0;i<BROOK_VK_GRAPHICS_MAX_LAYERS;++i)descriptors[i]=(VkDescriptorImageInfo){
        .sampler=p->sampler,.imageView=p->image[i<count?i:0].view,.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo metadata={.buffer=p->metadata,.offset=0,.range=(BROOK_VK_SCENE_HEADER+BROOK_VK_GRAPHICS_MAX_LAYERS*BROOK_VK_LAYER_WORDS)*4u};
    VkWriteDescriptorSet writes[2]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.dstBinding=0,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&metadata},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.dstBinding=1,.descriptorCount=BROOK_VK_GRAPHICS_MAX_LAYERS,
         .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=descriptors}};
    vkUpdateDescriptorSets(p->device,2,writes,0,NULL);
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=p->renderPass,
        .attachmentCount=1,.pAttachments=&p->image[count].view,.width=output->width,.height=output->height,.layers=1};
    if(vkCreateFramebuffer(p->device,&fb,NULL,&p->framebuffer)!=VK_SUCCESS)goto reject;
    if(vkResetCommandBuffer(p->command,0)!=VK_SUCCESS || vkResetFences(p->device,1,&p->fence)!=VK_SUCCESS)goto reject;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if(vkBeginCommandBuffer(p->command,&begin)!=VK_SUCCESS)goto reject;
    p->gpuValid=0;
    if(p->timestamps){vkCmdResetQueryPool(p->command,p->timestamps,0,2);vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,p->timestamps,0);}
    VkImageMemoryBarrier barriers[BROOK_VK_GRAPHICS_MAX_LAYERS];
    for(size_t i=0;i<count;++i)barriers[i]=(VkImageMemoryBarrier){
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_PREINITIALIZED,
        .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=p->image[i].image,
        .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&host,0,NULL,(uint32_t)count,barriers);
    VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=p->renderPass,
        .framebuffer=p->framebuffer,.renderArea={{0,0},{output->width,output->height}}};
    vkCmdBeginRenderPass(p->command,&render,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport={0,0,output->width,output->height,0,1};
    vkCmdSetViewport(p->command,0,1,&viewport);
    vkCmdBindPipeline(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipeline);
    vkCmdBindDescriptorSets(p->command,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipelineLayout,0,1,&p->set,0,NULL);
    if(!regionCount){VkRect2D scissor={{0,0},{output->width,output->height}};
        vkCmdSetScissor(p->command,0,1,&scissor);vkCmdDraw(p->command,3,1,0,0);
    }else for(size_t i=0;i<regionCount;++i){
        VkRect2D scissor={{(int32_t)regions[i].x,(int32_t)regions[i].y},{regions[i].width,regions[i].height}};
        vkCmdSetScissor(p->command,0,1,&scissor);vkCmdDraw(p->command,3,1,0,0);
    }
    vkCmdEndRenderPass(p->command);
    VkMemoryBarrier release={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT};
    vkCmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&release,0,NULL,0,NULL);
    if(p->timestamps)vkCmdWriteTimestamp(p->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->timestamps,1);
    if(vkEndCommandBuffer(p->command)!=VK_SUCCESS)goto reject;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=waitCount,.pWaitSemaphores=waits,
        .pWaitDstStageMask=stages,.commandBufferCount=1,.pCommandBuffers=&p->command};
    if(vkQueueSubmit(p->queue,1,&submit,p->fence)!=VK_SUCCESS){p->uncertain=1;return BROOK_VK_UNCERTAIN;}
    p->pending=1;return BROOK_VK_PENDING;
reject:
    frame_cleanup(p);return BROOK_VK_REJECTED;
}

BrookVkResult brook_vk_graphics_pass_submit_scene(BrookVkGraphicsPass *p,const uint32_t *scene,size_t words,
        const struct BrookVkGraphicsBuffer *sources,size_t count,const struct BrookVkGraphicsBuffer *output)
{
    return brook_vk_graphics_pass_submit_regions(p,scene,words,sources,count,output,NULL,0);
}

BrookVkResult brook_vk_graphics_pass_submit(BrookVkGraphicsPass *p,
        const struct BrookVkGraphicsBuffer *bg,const struct BrookVkGraphicsBuffer *fg,
        const struct BrookVkGraphicsBuffer *output,const struct BrookVkGraphicsLayer *layer)
{
    if(!bg || !fg || !output || !brook_vk_graphics_layer_valid(layer,output->width,output->height,fg->width,fg->height) ||
       bg->width!=output->width || bg->height!=output->height)return BROOK_VK_REJECTED;
    uint32_t scene[BROOK_VK_SCENE_HEADER+2*BROOK_VK_LAYER_WORDS]={output->width,output->height,2,0};
    struct BrookVkSceneLayer base={bg->offsetWords,bg->strideWords,bg->width,bg->height,0,0,bg->width,bg->height,
        0,0,bg->width,bg->height,0,0,bg->width,bg->height,255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE};
    struct BrookVkSceneLayer front={fg->offsetWords,fg->strideWords,fg->width,fg->height,
        layer->sourceX,layer->sourceY,layer->sourceWidth,layer->sourceHeight,layer->x,layer->y,layer->width,layer->height,
        layer->clipX,layer->clipY,layer->clipWidth,layer->clipHeight,layer->opacity|(layer->premultiplied?0:BROOK_VK_STRAIGHT_ALPHA)};
    memcpy(scene+BROOK_VK_SCENE_HEADER,&base,sizeof(base));memcpy(scene+BROOK_VK_SCENE_HEADER+BROOK_VK_LAYER_WORDS,&front,sizeof(front));
    const struct BrookVkGraphicsBuffer sources[2]={*bg,*fg};
    return brook_vk_graphics_pass_submit_scene(p,scene,sizeof(scene)/sizeof(scene[0]),sources,2,output);
}

BrookVkResult brook_vk_graphics_pass_poll(BrookVkGraphicsPass *p)
{
    if(!p || !p->pending || p->uncertain)return BROOK_VK_REJECTED;
    VkResult status=vkGetFenceStatus(p->device,p->fence);
    if(status==VK_NOT_READY)return BROOK_VK_PENDING;
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
int brook_vk_graphics_pass_gpu_timestamp_ns(const BrookVkGraphicsPass *p,uint64_t *ns)
{
    if(!p || !ns || !p->gpuValid)return 0;
    *ns=p->gpuNs;return 1;
}
