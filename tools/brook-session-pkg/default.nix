# The compatibility session bus is a desktop service, not an app descendant.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.runCommand "brook-session-dbus-1" {
  meta = {
    description = "Brook desktop session D-Bus daemon";
    inherit (pkgs.dbus.meta) license;
    platforms = [ "x86_64-linux" ];
  };
} ''
  mkdir -p $out/bin
  # Keep a stable executable basename for the kernel shell's run --once check.
  cp ${pkgs.dbus}/bin/dbus-daemon $out/bin/brook-dbus
  # Brook does not populate Linux's /etc/dbus-1. Ship the upstream session
  # policy beside the stable executable and select it explicitly at startup.
  cp ${pkgs.dbus}/share/dbus-1/session.conf $out/bin/brook-session.conf
  $out/bin/brook-dbus --version
''
