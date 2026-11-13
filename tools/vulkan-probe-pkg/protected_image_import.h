/* Import and bind an OS-protected output. No GPU work or scanout is issued. */
static int protected_image_import(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,int write)
{
    int passed=0,fd=-1,owned=-1;VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    int attached=0,submitted=0;long ticket=-1;
    VkBuffer readback=VK_NULL_HANDLE;VkDeviceMemory readMemory=VK_NULL_HANDLE;void *pixels=NULL;
    VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={64,64,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(device,&ii,NULL,&image));
    VkMemoryRequirements needs;vkGetImageMemoryRequirements(device,image,&needs);
    VkImageSubresource imageSub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout imageLayout;
    vkGetImageSubresourceLayout(device,image,&imageSub,&imageLayout);
    CHECK(needs.size>0 && needs.size<=BROOK_GPU_OUTPUT_MAX_BYTES);
    fd=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,(long)needs.size,0L,0L,0L,0L,0L);CHECK(fd>=0);
    owned=dup(fd);CHECK(owned>=0);
    PFN_vkGetMemoryFdPropertiesKHR getFd=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR");
    CHECK(getFd!=NULL);VkMemoryFdPropertiesKHR fp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VKCHECK(getFd(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,owned,&fp));
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    uint32_t allowed=needs.memoryTypeBits&fp.memoryTypeBits,index=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;++i)if(allowed&(1u<<i)){index=i;break;}
    CHECK(index!=UINT32_MAX);
    VkImportMemoryFdInfoKHR import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=owned};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,
        .allocationSize=needs.size,.memoryTypeIndex=index};
    VkResult result=vkAllocateMemory(device,&ai,NULL,&memory);if(result==VK_SUCCESS)owned=-1;CHECK(result==VK_SUCCESS);
    VKCHECK(vkBindImageMemory(device,image,memory,0));
    printf("PROTECTED_IMAGE_IMPORT_PASS size=%llu\n",(unsigned long long)needs.size);
    CHECK(!(imageLayout.offset&3u) && !(imageLayout.rowPitch&3u) &&
          imageLayout.offset/4u<=UINT32_MAX && imageLayout.rowPitch/4u<=UINT32_MAX);
    struct BrookVkGraphicsImage shared={0};
    CHECK(brook_vk_graphics_image_import(physical,device,fd,64,64,(uint32_t)(imageLayout.offset/4u),
        (uint32_t)(imageLayout.rowPitch/4u),needs.size,ii.usage,&shared));
    vkDestroyImage(device,image,NULL);vkFreeMemory(device,memory,NULL);
    image=shared.image;memory=shared.memory;view=shared.view;
    puts("PROTECTED_IMAGE_SHARED_IMPORT_PASS");
    if(write){
        CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,0L,0L,0L,0L,0L));attached=1;
        ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,fd,0L,0L,0L,0L);CHECK(ticket>0);
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64*64*4,
            .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        VKCHECK(vkCreateBuffer(device,&bi,NULL,&readback));
        VkMemoryRequirements readNeed;vkGetBufferMemoryRequirements(device,readback,&readNeed);index=UINT32_MAX;
        for(uint32_t i=0;i<mp.memoryTypeCount;++i)
            if((readNeed.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){index=i;break;}
        CHECK(index!=UINT32_MAX);const VkMemoryPropertyFlags flags=mp.memoryTypes[index].propertyFlags;
        VkMemoryAllocateInfo rai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=readNeed.size,.memoryTypeIndex=index};
        VKCHECK(vkAllocateMemory(device,&rai,NULL,&readMemory));VKCHECK(vkBindBufferMemory(device,readback,readMemory,0));
        VKCHECK(vkMapMemory(device,readMemory,0,VK_WHOLE_SIZE,0,&pixels));
        VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
            .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
        VkSubpassDependency dependencies[2]={
            {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
            {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             .dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
        VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
            .subpassCount=1,.pSubpasses=&subpass,.dependencyCount=2,.pDependencies=dependencies};
        VKCHECK(vkCreateRenderPass(device,&rp,NULL,&pass));
        VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
            .attachmentCount=1,.pAttachments=&view,.width=64,.height=64,.layers=1};
        VKCHECK(vkCreateFramebuffer(device,&fb,NULL,&framebuffer));
        VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(vertexCode),.pCode=vertexCode};
        VKCHECK(vkCreateShaderModule(device,&si,NULL,&vertex));si.codeSize=sizeof(fragmentCode);si.pCode=fragmentCode;
        VKCHECK(vkCreateShaderModule(device,&si,NULL,&fragment));
        VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VKCHECK(vkCreatePipelineLayout(device,&pli,NULL,&pipelineLayout));
        VkPipelineShaderStageCreateInfo stages[2]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
        VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkViewport viewport={0,0,64,64,0,1};VkRect2D scissor={{0,0},{64,64}};
        VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
        VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};
        VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
        VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
            .pVertexInputState=&vin,.pInputAssemblyState=&assembly,.pViewportState=&vp,.pRasterizationState=&raster,
            .pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pipelineLayout,.renderPass=pass};
        VKCHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,NULL,&pipeline));
        VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,
            .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        VKCHECK(vkCreateCommandPool(device,&pi,NULL,&pool));
        VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
            .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;
        VKCHECK(vkAllocateCommandBuffers(device,&ca,&command));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VKCHECK(vkBeginCommandBuffer(command,&begin));
        VkClearValue clear={.color={{0,0,0,1}}};
        VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,
            .renderArea={{0,0},{64,64}},.clearValueCount=1,.pClearValues=&clear};
        vkCmdBeginRenderPass(command,&rbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={64,64,1}};
        vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
        VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
        VKCHECK(vkEndCommandBuffer(command));VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKCHECK(vkCreateFence(device,&fi,NULL,&fence));
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
        submitted=1;VKCHECK(vkQueueSubmit(queue,1,&submit,fence));
        VkResult waited=vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull);
        if(waited!=VK_SUCCESS){fprintf(stderr,"PROTECTED_IMAGE_COMPLETION_UNCERTAIN result=%d\n",waited);for(;;)pause();}
        long observed=0;
        for(unsigned i=0;i<5000 && observed==0;++i){
            observed=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0L,0L,0L,0L);
            if(observed==0)usleep(1000);
        }
        if(observed!=1){fprintf(stderr,"PROTECTED_IMAGE_HANDOFF_UNCERTAIN status=%ld\n",observed);for(;;)pause();}
        submitted=0;
        if(!(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
            VkMappedMemoryRange mr={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=readMemory,.size=VK_WHOLE_SIZE};
            VKCHECK(vkInvalidateMappedMemoryRanges(device,1,&mr));
        }
        const unsigned positions[][3]={{32,32,1},{32,40,1},{2,2,0},{61,2,0},{2,61,0},{61,61,0},{10,50,0}};
        for(unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);++i){
            const unsigned *p=positions[i];const uint32_t pixel=((uint32_t*)pixels)[p[1]*64+p[0]];
            if(pixel!=(p[2]?0xff00ff00u:0xff000000u)){
                fprintf(stderr,"PROTECTED_IMAGE_PIXEL_FAIL x=%u y=%u pixel=%08x\n",p[0],p[1],pixel);goto cleanup;
            }
        }
        printf("PROTECTED_IMAGE_TRIANGLE_PASS samples=7 observed=%ld\n",observed);
        CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0L,0L,0L,0L));ticket=-1;
        CHECK(sample_imported(physical,device,queue,pass,framebuffer,pool,command,fence,
                              32,32,64,64,2,0,0,0,image,readback,fd,&ticket));
        observed=0;
        for(unsigned i=0;i<5000 && observed==0;++i){
            observed=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0L,0L,0L,0L);
            if(observed==0)usleep(1000);
        }
        if(observed!=1){fprintf(stderr,"PROTECTED_IMAGE_HANDOFF_UNCERTAIN status=%ld\n",observed);for(;;)pause();}
        if(!(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
            VkMappedMemoryRange mr={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=readMemory,.size=VK_WHOLE_SIZE};
            VKCHECK(vkInvalidateMappedMemoryRanges(device,1,&mr));
        }
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
            const uint32_t sx=x/2,sy=y/2;
            const uint32_t expected=(sx==0 && sy==0)?0xff13579bu:
                (0xff000000u|(sx<<16)|(sy<<8)|(sx^sy));
            const uint32_t pixel=((uint32_t*)pixels)[y*64+x];
            if(pixel!=expected){fprintf(stderr,"PROTECTED_IMAGE_SAMPLE_FAIL x=%u y=%u pixel=%08x expected=%08x\n",x,y,pixel,expected);goto cleanup;}
        }
        printf("PROTECTED_IMAGE_PENDING_SAMPLE_PASS pixels=4096 observed=%ld\n",observed);
        CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0L,0L,0L,0L));ticket=-1;
        CHECK(protected_graphics_pass(physical,device,queue,family,fd,needs.size,
              (uint32_t)(imageLayout.offset/4u),(uint32_t)(imageLayout.rowPitch/4u)));
    }
    passed=1;
cleanup:
    if(submitted){fprintf(stderr,"PROTECTED_IMAGE_RESOURCES_RETAINED\n");for(;;)pause();}
    if(ticket>0)(void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0L,0L,0L,0L);
    if(attached)(void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_DETACH,0L,0L,0L,0L,0L);
    if(pixels)vkUnmapMemory(device,readMemory);
    if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
    if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,NULL);
    if(vertex)vkDestroyShaderModule(device,vertex,NULL);
    if(fragment)vkDestroyShaderModule(device,fragment,NULL);
    if(framebuffer)vkDestroyFramebuffer(device,framebuffer,NULL);
    if(pass)vkDestroyRenderPass(device,pass,NULL);
    if(view)vkDestroyImageView(device,view,NULL);
    if(fence)vkDestroyFence(device,fence,NULL);
    if(pool)vkDestroyCommandPool(device,pool,NULL);
    if(readback)vkDestroyBuffer(device,readback,NULL);
    if(readMemory)vkFreeMemory(device,readMemory,NULL);
    if(image)vkDestroyImage(device,image,NULL);
    if(memory)vkFreeMemory(device,memory,NULL);
    if(owned>=0)close(owned);
    if(fd>=0)close(fd);
    return passed;
}
