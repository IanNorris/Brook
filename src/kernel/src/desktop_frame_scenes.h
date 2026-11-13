#pragma once
#include <stdint.h>
#include "desktop_present_dependencies.h"

namespace brook {
// Caller serializes access and owns the scene references. Each ticket gets
// its own immutable scene/backdrop/chrome metadata at reservation time.
template<class Scene, unsigned Capacity=2> struct DesktopFrameScenes {
    struct Binding {uint64_t owner,ticket,sceneId,display,backdrop;uint32_t chromeMask;bool chromeTracking;Scene* scene;bool fullOnly;
        DesktopPresentDependencies dependencies{};};
    Binding entries[Capacity]{};
    Binding* Find(uint64_t owner,uint64_t ticket){
        if(!owner || !ticket)return nullptr;
        for(auto& e:entries)if(e.owner==owner && e.ticket==ticket)return &e;
        return nullptr;
    }
    bool Insert(const Binding& binding){
        if(!binding.owner || !binding.ticket || !binding.sceneId || !binding.display || !binding.scene || Find(binding.owner,binding.ticket))return false;
        for(auto& e:entries)if(!e.ticket){e=binding;return true;}
        return false;
    }
    Scene* Remove(uint64_t owner,uint64_t ticket){
        auto* e=Find(owner,ticket);if(!e)return nullptr;
        Scene* result=e->scene;*e={};return result;
    }
    // Call only after successful OS-owned presentation, under the same lock
    // that serializes reservations and accepted presentations. Later tickets
    // need the actual intervening writes regardless of their source scene.
    void RecordAccepted(uint64_t owner,uint64_t ticket,const DesktopDependencyRect* rects,uint32_t count,bool full){
        if(!owner || !ticket)return;
        for(auto& e:entries)if(e.owner==owner && e.ticket>ticket){
            if(full)DesktopDependenciesFull(e.dependencies);
            else DesktopDependenciesAdd(e.dependencies,rects,count);
        }
    }
};
}
