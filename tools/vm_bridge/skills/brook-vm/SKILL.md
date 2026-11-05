---
name: brook-vm
description: Operate the Brook debug bridge MCP for opted-in QEMU VMs and explicitly selected physical Brook machines. Use for guest inspection, command jobs, application and window control, capture, transfers, bridge diagnostics, and guest lifecycle operations. Does not control the host desktop.
---

# Brook debug bridge

Use the connected `brook-vm` MCP server. Tools are normally named `mcp__brook_vm__vm_*` and `mcp__brook_vm__hw_*`; discover their current schemas before forming calls. Select the physical or virtual family explicitly. A session ID from one registry is not interchangeable with the other.

This skill covers operating the bridge. It does not start a test campaign or grant permission for a deployment, reboot, filesystem repair or unrelated guest work. During read-only handover onboarding, inspect local documentation, configuration and existing records; do not launch a VM, probe the Dell, or run acceptance tests just to establish that the skill works.

## Find the intended guest

- **Physical machine:** `hw_sessions` lists existing opted-in attachments without network probing. Select the intended record. For an explicit current IPv4 endpoint, `hw_attach` pins the boot; `hw_doctor` checks identity and advertised capabilities. Reattach explicitly after a reboot or identity mismatch. Read [hardware.md](references/hardware.md) for diagnostics, transfers or power operations.
- **QEMU:** `vm_sessions` lists currently attachable runs; follow pagination if needed. `vm_doctor` checks the selected transport and guest. Attach to an existing user run without restarting it. `vm_start` creates new experiment disk clones and is appropriate when the task calls for a new disposable run. Read [vm.md](references/vm.md) for observation, input, profiling, panic inspection or lifecycle operations.

Keep the returned session generation and guest boot identity with operation records. Fetch fresh app/window references after a boot change. An old frame, process ID, directory path or screenshot cannot establish current target identity.

## Choose the smallest useful operation

Use `*_apps` and `*_windows` for catalog/window discovery, `*_stats` for bounded metrics, and incremental `*_logs` for a specific diagnostic. Screenshots and full observation bundles are optional costs. Prefer semantic launch/window actions for ordinary application control; use structured guest jobs for command execution.

Read [jobs.md](references/jobs.md) before the first command job or when recovering uncertain job/UI delivery. Persist a fresh 32-character lowercase hexadecimal request ID before each new action. A timeout is an observation outcome: inspect the same operation with `*_job`, `*_ui`, lifecycle status or reset status. Do not create a new request ID to recover a lost reply.

Request success, a visible window, process exit, drained descendants and checked device flush establish different facts. Verify the one needed for the current task. Poll within the supported wait budget and use returned cursors; avoid rapid duplicate polling or repeated full logs. Stop after the relevant result unless changed evidence requires another check.

## Source and installation

On Ian's current host, the registered stdio command is `python3 /data/Programming/Codex/Brook/tools/vm_bridge/server.py`, with `BROOK_ROOT=/data/Programming/Codex/Brook` and VM registry `build/vm-bridge`. The physical default registry is `build/hardware-bridge`. Resolve paths from the active checkout/configuration when working elsewhere.

This skill adapts the existing repository skill at `tools/vm_bridge/skills/brook-vm/SKILL.md`. Current tool schemas, `server.py`, `hardware.py` and the implementation override stale capability claims in old notes. The maintained API guide is `tools/vm_bridge/README.md`; use its relevant section, not the whole historical test inventory. No bridge implementation or registration is bundled here.

If MCP discovery is unavailable, inspect the existing registration first. The same implementation supports `python3 tools/vm_bridge/server.py --call TOOL --arguments 'JSON'` from the checkout. It has the same operation side effects and authorization boundaries as MCP. Do not re-register servers or change host configuration merely to inspect a guest.
