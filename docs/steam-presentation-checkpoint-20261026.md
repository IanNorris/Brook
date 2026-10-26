# Steam presentation and interface-query checkpoint

This follows the [PI-futex implementation](pi-futex-implementation.md) and
[MAP_32BIT repair](mmap32-checkpoint.md). Neither Graveyard Keeper nor Untitled
Goose Game is marked compatible by these tests. Native Graveyard Keeper now
passes the earlier mutex and Mono failures, renders its title background and
delivers audio. Resizing makes its X11 source image black; restoring the size
does not recover it. This is a remaining OpenGL/application presentation issue,
not a successful Vulkan presentation test.

## Minimal Vulkan presentation control

Build `nix-build tools/vulkan-probe-pkg --no-out-link`. Its
`bin/vulkan-wsi-probe` creates a 320x240 Xlib surface, chooses a graphics queue
with presentation support, acquires swapchain images, clears them with GPU
commands, presents, and checks the centre pixel through XGetImage. The sequence
is red, green, blue, white. Rendering-complete semaphores belong to swapchain
images and are reused after reacquisition. Run under an external timeout in
case a driver blocks; this is a diagnostic, not a benchmark or resize test.

On candidate kernel SHA256
`fec9ef09445a1720524fa1a992e51c4f7f9369c00069d4f3a03a63b822ce2a42`,
the final probe SHA256
`d92d7f4b5e3d529915d8bb01968d13f5a69c2dc9a84acba97b9ae452ac24944a`
produces these controlled results on Venus / Intel UHD 630:

- Default X11 WSI: vkCreateSwapchainKHR returns -3,
  VK_ERROR_INITIALIZATION_FAILED, before any frame submission.
- `MESA_VK_WSI_DEBUG=sw,noshm`: all four exact RGB readbacks pass; exit zero.
  This selects software presentation, while the Vulkan rendering device
  remains Venus. It is not a software-rendering comparison.
- The separate compute/raster/readback probe passes both its ordinary launch
  and an explicit dynamic-loader launch that places native libraries high.
  This does not establish that all address-layout-sensitive code is correct.

An earlier equivalent probe, observed without changing XCB requests, receives
BadAlloc (11) from DRI3 PixmapFromBuffers (major 147, minor 7). A scoped Xwayland
ioctl trace records PRIME_FD_TO_HANDLE (0xc00c642e) returning EOPNOTSUPP (-95).
The trace is disabled after the test. Brook deliberately rejects importing a
Venus/host-blob resource into a different legacy VirGL file/context. The guard
protects an unsupported resource type and lifetime boundary; removing it is
not a repair. Proper cross-capset import needs attachment/type validation,
producer completion synchronization, and shared retirement rules. The
software-presentation control provides a bounded fallback for further tests.

The final probe staging first failed a checksum before executing the binary;
the corrected request ran once and produced the results above. That failed
staging attempt is retained separately from driver results.

## Goose and network-interface diagnostics

With the combined PI/MAP_32BIT kernel and an explicit software-WSI Steam client,
the stock Win64 Goose launch creates its D3D11 Venus device and loads initial
Unity assets, but exits before the earlier controller/Steam initialization
milestone. The launch command's zero status is only a Steam acknowledgement;
the actual game wrapper exits one. There is no active white-window stall in
that run, and the default WSI import failure alone does not explain the exit.

The Proton log contains a handled access violation in nsiproxy.so at offset
0x9c20. Disassembly of the exact installed module places this at an unchecked
dereference immediately after if_nameindex. Its caller has already locked
if_list_lock and has not reached its unlock. This provides a possible stranded
ordinary mutex; it is not evidence of a remaining PI-futex failure or proof
that this fault alone causes Goose's exit.

Native guest controls confirm if_nameindex returns NULL, getifaddrs returns
-1, and AF_NETLINK/NETLINK_ROUTE socket creation fails with EAFNOSUPPORT (97).
Both SOCK_RAW and SOCK_DGRAM fail. The public
[glibc interface enumeration implementation](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/if_index.c)
uses rtnetlink. Current upstream
[Wine interface enumeration](https://github.com/wine-mirror/wine/blob/master/dlls/nsiproxy.sys/ndis.c)
checks a null if_nameindex result; the exact installed Proton module does not.
These source comparisons supplement the observed return and exact-module
disassembly rather than identify the installed binary from an upstream branch.

The owned [Windows network control](../tools/windows-network-probe-pkg/README.md)
builds as PE32+ AMD64. Stock Proton runs reproduce the NSI fault during startup
but do not produce an observable BEGIN or API result within the external
deadline, including a version that delays importing iphlpapi.dll and requests
an explicit output file. Those runs are inconclusive for the proposed
two-call/mutex test, not passing GetIfTable2 results. Their original operation
receipts are recovered; all diagnostic descendants drain before shutdown.

The proposed OS follow-up is a truthful, read-only NETLINK_ROUTE interface and
address snapshot, with matching SIOCGIF* queries. It should have its own
refcounted datagram endpoint: Brook's existing Socket backend is TCP/UDP-specific.
Required controls include multipart framing, request sequences and local port
IDs, kernel sender identity, aligned attributes, short rtgenmsg requests,
MSG_PEEK/TRUNC, nonblocking reads, poll/epoll, descriptor aliasing and close.
Unsupported mutation/subscription operations must report errors. Returning an
empty success list or suppressing the Wine fault does not implement interface
enumeration. No netlink kernel implementation is claimed in this checkpoint.

## Review closure and additional games

Half-Life, Torchlight and original Left 4 Dead are not installed in this review.
The [shortlist preflight](steam-game-preflight-20261026.md) still requires
actual engine/header or available-depot checks; no additional game is claimed
tested merely from a cached launch entry. These checks remain follow-up work.

Private logs, account state, game binaries and saves remain in ignored review
images. No game save was intentionally loaded in these application runs.
The original 33 Stardew files retain their contents and non-cloud-marker
timestamps; the cloud marker timestamp changes. Goose's original save contents
and timestamp and both retained Graveyard save files' contents are unchanged.
Steam's original client operation ends zero with no live tasks. Checked VM
stop confirms filesystem/device flush, its original owner exits zero, and all
four offline filesystem checks return zero.
