// Linux-only diagnostic, bounded storage, no per-frame output or extra waits.
#include <time.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
enum { BROOK_TIMING_CAPACITY = 8192 };
typedef struct {
    uint64_t cycle_start, cpu[6], gpu[2];
    unsigned submitted, ready;
} BrookTimingRow;
static struct {
    int initialized, enabled;
    unsigned count, errors, overflow, bits;
    float period;
    VkQueryPool pool;
    PFN_vkCmdResetQueryPool reset;
    PFN_vkCmdWriteTimestamp write;
    PFN_vkGetQueryPoolResults get;
    PFN_vkDestroyQueryPool destroy;
    int pending[FRAME_LAG];
    BrookTimingRow rows[BROOK_TIMING_CAPACITY];
    BrookTimingRow* current;
} brook_timing;

static uint64_t brook_timing_clock(void) {
    if (!brook_timing.enabled) return 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0) { ++brook_timing.errors; return 0; }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static void brook_timing_init(VkPhysicalDevice physical, VkDevice device, unsigned family) {
    if (brook_timing.initialized) return;
    brook_timing.initialized = 1;
    if (!getenv("BROOK_CUBE_TRACE")) return;
    for (unsigned i=0;i<FRAME_LAG;++i) brook_timing.pending[i]=-1;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,NULL);
    VkQueueFamilyProperties queues[64];
    if (!count || count>64 || family>=count) { ++brook_timing.errors; return; }
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues);
    brook_timing.bits=queues[family].timestampValidBits;
    brook_timing.period=properties.limits.timestampPeriod;
    if (!brook_timing.bits || brook_timing.bits>64 || !(brook_timing.period>0)) { ++brook_timing.errors; return; }
    PFN_vkCreateQueryPool create=(PFN_vkCreateQueryPool)vkGetDeviceProcAddr(device,"vkCreateQueryPool");
    brook_timing.reset=(PFN_vkCmdResetQueryPool)vkGetDeviceProcAddr(device,"vkCmdResetQueryPool");
    brook_timing.write=(PFN_vkCmdWriteTimestamp)vkGetDeviceProcAddr(device,"vkCmdWriteTimestamp");
    brook_timing.get=(PFN_vkGetQueryPoolResults)vkGetDeviceProcAddr(device,"vkGetQueryPoolResults");
    brook_timing.destroy=(PFN_vkDestroyQueryPool)vkGetDeviceProcAddr(device,"vkDestroyQueryPool");
    if (!create || !brook_timing.reset || !brook_timing.write || !brook_timing.get || !brook_timing.destroy) { ++brook_timing.errors; return; }
    VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=FRAME_LAG*2};
    if (create(device,&info,NULL,&brook_timing.pool)!=VK_SUCCESS) { ++brook_timing.errors; return; }
    brook_timing.enabled=1;
}
static void brook_timing_reap(VkDevice device, unsigned slot) {
    if (!brook_timing.enabled || brook_timing.pending[slot]<0) return;
    struct { uint64_t ticks,available; } values[2]={{0}};
    VkResult status=brook_timing.get(device,brook_timing.pool,slot*2,2,sizeof(values),values,sizeof(values[0]),
        VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    BrookTimingRow* row=&brook_timing.rows[brook_timing.pending[slot]];
    if (status!=VK_SUCCESS || !values[0].available || !values[1].available) ++brook_timing.errors;
    else { row->gpu[0]=values[0].ticks;row->gpu[1]=values[1].ticks;row->ready=1; }
    brook_timing.pending[slot]=-1;
}
static void brook_timing_new(unsigned slot, uint64_t cycle_start) {
    (void)slot;
    brook_timing.current=NULL;
    if (!brook_timing.enabled) return;
    if (brook_timing.count==BROOK_TIMING_CAPACITY) { ++brook_timing.overflow;return; }
    brook_timing.current=&brook_timing.rows[brook_timing.count++];
    brook_timing.current->cycle_start=cycle_start;
    brook_timing.current->cpu[0]=brook_timing_clock();
}
static void brook_timing_cpu(unsigned point) {
    if (brook_timing.current) brook_timing.current->cpu[point]=brook_timing_clock();
}
static void brook_timing_begin(VkCommandBuffer command, unsigned slot) {
    if (!brook_timing.current) return;
    brook_timing.reset(command,brook_timing.pool,slot*2,2);
    brook_timing.write(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,brook_timing.pool,slot*2);
}
static void brook_timing_end(VkCommandBuffer command, unsigned slot) {
    if (brook_timing.current) brook_timing.write(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,brook_timing.pool,slot*2+1);
}
static void brook_timing_submitted(unsigned slot, VkResult result) {
    if (!brook_timing.current) return;
    if (result!=VK_SUCCESS) { ++brook_timing.errors;return; }
    if (brook_timing.pending[slot]>=0) ++brook_timing.errors;
    brook_timing.pending[slot]=(int)(brook_timing.current-brook_timing.rows);
    brook_timing.current->submitted=1;
}
static void brook_timing_finish(VkDevice device) {
    const char* path=getenv("BROOK_CUBE_TRACE");
    if (!path) return;
    // Called after the cube's existing device-idle wait; drain final slots.
    for (unsigned i=0;i<FRAME_LAG;++i) brook_timing_reap(device,i);
    if (brook_timing.pool) { brook_timing.destroy(device,brook_timing.pool,NULL);brook_timing.pool=VK_NULL_HANDLE; }
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if (fd<0) { fprintf(stderr,"BROOK_CUBE_TRACE_ERROR open\n");return; }
    FILE* file=fdopen(fd,"w");
    if (!file) { close(fd);fprintf(stderr,"BROOK_CUBE_TRACE_ERROR fdopen\n");return; }
    fprintf(file,"# bits=%u period_ns=%.9g errors=%u overflow=%u\n",brook_timing.bits,(double)brook_timing.period,brook_timing.errors,brook_timing.overflow);
    fprintf(file,"frame,cycle_start_ns,after_wait_ns,after_acquire_ns,before_submit_ns,after_submit_ns,before_present_ns,after_present_ns,gpu_start,gpu_end,submitted,ready\n");
    for (unsigned i=0;i<brook_timing.count;++i) {
        BrookTimingRow* row=&brook_timing.rows[i];
        fprintf(file,"%u,%" PRIu64,i,row->cycle_start);
        for (unsigned j=0;j<6;++j) fprintf(file,",%" PRIu64,row->cpu[j]);
        fprintf(file,",%" PRIu64 ",%" PRIu64 ",%u,%u\n",row->gpu[0],row->gpu[1],row->submitted,row->ready);
    }
    int failed=ferror(file);
    if (fclose(file)) failed=1;
    fprintf(stderr,"BROOK_CUBE_TRACE_%s rows=%u errors=%u overflow=%u\n",failed?"ERROR":"SAVED",brook_timing.count,brook_timing.errors,brook_timing.overflow);
}
