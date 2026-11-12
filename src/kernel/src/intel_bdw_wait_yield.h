#pragma once
namespace brook {
// The caller holds the device claim and mapping lock. No device access is
// allowed between Unlock and a successful Audit. Failure permanently poisons
// access through this owner; the caller must retain pages and recover.
template<class Io> bool BdwYieldGpuWait(Io& io) {
    if(!io.MayYield())return false;
    io.Unlock();io.Wait();io.Lock();
    if(!io.Audit()){io.Poison();return false;}
    return true;
}
// Check once under the existing ownership and deadline before sleeping. If
// the device is still pending, every subsequent check uses the normal yield
// and re-audit path; this is not an unbounded spin.
template<class Owner> bool BdwFirstGpuPoll(Owner& owner,bool& first) {
    if(first){first=false;return owner.immediatePollAllowed();}
    return owner.pollContinue();
}
}
