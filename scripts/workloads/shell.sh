#!/usr/bin/env bash
# Run in Brook after installing stock fastfetch, zsh and starship.
set -euo pipefail
export PATH=/nix/profile/bin:/nix/bin:/bin
export HOME=/data/home
export TERM=xterm-256color
work=/data/home/shell-acceptance
export TMPDIR="$work/tmp"
mkdir -p "$TMPDIR"
cat > "$work/zsh-test.zsh" <<'ZSH'
set -eu
cd /data/home/shell-acceptance
typeset -a words=(Brook shell acceptance)
[[ ${#words} == 3 && ${words[2]} == shell ]]
print -r -- "${(j: :)words}" > result.txt
[[ $(<result.txt) == 'Brook shell acceptance' ]]
[[ $(print -r -- 'c b a' | tr ' ' '\n' | sort | tr '\n' ' ') == 'a b c ' ]]
typeset -a matches=(result.*)
[[ ${#matches} == 1 && ${matches[1]} == result.txt ]]
print -r -- ZSH_WORKLOAD_PASS
ZSH
zsh -f "$work/zsh-test.zsh"
fastfetch --logo-type file --logo /boot/BROOK.ASC \
    --logo-color-1 cyan --logo-color-2 white \
    --structure OS:Kernel:Uptime:CPU:Memory:Shell --show-errors true
fastfetch --logo-type none --structure OS:Kernel:Uptime:CPU:Memory --format json > "$work/fastfetch.json"
test -s "$work/fastfetch.json"
echo FASTFETCH_WORKLOAD_PASS
cat > "$work/starship.toml" <<'TOML'
add_newline = false
format = '$directory$character'
[directory]
style = 'bold cyan'
[character]
success_symbol = '[>](bold green)'
error_symbol = '[>](bold red)'
TOML
export STARSHIP_CONFIG="$work/starship.toml"
starship prompt --path "$work" --status 0 --terminal-width 100 > "$work/prompt.txt"
test -s "$work/prompt.txt"
starship init zsh > "$work/starship-init.zsh"
zsh -f -n "$work/starship-init.zsh"
echo STARSHIP_WORKLOAD_PASS
sync
echo SHELL_WORKLOAD_EXIT_0
