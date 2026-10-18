{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.vlc.overrideAttrs (old: {
  pname = "vlc-brook";
  patches = (old.patches or []) ++ [
    ./software-format.patch ./memfd.patch ./oss3.patch
    ./wayland-close.patch ./buffer-release.patch ./wayland-window.patch
    ./oss-write-recovery.patch
  ];
  configureFlags = (old.configureFlags or []) ++ [ "--enable-oss" ];
  nativeBuildInputs = (old.nativeBuildInputs or []) ++ [ pkgs.python3 ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    python3 ${./check-oss-write.py} . --cc "$CC" --patched-source
    runHook postCheck
  '';
})
