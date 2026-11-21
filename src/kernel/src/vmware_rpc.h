#pragma once

#include <stdint.h>

namespace brook::vmware {

struct ResizeMode { uint32_t width, height; };

enum class RpcResult {
    Closed, Idle, Handled, ModeRequested, WaitingForApply, PowerRequested, TransportError
};

// Soft power operations from Workstation's Shut Down Guest / Restart Guest.
enum class PowerOp : uint8_t { None, Halt, Reboot };

struct RpcPoll {
    RpcResult result;
    ResizeMode mode{};
    PowerOp power = PowerOp::None;     // with PowerRequested: answer before polling again
    PowerOp committed = PowerOp::None; // reply and success status delivered: start it now
};

namespace rpc {
// VMware GuestRPC MESSAGE operations and register layout. See Open VM
// Tools lib/message/message.c and lib/include/guest_msg_def.h.
enum class Operation : uint16_t {
    Open, SendSize, SendPayload, ReceiveSize, ReceivePayload, ReceiveStatus, Close
};
constexpr uint32_t Magic = 0x564d5868;
constexpr uint16_t Port = 0x5658;
constexpr uint16_t MessageCommand = 30;
constexpr uint32_t CookieFlag = 0x80000000;
constexpr uint32_t Tclo = 0x4f4c4354;
constexpr uint32_t Rpci = 0x49435052;
constexpr uint16_t Success = 1;
constexpr uint16_t HaveMessage = 2;
constexpr uint32_t MessageLimit = 256;

struct Registers { uint32_t ax, bx, cx, dx, si, di; };
constexpr uint16_t High(uint32_t value) { return value >> 16; }

enum class ReceiveResult { Empty, Message, Failed };

class Channel {
    uint16_t id_ = 0;
    uint32_t cookieHigh_ = 0, cookieLow_ = 0;
    bool opened_ = false;

    template<class PortIO>
    bool Call(PortIO& io, Operation op, uint32_t value, Registers& reply) const
    {
        reply = {Magic, value, uint32_t(op) << 16 | MessageCommand,
                 uint32_t(id_) << 16 | Port, cookieHigh_, cookieLow_};
        return io(reply) && (High(reply.cx) & Success);
    }

public:
    template<class PortIO>
    bool Open(PortIO& io, uint32_t protocol)
    {
        Registers reply{};
        // Only a rejected cookie negotiation falls back to the legacy form.
        // A platform without the transport should receive no further I/O.
        for (const uint32_t flag : CookieOptions) {
            reply = {Magic, protocol | flag, MessageCommand, Port, 0, 0};
            if (!io(reply)) return false;
            if (High(reply.cx) & Success) {
                id_ = High(reply.dx);
                cookieHigh_ = flag ? reply.si : 0;
                cookieLow_ = flag ? reply.di : 0;
                opened_ = true;
                return true;
            }
        }
        return false;
    }

    bool IsOpen() const { return opened_; }

    template<class PortIO>
    void Close(PortIO& io)
    {
        Registers reply{};
        if (opened_) (void)Call(io, Operation::Close, 0, reply);
        *this = {};
    }

    template<class PortIO>
    bool Send(PortIO& io, const char* data, uint32_t size) const
    {
        Registers reply{};
        if (!opened_ || size > MessageLimit || !Call(io, Operation::SendSize, size, reply))
            return false;
        // Linux vmwgfx also uses the low-bandwidth path after an HB reply
        // when encrypted guest memory precludes the HB buffer transfer.
        for (uint32_t i = 0; i < size; i += 4) {
            uint32_t word = 0;
            for (uint32_t j = 0; j < 4 && i + j < size; ++j)
                word |= uint32_t(uint8_t(data[i + j])) << (j * 8);
            if (!Call(io, Operation::SendPayload, word, reply)) return false;
        }
        return true;
    }

    template<class PortIO>
    ReceiveResult Receive(PortIO& io, char (&data)[MessageLimit], uint32_t& size) const
    {
        Registers reply{};
        size = 0;
        if (!opened_ || !Call(io, Operation::ReceiveSize, 0, reply)) return ReceiveResult::Failed;
        if (!(High(reply.cx) & HaveMessage)) return ReceiveResult::Empty;
        const uint32_t total = reply.bx;
        if (High(reply.dx) != uint16_t(Operation::SendSize)) return ReceiveResult::Failed;
        if (total > MessageLimit) {
            (void)Call(io, Operation::ReceivePayload, 0, reply);
            return ReceiveResult::Failed;
        }
        for (uint32_t i = 0; i < total; i += 4) {
            if (!Call(io, Operation::ReceivePayload, Success, reply) ||
                High(reply.dx) != uint16_t(Operation::SendPayload)) {
                (void)Call(io, Operation::ReceivePayload, 0, reply);
                return ReceiveResult::Failed;
            }
            for (uint32_t j = 0; j < 4 && i + j < total; ++j)
                data[i + j] = char((reply.bx >> (j * 8)) & 255);
        }
        if (!Call(io, Operation::ReceiveStatus, Success, reply)) {
            (void)Call(io, Operation::ReceiveStatus, 0, reply);
            return ReceiveResult::Failed;
        }
        size = total;
        return ReceiveResult::Message;
    }

private:
    static constexpr uint32_t CookieOptions[] = {CookieFlag, 0};
};

inline uint32_t Length(const char* literal)
{
    uint32_t size = 0;
    while (literal[size]) ++size;
    return size;
}

inline bool Matches(const char* data, uint32_t size, const char* literal)
{
    if (size != Length(literal)) return false;
    for (uint32_t i = 0; i < size; ++i)
        if (data[i] != literal[i]) return false;
    return true;
}

inline bool Integer(const char* data, uint32_t size, uint32_t& i, uint32_t limit, uint32_t& value)
{
    value = 0;
    const uint32_t start = i;
    for (; i < size && data[i] >= '0' && data[i] <= '9'; ++i) {
        const uint64_t next = uint64_t(value) * 10 + uint32_t(data[i] - '0');
        if (next > limit) return false;
        value = uint32_t(next);
    }
    return i != start && value != 0;
}

inline bool ModeArguments(const char* data, uint32_t size, uint32_t i, ResizeMode limit, ResizeMode& mode)
{
    if (i == size) return false;
    while (i < size && data[i] == ' ') ++i;
    if (!Integer(data, size, i, limit.width, mode.width) || i == size || data[i] != ' ')
        return false;
    while (i < size && data[i] == ' ') ++i;
    return Integer(data, size, i, limit.height, mode.height) && i == size;
}

enum class HostReply { Ok, Declined, TransportError };

// Bounded text builder for the fixed RPCI messages (no libc in the kernel).
class Text {
    char data_[MessageLimit];
    uint32_t size_ = 0;
    bool overflow_ = false;

public:
    Text& operator<<(const char* text)
    {
        while (*text) {
            if (size_ == MessageLimit) { overflow_ = true; break; }
            data_[size_++] = *text++;
        }
        return *this;
    }
    Text& operator<<(uint32_t value)
    {
        char digits[10];
        uint32_t count = 0;
        do { digits[count++] = char('0' + value % 10); value /= 10; } while (value);
        while (count) {
            if (size_ == MessageLimit) { overflow_ = true; break; }
            data_[size_++] = digits[--count];
        }
        return *this;
    }
    Text& Ipv4(uint32_t networkOrder)
    {
        for (uint32_t i = 0; i < 4; ++i) {
            if (i) *this << ".";
            *this << ((networkOrder >> (i * 8)) & 255);
        }
        return *this;
    }
    const char* Data() const { return data_; }
    uint32_t Size() const { return size_; }
    bool Valid() const { return !overflow_; }
};

// Values from Open VM Tools lib/include/vmware/guestrpc/powerops.h,
// lib/include/guestInfo.h and lib/include/vm_version.h.
enum GuestOsState : uint32_t { StateHalt = 1, StateReboot = 2, StatePowerOn = 3, StateResume = 4, StateSuspend = 5 };
constexpr uint32_t InfoIpAddress = 2;
// TOOLS_VERSION_UNMANAGED: a guest-managed (distribution) toolset, so
// Workstation neither nags about installing VMware Tools nor offers upgrades.
constexpr uint32_t ToolsVersionUnmanaged = 0x7fffffff;
}

// One serialized TCLO session acting as the guest "toolbox" service, plus the
// RPCI client it reports through. The caller owns polling cadence, explicit
// Close() on shutdown, and mode and power policy. In particular, no resize
// success reply is sent until the caller confirms that the requested mode was
// applied (or was already current), and a power command is not answered
// until the caller accepts or declines it.
enum class ResizeApplyResult { Applied, Invalid, Retry };

template<class PortIO>
class ToolsRpc {
    PortIO& io_;
    rpc::Channel listener_, client_;
    const char* reply_ = "";
    ResizeMode limit_{};
    bool registered_ = false;
    bool awaitingApply_ = false;
    bool ipRequested_ = false;
    ResizeMode pendingMode_{};
    uint64_t applyStarted_ = 0;
    PowerOp pendingPower_ = PowerOp::None, statusCommit_ = PowerOp::None;
    uint32_t statusState_ = 0;
    static constexpr uint64_t ApplyTimeoutMs = 1000;

    rpc::HostReply Call(const char* command, uint32_t size)
    {
        using namespace rpc;
        if (!client_.Send(io_, command, size)) return HostReply::TransportError;
        char reply[MessageLimit];
        uint32_t bytes;
        if (client_.Receive(io_, reply, bytes) != ReceiveResult::Message ||
            bytes < 2 || reply[1] != ' ' || (reply[0] != '0' && reply[0] != '1'))
            return HostReply::TransportError;
        return reply[0] == '1' ? HostReply::Ok : HostReply::Declined;
    }

    rpc::HostReply Call(const rpc::Text& text)
    {
        return text.Valid() ? Call(text.Data(), text.Size()) : rpc::HostReply::Declined;
    }

    RpcPoll Failed()
    {
        // Checkpoint/restore and transport errors invalidate this exchange.
        // The caller can reopen on a later tick. Never replay a resize.
        Close();
        return {RpcResult::TransportError};
    }

    bool ResolutionEnabled() const { return limit_.width && limit_.height; }

public:
    constexpr explicit ToolsRpc(PortIO& io) : io_(io) {}
    ToolsRpc(const ToolsRpc&) = delete;
    ToolsRpc& operator=(const ToolsRpc&) = delete;

    bool Open()
    {
        if (listener_.IsOpen()) return true;
        if (!listener_.Open(io_, rpc::Tclo)) return false;
        if (client_.Open(io_, rpc::Rpci)) return true;
        listener_.Close(io_);
        return false;
    }

    bool IsOpen() const { return listener_.IsOpen(); }

    void Close()
    {
        listener_.Close(io_);
        client_.Close(io_);
        reply_ = "";
        registered_ = awaitingApply_ = false;
        pendingPower_ = statusCommit_ = PowerOp::None;
        statusState_ = 0;
    }

    // A zero limit leaves host resize hints unregistered.
    void SetResolutionLimit(ResizeMode limit) { limit_ = limit; }

    // Registers the toolset version and capabilities, as vmtoolsd does at
    // startup and on Capabilities_Register. Only a transport error fails;
    // a host that declines an optional capability keeps the session.
    bool Announce()
    {
        using namespace rpc;
        if (!client_.IsOpen()) return false;
        Text version;
        version << "tools.set.version " << ToolsVersionUnmanaged;
        static constexpr char stateChange[] = "tools.capability.statechange ";
        static constexpr char retry[] = "tools.capability.softpowerop_retry ";
        if (Call(version) == HostReply::TransportError ||
            Call(stateChange, sizeof(stateChange) - 1) == HostReply::TransportError ||
            Call(retry, sizeof(retry) - 1) == HostReply::TransportError) return false;
        registered_ = false;
        if (!ResolutionEnabled()) return true;
        static constexpr char server[] = "tools.capability.resolution_server toolbox 1";
        static constexpr char mode[] = "tools.capability.resolution_set 1";
        const auto first = Call(server, sizeof(server) - 1);
        const auto second = first == HostReply::Ok ? Call(mode, sizeof(mode)) : first;
        if (second == HostReply::TransportError) return false;
        registered_ = second == HostReply::Ok;
        return true;
    }

    // Publishes the primary IPv4 address (network byte order) through the
    // legacy guest info key and the guestinfo.ip variable.
    bool SendGuestIp(uint32_t address)
    {
        using namespace rpc;
        if (!client_.IsOpen()) return false;
        Text info, variable;
        info << "SetGuestInfo  " << InfoIpAddress << " ";
        info.Ipv4(address);
        variable << "info-set guestinfo.ip ";
        variable.Ipv4(address);
        return Call(info) != HostReply::TransportError && Call(variable) != HostReply::TransportError;
    }

    bool Log(const char* message)
    {
        rpc::Text text;
        text << "log " << message;
        return client_.IsOpen() && Call(text) != rpc::HostReply::TransportError;
    }

    // True once after the host sets the broadcastIP option.
    bool TakeIpRequest()
    {
        const bool requested = ipRequested_;
        ipRequested_ = false;
        return requested;
    }

    RpcPoll Poll(uint64_t now = 0)
    {
        using namespace rpc;
        if (!listener_.IsOpen()) return {RpcResult::Closed};
        if (awaitingApply_) return {RpcResult::WaitingForApply};
        if (pendingPower_ != PowerOp::None) return {RpcResult::PowerRequested, {}, pendingPower_};
        if (!listener_.Send(io_, reply_, Length(reply_))) return Failed();
        reply_ = "";
        RpcPoll result{RpcResult::Handled};
        if (statusState_) {
            // Open VM Tools reports the state change after answering the
            // command, then starts the operation.
            Text status;
            status << "tools.os.statechange.status 1 " << statusState_;
            statusState_ = 0;
            if (Call(status) == HostReply::TransportError) return Failed();
            result.committed = statusCommit_;
            statusCommit_ = PowerOp::None;
        }
        char command[MessageLimit];
        uint32_t size;
        switch (listener_.Receive(io_, command, size)) {
        case ReceiveResult::Empty: result.result = RpcResult::Idle; return result;
        case ReceiveResult::Failed: {
            const auto committed = result.committed;
            result = Failed();
            result.committed = committed;
            return result;
        }
        case ReceiveResult::Message: break;
        }
        if (size && command[size - 1] == '\0') --size;
        if (!size) { result.result = RpcResult::Idle; return result; }
        uint32_t verb = 0;
        while (verb < size && command[verb] != ' ') ++verb;
        uint32_t argument = verb;
        while (argument < size && command[argument] == ' ') ++argument;
        const bool noArguments = argument == size;
        if (noArguments && Matches(command, verb, "reset")) {
            registered_ = false;
            reply_ = "OK ATR toolbox";
        } else if (noArguments && Matches(command, verb, "ping")) {
            reply_ = "OK ";
        } else if (noArguments && Matches(command, verb, "Capabilities_Register")) {
            if (!Announce()) {
                const auto committed = result.committed;
                result = Failed();
                result.committed = committed;
                return result;
            }
            reply_ = !ResolutionEnabled() || registered_ ? "OK " : "ERROR Capability registration failed";
        } else if (Matches(command, verb, "Set_Option")) {
            uint32_t name = argument;
            while (name < size && command[name] != ' ') ++name;
            uint32_t value = name;
            while (value < size && command[value] == ' ') ++value;
            if (argument == size || value == size) {
                reply_ = "ERROR Invalid arguments";
            } else {
                if (Matches(command + argument, name - argument, "broadcastIP") &&
                    Matches(command + value, size - value, "1")) ipRequested_ = true;
                reply_ = "OK ";
            }
        } else if (noArguments && (Matches(command, verb, "OS_Halt") || Matches(command, verb, "OS_Reboot"))) {
            pendingPower_ = Matches(command, verb, "OS_Halt") ? PowerOp::Halt : PowerOp::Reboot;
            result.result = RpcResult::PowerRequested;
            result.power = pendingPower_;
            return result;
        } else if (noArguments && (Matches(command, verb, "OS_PowerOn") ||
                                   Matches(command, verb, "OS_Resume") ||
                                   Matches(command, verb, "OS_Suspend"))) {
            // No guest scripts: acknowledge so the host completes the change.
            statusState_ = Matches(command, verb, "OS_PowerOn") ? StatePowerOn :
                           Matches(command, verb, "OS_Resume") ? StateResume : StateSuspend;
            reply_ = "OK ";
        } else if (Matches(command, verb, "Resolution_Set")) {
            ResizeMode mode{};
            if (!registered_) reply_ = "ERROR Invalid guest state";
            else if (!ModeArguments(command, size, verb, limit_, mode))
                reply_ = "ERROR Invalid arguments";
            else {
                awaitingApply_ = true;
                pendingMode_ = mode;
                applyStarted_ = now;
                result.result = RpcResult::ModeRequested;
                result.mode = mode;
                return result;
            }
        } else {
            reply_ = "ERROR Unknown Command";
        }
        return result;
    }

    // Answers a pending OS_Halt/OS_Reboot. An accepted operation is reported
    // as committed by the poll that delivers the reply and success status.
    void AnswerPower(bool accept)
    {
        if (pendingPower_ == PowerOp::None) return;
        if (accept) {
            reply_ = "OK ";
            statusState_ = pendingPower_ == PowerOp::Halt ? rpc::StateHalt : rpc::StateReboot;
            statusCommit_ = pendingPower_;
        } else {
            reply_ = "ERROR Power operation unavailable";
        }
        pendingPower_ = PowerOp::None;
    }

    // Returns the requested mode while it is still within its deadline.
    // An expired request is answered with a timeout instead.
    bool PendingMode(uint64_t now, ResizeMode& mode)
    {
        if (!awaitingApply_) return false;
        if (now - applyStarted_ >= ApplyTimeoutMs) {
            reply_ = "ERROR Resize timed out";
            awaitingApply_ = false;
            return false;
        }
        mode = pendingMode_;
        return true;
    }

    void CompletePending(ResizeApplyResult result)
    {
        if (!awaitingApply_ || result == ResizeApplyResult::Retry) return;
        reply_ = result == ResizeApplyResult::Applied ? "OK " : "ERROR Invalid arguments";
        awaitingApply_ = false;
    }

    template<class Apply>
    bool ApplyPending(uint64_t now, Apply apply)
    {
        if (!awaitingApply_) return false;
        ResizeMode mode{};
        if (!PendingMode(now, mode)) return true;
        const auto result = apply(mode);
        CompletePending(result);
        return result != ResizeApplyResult::Retry;
    }
};

}
