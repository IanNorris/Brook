{ pkgs, ... }:
let
  steamPkgs = import ../nixpkgs.nix {
    config.allowUnfreePredicate = package: pkgs.lib.getName package == "brook-steam-client";
  };
  steam = import ../../tools/brook-steam-pkg { inherit pkgs steamPkgs; };
in {
  name = "Steam";
  inherit (steam) packages version;
  executable = "brook-steam";
  desktop = {
    id = "brook-hardware-steam";
    name = "Steam";
    icon = "applications-games";
    categories = "Game;";
  };
  services = [ "dbus" ];
  unfree = true;
  sizeMiB = 1800;
}
