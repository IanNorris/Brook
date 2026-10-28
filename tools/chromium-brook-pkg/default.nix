# Keep Chromium's ALSA recovery on the same clocked OSS route as Firefox.
{ pkgs ? import ../../nix/nixpkgs.nix {}, downstreamStaging ? false }:
let
  audio = import ../alsa-oss-pkg { inherit pkgs downstreamStaging; };
  config = pkgs.writeText "brook-chromium-asound.conf" ''
    pcm_type.oss {
      lib "${audio}/lib/alsa-lib/libasound_module_pcm_oss.so"
    }
    pcm.brook_oss { type oss; device "/dev/dsp"; }
    pcm.!default {
      type plug
      slave { pcm "brook_oss"; format S16_LE; rate 44100; channels 2; }
    }
    ctl.!default { type empty }
  '';
in pkgs.symlinkJoin {
  name = "chromium-brook-${pkgs.chromium.version}";
  paths = [ pkgs.chromium ];
  nativeBuildInputs = [ pkgs.makeWrapper ];
  postBuild = ''
    wrapProgram $out/bin/chromium --run 'if [ -z "''${ALSA_CONFIG_PATH:-}" ] || [ "$ALSA_CONFIG_PATH" = /nix/etc/asound.conf ]; then export ALSA_CONFIG_PATH=${config}; fi'
  '';
}
