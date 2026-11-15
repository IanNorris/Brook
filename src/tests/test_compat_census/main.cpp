// Compat census table: one entry per key, bounded probing with an overflow
// counter, reset, sorted snapshots, path normalization, and exact counts
// under concurrent writers.
#include "../../kernel/src/compat_census.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK failed %s:%d %s\n",__FILE__,__LINE__,#x); std::abort(); } } while(0)
using namespace brook;

namespace {
CompatKey Ioctl(uint64_t request, uint64_t fdType = 1)
{
    CompatKey k{};k.kind=CompatKind::Ioctl;k.nr=16;k.error=25;k.a=fdType;k.b=request;return k;
}

void BasicCounts()
{
    static CompatCensusTable<64> t{};
    CHECK(t.Note(Ioctl(0x5401),nullptr,10,7,"bash")==CompatNote::New);
    CHECK(t.Note(Ioctl(0x5401),nullptr,20,8,"other")==CompatNote::Counted);
    CHECK(t.Note(Ioctl(0x5401,2),nullptr,30,9,"x")==CompatNote::New);   // fd type is part of the key
    CompatKey e=Ioctl(0x5401);e.error=22;
    CHECK(t.Note(e,nullptr,40,9,"x")==CompatNote::New);                // errno is part of the key
    CompatKey p{};p.kind=CompatKind::Path;p.nr=257;p.error=2;p.textHash=CompatTextHash("/proc/N/foo");
    CHECK(t.Note(p,"/proc/N/foo",50,3,"firefox-with-a-long-name")==CompatNote::New);
    CHECK(t.Note(p,"/proc/N/foo",60,4,"y")==CompatNote::Counted);
    CHECK(t.Note(p,"/proc/N/foo",70,4,"y")==CompatNote::Counted);
    CHECK(t.total==7 && t.overflow==0);

    CompatEntry out[8];uint32_t used=0;
    const uint32_t n=t.Snapshot(out,8,&used);
    CHECK(n==4 && used==4);
    CHECK(out[0].key.kind==CompatKind::Path && out[0].count==3);
    CHECK(std::strcmp(out[0].text,"/proc/N/foo")==0);
    CHECK(out[0].firstTick==50 && out[0].lastTick==70 && out[0].firstPid==3);
    CHECK(std::strlen(out[0].firstName)==CompatNameBytes-1);           // truncated, terminated
    CHECK(out[1].count==2 && out[1].key.b==0x5401 && out[1].key.a==1 && out[1].firstPid==7);
    CHECK(std::strcmp(out[1].firstName,"bash")==0 && out[1].lastTick==20);
    CHECK(out[2].count==1 && out[3].count==1);

    // Bounded snapshot keeps the top entries.
    CompatEntry top[2];
    CHECK(t.Snapshot(top,2,&used)==2 && used==4);
    CHECK(top[0].count==3 && top[1].count==2);

    t.Reset();
    CHECK(t.Snapshot(out,8,&used)==0 && used==0 && t.total==0 && t.resets==1);
    CHECK(t.Note(Ioctl(0x5401),nullptr,80,1,"a")==CompatNote::New);
    CHECK(t.Snapshot(out,8,&used)==1 && out[0].count==1 && out[0].firstTick==80);
}

void Overflow()
{
    static CompatCensusTable<16,16> t{};
    for (uint64_t i=0;i<16;++i) CHECK(t.Note(Ioctl(i),nullptr,i,1,"a")==CompatNote::New);
    CHECK(t.Note(Ioctl(100),nullptr,0,1,"a")==CompatNote::Overflow);
    CHECK(t.Note(Ioctl(101),nullptr,0,1,"a")==CompatNote::Overflow);
    CHECK(t.overflow==2 && t.total==18);
    for (uint64_t i=0;i<16;++i) CHECK(t.Note(Ioctl(i),nullptr,i,1,"a")==CompatNote::Counted);
    CHECK(t.overflow==2);
}

void Concurrent()
{
    static CompatCensusTable<512> t{};
    constexpr unsigned threads=8,keys=200,rounds=500;
    std::vector<std::thread> pool;
    for (unsigned w=0;w<threads;++w)
        pool.emplace_back([w]{
            for (unsigned r=0;r<rounds;++r)
                for (unsigned k=0;k<keys;++k)
                    t.Note(Ioctl(0x1000+k),nullptr,r,w,"worker");
        });
    for (auto& th:pool) th.join();
    static CompatEntry out[512];uint32_t used=0;
    const uint32_t n=t.Snapshot(out,512,&used);
    uint64_t sum=0;for (uint32_t i=0;i<n;++i) sum+=out[i].count;
    CHECK(t.overflow==0);
    CHECK(sum==uint64_t(threads)*keys*rounds);
    CHECK(t.total==sum);
    // A racing creator may give up waiting and make a duplicate, but with a
    // bounded fill it publishes within the spin budget.
    CHECK(n>=keys && n<=keys+threads);
    for (uint32_t i=1;i<n;++i) CHECK(out[i-1].count>=out[i].count);
}

void Paths()
{
    char out[CompatTextBytes];
    CHECK(CompatNormalizePath("/proc/1234/status",out,sizeof(out)) && std::strcmp(out,"/proc/N/status")==0);
    CHECK(CompatNormalizePath("/proc/self/task/77/stat",out,sizeof(out)) && std::strcmp(out,"/proc/self/task/N/stat")==0);
    CHECK(CompatNormalizePath("/sys/devices/system/cpu/cpu0/cpufreq",out,sizeof(out)) &&
          std::strcmp(out,"/sys/devices/system/cpu/cpu0/cpufreq")==0);
    CHECK(CompatNormalizePath("/dev/dri/card0",out,sizeof(out)) && std::strcmp(out,"/dev/dri/card0")==0);
    CHECK(CompatNormalizePath("/proc/12ab",out,sizeof(out)) && std::strcmp(out,"/proc/12ab")==0);
    CHECK(CompatNormalizePath("/proc",out,sizeof(out)) && std::strcmp(out,"/proc")==0);
    CHECK(CompatNormalizePath("/proc/self/fd/3",out,sizeof(out)) && std::strcmp(out,"/proc/self/fd/N")==0);
    CHECK(!CompatNormalizePath("/processes",out,sizeof(out)));
    CHECK(!CompatNormalizePath("/usr/lib/libc.so",out,sizeof(out)));
    CHECK(!CompatNormalizePath("proc/self",out,sizeof(out)));
    CHECK(!CompatNormalizePath(nullptr,out,sizeof(out)));
    char small[8];
    CHECK(CompatNormalizePath("/sys/class/drm/card0",small,sizeof(small)) && std::strlen(small)==7);
    CHECK(CompatTextHash("")==0 && CompatTextHash("a")!=0 && CompatTextHash("a")!=CompatTextHash("b"));
}
}

int main()
{
    BasicCounts();
    Overflow();
    Concurrent();
    Paths();
    std::puts("test_compat_census: ok");
    return 0;
}
