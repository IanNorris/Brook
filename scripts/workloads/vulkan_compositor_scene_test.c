#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../tools/vulkan-compositor-pkg/compositor_probe.h"

int main(void) {
    uint32_t v[16384]; compositor_probe_prepare(v,16384,1);
    assert(brook_vk_scene_valid(v,16384));
    size_t sources[]={4160,8256};
    assert(brook_vk_scene_valid_multi(v,16384,sources,2));
    sources[0]=4159;assert(!brook_vk_scene_valid_multi(v,16384,sources,2));sources[0]=4160;
    sources[1]=8255;assert(!brook_vk_scene_valid_multi(v,16384,sources,2));sources[1]=8256;
    assert(!brook_vk_scene_valid_multi(v,16384,sources,1));
    assert(!brook_vk_scene_valid_multi(v,16384,NULL,2));
    uint32_t saved=v[21]; v[21]=12288; assert(!brook_vk_scene_valid(v,16384)); v[21]=saved;
    saved=v[3]; v[3]=UINT32_MAX; assert(!brook_vk_scene_valid(v,16384)); v[3]=saved;
    saved=v[21+10]; v[21+10]=0; assert(!brook_vk_scene_valid(v,16384)); v[21+10]=saved;
    saved=v[21+16]; v[21+16]=BROOK_VK_STRAIGHT_ALPHA|255; assert(brook_vk_scene_valid(v,16384));
    v[21+16]=512; assert(!brook_vk_scene_valid(v,16384)); v[21+16]=saved;
    saved=v[21+8]; v[21+8]=0x80000000u; assert(!brook_vk_scene_valid(v,16384)); v[21+8]=saved;
    saved=v[21+6]; v[21+6]=64; assert(!brook_vk_scene_valid(v,16384)); v[21+6]=saved;
    assert(!brook_vk_scene_valid(v,100));
    assert(brook_vk_scene_valid(v,16384));
    assert(compositor_probe_pixel(v,0,0)==v[64]);
    assert(compositor_probe_pixel(v,5,12)!=v[64+12*64+5]);
    puts("VULKAN_COMPOSITOR_SCENE_VALIDATION_PASS");
}
