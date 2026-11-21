# Validate a custom selection before building packages. The exact built kernel
# can be checked here when supplied, or by the image tool after realization.
{ lib, registry, apps, target, services, baseLaunchers, baseExecutables
, allowUnfree, publicRelease, kernelCapabilities ? null }:
let
  rejectDuplicates = kind: values:
    if lib.length values == lib.length (lib.unique values) then true
    else throw "Conflicting Brook ${kind}: ${lib.concatStringsSep ", " values}";
  appFor = id:
    if !builtins.hasAttr id registry then throw "Unknown Brook app: ${id}"
    else let app = registry.${id}; in
      if app.availability != "packaged" then throw "Brook app ${id} is ${app.availability}: ${app.reason}"

      else if !builtins.elem target app.targets then throw "Brook app ${id} is unavailable for ${target}"
      else if app.unfree && (!allowUnfree || publicRelease) then
        throw "Brook app ${id} requires a personal image with --allow-unfree"
      else app;
  selected = map appFor apps;
  launcherIds = baseLaunchers ++ map (app: app.desktop.id) selected;
  executables = baseExecutables ++ map (app: app.executable) selected;
  requiredServices = lib.unique (lib.concatMap (app: app.services) selected);
  missingServices = lib.subtractLists services requiredServices;
  requirements = kind: lib.foldl' (acc: app: lib.foldlAttrs
    (caps: name: version:
      if caps ? ${name} && caps.${name} != version then
        throw "Conflicting Brook capability versions for ${name}"
      else caps // { ${name} = version; })
    acc app.${kind}) { } selected;
  requiredCapabilities = requirements "requirements";
  optionalCapabilities = requirements "optionalRequirements";
  mismatches = lib.filterAttrs (name: version: (kernelCapabilities.${name} or null) != version)
    requiredCapabilities;
in
assert !(publicRelease && allowUnfree) || throw "Public Brook images cannot opt into unfree apps";
assert rejectDuplicates "app IDs" apps;
assert rejectDuplicates "launcher IDs" launcherIds;
assert rejectDuplicates "executables" executables;
assert missingServices == [ ] || throw "Missing Brook services: ${lib.concatStringsSep ", " missingServices}";
assert kernelCapabilities == null || mismatches == { } ||
  throw "Kernel lacks Brook capability: ${lib.concatStringsSep ", " (builtins.attrNames mismatches)}";
{
  inherit selected launcherIds requiredServices requiredCapabilities optionalCapabilities;
  redistributable = !allowUnfree && !lib.any (app: app.unfree) selected;
}
