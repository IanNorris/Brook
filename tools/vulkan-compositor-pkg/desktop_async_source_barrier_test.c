#include <assert.h>
#include <stdio.h>
#include "desktop_async_source_barrier.h"
int main(void){
    struct AsyncSourceBarrier b[BROOK_DESKTOP_LAYER_LIMIT]={0};
    struct AsyncWindowSource direct={7,10,25,0,2,0};
    struct AsyncWindowSource cpu={7,10,0,0,2,0};
    assert(async_sources_presentable(b,&cpu,1));
    async_sources_committed(b,&direct,1);
    assert(!async_sources_presentable(b,&cpu,1));
    assert(async_sources_presentable(b,&direct,1));
    cpu.generation=11;assert(!async_sources_presentable(b,&cpu,1));
    cpu.snapshotCommit=24;assert(!async_sources_presentable(b,&cpu,1));
    cpu.snapshotCommit=25;assert(async_sources_presentable(b,&cpu,1));
    cpu.snapshotCommit=26;assert(async_sources_presentable(b,&cpu,1));
    async_sources_committed(b,&cpu,1);assert(b[1].commit==26);
    direct.commit=25;assert(!async_sources_presentable(b,&direct,1));
    direct.commit=26;assert(async_sources_presentable(b,&direct,1));
    struct AsyncWindowSource omitted={7,12,0,40,2,1};
    assert(async_sources_presentable(b,&omitted,1));async_sources_committed(b,&omitted,1);
    assert(b[1].commit==26);omitted.snapshotCommit=0;assert(async_sources_presentable(b,&omitted,1));
    struct AsyncWindowSource mapped[2]={direct,cpu};unsigned indices[2]={2,1};uint8_t used[3]={1,0,1};
    assert(async_sources_mark_omitted(mapped,2,indices,used,3) && !mapped[0].omitted && mapped[1].omitted);
    indices[1]=3;assert(!async_sources_mark_omitted(mapped,2,indices,used,3) && !mapped[0].omitted && mapped[1].omitted);
    direct.commit=24;assert(!async_sources_presentable(b,&direct,1));direct.commit=26;
    cpu.generation=1;cpu.incarnation=8;assert(async_sources_presentable(b,&cpu,1));
    direct.incarnation=8;direct.generation=1;direct.commit=27;async_sources_committed(b,&direct,1);
    assert(!async_sources_presentable(b,&cpu,1));
    struct AsyncWindowSource invalid={0};assert(!async_sources_presentable(b,&invalid,1));
    puts("DESKTOP_ASYNC_SOURCE_BARRIER_PASS");return 0;
}
