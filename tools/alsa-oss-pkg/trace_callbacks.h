/* Optional diagnostic build only. Record callback results without querying
 * ALSA or the device. Bounded snapshots use private per-process /tmp files.
 * The normal package neither compiles nor enables this recorder. */
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

enum { TraceCapacity = 256, TraceTail = 24, TraceErrorLimit = 8 };
struct oss_trace_entry {
    uint64_t begin, end;
    unsigned long sequence;
    long tid, result;
    const void *io;
    const char *operation;
    snd_pcm_uframes_t request, appl, hw, expected, boundary, buffer, period;
};
static struct oss_trace_entry oss_trace_entries[TraceCapacity];
static unsigned long oss_trace_sequence;
static unsigned oss_trace_errors, oss_trace_stops;
static unsigned oss_trace_snapshots, oss_trace_first_write;
static uint64_t oss_trace_next_snapshot;
static struct oss_trace_entry oss_trace_writes[16];
static unsigned long oss_trace_write_sequence;
static pthread_mutex_t oss_trace_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t oss_trace_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void oss_trace_dump_locked(const char *reason)
{
    char path[96];
    snprintf(path, sizeof(path), "/tmp/brook-alsa-trace-%ld.log", (long)getpid());
    FILE *output = fopen(path, "a");
    if (!output) output = stderr;
    unsigned long first = oss_trace_sequence > TraceTail ? oss_trace_sequence - TraceTail : 0;
    fprintf(output, "BROOK_ALSA_TRACE reason=%s pid=%ld sequence=%lu errors=%u stops=%u retained=%lu writes=%lu snapshots=%u\n",
        reason, (long)getpid(), oss_trace_sequence, oss_trace_errors, oss_trace_stops,
        oss_trace_sequence - first, oss_trace_write_sequence, oss_trace_snapshots);
    for (unsigned long n = first; n < oss_trace_sequence; ++n) {
        const struct oss_trace_entry *e = &oss_trace_entries[n % TraceCapacity];
        fprintf(output, "BROOK_ALSA_EVENT seq=%lu tid=%ld io=%p op=%s begin=%llu end=%llu result=%ld request=%lu appl=%lu hw=%lu expected=%lu boundary=%lu buffer=%lu period=%lu\n",
            e->sequence, e->tid, e->io, e->operation,
            (unsigned long long)e->begin, (unsigned long long)e->end, e->result,
            e->request, e->appl, e->hw, e->expected, e->boundary, e->buffer, e->period);
    }
    first = oss_trace_write_sequence > 8 ? oss_trace_write_sequence - 8 : 0;
    for (unsigned long n = first; n < oss_trace_write_sequence; ++n) {
        const struct oss_trace_entry *e = &oss_trace_writes[n % 16];
        fprintf(output, "BROOK_ALSA_WRITE seq=%lu tid=%ld io=%p begin=%llu end=%llu result=%ld request=%lu appl=%lu hw=%lu expected=%lu buffer=%lu\n",
            e->sequence, e->tid, e->io, (unsigned long long)e->begin,
            (unsigned long long)e->end, e->result, e->request,
            e->appl, e->hw, e->expected, e->buffer);
    }
    if (output != stderr) fclose(output);
}

static void oss_trace_record(snd_pcm_ioplug_t *io, const char *operation,
    uint64_t begin, long result, snd_pcm_uframes_t request, int stopping)
{
    snd_pcm_oss_t *oss = io->private_data;
    uint64_t end = oss_trace_now();
    pthread_mutex_lock(&oss_trace_mutex);
    unsigned long sequence = oss_trace_sequence++;
    oss_trace_entries[sequence % TraceCapacity] = (struct oss_trace_entry) {
        .begin=begin, .end=end, .sequence=sequence, .tid=syscall(SYS_gettid),
        .result=result, .io=io, .operation=operation, .request=request,
        .appl=io->appl_ptr, .hw=io->hw_ptr, .expected=oss->expected_appl,
        .boundary=oss->boundary, .buffer=io->buffer_size, .period=io->period_size
    };
    int is_write = strcmp(operation, "write") == 0;
    if (is_write)
        oss_trace_writes[oss_trace_write_sequence++ % 16] = oss_trace_entries[sequence % TraceCapacity];
    if (is_write && !oss_trace_first_write) {
        oss_trace_first_write = 1;
        oss_trace_next_snapshot = end + 1000000000ULL;
        oss_trace_dump_locked("first_write");
    } else if (oss_trace_first_write && end >= oss_trace_next_snapshot && oss_trace_snapshots < 32) {
        ++oss_trace_snapshots;
        oss_trace_next_snapshot = end + 1000000000ULL;
        oss_trace_dump_locked("periodic");
    }
    if (result < 0 && result != -EAGAIN && oss_trace_errors++ < TraceErrorLimit)
        oss_trace_dump_locked("error");
    if (stopping && oss_trace_stops++ < TraceErrorLimit)
        oss_trace_dump_locked(operation);
    pthread_mutex_unlock(&oss_trace_mutex);
}

#define OSS_TRACE_CONTROL(name, stopping) \
static int trace_##name(snd_pcm_ioplug_t *io) { \
    uint64_t begin = oss_trace_now(); \
    int result = oss_##name(io); \
    oss_trace_record(io, #name, begin, result, 0, stopping); \
    return result; \
}
OSS_TRACE_CONTROL(start, 0)
OSS_TRACE_CONTROL(stop, 1)
OSS_TRACE_CONTROL(prepare, 0)
OSS_TRACE_CONTROL(drain, 1)
#undef OSS_TRACE_CONTROL

static snd_pcm_sframes_t trace_pointer(snd_pcm_ioplug_t *io)
{
    uint64_t begin = oss_trace_now();
    snd_pcm_sframes_t result = oss_pointer(io);
    oss_trace_record(io, "pointer", begin, result, 0, 0);
    return result;
}
static int trace_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delay)
{
    uint64_t begin = oss_trace_now();
    int result = oss_delay(io, delay);
    oss_trace_record(io, "delay", begin, result, result < 0 ? 0 : *delay, 0);
    return result;
}
static snd_pcm_sframes_t trace_write(snd_pcm_ioplug_t *io,
    const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
    snd_pcm_uframes_t size)
{
    uint64_t begin = oss_trace_now();
    snd_pcm_sframes_t result = oss_write(io, areas, offset, size);
    oss_trace_record(io, "write", begin, result, size, 0);
    return result;
}
static int trace_close(snd_pcm_ioplug_t *io)
{
    /* oss_close frees private_data, so record before forwarding. */
    oss_trace_record(io, "close_entry", oss_trace_now(), 0, 0, 1);
    return oss_close(io);
}
