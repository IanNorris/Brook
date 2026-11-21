{ ... }:
{
  name = "Calculator";
  # The close fix is applied here, not in the shared source, so the pinned
  # compatibility preset keeps its exact Calculator build.
  packages = [ ((import ../../tools/wayland-calc-pkg {}).overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./calculator-close.patch ];
  })) ];
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
