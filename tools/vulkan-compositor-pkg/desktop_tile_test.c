#include "desktop_tile.h"
#include "compositor_probe.h"
#include <assert.h>
#include <stdio.h>
static uint32_t rng=0xabc123u;
static uint32_t next_random(void){rng=rng*1664525u+1013904223u;return rng;}
static int covers(const uint32_t *l,int x,int y){
    const int dx=(int32_t)l[8],dy=(int32_t)l[9],cx=(int32_t)l[12],cy=(int32_t)l[13];
    return x>=dx && y>=dy && x<dx+(int)l[10] && y<dy+(int)l[11] &&
           x>=cx && y>=cy && x<cx+(int)l[14] && y<cy+(int)l[15];
}
static void occlusion_tests(void){
    uint32_t scene[4+6*17]={16,16,6,0},out[4+6*17],indices[6];
    size_t source[6]={256,256,256,256,256,256},kept;
    struct DesktopDamageSet damage={.count=2,.rects={{0,0,8,16},{8,0,16,16}}};
    for(unsigned iteration=0;iteration<1500;++iteration){
        for(unsigned i=0;i<6;++i){
            uint32_t *l=scene+4+i*17;
            const int x=(int)(next_random()%17)-4,y=(int)(next_random()%17)-4;
            const uint32_t w=1+next_random()%16,h=1+next_random()%16;
            const int cx=(int)(next_random()%17)-4,cy=(int)(next_random()%17)-4;
            const uint32_t cw=next_random()%17,ch=next_random()%17;
            const uint32_t mode=next_random()%4;
            struct BrookVkSceneLayer layer={0,16,16,16,0,0,16,16,x,y,w,h,cx,cy,cw,ch,
                mode==0?0:mode==1?127:mode==2?255:255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE};
            memcpy(l,&layer,sizeof(layer));
        }
        assert(desktop_tile_cull(scene,sizeof(scene)/4,source,6,&damage,out,sizeof(out)/4,indices,&kept));
        int selected[6]={0};for(size_t n=0;n<kept;++n)selected[indices[n]]=1;
        assert(selected[0]);
        /* Independent pixel oracle: every dropped, visible nonzero-opacity
         * pixel must have a retained fully opaque layer above it. */
        for(unsigned i=1;i<6;++i){const uint32_t *l=scene+4+i*17;
            if(selected[i] || !(l[16]&255))continue;
            for(int y=0;y<16;++y)for(int x=0;x<16;++x)if(covers(l,x,y)){
                int hidden=0;
                for(unsigned k=i+1;k<6;++k){const uint32_t *u=scene+4+k*17;
                    if(selected[k] && (u[16]&(BROOK_VK_OPAQUE_SOURCE|255u))==(BROOK_VK_OPAQUE_SOURCE|255u) && covers(u,x,y))hidden=1;
                }
                assert(hidden);
            }
        }
    }
    for(unsigned i=0;i<6;++i){struct BrookVkSceneLayer l={0,16,16,16,0,0,16,16,0,0,16,16,0,0,16,16,255};
        memcpy(scene+4+i*17,&l,sizeof(l));}
    scene[4+5*17+16]=255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE;
    assert(desktop_tile_cull(scene,sizeof(scene)/4,source,6,&damage,out,sizeof(out)/4,indices,&kept));
    assert(kept==2 && indices[0]==0 && indices[1]==5);
    assert(desktop_tile_cull_policy(scene,sizeof(scene)/4,source,6,&damage,out,sizeof(out)/4,indices,&kept,0));
    assert(kept==6 && out[4+5*17+16]==(255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE));
    scene[4+5*17+14]=15; /* one exposed column keeps all lower layers */
    assert(desktop_tile_cull(scene,sizeof(scene)/4,source,6,&damage,out,sizeof(out)/4,indices,&kept));
    assert(kept==6);
    scene[4+5*17+14]=16;scene[4+5*17+16]=254|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE;
    assert(desktop_tile_cull(scene,sizeof(scene)/4,source,6,&damage,out,sizeof(out)/4,indices,&kept) && kept==6);
}
int main(void){
    occlusion_tests();
    uint32_t scene[16384],tile[4+2*17];size_t sources[]={16384,16384};
    compositor_probe_prepare(scene,16384,1);scene[21]=0;scene[3]=0;
    assert(desktop_tile_scene(scene,38,sources,2,7,9,37,25,tile,38));
    assert(tile[0]==37 && tile[1]==25 && tile[2]==2 && tile[3]==0);
    assert((int32_t)tile[4+8]==-7 && (int32_t)tile[4+9]==-9);
    assert((int32_t)tile[21+8]==-11 && (int32_t)tile[21+9]==-2);
    assert((int32_t)tile[21+12]==-5 && (int32_t)tile[21+13]==1);
    tile[0]=123;
    assert(!desktop_tile_scene(scene,38,sources,2,64,0,1,1,tile,38) && tile[0]==123);
    assert(!desktop_tile_scene(scene,38,sources,2,63,0,2,1,tile,38) && tile[0]==123);
    assert(!desktop_tile_scene(scene,37,sources,2,0,0,1,1,tile,38) && tile[0]==123);
    assert(!desktop_tile_scene(scene,38,sources,2,0,0,1,1,tile,37) && tile[0]==123);
    uint32_t crop[37*25];
    assert(desktop_tile_crop_backdrop(scene,38,sources,2,7,9,37,25,scene,crop,37*25,tile,38));
    for(unsigned y=0;y<25;++y)for(unsigned x=0;x<37;++x)
        assert(crop[y*37+x]==scene[64+(y+9)*64+x+7]);
    assert(tile[4]==0 && tile[5]==37 && tile[6]==37 && tile[7]==25);
    tile[0]=123;crop[0]=456;
    assert(!desktop_tile_crop_backdrop(scene,38,sources,2,7,9,37,25,scene,crop,37*25-1,tile,38) && tile[0]==123 && crop[0]==456);
    assert(!desktop_tile_crop_backdrop(scene,38,sources,2,7,9,37,25,scene,scene+64,37*25,tile,38) && tile[0]==123);
    scene[20]=254;
    assert(!desktop_tile_crop_backdrop(scene,38,sources,2,7,9,37,25,scene,crop,37*25,tile,38) && tile[0]==123);
    scene[20]=255;
    struct DesktopDamageSet damage={0};struct DesktopPackedTile packedTiles[2];
    {
        struct DesktopDamageSet selectionDamage={0};uint32_t filtered[38],indices[2];size_t kept=0;
        desktop_damage_add(&selectionDamage,(struct DesktopDamageRect){50,50,60,60},64,64);
        assert(desktop_tile_cull(scene,38,sources,2,&selectionDamage,filtered,38,indices,&kept));
        assert(kept==1 && indices[0]==0 && filtered[2]==1);
        desktop_damage_add(&selectionDamage,(struct DesktopDamageRect){10,10,20,20},64,64);
        assert(desktop_tile_cull(scene,38,sources,2,&selectionDamage,filtered,38,indices,&kept));
        assert(kept==2 && indices[0]==0 && indices[1]==1 && !memcmp(filtered+21,scene+21,17*sizeof(uint32_t)));
        scene[21+16]=0;
        assert(desktop_tile_cull(scene,38,sources,2,&selectionDamage,filtered,38,indices,&kept) && kept==1);
        scene[21+16]=127;
        selectionDamage.rects[0].right=65;
        assert(!desktop_tile_cull(scene,38,sources,2,&selectionDamage,filtered,38,indices,&kept));
    }
    desktop_damage_add(&damage,(struct DesktopDamageRect){0,0,10,10},64,64);
    desktop_damage_add(&damage,(struct DesktopDamageRect){30,20,40,30},64,64);
    uint32_t packed[212],scenes[2*BROOK_VK_TILE_STRIDE]={0};
    assert(desktop_tile_pack(scene,38,sources,2,&damage,scene,packed,212,scenes,2*BROOK_VK_TILE_STRIDE,packedTiles,2));
    assert(packedTiles[0].offsetBytes==0 && packedTiles[1].offsetBytes==448 &&
           scenes[BROOK_VK_TILE_STRIDE+3]==112 && scenes[BROOK_VK_TILE_STRIDE+4]==112);
    assert(packed[0]==scene[64] && packed[112]==scene[64+20*64+30]);
    {
        /* Exercise the renderer's cull -> compact source list -> pack path.
         * A removed middle layer must not shift the last layer onto its
         * descriptor, including when multiple disjoint tiles are packed. */
        uint32_t filtered[55],indices[3],batch[2*BROOK_VK_TILE_STRIDE]={0};
        size_t originalSources[]={16384,9000,14000},compactSources[3],kept=0;
        memcpy(scene+38,scene+21,17*sizeof(uint32_t));scene[2]=3;scene[21+16]=0;
        assert(desktop_tile_cull(scene,55,originalSources,3,&damage,filtered,55,indices,&kept));
        assert(kept==2 && indices[0]==0 && indices[1]==2);
        for(size_t i=0;i<kept;++i)compactSources[i]=originalSources[indices[i]];
        assert(desktop_tile_pack(filtered,55,compactSources,kept,&damage,scene,packed,212,
                                 batch,2*BROOK_VK_TILE_STRIDE,packedTiles,2));
        for(unsigned i=0;i<2;++i){
            uint32_t *layer=batch+i*BROOK_VK_TILE_STRIDE+21;
            assert(batch[i*BROOK_VK_TILE_STRIDE+2]==2 && layer[0]==scene[38]);
            assert((int32_t)layer[8]==(int32_t)scene[38+8]-(int32_t)damage.rects[i].left);
            assert((int32_t)layer[9]==(int32_t)scene[38+9]-(int32_t)damage.rects[i].top);
        }
        assert(packed[0]==scene[64] && packed[112]==scene[64+20*64+30]);
        scene[2]=2;scene[21+16]=127;
    }
    assert(desktop_tile_pack(scene,38,sources,2,&damage,NULL,NULL,212,scenes,2*BROOK_VK_TILE_STRIDE,packedTiles,2));
    const uint32_t *cachedTile=scenes+BROOK_VK_TILE_STRIDE;
    assert(packedTiles[1].offsetBytes==448 && cachedTile[3]==112 && cachedTile[4]==64);
    assert((int32_t)cachedTile[12]==-30 && (int32_t)cachedTile[13]==-20);
    assert(cachedTile[5]==64 && cachedTile[6]==64 && cachedTile[7]==64);
    assert(!desktop_tile_pack(scene,38,sources,2,&damage,scene,packed,211,scenes,2*BROOK_VK_TILE_STRIDE,packedTiles,2));
    damage.rects[1]=damage.rects[0];
    assert(!desktop_tile_pack(scene,38,sources,2,&damage,scene,packed,212,scenes,2*BROOK_VK_TILE_STRIDE,packedTiles,2));
    sources[1]=2;assert(!desktop_tile_scene(scene,38,sources,2,0,0,1,1,tile,38) && tile[0]==123);
    puts("DESKTOP_TILE_VALIDATION_PASS");return 0;
}
