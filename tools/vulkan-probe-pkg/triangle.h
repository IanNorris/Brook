// Deterministic offscreen rasterization into a GPU image, then buffer readback.
#include "triangle_spv.h"
static VkResult render_triangle(VkDevice device,VkPhysicalDevice physical,
                                const VkPhysicalDeviceMemoryProperties* memory,
                                VkQueue queue,VkCommandBuffer command,VkFence fence,
                                Buffer* readback)
{
    int passed=0;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory imageMemory=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkFramebuffer frame=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
    const VkFormatFeatureFlags features=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if((format.optimalTilingFeatures&features)!=features)goto cleanup;
    VkImageCreateInfo imageInfo={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,64,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK_VK(vkCreateImage(device,&imageInfo,NULL,&image));
    VkMemoryRequirements needs;vkGetImageMemoryRequirements(device,image,&needs);
    uint32_t index=UINT32_MAX;
    for(uint32_t i=0;i<memory->memoryTypeCount;++i)
        if((needs.memoryTypeBits&(1u<<i))&&(memory->memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){index=i;break;}
    if(index==UINT32_MAX)goto cleanup;
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=needs.size,.memoryTypeIndex=index};
    CHECK_VK(vkAllocateMemory(device,&allocation,NULL,&imageMemory));
    CHECK_VK(vkBindImageMemory(device,image,imageMemory,0));
    VkImageViewCreateInfo viewInfo={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK_VK(vkCreateImageView(device,&viewInfo,NULL,&view));
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference reference={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&reference};
    VkSubpassDependency deps[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo passInfo={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,
        .dependencyCount=2,.pDependencies=deps};
    CHECK_VK(vkCreateRenderPass(device,&passInfo,NULL,&pass));
    VkFramebufferCreateInfo frameInfo={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=pass,.attachmentCount=1,.pAttachments=&view,.width=64,.height=64,.layers=1};
    CHECK_VK(vkCreateFramebuffer(device,&frameInfo,NULL,&frame));
    VkShaderModuleCreateInfo shaderInfo={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(vertexCode),.pCode=vertexCode};
    CHECK_VK(vkCreateShaderModule(device,&shaderInfo,NULL,&vertex));
    shaderInfo.codeSize=sizeof(fragmentCode);shaderInfo.pCode=fragmentCode;
    CHECK_VK(vkCreateShaderModule(device,&shaderInfo,NULL,&fragment));
    VkPipelineLayoutCreateInfo layoutInfo={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    CHECK_VK(vkCreatePipelineLayout(device,&layoutInfo,NULL,&layout));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vertexInput={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,64,64,0,1};VkRect2D scissor={{0,0},{64,64}};
    VkPipelineViewportStateCreateInfo viewportInfo={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo samples={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blendInfo={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend};
    VkGraphicsPipelineCreateInfo graphics={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vertexInput,.pInputAssemblyState=&assembly,
        .pViewportState=&viewportInfo,.pRasterizationState=&raster,.pMultisampleState=&samples,
        .pColorBlendState=&blendInfo,.layout=layout,.renderPass=pass,.subpass=0};
    CHECK_VK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&graphics,NULL,&pipeline));
    CHECK_VK(vkResetCommandBuffer(command,0));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK_VK(vkBeginCommandBuffer(command,&begin));
    VkClearValue clear={.color={{0,0,0,1}}};
    VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass=pass,.framebuffer=frame,.renderArea={{0,0},{64,64}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(command,&render,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={64,64,1}};
    vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->buffer,1,&copy);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK_VK(vkEndCommandBuffer(command));CHECK_VK(vkResetFences(device,1,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK_VK(vkQueueSubmit(queue,1,&submit,fence));CHECK_VK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ULL));
    if(!(readback->flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=readback->memory,.size=VK_WHOLE_SIZE};
        CHECK_VK(vkInvalidateMappedMemoryRanges(device,1,&range));
    }
    const unsigned char* pixels=readback->mapped;
    const unsigned char green[4]={0,255,0,255},black[4]={0,0,0,255};
    const unsigned samples_xy[][3]={{32,32,1},{32,40,1},{2,2,0},{61,2,0},{2,61,0},{61,61,0},{10,50,0}};
    for(unsigned i=0;i<sizeof(samples_xy)/sizeof(samples_xy[0]);++i) {
        const unsigned* at=samples_xy[i];
        if(memcmp(pixels+4*(at[1]*64+at[0]),at[2]?green:black,4)) {
            fprintf(stderr,"TRIANGLE_READBACK_MISMATCH x=%u y=%u\n",at[0],at[1]);goto cleanup;
        }
    }
    const char* output=getenv("BROOK_VULKAN_TRIANGLE_OUTPUT");
    if(output) {
        FILE* file=fopen(output,"wb");
        if(!file)goto cleanup;
        int okay=fprintf(file,"P6\n64 64\n255\n")>0;
        for(unsigned i=0;i<64*64&&okay;++i)okay=fwrite(pixels+4*i,1,3,file)==3;
        if(fclose(file))okay=0;
        if(!okay)goto cleanup;
    }
    fprintf(stderr,"VULKAN_TRIANGLE_PASS center=00ff00ff corner=000000ff size=64x64\n");passed=1;
cleanup:
    if(!passed)(void)vkDeviceWaitIdle(device);
    if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
    if(layout)vkDestroyPipelineLayout(device,layout,NULL);
    if(vertex)vkDestroyShaderModule(device,vertex,NULL);
    if(fragment)vkDestroyShaderModule(device,fragment,NULL);
    if(frame)vkDestroyFramebuffer(device,frame,NULL);
    if(pass)vkDestroyRenderPass(device,pass,NULL);
    if(view)vkDestroyImageView(device,view,NULL);
    if(image)vkDestroyImage(device,image,NULL);
    if(imageMemory)vkFreeMemory(device,imageMemory,NULL);
    return passed?VK_SUCCESS:VK_ERROR_UNKNOWN;
}
