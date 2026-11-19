#pragma once
#include <stdint.h>
#include "../../shared/desktop_layers_abi.h"
// App-driven present policy for the desktop renderer (BROOK_DESKTOP_POLICY_*
// bits, reported by BROOK_DESKTOP_FOCUS_QUERY). RAM only: set by the
// "desktop present ..." debug commands and reset at boot. Boot default
// (Ian, 19 Nov): app-driven cadence on, direct scanout shadow (counters
// only). Rollback: "desktop present app off" or BROOK_RENDER_APP_PRESENT=0.
namespace brook {
extern uint32_t g_desktopPresentPolicy,g_desktopPresentDeferUs;
inline uint32_t DesktopPresentPolicy(){return __atomic_load_n(&g_desktopPresentPolicy,__ATOMIC_ACQUIRE);}
inline void DesktopPresentPolicySet(uint32_t policy){__atomic_store_n(&g_desktopPresentPolicy,policy,__ATOMIC_RELEASE);}
inline uint32_t DesktopPresentDeferUs(){return __atomic_load_n(&g_desktopPresentDeferUs,__ATOMIC_ACQUIRE);}
inline void DesktopPresentDeferSet(uint32_t us){__atomic_store_n(&g_desktopPresentDeferUs,us,__ATOMIC_RELEASE);}
}
