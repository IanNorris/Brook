#!/boot/bin/bash
# One-time maintenance migration using the public key from the signed boot set.
set -eu
export PATH=/nix/profile/bin
boot_key=/etc/usb-profile-plan.pub
house=/nix/etc/brook
key=$house/stage-plan.pub
[[ -s $boot_key && -d /data/.brook-transfer && ! -L /data/.brook-transfer ]]
[[ -d /nix/etc && ! -L /nix/etc ]]
mkdir -p -- "$house"
[[ ! -L $house ]]
if [[ ! -e $key && ! -L $key ]]; then
    scratch=$(mktemp "$house/.usb-plan-key.XXXXXXXX")
    trap 'rm -f -- "$scratch"' EXIT
    install -m 0644 -- "$boot_key" "$scratch"
    ln -T -- "$scratch" "$key"
    sync
fi
[[ -f $key && ! -L $key && $(<"$key") == $(<"$boot_key") ]]
/nix/bin/brook-stage-profile initialize
