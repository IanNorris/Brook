{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# Brook user apps: brook-apps-index (desktop entries a user installed, such as
# web apps, Chromium installs and Flatpak exports, into the per-user launcher
# manifest) and brook-webapp (install a website as an app).
let
  python = pkgs.python3.withPackages (ps: [ ps.pillow ps.cairosvg ]);
in pkgs.stdenv.mkDerivation {
  pname = "brook-apps";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ python ];
  doCheck = true;
  checkPhase = ''
    HOME=$TMPDIR python -m unittest -v test_brook_apps
  '';
  installPhase = ''
    mkdir -p $out/bin $out/lib/brook-apps
    install -m644 brook_apps.py $out/lib/brook-apps/
    for tool in brook-apps-index brook-webapp; do
      install -m755 $tool $out/lib/brook-apps/$tool
      cat > $out/bin/$tool <<EOF
    #!${pkgs.bash}/bin/bash
    export PYTHONPATH=$out/lib/brook-apps
    export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
    exec ${python}/bin/python3 $out/lib/brook-apps/$tool "\$@"
    EOF
      chmod 755 $out/bin/$tool
    done
  '';
}
