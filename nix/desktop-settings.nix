# Defaults shared by Brook desktop profiles.
{ pkgs ? import ./nixpkgs.nix {} }:
let
  buttons = "minimize,maximize,close";
  gtkLayout = "icon:${buttons}";
  # GDK translates the GNOME menu token to the GTK icon token.
  gnomeLayout = "menu:${buttons}";
  gnome = pkgs.gsettings-desktop-schemas;
  gnomeSchemas = "${gnome}/share/gsettings-schemas/${gnome.name}/glib-2.0/schemas";
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

  # GDK Wayland gets this preference through GSettings when the settings
  # portal is absent. Ship this one schema with a vendor default to keep
  # user values and the other GNOME schemas on their normal lookup path.
  schemas="$out/share/brook/gsettings"
  mkdir -p "$schemas" schema-source
  cp ${gnomeSchemas}/org.gnome.desktop.enums.xml      ${gnomeSchemas}/org.gnome.desktop.wm.preferences.gschema.xml schema-source/
  cat >schema-source/99-brook.gschema.override <<'SETTINGS'
  [org.gnome.desktop.wm.preferences]
  button-layout='${gnomeLayout}'
  SETTINGS
  glib-compile-schemas --strict --targetdir="$schemas" schema-source
  schema=org.gnome.desktop.wm.preferences
  export GSETTINGS_SCHEMA_DIR="$schemas" GSETTINGS_BACKEND=memory
  test "$(gsettings get "$schema" button-layout)" = "'${gnomeLayout}'"

  export XDG_CONFIG_HOME="$PWD/user-config" GSETTINGS_BACKEND=keyfile
  keyfile="$XDG_CONFIG_HOME/glib-2.0/settings/keyfile"
  mkdir -p "$(dirname "$keyfile")"
  printf '%s\n' "[org/gnome/desktop/wm/preferences]" "button-layout='close:'" >"$keyfile"
  cp "$keyfile" before
  test "$(gsettings get "$schema" button-layout)" = "'close:'"
  cmp before "$keyfile"
''
