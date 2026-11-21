{ pkgs, ... }:
let gallery = import ../../tools/brook-gallery-pkg { inherit pkgs; };
in {
  name = "App Gallery";
  packages = [ gallery ];
  version = gallery.version;
  executable = "hardware-brook-gallery";
  wrap = "brook-gallery";
  desktop = { id = "brook-gallery"; };
  services = [ "dbus" "message" ];
  sizeMiB = 20;
}
