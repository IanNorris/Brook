// Opt-in capability/timing probe. Query reads follow the correctness probe's
// existing fence; no additional wait, queue-idle or flush is introduced here.
#include <time.h>
#include <inttypes.h>
enum { TIMING_POINTS = 4 };
typedef struct {
    VkQueryPool pool;
    uint32_t bits;
    float period;
    PFN_vkGetCalibratedTimestampsEXT calibrate;
    uint64_t ticks[ROUNDS][TIMING_POINTS];
    uint64_t cpu[ROUNDS][3]; // before submit, submit returned, completion returned
    uint64_t calibration[2][3]; // GPU, CLOCK_MONOTONIC, maximum deviation
    unsigned rounds, calibrations;
} ProbeTiming;

static int monotonic_ns(uint64_t* value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0) return 0;
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return 1;
}

// Only enable a calibration extension if both GPU and guest monotonic domains
// are advertised. A guest CPU domain is not automatically the host CPU clock.
static int timing_calibration_supported(VkInstance instance, VkPhysicalDevice physical)
{
    uint32_t count = 0;
    VkExtensionProperties extensions[512];
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS || count > 512) return 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions) != VK_SUCCESS) return 0;
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) found = 1;
    if (!found) return 0;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains =
        (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
    VkTimeDomainEXT values[32];
    if (!domains || domains(physical, &count, NULL) != VK_SUCCESS || count > 32) return 0;
    if (domains(physical, &count, values) != VK_SUCCESS) return 0;
    int gpu = 0, cpu = 0;
    for (uint32_t i = 0; i < count; ++i) {
        gpu |= values[i] == VK_TIME_DOMAIN_DEVICE_EXT;
        cpu |= values[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
    }
    return gpu && cpu;
}

static VkResult timing_calibrate(VkDevice device, ProbeTiming* timing)
{
    if (!timing->calibrate) return VK_SUCCESS;
    VkCalibratedTimestampInfoEXT info[2] = {
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT},
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}};
    if (timing->calibrations >= 2) return VK_ERROR_UNKNOWN;
    uint64_t* result = timing->calibration[timing->calibrations];
    VkResult status = timing->calibrate(device, 2, info, result, &result[2]);
    if (status == VK_SUCCESS) ++timing->calibrations;
    return status;
}

static VkResult timing_read(VkDevice device, ProbeTiming* timing, unsigned round)
{
    struct { uint64_t ticks, available; } values[TIMING_POINTS] = {0};
    VkResult result = vkGetQueryPoolResults(device, timing->pool, round * TIMING_POINTS,
        TIMING_POINTS, sizeof(values), values, sizeof(values[0]),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS) return result;
    for (unsigned i = 0; i < TIMING_POINTS; ++i) {
        if (!values[i].available) return VK_NOT_READY;
        timing->ticks[round][i] = values[i].ticks;
    }
    ++timing->rounds;
    return VK_SUCCESS;
}

static void timing_print(const ProbeTiming* timing)
{
    printf(",\"timing\":{\"valid_bits\":%u,\"period_ns\":%.9g,\"cpu_domain\":\"CLOCK_MONOTONIC\",\"points\":[\"start\",\"upload_done\",\"compute_done\",\"readback_done\"],\"rounds\":[",
        timing->bits, (double)timing->period);
    for (unsigned r = 0; r < timing->rounds; ++r) {
        printf("%s{\"ticks\":[", r ? "," : "");
        for (unsigned i = 0; i < TIMING_POINTS; ++i) printf("%s%" PRIu64, i ? "," : "", timing->ticks[r][i]);
        printf("],\"cpu_ns\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]}", timing->cpu[r][0], timing->cpu[r][1], timing->cpu[r][2]);
    }
    printf("],\"calibrations\":[");
    for (unsigned i = 0; i < timing->calibrations; ++i)
        printf("%s{\"gpu_ticks\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"max_deviation_ns\":%" PRIu64 "}",
            i ? "," : "", timing->calibration[i][0], timing->calibration[i][1], timing->calibration[i][2]);
    printf("]}");
}
