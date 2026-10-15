#pragma once
namespace brook {
struct Process;
void SchedulerBlock(Process*);
void SchedulerUnblock(Process*);
void SchedulerKernelRegionEnter(Process*);
void SchedulerKernelRegionLeave(Process*);
}
