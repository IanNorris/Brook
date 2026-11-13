#pragma once
#include <stdint.h>

namespace brook {
// The caller serializes access and retains the handoff session until work
// retires. A detached owner's running job keeps the slot until completion.
class BdwPresentQueue {
    enum class State {Idle,Queued,Running,Abandoned,Done};
    State state_=State::Idle;
    uint64_t owner_=0,ticket_=0;
    int64_t result_=0;
public:
    bool Admit(uint64_t owner,uint64_t ticket){
        if(state_!=State::Idle || !owner || !ticket)return false;
        owner_=owner;ticket_=ticket;state_=State::Queued;
        return true;
    }
    bool Take(){
        if(state_!=State::Queued)return false;
        state_=State::Running;
        return true;
    }
    bool Holds(uint64_t owner,uint64_t ticket)const{
        return owner_==owner && ticket_==ticket &&
            (state_==State::Queued || state_==State::Running || state_==State::Abandoned);
    }
    void DropOwner(uint64_t owner){
        if(!owner || owner_!=owner)return;
        if(state_==State::Running)state_=State::Abandoned;
        else if(state_!=State::Abandoned)*this={};
    }
    bool Complete(int64_t result){
        if(state_==State::Abandoned){*this={};return true;}
        if(state_!=State::Running || result>0 || result<-4095)return false;
        state_=State::Done;result_=result;
        return true;
    }
    // Zero means pending. Terminal results are consumed and encode success
    // as one or a presentation error as one plus its errno.
    int64_t Result(uint64_t owner,uint64_t ticket){
        if(!owner || !ticket || owner_!=owner || ticket_!=ticket || state_==State::Abandoned)return -2;
        if(state_!=State::Done)return 0;
        const auto value=1-result_;
        *this={};
        return value;
    }
};
}
