# Clocked native playback slave for Brook. Select through an outer ALSA plug;
# keep the existing default until guest pointer/delay/drain acceptance passes.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.alsa-plugins.overrideAttrs (old: {
  pname = "alsa-plugins-oss-brook";
  buildInputs = [ pkgs.alsa-lib ];
  patches = (old.patches or []) ++ [ ./clocked-oss.patch ];
  configureFlags = (old.configureFlags or []) ++ [
    "--enable-oss" "--disable-mix" "--disable-usbstream" "--disable-arcamav"
    "--disable-jack" "--disable-pulseaudio" "--disable-samplerate"
    "--disable-libav" "--disable-a52" "--disable-lavrate" "--disable-speexdsp"
  ];
  makeFlags = (old.makeFlags or []) ++ [ "SUBDIRS=oss" ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    $CC -DPIC -I. -Ioss ${./test_callbacks.c} $(pkg-config --cflags --libs alsa) -o test-oss-callbacks
    ./test-oss-callbacks
    $CC -DPIC -fPIC -shared -I. -Ioss ${./test_rate_plugin.c} $(pkg-config --cflags --libs alsa) -o test-oss-rate.so
    $CC ${./test_rate_negotiation.c} $(pkg-config --cflags --libs alsa) -o test-oss-rate-negotiation
    ./test-oss-rate-negotiation "$PWD/test-oss-rate.so"
    runHook postCheck
  '';
})
