# Brook's Files integration keeps the pinned upstream application intact.
# When the Brook fork carries patches, override nautilus here with a pinned
# source revision and hash. Keep its desktop ID and About metadata upstream.
{ pkgs ? import ./nixpkgs.nix {}, nautilus ? pkgs.nautilus }:
let
  # Local mounts use GIO's Unix monitor. Trash needs GVfs but neither feature
  # needs a host udisks, udev or systemd session.
  gvfs = pkgs.gvfs.override { udevSupport = false; };
in {
  inherit nautilus gvfs;
  integration = pkgs.runCommand "brook-nautilus-integration" {
    meta.license = nautilus.meta.license;
  } ''
    mkdir -p "$out/bin" "$out/etc/xdg" "$out/share/applications" "$out/share/dbus-1/services"
    cat > "$out/bin/brook-nautilus" <<'WRAPPER'
    #!${pkgs.bash}/bin/bash
    export GIO_USE_VFS=gvfs
    export GIO_EXTRA_MODULES="${gvfs}/lib/gio/modules''${GIO_EXTRA_MODULES:+:$GIO_EXTRA_MODULES}"
    export GSETTINGS_BACKEND=keyfile
    exec /nix/profile/bin/brook-run ${nautilus}/bin/nautilus "$@"
    WRAPPER
    chmod +x "$out/bin/brook-nautilus"
    ln -s brook-nautilus "$out/bin/nautilus"
    ln -s "${nautilus}/share/icons" "$out/share/icons"

    desktop="$out/share/applications/org.gnome.Nautilus.desktop"
    cp "${nautilus}/share/applications/org.gnome.Nautilus.desktop" "$desktop"
    chmod u+w "$desktop"
    sed -i -e 's|^Exec=.*nautilus|Exec=/nix/profile/bin/brook-nautilus|' \
           -e 's|^TryExec=.*|TryExec=/nix/profile/bin/brook-nautilus|' \
           -e 's|^DBusActivatable=.*|DBusActivatable=false|' "$desktop"
    grep -qx 'Name=Files' "$desktop"
    grep -qx 'Exec=/nix/profile/bin/brook-nautilus --new-window %U' "$desktop"

    cat > "$out/etc/xdg/mimeapps.list" <<'MIME'
    [Default Applications]
    inode/directory=org.gnome.Nautilus.desktop;
    MIME
    for name in org.gnome.Nautilus org.freedesktop.FileManager1; do
      cat > "$out/share/dbus-1/services/$name.service" <<SERVICE
    [D-BUS Service]
    Name=$name
    Exec=/nix/profile/bin/brook-nautilus --gapplication-service
    SERVICE
    done
  '';
}
