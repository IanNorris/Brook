# Networked Dell desktop candidate. Keep the small accepted probe reproducible.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; } }:
let
  base = import ./hardware-packages.nix { inherit pkgs; };
  fonts = import ../tools/brook-fonts-pkg {};
  cursors = import ../tools/brook-cursor-theme {};
  graphics = import ./runtime-graphics.nix { inherit pkgs; };
  # The Dell kernel enables a 4096-frame downstream allowance with its HDA
  # runtime; the downstream-aware OSS plugin is the matching client half.
  firefox = import ../tools/firefox-brook-pkg { inherit pkgs; downstreamStaging = true; };
  chromium = import ../tools/chromium-brook-pkg { inherit pkgs; downstreamStaging = true; };
  gallery = import ../tools/brook-gallery-pkg { inherit pkgs; };
  intelVulkan = import ../tools/mesa-hasvk-brook-pkg/launcher.nix { inherit pkgs; };
  intelVulkanDriver = import ../tools/mesa-hasvk-brook-pkg/default.nix { inherit pkgs; };
  intelGl = import ../tools/mesa-iris-brook-pkg/runtime.nix { inherit pkgs; };
  desktopCompositor = import ../tools/vulkan-compositor-pkg/default.nix { inherit pkgs; };
  launchers = pkgs.runCommand "brook-hardware-network-launchers" {} ''
    mkdir -p $out/bin $out/share/applications
    cp ${../data/nix-wrappers/brook-firefox} $out/bin/brook-firefox
    cp ${../data/nix-wrappers/brook-chromium} $out/bin/brook-chromium
    cat > $out/bin/brook-run <<'SCRIPT'
    ${import ./network-session-wrapper.nix { inherit pkgs fonts cursors; }}SCRIPT
    cat > $out/bin/hardware-brook-gallery <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    exec /nix/profile/bin/brook-run /nix/profile/bin/brook-gallery "$@"
    SCRIPT
    cat > $out/share/applications/brook-hardware-firefox.desktop <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=Firefox
    Exec=/nix/profile/bin/brook-firefox %U
    Icon=firefox
    Categories=Network;WebBrowser;
    DESKTOP
    cat > $out/share/applications/brook-hardware-chromium.desktop <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=Chromium
    Exec=/nix/profile/bin/brook-chromium --brook-launch-log %U
    Icon=chromium
    Categories=Network;WebBrowser;
    DESKTOP
    cat > $out/share/applications/brook-hardware-gallery.desktop <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=App Gallery
    Exec=/nix/profile/bin/hardware-brook-gallery
    Icon=brook-gallery
    Categories=System;PackageManager;
    DESKTOP
    chmod +x $out/bin/*
  '';
  appLaunchers = import ./dell-app-launchers.nix { inherit pkgs; };
  packages = [ (import ./closure-stage.nix { inherit pkgs; }) (pkgs.lib.hiPrio launchers) appLaunchers intelVulkan desktopCompositor firefox chromium gallery graphics.mesaRuntime pkgs.xz.bin pkgs.zstd.bin ] ++ base.packages ++ [ (pkgs.lib.lowPrio intelGl) ];
in {
  inherit packages firefox chromium gallery intelVulkanDriver launchers intelGl;
  profile = pkgs.buildEnv { name = "brook-network-hardware-desktop"; paths = packages; ignoreCollisions = true; };
}
