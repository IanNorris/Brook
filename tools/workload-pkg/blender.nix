{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  # Backport the dependency alignment from nixpkgs#514285. The SDK pin's
  # Blender/Mesa use LLVM 21 but OSL pulls in LLVM 19; mixing their C++ APIs
  # crashes during software OpenGL context creation (nixpkgs#508671).
  # Preserve the OSL version, ABI and features. Replacing this dependency in
  # the closure avoids a full Blender rebuild for this compatibility test.
  fixedOsl = pkgs.openshadinglanguage.override {
    llvmPackages_19 = pkgs.llvmPackages;
  };
in pkgs.replaceDependency {
  drv = pkgs.blender;
  oldDependency = pkgs.openshadinglanguage;
  newDependency = fixedOsl;
}
