{ pkgs ? import ../../nix/nixpkgs.nix {} }:

# Steam's distributed helper is i386. Build the public upstream implementation
# for Brook's supported native64 ABI; no proprietary Steam/game data is bundled.
pkgs.pkgsCross.musl64.stdenv.mkDerivation {
  pname = "brook-reaper-native64";
  version = "0-fb26ffe";
  src = ./.;
  strictDeps = true;
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    $CC -x c++ -static -O2 -Wall -Wextra -fno-exceptions -fno-rtti reaper.cpp -o reaper
    runHook postBuild
  '';
  # The target is a static x86-64 Linux-ABI executable, runnable on this host.
  doCheck = true;
  doCrossCheck = true;
  checkPhase = ''
    runHook preCheck
    $CC -static -O2 -Wall -Wextra -Werror ${../at-syscall-probe/reapercheck.c} -o reapercheck
    ./reapercheck ./reaper
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 reaper "$out/bin/reaper"
    install -Dm644 LICENSE "$out/share/licenses/reaper/LICENSE"
    runHook postInstall
  '';
  meta = {
    description = "Native64 build of the public Steam process-tree reaper";
    homepage = "https://github.com/Plagman/reaper";
    license = pkgs.lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
