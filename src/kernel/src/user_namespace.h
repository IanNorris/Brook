#pragma once
#include <stdint.h>
#include "memory/heap.h"

namespace brook {
// All stored identities are canonical initial-namespace IDs. Null is initial.
// Maps publish once; task/parent/mount/proc references independently pin storage.
static constexpr uint32_t UserIdInvalid=~uint32_t{0}, UserIdOverflow=65534;
static constexpr uint64_t UserCapabilities=(uint64_t{1}<<41)-1;
enum UserCapability : unsigned { CapChown=0,CapDacOverride=1,CapFowner=3,
    CapFsetid=4,CapSetgid=6,CapSetuid=7,CapSetpcap=8,CapSysAdmin=21,CapSetfcap=31 };
struct UserIdMap {
    uint32_t inner=UserIdInvalid,canonical=UserIdInvalid;
    bool written=false;
};
struct UserNamespace {
    uint64_t references=1;
    UserNamespace* parent=nullptr;
    uint64_t identity=0;
    uint32_t owner=0,creatorGroup=0,depth=0;
    bool creatorSetfcap=false;
    UserIdMap users{},groups{};
    bool denyGroups=false;
};
inline void UserNamespaceRetain(UserNamespace* ns) {
    if(!ns)return;
    uint64_t value=__atomic_load_n(&ns->references,__ATOMIC_RELAXED);
    for(;;) {
        if(!value || value==~uint64_t{0})__builtin_trap();
        if(__atomic_compare_exchange_n(&ns->references,&value,value+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return;
    }
}
inline void UserNamespaceRelease(UserNamespace* ns) {
    while(ns) {
        uint64_t value=__atomic_load_n(&ns->references,__ATOMIC_ACQUIRE);
        for(;;) {
            if(!value)__builtin_trap();
            if(__atomic_compare_exchange_n(&ns->references,&value,value-1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
        }
        if(value!=1)return;
        auto* parent=ns->parent;kfree(ns);ns=parent;
    }
}
// Map and capability policy below requires the credential publication lock.
inline uint32_t UserIdFromCanonical(const UserNamespace* ns,bool group,uint32_t id) {
    if(!ns)return id;
    const auto& map=group?ns->groups:ns->users;
    return map.written && map.canonical==id?map.inner:UserIdInvalid;
}
inline uint32_t UserIdToCanonical(const UserNamespace* ns,bool group,uint32_t id) {
    if(!ns)return id;
    const auto& map=group?ns->groups:ns->users;
    return map.written && map.inner==id?map.canonical:UserIdInvalid;
}
inline uint32_t UserIdReport(const UserNamespace* ns,bool group,uint32_t id) {
    const auto result=UserIdFromCanonical(ns,group,id);
    return result==UserIdInvalid?UserIdOverflow:result;
}
struct UserAuthority {
    UserNamespace* ns=nullptr;
    uint32_t uid=0,gid=0;
    uint64_t effective=0;
};
inline bool UserNamespaceCapable(const UserAuthority& actor,const UserNamespace* target,unsigned capability) {
    if(capability>40)return false;
    for(auto* ns=target;;ns=ns->parent) {
        if(ns==actor.ns)return actor.ns?(actor.effective&(uint64_t{1}<<capability))!=0:actor.uid==0;
        if(!ns)return false;
        if(ns->parent==actor.ns && ns->owner==actor.uid)return true;
    }
}
// Restricted single-ID self/parent mapping: no subordinate-ID delegation.
inline int UserNamespaceWriteMap(UserNamespace* ns,const UserAuthority& actor,bool group,
                                  uint32_t inner,uint32_t outer,uint32_t count) {
    if(!ns)return -1;
    auto& map=group?ns->groups:ns->users;
    if(map.written)return -1;
    if(count!=1 || inner==UserIdInvalid || outer==UserIdInvalid)return -22;
    if(actor.ns!=ns && actor.ns!=ns->parent)return -1;
    if(!UserNamespaceCapable(actor,ns,CapSysAdmin))return -1;
    const auto canonical=UserIdToCanonical(ns->parent,group,outer);
    if(canonical==UserIdInvalid)return -1;
    // A deliberately narrower form than privileged Linux multi-ID mappings.
    if(actor.uid!=ns->owner || canonical!=(group?ns->creatorGroup:ns->owner) ||
       canonical!=(group?actor.gid:actor.uid))return -1;
    if(group && !UserNamespaceCapable(actor,ns->parent,CapSetgid) && !ns->denyGroups)return -1;
    if(!group && outer==0 && (actor.ns==ns?!ns->creatorSetfcap:
       !UserNamespaceCapable(actor,ns->parent,CapSetfcap)))return -1;
    map={inner,canonical,true};return 0;
}
inline int UserNamespaceSetgroups(UserNamespace* ns,const UserAuthority& actor,bool deny) {
    if(!ns || !UserNamespaceCapable(actor,ns,CapSysAdmin))return -1;
    if(deny && ns->groups.written)return -1;
    if(!deny && ns->denyGroups)return -1;
    ns->denyGroups=deny;return 0;
}
struct UserCapabilityState {
    uint64_t effective=0,permitted=0,inheritable=0,bounding=UserCapabilities;
};
inline int UserCapabilitiesSet(UserCapabilityState& old,uint64_t effective,uint64_t permitted,uint64_t inheritable) {
    if((effective|permitted|inheritable)&~UserCapabilities)return -1;
    if((effective&~permitted) || (permitted&~old.permitted))return -1;
    // Selected Linux subset: cannot introduce inheritable capabilities outside
    // old permitted/inheritable, and newly inherited bits must be in bounding.
    if((inheritable&~(old.inheritable|old.permitted)) ||
       ((inheritable&~old.inheritable)&~old.bounding))return -1;
    old.effective=effective;old.permitted=permitted;old.inheritable=inheritable;return 0;
}
inline void UserCapabilitiesExec(UserCapabilityState& caps,bool namespaceRoot,bool nnp) {
    const uint64_t next=namespaceRoot?(caps.bounding|caps.inheritable):0;
    caps.permitted=nnp?(next&caps.permitted):next;
    caps.effective=caps.permitted;
}
} // namespace brook
