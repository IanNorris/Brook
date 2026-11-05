# Brook debug bridge and MCP handover

The bridge is already connected on this host. It is a local Python stdio MCP server serving two distinct target families: `vm_*` for opted-in QEMU guests and `hw_*` for explicitly attached physical Brook guests. It does not control the Linux host desktop. The new chat should use the existing connection and read the `brook-vm` skill before operating a guest.

## Current host configuration

| Component | Observed configuration |
| --- | --- |
| MCP registration name | `brook-vm` |
| Host server | `python3 /data/Programming/Codex/Brook/tools/vm_bridge/server.py` |
| Checkout environment | `BROOK_ROOT=/data/Programming/Codex/Brook` |
| VM registry | `/data/Programming/Codex/Brook/build/vm-bridge` |
| Hardware registry | Default `/data/Programming/Codex/Brook/build/hardware-bridge` unless overridden |
| Debug channel | Boot-guarded guest TCP, normally port 1234 on physical hardware |
| Binary channel | Separately authenticated helper, normally port 1236; keys remain in private image/update artifacts |

Connected tools normally appear under `mcp__brook_vm__`. Discover their current definitions rather than rebuilding a remembered schema. Configuration and registries persist across chat changes; they do not require restarting guests. No server registration or guest state was changed when preparing this handover.

## What onboarding should establish

Read the installed skill at `/home/ian/.codex/skills/brook-vm/SKILL.md`, or the bundled [skill entrypoint](../../tools/vm_bridge/skills/brook-vm/SKILL.md) if installation/discovery is unavailable. Understand the target families, operation identities and where the local registries live. Reading local configuration/records is sufficient for onboarding; no guest probe, screenshot, test launch, transfer or reboot is required.

The final paused attachment, boot ID and completed recovery job IDs are in [evidence/final-state.json](/data/Programming/DSHScratch/brook-handoff/evidence/final-state.json). They are historical provenance, not permission to replay actions or assume a current connection. Before a later authorized hardware action, select the intended attachment and establish current identity/capabilities. Explicitly reattach after a boot change. Onboarding has been confirmed; Ian will separately confirm when he is ready for the physical F12/F5 test.

## Operational distinctions worth retaining

- `*_guest_run` starts a structured guest command job; `*_job` observes that same operation. A timeout or cancelled wait does not stop it. Preserve independent stdout/stderr cursors, exit status and descendant/EOF state.
- `*_apps`/`*_launch` exercise the installed-app launcher. `*_windows`/`*_window` use semantic window references; `*_ui` recovers the same UI request. Window disappearance is separate from process exit.
- `hw_doctor` distinguishes a local lock, TCP reachability, valid replies and matching boot identity. Connection loss alone cannot distinguish panic from network failure. The guest debug listener is serial; duplicate simultaneous probes can compete with real work.
- Current hardware can transfer completed-frame screenshots when both advertised APIs exist. `hw_transfer` starts a guest helper and returns local files/receipts. Older notes declaring every hardware screenshot unavailable are stale. No QMP, arbitrary hardware registers/RAM or post-panic TCP inspection is available through that path.
- `vm_observe` has a capture/pause cost; use saved `vm_frame` views, incremental logs or semantic data when sufficient. Keep binary/base64 payloads out of textual tool output.
- The optical panic receiver must use the exact deployed kernel ELF. Its current process, symbols and service require inspection before decoding another boot; an old scanner's successful health check does not establish matching symbols.
- Graceful lifecycle/power requests have their own retained IDs and supported phases. Reboot invalidates guest-bound references. The three-minute bridge watchdog is still design-only at the paused handover.

The reusable skill holds the detailed workflows. Live Dell payload/configuration state and the recovery todo list remain in HANDOVER.md. Existing implementation detail is in `tools/vm_bridge/README.md`, `server.py`, `hardware.py` and `tools/hardware_bulk_workflow.py` under the Brook checkout. No broad acceptance rerun is needed to onboard.

## Additional repository skill context

For VM recording, prepare `build/vm-recording-runtime` using `recording-runtime.nix` before launching the VM. `BROOK_AUDIODEV=none` retains guest sound capture without speaker output. Recording timestamps reflect host transport arrival; upstream drops and exact source audio/video timing remain unknown.

Saved frame crops use original display coordinates; the optional overlay adds a native-coordinate ruler and grid. A panic frame's geometry can belong to an earlier boot and is unverified. A diagnostic reset on an older or custom launch without a known reboot-policy restoration path may be refused. Consult the current tool schema and implementation when these details matter.
