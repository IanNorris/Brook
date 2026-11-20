#include <cstdio>
#include "../../kernel/src/desktop_task_manager.cpp"

namespace {
unsigned failures = 0;
brook::Workspace sample{};

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "task manager line %d: %s\n", __LINE__, #expr); ++failures; } \
} while (0)

void Add(unsigned index, uint16_t pid, uint16_t group, uint64_t incarnation, uint64_t groupIncarnation,
         uint64_t ticks, uint32_t uid = 1000, bool system = false)
{
    auto& event = sample.tasks[index];
    event.pid = pid;
    event.tgid = group;
    event.incarnation = incarnation;
    event.groupIncarnation = groupIncarnation;
    event.userTicks = ticks;
    event.sysTicks = 1;
    event.uid = event.euid = uid;
    event.systemService = system;
    event.name[0] = char('a' + index);
}

const BrookTaskManagerRow& Group(uint16_t id) { return sample.rows[sample.groupRows[id] - 1]; }
}

int main()
{
    // Thread may appear before the group leader. Prefer the leader's name and
    // identity, while counting the worker's CPU exactly once.
    Add(0, 23, 17, 47, 42, 25);
    Add(1, 17, 17, 42, 42, 47);
    Add(2, 16, 16, 40, 40, 9);
    Add(3, 12, 12, 32, 32, 8, 0);
    Add(4, 15, 15, 39, 39, 6, 1000, true);
    Add(5, 4, 4, 11, 11, 13, 2000);
    Add(6, 18, 18, 43, 43, 7);
    sample.tasks[6].state = brook::ProcessState::Terminated;
    Add(7, 24, 20, 48, 45, 4); // live thread after its leader exited

    CHECK(CollectGroups(sample, 8, 16, 1000) == 6);
    const auto& group = Group(17);
    CHECK(group.pid == 17 && group.incarnation == 42 && group.cpu_ms == 74);
    CHECK(group.threads == 2 && group.name[0] == 'b' && group.flags == BROOK_TASK_MANAGER_CAN_END);
    CHECK(sample.representativePid[sample.groupRows[17] - 1] == 17);
    CHECK(sample.liveGroups[17] == 42);
    CHECK(!sample.groupRows[18] && !sample.liveGroups[18]);
    CHECK(Group(12).flags == BROOK_TASK_MANAGER_SYSTEM && Group(15).flags == BROOK_TASK_MANAGER_SYSTEM);
    CHECK(Group(16).flags == 0 && Group(4).flags == 0 && Group(20).flags == 0);
    CHECK(Group(20).incarnation == 45 && Group(20).threads == 1 && Group(20).cpu_ms == 5);

    CHECK(brook::DesktopTaskMayEnd(1000, 16, 17, 1000, 1000, false));
    CHECK(!brook::DesktopTaskMayEnd(1000, 16, 1, 1000, 1000, false));
    CHECK(!brook::DesktopTaskMayEnd(1000, 16, 16, 1000, 1000, false));
    CHECK(!brook::DesktopTaskMayEnd(1000, 16, 17, 2000, 2000, false));
    CHECK(!brook::DesktopTaskMayEnd(1000, 16, 17, 1000, 0, false));
    CHECK(!brook::DesktopTaskMayEnd(1000, 16, 17, 1000, 1000, true));
    return failures != 0;
}
