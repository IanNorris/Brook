# Persistent Linux workload control. Supply only a disposable SSH PUBLIC key.
# Guest apps live on its own disk, not a shared host Nix store or a RAM root.
{ pkgs ? import ../../nix/nixpkgs.nix {}, sshPublicKey }:
let
  lib = pkgs.lib;
  renderer = import ../venus-renderer-pkg { inherit pkgs; };
  schedulerMix = pkgs.stdenv.mkDerivation {
    pname = "brook-scheduler-mix";
    version = "1";
    dontUnpack = true;
    buildPhase = ''
      $CC -O2 -pthread ${../../scripts/workloads/scheduler_mix.c} -o scheduler-mix
    '';
    installPhase = ''mkdir -p $out/bin; cp scheduler-mix $out/bin/'';
  };
  configuration = { ... }: {
    system.stateVersion = "26.05";
    nixpkgs.pkgs = pkgs;
    boot.loader.grub.enable = false;
    boot.initrd.availableKernelModules = [ "virtio_pci" "virtio_blk" "ext4" ];
    boot.kernelModules = [ "virtio_gpu" "virtio_net" "snd_hda_intel" ];
    boot.kernelParams = [ "console=ttyS0" "console=tty1" "panic=-1" ];
    fileSystems."/" = { device = "/dev/disk/by-label/brook-linux-ref"; fsType = "ext4"; };
    swapDevices = [];
    networking.hostName = "brook-linux-reference";
    networking.useDHCP = false;
    networking.useNetworkd = true;
    systemd.network.networks."10-ethernet" = {
      matchConfig.Type = "ether";
      networkConfig.DHCP = "ipv4";
    };
    # The launcher must bind its only SSH forward to host loopback.
    networking.firewall.enable = true;
    networking.firewall.allowedTCPPorts = [ 22 ];
    services.openssh = {
      enable = true;
      settings = { PasswordAuthentication = false; KbdInteractiveAuthentication = false; PermitRootLogin = "no"; };
    };
    users.mutableUsers = false;
    users.users.brook = {
      isNormalUser = true; uid = 1000; home = "/home/brook";
      extraGroups = [ "wheel" "video" "render" "audio" "input" ];
      openssh.authorizedKeys.keys = [ sshPublicKey ];
    };
    users.users.root.hashedPassword = "!";
    time.timeZone = "UTC";
    hardware.graphics.enable = true;
    services.dbus.enable = true;
    security.polkit.enable = true;
    services.pipewire = { enable = true; alsa.enable = true; pulse.enable = true; };
    services.pipewire.extraConfig.pipewire."10-benchmark-clock" = {
      "context.properties" = {
        "default.clock.rate" = 44100;
        "default.clock.allowed-rates" = [ 44100 ];
      };
    };
    programs.xwayland.enable = true;
    services.greetd = {
      enable = true;
      settings.default_session = {
        command = "${pkgs.weston}/bin/weston --backend=drm --renderer=gl --config=/etc/xdg/weston/weston.ini";
        user = "brook";
      };
    };
    environment.etc."xdg/weston/weston.ini".text = ''
      [core]
      idle-time=0
      xwayland=true
      [shell]
      locking=false
      background-color=0xff152333
      [output]
      name=Virtual-1
      mode=1920x1080
    '';
    environment.sessionVariables = { MOZ_ENABLE_WAYLAND = "1"; GDK_BACKEND = "wayland"; };
    environment.systemPackages = with pkgs; [
      firefox yquake2 supertuxkart gltron weston python3 curl jq procps
      schedulerMix
    ];
    fonts.packages = [ pkgs.noto-fonts ];
  };
  system = import (pkgs.path + "/nixos") { inherit configuration; };
  image = import (pkgs.path + "/nixos/lib/make-disk-image.nix") {
    inherit pkgs lib;
    config = system.config;
    name = "brook-linux-workload-reference-image";
    baseName = "root";
    label = "brook-linux-ref";
    partitionTableType = "none";
    installBootLoader = false;
    diskSize = "auto";
    additionalSpace = "8G";
    memSize = 1024;
    format = "raw";
  };
in assert lib.hasPrefix "ssh-ed25519 " sshPublicKey;
pkgs.runCommand "brook-linux-workload-reference" {} ''
  mkdir -p $out
  ln -s ${image}/root.img $out/root.img
  ln -s ${system.config.system.build.kernel}/bzImage $out/bzImage
  ln -s ${system.config.system.build.initialRamdisk}/initrd $out/initrd
  cat > $out/manifest.json <<'JSON'
  ${builtins.toJSON {
    nixpkgs = toString pkgs.path;
    system = toString system.config.system.build.toplevel;
    init = "${system.config.system.build.toplevel}/init";
    kernel = toString system.config.boot.kernelPackages.kernel;
    kernelVersion = system.config.boot.kernelPackages.kernel.version;
    kernelParams = system.config.boot.kernelParams;
    qemu = toString pkgs.qemu_full;
    mesa = toString pkgs.mesa;
    virglrenderer = toString renderer;
    glvnd = toString pkgs.libglvnd;
    vulkanLoader = toString pkgs.vulkan-loader;
    firefox = { package = toString pkgs.firefox; version = pkgs.firefox.version; };
    yquake2 = { package = toString pkgs.yquake2; version = pkgs.yquake2.version; };
    supertuxkart = toString pkgs.supertuxkart;
    gltron = toString pkgs.gltron;
    schedulerMix = toString schedulerMix;
    schedulerMixSourceSha256 = builtins.hashFile "sha256" ../../scripts/workloads/scheduler_mix.c;
    rootImage = toString image;
    transport = "key-only SSH through launcher-owned loopback forward";
    diskBackedApps = true;
    swap = false;
  }}
  JSON
''
