# App-selected hardware session profile. The compatibility preset keeps the
# original package ordering, link priorities, and grouped launcher output.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; }
, preset ? null
, apps ? [ ]
, extraPackages ? [ ]
, extraPackageNames ? [ ]
, target ? "dell-pair"
, allowUnfree ? false
, publicRelease ? false
, kernelCapabilities ? null
  # RTX desktop store: the pinned GSP-RM firmware set (nix/nv-gsp-firmware.nix,
  # unmodified, with LICENCE.nvidia) under /nix/profile/lib/firmware.
, nvGspFirmware ? false
}:
let
  lib = pkgs.lib;
  base = import ./hardware-packages.nix { inherit pkgs; includeOfflineApps = false; };
  # Browsers need the downstream-aware OSS plugin where the kernel enables a
  # default downstream allowance: the Dell HDA runtime and VMware intel_hda.
  catalog = import ./app-registry.nix { inherit pkgs; downstreamStaging = builtins.elem target [ "dell-pair" "vmware" ]; };
  compat = import ./hardware-network-compat.nix { inherit pkgs; };
  compatApps = [ "chromium" "files" "firefox" "gallery" "mousepad"
                 "quake2" "steam" "vscodium" ];
  fonts = import ../tools/brook-fonts-pkg {};
  cursors = import ../tools/brook-cursor-theme {};
  baseLaunchers = [ "brook-settings" "org.brook.TaskManager" ];
  baseExecutables = [ "brook-run" "hardware-dbus" "hardware-message" "hardware-brook-settings"
                      "hardware-brook-task-manager" "brook-stage-profile" ];
  selection = import ./resolve-brook-apps.nix {
    inherit lib apps target allowUnfree publicRelease kernelCapabilities baseLaunchers baseExecutables;
    registry = catalog.apps;
    services = [ "dbus" "message" ];
  };
  inherit (selection) selected launcherIds;
  nvGspFirmwarePackage = import ./nv-gsp-firmware.nix { inherit pkgs; };
  cliPackageSets = [ "python3Packages" "xfce" "kdePackages" ];
  packageFor = name:
    let
      components = lib.splitString "." name;
      first = lib.head components;
      allowed = builtins.match "[A-Za-z0-9_+-]+(\\.[A-Za-z0-9_+-]+)*" name != null
        && (if lib.length components == 1 then
          !(lib.hasPrefix "pkgs" first || lib.hasPrefix "__" first)
        else builtins.elem first cliPackageSets && lib.length components == 2);
      package = if allowed then lib.attrByPath components null pkgs else null;
      host = package.stdenv.hostPlatform.system or (package.system or null);
    in
      if !allowed || !lib.isDerivation package || host != "x86_64-linux" ||
         !(lib.meta.availableOn pkgs.stdenv.hostPlatform package)
      then throw "Unknown or unsupported Brook x86_64 package: ${name}"
      else package;
  cliPackages = map
    (p: if !lib.isDerivation p then throw "Brook extra packages must be Nix derivations"
        else if p.meta.unfree or false then
          throw "Unfree extra packages require an approved Brook app descriptor"
        else p)
    (extraPackages ++ map packageFor extraPackageNames);
  cli = pkgs.buildEnv {
    name = "brook-command-line-extras";
    paths = cliPackages;
    pathsToLink = [ "/bin" "/share/man" "/share/info" "/share/bash-completion"
                    "/share/zsh" "/share/fish" "/share/mime" ];
  };
  # Reuse the pinned font files and aliases. Caches are mutable user data,
  # so a desktop startup must never write them into a signed store path.
  fontConfig = pkgs.runCommand "brook-selected-fontconfig" {} ''
    mkdir -p "$out/etc/fonts"
    sed -e '\|<cachedir>.*</cachedir>|d' \
        -e '\|</dir>|a\  <cachedir prefix="xdg">fontconfig</cachedir>' \
        ${fonts}/etc/fonts/fonts.conf > "$out/etc/fonts/brook-selected.conf"
    test "$(grep -c '<cachedir' "$out/etc/fonts/brook-selected.conf")" = 1
  '';
  launchers = pkgs.runCommand "brook-selected-app-launchers" {} ''
    mkdir -p "$out/bin" "$out/share/applications"
    cat > "$out/bin/brook-run" <<'SCRIPT'
    ${import ./network-session-wrapper.nix { inherit pkgs fonts cursors; selectedProfile = true; }}SCRIPT
    ${lib.concatMapStringsSep "\n" (app:
      lib.optionalString (app ? wrapperSource) ''
        install -m 755 ${app.wrapperSource} "$out/bin/${app.executable}"
      '' + lib.optionalString (app ? wrap) ''
        cat > "$out/bin/${app.executable}" <<'SCRIPT'
        #!${pkgs.bash}/bin/bash
        exec /nix/profile/bin/brook-run /nix/profile/bin/${app.wrap} "$@"
        SCRIPT
      '' + lib.optionalString (app ? iconSource) ''
        install -Dm644 ${app.iconSource} "$out/share/icons/hicolor/scalable/apps/${app.desktop.icon}.svg"
      '' + lib.optionalString (app.desktop ? name) ''
        cat > "$out/share/applications/${app.desktop.id}.desktop" <<'DESKTOP'
        [Desktop Entry]
        Type=Application
        Name=${app.desktop.name}
        Exec=/nix/profile/bin/${app.executable}${lib.optionalString (app.desktop ? arguments) " ${app.desktop.arguments}"}
        Icon=${app.desktop.icon}
        Categories=${app.desktop.categories}
        DESKTOP
      '') selected}
    chmod +x "$out/bin/"*
  '';
  sessionPackages = [ (import ./closure-stage.nix { inherit pkgs; selectedProfiles = true; })
                      (lib.hiPrio launchers) fontConfig pkgs.xz.bin pkgs.zstd.bin ]
    ++ base.packages ++ lib.concatMap (app: app.packages) selected
    ++ lib.optional nvGspFirmware nvGspFirmwarePackage;
  directPackages = lib.unique (sessionPackages ++ cliPackages);
  linkedPackages = lib.unique (sessionPackages ++ [ manifestPackage ] ++ lib.optional (cliPackages != [ ]) cli);
  packageRecord = p: {
    name = p.pname or p.name;
    version = p.version or "";
    store = builtins.baseNameOf (toString p);
  };
  manifest = {
    schema = "brook-profile/1";
    preset = null;
    inherit target launcherIds;
    inherit (selection) requiredCapabilities optionalCapabilities redistributable;
    requiredServices = lib.unique ([ "dbus" "message" ] ++ selection.requiredServices);
    apps = map (app: catalog.metadata.${app.id}) selected;
    extraPackages = map packageRecord cliPackages;
    packages = map packageRecord directPackages;
  };
  manifestPackage = pkgs.writeTextDir "share/brook/profile.json" (builtins.toJSON manifest);
  profile = pkgs.buildEnv {
    name = "brook-selected-desktop";
    paths = linkedPackages;
    # The runner overlay is intentional. Existing base packages export
    # non-executable shared files with overlapping names.
    ignoreCollisions = true;
    postBuild = ''
      ${lib.optionalString (cliPackages != [ ]) ''
        for path in ${cli}/bin/*; do
          [ -f "$path" ] || continue
          if [ ! "$out/bin/${"$"}{path##*/}" -ef "$path" ]; then
            echo "Brook executable collision: ${"$"}{path##*/}" >&2
            exit 1
          fi
        done
      ''}
      desktop="$out/share/applications"
      if [ -L "$desktop" ]; then
        old=$(readlink -f "$desktop")
        rm "$desktop"
        mkdir -p "$desktop"
        for entry in "$old/"*; do
          [ -e "$entry" ] && ln -s "$entry" "$desktop/" || :
        done
      fi
      for entry in "$desktop/"*.desktop; do
        case "${"$"}{entry##*/}" in
          ${lib.concatMapStringsSep "|" (id: "${id}.desktop") launcherIds}) ;;
          *) rm -f -- "$entry";;
        esac
      done
      ${lib.concatMapStringsSep "\n" (id: ''
        test -f "$desktop/${id}.desktop" || { echo "Missing Brook launcher: ${id}" >&2; exit 1; }
      '') launcherIds}
      ${lib.concatMapStringsSep "\n" (app: ''
        test -x "$out/bin/${app.executable}" || { echo "Missing Brook executable: ${app.executable}" >&2; exit 1; }
      '') selected}
    '';
  };
in
assert builtins.elem target [ "dell-pair" "vmware" "qemu" "usb" ] || throw "Unknown Brook profile target: ${target}";
assert lib.length extraPackageNames == lib.length (lib.unique extraPackageNames) ||
  throw "Repeated Brook extra package name";
if preset == "network" then
  assert apps == [ ] && extraPackages == [ ] && extraPackageNames == [ ] && !allowUnfree ||
    throw "The Brook compatibility preset cannot be combined with app or package selections";
  compat // {
    manifest = {
      schema = "brook-profile/1";
      inherit preset target;
      apps = map (id: catalog.metadata.${id}) compatApps;
      redistributable = true;
    };
  }
else if preset != null then throw "Unknown Brook profile preset: ${preset}"
else {
  inherit profile manifest launchers;
  packages = directPackages;
  intelVulkanDriver = null;
}
