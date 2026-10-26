# Windows interface-query control

Build with `nix-build tools/windows-network-probe-pkg --no-out-link`. This
produces an owned AMD64 console program for testing Wine/Proton's networking
interface queries without a game or Steam SDK.

Run `windows-network-probe.exe --if-table` for two successive GetIfTable2 calls,
or use `--adapters` in a separate process for GetAdaptersAddresses. An optional
third argument names a Windows log path; use it when Proton does not forward
console output. The log contains stages, return codes and counts, not interface
names, addresses or account data.

The program loads iphlpapi.dll explicitly after starting a 20-second watchdog,
so the log can distinguish DLL initialization from the API calls. A stalled
DLL detach can still prevent ExitProcess from finishing: retain an external
deadline and recover the original process before launching another copy. A
missing BEGIN marker is not a failed GetIfTable2 result.

Expected success requires a nonempty interface table on both calls, or a
successful adapter size query and fill. Error returns and watchdog termination
are diagnostic failures, not successful compatibility. This probe does not
mutate adapters, contact remote services or open game saves.
