#pragma once
namespace brook {
enum class SignalDefault { Terminate, Ignore, Stop, Continue };
// Linux x86-64 signal numbers, including default-fatal real-time signals.
constexpr SignalDefault DefaultSignalAction(int signal) {
    switch (signal) {
    case 17: case 23: case 28: return SignalDefault::Ignore;
    case 19: case 20: case 21: case 22: return SignalDefault::Stop;
    case 18: return SignalDefault::Continue;
    default: return SignalDefault::Terminate;
    }
}
}
