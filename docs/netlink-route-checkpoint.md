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
and unredacted screenshots stay outside git. The Goose software-presentation
Steam retry is the next application gate; successful interface queries do not
certify that game or repair the separate cross-capset graphics import boundary.
