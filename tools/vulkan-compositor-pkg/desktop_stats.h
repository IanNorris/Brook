#pragma once
/* Always-on, allocation-free renderer counters. Recording is pure in-memory
 * arithmetic; desktop_stats_format() renders a JSON snapshot that the
 * renderer publishes at most about once per second (see desktop_renderer.c).
 *
 * A displayed frame is a distinct client commit (window, incarnation,
 * commit) included in a composition whose present completed successfully.
 * It is counted once, at present completion. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "desktop_async_source_barrier.h"

enum {DESKTOP_STATS_WINDOWS=16,DESKTOP_STATS_BUCKETS=9};
/* Inclusive upper edges in ms; the final bucket is >100 ms. */
static const uint32_t desktop_stats_bucket_ms[DESKTOP_STATS_BUCKETS-1]={8,12,17,20,25,33,50,100};

struct DesktopStatsDuration {uint64_t count,sumNs,minNs,maxNs;};
struct DesktopStatsWindow {
    uint32_t window,lastGpu;uint64_t incarnation,lastUse;
    uint64_t displayed,repeated,stale,omitted,unidentified;
    uint64_t lastCommit,firstDisplayNs,lastDisplayNs;
    uint64_t hist[DESKTOP_STATS_BUCKETS];struct DesktopStatsDuration interval;
};
enum {DESKTOP_STATS_DIRECT=0,DESKTOP_STATS_NONDIRECT=1};
struct DesktopStatsClass {
    uint64_t frames;
    struct DesktopStatsDuration gpuTimestamp,submitToFence,prepare;
};
struct DesktopStats {
    uint64_t useClock,evictions;
    struct DesktopStatsWindow windows[DESKTOP_STATS_WINDOWS];
    uint64_t presents,full,partial,graphics,compute,directLayers;
    uint64_t presentFailures,presentStale,presentBusy,prepareFailures;
    struct DesktopStatsClass cls[2];
    uint64_t overlayCaptures,overlayFailures,overlayPolls,overlayMaxPolls;
    struct DesktopStatsDuration overlayWait;
};
/* One successfully presented composition. Timestamps are CLOCK_MONOTONIC ns;
 * zero means unknown and the dependent duration is not recorded. */
struct DesktopStatsFrame {
    const struct AsyncWindowSource *sources;unsigned count,direct;int partial,graphics;
    uint64_t presentedNs,startNs,submitStartNs,fenceNs,gpuNs;int gpuValid;
};

static inline unsigned desktop_stats_bucket(uint64_t ns){
    for(unsigned i=0;i<DESKTOP_STATS_BUCKETS-1;++i)if(ns<=(uint64_t)desktop_stats_bucket_ms[i]*1000000ull)return i;
    return DESKTOP_STATS_BUCKETS-1;
}
static inline void desktop_stats_duration(struct DesktopStatsDuration *d,uint64_t ns){
    if(!d->count || ns<d->minNs)d->minNs=ns;
    if(ns>d->maxNs)d->maxNs=ns;
    ++d->count;d->sumNs+=ns;
}
static inline void desktop_stats_span(struct DesktopStatsDuration *d,uint64_t from,uint64_t to){
    if(from && to>=from)desktop_stats_duration(d,to-from);
}
/* Find or allocate the (window, incarnation) entry, evicting the least
 * recently used entry when the table is full. */
static inline struct DesktopStatsWindow *desktop_stats_window(struct DesktopStats *s,uint32_t window,uint64_t incarnation){
    struct DesktopStatsWindow *victim=&s->windows[0];
    for(unsigned i=0;i<DESKTOP_STATS_WINDOWS;++i){struct DesktopStatsWindow *w=&s->windows[i];
        if(w->window==window && w->incarnation==incarnation && w->lastUse){w->lastUse=++s->useClock;return w;}
        if(!w->lastUse){if(victim->lastUse)victim=w;}
        else if(victim->lastUse && w->lastUse<victim->lastUse)victim=w;
    }
    if(victim->lastUse)++s->evictions;
    memset(victim,0,sizeof(*victim));
    victim->window=window;victim->incarnation=incarnation;victim->lastUse=++s->useClock;
    return victim;
}
static inline void desktop_stats_source(struct DesktopStats *s,const struct AsyncWindowSource *src,uint64_t presentedNs){
    if(!src->window || !src->incarnation)return;
    struct DesktopStatsWindow *w=desktop_stats_window(s,src->window,src->incarnation);
    if(src->omitted){++w->omitted;return;}
    const uint64_t commit=src->commit?src->commit:src->snapshotCommit;
    if(!commit){++w->unidentified;return;}
    if(w->displayed && commit==w->lastCommit){++w->repeated;return;}
    if(w->displayed && commit<w->lastCommit){++w->stale;return;}
    if(w->displayed && w->lastDisplayNs && presentedNs>=w->lastDisplayNs){
        const uint64_t ns=presentedNs-w->lastDisplayNs;
        ++w->hist[desktop_stats_bucket(ns)];desktop_stats_duration(&w->interval,ns);
    }
    if(!w->displayed)w->firstDisplayNs=presentedNs;
    ++w->displayed;w->lastCommit=commit;w->lastDisplayNs=presentedNs;w->lastGpu=src->commit!=0;
}
static inline void desktop_stats_presented(struct DesktopStats *s,const struct DesktopStatsFrame *f){
    ++s->presents;
    if(f->partial)++s->partial;else ++s->full;
    if(f->graphics)++s->graphics;else ++s->compute;
    s->directLayers+=f->direct;
    struct DesktopStatsClass *c=&s->cls[f->direct?DESKTOP_STATS_DIRECT:DESKTOP_STATS_NONDIRECT];
    ++c->frames;
    if(f->gpuValid)desktop_stats_duration(&c->gpuTimestamp,f->gpuNs);
    desktop_stats_span(&c->submitToFence,f->submitStartNs,f->fenceNs);
    desktop_stats_span(&c->prepare,f->startNs,f->submitStartNs);
    const unsigned n=f->count<=BROOK_DESKTOP_LAYER_LIMIT?f->count:BROOK_DESKTOP_LAYER_LIMIT;
    for(unsigned i=0;f->sources && i<n;++i)desktop_stats_source(s,&f->sources[i],f->presentedNs);
}
static inline void desktop_stats_overlay(struct DesktopStats *s,int ok,unsigned polls,uint64_t waitNs){
    ++s->overlayCaptures;if(!ok)++s->overlayFailures;
    s->overlayPolls+=polls;if(polls>s->overlayMaxPolls)s->overlayMaxPolls=polls;
    desktop_stats_duration(&s->overlayWait,waitNs);
}

struct DesktopStatsBuffer {char *data;size_t size,used;int overflow;};
static inline void desktop_stats_put(struct DesktopStatsBuffer *b,const char *format,...)
    __attribute__((format(printf,2,3)));
static inline void desktop_stats_put(struct DesktopStatsBuffer *b,const char *format,...){
    if(b->overflow)return;
    va_list args;va_start(args,format);
    const int n=vsnprintf(b->data+b->used,b->size-b->used,format,args);va_end(args);
    if(n<0 || (size_t)n>=b->size-b->used){b->overflow=1;return;}
    b->used+=(size_t)n;
}
static inline void desktop_stats_put_duration(struct DesktopStatsBuffer *b,const char *name,const struct DesktopStatsDuration *d){
    desktop_stats_put(b,"\"%s\":{\"count\":%llu,\"sum_ns\":%llu,\"min_ns\":%llu,\"max_ns\":%llu}",name,
        (unsigned long long)d->count,(unsigned long long)d->sumNs,(unsigned long long)d->minNs,(unsigned long long)d->maxNs);
}
struct DesktopStatsMeta {uint64_t seq,nowNs,startedNs,publishFailures;long pid;unsigned depth;int asyncPresent,gpuTimestamps;};
/* Returns the JSON length, or 0 when the buffer is too small. */
static inline size_t desktop_stats_format(const struct DesktopStats *s,const struct DesktopStatsMeta *m,char *data,size_t size){
    struct DesktopStatsBuffer b={data,size,0,0};
    if(!data || !size)return 0;
    desktop_stats_put(&b,"{\"schema\":\"brook-desktop-stats/1\",\"pid\":%ld,\"seq\":%llu,\"monotonic_ns\":%llu,\"started_ns\":%llu,"
        "\"publish_failures\":%llu,\"mode\":\"%s\",\"depth\":%u,\"async_present\":%d,\"gpu_timestamps\":%d,",
        m->pid,(unsigned long long)m->seq,(unsigned long long)m->nowNs,(unsigned long long)m->startedNs,
        (unsigned long long)m->publishFailures,
        m->depth?"async":"sync",m->depth,m->asyncPresent,m->gpuTimestamps);
    desktop_stats_put(&b,"\"interval_bucket_le_ms\":[");
    for(unsigned i=0;i<DESKTOP_STATS_BUCKETS-1;++i)desktop_stats_put(&b,"%u,",desktop_stats_bucket_ms[i]);
    desktop_stats_put(&b,"null],");
    desktop_stats_put(&b,"\"presents\":{\"completed\":%llu,\"full\":%llu,\"partial\":%llu,\"graphics\":%llu,\"compute\":%llu,"
        "\"failed\":%llu,\"stale\":%llu,\"busy_retries\":%llu,\"prepare_failures\":%llu,\"direct_layers\":%llu},",
        (unsigned long long)s->presents,(unsigned long long)s->full,(unsigned long long)s->partial,
        (unsigned long long)s->graphics,(unsigned long long)s->compute,(unsigned long long)s->presentFailures,
        (unsigned long long)s->presentStale,(unsigned long long)s->presentBusy,(unsigned long long)s->prepareFailures,
        (unsigned long long)s->directLayers);
    desktop_stats_put(&b,"\"classes\":{");
    static const char *const names[2]={"direct","nondirect"};
    for(unsigned i=0;i<2;++i){const struct DesktopStatsClass *c=&s->cls[i];
        desktop_stats_put(&b,"%s\"%s\":{\"frames\":%llu,",i?",":"",names[i],(unsigned long long)c->frames);
        desktop_stats_put_duration(&b,"gpu_timestamp",&c->gpuTimestamp);desktop_stats_put(&b,",");
        desktop_stats_put_duration(&b,"cpu_submit_to_fence_observed",&c->submitToFence);desktop_stats_put(&b,",");
        desktop_stats_put_duration(&b,"cpu_prepare",&c->prepare);desktop_stats_put(&b,"}");
    }
    desktop_stats_put(&b,"},\"overlay\":{\"captures\":%llu,\"failures\":%llu,\"polls\":%llu,\"max_polls\":%llu,",
        (unsigned long long)s->overlayCaptures,(unsigned long long)s->overlayFailures,
        (unsigned long long)s->overlayPolls,(unsigned long long)s->overlayMaxPolls);
    desktop_stats_put_duration(&b,"wait",&s->overlayWait);
    desktop_stats_put(&b,"},\"window_evictions\":%llu,\"windows\":[",(unsigned long long)s->evictions);
    int first=1;
    for(unsigned i=0;i<DESKTOP_STATS_WINDOWS;++i){const struct DesktopStatsWindow *w=&s->windows[i];
        if(!w->lastUse)continue;
        desktop_stats_put(&b,"%s{\"window\":%u,\"incarnation\":%llu,\"source\":\"%s\",\"displayed\":%llu,\"repeated\":%llu,"
            "\"stale\":%llu,\"omitted\":%llu,\"unidentified\":%llu,\"last_commit\":%llu,\"first_display_ns\":%llu,\"last_display_ns\":%llu,\"hist\":[",
            first?"":",",w->window,(unsigned long long)w->incarnation,!w->displayed?"none":w->lastGpu?"gpu":"cpu",
            (unsigned long long)w->displayed,(unsigned long long)w->repeated,(unsigned long long)w->stale,
            (unsigned long long)w->omitted,(unsigned long long)w->unidentified,(unsigned long long)w->lastCommit,
            (unsigned long long)w->firstDisplayNs,(unsigned long long)w->lastDisplayNs);
        for(unsigned j=0;j<DESKTOP_STATS_BUCKETS;++j)desktop_stats_put(&b,"%s%llu",j?",":"",(unsigned long long)w->hist[j]);
        desktop_stats_put(&b,"],");desktop_stats_put_duration(&b,"interval",&w->interval);desktop_stats_put(&b,"}");
        first=0;
    }
    desktop_stats_put(&b,"]}\n");
    return b.overflow?0:b.used;
}
