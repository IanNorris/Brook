#pragma once
#include <stdint.h>

// Dell/Broadwell GPU critical-section entry points. Each runs inside the
// kernel page-table critical section provided by the VMM
// (VmmKernelPtCriticalEnter/Exit, VmmKernelPtAuditPage) and accesses device
// MMIO only through audited direct-map aliases. The historical Vmm prefix is
// kept so existing callers and host-test fakes are unchanged.
namespace brook {
// Restricted Dell diagnostic; caller must enforce audited opt-in, firmware,
// boot-map and PCI preconditions before entering this critical section.
bool VmmDellI2cIdCriticalRead(uint32_t& value);
struct DellGpuVtdStatus;
// Internal diagnostic only. Caller must prove exact PCI identity, one unique
// audited DMAR for this boot, no conflicting resources across the register
// page, and an opted-in single BSP attempt. Output is unchanged on failure.
bool VmmDellGpuVtdCriticalRead(DellGpuVtdStatus& status);
struct BdwGopPtePlan;struct BdwGopPtes;
struct BdwDisplaySnapshot;
bool VmmDellDisplayCriticalRead(BdwDisplaySnapshot& result);
// Caller proves audited Dell/GOP identity, BAR extents, GPU D0 and opt-in.
bool VmmDellGopPtesCriticalRead(const BdwGopPtePlan& plan,BdwGopPtes& result);
bool VmmDellGopBackingPage(uint32_t first,uint32_t count,uint64_t& physicalBase);
struct BdwGgttReservedPtes;
struct BdwGgttSurvey;
bool VmmDellGgttSurveyCriticalRead(uint32_t first,BdwGgttSurvey& result);
bool VmmDellReservedGgttCriticalRead(uint32_t first,BdwGgttReservedPtes& result);
// Caller proves audited Dell identity, BAR extent, D0 and opt-in.
bool VmmDellForcewakeAckCriticalRead(uint32_t& value);
struct BdwForcewakeAdapterResult;
struct BdwPatTestResult;
struct BdwBcsRunResult;
struct BdwRcsResetTestResult;
struct BdwRenderGtInitResult;
struct BdwRenderRunResult;
void VmmDellRenderRun(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,
                      uint32_t (&snapshot)[20*1024],BdwForcewakeAdapterResult& result,BdwRenderRunResult& render);
void VmmDellRenderGtInit(uint32_t khz,BdwForcewakeAdapterResult& result,BdwRenderGtInitResult& init);
void VmmDellRcsReset(uint32_t khz,BdwForcewakeAdapterResult& result,BdwRcsResetTestResult& reset);
void VmmDellBcsCopy(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& copy);
#if defined(BROOK_SSD_AUTO_RECOVERY) && defined(BROOK_BDW_SCANOUT_FLIP)
class BdwGgttExtentBind;struct BdwGgttExtentLease;
// Bind (or retire) one kernel flip buffer's GGTT extent through audited UC
// direct-map aliases of its PTE pages, under the page-table lock. Caller is
// the present worker on the BSP (the only GPU owner), owns the reservation
// and pinned pages, and for retire proves the display no longer reads it.
bool VmmDellFlipGgttBind(BdwGgttExtentBind& bind,const BdwGgttExtentLease& lease,bool retire,bool displayQuiescent);
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY)
void VmmDellBcsScanout(uint32_t khz,uint32_t index,uint64_t token,uint64_t scratch,BdwForcewakeAdapterResult& result,BdwBcsRunResult& copy);
#endif
void VmmDellForcewakePatTest(uint32_t khz,BdwForcewakeAdapterResult& result,BdwPatTestResult& pat);
void VmmDellForcewakeCriticalRun(uint32_t khz,BdwForcewakeAdapterResult& result,bool otherEngines=false,bool patRequested=false,bool tlbRequested=false,bool resetRequested=false,bool scopeCheck=false);
struct BdwScratchTakeoverResult;
// Caller pins the exact scratch page/range and establishes device admission;
// an attempted PTE write leaves both pinned, even if this returns failure.
void VmmDellForcewakeScratchRun(uint32_t khz,uint32_t index,uint64_t dma,BdwForcewakeAdapterResult& result,BdwScratchTakeoverResult& scratch);
void VmmDellForcewakeClearStale(uint32_t khz,uint64_t expected,BdwForcewakeAdapterResult& result,BdwScratchTakeoverResult& clear);
struct BdwBcsInitResult;
void VmmDellForcewakeBcsInit(uint32_t khz,uint32_t index,uint64_t dma,uint8_t* cpu,
                             BdwForcewakeAdapterResult& result,BdwBcsInitResult& init);
} // namespace brook
