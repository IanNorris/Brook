#include "pid_namespace.h"
#include "process.h"

#include <cassert>
#include <cstdlib>

using namespace brook;

namespace brook {
namespace { unsigned allocated; }
void* kmalloc(uint64_t size) { ++allocated; return std::malloc(size); }
void kfree(void* object) { if (object) --allocated; std::free(object); }
UserNamespace* ProcessUserNamespaceAcquire(Process*) { return nullptr; }
}

int main()
{
    Process outer{}, abandoned{}, init{}, worker{}, sibling{}, child{};
    outer.pid = outer.tgid = 900;
    abandoned.pid = abandoned.tgid = 901;
    init.pid = init.tgid = 902;
    worker.pid = 903; worker.tgid = 902; worker.isThread = true;
    sibling.pid = sibling.tgid = 904;
    child.pid = child.tgid = 905;

    assert(ProcessPidUnshare(&outer) == 0);
    assert(ProcessPidView(&outer) == nullptr);
    assert(ProcessVisibleTgid(&outer, nullptr) == 900);
    assert(ProcessPidUnshare(&outer) == -22);
    assert(ProcessPidPrepareChild(&abandoned, &outer, false) == 0);
    assert(ProcessPidIsNamespaceInit(&abandoned));
    assert(ProcessPidPrepareChild(&init, &outer, false) == -12);
    ProcessPidRelease(&abandoned);

    assert(ProcessPidPrepareChild(&init, &outer, false) == 0);
    const auto* view = ProcessPidView(&init);
    assert(ProcessPidIsNamespaceInit(&init) && ProcessPidPublish(&init));
    assert(ProcessVisibleTid(&init, view) == 1 && ProcessVisibleTid(&init, nullptr) == 902);
    assert(ProcessVisibleTid(&outer, view) == 0);
    assert(ProcessPidPrepareChild(&worker, &init, false) == 0);
    assert(ProcessPidPublish(&worker) && ProcessPidIsNamespaceInit(&worker));
    assert(ProcessVisibleTid(&worker, view) == 2 && ProcessVisibleTgid(&worker, view) == 1);
    assert(ProcessPidPrepareChild(&sibling, &init, true) == 0);
    assert(ProcessPidPublish(&sibling));
    const auto* nested = ProcessPidView(&sibling);
    assert(ProcessVisibleTgid(&sibling, view) == 3 && ProcessVisibleTgid(&sibling, nested) == 1);
    assert(ProcessVisibleTgid(&init, nested) == 0);
    assert(PidNamespaceContains(nullptr, nested) && PidNamespaceContains(view, nested));
    assert(PidNamespaceContains(nested, nested) && !PidNamespaceContains(nested, view));

    assert(ProcessPidPrepareChild(&child, &sibling, false) == 0);
    assert(PidNamespaceStopBirths(view));
    assert(PidNamespaceDraining(view));
    assert(!PidNamespaceStopBirths(view));
    assert(!ProcessPidPublish(&child));
    ProcessPidRelease(&child);
    Process childAfterExit{};
    assert(ProcessPidPrepareChild(&childAfterExit, &sibling, false) == -12);

    PidNamespaceFinishDrain(view);
    assert(!PidNamespaceDraining(view));
    assert(ProcessPidPrepareChild(&childAfterExit, &sibling, false) == -12);

    const auto credentials = ProcessPidGroupIdentity(&sibling);
    const auto nestedTag = PidNamespaceTag(nested);
    const auto parentTag = PidNamespaceTag(view);
    assert(credentials.Visible(0) == 904);
    assert(credentials.Visible(nestedTag) == 1 && credentials.Visible(parentTag) == 3);

    // A retained proc view pins both namespaces independently of the tasks.
    PidNamespaceRetain(nested);
    ProcessPidRelease(&sibling);
    ProcessPidRelease(&worker);
    ProcessPidRelease(&init);
    ProcessPidRelease(&outer);
    assert(allocated == 2);
    PidNamespaceRelease(nested);
    assert(allocated == 0);
    assert(credentials.Visible(nestedTag) == 1);
    Process replacementParent{}, replacementInit{};
    replacementParent.pid = replacementParent.tgid = 900;
    replacementInit.pid = replacementInit.tgid = 904;
    assert(ProcessPidUnshare(&replacementParent) == 0);
    assert(ProcessPidPrepareChild(&replacementInit, &replacementParent, false) == 0);
    const auto* replacement = ProcessPidView(&replacementInit);
    assert(PidNamespaceTag(replacement) != nestedTag);
    assert(credentials.Visible(PidNamespaceTag(replacement)) == 0);
    ProcessPidRelease(&replacementInit);
    ProcessPidRelease(&replacementParent);
    assert(allocated == 0);
}
