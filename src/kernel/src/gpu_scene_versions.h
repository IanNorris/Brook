#pragma once
#include <stdint.h>
#include "gpu_scene.h"

namespace brook {

// Kernel-private metadata for immutable scene images. The scene lock protects
// every call. GPU work happens OUTSIDE that lock, between Reserve/Complete,
// Pin/Unpin or BeginEviction/EndEviction. No allocator or GPU callbacks run here.
// A queued draw owns pins on its exact versions, independently of public IDs.
// Wayland committed/cached state belongs to the display server, not this table.
// Published represents ONE public ownership claim. A server storing the same
// ID in several surface states must coalesce those references before Release.
enum class SceneVersionState : uint8_t {
    Empty, Reserved, Published, Retired, Idle, Evicting, Quarantined
};
enum class SceneReserveResult { Ready, Invalid, Closing, Capacity, IdExhausted };
enum class SceneCompleteResult { Invalid, Published, Discarded };
struct SceneImageInfo {
    // Internal descriptor only: never copy the backend texture handle to the
    // userspace scene ABI. Its public capability is the scene-scoped version.
    uint64_t version;
    uint32_t texture,width,height,format;
};
struct SceneVersionStats {
    // Logical pixel bytes, including reservations, idle images and quarantine;
    // not a measurement of host VRAM usage or driver metadata overhead.
    uint64_t bytes,byteLimit,pinned;
    uint32_t slots,published,reserved,idle,evicting,quarantined;
    bool closing;
};

template<uint32_t Capacity> class SceneImageVersions {
    static_assert(Capacity>0);
    struct Entry {
        SceneImageInfo image{};
        uint32_t pins=0;
        SceneVersionState state=SceneVersionState::Empty;
    };
    Entry entries_[Capacity]{};
    uint64_t next_,bytes_=0,limit_;
    bool closing_=false;
    static constexpr uint64_t MaxVersion=0x7fffffffffffffffull;
    static uint64_t Bytes(const SceneImageInfo& image) {
        return uint64_t(image.width)*image.height*4;
    }
    Entry* Find(uint64_t version) {
        if (!version) return nullptr;
        for (auto& entry:entries_) if (entry.image.version==version) return &entry;
        return nullptr;
    }
    const Entry* Find(uint64_t version) const {
        if (!version) return nullptr;
        for (const auto& entry:entries_) if (entry.image.version==version) return &entry;
        return nullptr;
    }
    static void MakeIdle(Entry& entry) {
        // Keep the retired ID as an internal eviction ticket. Public lookup
        // checks state; a recycled allocation receives a fresh nonwrapping ID.
        entry.state=SceneVersionState::Idle;
    }
    bool Adopt(Entry& entry,uint32_t texture) {
        if (!texture || (entry.image.texture && entry.image.texture!=texture)) return false;
        // A backend handle may only belong to one allocation in this table.
        for (const auto& other:entries_)
            if (&other!=&entry && other.image.texture==texture) return false;
        entry.image.texture=texture;return true;
    }
public:
    // firstVersion is useful for exhaustion tests; production scenes start at 1.
    explicit SceneImageVersions(uint64_t byteLimit,uint64_t firstVersion=1)
        :next_(firstVersion),limit_(byteLimit) {}
    SceneImageVersions(const SceneImageVersions&)=delete;
    SceneImageVersions& operator=(const SceneImageVersions&)=delete;

    SceneReserveResult Reserve(uint32_t w,uint32_t h,uint32_t format,SceneImageInfo& out) {
        out={};
        if (!w || !h || w>GPU_SCENE_MAX_DIMENSION || h>GPU_SCENE_MAX_DIMENSION ||
            (format!=1 && format!=2)) return SceneReserveResult::Invalid;
        if (closing_) return SceneReserveResult::Closing;
        if (!next_ || next_>MaxVersion) return SceneReserveResult::IdExhausted;
        Entry* selected=nullptr;
        // Prefer a completed, unreferenced matching image. Reusing this storage
        // must not consume new driver resource IDs every frame.
        for (auto& entry:entries_)
            if (entry.state==SceneVersionState::Idle && entry.image.width==w &&
                entry.image.height==h && entry.image.format==format) { selected=&entry;break; }
        if (!selected) {
            const uint64_t bytes=uint64_t(w)*h*4;
            if (bytes>limit_ || bytes_>limit_-bytes) return SceneReserveResult::Capacity;
            for (auto& entry:entries_) if (entry.state==SceneVersionState::Empty) { selected=&entry;break; }
            if (!selected) return SceneReserveResult::Capacity;
            bytes_+=bytes;selected->image={0,0,w,h,format};
        }
        selected->image.version=next_++;selected->state=SceneVersionState::Reserved;
        out=selected->image;return SceneReserveResult::Ready;
    }
    // Only verified completion publishes a version. Closing races discard the
    // public ID, retaining the finished texture for worker retirement instead.
    SceneCompleteResult Complete(uint64_t version,uint32_t texture) {
        auto* entry=Find(version);
        if (!entry || entry->state!=SceneVersionState::Reserved || !Adopt(*entry,texture))
            return SceneCompleteResult::Invalid;
        if (closing_) { MakeIdle(*entry);return SceneCompleteResult::Discarded; }
        entry->state=SceneVersionState::Published;return SceneCompleteResult::Published;
    }
    // Rejected work made no commands that can still access the target. A newly
    // allocated verified texture can be kept for reuse; no texture frees bytes.
    bool Reject(uint64_t version,uint32_t texture=0) {
        auto* entry=Find(version);
        if (!entry || entry->state!=SceneVersionState::Reserved) return false;
        if (texture && !Adopt(*entry,texture)) return false;
        if (entry->image.texture) MakeIdle(*entry);
        else { bytes_-=Bytes(entry->image);*entry={}; }
        return true;
    }
    // Observation only. GPU work must pin the version before dropping the lock.
    bool GetPublished(uint64_t version,SceneImageInfo& out) const {
        out={};const auto* entry=Find(version);
        if (closing_ || !entry || entry->state!=SceneVersionState::Published) return false;
        out=entry->image;return true;
    }
    // All-or-nothing admission, including repeated versions in one draw list.
    // No caller-owned array is saved: the job must own its copied IDs/draw list.
    bool Pin(const uint64_t* versions,uint32_t count) {
        if (closing_ || count>GPU_SCENE_MAX_LAYERS || (count && !versions)) return false;
        for (uint32_t i=0;i<count;++i) {
            auto* entry=Find(versions[i]);
            if (!entry || entry->state!=SceneVersionState::Published) return false;
            uint32_t occurrences=0;
            for (uint32_t j=0;j<count;++j) if (versions[i]==versions[j]) ++occurrences;
            if (entry->pins>~uint32_t{0}-occurrences) return false;
        }
        for (uint32_t i=0;i<count;++i) ++Find(versions[i])->pins;
        return true;
    }
    bool GetPinned(uint64_t version,SceneImageInfo& out) const {
        out={};const auto* entry=Find(version);
        if (!entry || !entry->pins || (entry->state!=SceneVersionState::Published &&
            entry->state!=SceneVersionState::Retired)) return false;
        out=entry->image;return true;
    }
    bool Unpin(const uint64_t* versions,uint32_t count) {
        if (count>GPU_SCENE_MAX_LAYERS || (count && !versions)) return false;
        for (uint32_t i=0;i<count;++i) {
            auto* entry=Find(versions[i]);
            if (!entry || (entry->state!=SceneVersionState::Published &&
                entry->state!=SceneVersionState::Retired)) return false;
            uint32_t occurrences=0;
            for (uint32_t j=0;j<count;++j) if (versions[i]==versions[j]) ++occurrences;
            if (entry->pins<occurrences) return false;
        }
        // Clear IDs only after processing every occurrence of a duplicate.
        for (uint32_t i=0;i<count;++i) --Find(versions[i])->pins;
        for (auto& entry:entries_)
            if (entry.state==SceneVersionState::Retired && !entry.pins) MakeIdle(entry);
        return true;
    }
    bool Release(uint64_t version) {
        auto* entry=Find(version);
        if (!entry || entry->state!=SceneVersionState::Published) return false;
        if (entry->pins) entry->state=SceneVersionState::Retired;else MakeIdle(*entry);
        return true;
    }
    // Uncertain GPU completion is terminal until device reset. This also
    // retains queued pins and allocation budget. Neither close nor unpin can
    // turn an unverified completion into permission to reuse/free storage.
    bool Quarantine(uint64_t version,uint32_t texture=0) {
        auto* entry=Find(version);
        if (!entry || (entry->state!=SceneVersionState::Reserved &&
            entry->state!=SceneVersionState::Published && entry->state!=SceneVersionState::Retired)) return false;
        if (texture && !Adopt(*entry,texture)) return false;
        entry->state=SceneVersionState::Quarantined;return true;
    }
    // Worker-only two-phase eviction. Accounting is released AFTER verified
    // driver retirement, never when an asynchronous destruction is scheduled.
    bool BeginEviction(uint32_t& slot,SceneImageInfo& out) {
        out={};slot=Capacity;
        for (uint32_t i=0;i<Capacity;++i) if (entries_[i].state==SceneVersionState::Idle) {
            entries_[i].state=SceneVersionState::Evicting;slot=i;out=entries_[i].image;return true;
        }
        return false;
    }
    bool EndEviction(uint32_t slot,uint64_t ticket,bool verified) {
        if (slot>=Capacity || entries_[slot].state!=SceneVersionState::Evicting ||
            !ticket || entries_[slot].image.version!=ticket) return false;
        auto& entry=entries_[slot];
        if (verified) { bytes_-=Bytes(entry.image);entry={}; }
        else entry.state=SceneVersionState::Quarantined;
        return true;
    }
    void Close() {
        closing_=true;
        for (auto& entry:entries_) if (entry.state==SceneVersionState::Published) {
            if (entry.pins) entry.state=SceneVersionState::Retired;else MakeIdle(entry);
        }
    }
    SceneVersionStats Stats() const {
        SceneVersionStats s{bytes_,limit_,0,Capacity,0,0,0,0,0,closing_};
        for (const auto& entry:entries_) {
            s.pinned+=entry.pins;
            switch (entry.state) {
            case SceneVersionState::Published:++s.published;break;
            case SceneVersionState::Reserved:++s.reserved;break;
            case SceneVersionState::Idle:++s.idle;break;
            case SceneVersionState::Evicting:++s.evicting;break;
            case SceneVersionState::Quarantined:++s.quarantined;break;
            default:break;
            }
        }
        return s;
    }
    // The owner still needs separate queued-job/FD references. In particular a
    // reservation without a texture is not reclaimable merely because bytes=0.
    bool Drained() const {
        if (!closing_) return false;
        for (const auto& entry:entries_) if (entry.state!=SceneVersionState::Empty) return false;
        return true;
    }
};
}
