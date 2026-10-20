#include "module_abi.h"
#include "sched_ops.h"
#include "scheduler.h"
MODULE_IMPORT_SYMBOL(SchedulerRegisterPolicy);
static int Init(){return brook::SchedulerRegisterPolicy(GetSchedOps())?0:-1;}
static void Exit(){}
DECLARE_MODULE("sched_eevdf",Init,Exit,"Experimental EEVDF scheduler policy");
