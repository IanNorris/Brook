# Opt-in hardware bridge watchdog

An optional unattended mode should recover a Dell whose debug bridge has stopped
making progress. It must remain off by default and be explicitly enabled for
the current boot; the MCP client should show whether it is armed.

For a physical machine, a host-only timer cannot reset a guest after the only
network channel has failed. A guest-side watchdog needs periodic, boot-bound
heartbeats from the selected MCP host and should request a checked restart when
they have been absent for three minutes. The heartbeat must traverse the actual
bridge path, not merely observe that the kernel's network thread is alive. A
fresh boot must not immediately rearm from stale host state; require an explicit
new arm after reconnecting to avoid a reboot loop when the host or cable is down.

The timer is a liveness signal, not proof of a guest fault: host sleep, network
failure and a wedged listener all look alike. Report the last successful
heartbeat and eventual reboot reason. Never automatically reset while an SSD or
USB filesystem repair, boot-slot update, flush or shutdown is in progress;
these operations need an explicit inhibition lease and observable completion.
If a checked restart cannot quiesce or the guest itself is completely stuck,
this software mechanism alone cannot guarantee recovery. A hardware watchdog
or an independently controlled power device would be needed for that case.

For QEMU, an independent host-side watchdog can use its existing lifecycle
channel, but it should follow the same opt-in, boot-identity and repair
inhibition rules. Test transient failures, host loss, expiry, reattachment,
reboot-loop prevention, and repair/update inhibition on disposable media before
enabling it on the Dell. No watchdog is implemented or active yet.
