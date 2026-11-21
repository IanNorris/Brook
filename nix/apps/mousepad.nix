{ pkgs, ... }:
{
  name = "Mousepad";
  packages = [ pkgs.xfce.mousepad ];
  version = pkgs.xfce.mousepad.version;
  executable = "hardware-mousepad";
  wrap = "mousepad";
  desktop = { id = "org.xfce.mousepad"; };
  sizeMiB = 35;
}
