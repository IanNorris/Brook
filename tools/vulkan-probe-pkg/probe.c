#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/sync_file.h>
// Generated compute, transfer and mapped-memory correctness test; no WSI.
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe_spv.h"
#include "compositor_spv.h"
#include "compositor_probe.h"

enum { WORDS = 16384, ROUNDS = 3 };
static const VkDeviceSize bytes = WORDS * sizeof(uint32_t);
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkMemoryPropertyFlags flags;
} Buffer;

static VkResult make_buffer(VkDevice device, const VkPhysicalDeviceMemoryProperties* memory,
                            VkBufferUsageFlags usage, VkMemoryPropertyFlags required, Buffer* out)
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkResult result = vkCreateBuffer(device, &info, NULL, &out->buffer);
    if (result) return result;
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(device, out->buffer, &needs);
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i)
        if ((needs.memoryTypeBits & (1u << i)) &&
            (memory->memoryTypes[i].propertyFlags & required) == required) { index = i; break; }
    if (index == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    out->flags = memory->memoryTypes[index].propertyFlags;
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = needs.size, .memoryTypeIndex = index };
    result = vkAllocateMemory(device, &allocation, NULL, &out->memory);
    if (result) return result;
    return vkBindBufferMemory(device, out->buffer, out->memory, 0);
}

static void release_buffer(VkDevice device, Buffer* buffer)
{
    if (buffer->mapped) vkUnmapMemory(device, buffer->memory);
    if (buffer->buffer) vkDestroyBuffer(device, buffer->buffer, NULL);
    if (buffer->memory) vkFreeMemory(device, buffer->memory, NULL);
}

static void json_string(const char* text)
{
    putchar('"');
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

#define CHECK_VK(call) do { VkResult status = (call); if (status != VK_SUCCESS) { \
    fprintf(stderr, "VULKAN_PROBE_ERROR %s result=%d\n", #call, status); goto cleanup; } } while (0)

#include "triangle.h"
#include "timestamps.h"
#include "cache_probe.h"

int main(int argc, char** argv)
{
    const char* expected = argc == 3 && !strcmp(argv[1], "--expect-driver") ? argv[2] : "hardware";
    if ((argc != 1 && argc != 3) ||
        (argc == 3 && strcmp(argv[1], "--expect-driver")) ||
        (strcmp(expected, "hardware") && strcmp(expected, "venus") && strcmp(expected, "intel") && strcmp(expected,"software"))) {
        fprintf(stderr, "usage: vulkan-probe [--expect-driver hardware|intel|venus|software]\n"); return 2;
    }
    int passed = 0;
    const char* cacheArm=getenv("BROOK_VULKAN_CACHE_PROBE");
    const int cacheProbe=cacheArm!=NULL;
    unsigned cacheOmit=0,cacheWords=1024;
    enum { CacheRounds=12 };
    int node=-1, inputFd=-1, outputFd=-1;
    if(cacheProbe){
        if(cacheArm[0]<'0' || cacheArm[0]>'3' || cacheArm[1] ||
           strcmp(expected,"intel"))return 2;
        cacheOmit=(unsigned)(cacheArm[0]-'0');
        const char* footprint=getenv("BROOK_VULKAN_CACHE_PROBE_LARGE");
        if(footprint && !strcmp(footprint,"1"))cacheWords=524288;
    }
    const int timestamping = getenv("BROOK_VULKAN_TIMESTAMPS") != NULL;
    ProbeTiming timing = {0};
    const int externalSync=getenv("BROOK_VULKAN_SYNC_FD")!=NULL;
    const int compositorTest=getenv("BROOK_VULKAN_COMPOSITOR_TEST")!=NULL;
    if(cacheProbe && (timestamping || externalSync || compositorTest || getenv("BROOK_VULKAN_FILL_DIAGNOSTIC")))return 2;
    if(compositorTest && externalSync) {fprintf(stderr,"Compositor test excludes repeated sync-fd dispatch\n");return 2;}
    int exportedFd=-1;
    VkSemaphore exportedSemaphore=VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer upload = {0}, storage = {0}, readback = {0};
    VkPhysicalDeviceProperties properties = {0};
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Brook Vulkan correctness probe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo create = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK_VK(vkCreateInstance(&create, NULL, &instance));
    uint32_t count = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count || count > 32) { fprintf(stderr, "Unexpected device count %u\n", count); goto cleanup; }
    VkPhysicalDevice devices[32], physical = VK_NULL_HANDLE;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, devices));
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties2 info = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver };
        vkGetPhysicalDeviceProperties2(devices[i], &info);
        if (info.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && strcmp(expected,"software")) continue;
        if (info.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && !strcmp(expected,"software")) continue;
        if (!strcmp(expected, "venus") && driver.driverID != VK_DRIVER_ID_MESA_VENUS) continue;
        if (!strcmp(expected, "intel") && driver.driverID != VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA) continue;
        properties = info.properties; physical = devices[i]; break;
    }
    if (!physical) { fprintf(stderr, "No matching hardware-backed device (%s)\n", expected); goto cleanup; }
    fprintf(stderr, "VULKAN_PROBE_DEVICE %s driver=%s id=%u vendor=%04x device=%04x\n",
            properties.deviceName, driver.driverName, driver.driverID, properties.vendorID, properties.deviceID);
    uint32_t families = 0, family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    if (!families || families > 64) goto cleanup;
    VkQueueFamilyProperties queues[64];
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues);
    for (uint32_t i = 0; i < families; ++i)
        if (queues[i].queueCount && ((queues[i].queueFlags & (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)) == (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT))) { family = i; break; }
    if (family == UINT32_MAX) goto cleanup;
    if (timestamping) {
        timing.bits = queues[family].timestampValidBits;
        timing.period = properties.limits.timestampPeriod;
        if (!timing.bits || timing.bits > 64 || !(timing.period > 0)) {
            fprintf(stderr, "VULKAN_PROBE_ERROR timestamp support unavailable\n"); goto cleanup;
        }
    }
    const int calibration = timestamping && timing_calibration_supported(instance, physical);
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    const char* deviceExtensions[4];
    uint32_t extensionCount = 0;
    if (externalSync) deviceExtensions[extensionCount++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    if (calibration) deviceExtensions[extensionCount++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    if(cacheProbe){
        deviceExtensions[extensionCount++]=VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        deviceExtensions[extensionCount++]=VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    VkDeviceCreateInfo deviceInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount=extensionCount,.ppEnabledExtensionNames=deviceExtensions };
    CHECK_VK(vkCreateDevice(physical, &deviceInfo, NULL, &device));
    if (timestamping) {
        VkQueryPoolCreateInfo info = {.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType=VK_QUERY_TYPE_TIMESTAMP, .queryCount=ROUNDS * TIMING_POINTS};
        CHECK_VK(vkCreateQueryPool(device, &info, NULL, &timing.pool));
        if (calibration) {
            timing.calibrate = (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(device, "vkGetCalibratedTimestampsEXT");
            if (!timing.calibrate) goto cleanup;
        }
        CHECK_VK(timing_calibrate(device, &timing));
    }
    if(externalSync) {
        VkExportSemaphoreCreateInfo exportInfo={.sType=VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkSemaphoreCreateInfo semaphoreInfo={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&exportInfo};
        CHECK_VK(vkCreateSemaphore(device,&semaphoreInfo,NULL,&exportedSemaphore));
    }
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const VkDeviceSize probeBytes=cacheProbe?(VkDeviceSize)cacheWords*sizeof(uint32_t):bytes;
    if(cacheProbe){
        node=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);
        if(node<0 || !cache_probe_allocate(node,device,cacheOmit&1u,&memory,probeBytes,&upload,&inputFd) ||
           !cache_probe_allocate(node,device,cacheOmit&2u,&memory,probeBytes,&readback,&outputFd)){
            fprintf(stderr,"CACHE_PROBE_ALLOC_FAIL\n");goto cleanup;
        }
    }else{
        CHECK_VK(make_buffer(device, &memory, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &upload));
        CHECK_VK(make_buffer(device, &memory, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &readback));
        CHECK_VK(make_buffer(device, &memory, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &storage));
        CHECK_VK(vkMapMemory(device, upload.memory, 0, VK_WHOLE_SIZE, 0, &upload.mapped));
        CHECK_VK(vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &readback.mapped));
    }
    VkDescriptorSetLayoutBinding binding[2] = {
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
    };
    const unsigned descriptorCount=cacheProbe?2u:1u;
    VkDescriptorSetLayoutCreateInfo layoutInfo = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=descriptorCount,.pBindings=binding};
    CHECK_VK(vkCreateDescriptorSetLayout(device, &layoutInfo, NULL, &layout));
    VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(uint32_t) };
    VkPipelineLayoutCreateInfo pipelineInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    CHECK_VK(vkCreatePipelineLayout(device, &pipelineInfo, NULL, &pipelineLayout));
    VkShaderModuleCreateInfo shaderInfo = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(shaderCode), .pCode = shaderCode };
    if(compositorTest) {shaderInfo.codeSize=sizeof(compositorCode);shaderInfo.pCode=compositorCode;}
    if(cacheProbe) {shaderInfo.codeSize=sizeof(cacheProbeCode);shaderInfo.pCode=cacheProbeCode;}
    CHECK_VK(vkCreateShaderModule(device, &shaderInfo, NULL, &shader));
    VkComputePipelineCreateInfo compute = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" }, .layout = pipelineLayout };
    CHECK_VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, NULL, &pipeline));
    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptorCount };
    VkDescriptorPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &poolSize };
    CHECK_VK(vkCreateDescriptorPool(device, &poolInfo, NULL, &descriptors));
    VkDescriptorSetAllocateInfo allocateSet = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &layout };
    VkDescriptorSet set;
    CHECK_VK(vkAllocateDescriptorSets(device, &allocateSet, &set));
    VkDescriptorBufferInfo bufferInfo[2] = {
        {cacheProbe?upload.buffer:storage.buffer,0,probeBytes},
        {readback.buffer,0,probeBytes},
    };
    VkWriteDescriptorSet write[2] = {
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bufferInfo[0]},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bufferInfo[1]},
    };
    vkUpdateDescriptorSets(device,descriptorCount,write,0,NULL);
    VkCommandPoolCreateInfo commandInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    CHECK_VK(vkCreateCommandPool(device, &commandInfo, NULL, &commands));
    VkCommandBufferAllocateInfo allocateCommand = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer command;
    CHECK_VK(vkAllocateCommandBuffers(device, &allocateCommand, &command));
    VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK_VK(vkCreateFence(device, &fenceInfo, NULL, &fence));
    if(cacheProbe){
        volatile uint32_t* input=upload.mapped;
        volatile uint32_t* output=readback.mapped;
        for(uint32_t round=0;round<CacheRounds;++round){
            const uint32_t seed=7u+round*31u;
            const uint32_t sentinel=0xcafe0123u^seed;
            CHECK_VK(vkResetCommandBuffer(command,0));
            VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK_VK(vkBeginCommandBuffer(command,&begin));
            VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0,1,&barrier,0,NULL,0,NULL);
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,NULL);
            vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(seed),&seed);
            // Leave the last workgroup as an overwrite guard.
            vkCmdDispatch(command,cacheWords/64-1,1,1);
            barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                                 0,1,&barrier,0,NULL,0,NULL);
            CHECK_VK(vkEndCommandBuffer(command));
            CHECK_VK(vkResetFences(device,1,&fence));
            for(uint32_t i=0;i<cacheWords;++i)output[i]=sentinel;
            for(uint32_t i=0;i<cacheWords;++i)input[i]=(i*17u)^seed;
            // Populate CPU lines in the writer's cache. The kernel additionally
            // primes output lines after the input-side pass in post-omit arms.
            for(uint32_t i=0;i<cacheWords;i+=16)if(output[i]!=sentinel)goto cleanup;
            VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount=1,.pCommandBuffers=&command};
            CHECK_VK(vkQueueSubmit(queue,1,&submit,fence));
            CHECK_VK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ULL));
            const uint64_t pages=probeBytes/4096;
            if(!cache_probe_query(node,inputFd,cacheOmit&1u,probeBytes,cacheOmit&1u?pages*(round+1):0,0) ||
               !cache_probe_query(node,outputFd,cacheOmit&2u,probeBytes,0,cacheOmit&2u?pages*(round+1):0))
                goto cleanup;
            for(uint32_t i=0;i<cacheWords;++i){
                const uint32_t want=i>=cacheWords-64?sentinel:
                    (((i*17u)^seed)^(0x9e3779b9u+seed))*1664525u+1013904223u;
                if(output[i]!=want){
                    fprintf(stderr,"CACHE_PROBE_MISMATCH arm=%u round=%u word=%u got=%08x expected=%08x\n",
                            cacheOmit,round,i,output[i],want);goto cleanup;
                }
            }
        }
        printf("CACHE_PROBE_PASS omit=%u bytes=%llu rounds=%u\n",cacheOmit,
               (unsigned long long)probeBytes,CacheRounds);
        passed=1;
        goto cleanup;
    }
    if (getenv("BROOK_VULKAN_FILL_DIAGNOSTIC")) {
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK_VK(vkBeginCommandBuffer(command, &begin));
        vkCmdFillBuffer(command, readback.buffer, 0, bytes, 0x95c73a2du);
        VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK_VK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command };
        CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL));
        if (!(readback.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = readback.memory, .size = VK_WHOLE_SIZE };
            CHECK_VK(vkInvalidateMappedMemoryRanges(device, 1, &range));
        }
        fprintf(stderr, "VULKAN_FILL_DIAGNOSTIC first=%08x last=%08x\n",
                ((const uint32_t*)readback.mapped)[0], ((const uint32_t*)readback.mapped)[WORDS - 1]);
        if (((const uint32_t*)readback.mapped)[0] != 0x95c73a2du ||
            ((const uint32_t*)readback.mapped)[WORDS - 1] != 0x95c73a2du) goto cleanup;
    }
    for (uint32_t round = 0; round < ROUNDS; ++round) {
        uint32_t seed = 7 + round * 31;
        uint32_t* input = upload.mapped;
        for (uint32_t i = 0; i < WORDS; ++i) input[i] = (i * 17u) ^ seed;
        if(compositorTest) {
            compositor_probe_prepare(input,WORDS,round);
            if(!brook_vk_scene_valid(input,WORDS)) {fprintf(stderr,"Invalid compositor fixture\n");goto cleanup;}
        }
        if (!(upload.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = upload.memory, .size = VK_WHOLE_SIZE };
            CHECK_VK(vkFlushMappedMemoryRanges(device, 1, &range));
        }
        CHECK_VK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK_VK(vkBeginCommandBuffer(command, &begin));
        if (timestamping) {
            vkCmdResetQueryPool(command, timing.pool, round * TIMING_POINTS, TIMING_POINTS);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing.pool, round * TIMING_POINTS);
        }
        VkBufferCopy copy = { .size = bytes };
        vkCmdCopyBuffer(command, upload.buffer, storage.buffer, 1, &copy);
        VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        if(!compositorTest)vkCmdFillBuffer(command, storage.buffer, 256, 512, 0x13579bdfu);
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        if (timestamping) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing.pool, round * TIMING_POINTS + 1);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(seed), &seed);
        const unsigned dispatches=externalSync?256:1;
        for(unsigned step=0;step<dispatches;++step) {
            vkCmdDispatch(command, WORDS / 64, 1, 1);
            if(step+1<dispatches) {
                VkMemoryBarrier between={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
                vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0,1,&between,0,NULL,0,NULL);
            }
        }
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        if (timestamping) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing.pool, round * TIMING_POINTS + 2);
        vkCmdCopyBuffer(command, storage.buffer, readback.buffer, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        if (timestamping) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing.pool, round * TIMING_POINTS + 3);
        CHECK_VK(vkEndCommandBuffer(command));
        CHECK_VK(vkResetFences(device, 1, &fence));
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command,
            .signalSemaphoreCount=externalSync?1:0,.pSignalSemaphores=&exportedSemaphore };
        if (timestamping && !monotonic_ns(&timing.cpu[round][0])) goto cleanup;
        CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence));
        if (timestamping && !monotonic_ns(&timing.cpu[round][1])) goto cleanup;
        if(externalSync) {
            VkSemaphoreGetFdInfoKHR info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                .semaphore=exportedSemaphore,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
            PFN_vkGetSemaphoreFdKHR getFd=(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device,"vkGetSemaphoreFdKHR");
            if(!getFd)goto cleanup;
            CHECK_VK(getFd(device,&info,&exportedFd));
            if(exportedFd<0)goto cleanup;
            struct pollfd wait={.fd=exportedFd,.events=POLLIN};
            if(poll(&wait,1,10000)!=1 || wait.revents!=POLLIN)goto cleanup;
            struct sync_file_info status={0};
            if(ioctl(exportedFd,SYNC_IOC_FILE_INFO,&status) || status.status!=1)goto cleanup;
            close(exportedFd);exportedFd=-1;
        } else {
            CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL));
        }
        if (timestamping) {
            if (!monotonic_ns(&timing.cpu[round][2])) goto cleanup;
            CHECK_VK(timing_read(device, &timing, round));
        }
        if (!(readback.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = readback.memory, .size = VK_WHOLE_SIZE };
            CHECK_VK(vkInvalidateMappedMemoryRanges(device, 1, &range));
        }
        const uint32_t* output = readback.mapped;
        for (uint32_t i = 0; i < WORDS; ++i) {
            uint32_t want;
            if(compositorTest)want=i>=12288?compositor_probe_pixel(input,(i-12288)%64,(i-12288)/64):input[i];
            else {
                want=i >= 64 && i < 192 ? 0x13579bdfu : (i * 17u) ^ seed;
                for(unsigned step=0;step<dispatches;++step)want=(want^(0x9e3779b9u+seed))*1664525u+1013904223u;
            }
            if (output[i] != want) {
                fprintf(stderr, "READBACK_MISMATCH round=%u index=%u actual=%08x expected=%08x\n",
                        round, i, output[i], want); goto cleanup;
            }
        }
        // Readback above must be correct BEFORE this ordinary fence wait.
        if(externalSync)CHECK_VK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ULL));
    }
    if (timestamping) CHECK_VK(timing_calibrate(device, &timing));
    CHECK_VK(render_triangle(device,physical,&memory,queue,command,fence,&readback));
    CHECK_VK(vkDeviceWaitIdle(device));
    passed = 1;
cleanup:
    if(exportedFd>=0)close(exportedFd);
    if (device) {
        // Process/VM supervisors also bound failures where a driver cannot finish.
        if (!passed) (void)vkDeviceWaitIdle(device);
        if(exportedSemaphore)vkDestroySemaphore(device,exportedSemaphore,NULL);
        if (timing.pool) vkDestroyQueryPool(device, timing.pool, NULL);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (commands) vkDestroyCommandPool(device, commands, NULL);
        if (descriptors) vkDestroyDescriptorPool(device, descriptors, NULL);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, NULL);
        if (layout) vkDestroyDescriptorSetLayout(device, layout, NULL);
        if (shader) vkDestroyShaderModule(device, shader, NULL);
        release_buffer(device, &readback); release_buffer(device, &storage); release_buffer(device, &upload);
        vkDestroyDevice(device, NULL);
    }
    if(inputFd>=0)close(inputFd);
    if(outputFd>=0)close(outputFd);
    if(node>=0)close(node);
    if (instance) vkDestroyInstance(instance, NULL);
    if(cacheProbe)return passed?0:1;
    printf("{\"passed\":%s,\"device\":", passed ? "true" : "false"); json_string(properties.deviceName);
    printf(",\"driver\":"); json_string(driver.driverName);
    printf(",\"driver_id\":%u,\"vendor_id\":%u,\"device_id\":%u,\"words_per_round\":%u,\"rounds\":%u,"
           "\"upload_memory_flags\":%u,\"readback_memory_flags\":%u,\"triangle_passed\":%s,\"sync_fd_readback\":%s,\"compositor_scene_passed\":%s",
           driver.driverID, properties.vendorID, properties.deviceID, WORDS, passed ? ROUNDS : 0,
           upload.flags, readback.flags, passed ? "true" : "false", passed && externalSync ? "true" : "false",
           passed && compositorTest ? "true" : "false");
    if (timestamping) timing_print(&timing);
    puts("}");
    return passed ? 0 : 1;
}
