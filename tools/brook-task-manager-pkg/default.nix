{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-task-manager";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ pkgs.gtk3 ];
  postPatch = ''
    cp ${../../src/shared/brook_task_manager.h} brook_task_manager.h
  '';
  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE task_manager.c $(pkg-config --cflags --libs gtk+-3.0) -o brook-task-manager
    runHook postBuild
  '';
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    $CC -std=c11 -O2 -Wall -Wextra -Werror test_task_rows.c -o test-task-rows
    ./test-task-rows
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 brook-task-manager $out/bin/brook-task-manager
    install -Dm644 org.brook.TaskManager.desktop $out/share/applications/org.brook.TaskManager.desktop
    runHook postInstall
  '';
}
