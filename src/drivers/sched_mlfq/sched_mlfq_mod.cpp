// sched_mlfq_mod.cpp — MLFQ scheduler loadable module
//
// Wraps the MLFQ scheduler policy as a Brook module that registers itself
// with the kernel's scheduler via SchedulerRegisterPolicy().
//
// Loading this module makes "mlfq" available as a scheduler policy.
// Use "sched mlfq" from the shell to switch to it at runtime.

#include "module_abi.h"
#include "sched_ops.h"
#include "serial.h"
#include "kprintf.h"
#include "scheduler.h"

MODULE_IMPORT_SYMBOL(SchedulerRegisterPolicy);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);

// GetSchedOps is defined in sched_mlfq.cpp (linked into this module).
extern "C" const brook::SchedOps* GetSchedOps();

using namespace brook;

static int SchedMlfqModuleInit()
{
    SerialPuts("sched_mlfq module: init\n");
    const SchedOps* ops = GetSchedOps();
    SchedulerRegisterPolicy(ops);
    KPrintf("sched_mlfq: registered '%s' policy\n", ops->name);
    return 0;
}

static void SchedMlfqModuleExit()
{
    SerialPuts("sched_mlfq module: exit\n");
}

DECLARE_MODULE("sched_mlfq", SchedMlfqModuleInit, SchedMlfqModuleExit,
               "Multi-Level Feedback Queue scheduler policy");
