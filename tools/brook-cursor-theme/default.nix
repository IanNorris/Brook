{ stdenvNoCC ? (import <nixpkgs> {}).stdenvNoCC
, adwaita-icon-theme ? (import <nixpkgs> {}).adwaita-icon-theme
}:

# Brook cursor theme:
#   Provides $out/share/icons/default/{index.theme,cursors/...} so that
#   libwayland-cursor's wl_cursor_theme_load("default", ...) finds at least
#   one valid XCursor file and does not return NULL.
#
#   Without this, libtoytoolkit-linked Wayland clients (weston-flower,
#   weston-eventdemo, foot, etc.) crash in pointer_handle_enter at
#   theme->cursor[i]->dereference, because their pointer_handle_enter
#   path unconditionally derefs the loaded theme pointer.
#
#   We just copy Adwaita's cursors verbatim — total ~12 MB, single
#   directory, no symlinks (so it survives FAT/ext2 transparently).

stdenvNoCC.mkDerivation {
  pname = "brook-cursor-theme";
  version = "0.1";

  dontUnpack = true;
  dontBuild = true;
  dontFixup = true;

  installPhase = ''
    mkdir -p $out/share/icons/default/cursors
    cp -r ${adwaita-icon-theme}/share/icons/Adwaita/cursors/. \
          $out/share/icons/default/cursors/

    cat > $out/share/icons/default/index.theme <<'EOF'
[Icon Theme]
Name=default
Comment=Brook default cursor theme (Adwaita cursors repackaged)
Inherits=Adwaita
EOF
  '';
}
