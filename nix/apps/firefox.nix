{ pkgs, downstreamStaging, ... }:
{
  name = "Firefox";
  packages = [ (import ../../tools/firefox-brook-pkg { inherit pkgs downstreamStaging; }) ];
  version = pkgs.firefox.version;
  executable = "brook-firefox";
  wrapperSource = ../../data/nix-wrappers/brook-firefox;
  desktop = {
    id = "brook-hardware-firefox";
    name = "Firefox";
    arguments = "%U";
    icon = "firefox";
    categories = "Network;WebBrowser;";
  };
  services = [ "dbus" ];
  optionalRequirements = { broadwell-video-engine = 1; };
  sizeMiB = 290;
}
