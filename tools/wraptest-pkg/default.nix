{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, bash ? (import ../../nix/nixpkgs.nix {}).bash
, coreutils ? (import ../../nix/nixpkgs.nix {}).coreutils
, makeWrapper ? (import ../../nix/nixpkgs.nix {}).makeWrapper
}:
# Minimal makeWrapper repro: a bash wrapper that sets an env var and execs echo.
# Exercises the shell's shebang-unwinding path that broke nix makeWrapper scripts.
stdenv.mkDerivation {
  pname = "wraptest"; version = "0.1";
  dontUnpack = true;
  nativeBuildInputs = [ makeWrapper ];
  installPhase = ''
    mkdir -p $out/bin
    cat > $out/bin/.real <<EOF
#!${bash}/bin/bash
echo "WRAPTEST OK arg=\$1 FOO=\$FOO"
EOF
    chmod +x $out/bin/.real
    makeWrapper $out/bin/.real $out/bin/wraptest --set FOO baz
  '';
}
