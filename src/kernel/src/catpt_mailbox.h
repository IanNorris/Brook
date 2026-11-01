#pragma once
#include <stdint.h>

// Resource-bounded CATPT firmware-ready admission. This component does not
// discover, map, power or reset a device. Its read callback is supplied by a
// platform owner only after exclusive ownership and a valid RAM range exist.
// Caller serializes every method and keeps the callback/context alive. The
// platform must establish that the admitted subrange is DSP RAM, not SHIM/MMIO.
namespace brook::catpt {
struct MailboxConfig { uint32_t inboxOffset, outboxOffset, inboxSize, outboxSize, infoSize; };
using ReadAperture = bool (*)(void*, uint32_t, void*, uint32_t);
enum class MailboxState : uint8_t { Unavailable, AwaitingReady, Ready, Failed };

class Mailbox {
public:
    static constexpr uint32_t ReadyBit=1u<<29;
    static constexpr uint32_t BusyBit=1u<<31;
    static constexpr uint32_t InfoMax=100;
    static constexpr uint32_t ConfigBytes=20+InfoMax;
    static constexpr uint32_t MailboxLimit=4096; // implementation bound, not a hardware limit

    // No callback is invoked by admission. A missing base or size is not an
    // address from which firmware can be read.
    bool admit(uint64_t physicalBase,uint32_t apertureSize,uint32_t ramStart,uint32_t ramSize,ReadAperture read,void* context) {
        if(state_!=MailboxState::Unavailable || !physicalBase || !apertureSize || !read ||
           physicalBase>UINT64_MAX-apertureSize || !ramSize || ramStart>apertureSize ||
           ramSize>apertureSize-ramStart)return false;
        ramStart_=ramStart;ramEnd_=ramStart+ramSize;read_=read;context_=context;state_=MailboxState::AwaitingReady;return true;
    }
    bool firmwareReady(uint32_t header) {
        if(state_!=MailboxState::AwaitingReady || (header&(ReadyBit|BusyBit))!=(ReadyBit|BusyBit))return false;
        uint64_t offset=static_cast<uint64_t>(header&((1u<<29)-1))*8;
        if(offset<ramStart_ || offset>ramEnd_ || ConfigBytes>ramEnd_-offset){state_=MailboxState::Failed;return false;}
        uint8_t raw[ConfigBytes]{};
        if(!read_(context_,static_cast<uint32_t>(offset),raw,sizeof(raw))){state_=MailboxState::Failed;return false;}
        MailboxConfig cfg{le32(raw),le32(raw+4),le32(raw+8),le32(raw+12),le32(raw+16)};
        if(cfg.infoSize>InfoMax || !validRange(cfg.inboxOffset,cfg.inboxSize) ||
           !validRange(cfg.outboxOffset,cfg.outboxSize) ||
           (cfg.inboxOffset<cfg.outboxOffset+cfg.outboxSize && cfg.outboxOffset<cfg.inboxOffset+cfg.inboxSize)) {
            state_=MailboxState::Failed;return false;
        }
        config_=cfg;state_=MailboxState::Ready;return true;
    }
    MailboxState state() const { return state_; }
    MailboxConfig config() const { return config_; }
private:
    static uint32_t le32(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
    }
    bool validRange(uint32_t offset,uint32_t length) const {
        return length && length<=MailboxLimit && offset>=ramStart_ && offset<=ramEnd_ && length<=ramEnd_-offset;
    }
    MailboxState state_=MailboxState::Unavailable;
    uint32_t ramStart_=0,ramEnd_=0;ReadAperture read_=nullptr;void* context_=nullptr;MailboxConfig config_{};
};
}
