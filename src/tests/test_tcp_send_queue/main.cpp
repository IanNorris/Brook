#include "tcp_send_queue.h"
#include <cassert>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

int main() {
    {TcpSendQueue q{};uint8_t data=1,out=0;
     assert(q.Append(&data,1)==0 && q.ReserveCopy(&out,1)==0 && !q.Retire(1));}
    {uint8_t one[1]{},a=7,out=0;TcpSendQueue q{};assert(q.Init(one,1));
     assert(q.Append(&a,1)==1 && q.Append(&a,1)==0 && q.ReserveCopy(&out,1)==1 && out==7);
     assert(q.Retire(1) && q.Append(&a,1)==1);}
    uint8_t storage[31]{};TcpSendQueue queue{};
    assert(!queue.Init(nullptr,31) && !queue.Init(storage,0));
    assert(queue.Init(storage,sizeof(storage)) && !queue.Init(storage,sizeof(storage)));
    std::deque<uint8_t> model;unsigned reserved=0;std::mt19937 random(0x42524f4f);
    for(unsigned turn=0;turn<100000;++turn){
        unsigned operation=random()%4;
        if(operation==0){
            std::vector<uint8_t> source(random()%50);for(auto& byte:source)byte=static_cast<uint8_t>(random());
            unsigned expected=source.size();if(expected>sizeof(storage)-model.size())expected=sizeof(storage)-model.size();
            assert(queue.Append(source.data(),source.size())==expected);
            for(unsigned i=0;i<expected;++i)model.push_back(source[i]);
        }else if(operation==1){
            uint8_t output[50]{};unsigned requested=random()%50;
            unsigned expected=model.size()-reserved;if(expected>requested)expected=requested;
            assert(queue.ReserveCopy(output,requested)==expected);
            for(unsigned i=0;i<expected;++i)assert(output[i]==model[reserved+i]);
            reserved+=expected;
        }else if(operation==2){
            unsigned n=random()%50;bool expected=n<=reserved;
            assert(queue.Retire(n)==expected);
            if(expected){for(unsigned i=0;i<n;++i)model.pop_front();reserved-=n;}
        }else{
            assert(queue.Append(nullptr,5)==0 && queue.ReserveCopy(nullptr,5)==0);
        }
        assert(queue.used==model.size() && queue.reserved==reserved);
        assert(queue.Free()==sizeof(storage)-model.size() && queue.Unreserved()==model.size()-reserved);
    }
    // A copied flight remains stable after producer input changes, and its
    // capacity cannot be reused until acknowledged bytes are retired.
    uint8_t tiny[4]{},payload[]={1,2,3,4},flight[4]{};TcpSendQueue q{};
    assert(q.Init(tiny,4) && q.Append(payload,4)==4 && q.ReserveCopy(flight,4)==4);
    payload[0]=9;assert(flight[0]==1 && q.Append(payload,1)==0);
    assert(!q.Retire(5) && q.Retire(2) && q.Append(payload,2)==2);
    assert(flight[0]==1 && flight[1]==2);
    uint8_t wrapped[2]{};assert(q.ReserveCopy(wrapped,2)==2 && wrapped[0]==9 && wrapped[1]==2);
    assert(q.used==4 && q.reserved==4 && !q.Retire(5));
    assert(q.used==4 && q.reserved==4 && q.Retire(4) && q.Free()==4);
}
