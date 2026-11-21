#include "../../kernel/src/vmware_rpc_port.h"

#include <cassert>
#include <cstdio>
#include <string_view>
#include <vector>

using brook::vmware::ResizeApplyResult;
using brook::vmware::ResizeMode;
using brook::vmware::ToolsRpc;
using brook::vmware::RpcResult;
using brook::vmware::rpc::Registers;

namespace {

// Encode the expected protocol independently of the production register
// builder and parser. The mock never executes a privileged instruction.
constexpr uint32_t Magic = 0x564d5868, Port = 0x5658, Message = 30;
constexpr uint32_t Tclo = 0x4f4c4354, Rpci = 0x49435052, Cookie = 0x80000000;
constexpr uint32_t Ok = 1, Data = 2, Checkpoint = 0x10, HighBandwidth = 0x80;
constexpr uint32_t Open = 0, SendLength = 1, SendData = 2, RecvLength = 3;
constexpr uint32_t RecvData = 4, RecvStatus = 5, Close = 6;

struct Endpoint { uint16_t id; uint32_t high, low; };
constexpr Endpoint Listener{0, 0xa2b4c681, 0xeed84910};
constexpr Endpoint Client{23, 0x03811234, 0x429862aa};

struct Exchange { Registers in, out; };
class Backdoor {
    std::vector<Exchange> exchanges_;
    size_t index_ = 0;
public:
    bool operator()(Registers& r) {
        assert(index_ < exchanges_.size());
        const auto& x = exchanges_[index_++];
        assert(r.ax == x.in.ax && r.bx == x.in.bx && r.cx == x.in.cx &&
               r.dx == x.in.dx && r.si == x.in.si && r.di == x.in.di);
        r = x.out;
        return true;
    }

    void CheckConsumed() const { assert(index_ == exchanges_.size()); }

    void Negotiate(uint32_t protocol, Endpoint endpoint, bool cookies, bool succeed = true) {
        exchanges_.push_back({{Magic, protocol | (cookies ? Cookie : 0), Message | Open << 16, Port, 0, 0},
                              {0, 0, succeed ? Ok << 16 : 0, uint32_t(endpoint.id) << 16,
                               endpoint.high, endpoint.low}});
    }

    void Command(Endpoint endpoint, uint32_t op, uint32_t value, uint32_t status = Ok,
                 uint32_t reply = 0, uint32_t replyType = 0) {
        exchanges_.push_back({{Magic, value, Message | op << 16, Port | uint32_t(endpoint.id) << 16,
                               endpoint.high, endpoint.low},
                              {0, reply, status << 16, replyType << 16, 0, 0}});
    }

    static uint32_t LittleWord(std::string_view data) {
        uint32_t word = 0;
        for (size_t i = 0; i < data.size(); ++i)
            word |= uint32_t(uint8_t(data[i])) << (8 * i);
        return word;
    }

    void Send(Endpoint endpoint, std::string_view data, uint32_t extra = 0) {
        Command(endpoint, SendLength, data.size(), Ok | extra);
        for (size_t i = 0; i < data.size(); i += 4)
            Command(endpoint, SendData, LittleWord(data.substr(i, 4)));
    }

    void Receive(Endpoint endpoint, std::string_view data, uint32_t extra = 0) {
        Command(endpoint, RecvLength, 0, Ok | Data | extra, data.size(), SendLength);
        for (size_t i = 0; i < data.size(); i += 4)
            Command(endpoint, RecvData, Ok, Ok, LittleWord(data.substr(i, 4)), SendData);
        Command(endpoint, RecvStatus, Ok);
    }

    void Poll(std::string_view reply, std::string_view request, uint32_t extra = 0) {
        Send(Listener, reply, extra);
        Receive(Listener, request, extra);
    }

    void EmptyPoll(std::string_view reply) {
        Send(Listener, reply);
        Command(Listener, RecvLength, 0);
    }

    void OpenChannels() {
        Negotiate(Tclo, Listener, true);
        Negotiate(Rpci, Client, true);
    }

    void CloseChannels() {
        Command(Listener, Close, 0);
        Command(Client, Close, 0);
    }

    void Call(std::string_view request, std::string_view response = "1 ", uint32_t extra = 0) {
        Send(Client, request, extra);
        Receive(Client, response, extra);
    }

    void Announce(uint32_t extra = 0) {
        Call("tools.set.version 2147483647", "1 ", extra);
        Call("tools.capability.statechange ", "1 ", extra);
        Call("tools.capability.softpowerop_retry ", "1 ", extra);
    }

    void Register(std::string_view response = "1 ", uint32_t extra = 0) {
        Announce(extra);
        Send(Client, "tools.capability.resolution_server toolbox 1", extra);
        Receive(Client, "1 ", extra);
        constexpr char enable[] = "tools.capability.resolution_set 1";
        Send(Client, std::string_view(enable, sizeof(enable)), extra);
        Receive(Client, response, extra);
    }
};

constexpr ResizeMode Limits{3840, 2160};

void ResizeSession() {
    Backdoor io;
    io.OpenChannels();
    ToolsRpc rpc(io);
    rpc.SetResolutionLimit(Limits);
    assert(rpc.Poll().result == RpcResult::Closed);
    assert(rpc.Open());
    io.EmptyPoll("");
    assert(rpc.Poll().result == RpcResult::Idle);
    io.Poll("", "reset", HighBandwidth);
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("OK ATR toolbox", "Capabilities_Register ", HighBandwidth);
    io.Register("1 ", HighBandwidth);
    assert(rpc.Poll().result == RpcResult::Handled);

    constexpr char hostRequest[] = "Resolution_Set 1920 1080";
    io.Poll("OK ", std::string_view(hostRequest, sizeof(hostRequest)), HighBandwidth);
    auto request = rpc.Poll();
    assert(request.result == RpcResult::ModeRequested);
    assert(request.mode.width == 1920 && request.mode.height == 1080);
    assert(rpc.Poll().result == RpcResult::WaitingForApply);
    io.CheckConsumed();
    auto retry = [](ResizeMode mode) {
        assert(mode.width == 1920 && mode.height == 1080);
        return ResizeApplyResult::Retry;
    };
    auto applied = [](ResizeMode) { return ResizeApplyResult::Applied; };
    assert(!rpc.ApplyPending(1, retry));
    assert(!rpc.ApplyPending(900, retry));
    assert(rpc.Poll(901).result == RpcResult::WaitingForApply);
    assert(rpc.ApplyPending(999, applied));
    assert(!rpc.ApplyPending(999, applied));
    io.Poll("OK ", "Resolution_Set  800  600");
    request = rpc.Poll();
    assert(request.result == RpcResult::ModeRequested);
    assert(request.mode.width == 800 && request.mode.height == 600);
    assert(rpc.ApplyPending(0, [](ResizeMode) { return ResizeApplyResult::Invalid; }));
    io.Poll("ERROR Invalid arguments", "Resolution_Set 800 600");
    assert(rpc.Poll(2500).result == RpcResult::ModeRequested);
    assert(!rpc.ApplyPending(3499, [](ResizeMode) { return ResizeApplyResult::Retry; }));
    assert(rpc.ApplyPending(3500, [](ResizeMode) {
        assert(false && "expired resize must not run");
        return ResizeApplyResult::Applied;
    }));
    io.EmptyPoll("ERROR Resize timed out");
    assert(rpc.Poll(3501).result == RpcResult::Idle);

    static constexpr std::string_view invalid[] = {
        "Resolution_Set -1 600", "Resolution_Set 42949672960 600",
        "Resolution_Set 3841 720", "Resolution_Set 1280 2161", "Resolution_Set 800 0",
        "Resolution_Set +800 600", "Resolution_Set 800", "Resolution_Set 800 600 garbage",
        "Resolution_Set 800 600\n", std::string_view("Resolution_Set 800\0 600", 23),
    };
    std::string_view previousReply;
    for (auto cmd : invalid) {
        io.Poll(previousReply, cmd);
        assert(rpc.Poll().result == RpcResult::Handled);
        assert(!rpc.ApplyPending(0, [](ResizeMode) { return ResizeApplyResult::Applied; }));
        previousReply = "ERROR Invalid arguments";
    }
    io.Poll("ERROR Invalid arguments", "DisplayTopology_Set 1");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("ERROR Unknown Command", "reset");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("OK ATR toolbox", "Resolution_Set 800 600");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("ERROR Invalid guest state", "ping");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.EmptyPoll("OK ");
    assert(rpc.Poll().result == RpcResult::Idle);
    io.CloseChannels();
    rpc.Close();
    rpc.Close();
    io.CheckConsumed();
}

void FallbackAndErrors() {
    Backdoor io;
    ToolsRpc rpc(io);
    rpc.SetResolutionLimit(Limits);
    io.Negotiate(Tclo, Listener, true, false);
    io.Negotiate(Tclo, Listener, false, false);
    assert(!rpc.Open());
    io.CheckConsumed();
    assert(rpc.Poll().result == RpcResult::Closed);

    io.Negotiate(Tclo, Listener, true);
    io.Negotiate(Rpci, Client, true, false);
    io.Negotiate(Rpci, Client, false, false);
    io.Command(Listener, Close, 0);
    assert(!rpc.Open());
    io.CheckConsumed();

    constexpr Endpoint LegacyListener{71, 0, 0};
    io.Negotiate(Tclo, LegacyListener, true, false);
    // Non-cookie replies need not provide usable cookie words.
    io.Negotiate(Tclo, {71, 0x55, 0xaa}, false);
    io.Negotiate(Rpci, Client, true);
    assert(rpc.Open());
    io.Send(LegacyListener, "");
    io.Command(LegacyListener, RecvLength, 0, Ok | Data, 257, SendLength);
    io.Command(LegacyListener, RecvData, 0);
    io.Command(LegacyListener, Close, 0);
    io.Command(Client, Close, 0);
    assert(rpc.Poll().result == RpcResult::TransportError);
    io.CheckConsumed();
    assert(!rpc.ApplyPending(0, [](ResizeMode) { return ResizeApplyResult::Applied; }));

    io.OpenChannels();
    assert(rpc.Open());
    io.Poll("", "Capabilities_Register");
    io.Register("0 unavailable");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.EmptyPoll("ERROR Capability registration failed");
    assert(rpc.Poll().result == RpcResult::Idle);

    // A checkpoint interrupts a payload. Reject it and reopen at caller pace.
    io.Send(Listener, "");
    io.Command(Listener, RecvLength, 0, Ok | Data, 16, SendLength);
    io.Command(Listener, RecvData, Ok, Checkpoint);
    io.Command(Listener, RecvData, 0);
    io.CloseChannels();
    assert(rpc.Poll().result == RpcResult::TransportError);
    io.CheckConsumed();
    assert(rpc.Poll().result == RpcResult::Closed);

    io.OpenChannels();
    assert(rpc.Open());
    io.Send(Listener, "");
    io.Command(Listener, RecvLength, 0, Ok | Data, 4, SendLength);
    io.Command(Listener, RecvData, Ok, Ok, Backdoor::LittleWord("ping"), SendData);
    io.Command(Listener, RecvStatus, Ok, Checkpoint);
    io.Command(Listener, RecvStatus, 0);
    io.CloseChannels();
    assert(rpc.Poll().result == RpcResult::TransportError);
    io.CheckConsumed();

    io.OpenChannels();
    assert(rpc.Open());
    io.Poll("", "reset");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.EmptyPoll("OK ATR toolbox");
    assert(rpc.Poll().result == RpcResult::Idle);
    io.CloseChannels();
    rpc.Close();
    io.CheckConsumed();
}

void PowerAndGuestInfo() {
    using brook::vmware::PowerOp;
    Backdoor io;
    ToolsRpc rpc(io);
    io.OpenChannels();
    assert(rpc.Open());
    io.Announce();
    assert(rpc.Announce());
    io.Call("log brook: tools service started");
    assert(rpc.Log("brook: tools service started"));
    io.Call("SetGuestInfo  2 192.168.10.129", "0 deprecated");
    io.Call("info-set guestinfo.ip 192.168.10.129");
    assert(rpc.SendGuestIp(0x810aa8c0));

    io.Poll("", "reset");
    assert(rpc.Poll().result == RpcResult::Handled);
    // Without a resolution limit only the toolbox capabilities register.
    io.Poll("OK ATR toolbox", "Capabilities_Register");
    io.Announce();
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("OK ", "Set_Option broadcastIP 1");
    assert(rpc.Poll().result == RpcResult::Handled);
    assert(rpc.TakeIpRequest() && !rpc.TakeIpRequest());
    io.Poll("OK ", "Set_Option enableDnD 0");
    assert(rpc.Poll().result == RpcResult::Handled && !rpc.TakeIpRequest());
    io.Poll("OK ", "Set_Option");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("ERROR Invalid arguments", "Resolution_Set 800 600");
    assert(rpc.Poll().result == RpcResult::Handled);

    io.Poll("ERROR Invalid guest state", std::string_view("OS_Halt\0", 8));
    auto halt = rpc.Poll();
    assert(halt.result == RpcResult::PowerRequested && halt.power == PowerOp::Halt &&
           halt.committed == PowerOp::None);
    io.CheckConsumed();
    // Unanswered: no further backdoor traffic and the request is repeated.
    halt = rpc.Poll();
    assert(halt.result == RpcResult::PowerRequested && halt.power == PowerOp::Halt);
    io.CheckConsumed();
    rpc.AnswerPower(true);
    rpc.AnswerPower(false);
    io.Send(Listener, "OK ");
    io.Call("tools.os.statechange.status 1 1");
    io.Command(Listener, RecvLength, 0);
    halt = rpc.Poll();
    assert(halt.result == RpcResult::Idle && halt.committed == PowerOp::Halt);

    io.EmptyPoll("");
    assert(rpc.Poll().committed == PowerOp::None);
    io.Poll("", "OS_Reboot");
    auto reboot = rpc.Poll();
    assert(reboot.result == RpcResult::PowerRequested && reboot.power == PowerOp::Reboot);
    rpc.AnswerPower(false);
    io.Poll("ERROR Power operation unavailable", "OS_Halt now");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("ERROR Unknown Command", "OS_Suspend");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Send(Listener, "OK ");
    io.Call("tools.os.statechange.status 1 5");
    io.Receive(Listener, "OS_Reboot");
    reboot = rpc.Poll();
    assert(reboot.result == RpcResult::PowerRequested && reboot.power == PowerOp::Reboot &&
           reboot.committed == PowerOp::None);
    rpc.AnswerPower(true);
    // A transport error after the status was delivered still commits.
    io.Send(Listener, "OK ");
    io.Call("tools.os.statechange.status 1 2");
    io.Command(Listener, RecvLength, 0, 0);
    io.CloseChannels();
    reboot = rpc.Poll();
    assert(reboot.result == RpcResult::TransportError && reboot.committed == PowerOp::Reboot);
    io.CheckConsumed();
    assert(rpc.Poll().result == RpcResult::Closed);

    io.OpenChannels();
    assert(rpc.Open());
    io.Poll("", "Time_Synchronize 1");
    assert(rpc.Poll().result == RpcResult::Handled);
    io.Poll("ERROR Unknown Command", "OS_Halt");
    assert(rpc.Poll().result == RpcResult::PowerRequested);
    // Close drops an unanswered request instead of replaying it.
    io.CloseChannels();
    rpc.Close();
    assert(rpc.Poll().result == RpcResult::Closed);
    io.CheckConsumed();
}

// Instantiate the six-register production port function for build-time
// checking. The host fixture executes the scripted backend only.
[[gnu::used]] bool CompilePortAdapter(brook::vmware::RpcPort& port, Registers& message) {
    return port(message);
}

}

int main() {
    ResizeSession();
    FallbackAndErrors();
    PowerAndGuestInfo();
    puts("VMWARE_RPC_PASS TCLO/RPCI, low bandwidth with HB advertised, validated modes, bounded reset, power ops, guest info");
}
