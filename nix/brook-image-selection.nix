{ configJSON }:
let
  composed = import ./brook-profile.nix (builtins.fromJSON configJSON);
in {
  inherit (composed) profile;
  selection = {
    schema = "brook-profile-selection/1";
    profile = toString composed.profile;
    packages = map (package: {
      name = package.pname or package.name;
      version = package.version or "";
      store = builtins.baseNameOf (toString package);
    }) composed.packages;
    intelVulkanDriver = if composed.intelVulkanDriver or null == null
                       then null else toString composed.intelVulkanDriver;
    inherit (composed) manifest;
  };
}
