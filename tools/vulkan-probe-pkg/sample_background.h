/* Independent linear DMA-BUF with a patterned opaque background.
 * This is fixture infrastructure; the desktop backend needs explicit layout
 * negotiation and must retain every imported image until GPU retirement. */
struct SampleBackground {
    struct gem_create gem;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    void *map;
    size_t size;
};
static void sample_background_destroy(VkDevice device,int drm,struct SampleBackground *bg)
{
    if(bg->view)vkDestroyImageView(device,bg->view,NULL);
    if(bg->image)vkDestroyImage(device,bg->image,NULL);
    if(bg->memory)vkFreeMemory(device,bg->memory,NULL);
    if(bg->map && bg->map!=MAP_FAILED)munmap(bg->map,bg->size);
    if(drm>=0 && bg->gem.handle){struct gem_close c={bg->gem.handle,0};(void)ioctl(drm,IOC(9,sizeof(c)),&c);}
    memset(bg,0,sizeof(*bg));
}
static int sample_background_create(VkPhysicalDevice physical,VkDevice device,int drm,
                                    unsigned width,unsigned height,struct SampleBackground *bg)
{
    int fd=-1,ok=0;memset(bg,0,sizeof(*bg));
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,.usage=VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED};
    VKCHECK(vkCreateImage(device,&ii,NULL,&bg->image));
    VkMemoryRequirements needs;vkGetImageMemoryRequirements(device,bg->image,&needs);
    VkImageSubresource sub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device,bg->image,&sub,&layout);
    CHECK(needs.size && needs.size<=64*1024*1024 && layout.rowPitch>=width*4 &&
          layout.offset+(height-1)*layout.rowPitch+width*4<=needs.size);
    CHECK(!(layout.offset&3u) && !(layout.rowPitch&3u) && layout.offset/4u<=UINT32_MAX &&
          layout.rowPitch/4u<=UINT32_MAX &&
          brook_vk_graphics_linear_layout_valid(width,height,(uint32_t)(layout.offset/4u),
              (uint32_t)(layout.rowPitch/4u),needs.size,layout.offset,layout.rowPitch,needs.size));
    bg->gem.size=needs.size;CHECK(!ioctl(drm,IOC(0x5b,sizeof(bg->gem)),&bg->gem));bg->size=(size_t)bg->gem.size;
    struct map_offset mo={.handle=bg->gem.handle,.flags=2};CHECK(!ioctl(drm,IOC(0x64,sizeof(mo)),&mo));
    bg->map=mmap(NULL,bg->size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)mo.offset);CHECK(bg->map!=MAP_FAILED);
    struct gem_domain domain={.handle=bg->gem.handle,.read=1,.write=1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(domain)),&domain));
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const uint32_t r=(x*3u+17u)&255u,g=(y*5u+23u)&255u,b=(x*7u+y*11u)&255u;
        const uint32_t pixel=0xff000000u|(r<<16)|(g<<8)|b;
        memcpy((uint8_t*)bg->map+layout.offset+y*layout.rowPitch+x*4,&pixel,4);
    }
    struct prime prime={.handle=bg->gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
    CHECK(!ioctl(drm,IOC(0x2d,sizeof(prime)),&prime));fd=prime.fd;
    vkDestroyImage(device,bg->image,NULL);bg->image=VK_NULL_HANDLE;
    struct BrookVkGraphicsImage imported={0};
    int invalidAccepted=brook_vk_graphics_image_import(physical,device,fd,width,height,(uint32_t)(layout.offset/4u),
        (uint32_t)(layout.rowPitch/4u)+1u,bg->size,VK_IMAGE_USAGE_SAMPLED_BIT,&imported);
    if(invalidAccepted)brook_vk_graphics_image_destroy(device,&imported);
    CHECK(!invalidAccepted);
    CHECK(brook_vk_graphics_image_import(physical,device,fd,width,height,(uint32_t)(layout.offset/4u),
        (uint32_t)(layout.rowPitch/4u),bg->size,VK_IMAGE_USAGE_SAMPLED_BIT,&imported));
    bg->image=imported.image;bg->memory=imported.memory;bg->view=imported.view;ok=1;
cleanup:
    if(fd>=0)close(fd);
    if(!ok)sample_background_destroy(device,drm,bg);
    return ok;
}
