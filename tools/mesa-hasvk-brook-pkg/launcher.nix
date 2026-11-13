# Explicit Intel Vulkan launcher; the ordinary GL/Mesa profile is unchanged.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  hasvk = import ./default.nix { inherit pkgs; };
in pkgs.writeShellScriptBin "brook-intel-vulkan" ''
  if [ "$#" -eq 0 ]; then
    echo 'usage: brook-intel-vulkan PROGRAM [ARG ...]' >&2
    exit 2
  fi
  export VK_DRIVER_FILES=${hasvk}/share/vulkan/icd.d/intel_hasvk_icd.x86_64.json
  unset VK_ICD_FILENAMES
  export BROOK_I915_PARTIAL_CAPTURE=1
  exec "$@"
''
