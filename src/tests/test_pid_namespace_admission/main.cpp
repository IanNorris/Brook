#include "pid_namespace.h"
#include "process.h"

#include <cassert>
#include <cstdlib>
#include <initializer_list>

using namespace brook;

namespace brook {
namespace { unsigned allocated; }
void* kmalloc(uint64_t size) { ++allocated; return std::malloc(size); }
void kfree(void* object) { if (object) --allocated; std::free(object); }
UserNamespace* ProcessUserNamespaceAcquire(Process*) { return nullptr; }
}

static void RecyclePidRange()
{
    PidIdAllocator ids;
    PidIdLease first, tid, tgid, group, session, temp;
    assert(ids.Reserve(first, true) == 1);
    assert(ids.Reserve(temp, true) == 0);
    PidIdAllocator::Release(first);
    assert(ids.Reserve(first, true) == 1);
    assert(ids.Reserve(tid, false) == 2);
    ids.Retain(tgid, 2);
    ids.Retain(group, 2);
    ids.Retain(session, 2);
    PidIdAllocator::Release(tid);

    // Exercise the actual production range, including a wrap with each
    // type of persistent numeric ownership and reuse after the final drop.
    for (auto* lease : {&tgid, &group, &session}) {
        for (uint32_t number = 3; number < PidNamespaceIdLimit; ++number) {
            assert(ids.Reserve(temp, false) == number);
            PidIdAllocator::Release(temp);
        }
        assert(ids.Reserve(temp, false) == 3);
        PidIdAllocator::Release(temp);
        PidIdAllocator::Release(*lease);
        for (uint32_t number = 4; number < PidNamespaceIdLimit; ++number) {
            assert(ids.Reserve(temp, false) == number);
            PidIdAllocator::Release(temp);
        }
    }
    assert(ids.Reserve(temp, false) == 2);
    PidIdAllocator::Release(temp);
    PidIdAllocator::Release(first);
}

int main()
{
    RecyclePidRange();
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
    assert(PidNamespaceTaskCount(view) == 3 && PidNamespaceTaskCount(nested) == 1);
    assert(PidNamespaceContains(nullptr, nested) && PidNamespaceContains(view, nested));
    assert(PidNamespaceContains(nested, nested) && !PidNamespaceContains(nested, view));
    Process remote{};
    remote.pid = 906; remote.tgid = init.tgid; remote.isThread = true;
    assert(ProcessPidUnshare(&init) == 0);
    assert(ProcessPidPrepareChild(&remote, &init, false) == -22);
    assert(ProcessPidPrepareInjectedThread(&remote, &init) == 0);
    assert(ProcessPidView(&remote) == view && ProcessVisibleTgid(&remote, view) == 1);
    assert(ProcessVisibleTid(&remote, view) == 4 && ProcessPidPublish(&remote));
    ProcessPidRelease(&remote);


    assert(ProcessPidPrepareChild(&child, &sibling, false) == 0);
    assert(PidNamespaceStopBirths(view));
    assert(PidNamespaceDraining(view));
    assert(!PidNamespaceStopBirths(view));
    assert(!ProcessPidPublish(&child));
    ProcessPidRelease(&child);
    assert(PidNamespaceTaskCount(view) == 3 && PidNamespaceTaskCount(nested) == 1);
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
    assert(PidNamespaceTaskCount(view) == 2 && PidNamespaceTaskCount(nested) == 0);
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
