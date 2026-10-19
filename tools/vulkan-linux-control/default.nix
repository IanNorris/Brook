{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  kernel = pkgs.linuxPackages.kernel;
  probe = import ../vulkan-probe-pkg { inherit pkgs; };
  emptyFirmware = pkgs.runCommand "brook-control-empty-firmware" {} ''
    mkdir -p $out/lib/firmware
  '';
  modules = pkgs.makeModulesClosure {
    kernel = kernel.modules;
    firmware = emptyFirmware;
    rootModules = [ "virtio_pci" "virtio_gpu" ];
  };
  init = pkgs.writeScript "brook-vulkan-control-init" ''
    #!${pkgs.busybox}/bin/sh
    export PATH=${pkgs.busybox}/bin:${pkgs.kmod}/bin
    mkdir -p /proc /sys /dev /run /tmp /root /etc
    mount -t proc proc /proc
    mount -t sysfs sysfs /sys
    mount -t devtmpfs devtmpfs /dev
    mkdir -p /dev/shm /run/control
    mount -t tmpfs tmpfs /dev/shm
    # Preserve dmesg after testing rather than interleaving printk with JSON.
    dmesg -n 1
    chmod 700 /run/control
    export HOME=/root
    export XDG_RUNTIME_DIR=/run/control
    export VK_DRIVER_FILES=${pkgs.mesa}/share/vulkan/icd.d/virtio_icd.x86_64.json
    export MESA_SHADER_CACHE_DISABLE=true
    unset DISPLAY WAYLAND_DISPLAY
    echo BROOK_VULKAN_CONTROL_START
    uname -a
    echo 'NAME="Brook Linux reference"' > /etc/os-release
    ${pkgs.kmod}/bin/modprobe -d ${modules} -S ${kernel.modDirVersion} virtio_pci
    pci_exit=$?
    ${pkgs.kmod}/bin/modprobe -d ${modules} -S ${kernel.modDirVersion} virtio_gpu
    gpu_exit=$?
    echo "BROOK_VULKAN_CONTROL_MODULES pci=$pci_exit gpu=$gpu_exit"
    ls -l /dev/dri
    ${probe}/bin/virtgpu-query
    drm_exit=$?
    echo "BROOK_VULKAN_CONTROL_DRM_EXIT $drm_exit"
    ${pkgs.vulkan-tools}/bin/vulkaninfo --summary
    info_exit=$?
    echo "BROOK_VULKAN_CONTROL_INFO_EXIT $info_exit"
    ${probe}/bin/vulkan-probe --expect-driver venus
    probe_exit=$?
    echo "BROOK_VULKAN_CONTROL_PROBE_EXIT $probe_exit"
    sync
    echo "BROOK_VULKAN_CONTROL_END pci=$pci_exit gpu=$gpu_exit drm=$drm_exit info=$info_exit probe=$probe_exit"
    echo BROOK_VULKAN_CONTROL_DMESG_START
    dmesg
    echo BROOK_VULKAN_CONTROL_DMESG_END
    exec poweroff -f
  '';
  initrd = pkgs.makeInitrd {
    name = "brook-vulkan-linux-control-initrd";
    contents = [ { object = init; symlink = "/init"; } ];
    compressor = "zstd";
    compressorArgs = [ "-3" "-T2" ];
  };
in pkgs.runCommand "brook-vulkan-linux-control" {} ''
  mkdir -p $out
  ln -s ${kernel}/bzImage $out/bzImage
  ln -s ${initrd}/initrd $out/initrd
  ln -s ${probe} $out/probe
  cat > $out/manifest.json <<'JSON'
  ${builtins.toJSON {
    kernel = toString kernel;
    kernelVersion = kernel.version;
    modules = toString modules;
    initrd = toString initrd;
    probe = toString probe;
    mesa = toString pkgs.mesa;
    qemu = toString pkgs.qemu_full;
    glvnd = toString pkgs.libglvnd;
    virglrenderer = toString pkgs.virglrenderer;
    vulkanTools = toString pkgs.vulkan-tools;
    vulkanLoader = toString pkgs.vulkan-loader;
    guestICD = "${pkgs.mesa}/share/vulkan/icd.d/virtio_icd.x86_64.json";
  }}
  JSON
''
