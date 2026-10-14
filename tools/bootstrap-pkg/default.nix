{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  llvm = pkgs.llvmPackages_18;
  python = pkgs.python3.withPackages (ps: [ ps.freetype-py ps.pillow ]);
  nativeCC = llvm.libcxxStdenv.cc;
  tools = pkgs.buildEnv {
    name = "brook-bootstrap-tools";
    paths = [
      llvm.clang-unwrapped llvm.lld nativeCC python
      pkgs.gitMinimal pkgs.cmake pkgs.ninja pkgs.bash pkgs.coreutils
      pkgs.gnugrep pkgs.gnused pkgs.findutils pkgs.gnutar pkgs.gzip
      pkgs.hack-font pkgs.cacert
    ];
    ignoreCollisions = true;
  };
  shell = pkgs.writeShellScriptBin "brook-bootstrap-shell" ''
    export PATH=${llvm.clang-unwrapped}/bin:${llvm.lld}/bin:${tools}/bin:/nix/profile/bin:/boot/BIN
    export CC=${llvm.clang-unwrapped}/bin/clang
    export CXX=${llvm.clang-unwrapped}/bin/clang++
    export HOST_CC=${nativeCC}/bin/cc
    export HOST_CXX=${nativeCC}/bin/c++
    export HACK_FONT_TTF=${pkgs.hack-font}/share/fonts/truetype/Hack-Regular.ttf
    export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
    export GIT_SSL_CAINFO="$SSL_CERT_FILE"
    export HOME=/data/home
    export TMPDIR=/data/tmp
    exec ${pkgs.bash}/bin/bash "$@"
  '';
in pkgs.buildEnv {
  name = "brook-bootstrap";
  paths = [ tools shell ];
  ignoreCollisions = true;
}
