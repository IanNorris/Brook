#!/nix/profile/bin/bash
# Optional installation, not a promise of acceptance on the software profile.
# The image's package index may carry another version than the workload reports.
set -eu
case "${1:-}" in
  libreoffice|codium|supertuxkart|gltron|warzone2100) package=$1 ;;
  *) echo 'Usage: open-source-app.sh libreoffice|codium|supertuxkart|gltron|warzone2100' >&2; exit 64 ;;
esac
echo "Installing $package through Brook nix-install. See software.json for tested scope."
exec /nix/bin/nix-install "$package"
