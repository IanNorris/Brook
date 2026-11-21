{ pkgs, downstreamStaging, ... }:
{
  name = "Chromium";
  packages = [ (import ../../tools/chromium-brook-pkg { inherit pkgs downstreamStaging; }) ];
  version = pkgs.chromium.version;
  executable = "brook-chromium";
  wrapperSource = ../../data/nix-wrappers/brook-chromium;
  desktop = {
    id = "brook-hardware-chromium";
    name = "Chromium";
    arguments = "%U";
    icon = "chromium";
    categories = "Network;WebBrowser;";
  };
  services = [ "dbus" ];
  requirements = { chromium-sandbox = 1; };
  optionalRequirements = { broadwell-video-engine = 1; };
  sizeMiB = 430;
}
