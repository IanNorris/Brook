#pragma once
#include <stdint.h>

namespace brook {
struct CredentialIds { uint32_t real, effective, saved; };
enum class CredentialChange { Single, Re, Res };
static constexpr uint32_t CredentialUnchanged = ~uint32_t{0};

// Linux ID transition rules, using the caller's privilege decision. Validate
// against the complete old tuple and publish only a successful result.
inline int ChangeCredentialIds(CredentialIds& ids, CredentialChange operation,
                               bool privileged, uint32_t real, uint32_t effective = CredentialUnchanged,
                               uint32_t saved = CredentialUnchanged) {
    const auto old = ids;
    auto member = [&](uint32_t value) {
        return value == CredentialUnchanged || value == old.real || value == old.effective || value == old.saved;
    };
    if (operation == CredentialChange::Single) {
        if (real == CredentialUnchanged) return -22;
        if (privileged) ids = {real, real, real};
        else {
            if (real != old.real && real != old.saved) return -1;
            ids.effective = real;
        }
    } else if (operation == CredentialChange::Res) {
        if (!privileged && (!member(real) || !member(effective) || !member(saved))) return -1;
        if (real != CredentialUnchanged) ids.real = real;
        if (effective != CredentialUnchanged) ids.effective = effective;
        if (saved != CredentialUnchanged) ids.saved = saved;
    } else {
        if (!privileged && ((real != CredentialUnchanged && real != old.real && real != old.effective) ||
                            !member(effective))) return -1;
        if (real != CredentialUnchanged) ids.real = real;
        if (effective != CredentialUnchanged) ids.effective = effective;
        if (real != CredentialUnchanged || (effective != CredentialUnchanged && effective != old.real))
            ids.saved = ids.effective;
    }
    return 0;
}
} // namespace brook
