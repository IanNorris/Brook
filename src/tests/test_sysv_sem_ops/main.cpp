#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/sem.h>
#include "../../kernel/src/sysv_sem_ops.h"
namespace {
using namespace brook;
unsigned failures;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"SEM line %d: %s errno=%d\n",__LINE__,#x,errno);++failures;}}while(0)
union Argument {int val; unsigned short* array;};
struct Set {
    int id=semget(IPC_PRIVATE,3,IPC_CREAT|0600);
    ~Set(){if(id>=0)semctl(id,0,IPC_RMID);}
};
void compare(Set& set,const uint16_t (&initial)[3],const SysvSemOp* ops,unsigned count) {
    unsigned short reset[3]={initial[0],initial[1],initial[2]};
    Argument argument{};argument.array=reset;
    CHECK(semctl(set.id,0,SETALL,argument)==0);
    uint16_t proposed[3]{};int32_t adjustment[3]{};
    auto result=SysvSemPreview(initial,3,nullptr,ops,count,proposed,adjustment);
    sembuf actual[8]{};CHECK(count<=8);
    for(unsigned i=0;i<count;++i)actual[i]={ops[i].number,ops[i].delta,short(ops[i].flags|IPC_NOWAIT)};
    errno=0;const int rc=semop(set.id,actual,count);const int error=errno;
    if(result.state==SysvSemOpState::Ready)CHECK(rc==0);
    else CHECK(rc==-1 && error==(result.state==SysvSemOpState::Wait?EAGAIN:result.error));
    unsigned short observed[3]{};argument.array=observed;
    CHECK(semctl(set.id,0,GETALL,argument)==0);
    for(unsigned i=0;i<3;++i)
        CHECK(observed[i]==(result.state==SysvSemOpState::Ready?proposed[i]:initial[i]));
}
}
int main() {
    using namespace brook;
    CHECK(SysvSemWakeTick(1000001,0,10)==12);
    CHECK(SysvSemWakeTick(1000000,0,10)==11);
    CHECK(SysvSemWakeTick(9,10,50)==50);
    CHECK(SysvSemWakeTick(~uint64_t{0},10,50)==~uint64_t{0});
    CHECK(SysvSemWakeTick(5000000,0,~uint64_t{0}-3)==~uint64_t{0}-1);
    Set set;if(set.id<0){std::perror("private Linux semaphore oracle");return 1;}
    const uint16_t start[3]={2,0,1};
    SysvSemOp blocked[]={{0,-1,0},{1,-1,0}};compare(set,start,blocked,2);
    SysvSemOp ready[]={{0,-2,0},{0,0,0},{1,1,0}};compare(set,start,ready,3);
    SysvSemOp badIndex[]={{1,-1,SysvSemNowait},{3,1,0}};compare(set,start,badIndex,2);
    SysvSemOp overflow[]={{2,32767,0}};compare(set,start,overflow,1);
    SysvSemOp undo[]={{0,-1,SysvSemUndo},{0,1,SysvSemUndo}};compare(set,start,undo,2);
    unsigned random=0x514248;
    auto next=[&]{random=random*1664525u+1013904223u;return random;};
    for(unsigned round=0;round<200;++round) {
        uint16_t initial[3]={uint16_t(next()%5),uint16_t(next()%5),uint16_t(next()%5)};
        SysvSemOp ops[4]{};unsigned count=1+next()%4;
        for(unsigned i=0;i<count;++i)ops[i]={uint16_t(next()%3),int16_t(int(next()%7)-3),uint16_t((next()&1)?SysvSemUndo:0)};
        compare(set,initial,ops,count);
    }
    uint16_t proposed[3]{};int32_t prior[3]={32767,0,0},changes[3]{};
    SysvSemOp moreUndo[]={{0,-1,SysvSemUndo}};
    auto r=SysvSemPreview(start,3,prior,moreUndo,1,proposed,changes);
    CHECK(r.state==SysvSemOpState::Error && r.error==ERANGE && prior[0]==32767);
    r=SysvSemPreview(start,3,nullptr,blocked,2,proposed,changes);
    CHECK(r.state==SysvSemOpState::Wait && r.waitSemaphore==1 && !r.waitZero);
    std::printf("SYSV_SEM_OPS %s (Linux differential cases)\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
