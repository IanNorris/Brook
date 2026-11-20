set -e
bin=/nix/profile/bin
finish() {
    status=$?
    "$bin/sync"
    printf 'PID_NAMESPACE_GUEST_DONE status=%d\n' "$status"
}
trap finish EXIT
"$bin/pid-namespace-guest"
"$bin/bwrap" --unshare-all --ro-bind / / --proc /proc --dev /dev --tmpfs /tmp "$bin/pid-namespace-guest" --inside-bwrap
echo PID_NAMESPACE_BWRAP_ALL_PASS
"$bin/seccomp-guest" --export >/tmp/brook-pid-seccomp.bpf
"$bin/bwrap" --unshare-user --unshare-ipc --unshare-pid --unshare-net --unshare-uts --unshare-cgroup-try --die-with-parent --new-session --ro-bind / / --proc /proc --dev /dev --tmpfs /tmp --seccomp 3 "$bin/seccomp-guest" --sandboxed 3</tmp/brook-pid-seccomp.bpf
echo PID_NAMESPACE_BWRAP_FILTER_PASS
