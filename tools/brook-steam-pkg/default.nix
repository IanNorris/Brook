{ pkgs, steamPkgs }:
let
  inherit (pkgs) lib;
  source = builtins.fromJSON (builtins.readFile ./valve-client.json);
  runtime = import ../../nix/runtime-graphics.nix { inherit pkgs; };
  exports = pkgs.buildEnv {
    name = "brook-steam-provider-exports";
    pathsToLink = [ "/lib" ];
    ignoreCollisions = true;
    # getLib: buildEnv would otherwise link a package's outputsToInstall
    # (often bin), which left libc, libdrm, libssl, libdbus... unexported.
    paths = map lib.getLib (with pkgs; [
      glibc stdenv.cc.cc.lib libglvnd runtime.mesaRuntime wayland libx11 libxext
      libxcb libxrandr libxi libxfixes libxcomposite libxrender libxdamage
      libxcursor libxxf86vm libxscrnsaver libdrm libgbm udev libgudev
      zlib zstd openssl gnutls nettle pulseaudio dbus libcap libseccomp
      fontconfig freetype alsa-lib
    ]);
  };
  loader = "${pkgs.glibc}/lib/ld-linux-x86-64.so.2";
  provider = pkgs.runCommand "brook-steam-provider" { nativeBuildInputs = [ pkgs.python3 ]; } ''
    python3 ${../../scripts/workloads/steamrt_provider_cache.py} \
      --ldconfig ${lib.getBin pkgs.glibc}/bin/ldconfig --loader ${loader} \
      --exports ${exports}/lib --include-directory ${pkgs.glibc}/lib --output "$out"
  '';
  archives = map (entry: entry // { store = toString (pkgs.fetchurl {
    url = "https://client-update.steamstatic.com/${entry.file}";
    inherit (entry) sha256;
  }); }) source.packages;
  plan = pkgs.writeText "brook-steam-client-archive-plan.json"
    (builtins.toJSON (source // { packages = archives; }));
  client = steamPkgs.stdenvNoCC.mkDerivation {
    pname = "brook-steam-client";
    inherit (source) version;
    dontUnpack = true;
    dontFixup = true;
    nativeBuildInputs = [ pkgs.python3 pkgs.patchelf ];
    installPhase = ''
      python3 ${./install-client.py} \
        --bootstrap ${../steam-bootstrap}/bootstrap.py \
        --patch-elf ${pkgs.patchelf}/bin/patchelf --loader ${loader} \
        --output "$out/share/brook/steam-client" --plan ${plan}
      install -Dm644 ${./valve-client.json} "$out/share/brook/steam-client-source.json"
      ln -s ${exports}/lib "$out/share/brook/steam-client/hostlibs"
    '';
    meta = {
      description = "Valve Steam x86-64 client and pinned SteamRT3c runtime";
      platforms = [ "x86_64-linux" ];
      license = lib.licenses.unfree;
    };
  };
  # Profile transfers carry each store path in one bounded bundle (under
  # 960 MiB), and the whole client is about 1.3 GB. Ship steamrt64 as its own
  # path; the seed links it and lists it in .brook-seed-layers, so the first
  # launch copies it as a real directory (brook_steam_seed.py).
  clientRuntime = pkgs.runCommand "brook-steam-client-steamrt64-${source.version}" { disallowedReferences = [ client ]; } ''
    cp -a ${client}/share/brook/steam-client/steamrt64 "$out"
    chmod -R u+w "$out"
    # Valve's archives carry SteamRT's soname symlinks only in usr-mtree.txt.gz;
    # the extraction keeps regular files. Recreate the links (libexpat.so.1 ...).
    ${pkgs.python3}/bin/python3 - "$out"/pv-runtime/steam-runtime-steamrt/steamrt3c_platform_* <<'PY'
    import gzip, os, sys
    for platform in sys.argv[1:]:
        made = 0
        with gzip.open(os.path.join(platform, 'usr-mtree.txt.gz'), 'rt') as listing:
            for line in listing:
                fields = line.split()
                if 'type=link' not in fields:
                    continue
                link = next(f[5:] for f in fields if f.startswith('link='))
                path = os.path.normpath(os.path.join(platform, 'files', fields[0].replace('\\040', ' ')))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                if not os.path.lexists(path):
                    os.symlink(link.replace('\\040', ' '), path)
                    made += 1
        if made < 1000:
            raise SystemExit(f'unexpectedly few SteamRT links: {made}')
        print('SteamRT links', made)
    PY
    test -L "$out"/pv-runtime/steam-runtime-steamrt/steamrt3c_platform_*/files/lib/x86_64-linux-gnu/libexpat.so.1
  '';
  seed = pkgs.runCommand "brook-steam-client-seed-${source.version}" {
    disallowedReferences = [ client ];
  } ''
    mkdir -p "$out/share/brook"
    cp -a ${client}/share/brook/steam-client-source.json "$out/share/brook/"
    mkdir "$out/share/brook/steam-client"
    for entry in ${client}/share/brook/steam-client/* ${client}/share/brook/steam-client/.[!.]*; do
      [ -e "$entry" ] || [ -L "$entry" ] || continue
      [ "''${entry##*/}" = steamrt64 ] || cp -a "$entry" "$out/share/brook/steam-client/"
    done
    ln -s ${clientRuntime} "$out/share/brook/steam-client/steamrt64"
    echo steamrt64 > "$out/share/brook/steam-client/.brook-seed-layers"
  '';
  # Steam's steamrt64/reaper is i386, which Brook cannot run (every native game exited 126);
  # the launcher puts this native64 build of the public upstream reaper in its place.
  reaper = import ../reaper-pkg { inherit pkgs; };
  launcher = pkgs.runCommand "brook-steam-launcher-${source.version}" {} ''
    mkdir -p "$out/bin" "$out/libexec"
    install -m644 ${../../scripts/workloads/brook_steam_launcher.py} "$out/libexec/brook-steam.py"
    install -m644 ${../../scripts/workloads/brook_steam_seed.py} "$out/libexec/brook_steam_seed.py"
    install -m644 ${../steam-bootstrap}/bootstrap.py "$out/libexec/bootstrap.py"
    install -m644 ${../../scripts/workloads/steam_client_check.py} "$out/libexec/steam-client-check.py"
    cat > "$out/bin/brook-steam" <<EOF
    #!${pkgs.bash}/bin/bash
    umask 077
    export BROOK_STEAM_PACKAGED_SEED=${seed}/share/brook/steam-client
    export BROOK_STEAM_PACKAGED_VERSION=${source.version}
    export BROOK_STEAM_PROVIDER=${provider}
    export BROOK_STEAM_PULSEAUDIO=${pkgs.pulseaudio}/bin/pulseaudio
    export BROOK_STEAM_PACTL=${pkgs.pulseaudio}/bin/pactl
    export BROOK_STEAM_REAPER=${reaper}/bin/reaper
    exec /nix/profile/bin/brook-run ${pkgs.python3}/bin/python3 $out/libexec/brook-steam.py "\$@"
    EOF
    chmod 755 "$out/bin/brook-steam"
  '';
in {
  inherit client clientRuntime seed provider launcher;
  inherit (source) version;
  packages = [ launcher runtime.mesaRuntime pkgs.libglvnd ];
}
