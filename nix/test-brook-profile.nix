# nix-instantiate --eval --strict --expr 'import ./nix/test-brook-profile.nix {}'
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; } }:
let
  lib = pkgs.lib;
  compat = (import ./hardware-network-packages.nix { inherit pkgs; }).profile;
  fonts = import ../tools/brook-fonts-pkg { };
  cursors = import ../tools/brook-cursor-theme { };
  wrapper = selectedProfile: import ./network-session-wrapper.nix {
    inherit pkgs fonts cursors selectedProfile;
  };
  normalizedSelectedWrapper = lib.replaceStrings
    [ "/nix/profile/etc/fonts/brook-selected.conf" "/nix/profile/etc/fonts" ]
    [ "${fonts}/etc/fonts/fonts.conf" "${fonts}/etc/fonts" ] (wrapper true);
  app = {
    availability = "packaged";
    unfree = false;
    targets = [ "qemu" ];
    executable = "demo";
    desktop.id = "example.Demo";
    services = [ "dbus" ];
    requirements = { wayland-demo = 1; };
    optionalRequirements = { acceleration = 1; };
  };
  resolve = overrides: import ./resolve-brook-apps.nix ({
    inherit lib;
    registry = { demo = app; };
    apps = [ "demo" ];
    services = [ "dbus" ];
    target = "qemu";
    baseLaunchers = [ "example.Settings" ];
    baseExecutables = [ "settings" ];
    allowUnfree = false;
    publicRelease = false;
  } // overrides);
  rejects = expression: !(builtins.tryEval (builtins.deepSeq expression true)).success;
  profile = overrides: import ./brook-profile.nix ({ inherit pkgs; target = "qemu"; } // overrides);
in
# A compatibility preset update needs a deliberate change to both pins.
assert compat.drvPath == "/nix/store/31dp7ggq7pf1aq6kdrgxy6aw40y1vx5b-brook-network-hardware-desktop.drv";
assert compat.outPath == "/nix/store/6mz4hyc655d468wdlcs4k85s4q0ic6bw-brook-network-hardware-desktop";
assert normalizedSelectedWrapper == wrapper false;
assert (resolve {}).launcherIds == [ "example.Settings" "example.Demo" ];
assert (resolve { kernelCapabilities.wayland-demo = 1; }).requiredCapabilities.wayland-demo == 1;
assert (resolve {}).optionalCapabilities.acceleration == 1;
assert rejects (resolve { apps = [ "missing" ]; });
assert rejects (resolve { apps = [ "demo" "demo" ]; });
assert rejects (resolve { baseLaunchers = [ "example.Demo" ]; });
assert rejects (resolve { baseExecutables = [ "demo" ]; });
assert rejects (resolve { services = [ ]; });
assert rejects (resolve { target = "usb"; });
assert rejects (resolve { kernelCapabilities = { }; });
assert rejects (resolve { kernelCapabilities.wayland-demo = 2; });
assert rejects (resolve { registry.demo = app // { unfree = true; }; });
assert rejects (resolve {
  registry.demo = app // { unfree = true; };
  allowUnfree = true;
  publicRelease = true;
});
assert !(resolve {
  registry.demo = app // { unfree = true; };
  allowUnfree = true;
}).redistributable;
assert !(resolve { allowUnfree = true; }).redistributable;
assert rejects (resolve { allowUnfree = true; publicRelease = true; });
assert rejects ((profile { apps = [ "steam" ]; }).manifest);
assert rejects ((profile { extraPackageNames = [ "no-such-brook-test-package" ]; }).manifest);
assert rejects ((profile { extraPackageNames = [ "lib" ]; }).manifest);
assert rejects ((profile { extraPackageNames = [ "pkgsCross.aarch64-multiplatform.hello" ]; }).manifest);
assert rejects ((profile { extraPackageNames = [ "pkgsi686Linux.hello" ]; }).manifest);
assert rejects ((profile { extraPackageNames = [ "pkgsStatic.hello" ]; }).manifest);
assert (profile { extraPackageNames = [ "python3Packages.pip" ]; }).manifest.extraPackages != [ ];
assert rejects ((profile { extraPackageNames = [ "htop" "htop" ]; }).manifest);
assert rejects ((profile { apps = [ "chromium" ]; kernelCapabilities = { }; }).manifest);
assert (profile { apps = [ "chromium" ]; kernelCapabilities.chromium-sandbox = 1; })
  .manifest.requiredCapabilities.chromium-sandbox == 1;
assert (profile { apps = [ "calculator" ]; extraPackageNames = [ "htop" ]; })
  .manifest.launcherIds == [ "brook-settings" "org.brook.TaskManager" "brook.calc" ];
true
