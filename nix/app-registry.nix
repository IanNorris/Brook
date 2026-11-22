# Stable selection IDs. Availability and metadata can be evaluated without
# realizing app packages. Legacy rows describe the existing profile only.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; }
, downstreamStaging ? false
}:
let
  lib = pkgs.lib;
  defaults = {
    availability = "packaged";
    reason = null;
    targets = [ "dell-pair" "vmware" "qemu" "usb" ];
    packages = [ ];
    version = null;
    executable = null;
    desktop = null;
    services = [ ];
    requirements = { };
    optionalRequirements = { };
    unfree = false;
    defaultPin = false;
    autostart = false;
    sizeMiB = null;
  };
  legacy = name: reason: { inherit name reason; availability = "legacy"; targets = [ ]; };
  entries = {
    chromium = import ./apps/chromium.nix { inherit pkgs downstreamStaging; };
    files = import ./apps/files.nix { inherit pkgs; };
    firefox = import ./apps/firefox.nix { inherit pkgs downstreamStaging; };
    gallery = import ./apps/gallery.nix { inherit pkgs; };
    mousepad = import ./apps/mousepad.nix { inherit pkgs; };
    qalculate = import ./apps/qalculate.nix { inherit pkgs; };
    quake2 = legacy "Quake II" "Launcher only; the engine and OpenAL come from a separately staged closure.";
    steam = legacy "Steam" "Launcher uses the existing installation in the user's home.";
    vscodium = legacy "VSCodium" "Launcher only; the application comes from a separately staged closure.";
  };
  apps = lib.mapAttrs (id: entry: defaults // entry // { inherit id; }) entries;
  metadata = lib.mapAttrs (_: app: {
    inherit (app) id name availability reason targets version executable desktop
      services requirements optionalRequirements unfree defaultPin autostart sizeMiB;
  }) apps;
in {
  inherit apps metadata;
}
