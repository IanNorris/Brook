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

int main(int argc, char** argv)
{
    const char* expected = argc == 3 && !strcmp(argv[1], "--expect-driver") ? argv[2] : "hardware";
    if ((argc != 1 && argc != 3) ||
        (argc == 3 && strcmp(argv[1], "--expect-driver")) ||
        (strcmp(expected, "hardware") && strcmp(expected, "venus") && strcmp(expected, "intel"))) {
        fprintf(stderr, "usage: vulkan-probe [--expect-driver hardware|intel|venus]\n"); return 2;
    }
    int passed = 0;
    const int externalSync=getenv("BROOK_VULKAN_SYNC_FD")!=NULL;
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
        if (info.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
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
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    const char* deviceExtensions[]={VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount=externalSync?1:0,.ppEnabledExtensionNames=deviceExtensions };
    CHECK_VK(vkCreateDevice(physical, &deviceInfo, NULL, &device));
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
    CHECK_VK(make_buffer(device, &memory, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &upload));
    CHECK_VK(make_buffer(device, &memory, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &readback));
    CHECK_VK(make_buffer(device, &memory, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &storage));
    CHECK_VK(vkMapMemory(device, upload.memory, 0, VK_WHOLE_SIZE, 0, &upload.mapped));
    CHECK_VK(vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &readback.mapped));
    VkDescriptorSetLayoutBinding binding = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo layoutInfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding };
    CHECK_VK(vkCreateDescriptorSetLayout(device, &layoutInfo, NULL, &layout));
    VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(uint32_t) };
    VkPipelineLayoutCreateInfo pipelineInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    CHECK_VK(vkCreatePipelineLayout(device, &pipelineInfo, NULL, &pipelineLayout));
    VkShaderModuleCreateInfo shaderInfo = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(shaderCode), .pCode = shaderCode };
    CHECK_VK(vkCreateShaderModule(device, &shaderInfo, NULL, &shader));
    VkComputePipelineCreateInfo compute = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" }, .layout = pipelineLayout };
    CHECK_VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, NULL, &pipeline));
    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &poolSize };
    CHECK_VK(vkCreateDescriptorPool(device, &poolInfo, NULL, &descriptors));
    VkDescriptorSetAllocateInfo allocateSet = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &layout };
    VkDescriptorSet set;
    CHECK_VK(vkAllocateDescriptorSets(device, &allocateSet, &set));
    VkDescriptorBufferInfo bufferInfo = { storage.buffer, 0, bytes };
    VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufferInfo };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkCommandPoolCreateInfo commandInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    CHECK_VK(vkCreateCommandPool(device, &commandInfo, NULL, &commands));
    VkCommandBufferAllocateInfo allocateCommand = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer command;
    CHECK_VK(vkAllocateCommandBuffers(device, &allocateCommand, &command));
    VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK_VK(vkCreateFence(device, &fenceInfo, NULL, &fence));
    for (uint32_t round = 0; round < ROUNDS; ++round) {
        uint32_t seed = 7 + round * 31;
        uint32_t* input = upload.mapped;
        for (uint32_t i = 0; i < WORDS; ++i) input[i] = (i * 17u) ^ seed;
        if (!(upload.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = upload.memory, .size = VK_WHOLE_SIZE };
            CHECK_VK(vkFlushMappedMemoryRanges(device, 1, &range));
        }
        CHECK_VK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK_VK(vkBeginCommandBuffer(command, &begin));
        VkBufferCopy copy = { .size = bytes };
        vkCmdCopyBuffer(command, upload.buffer, storage.buffer, 1, &copy);
        VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        vkCmdFillBuffer(command, storage.buffer, 256, 512, 0x13579bdfu);
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
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
        vkCmdCopyBuffer(command, storage.buffer, readback.buffer, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK_VK(vkEndCommandBuffer(command));
        CHECK_VK(vkResetFences(device, 1, &fence));
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command,
            .signalSemaphoreCount=externalSync?1:0,.pSignalSemaphores=&exportedSemaphore };
        CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence));
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
        if (!(readback.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = readback.memory, .size = VK_WHOLE_SIZE };
            CHECK_VK(vkInvalidateMappedMemoryRanges(device, 1, &range));
        }
        const uint32_t* output = readback.mapped;
        for (uint32_t i = 0; i < WORDS; ++i) {
            uint32_t value = i >= 64 && i < 192 ? 0x13579bdfu : (i * 17u) ^ seed;
            uint32_t want=value;
            for(unsigned step=0;step<dispatches;++step)want=(want^(0x9e3779b9u+seed))*1664525u+1013904223u;
            if (output[i] != want) {
                fprintf(stderr, "READBACK_MISMATCH round=%u index=%u actual=%08x expected=%08x\n",
                        round, i, output[i], want); goto cleanup;
            }
        }
        // Readback above must be correct BEFORE this ordinary fence wait.
        if(externalSync)CHECK_VK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ULL));
    }
    CHECK_VK(render_triangle(device,physical,&memory,queue,command,fence,&readback));
    CHECK_VK(vkDeviceWaitIdle(device));
    passed = 1;
cleanup:
    if(exportedFd>=0)close(exportedFd);
    if (device) {
        // Process/VM supervisors also bound failures where a driver cannot finish.
        if (!passed) (void)vkDeviceWaitIdle(device);
        if(exportedSemaphore)vkDestroySemaphore(device,exportedSemaphore,NULL);
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
    if (instance) vkDestroyInstance(instance, NULL);
    printf("{\"passed\":%s,\"device\":", passed ? "true" : "false"); json_string(properties.deviceName);
    printf(",\"driver\":"); json_string(driver.driverName);
    printf(",\"driver_id\":%u,\"vendor_id\":%u,\"device_id\":%u,\"words_per_round\":%u,\"rounds\":%u,"
           "\"upload_memory_flags\":%u,\"readback_memory_flags\":%u,\"triangle_passed\":%s,\"sync_fd_readback\":%s}\n",
           driver.driverID, properties.vendorID, properties.deviceID, WORDS, passed ? ROUNDS : 0,
           upload.flags, readback.flags, passed ? "true" : "false", passed && externalSync ? "true" : "false");
    return passed ? 0 : 1;
}
