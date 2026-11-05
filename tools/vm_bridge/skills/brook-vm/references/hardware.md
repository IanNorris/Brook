# Physical Brook bridge

Use `hw_*` for the explicitly selected Brook machine. Hardware tools do not provide QMP, host input, arbitrary registers/RAM or post-panic TCP access. Current completed-frame transfer is an optional guest capability; older documentation saying all hardware screenshots are unavailable is superseded by the implementation and capability probe.

## Attach and diagnose

`hw_sessions` reads the local opted-in registry without scanning or probing the LAN. `hw_attach` takes a literal current IPv4 address and optional port, default 1234. Use the endpoint displayed by the guest or supplied for the task; do not guess it from a previous boot or scan the subnet. Attaching pins a boot identity. The development debug port assumes a trusted network; its boot guard prevents stale targeting and is not authentication.

On an authorized inspection, use `hw_doctor` and distinguish:

| Result | Meaning and next step |
| --- | --- |
| `reachable: null`, local lock stage | No network reachability conclusion; inspect the local contention/owner. |
| `reachable: false` | A connection failed; it does not identify a panic or a dead machine. |
| `reachable: true`, incomplete probe | TCP connected, but valid capability/identity replies are not established. |
| `identity_matches: false` | The pinned boot changed; establish the intended new boot and attach explicitly. |
| Verified probe and matching identity | Consult current capabilities for the requested operation; this is not an application-health guarantee. |

For a focused host-side connection diagnosis, `scripts/probe_hardware_link.py TARGET_IPV4` reports route, neighbor state, ping and a framed capabilities request. It sends guest/network traffic, so it is not needed for offline onboarding. Use one bounded probe unless another sample could change the diagnosis. The debug listener processes one connection at a time: avoid concurrent/redundant guest probes that compete with an existing command. A zero boot ID before attach is not a pinned identity.

`hw_logs` uses completed-line cursors, not VM byte offsets. Follow `next_cursor` and report retention loss. `hw_stats`, `hw_apps` and `hw_windows` provide bounded metrics and references. `hw_command` sends a boot-guarded kernel debug command, not a POSIX shell command; use structured jobs for guest programs.

Connection loss can be link failure, reboot, panic or a guest hang. Recover operation state without replay where the transport still works; otherwise retain the uncertainty and arrange a local observation. Physical panic handling halts CPUs, so the debug socket is not a panic recovery channel. Brook's optical route uses `tools/panic_receiver.py` and `tools/panic_scanner.html`. Inspect the current receiver/service and pin its decoder to the exact last-confirmed guest kernel ELF before interpreting stacks; do not assume an old scanner's symbols match a new kernel. Changing the scanner/service is a separate authorized action.

## Binary files and completed screenshots

`hw_doctor` advertises `binary_transfer` and, when both required APIs exist, `frame_capture`. `hw_transfer` supports `put`, `get` and `frame` using an authenticated binary helper, normally on port 1236. It requires `session`, a fresh `request_id`, a local `key_file` and a new host `output` directory. Use `source` for `put`; use the proper artifact ID for `get`. `frame` returns a completed guest-frame PNG by default; raw diagnostic pixels require `keep_raw: true`.

This is an active workflow: it launches `/boot/HWBULK.ELF` through a guarded guest job as root and uses current-boot RAM. It is not a cached screenshot read or automatic installation onto the SSD. Use the existing private key file from the matching prepared image/update workflow, not an invented token; never print or copy its contents into notes. Public debug-port boot identities are not that binary-channel authentication key.

The helper verifies payload hashes and a matching guest/binary receipt. Results contain local paths and metadata rather than embedded binary data. Save `receipt_path`, `transaction_id`, transfer hash and artifact path. Display a returned PNG from its local path when needed. A completed receiver job alone does not prove a matching host artifact was fully received.

The output directory is deliberately created exclusively. On `needs_inspection`, inspect its `receipt.json`. In the current MCP workflow, the supplied `request_id`, receipt `transaction_id` and receiver job `operation_id` are the same 32-character value: use that value with `hw_job` on the original attached boot. Do not rerun a transfer into that directory or create a new transaction merely to get a reply. If the boot changed, treat the old operation as historical; do not query or resubmit it against the new boot as though it could resume there. Uploads staged in RAM disappear after reboot; preserve surviving host evidence and report lost RAM state before deciding whether another upload is needed. Capture/transfer size and lease limits are reported by the guest; do not stream binary data through ordinary command output.

The CLI `scripts/hardware_transfer.py` exposes additional modes such as identity-bound disk export that are not in the current MCP schema. Consult its help and implementation only if the requested task needs them; do not invent extra MCP actions.

## Power and recovery

Inspect `hw_doctor` power capabilities separately: graceful quiescence, reset and automatic power-off can differ. `hw_power` starts shutdown/restart with a fresh request ID. Observe using `operation: "status"` on the same ID. Resolve save dialogs; cancellation is only supported before the service/write-barrier stage. Use commit only for an already-ready request and a supported transition.

An unsupported automatic power-off can still produce a synced, quiescent “safe to power off manually” state; it has not removed physical power. A timeout never justifies a new restart request. After a verified reboot, attach to the new boot explicitly and obtain new references. Do not automatically reboot on bridge loss; the three-minute watchdog described in Brook's notes remains unimplemented at the paused handover.

Current Dell bootloader/payload paths, recovery state and the deferred F12/F5 test belong in HANDOVER.md, not this reusable skill. Keep hardware onboarding read-only until Ian confirms the transition.
