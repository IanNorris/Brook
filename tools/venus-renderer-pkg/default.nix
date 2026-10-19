{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# Keep the pinned QEMU ABI, but bound the renderer's exported map to the guest
# resource extent before QEMU installs a host-visible MemoryRegion.
pkgs.virglrenderer.overrideAttrs (old: {
  patches = (old.patches or []) ++ [ ./blob-map-boundary.patch ];
  nativeBuildInputs = (old.nativeBuildInputs or []) ++ [ pkgs.python3 ];
  postBuild = (old.postBuild or "") + ''
    python3 ${./check-map-boundary.py} ${./map-boundary-test.c}
  '';
})
