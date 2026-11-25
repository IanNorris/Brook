#pragma once
// GSP-RM M1 live path (BROOK_NV_GSP): `bridge hardware nv-gsp <verb>`.
// Nothing runs at boot. The verbs act only with /etc/nv-gsp = "1\n" in the
// initrd, and the GPU-writing verbs (p3, fini) run at most once per boot.

namespace brook {
// Handles "bridge hardware nv-gsp ..."; false if the command is not ours.
bool NvGspHandle(const char* command);
}
