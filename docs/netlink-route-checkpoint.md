# Read-only interface queries (2026-10-26)

This follows the [presentation/NSI diagnosis](steam-presentation-checkpoint-20261026.md).
It implements actual interface enumeration, rather than replacing Wine's null
result with an empty successful list. Game acceptance remains separate.

## Endpoint and interface view

AF_NETLINK supports NETLINK_ROUTE RAW/DGRAM endpoints with CLOEXEC/NONBLOCK.
The appended NetlinkSocket descriptor type has its own refcounted receive
queue; it does not enter INET demultiplexing, timers or NIC polling. Descriptors
share their port, queue and nonblocking state through dup, fork and SCM_RIGHTS.
Socket identities use the existing shared inode allocator. fstat, proc-fd
readlink and following the proc-fd metadata path retain that same identity and
access policy. Unsupported listen/accept/shutdown report unsupported operations.

Binding and autobinding allocate unique live local port IDs. Only kernel
destination/group zero is supported. Multicast membership and change
notifications fail explicitly; there is no successful subscription that never
delivers events. GETLINK and GETADDR dumps accept glibc's short rtgenmsg request
and return aligned multipart messages with DONE. The response header carries
the request sequence and local socket port; recvmsg's sender is kernel port
zero. Unsupported request types return a capped NLMSG_ERROR. Malformed framing
is rejected before any response is published. Route dumps, mutation and IPv6
configuration are not implemented by this checkpoint.

A shared snapshot reports the existing working loopback as lo/index 1 and
registered NICs as ethN/index N+2, matching existing physical-interface names.
NIC registration and DHCP/static configuration publication serialize with
snapshot reads. MAC, configured IPv4/netmask and Ethernet MTU come from the
backend. NIC carrier state and counters are not invented: no IFF_RUNNING,
IFF_LOWER_UP, operstate or statistics attributes are asserted for physical
interfaces without backend support. Loopback reports its software operational
state. SIOCGIFNAME/INDEX/HWADDR/FLAGS/MTU/ADDR/NETMASK/BRDADDR use the same view.
These are generic socket ioctls, including AF_UNIX: glibc opens that domain for
name/index queries.

Requests and replies are staged before queue publication; no user copy runs
under the queue spinlock. One complete datagram is enqueued atomically or
admission fails. Queue limits charge the actual fixed packet allocation,
including header/unused storage, rather than just its payload. Reducing the
limit below existing usage prevents additional admission without unsigned
underflow or discarding already queued responses. Receive supports PEEK,
TRUNC, vectored messages and nonblocking EAGAIN. Wait registrations retain the
endpoint and task, recheck readiness before sleeping, and remove their exact
registration before releasing references. Poll/epoll use the same queue state.

## Controls

The wire test parses production output with Linux's UAPI macros and checks
short requests, link/address attributes, multipart termination, error replies,
capacity failure, and 20,000 randomized input frames with buffer guards. The
endpoint test compiles the actual production implementation with host scheduler
and user-copy fixtures. It checks allocation accounting, admission failure,
shrinking below current usage, peek/truncation, held aliases, task references,
notification-before-block, blocking arrival and signal cancellation. User-copy
fixtures assert that the endpoint lock is not held. Socket metadata permission
tests cover the new descriptor type too. All 114 host tests pass. Exact compiled
entry and scheduler-start mutation audits pass on the tested candidate.

Candidate kernel SHA256:
`70db0b9dbc1828a6088de0b82902d09935a6433670a7a2355444533a22cdbeb6`.
Module ABI remains 56; the enum is appended without changing descriptor layout.

The public [native glibc control](../tools/at-syscall-probe/netlinkcheck.c), SHA256
`f607e4bc6a2cd1a89249601069a79cd5ee640a7138cbb4543bc83059ef774e2b`,
passes unchanged on Linux and Brook. Brook reports two interfaces and two IPv4
addresses. The real syscall path verifies enumeration, interface ioctls,
socket identity, dump framing, poll/epoll, dup/fork and SCM_RIGHTS. A separate
native Python control follows /proc/self/fd/N and confirms the same socket
inode/mode. The previous PI ownership/contention/robust/signal probes pass again.

The initial guest candidate implemented interface ioctls only on INET sockets;
its control stopped at glibc's AF_UNIX SIOCGIFINDEX query. That failure is
retained, followed by checked stop and four zero-status filesystem checks. The
corrected candidate is tested in fresh private copies. One earlier build used
the wrong target/environment and failed before compilation; subsequent builds
use the project's Nix shell and wrapped host compiler.

## Stock Proton result

The unchanged owned Win64 control, SHA256
`5873c8eb4aa76cb1944544acf590a6362f9d5e117bf5e91b5724bad45ca22ba4`,
now reaches main, loads iphlpapi.dll, and completes both consecutive GetIfTable2
calls with result zero/count two. Its actual child exits zero. The NSI
access-violation offset seen before the fix does not occur in this run's log.

GetAdaptersAddresses is tested separately: DLL initialization succeeds, but its
size query returns ERROR_NOT_SUPPORTED (50), zero bytes; the actual child exits
one. The helper's own successful observation exit is not an application pass.
Both logs still report unsupported NSI change-notification binding. Neither
adapter-address enumeration nor notification support is claimed complete.
The remaining adapter error needs a scoped query trace; route support is a
possible dependency, not an established cause from this result alone.

Private evidence lives under build/workloads/pi-futex-20261026 and the
steam-games-netlink[-v2]-20261026 reviews. Licensed assets, saves, account state
and unredacted screenshots stay outside git. Successful interface queries do
not certify a game or repair the separate cross-capset graphics import boundary.

## Subsequent Steam retry and bounded discovery control

The native Steam client is started with software WSI, then Goose is launched
once through its normal installed Steam chain. The real Steam-tracked root
exits one; the launch CLI exits zero only as an acknowledgement. Unity reaches
its initial 3,373-asset stage but no usable game window is accepted. The current
Proton log has no old NSI access violation. It does contain an assertion that
IClientFriends::GetPersonaName failed with internal IPC code 2. That code has
not been decoded, and is not treated as errno, EResult, or lsteamclient's
Unix-call return status. The assertion's emitting process and its causal
position in startup versus teardown still need a controlled API probe.

An additional owned GetAdaptersAddresses run enables only iphlpapi/NSI tracing
while Steam is present. Its explicit file shows main, DLL load and the same
adapter error 50. The outer child does not drain within 55 seconds; its original
operation is recovered with no remaining descendants. That is not an API pass
or evidence that main failed to execute. Its contrast with the earlier run
also changes Steam presence and tracing, so no single-factor causal conclusion
is drawn from the exit timing.

[Proton's loader](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/unixlib.cpp)
uses HOME/.steam/sdk64/steamclient.so. In this review that resolves through a
directory symlink to client/linux64. The library beside the active Steam
executable in client/steamrt64 has different bytes. Hash inequality alone does
not establish an incompatible version: the libraries can target different
runtime environments. Proc maps expose no identifying steamclient pathname in
this run, so the active client's precise mapped DSO identity is not established
by that observation.

One private A/B changes only the sdk64 directory symlink, preserving its exact
original target and both real libraries. Selecting client/steamrt64 makes
Proton fail earlier to load the native client and assert in lsteamclient. It
does not fix Goose. The original directory symlink is restored and the original
library hash rechecked; no SDK binary or repository launcher is changed.
After saving that failure, stock wineserver -k stops only the agent-owned
Goose/diagnostic prefix. All Wine tasks drain.

The next discriminator is an owned Win64 Steam API control in the actual game
launch context, with phase/timing markers and boolean-only persona results,
compared with the corresponding native Linux interface sequence in the same
container. The
[generated Proton wrapper](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/cppISteamFriends_SteamFriends017.cpp)
does not by itself identify the internal Steam IPC error. Game saves and
licensed SDK/game binaries must stay private throughout that test.

The subsequent [API and present-wait checkpoint](steam-api-and-present-wait-checkpoint.md)
records those actual controls and the native worker dependency they exposed.

Review closure: the original 33 Stardew files retain contents and non-cloud-
marker timestamps; Goose's original save retains contents and timestamp; both
original Graveyard save files retain contents. Steam's original operation exits
zero with no live tasks. Checked VM stop confirms filesystem/device flush,
the original owner exits zero, and all four offline filesystem checks return
zero. No gameplay, audio or save/load acceptance is claimed for Goose here.
