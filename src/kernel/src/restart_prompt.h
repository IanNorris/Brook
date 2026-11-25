#pragma once
#include <stdint.h>

namespace brook {

enum class RestartReason : uint8_t { None, Agent, BootUpdate, SoftwareUpdate };
enum class RestartDecision : uint8_t { Bypassed, Prompting, Accepted, Timeout, Rejected };

inline const char* RestartDecisionName(RestartDecision decision) {
    switch (decision) {
    case RestartDecision::Prompting: return "prompting";
    case RestartDecision::Accepted: return "accepted";
    case RestartDecision::Timeout: return "timeout";
    case RestartDecision::Rejected: return "rejected";
    case RestartDecision::Bypassed: return "bypassed";
    }
    return "bypassed";
}

inline const char* RestartReasonText(RestartReason reason) {
    switch (reason) {
    case RestartReason::BootUpdate: return "Boot update";
    case RestartReason::SoftwareUpdate: return "Software update";
    case RestartReason::Agent: return "Agent restart";
    default: return "";
    }
}

constexpr uint32_t RestartDefaultSeconds = 60;
inline uint32_t RestartTimeoutSeconds(const char* data, uint32_t length) {
    if (!length || length > 4) return RestartDefaultSeconds;
    if (data[length - 1] == '\n') --length;
    uint32_t value = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (data[i] < '0' || data[i] > '9') return RestartDefaultSeconds;
        value = 10 * value + unsigned(data[i] - '0');
    }
    return value && value <= 600 ? value : RestartDefaultSeconds;
}

struct RestartPrompt {
    RestartDecision decision = RestartDecision::Bypassed;
    uint64_t deadline = 0;

    void Begin(uint64_t now, uint32_t seconds, bool desktop, bool force) {
        decision = desktop && !force ? RestartDecision::Prompting : RestartDecision::Bypassed;
        deadline = now + uint64_t(seconds) * 1000;
    }

    bool Choose(RestartDecision choice) {
        if (decision != RestartDecision::Prompting || choice == RestartDecision::Prompting ||
            choice == RestartDecision::Bypassed) return false;
        decision = choice;
        return true;
    }

    bool Tick(uint64_t now) {
        return now >= deadline && Choose(RestartDecision::Timeout);
    }

    uint32_t Remaining(uint64_t now) const {
        if (decision != RestartDecision::Prompting || now >= deadline) return 0;
        return uint32_t((deadline - now + 999) / 1000);
    }
};

} // namespace brook
