# Guest command jobs and semantic actions

Use the live tool schema. The examples below describe field choices; generate new IDs and select actual returned references rather than copying historical values.

## Execute and recover a command

`vm_guest_run` and `hw_guest_run` accept `session`, structured `argv`, a `request_id`, optional `cwd`, `env`, `uid`, `wait_s` and `task_limit`. Use `uuid.uuid4().hex` or an equivalent fresh 32-character lowercase hexadecimal ID. `argv[0]` must name a real absolute guest executable. No host shell is exposed by MCP; shell syntax requires an actual guest shell in argv.

The ordinary default is UID 1000 with cwd/home `/data/home` and null stdin. Minimal recovery/probe images may lack that directory, the Nix profile or ordinary shell utilities. Select an existing cwd and installed executable for the actual image. Prior full Dell boots used `/nix/profile/bin/bash`; a different minimal acceptance image used `/boot/BIN/BASH`. Neither path is universal. `grep` and `cmp` were absent from the final recovery-staging guest PATH; hash checks with available utilities were used instead.

Use `uid: 0` for an authorized maintenance operation that actually requires it. A task limit constrains births in the tagged job lineage; it is not a memory budget, time limit or protection from kernel deadlock. Consult current capability limits when choosing it.

The returned `operation_id` is the durable identity. Observe a long-running or uncertain command using `vm_job` or `hw_job` with that ID. Feed `stdout.next_offset` and `stderr.next_offset` back independently as `stdout_offset` and `stderr_offset`. Check read errors, timeout indicators, retention gaps, dropped bytes and EOF. An inline wait ending leaves the guest job running.

Completion requires the outcome relevant to the task. The overall command-job `completed` state accounts for collected leader exit, no live tagged descendants and both output EOFs. A transport-level `isError: false` or command admission is insufficient; inspect the guest exit code/signal and application result.

After unknown dispatch, inspect first. Explicit identical resubmission can recover verified unfinished staging when the protocol permits it; changed arguments or a different boot require a fresh operation. Never invent a fresh ID and replay a potentially completed write merely because observation failed. Preserve useful output before shutdown; host records survive reconnect, but full guest output is bounded and may not survive the boot.

Avoid the legacy VM `command: "run …"` form for tests needing exit status: it reports launch acknowledgement/PID rather than isolated output and completion.

## Launch and manipulate applications

Get an exact current app reference from `*_apps`, then call `*_launch` with a fresh request ID. It exercises launcher admission and dispatch. Wait conditions distinguish admitted, dispatched and window presence; an existing single-instance window is not proof the client became ready or was activated.

For semantic actions, use the exact reference returned by `*_windows`: session generation, guest boot, window ID and incarnation must match. `*_window` supports focus, minimize, maximize, restore and close; restore needs the appropriate explicit `restore_target` from the schema. Use `*_ui` with the original operation ID after uncertain delivery.

Close dispatch, window disappearance and originating client-group exit are distinct. A provider may own many windows; closing one is not permission to kill the provider or unrelated clients. A pending save dialog needs user handling or an authorized explicit choice.

Return bounded result summaries and artifact paths. Keep the operation ID, boot, request purpose, exit/descendant state and any unresolved uncertainty in the handover when work is interrupted.
