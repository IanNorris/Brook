# Clocked native playback slave for Brook. Select through an outer ALSA plug;
# keep the existing default until guest pointer/delay/drain acceptance passes.
{ pkgs ? import ../../nix/nixpkgs.nix {}, lowWaterReadiness ? false }:
pkgs.alsa-plugins.overrideAttrs (old: {
  pname = "alsa-plugins-oss-brook";
  buildInputs = [ pkgs.alsa-lib ];
  patches = (old.patches or []) ++ [ ./clocked-oss.patch ] ++ pkgs.lib.optional lowWaterReadiness ./low-water.patch;
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
    ${pkgs.lib.optionalString lowWaterReadiness ''
    $CC -DPIC -fPIC -shared -I. -Ioss ${./test_readiness_plugin.c} $(pkg-config --cflags --libs alsa) -o test-oss-readiness.so
    $CC ${./test_readiness.c} $(pkg-config --cflags --libs alsa) -ldl -o test-oss-readiness
    for readiness_rate in 8000 22050 44100 48000 96000; do
      ./test-oss-readiness "$PWD/test-oss-readiness.so" "$readiness_rate"
    done
    ''}
    runHook postCheck
  '';
})
