#include "debug_overlay.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main(){
    brook::DebugOverlayInit();
    for(unsigned i=0;i<530;++i){char line[32];std::snprintf(line,sizeof(line),"L%03u\n",i);brook::DebugOverlayPuts(line);}
    brook::DebugOverlayPuts("PARTIAL");
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
