#include "../../kernel/src/pid_namespace_ids.h"

#include <cassert>

namespace brook { struct PidNamespace {}; }

int main() {
    brook::PidNamespace parent, child, sibling;
    brook::PidNamespaceIds leader;
    assert(!leader.Active() && leader.VisibleTid(nullptr, 104) == 104);
    assert(leader.Add(&parent, 29, 29));
    assert(leader.Add(&child, 1, 1));
    assert(!leader.Add(&parent, 30, 30));

    brook::PidNamespaceIds thread;
    assert(thread.Add(&parent, 30, 29));
    assert(thread.Add(&child, 2, 1));
    assert(leader.Active() == &child && thread.Active() == &child);
    assert(leader.VisibleTid(&parent, 104) == 29);
    assert(leader.VisibleTgid(&child, 104) == 1);
    assert(thread.VisibleTid(&parent, 105) == 30);
    assert(thread.VisibleTgid(&parent, 104) == 29);
    assert(thread.VisibleTgid(&child, 104) == 1);
    assert(thread.VisibleTid(&child, 105) == 2);
    assert(thread.VisibleTid(&sibling, 105) == 0);
    assert(thread.VisibleTgid(&sibling, 104) == 0);
    assert(thread.VisibleTgid(nullptr, 104) == 104);

    brook::PidNamespaceIds deep;
    brook::PidNamespace chain[deep.MaxDepth + 1];
    for (unsigned index = 0; index < deep.MaxDepth; ++index)
        assert(deep.Add(&chain[index], 1, 1));
    assert(!deep.Add(&chain[deep.MaxDepth], 1, 1));
}
