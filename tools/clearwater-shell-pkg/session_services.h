#pragma once
#include "chrome_tray.h"

namespace brook {
using TrayPublish = bool (*)(const ChromeTrayState&);
using TrayReadInput = int (*)(TrayInput&);
// Runs in the desktop user's process. Poll callbacks return -1 to stop.
int RunSessionServices(TrayPublish, TrayReadInput, int parent);
}
