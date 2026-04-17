#!/usr/bin/env bash
# Build DOOM for Brook OS (static musl-libc binary)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BROOK_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DOOM_SRC="${DOOM_SRC:-$(cd "$BROOK_ROOT/../doomgeneric_enkel/doomgeneric" 2>/dev/null && pwd)}"
BROOK_SRC="$BROOK_ROOT/src/doom"
BUILD_DIR="$BROOK_ROOT/build/doom"

mkdir -p "$BUILD_DIR"

# Use musl cross-compiler from nix (pkgsCross.musl64.stdenv.cc)
CC="${MUSL_CC:-x86_64-unknown-linux-musl-gcc}"
CFLAGS="-static -Os -fno-stack-protector -std=gnu11 -DNORMALUNIX -DBROOK -DFEATURE_SOUND -D_DEFAULT_SOURCE -Wall -Wno-unused-result -Wno-incompatible-pointer-types"

# All DOOM source files (from the original Makefile, minus the Enkel platform file)
DOOM_SRCS="
dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c
d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c hu_lib.c
hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c i_sound.c
i_timer.c memio.c mus2mid.c m_argv.c m_bbox.c m_cheat.c m_config.c
m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c p_doors.c
p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c
p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c
p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c
r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c
s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c
w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c
"

# Compile each DOOM source file
OBJS=""
for src in $DOOM_SRCS; do
    obj="$BUILD_DIR/${src%.c}.o"
    if [ "$DOOM_SRC/$src" -nt "$obj" ] 2>/dev/null; then
        echo "  CC  $src"
        $CC $CFLAGS -I"$BROOK_SRC" -I"$DOOM_SRC" -c "$DOOM_SRC/$src" -o "$obj"
    fi
    OBJS="$OBJS $obj"
done

# Compile Brook platform file and overrides
echo "  CC  doomgeneric_brook.c"
$CC $CFLAGS -I"$BROOK_SRC" -I"$DOOM_SRC" -c "$BROOK_SRC/doomgeneric_brook.c" -o "$BUILD_DIR/doomgeneric_brook.o"
OBJS="$OBJS $BUILD_DIR/doomgeneric_brook.o"

echo "  CC  i_system.c (Brook override)"
$CC $CFLAGS -I"$BROOK_SRC" -I"$DOOM_SRC" -c "$BROOK_SRC/i_system.c" -o "$BUILD_DIR/i_system_brook.o"
OBJS="$OBJS $BUILD_DIR/i_system_brook.o"

echo "  CC  i_osssound.c (Brook OSS audio)"
$CC $CFLAGS -I"$BROOK_SRC" -I"$DOOM_SRC" -c "$BROOK_SRC/i_osssound.c" -o "$BUILD_DIR/i_osssound.o"
OBJS="$OBJS $BUILD_DIR/i_osssound.o"

# Link
echo "  LD  doom"
$CC $CFLAGS $OBJS -o "$BUILD_DIR/doom" -lc
echo "  SIZE $(wc -c < "$BUILD_DIR/doom") bytes"
echo "Done: $BUILD_DIR/doom"
