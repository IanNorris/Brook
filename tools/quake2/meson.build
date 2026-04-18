project('quake2', 'c')

m_dep = meson.get_compiler('c').find_library('m', required : false)

# Client + engine
client_sources = [
    'source/cl_cin.c',
    'source/cl_ents.c',
    'source/cl_fx.c',
    'source/cl_input.c',
    'source/cl_inv.c',
    'source/cl_main.c',
    'source/cl_newfx.c',
    'source/cl_parse.c',
    'source/cl_pred.c',
    'source/cl_scrn.c',
    'source/cl_tent.c',
    'source/cl_view.c',
    'source/console.c',
    'source/keys.c',
    'source/menu.c',
    'source/qmenu.c',
    'source/snd_dma.c',
    'source/snd_mem.c',
    'source/snd_mix.c',
]

# Common engine
common_sources = [
    'source/cmd.c',
    'source/cmodel.c',
    'source/common.c',
    'source/crc.c',
    'source/cvar.c',
    'source/files.c',
    'source/md4.c',
    'source/net_chan.c',
    'source/pmove.c',
    'source/q_shared.c',
]

# Server
server_sources = [
    'source/sv_ccmds.c',
    'source/sv_ents.c',
    'source/sv_game.c',
    'source/sv_init.c',
    'source/sv_main.c',
    'source/sv_send.c',
    'source/sv_user.c',
    'source/sv_world.c',
]

# Game logic (linked statically instead of dlopen)
game_sources = [
    'source/g_ai.c',
    'source/g_chase.c',
    'source/g_cmds.c',
    'source/g_combat.c',
    'source/g_func.c',
    'source/g_items.c',
    'source/g_main.c',
    'source/g_misc.c',
    'source/g_monster.c',
    'source/g_phys.c',
    'source/g_save.c',
    'source/g_spawn.c',
    'source/g_svcmds.c',
    'source/g_target.c',
    'source/g_trigger.c',
    'source/g_turret.c',
    'source/g_utils.c',
    'source/g_weapon.c',
    'source/m_actor.c',
    'source/m_berserk.c',
    'source/m_boss2.c',
    'source/m_boss3.c',
    'source/m_boss31.c',
    'source/m_boss32.c',
    'source/m_brain.c',
    'source/m_chick.c',
    'source/m_flash.c',
    'source/m_flipper.c',
    'source/m_float.c',
    'source/m_flyer.c',
    'source/m_gladiator.c',
    'source/m_gunner.c',
    'source/m_hover.c',
    'source/m_infantry.c',
    'source/m_insane.c',
    'source/m_medic.c',
    'source/m_move.c',
    'source/m_mutant.c',
    'source/m_parasite.c',
    'source/m_soldier.c',
    'source/m_supertank.c',
    'source/m_tank.c',
    'source/p_client.c',
    'source/p_hud.c',
    'source/p_trail.c',
    'source/p_view.c',
    'source/p_weapon.c',
]

# Software renderer (ref_soft)
refsoft_sources = [
    'source/r_aclip.c',
    'source/r_alias.c',
    'source/r_bsp.c',
    'source/r_draw.c',
    'source/r_edge.c',
    'source/r_image.c',
    'source/r_light.c',
    'source/r_main.c',
    'source/r_misc.c',
    'source/r_model.c',
    'source/r_part.c',
    'source/r_poly.c',
    'source/r_polyse.c',
    'source/r_rast.c',
    'source/r_scan.c',
    'source/r_sprite.c',
    'source/r_surf.c',
]

# Brook OS platform layer
brook_sources = [
    'source/sys_brook.c',
    'source/vid_brook.c',
    'source/swimp_brook.c',
    'source/in_brook.c',
    'source/snd_brook.c',
    'source/net_brook.c',
    'source/q_shbrook.c',
    'source/cd_null.c',
    'source/glob.c',
]

all_sources = client_sources + common_sources + server_sources + game_sources + refsoft_sources + brook_sources

static_library('quake2', all_sources,
    dependencies : m_dep,
    c_args : [
        '-DREF_HARD_LINKED',   # renderer linked statically
        '-DGAME_HARD_LINKED',  # game module linked statically
        '-DLINUX',
        '-Did386=0',           # no x86 asm (we're x86-64)
    ],
)
