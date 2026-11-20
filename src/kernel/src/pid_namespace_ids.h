#pragma once

#include <stdint.h>

namespace brook {

struct PidNamespace;

// Global scheduler IDs remain in Process. The view, TID and TGID are
// immutable after publication. Group and session IDs use the registry lock.
struct PidNamespaceIds {
    static constexpr unsigned MaxDepth = 8;

    struct Entry {
        const PidNamespace* ns;
        uint32_t tid;
        uint32_t tgid;
        uint32_t group;
        uint32_t session;
    };

    Entry entries[MaxDepth]{};
    unsigned count = 0;

    const PidNamespace* Active() const {
        return count ? entries[count - 1].ns : nullptr;
    }

    bool Add(const PidNamespace* ns, uint32_t tid, uint32_t tgid) {
        if (!ns || !tid || !tgid || count >= MaxDepth || Find(ns)) return false;
        entries[count++] = {ns, tid, tgid, 0, 0};
        return true;
    }

    uint32_t VisibleTid(const PidNamespace* view, uint32_t globalTid) const {
        if (!view) return globalTid;
        const auto* entry = Find(view);
        return entry ? entry->tid : 0;
    }

    uint32_t VisibleTgid(const PidNamespace* view, uint32_t globalTgid) const {
        if (!view) return globalTgid;
        const auto* entry = Find(view);
        return entry ? entry->tgid : 0;
    }

    uint32_t VisibleRelation(const PidNamespace* view, uint32_t globalId, bool session) const {
        if (!view) return globalId;
        const auto* entry = Find(view);
        return entry ? (session ? entry->session : entry->group) : 0;
    }

private:
    const Entry* Find(const PidNamespace* view) const {
        for (unsigned i = 0; i < count; ++i)
            if (entries[i].ns == view) return &entries[i];
        return nullptr;
    }
};

} // namespace brook
