# NVIDIA GSP-RM firmware set for the RTX 2080 (TU104), pinned to 570.144.
#
# The five files the Brook GSP bring-up loads, taken unmodified from the
# pinned nixpkgs linux-firmware *source* (the WHENCE links tu104/gsp ->
# tu102/gsp and tu104/acr -> tu102/acr are resolved here), with the
# licence and the nouveau WHENCE stanza beside them. Every file is checked
# against the hash Brook's kernel also pins (nv_gsp_chip.h): a nixpkgs bump
# that changes a blob fails the build instead of silently changing the RM
# ABI.
#
# Licence: LICENCE.nvidia (linux-firmware). Redistributable unmodified,
# with the licence, for OSI-licensed operating systems (section 2.1.2);
# Brook is MIT. See notes/rtx2080-3d-design.md section 1.6.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; } }:
let
  src = pkgs.linux-firmware.src;
  # chip directory, the source path inside linux-firmware, and its sha256.
  # Later generations (ga102, ad102) add their own list; nothing else here
  # is chip-specific.
  files = [
    { dst = "nvidia/tu104/gsp/gsp-570.144.bin";           from = "nvidia/tu102/gsp/gsp-570.144.bin";           sha256 = "3052aee2872182a14d8d7c069e3a14fe4642405894b24692c4aca4101dfb1809"; }
    { dst = "nvidia/tu104/gsp/booter_load-570.144.bin";   from = "nvidia/tu102/gsp/booter_load-570.144.bin";   sha256 = "7bb181f544a942299bbf4642da6d9bb58bfed8459725094e2ace56647cd3ec8c"; }
    { dst = "nvidia/tu104/gsp/booter_unload-570.144.bin"; from = "nvidia/tu102/gsp/booter_unload-570.144.bin"; sha256 = "5c7f1aa37d045cc20bb81584a830e8dfa4483f1cd63943de83a58c4be19a1782"; }
    { dst = "nvidia/tu104/gsp/bootloader-570.144.bin";    from = "nvidia/tu102/gsp/bootloader-570.144.bin";    sha256 = "12e987b636c2f00fa40f42fd95097515c0817b158119c584049a37faf38f8f96"; }
    { dst = "nvidia/tu104/acr/bl.bin";                    from = "nvidia/tu102/acr/bl.bin";                    sha256 = "01326cc997716bebb0eab9fe1f463fffa0ee586079e4ca0e5bb096ab6fcfedab"; }
  ];
  copy = f: ''
    install -D -m 0444 "$src/${f.from}" "$out/lib/firmware/${f.dst}"
    echo "${f.sha256}  lib/firmware/${f.dst}" >> "$out/SHA256SUMS"
  '';
in
pkgs.runCommand "nv-gsp-firmware-tu104-570.144" {
  inherit src;
  meta = {
    description = "NVIDIA GSP-RM 570.144 firmware set for TU104 (unmodified, from linux-firmware)";
    license = pkgs.lib.licenses.unfreeRedistributableFirmware;
  };
} ''
  set -eu
  mkdir -p "$out"
  : > "$out/SHA256SUMS"
  ${pkgs.lib.concatMapStrings copy files}
  (cd "$out" && sha256sum -c --strict SHA256SUMS)
  lic="$out/share/licenses/nv-gsp-firmware"
  install -D -m 0444 "$src/LICENCE.nvidia" "$lic/LICENCE.nvidia"
  # The nouveau stanza of WHENCE (origin, version and licence lines).
  awk '/^Driver: nouveau /{on=1} on&&/^-----/{exit} on{print}' "$src/WHENCE" > "$lic/WHENCE.nouveau"
  grep -q 'gsp-570.144.bin' "$lic/WHENCE.nouveau"
  grep -q 'LICENCE.nvidia' "$lic/WHENCE.nouveau"
  chmod 0444 "$lic/WHENCE.nouveau"
''
