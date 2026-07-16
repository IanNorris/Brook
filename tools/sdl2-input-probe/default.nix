# SDL2 keyboard/text-input probe for Brook BRO-216.
#
# A minimal SDL2 client that logs, per keypress: the SDL_KEYDOWN scancode (fixed
# evdev table, no xkb), the keysym.sym (from xkb), and any SDL_TEXTINPUT text
# (xkb utf8). Running it on Brook under waylandd and pressing keys splits, in one
# observation, why yquake2's console text is dead while its bindings work:
#
#   scancode OK, sym==0/UNKNOWN, no TEXTINPUT  => SDL2's xkb.state is NULL/invalid
#   scancode OK, sym valid, TEXTINPUT present  => xkb works; look elsewhere
#
# Uses the SAME SDL2 from the nixpkgs pin the yquake2 closure was built against,
# so it exercises the identical Wayland/xkb path yquake2 hits.
#
# Build:   nix-build tools/sdl2-input-probe --no-out-link
# Run on Brook (from a working terminal, software path — no GL needed):
#   SDL_VIDEODRIVER=wayland <out>/bin/kbdprobe
# then press qwerty/asdf and read the PROBE lines on serial. Esc to quit.

{ pkgs ? import ../../nix/nixpkgs.nix {} }:

pkgs.stdenv.mkDerivation {
  pname = "sdl2-kbdprobe";
  version = "1.0";
  src = ./.;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ pkgs.SDL2 pkgs.libxkbcommon ];
  buildPhase = ''
    # GUI probe: splits "SDL2 xkb works" from "xkb.state NULL" via keypresses.
    $CC -O2 -Wall -Wextra $(pkg-config --cflags sdl2) \
      kbdprobe.c -o kbdprobe $(pkg-config --libs sdl2)
    # Headless probe: replicates SDL2's mmap(MAP_PRIVATE, memfd)+xkb compile,
    # runnable from a plain terminal (no GUI / no input injection).
    $CC -O2 -Wall -Wextra $(pkg-config --cflags xkbcommon) \
      xkb_memfd_probe.c -o xkb_memfd_probe $(pkg-config --libs xkbcommon)
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp kbdprobe xkb_memfd_probe $out/bin/
  '';
}
