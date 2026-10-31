#include "../../kernel/src/dell_i2c_id_policy.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
using namespace brook;
struct Io {
    DellI2cCriticalEvidence e{};std::atomic<unsigned> reads{0};uint32_t returned=DellI2cComponentValue;
    bool evidence(DellI2cCriticalEvidence& out){out=e;return true;}
    bool readComponent(uint32_t& value){++reads;value=returned;return true;}
    bool criticalRead(uint32_t& value){return DellI2cIdCritical(*this,value);}
};
struct BlockingIo {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    unsigned reads = 0;
    bool criticalRead(uint32_t& value) {
        std::unique_lock<std::mutex> lock(mutex);
        ++reads;
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        value = DellI2cComponentValue;
        return true;
    }
};
static DellI2cIdPrerequisites valid(){return {true,true,true,true,true,true,DellI2c0Page,DellI2c0PrivatePage,1};}
static DellI2cCriticalEvidence evidence(){return {true,true,true,true,true,true,true,true,false,false,0,0,0,6,0};}
int main(){
    uint32_t value=0;Io io;io.e=evidence();DellI2cIdOneShot off;auto p=valid();p.optedIn=false;
    assert(off.Run(p,io,value)==DellI2cIdResult::Disabled && io.reads==0);
    assert(off.Run(valid(),io,value)==DellI2cIdResult::Identified && io.reads==1);
    assert(off.Run(valid(),io,value)==DellI2cIdResult::AlreadyUsed && io.reads==1);
    assert(off.Status().started && off.Status().completed && off.Status().result==DellI2cIdResult::Identified && off.Status().value==DellI2cComponentValue);
    for(unsigned n=0;n<8;++n){
        p=valid();switch(n){case 0:p.reviewedFirmware=false;break;case 1:p.stableNvs=false;break;case 2:p.bootMapHole=false;break;
        case 3:p.pciInventoryComplete=false;break;case 4:p.noPciBaseInPage=false;break;case 5:++p.controller;break;
        case 6:++p.privatePage;break;default:p.mode=0;break;}
        DellI2cIdOneShot once;Io test;test.e=evidence();assert(once.Run(p,test,value)==DellI2cIdResult::Refused && test.reads==0);
    }
    for(unsigned n=0;n<15;++n){
        DellI2cIdOneShot once;Io test;test.e=evidence();auto& e=test.e;
        switch(n){case 0:e.aliasesValid=false;break;case 1:e.identityPresent=false;break;case 2:e.identityMatches=false;break;
        case 3:e.directPresent=false;break;case 4:e.directMatches=false;break;case 5:e.currentDirectPresent=false;break;
        case 6:e.currentDirectMatches=false;break;case 7:e.cpuValid=false;break;case 8:e.cacheDisabled=true;break;
        case 9:e.notWriteThrough=true;break;case 10:e.identityPat=1;break;case 11:e.directPat=1;break;
        case 12:e.currentDirectPat=1;break;case 13:e.patSlot0=0;break;default:e.mtrrType=6;break;}
        assert(once.Run(valid(),test,value)==DellI2cIdResult::Refused && test.reads==0);
    }
    DellI2cIdOneShot unknown;Io odd;odd.e=evidence();odd.returned=0xffffffff;
    assert(unknown.Run(valid(),odd,value)==DellI2cIdResult::Unknown && value==0xffffffff && odd.reads==1);
    assert(unknown.Status().completed && unknown.Status().result==DellI2cIdResult::Unknown && unknown.Status().value==0xffffffff);
    DellI2cIdOneShot race;Io concurrent;concurrent.e=evidence();std::atomic<unsigned> identified{0};std::vector<std::thread> threads;
    for(unsigned i=0;i<32;++i)threads.emplace_back([&]{uint32_t x;if(race.Run(valid(),concurrent,x)==DellI2cIdResult::Identified)++identified;});
    for(auto& t:threads)t.join();assert(identified==1 && concurrent.reads==1);
    DellI2cIdOneShot blocked; BlockingIo blocking;
    std::thread worker([&] { uint32_t x; assert(blocked.Run(valid(), blocking, x) == DellI2cIdResult::Identified); });
    {
        std::unique_lock<std::mutex> lock(blocking.mutex);
        blocking.cv.wait(lock, [&] { return blocking.entered; });
        const auto status = blocked.Status();
        assert(status.started && !status.completed);
        assert(blocked.Run(valid(), blocking, value) == DellI2cIdResult::AlreadyUsed);
        blocking.release = true;
    }
    blocking.cv.notify_all(); worker.join();
    assert(blocking.reads == 1 && blocked.Status().completed && blocked.Status().result == DellI2cIdResult::Identified);
}
