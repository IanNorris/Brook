#pragma once
#include <stdint.h>

namespace brook {
enum class FramePhase : uint8_t {
    Other, DeferredCaptures, SceneDrain, Preparation, TextureUpload, QuadRecord,
    CommandLock, VertexUpload, CommandBuild, QueueSetup, FenceWait, AckWait,
    DisplayFlush, DebugCapture, DebugPoll, Count
};
inline constexpr unsigned FramePhaseCount = unsigned(FramePhase::Count);
inline const char* FramePhaseName(unsigned i) {
    constexpr const char* names[] = {"other", "deferred_captures", "scene_drain",
        "preparation", "texture_upload", "quad_record", "command_lock",
        "vertex_upload", "command_build", "queue_setup", "fence_wait", "ack_wait",
        "display_flush", "debug_capture", "debug_poll"};
    return i < FramePhaseCount ? names[i] : "invalid";
}
struct FrameScopeToken {
    uint64_t epoch=0, frame=0, id=0, parent=0, enteredNs=0;
    FramePhase previous=FramePhase::Other;
};
struct FrameTimingRow {
    uint64_t frame=0, beginNs=0, endNs=0, previousEndNs=0, sleepDeadlineTicks=0;
    uint64_t phases[FramePhaseCount]{};
    uint64_t commands=0, commandBytes=0, fencedCommands=0;
    uint64_t firstFence=0,lastFence=0;
    uint64_t commandNsByParent[FramePhaseCount]{}, commandsByParent[FramePhaseCount]{};
};
struct FrameTimingStatus {
    uint64_t epoch=0, completed=0, overwritten=0, invalidFrames=0, abortedFrames=0;
    uint64_t clockFailures=0;
    uint64_t contendedRecords=0; // lifetime wrapper counter, not reset with epoch
    unsigned count=0;
    bool enabled=false;
};
// Caller serializes. Fixed storage; phase scopes form a checked stack so nested
// driver waits are exclusive rather than added twice to their parent phase.
template<unsigned Capacity=256> class FrameTimingModel {
    static_assert(Capacity>0);
    FrameTimingStatus s_{};
    FrameTimingRow rows_[Capacity]{}, current_{};
    uintptr_t owner_=0;
    uint64_t last_=0, previousEnd_=0, scope_=0, scopeSequence_=0, sequence_=0;
    uint64_t commandScope_=0;
    bool commandRecorded_=false;
    FramePhase phase_=FramePhase::Other;
    bool Charge(uint64_t now) {
        if(now<last_){Invalidate();return false;}
        current_.phases[unsigned(phase_)]+=now-last_;last_=now;return true;
    }
public:
    bool Active(uintptr_t owner) const { return s_.enabled && owner_ && owner_==owner; }
    FrameTimingStatus Status() const { return s_; }
    bool Control(int mode,bool reset=false) {
        if(reset || (mode==1 && !s_.enabled)) {
            if(s_.epoch==~uint64_t(0))return false;
            const auto epoch=s_.epoch+1;const bool enabled=mode<0?s_.enabled:mode==1;
            s_={};s_.epoch=epoch;s_.enabled=enabled;
            owner_=last_=previousEnd_=scope_=scopeSequence_=sequence_=0;
        } else if(mode==0) {
            if(owner_)++s_.abortedFrames;
            s_.enabled=false;owner_=0;previousEnd_=0;
        }
        return true;
    }
    void Invalidate() {
        if(owner_)++s_.invalidFrames;
        owner_=0;previousEnd_=0;
    }
    void ClockFailure() { ++s_.clockFailures;Invalidate(); }
    void Begin(uintptr_t owner,uint64_t now) {
        if(!s_.enabled || !owner)return;
        if(owner_)Invalidate();
        if(previousEnd_ && now<previousEnd_){++s_.invalidFrames;previousEnd_=0;}
        if(sequence_==~uint64_t(0)){s_.enabled=false;return;}
        current_={};current_.frame=++sequence_;current_.beginNs=now;
        current_.previousEndNs=previousEnd_;last_=now;owner_=owner;
        scope_=scopeSequence_=commandScope_=0;commandRecorded_=false;phase_=FramePhase::Other;
    }
    FrameScopeToken Enter(uintptr_t owner,FramePhase phase,uint64_t now) {
        if(!Active(owner))return {};
        if(unsigned(phase)>=FramePhaseCount || scopeSequence_==~uint64_t(0)) {Invalidate();return {};}
        if(phase==FramePhase::QueueSetup && commandScope_){Invalidate();return {};}
        if(!Charge(now))return {};
        FrameScopeToken token{s_.epoch,current_.frame,++scopeSequence_,scope_,now,phase_};
        scope_=token.id;phase_=phase;
        if(phase==FramePhase::QueueSetup){commandScope_=token.id;commandRecorded_=false;}
        return token;
    }
    void Leave(uintptr_t owner,FrameScopeToken token,uint64_t now) {
        if(!token.id || !Active(owner) || token.epoch!=s_.epoch || token.frame!=current_.frame)return;
        if(scope_!=token.id || unsigned(token.previous)>=FramePhaseCount ||
           token.enteredNs<current_.beginNs || token.enteredNs>now){Invalidate();return;}
        if(!Charge(now))return;
        if(phase_==FramePhase::QueueSetup){
            if(commandScope_!=token.id || !commandRecorded_){Invalidate();return;}
            current_.commandNsByParent[unsigned(token.previous)]+=now-token.enteredNs;
            ++current_.commandsByParent[unsigned(token.previous)];commandScope_=0;commandRecorded_=false;
        }
        phase_=token.previous;scope_=token.parent;
    }
    void Command(uintptr_t owner,uint32_t bytes,bool fenced,uint64_t fence) {
        if(!Active(owner))return;
        if(!commandScope_ || scope_!=commandScope_ || commandRecorded_){Invalidate();return;}
        if(fenced && (!fence || fence<=current_.lastFence)){Invalidate();return;}
        if(current_.commands==~uint64_t(0) || current_.commandBytes>~uint64_t(0)-bytes){Invalidate();return;}
        ++current_.commands;current_.commandBytes+=bytes;current_.fencedCommands+=fenced;
        commandRecorded_=true;
        if(fenced){if(!current_.firstFence)current_.firstFence=fence;current_.lastFence=fence;}
    }
    void End(uintptr_t owner,uint64_t now,uint64_t sleepDeadline) {
        if(!Active(owner))return;
        if(scope_ || !Charge(now)){Invalidate();return;}
        current_.endNs=now;current_.sleepDeadlineTicks=sleepDeadline;
        rows_[s_.completed%Capacity]=current_;
        if(s_.count<Capacity)++s_.count;else ++s_.overwritten;
        ++s_.completed;previousEnd_=now;owner_=0;
    }
    bool Read(uint64_t epoch,unsigned index,FrameTimingRow* out) const {
        if(!out || s_.enabled || epoch!=s_.epoch || index>=s_.count)return false;
        *out=rows_[(s_.completed-s_.count+index)%Capacity];return true;
    }
};

bool FrameTimingControl(int mode,bool reset=false);
FrameTimingStatus FrameTimingReadStatus();
bool FrameTimingReadRow(uint64_t epoch,unsigned index,FrameTimingRow* output);
void FrameTimingBegin();
void FrameTimingEnd(uint64_t sleepDeadlineTicks);
extern "C" void FrameTimingEnter(unsigned phase,FrameScopeToken* token);
extern "C" void FrameTimingLeave(const FrameScopeToken* token);
extern "C" void FrameTimingCommand(uint32_t bytes,bool fenced,uint64_t fence);
class FrameTimingScope {
    FrameScopeToken token_{};
public:
    explicit FrameTimingScope(FramePhase phase){FrameTimingEnter(unsigned(phase),&token_);}
    ~FrameTimingScope(){if(token_.id)FrameTimingLeave(&token_);}
    void Change(FramePhase phase){if(token_.id)FrameTimingLeave(&token_);FrameTimingEnter(unsigned(phase),&token_);}
    FrameTimingScope(const FrameTimingScope&)=delete;
    FrameTimingScope& operator=(const FrameTimingScope&)=delete;
};
} // namespace brook
