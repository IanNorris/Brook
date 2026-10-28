#include "user_access_scope.h"
#include "process.h"
#include "scheduler.h"
#include "memory/address_space.h"
#include "panic.h"
namespace brook {
UserAccessScope::UserAccessScope(Process* actor) {
    if(!actor || !actor->addressSpace || actor->addressSpace->kernel ||
       !actor->addressSpace->pagingAdmission.Exclusive())return;
    actor_=actor;space_=actor->addressSpace;
    SchedulerKernelRegionEnter(actor_);AddressSpaceRef(space_);
    KMutexLock(&space_->mutation);space_->userAccess.Add(lease_);KMutexUnlock(&space_->mutation);
    previous_=actor_->userAccessScope;actor_->userAccessScope=this;
}
UserAccessScope::~UserAccessScope() {
    if(!actor_)return;
    if(actor_->userAccessScope!=this)KernelPanic("uaccess: scope unwind order");
    actor_->userAccessScope=previous_;
    KMutexLock(&space_->mutation);
    const bool removed=space_->userAccess.Remove(lease_);
    KMutexUnlock(&space_->mutation);
    if(!removed)KernelPanic("uaccess: missing live lease");
    AddressSpaceUnref(space_);SchedulerKernelRegionLeave(actor_);
}
bool UserAccessScope::PinCurrent(uint64_t address,uint64_t length) {
    auto* actor=ProcessCurrent();auto* scope=actor?actor->userAccessScope:nullptr;
    if(!scope)return true;
    if(scope->space_!=actor->addressSpace)return false;
    KMutexLock(&scope->space_->mutation);
    const bool result=scope->lease_.ranges.Add(address,length);
    KMutexUnlock(&scope->space_->mutation);return result;
}
}
