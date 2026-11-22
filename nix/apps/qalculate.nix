{ pkgs, ... }:
{
  name = "Qalculate!";
  packages = [ pkgs.qalculate-gtk ];
  version = pkgs.qalculate-gtk.version;
  executable = "hardware-qalculate";
  wrap = "qalculate-gtk";
  # Replaces the package's own entry so the launcher runs the session wrapper.
  # The hicolor "qalculate" icon comes from the package.
  desktop = {
    id = "qalculate-gtk";
    name = "Qalculate!";
    icon = "qalculate";
    categories = "Utility;Calculator;";
  };
  # Beyond the GTK 3 runtime Mousepad already brings (382 MiB closure alone).
  sizeMiB = 48;
}
