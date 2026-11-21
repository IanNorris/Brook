# Exact network compatibility preset. Keep its grouped outputs for existing
# disks and signed stage receipts. App-selected profiles use the same composer.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; } }:
import ./brook-profile.nix { inherit pkgs; preset = "network"; }
