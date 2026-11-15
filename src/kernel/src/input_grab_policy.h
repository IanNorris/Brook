#pragma once
#include "process.h"

namespace brook {
// H-04 hardening: may `caller` replace `current` as the global input grabber?
// Another process may not silently take every keyboard and mouse event away
// from a live grabber; the grabber itself, root (a restarted display server)
// or anyone after the grabber exits may. The wider authorisation policy for
// Brook syscalls is still open (REFACTOR_ISSUES.md, H-03/H-04/H-21).
inline bool InputGrabTakeoverAllowed(const Process* current, const Process* caller)
{
    if (!current || current == caller || caller->euid == 0) return true;
    return current->magic != PROCESS_MAGIC || current->state == ProcessState::Terminated;
}
}
