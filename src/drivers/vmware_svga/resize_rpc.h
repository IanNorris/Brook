#pragma once

#include <stdint.h>

namespace brook::svga {

struct ResizeMode { uint32_t width, height; };

enum class ResizeRpcResult {
    Closed, Idle, Handled, ModeRequested, WaitingForApply, TransportError
};

struct ResizeRpcPoll {
    ResizeRpcResult result;
    ResizeMode mode{};
};

namespace resize_rpc {
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
}

// One serialized TCLO session. The caller owns polling cadence, explicit
// Close() on shutdown, and mode policy. In particular, no success reply is sent until it confirms that
// the requested mode was applied (or was already current).
template<class PortIO>
class ResizeRpc {
    PortIO& io_;
    resize_rpc::Channel listener_, client_;
    const char* reply_ = "";
    bool registered_ = false;
    bool awaitingApply_ = false;

    resize_rpc::HostReply Capability(const char* command, uint32_t size)
    {
        using namespace resize_rpc;
        if (!client_.Send(io_, command, size)) return HostReply::TransportError;
        char reply[MessageLimit];
        uint32_t bytes;
        if (client_.Receive(io_, reply, bytes) != ReceiveResult::Message ||
            bytes < 2 || reply[1] != ' ' || (reply[0] != '0' && reply[0] != '1'))
            return HostReply::TransportError;
        return reply[0] == '1' ? HostReply::Ok : HostReply::Declined;
    }

    ResizeRpcPoll Failed()
    {
        // Checkpoint/restore and transport errors invalidate this exchange.
        // The caller can reopen on a later tick. Never replay a resize.
        Close();
        return {ResizeRpcResult::TransportError};
    }

public:
    constexpr explicit ResizeRpc(PortIO& io) : io_(io) {}
    ResizeRpc(const ResizeRpc&) = delete;
    ResizeRpc& operator=(const ResizeRpc&) = delete;

    bool Open()
    {
        if (listener_.IsOpen()) return true;
        if (!listener_.Open(io_, resize_rpc::Tclo)) return false;
        if (client_.Open(io_, resize_rpc::Rpci)) return true;
        listener_.Close(io_);
        return false;
    }

    void Close()
    {
        listener_.Close(io_);
        client_.Close(io_);
        reply_ = "";
        registered_ = awaitingApply_ = false;
    }

    ResizeRpcPoll Poll(ResizeMode limit)
    {
        using namespace resize_rpc;
        if (!listener_.IsOpen()) return {ResizeRpcResult::Closed};
        if (awaitingApply_) return {ResizeRpcResult::WaitingForApply};
        if (!listener_.Send(io_, reply_, Length(reply_))) return Failed();
        reply_ = "";
        char command[MessageLimit];
        uint32_t size;
        switch (listener_.Receive(io_, command, size)) {
        case ReceiveResult::Empty: return {ResizeRpcResult::Idle};
        case ReceiveResult::Failed: return Failed();
        case ReceiveResult::Message: break;
        }
        if (size && command[size - 1] == '\0') --size;
        if (!size) return {ResizeRpcResult::Idle};
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
            static constexpr char server[] = "tools.capability.resolution_server toolbox 1";
            static constexpr char mode[] = "tools.capability.resolution_set 1";
            const auto first = Capability(server, sizeof(server) - 1);
            const auto second = first == HostReply::Ok ? Capability(mode, sizeof(mode)) : first;
            if (second == HostReply::TransportError) return Failed();
            registered_ = second == HostReply::Ok;
            reply_ = registered_ ? "OK " : "ERROR Capability registration failed";
        } else if (Matches(command, verb, "Resolution_Set")) {
            ResizeMode mode{};
            if (!registered_) reply_ = "ERROR Invalid guest state";
            else if (!ModeArguments(command, size, verb, limit, mode))
                reply_ = "ERROR Invalid arguments";
            else {
                awaitingApply_ = true;
                return {ResizeRpcResult::ModeRequested, mode};
            }
        } else {
            reply_ = "ERROR Unknown Command";
        }
        return {ResizeRpcResult::Handled};
    }

    bool CompleteResize(bool applied)
    {
        if (!awaitingApply_) return false;
        awaitingApply_ = false;
        reply_ = applied ? "OK " : "ERROR Invalid arguments";
        return true;
    }
};

}
