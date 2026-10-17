#pragma once
#include "launch_feedback.h"

namespace brook {
// Bridge-tracked launches only. UI-only admission is independent of this bound.
// Records and tombstones live for the boot; no pointers or evictable slots escape.
struct LaunchRecord {
    uint64_t token=0,requested=0,startedTick=0,dispatchTick=0,windowTick=0,windowCeiling=0;
    uint64_t windowIncarnation=0;
    uint32_t windowId=0;
    bool started=false,dispatchFinished=false,dispatchSucceeded=false,rawResultKnown=false;
    bool windowObserved=false,viaToken=false,existingWindow=false,cancelled=false,bridgeOnly=false;
    int rawResult=0;
    char appId[128]{},executable[64]{};
};
class LaunchHistory {
public:
    static constexpr unsigned Capacity=256;
    LaunchRecord records[Capacity]{};
    int Find(uint64_t token) const {
        for(unsigned i=0;i<Capacity;++i)if(records[i].token==token&&token)return static_cast<int>(i);
        return -1;
    }
    bool CanTrack(uint64_t existing=0) const {
        if(Find(existing)>=0)return true;
        for(const auto& r:records)if(!r.token)return true;
        return false;
    }
    bool Track(const LaunchFeedback& entry,bool bridgeOnly) {
        if(Find(entry.token)>=0)return true;
        for(auto& r:records)if(!r.token) {
            r={};r.token=entry.token;r.requested=entry.requested;r.windowCeiling=entry.windowCeiling;
            r.started=entry.started;r.dispatchFinished=entry.dispatchFinished;r.dispatchSucceeded=entry.dispatched;
            r.startedTick=entry.startedTick;r.dispatchTick=entry.dispatchTick;r.windowTick=entry.windowTick;
            r.windowObserved=entry.windowObserved;r.windowId=entry.observedWindowId;
            r.windowIncarnation=entry.observedWindowIncarnation;r.viaToken=entry.observedViaToken;r.existingWindow=entry.existingWindow;
            r.rawResultKnown=entry.rawResultKnown;r.rawResult=entry.rawResult;r.bridgeOnly=bridgeOnly;
            LaunchFeedbackTable::Copy(r.appId,sizeof(r.appId),entry.appId);
            LaunchFeedbackTable::Copy(r.executable,sizeof(r.executable),entry.executable);
            return true;
        }
        return false;
    }
    void ShareWithUser(uint64_t token) {int i=Find(token);if(i>=0)records[i].bridgeOnly=false;}
    void Start(uint64_t token,uint64_t tick) {
        int i=Find(token);if(i<0)return;auto& r=records[i];r.started=true;r.startedTick=tick;
    }
    void Complete(uint64_t token,int result,uint64_t tick) {
        int i=Find(token);if(i<0)return;auto& r=records[i];
        r.dispatchFinished=true;r.dispatchSucceeded=result>=0;r.rawResultKnown=true;r.rawResult=result;r.dispatchTick=tick;
    }
    // Bound correlation work to the launch timeout. Dispatch completion is
    // retained independently, even when it arrives beyond this interval.
    static bool NeedsObservation(const LaunchRecord& r,uint64_t now) {
        return r.token&&r.started&&!r.cancelled&&!(r.dispatchFinished&&!r.dispatchSucceeded)&&
            !(r.windowObserved&&r.viaToken)&&now-r.requested<LaunchFeedbackTable::TimeoutMs;
    }
    void Observe(uint64_t token,bool allowAppId,uint64_t birth,const char* appId,
                 uint32_t id,uint64_t incarnation,uint64_t tick) {
        for(auto& r:records) {
            if(!NeedsObservation(r,tick))continue;
            const bool exact=token==r.token;
            // App ID is presence evidence, not proof that this dispatch caused
            // or activated a window. An exact launch tag can strengthen it later.
            const bool presence=allowAppId&&(birth>r.windowCeiling||(r.dispatchFinished&&r.dispatchSucceeded))&&
                (LaunchFeedbackTable::Equal(appId,r.appId)||LaunchFeedbackTable::Equal(appId,r.executable));
            if(!(exact||presence)||(r.windowObserved&&(r.viaToken||!exact)))continue;
            r.windowObserved=true;r.viaToken=exact;r.existingWindow=birth<=r.windowCeiling;
            r.windowId=id;r.windowIncarnation=incarnation;r.windowTick=tick;
        }
    }
    static const char* State(const LaunchRecord& r,uint64_t now) {
        if(r.cancelled)return "cancelled";
        if(r.dispatchFinished&&!r.dispatchSucceeded)return "failed";
        if(r.dispatchFinished&&r.windowObserved)return "window_observed";
        if(now-r.requested>=LaunchFeedbackTable::TimeoutMs)return "timed_out";
        if(r.dispatchFinished)return "waiting_for_window";
        return r.started?"dispatching":"queued";
    }
};
}
