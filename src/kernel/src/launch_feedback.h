#pragma once
#include <stdint.h>
namespace brook {
enum class LaunchPhase : uint8_t { Empty, Queued, Dispatching, Waiting, Failed, TimedOut };
struct LaunchFeedback {
    uint64_t token=0, requested=0, changed=0, windowCeiling=0;
    uint64_t startedTick=0,dispatchTick=0,windowTick=0,observedWindowIncarnation=0;
    uint32_t observedWindowId=0;bool observedViaToken=false,existingWindow=false;
    uint32_t catalogGeneration=0;
    int rawResult=0;bool rawResultKnown=false;
    LaunchPhase phase=LaunchPhase::Empty;
    bool started=false, dispatched=false, dispatchFinished=false, windowObserved=false;
    char identity[33]{}, specification[33]{};
    char title[48]{}, appId[128]{}, executable[64]{};
    bool busy() const {return phase==LaunchPhase::Queued||phase==LaunchPhase::Dispatching||phase==LaunchPhase::Waiting;}
    bool inFlight() const {return started&&!dispatchFinished;}
    bool present() const {return phase!=LaunchPhase::Empty&&!(busy()&&windowObserved);}
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
    static bool Exact(const char* a,const char* b) {
        if(!a||!b)return a==b;
        while(*a&&*a==*b){++a;++b;}return *a==*b;
    }
    // A catalog identity coalesces only the exact frozen launch specification.
    // -2 means the same app is already admitted under a different specification.
    int Begin(uint64_t now,uint64_t ceiling,const char* title,const char* appId,const char* executable,
              const char* identity=nullptr,const char* specification=nullptr,bool* created=nullptr,uint32_t generation=0) {
        if(created)*created=false;
        for(unsigned i=0;i<Capacity;++i) {
            const auto& e=entries[i];
            if(!(e.busy()||e.inFlight()))continue;
            if(identity&&*identity) {
                if(Exact(e.identity,identity))return e.catalogGeneration==generation&&Exact(e.specification,specification)?static_cast<int>(i):-2;
            }else if((appId&&*appId&&Equal(e.appId,appId))||
                ((!appId||!*appId)&&Equal(e.title,title)&&
                 ((!e.executable[0]&&(!executable||!*executable))||Equal(e.executable,executable))))return i;
        }
        for(unsigned i=0;i<Capacity;++i)if(entries[i].phase==LaunchPhase::Empty) {
            auto& e=entries[i];e={};e.token=++nextToken;if(!e.token)e.token=++nextToken;
            e.requested=e.changed=now;e.windowCeiling=ceiling;e.catalogGeneration=generation;e.phase=LaunchPhase::Queued;
            Copy(e.title,sizeof(e.title),title);Copy(e.appId,sizeof(e.appId),appId);
            Copy(e.executable,sizeof(e.executable),executable);
            Copy(e.identity,sizeof(e.identity),identity);Copy(e.specification,sizeof(e.specification),specification);
            if(created)*created=true;return i;
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
        if(oldest>=0){entries[oldest].phase=LaunchPhase::Dispatching;entries[oldest].started=true;entries[oldest].startedTick=now;}
        return oldest;
    }
    void Complete(uint64_t token,bool success,uint64_t now,int rawResult=0,bool rawResultKnown=false) {
        int i=Find(token);if(i<0)return;auto& e=entries[i];
        e.dispatchTick=now;e.dispatched=success;e.dispatchFinished=true;e.rawResult=rawResult;e.rawResultKnown=rawResultKnown;
        if(success&&e.windowObserved){e={};return;}
        if(e.phase==LaunchPhase::TimedOut&&success)return;
        e.phase=success?LaunchPhase::Waiting:LaunchPhase::Failed;e.changed=now;
    }
    bool Window(uint64_t token,uint64_t birth,const char* appId,LaunchFeedback* observed=nullptr,
                uint32_t windowId=0,uint64_t incarnation=0,uint64_t tick=0) {
        for(auto& e:entries) {
            if(e.phase==LaunchPhase::Empty||e.windowObserved||!e.started||(e.dispatchFinished&&!e.dispatched))continue;
            // A known native launch token beats identity guesses. Wayland
            // application identity is presence evidence, not process ownership.
            bool match=token?token==e.token:
                (e.started&&(birth>e.windowCeiling||e.dispatched)&&
                 (Equal(appId,e.appId)||Equal(appId,e.executable)));
            if(match){
                e.windowObserved=true;e.observedWindowId=windowId;e.observedWindowIncarnation=incarnation;
                e.windowTick=tick;e.observedViaToken=token==e.token;e.existingWindow=birth<=e.windowCeiling;
                if(observed)*observed=e;
                if(e.dispatchFinished&&e.dispatched)e={};
                return true;
            }
        }return false;
    }
    void Advance(uint64_t token,uint64_t now,bool liveProcesses) {
        int i=Find(token);if(i<0)return;auto& e=entries[i];
        (void)liveProcesses; // A successful single-instance handoff can leave no tagged process.
        if(e.busy()&&now-e.requested>=TimeoutMs){e.phase=LaunchPhase::TimedOut;e.changed=now;}
        else if(!e.busy()&&!e.inFlight()&&now-e.changed>=NoticeMs)e={};
    }
    void Dismiss(unsigned slot) {if(slot<Capacity&&!entries[slot].busy()&&!entries[slot].inFlight())entries[slot]={};}
private:
    uint64_t nextToken=0;
};
}
