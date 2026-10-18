# Candidate normal-launcher audio integration. Guest acceptance is still required.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  audio = import ../alsa-oss-pkg { inherit pkgs; };
  config = pkgs.writeText "brook-firefox-asound.conf" ''
    pcm_type.oss {
      lib "${audio}/lib/alsa-lib/libasound_module_pcm_oss.so"
    }
    pcm.brook_oss {
      type oss
      device "/dev/dsp"
    }
    pcm.!default {
      type plug
      slave {
        pcm "brook_oss"
        format S16_LE
        rate 44100
        channels 2
      }
    }
    ctl.!default { type empty }
  '';
  browser = pkgs.firefox.override {
    # Set the default branch only: existing user prefs/user.js retain priority.
    # Keep Firefox's content and audio sandbox settings unchanged.
    extraPrefs = ''defaultPref("media.cubeb.backend", "alsa");'';
  };
in browser.overrideAttrs (old: {
  makeWrapperArgs = (old.makeWrapperArgs or []) ++ [
    "--run"
    # Brook's shell exports the legacy global file-over-null configuration.
    # Replace that inherited default, while respecting a custom caller config.
    ''if [ -z "''${ALSA_CONFIG_PATH:-}" ] || [ "$ALSA_CONFIG_PATH" = /nix/etc/asound.conf ]; then export ALSA_CONFIG_PATH=${config}; fi''
  ];
})
