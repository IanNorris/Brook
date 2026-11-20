# Defaults shared by Brook desktop profiles.
{ pkgs ? import ./nixpkgs.nix {}, nautilus ? pkgs.nautilus }:
let
  filesSchemas = "${nautilus}/share/gsettings-schemas/${nautilus.name}/glib-2.0/schemas";
  buttons = "minimize,maximize,close";
  gtkLayout = "icon:${buttons}";
  # GDK translates the GNOME menu token to the GTK icon token.
  gnomeLayout = "menu:${buttons}";
  gnome = pkgs.gsettings-desktop-schemas;
  schemaDir = pkg: "${pkg}/share/gsettings-schemas/${pkg.name}/glib-2.0/schemas";
in
pkgs.runCommand "brook-desktop-settings" {
  nativeBuildInputs = [ pkgs.glib.dev pkgs.glib.bin ];
  meta.license = gnome.meta.license;
} ''
  for version in 3.0 4.0; do
    settings="$out/etc/xdg/gtk-$version"
    mkdir -p "$settings"
    printf '%s\n' '[Settings]' 'gtk-decoration-layout=${gtkLayout}' >"$settings/settings.ini"
  done

  # Brook's runtime uses this directory for GSettings. Bundle the schemas
  # for both GTK generations with the GNOME defaults used by GDK Wayland.
  schemas="$out/share/brook/gsettings"
  mkdir -p "$schemas" schema-source
  cp ${schemaDir gnome}/*.xml \
    ${schemaDir pkgs.gtk3}/*.xml \
    ${schemaDir pkgs.gtk4}/*.xml schema-source/
  cp ${filesSchemas}/*.gschema.xml schema-source/
  cat >schema-source/99-brook.gschema.override <<'SETTINGS'
  [org.gnome.desktop.wm.preferences]
  button-layout='${gnomeLayout}'
  SETTINGS
  glib-compile-schemas --strict --targetdir="$schemas" schema-source
  schema=org.gnome.desktop.wm.preferences
  export GSETTINGS_SCHEMA_DIR="$schemas" GSETTINGS_BACKEND=memory
  export XDG_DATA_DIRS="$PWD/empty-data"
  gsettings list-schemas > installed-schemas
  for required in org.gtk.Settings.ColorChooser org.gtk.Settings.FileChooser \
                  org.gtk.gtk4.Settings.ColorChooser org.gtk.gtk4.Settings.FileChooser; do
    grep -Fx "$required" installed-schemas
  done
  test "$(gsettings get "$schema" button-layout)" = "'${gnomeLayout}'"

  gsettings list-schemas | grep -qx org.gnome.nautilus.preferences

  export XDG_CONFIG_HOME="$PWD/user-config" GSETTINGS_BACKEND=keyfile
  keyfile="$XDG_CONFIG_HOME/glib-2.0/settings/keyfile"
  mkdir -p "$(dirname "$keyfile")"
  printf '%s\n' "[org/gnome/desktop/wm/preferences]" "button-layout='close:'" >"$keyfile"
  cp "$keyfile" before
  test "$(gsettings get "$schema" button-layout)" = "'close:'"
  cmp before "$keyfile"
''
