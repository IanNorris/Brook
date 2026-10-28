#!/nix/profile/bin/bash
# Deliberate web handoff: generic Steam's bootstrap is 32-bit; Brook's accepted
# native64 SteamRT setup must not be replaced by an untested install command.
set -eu
case "${1:-}" in
  steam) url=https://store.steampowered.com/about/ ;;
  helltaker) url=https://store.steampowered.com/app/1289310/Helltaker/ ;;
  alpaca-stacka) url=https://store.steampowered.com/app/1655140/Alpaca_Stacka/ ;;
  *) echo 'Usage: publisher-page.sh steam|helltaker|alpaca-stacka' >&2; exit 64 ;;
esac
exec /nix/profile/bin/brook-chromium "$url"
