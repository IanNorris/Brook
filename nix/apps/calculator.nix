{ ... }:
{
  name = "Calculator";
  packages = [ (import ../../tools/wayland-calc-pkg {}) ];
  version = "0.1-brook";
  executable = "hardware-wayland-calc";
  wrap = "wayland-calc";
  iconSource = ../../data/icons/calculator.svg;
  desktop = {
    id = "brook.calc";
    name = "Calculator";
    icon = "brook-calculator";
    categories = "Utility;Calculator;";
  };
  sizeMiB = 1;
}
