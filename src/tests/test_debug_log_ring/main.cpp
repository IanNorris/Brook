#include "debug_overlay.h"
#include "console_viewport.h"
#include <cassert>
#include <cstdio>
#include <cstring>
namespace brook { int DebugOverlayTestPanicSnapshotBusy(char*,uint32_t); }
int main(){
    auto small=brook::ConsoleFitInitial(800,556,800,1000,2,28);
    assert(small.x+small.width+4<=800 && small.y+small.height+32<=556);
    assert(small.x==20 && small.y==60 && small.width==776 && small.height==464);
    auto unscaled=brook::ConsoleFitInitial(3200,1712,800,1000,4,72);
    assert(unscaled.width==800 && unscaled.height==1000);
    auto large=brook::ConsoleFitInitial(3200,1712,1600,2000,4,56);
    assert(large.x==20 && large.y==60 && large.width==1600 && large.height==1588);
    auto tiny=brook::ConsoleFitInitial(4,4,800,500,2,28);
    assert(tiny.width==0 && tiny.height==0);
    assert(brook::ConsoleVisibleHeight(98,464,512)==414);
    assert(brook::ConsoleVisibleHeight(-20,464,512)==464);
    assert(brook::ConsoleVisibleHeight(600,464,512)==0);
    // Follow, hold a historical position during writes, recover when that
    // history wraps, and resume following at the newest line.
    brook::ConsoleViewport view;uint32_t start=0,count=0;
    view.Range(100,511,10,start,count);assert(start==90 && count==10 && view.following);
    view.Scroll(9,100,511,10);view.Range(105,511,10,start,count);
    assert(start==81 && count==10 && !view.following);
    view.Scroll(1000,105,511,10);view.Range(800,511,10,start,count);
    assert(start==289 && count==10 && !view.following);
    view.Scroll(-1000,800,511,10);view.Range(805,511,10,start,count);
    assert(start==795 && count==10 && view.following);
    view.Range(3,511,10,start,count);assert(start==0 && count==3);
    view.Range(3,511,0,start,count);assert(start==3 && count==0);
    brook::DebugOverlayInit();
    for(unsigned i=0;i<530;++i){char line[32];std::snprintf(line,sizeof(line),"L%03u\n",i);brook::DebugOverlayPuts(line);}
    brook::DebugOverlayPuts("PARTIAL");
    char tail[12]{};
    assert(brook::DebugOverlayTryPanicSnapshot(tail,sizeof(tail))==10);
    assert(!std::memcmp(tail,"L528\nL529\n",10));
    assert(brook::DebugOverlayTestPanicSnapshotBusy(tail,sizeof(tail))==-16);
    char lines[64][161]{};uint32_t cursor=0,total=0,skipped=0;unsigned expected=19;
    do{
        int n=brook::DebugOverlayReadRange(&cursor,&lines[0][0],64,161,&total,&skipped);
        assert(n>=0 && total==530);assert(skipped==(expected==19?19:0));
        for(int i=0;i<n;++i){char line[32];std::snprintf(line,sizeof(line),"L%03u",expected++);assert(!std::strcmp(lines[i],line));}
    }while(cursor<total);
    assert(cursor==530 && expected==530);
    assert(brook::DebugOverlayReadRange(&cursor,&lines[0][0],64,161,&total,&skipped)==0);
    brook::DebugOverlayPuts("\n");
    assert(brook::DebugOverlayReadRange(&cursor,&lines[0][0],64,161,&total,&skipped)==1);
    assert(!std::strcmp(lines[0],"PARTIAL") && cursor==531 && skipped==0);
    uint32_t future=532;assert(brook::DebugOverlayReadRange(&future,&lines[0][0],1,161,&total,&skipped)==-22);
    uint32_t legacy=0;assert(brook::DebugOverlayReadFrom(&legacy,&lines[0][0],1,161)==1);assert(!std::strcmp(lines[0],"L020"));
}
