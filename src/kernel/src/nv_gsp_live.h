#pragma once
// GSP-RM live path (BROOK_NV_GSP): `bridge hardware nv-gsp <verb>`, plus the boot-time
// autostart (NvGspAutostart). Everything needs /etc/nv-gsp = "1\n" in the initrd, and the
// GPU-writing verbs (p3, p4, fini) run at most once per boot.

namespace brook {
// Handles "bridge hardware nv-gsp ..."; false if the command is not ours.
bool NvGspHandle(const char* command);
// Called by nv-disp's desktop-start thread once head 0 presents: with /etc/nv-gsp set and
// no /etc/nv-gsp-no-autostart, boots GSP-RM (p4 keep-display) and turns the CE present
// path on. Any failure leaves the CPU path (and, before INIT_DONE, no RM) in place.
void NvGspAutostart();
}
