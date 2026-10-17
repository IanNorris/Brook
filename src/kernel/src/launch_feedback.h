#pragma once
#include <stdint.h>
namespace brook {
enum class LaunchPhase : uint8_t { Empty, Queued, Dispatching, Waiting, Failed, TimedOut };
struct LaunchFeedback {
    uint64_t token=0, requested=0, changed=0, windowCeiling=0;
    LaunchPhase phase=LaunchPhase::Empty;
    bool started=false, dispatched=false;
    char title[48]{}, appId[128]{}, executable[64]{};
    bool busy() const {return phase==LaunchPhase::Queued||phase==LaunchPhase::Dispatching||phase==LaunchPhase::Waiting;}
};
// Value-only bounded state; its owner serializes access. No Process, Window,
// launcher snapshot or icon pointers survive in this state machine.
class LaunchFeedbackTable {
public:
    static constexpr unsigned Capacity=8;
    static constexpr uint64_t TimeoutMs=300000, NoticeMs=15000;
    LaunchFeedback entries[Capacity]{};
    static bool Equal(const char* a,const char* b) {
        if(!a||!b||!*a||!*b)return false;
        while(*a&&*b){char x=*a++,y=*b++;
            if(x>='A'&&x<='Z')x+='a'-'A';
            if(y>='A'&&y<='Z')y+='a'-'A';
            if(x!=y)return false;
        }return !*a&&!*b;
    }
    static void Copy(char* out,unsigned capacity,const char* value) {
        unsigned i=0;if(value)while(i+1<capacity&&value[i]){out[i]=value[i];++i;}out[i]=0;
    }
    int Find(uint64_t token) const {
        for(unsigned i=0;i<Capacity;++i)if(entries[i].phase!=LaunchPhase::Empty&&entries[i].token==token)return i;
        return -1;
    }
    int Begin(uint64_t now,uint64_t ceiling,const char* title,const char* appId,const char* executable) {
        for(unsigned i=0;i<Capacity;++i) {
            const auto& e=entries[i];
            if(e.busy()&&((appId&&*appId&&Equal(e.appId,appId))||
                ((!appId||!*appId)&&Equal(e.title,title)&&
                 ((!e.executable[0]&&(!executable||!*executable))||Equal(e.executable,executable)))))return i;
        }
        for(unsigned i=0;i<Capacity;++i)if(entries[i].phase==LaunchPhase::Empty) {
            auto& e=entries[i];e={};e.token=++nextToken;if(!e.token)e.token=++nextToken;
            e.requested=e.changed=now;e.windowCeiling=ceiling;e.phase=LaunchPhase::Queued;
            Copy(e.title,sizeof(e.title),title);Copy(e.appId,sizeof(e.appId),appId);
            Copy(e.executable,sizeof(e.executable),executable);return i;
        }
        return -1;
    }
    int Take(uint64_t now) {
        int oldest=-1;
        for(unsigned i=0;i<Capacity;++i) {
            auto& e=entries[i];
            if(e.phase!=LaunchPhase::Queued)continue;
            if(now-e.requested>=TimeoutMs){e.phase=LaunchPhase::TimedOut;e.changed=now;continue;}
            if(oldest<0||e.token<entries[oldest].token)oldest=i;
        }
        if(oldest>=0){entries[oldest].phase=LaunchPhase::Dispatching;entries[oldest].started=true;}
        return oldest;
    }
    void Complete(uint64_t token,bool success,uint64_t now) {
        int i=Find(token);if(i<0)return;auto& e=entries[i];e.dispatched=success;
        if(e.phase==LaunchPhase::TimedOut)return;
        e.phase=success?LaunchPhase::Waiting:LaunchPhase::Failed;e.changed=now;
    }
    bool Window(uint64_t token,uint64_t birth,const char* appId) {
        for(auto& e:entries) {
            if(e.phase==LaunchPhase::Empty)continue;
            // A known native launch token beats identity guesses. Wayland
            // application identity is presence evidence, not process ownership.
            bool match=token?token==e.token:
                (e.started&&(birth>e.windowCeiling||e.dispatched)&&
                 (Equal(appId,e.appId)||Equal(appId,e.executable)));
            if(match){e={};return true;}
        }return false;
    }
    void Advance(uint64_t token,uint64_t now,bool liveProcesses) {
        int i=Find(token);if(i<0)return;auto& e=entries[i];
        if(e.busy()&&now-e.requested>=TimeoutMs){e.phase=LaunchPhase::TimedOut;e.changed=now;}
        else if(e.phase==LaunchPhase::Waiting&&!liveProcesses){e.phase=LaunchPhase::Failed;e.changed=now;}
        else if(!e.busy()&&now-e.changed>=NoticeMs)e={};
    }
    void Dismiss(unsigned slot) {if(slot<Capacity&&!entries[slot].busy())entries[slot]={};}
private:
    uint64_t nextToken=0;
};
}
