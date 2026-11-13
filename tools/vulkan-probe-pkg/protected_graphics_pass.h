/* Execute the reusable pass into an already-attached protected output.
 * The handoff is observed before releasing any source or output ownership. */
static int protected_graphics_pass(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,
                                   int outputFd,uint64_t outputBytes,uint32_t outputOffset,uint32_t outputStride)
{
    int result=0,drm=-1,bgfd=-1,fgfd=-1;long ticket=-1;
    struct SampleBackground bg={0},fg={0};BrookVkGraphicsPass *pass=NULL;
    drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    CHECK(sample_background_create(physical,device,drm,64,64,&bg));
    CHECK(sample_background_create(physical,device,drm,64,64,&fg));
    VkImageSubresource sub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout bl,fl;
    vkGetImageSubresourceLayout(device,bg.image,&sub,&bl);vkGetImageSubresourceLayout(device,fg.image,&sub,&fl);
    struct prime b={.handle=bg.gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
    struct prime f={.handle=fg.gem.handle,.flags=O_CLOEXEC|O_RDWR,.fd=-1};
    CHECK(!ioctl(drm,IOC(0x2d,sizeof(b)),&b));bgfd=b.fd;
    CHECK(!ioctl(drm,IOC(0x2d,sizeof(f)),&f));fgfd=f.fd;
    struct BrookVkGraphicsBuffer background={bgfd,-1,64,64,(uint32_t)(bl.offset/4u),(uint32_t)(bl.rowPitch/4u),bg.size};
    struct BrookVkGraphicsBuffer foreground={fgfd,-1,64,64,(uint32_t)(fl.offset/4u),(uint32_t)(fl.rowPitch/4u),fg.size};
    struct BrookVkGraphicsBuffer output={outputFd,-1,64,64,outputOffset,outputStride,outputBytes};
    struct BrookVkGraphicsLayer layer={255,0,0,0,64,64,0,0,64,64,0,0,64,64};
    pass=brook_vk_graphics_pass_create(physical,device,queue,family);CHECK(pass);
    ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,outputFd,0L,0L,0L,0L);CHECK(ticket>0);
    BrookVkResult status=brook_vk_graphics_pass_submit(pass,&background,&foreground,&output,&layer);
    if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"PROTECTED_GRAPHICS_PASS_UNCERTAIN submit\n");for(;;)pause();}
    CHECK(status==BROOK_VK_PENDING);
    for(unsigned i=0;i<10000 && status==BROOK_VK_PENDING;++i){usleep(1000);status=brook_vk_graphics_pass_poll(pass);}
    if(status!=BROOK_VK_COMPLETE){fprintf(stderr,"PROTECTED_GRAPHICS_PASS_UNCERTAIN completion=%d\n",status);for(;;)pause();}
    long observed=0;
    for(unsigned i=0;i<5000 && observed==0;++i){
        observed=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0L,0L,0L,0L);
        if(!observed)usleep(1000);
    }
    if(observed!=1){fprintf(stderr,"PROTECTED_GRAPHICS_PASS_UNCERTAIN handoff=%ld\n",observed);for(;;)pause();}
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0L,0L,0L,0L));ticket=-1;
    puts("PROTECTED_GRAPHICS_PASS_HANDOFF_PASS observed=1");result=1;
cleanup:
    if(pass && !brook_vk_graphics_pass_destroy(pass)){fprintf(stderr,"PROTECTED_GRAPHICS_PASS_RETAINED\n");for(;;)pause();}
    if(ticket>0)(void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0L,0L,0L,0L);
    if(bgfd>=0)close(bgfd);
    if(fgfd>=0)close(fgfd);
    sample_background_destroy(device,drm,&bg);sample_background_destroy(device,drm,&fg);
    if(drm>=0)close(drm);
    return result;
}
