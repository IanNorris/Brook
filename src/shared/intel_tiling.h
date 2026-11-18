#pragma once
/* Intel X/Y tiling address arithmetic for 32-bit pixels (Gen8+, no bit-6
 * swizzle). X tiles are 512 bytes x 8 rows; Y tiles are 128 bytes x 32 rows
 * stored as eight 16-byte columns of 32 rows. pitch is in bytes. Used by
 * waylandd to read a tiled client buffer on the CPU fallback path and by
 * host tests to build tiled sources. */
#include <stddef.h>
#include <stdint.h>
enum { BROOK_TILING_LINEAR=0, BROOK_TILING_X=1, BROOK_TILING_Y=2 };
static inline uint64_t brook_tiled_offset(int tiling,uint64_t pitch,uint32_t x,uint32_t y){
    const uint64_t xb=(uint64_t)x*4u;
    if(tiling==BROOK_TILING_X)
        return ((uint64_t)(y>>3)*(pitch>>9)+(xb>>9))*4096u+(uint64_t)(y&7u)*512u+(xb&511u);
    if(tiling==BROOK_TILING_Y)
        return ((uint64_t)(y>>5)*(pitch>>7)+(xb>>7))*4096u+((xb&127u)>>4)*512u+(uint64_t)(y&31u)*16u+(xb&15u);
    return (uint64_t)y*pitch+xb;
}
/* Bytes a tiled surface of this size occupies (whole tile rows). */
static inline uint64_t brook_tiled_bytes(int tiling,uint64_t pitch,uint32_t height){
    const uint32_t rows=tiling==BROOK_TILING_X?8u:tiling==BROOK_TILING_Y?32u:1u;
    return pitch*(((uint64_t)height+rows-1u)/rows*rows);
}
/* Copy a tiled (or linear) source into a linear width x height destination
 * with dst_stride pixels per row. The pitch must be a multiple of the tile
 * width (512 bytes for X, 128 for Y). */
static inline void brook_detile32(uint32_t *dst,size_t dst_stride,const uint8_t *src,int tiling,uint64_t pitch,
                                  uint32_t width,uint32_t height){
    for(uint32_t y=0;y<height;++y){
        uint32_t *row=dst+(size_t)y*dst_stride;
        if(tiling==BROOK_TILING_X){
            /* 128-pixel spans are contiguous within an X tile row. */
            for(uint32_t x=0;x<width;x+=128u){
                const uint32_t n=width-x<128u?width-x:128u;
                const uint32_t *s=(const uint32_t *)(src+brook_tiled_offset(tiling,pitch,x,y));
                for(uint32_t i=0;i<n;++i)row[x+i]=s[i];
            }
        }else if(tiling==BROOK_TILING_Y){
            for(uint32_t x=0;x<width;x+=4u){
                const uint32_t n=width-x<4u?width-x:4u;
                const uint32_t *s=(const uint32_t *)(src+brook_tiled_offset(tiling,pitch,x,y));
                for(uint32_t i=0;i<n;++i)row[x+i]=s[i];
            }
        }else{
            const uint32_t *s=(const uint32_t *)(src+(uint64_t)y*pitch);
            for(uint32_t i=0;i<width;++i)row[i]=s[i];
        }
    }
}
static inline int brook_tiling_from_modifier(uint64_t modifier){
    return modifier==0x0100000000000001ull?BROOK_TILING_X:modifier==0x0100000000000002ull?BROOK_TILING_Y:
        modifier==0?BROOK_TILING_LINEAR:-1;
}
