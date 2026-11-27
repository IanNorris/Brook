#pragma once

#include "../../shared/desktop_layers_abi.h"

namespace brook {

struct Process;
bool DesktopBootRendererAuthorized(Process* proc);
bool DesktopNvRendererOwnedBy(uint64_t owner);
void DesktopNvRendererRevoke(uint64_t owner);

inline bool DesktopLayerContentOperation(uint64_t op)
{
    switch (op) {
    case BROOK_DESKTOP_LAYERS_QUERY:
    case BROOK_DESKTOP_LAYERS_CAPTURE:
    case BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED:
    case BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE:
    case BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_KEEP:
    case BROOK_DESKTOP_CHROME_QUERY:
    case BROOK_DESKTOP_CHROME_CAPTURE:
    case BROOK_DESKTOP_CHROME_TRACKED_CAPTURE:
    case BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_KEYED:
    case BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_SPANS:
    case BROOK_DESKTOP_CHROME_TRACKED_KEEP:
        return true;
    default:
        return false;
    }
}

inline bool DesktopLayerContentAllowed(bool service, bool originalNamespace, bool bootRenderer, bool attached)
{
    return service && originalNamespace && bootRenderer && attached;
}

} // namespace brook
