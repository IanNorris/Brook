{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, llvmPackages ? (import ../../nix/nixpkgs.nix {}).llvmPackages
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:
let libllvm = llvmPackages.libllvm.lib or llvmPackages.libllvm;
in stdenv.mkDerivation {
  pname = "dlopenllvm-brook"; version = "0.1";
  src = ./.;
  nativeBuildInputs = [ patchelf ];
  buildPhase = ''
    $CC -O2 -Wall -DLLVM_SO='"${libllvm}/lib/libLLVM.so.21.1"' \
        dlopenllvm.c -ldl \
        -Wl,-rpath,${libllvm}/lib:${stdenv.cc.libc}/lib \
        -o dlopenllvm
  '';
  installPhase = ''
    mkdir -p $out/bin
    install -m755 dlopenllvm $out/bin/dlopenllvm
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" $out/bin/dlopenllvm
  '';
}
