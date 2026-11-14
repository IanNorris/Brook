#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "desktop_stats.h"

static struct DesktopStats stats;
static void present(uint32_t window,uint64_t incarnation,uint64_t commit,uint64_t snapshot,uint32_t omitted,uint64_t ns,unsigned direct){
    struct AsyncWindowSource source={incarnation,1,commit,snapshot,window,omitted};
    const struct DesktopStatsFrame frame={&source,1,direct,0,0,ns,ns-3000000,ns-1000000,ns-500000,direct?200000:0,direct!=0,0,0,0,0,0};
    desktop_stats_presented(&stats,&frame);
}
static struct DesktopStatsWindow *find(uint32_t window,uint64_t incarnation){
    for(unsigned i=0;i<DESKTOP_STATS_WINDOWS;++i)if(stats.windows[i].lastUse && stats.windows[i].window==window &&
        stats.windows[i].incarnation==incarnation)return &stats.windows[i];
    return NULL;
}
int main(void){
    assert(desktop_stats_bucket(0)==0);
    assert(desktop_stats_bucket(8000000)==0 && desktop_stats_bucket(8000001)==1);
    assert(desktop_stats_bucket(12000000)==1 && desktop_stats_bucket(16700000)==2);
    assert(desktop_stats_bucket(17000000)==2 && desktop_stats_bucket(17000001)==3);
    assert(desktop_stats_bucket(33000000)==5 && desktop_stats_bucket(50000000)==6);
    assert(desktop_stats_bucket(100000000)==7 && desktop_stats_bucket(100000001)==8);

    const uint64_t base=1000000000ull;
    present(3,9,10,0,0,base,1);
    present(3,9,10,0,0,base+16000000,1);           /* repeated present of commit 10 */
    present(3,9,11,0,0,base+33000000,1);           /* 33 ms after commit 10 */
    present(3,9,12,0,0,base+49000000,1);           /* 16 ms */
    present(3,9,11,0,0,base+60000000,1);           /* older commit: stale */
    present(3,9,0,0,1,base+70000000,0);            /* culled in a partial frame */
    present(3,9,0,0,0,base+80000000,0);            /* no commit identity */
    present(3,9,0,13,0,base+300000000,0);          /* CPU snapshot commit, 251 ms */
    struct DesktopStatsWindow *w=find(3,9);
    assert(w && w->displayed==4 && w->repeated==1 && w->stale==1 && w->omitted==1 && w->unidentified==1);
    assert(w->lastCommit==13 && !w->lastGpu && w->firstDisplayNs==base && w->lastDisplayNs==base+300000000);
    assert(w->interval.count==3 && w->hist[2]==1 && w->hist[5]==1 && w->hist[8]==1);
    assert(w->interval.minNs==16000000 && w->interval.maxNs==251000000 && w->interval.sumNs==300000000);
    assert(stats.presents==8 && stats.full==8 && stats.compute==8 && stats.directLayers==5);
    assert(stats.cls[DESKTOP_STATS_DIRECT].frames==5 && stats.cls[DESKTOP_STATS_NONDIRECT].frames==3);
    assert(stats.cls[DESKTOP_STATS_DIRECT].gpuTimestamp.count==5 && stats.cls[DESKTOP_STATS_NONDIRECT].gpuTimestamp.count==0);
    assert(stats.cls[DESKTOP_STATS_DIRECT].prepare.sumNs==5*2000000ull);
    assert(stats.cls[DESKTOP_STATS_DIRECT].submitToFence.maxNs==500000);

    /* A new incarnation is a separate entry. */
    present(3,10,1,0,0,base+400000000,1);
    assert(find(3,10)->displayed==1 && find(3,9)->displayed==4 && !stats.evictions);

    /* Fill the table, then touch (3,9) so (3,10) becomes least recently used. */
    for(uint32_t i=0;i<DESKTOP_STATS_WINDOWS-2;++i)present(20+i,1,1,0,0,base+500000000+i,0);
    assert(!stats.evictions);
    present(3,9,14,0,0,base+600000000,1);
    present(99,1,5,0,0,base+700000000,0);
    assert(stats.evictions==1 && !find(3,10) && find(3,9) && find(99,1) && find(3,9)->displayed==5);
    present(3,10,2,0,0,base+800000000,1);          /* restarted entry evicts window 20 */
    assert(stats.evictions==2 && find(3,10)->displayed==1 && !find(20,1));

    desktop_stats_overlay(&stats,1,3,25000000);desktop_stats_overlay(&stats,0,500,0);
    assert(stats.overlayCaptures==2 && stats.overlayFailures==1 && stats.overlayPolls==503 && stats.overlayMaxPolls==500);
    assert(stats.overlayWait.minNs==0 && stats.overlayWait.maxNs==25000000);

    /* Composition work per class, the path counters and the last frame. */
    {const struct DesktopStatsFrame quad={NULL,0,1,1,0,base+850000000,0,0,0,0,0,DESKTOP_STATS_PATH_QUAD,3,7,4000000,3686400};
     const struct DesktopStatsFrame small={NULL,0,0,1,0,base+860000000,0,0,0,0,0,DESKTOP_STATS_PATH_QUAD,2,3,20000,10240};
     const uint64_t before=stats.cls[DESKTOP_STATS_DIRECT].work.pixelsShaded;
     desktop_stats_presented(&stats,&quad);desktop_stats_presented(&stats,&small);
     assert(stats.quad==2 && stats.cls[DESKTOP_STATS_DIRECT].work.pixelsShaded==before+4000000);
     assert(stats.cls[DESKTOP_STATS_NONDIRECT].work.maxLayersDrawn>=2 && stats.last.path==DESKTOP_STATS_PATH_QUAD);
     assert(stats.last.pixelsShaded==20000 && stats.last.damagePixels==10240 && stats.last.draws==3);
     stats.idleSkips=5;stats.keepalives=1;}
    /* Pipeline depth and forced full frames. */
    desktop_stats_in_flight(&stats,1);desktop_stats_in_flight(&stats,2);desktop_stats_in_flight(&stats,1);
    desktop_stats_submitted(&stats,0,0,DESKTOP_FULL_NO_BASELINE);
    desktop_stats_submitted(&stats,1,1,DESKTOP_FULL_NOT_FORCED);
    desktop_stats_submitted(&stats,1,1,DESKTOP_FULL_IN_FLIGHT);
    desktop_stats_submitted(&stats,5,0,DESKTOP_FULL_OTHER);
    assert(stats.pipeline.inFlight==1 && stats.pipeline.maxInFlight==2 && stats.pipeline.carried==2);
    assert(stats.pipeline.submitted[0]==1 && stats.pipeline.submitted[1]==2 && stats.pipeline.submitted[2]==1);
    assert(stats.fullForced.total==3 && stats.fullForced.noBaseline==1 && stats.fullForced.inFlight==1 && stats.fullForced.other==1);
    const struct DesktopStatsMeta meta={7,base+900000000,base,0,42,2,1,0,2};
    char small[64];assert(!desktop_stats_format(&stats,&meta,small,sizeof(small)));
    static char json[32768];
    const size_t length=desktop_stats_format(&stats,&meta,json,sizeof(json));
    assert(length && length==strlen(json) && json[length-1]=='\n');
    assert(strstr(json,"\"seq\":7,") && strstr(json,"\"window\":3,\"incarnation\":9,"));
    assert(strstr(json,"\"quad\":2,") && strstr(json,"\"skipped_polls\":5,") && strstr(json,"\"last_frame\":{\"path\":\"quad\""));
    assert(strstr(json,"\"composition\":{\"frames\":"));
    assert(strstr(json,"\"pipeline\":{\"depth\":2,\"in_flight\":1,\"max_in_flight\":2,\"submitted_by_in_flight\":[1,2,1],\"carried_damage\":2,"));
    assert(strstr(json,"\"full_forced\":{\"total\":3,\"no_baseline\":1,\"in_flight\":1,\"other\":1}"));
    /* Worst case: every table entry populated with large values still fits. */
    for(unsigned i=0;i<DESKTOP_STATS_WINDOWS;++i){stats.windows[i].displayed=UINT64_MAX;stats.windows[i].lastCommit=UINT64_MAX;
        stats.windows[i].incarnation=UINT64_MAX;stats.windows[i].window=UINT32_MAX;for(unsigned j=0;j<DESKTOP_STATS_BUCKETS;++j)stats.windows[i].hist[j]=UINT64_MAX;}
    for(unsigned i=0;i<2;++i){struct DesktopStatsWork *w=&stats.cls[i].work;w->frames=w->layersDrawn=w->draws=w->pixelsShaded=
        w->damagePixels=w->maxLayersDrawn=w->maxPixelsShaded=UINT64_MAX;}
    stats.last.pixelsShaded=stats.last.damagePixels=stats.last.submitToFenceNs=stats.last.gpuNs=UINT64_MAX;
    stats.idleSkips=stats.keepalives=stats.quad=UINT64_MAX;
    stats.pipeline.inFlight=UINT32_MAX;stats.pipeline.maxInFlight=stats.pipeline.carried=stats.pipeline.rebased=stats.pipeline.waitFull=
        stats.pipeline.waitBaseline=stats.pipeline.overlayAdopted=UINT64_MAX;for(unsigned i=0;i<3;++i)stats.pipeline.submitted[i]=UINT64_MAX;
    stats.fullForced.total=stats.fullForced.noBaseline=stats.fullForced.inFlight=stats.fullForced.other=UINT64_MAX;
    static char worst[32768];assert(desktop_stats_format(&stats,&meta,worst,sizeof(worst)));
    fputs(json,stdout);
    return 0;
}
