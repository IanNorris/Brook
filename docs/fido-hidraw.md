# FIDO authenticators on Brook

Brook binds the alternate-zero, non-boot HID interface of a USB authenticator
when its report descriptor identifies one FIDO application collection on
usage page `0xF1D0`. It requires 64-byte unnumbered input and output reports
and a matching pair of interrupt endpoints. Other interfaces on a composite
key keep their existing drivers and are outside the hidraw interface.

The device appears as a character node at `/dev/hidraw0` through
`/dev/hidraw7`, owned by uid and gid 1000 with mode 0600. The node is present
only while attached. Each open file has its own bounded input queue. Reads
return one report and respect `O_NONBLOCK`. Writes submit one 64-byte report
with a leading zero report ID. Poll and epoll report hangup and error on
detach. Held file descriptors and retained device inodes stay attached to the
old incarnation if a later device reuses the name. Transport errors stop
further I/O on that connection.

The desktop boot-status marker and the Dell image's USB hotplug marker enable
the existing xHCI port watcher. For other images, add `/etc/usb-hotplug` to
enable late attachment. The boot log's `USB_FIDO_DESCRIPTOR` line contains
controller and port, VID and PID, interface number, endpoint addresses, match
result and report descriptor bytes. It records enumeration metadata only.

The guest test uses QEMU's ephemeral software U2F authenticator. Build the
fixture with the pinned development shell and an x86-64 QEMU with U2F
support:

```sh
qemu=$(nix-build --no-out-link -E '
  let pkgs = import ./nix/nixpkgs.nix {};
  in pkgs.qemu.override {
    u2fEmuSupport = true;
    hostCpuTargets = [ "x86_64-softmmu" ];
  }')
nix-shell --run "python3 scripts/test_fido_hidraw_qemu.py \
  --qemu $qemu/bin/qemu-system-x86_64 --output build/fido-hidraw-qa"
```

The guest checks Linux hidraw descriptor ioctls, CTAPHID INIT and PING, poll
readiness, epoll on unplug and node reuse on replug. Chromium device discovery
is a separate phase.
