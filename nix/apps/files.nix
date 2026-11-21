{ pkgs, ... }:
let files = import ../desktop-files.nix { inherit pkgs; };
in {
  name = "Files";
  packages = [ files.integration files.gvfs ];
  version = files.nautilus.version;
  executable = "brook-nautilus";
  desktop = { id = "org.gnome.Nautilus"; };
  services = [ "dbus" ];
  defaultPin = true;
  sizeMiB = 180;
}
