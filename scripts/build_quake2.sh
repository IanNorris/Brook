#!/usr/bin/env bash
# Build Quake 2 for Brook OS (static musl-libc binary)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BROOK_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

Q2_SRC="$BROOK_ROOT/tools/quake2/source"
BUILD_DIR="$BROOK_ROOT/build/quake2"

mkdir -p "$BUILD_DIR"

CC="${MUSL_CC:-x86_64-unknown-linux-musl-gcc}"
CFLAGS="-static -O2 -fno-stack-protector -std=gnu99 -DLINUX -Did386=0 -DREF_HARD_LINKED -DGAME_HARD_LINKED -D_DEFAULT_SOURCE -w -Wno-error -Wno-incompatible-pointer-types -Wno-implicit-int -Wno-implicit-function-declaration -Wno-int-conversion"

# Client/engine sources
CLIENT_SRCS="
cl_cin.c cl_ents.c cl_fx.c cl_input.c cl_inv.c cl_main.c
cl_newfx.c cl_parse.c cl_pred.c cl_scrn.c cl_tent.c cl_view.c
console.c keys.c menu.c qmenu.c
snd_dma.c snd_mem.c snd_mix.c
"

# Common engine
COMMON_SRCS="
cmd.c cmodel.c common.c crc.c cvar.c files.c md4.c
net_chan.c pmove.c q_shared.c
"

# Server
SERVER_SRCS="
sv_ccmds.c sv_ents.c sv_game.c sv_init.c sv_main.c
sv_send.c sv_user.c sv_world.c
"

# Game logic
GAME_SRCS="
g_ai.c g_chase.c g_cmds.c g_combat.c g_func.c g_items.c
g_main.c g_misc.c g_monster.c g_phys.c g_save.c g_spawn.c
g_svcmds.c g_target.c g_trigger.c g_turret.c g_utils.c g_weapon.c
m_actor.c m_berserk.c m_boss2.c m_boss3.c m_boss31.c m_boss32.c
m_brain.c m_chick.c m_flash.c m_flipper.c m_float.c m_flyer.c
m_gladiator.c m_gunner.c m_hover.c m_infantry.c m_insane.c m_medic.c
m_move.c m_mutant.c m_parasite.c m_soldier.c m_supertank.c m_tank.c
p_client.c p_hud.c p_trail.c p_view.c p_weapon.c
"

# Software renderer (ref_soft)
REFSOFT_SRCS="
r_aclip.c r_alias.c r_bsp.c r_draw.c r_edge.c r_image.c
r_light.c r_main.c r_misc.c r_model.c r_part.c r_poly.c
r_polyse.c r_rast.c r_scan.c r_sprite.c r_surf.c
"

# Brook platform layer
BROOK_SRCS="
sys_brook.c vid_brook.c swimp_brook.c in_brook.c snd_brook.c
net_brook.c q_shbrook.c cd_brook.c stb_vorbis_impl.c glob.c
"

ALL_SRCS="$CLIENT_SRCS $COMMON_SRCS $SERVER_SRCS $GAME_SRCS $REFSOFT_SRCS $BROOK_SRCS"

# Compile each source file
OBJS=""
for src in $ALL_SRCS; do
    obj="$BUILD_DIR/${src%.c}.o"
    if [ "$Q2_SRC/$src" -nt "$obj" ] 2>/dev/null; then
        echo "  CC  $src"
        $CC $CFLAGS -I"$Q2_SRC" -c "$Q2_SRC/$src" -o "$obj"
    fi
    OBJS="$OBJS $obj"
done

# Link
echo "  LD  quake2"
$CC $CFLAGS $OBJS -o "$BUILD_DIR/quake2" -lm -lc
echo "  SIZE $(wc -c < "$BUILD_DIR/quake2") bytes"
echo "Done: $BUILD_DIR/quake2"
