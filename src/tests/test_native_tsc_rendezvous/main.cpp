#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include "../../kernel/src/native_tsc_rendezvous.h"
using namespace brook;
struct Clock {
    bool supported=true;
    bool capable(){return supported;}
    bool tsc(uint64_t& value){value=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());return true;}
    void pause(){std::this_thread::yield();}
};
struct Scripted {
    NativeTscMailbox* box=nullptr;Clock* responder=nullptr;unsigned reads=0,failAt=0;uint64_t times[2]{100,120};
    bool tsc(uint64_t& v){++reads;if(reads==failAt)return false;v=times[reads-1];return true;}
    void pause(){if(box && responder)NativeTscRespond(*box,*responder);}
};
int main(){
    NativeTscInterval x{};assert(NativeTscIntervalFrom(100,110,120,x) && x.lower==-10 && x.upper==10 && x.roundTrip==20);
    NativeTscInterval narrow{-4,8,12};assert(NativeTscIntersect(x,narrow) && x.lower==-4 && x.upper==8 && x.roundTrip==12);
    NativeTscInterval disjoint{9,12,3};assert(!NativeTscIntersect(x,disjoint) && x.lower==-4 && x.upper==8);
    assert(!NativeTscIntervalFrom(101,100,99,x));
    assert(!NativeTscIntervalFrom(0,~uint64_t(0),1,x));
    assert(!NativeTscIntervalFrom(~uint64_t(0)-1,0,~uint64_t(0),x));
    const uint64_t max=~uint64_t(0)>>1;int64_t difference=0;
    assert(NativeTscDifference(max,0,difference) && difference==int64_t(max));
    assert(NativeTscDifference(0,max,difference) && difference==-int64_t(max));
    assert(!NativeTscDifference(max+1,0,difference) && !NativeTscDifference(0,max+1,difference));
    NativeTscMailbox box;std::atomic<bool> stop{false};Clock bsp,ap;
    std::thread responder([&](){while(!stop.load(std::memory_order_acquire)){NativeTscRespond(box,ap);std::this_thread::yield();}});
    for(unsigned i=0;i<100;++i){NativeTscInterval result{};
        assert(NativeTscExchange(box,bsp,10000000,result)==NativeTscExchangeResult::Complete);
        assert(result.lower<=0 && result.upper>=0);}
    stop.store(true,std::memory_order_release);responder.join();
    assert(!NativeTscRespond(box,ap)); // no duplicate reply
    NativeTscMailbox late;NativeTscInterval ignored{};
    assert(NativeTscExchange(late,bsp,1,ignored)==NativeTscExchangeResult::Timeout);
    assert(NativeTscRespond(late,ap)); // a late AP cannot resurrect the exchange
    assert(NativeTscExchange(late,bsp,100,ignored)==NativeTscExchangeResult::Abandoned);
    NativeTscMailbox unsupported;Clock noAp;noAp.supported=false;
    std::atomic<bool> finished{false};std::thread bad([&](){while(!finished.load(std::memory_order_acquire)){NativeTscRespond(unsupported,noAp);std::this_thread::yield();}});
    assert(NativeTscExchange(unsupported,bsp,10000000,ignored)==NativeTscExchangeResult::Unavailable);
    finished.store(true,std::memory_order_release);bad.join();
    NativeTscMailbox invalid;assert(NativeTscExchange(invalid,bsp,0,ignored)==NativeTscExchangeResult::Invalid);
    assert(invalid.request==0 && invalid.response==0);
    NativeTscMailbox exhausted;exhausted.request=exhausted.response=~uint32_t(0);
    assert(NativeTscExchange(exhausted,bsp,100,ignored)==NativeTscExchangeResult::Abandoned && exhausted.abandoned);
    NativeTscMailbox outstanding;outstanding.request=1;
    assert(NativeTscExchange(outstanding,bsp,100,ignored)==NativeTscExchangeResult::Abandoned && outstanding.abandoned);
    NativeTscMailbox before;Scripted first;first.failAt=1;
    assert(NativeTscExchange(before,first,100,ignored)==NativeTscExchangeResult::Unavailable && before.request==0);
    NativeTscMailbox after;Scripted second;second.box=&after;second.responder=&ap;second.failAt=2;
    assert(NativeTscExchange(after,second,100,ignored)==NativeTscExchangeResult::Unavailable && after.request==after.response);
}
