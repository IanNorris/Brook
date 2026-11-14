---
name: brook-vm
description: Operate the Brook debug bridge for opted-in QEMU VMs (vm_*) and explicitly selected physical Brook machines (hw_*). Use for guest inspection, structured command jobs, app/window control, frame capture, binary transfers, bridge diagnostics, guest lifecycle and power operations. Load before any vm_* or hw_* call. Does not control the host desktop.
---

# Brook debug bridge (Claude Code)

This is the Claude Code adaptation of the canonical skill at `tools/vm_bridge/skills/brook-vm/`. The reference files here are copies of that version. When you change the workflow, update both and keep them consistent.

## Invoking the tools

- **MCP registered:** if a `brook-vm` MCP server is registered in this Claude Code session, the tools appear as `mcp__brook-vm__vm_*` and `mcp__brook-vm__hw_*` (check with ToolSearch). Use them directly.
- **MCP not registered:** the same implementation exposes a JSON CLI with identical side effects and authorization boundaries. From the repository root:

  ```sh
  BROOK_ROOT=$PWD python3 tools/vm_bridge/server.py --call hw_doctor --arguments '{"session":"…"}'
  ```

  The result is `{"content":[{"type":"text","text":"<json>"}]}`. `--help` lists every tool name.
- **Multi-step workflows** (jobs plus transfers): Python can import `tools/hardware.py` (`Hardware().guest_run/job/command/windows/stats/doctor`) and `tools/hardware_bulk_workflow.py` (`BulkRun`) with `sys.path[:0]=['tools','tools/vm_bridge']`. Keep ad-hoc scripts in the scratchpad and evidence under `build/`.

Discover the current tool schemas from `tools/vm_bridge/server.py` (`register(...)` calls) and `tools/vm_bridge/README.md` rather than relying on remembered shapes.

## Find the intended guest

- **Physical machine:** `hw_sessions` lists opted-in attachments from the local registry (`build/hardware-bridge`) without network probing; most of them are historical. For an explicit current IPv4 endpoint, `hw_attach` pins the boot and `hw_doctor` checks identity and capabilities. Reattach explicitly after a reboot or an identity mismatch. Read [hardware.md](references/hardware.md) before diagnostics, transfers or power operations.
- **QEMU:** `vm_sessions` lists attachable runs and `vm_doctor` checks one. Attach to an existing user run without restarting it. `vm_start` creates new disposable experiment clones. Read [vm.md](references/vm.md) for observation, input, profiling, panic inspection or lifecycle.

Keep the session generation and guest boot ID with operation records. Fetch fresh app/window references after a boot change. An old frame, PID, path or screenshot does not establish current target identity.

## Choose the smallest useful operation

- Use `*_apps`/`*_windows` for discovery, `*_stats` for bounded metrics and incremental `*_logs` for diagnostics. Screenshots and observation bundles are optional costs. `*_command` sends a kernel debug command (for example `gpu submission timing status`), not a shell command.
- Read [jobs.md](references/jobs.md) before the first command job or when recovering uncertain job/UI delivery. Use a fresh 32-character lowercase hex request ID per new action. A timeout is an observation outcome: inspect the same operation with `*_job`/`*_ui`/lifecycle status. Never create a new request ID to recover a lost reply.
- Request success, a visible window, process exit, drained descendants and a checked device flush are different facts. Verify the one the task needs.
- The physical debug listener serves one connection at a time. Only one agent should drive a physical machine at once; don't probe concurrently with running work.
- Guest PATH on full images lacks some utilities (`grep`, `cmp`); use `/nix/profile/bin/python3` or bash builtins.

## Source and installation

The bridge server is `tools/vm_bridge/server.py` with `BROOK_ROOT` set to the checkout. The VM registry is `build/vm-bridge` and the physical registry is `build/hardware-bridge`. Machine-specific endpoints, credentials, keys and current boot state belong in `docs/agent/HANDOVER.md` and private config, never in this skill.
