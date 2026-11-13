#version 450
// Each fragment owns one output pixel. Descriptor indices are compile-time
// constants so no sampled-image dynamic-indexing feature is required.
layout(set=0,binding=0,std430) readonly buffer Scene { uint words[]; } scene;
layout(set=0,binding=1) uniform sampler2D sourceImage[16];
layout(location=0) out vec4 color;
vec4 fetchSource(uint i,ivec2 p) {
    switch(i) {
    case 0u:return texelFetch(sourceImage[0],p,0); case 1u:return texelFetch(sourceImage[1],p,0);
    case 2u:return texelFetch(sourceImage[2],p,0); case 3u:return texelFetch(sourceImage[3],p,0);
    case 4u:return texelFetch(sourceImage[4],p,0); case 5u:return texelFetch(sourceImage[5],p,0);
    case 6u:return texelFetch(sourceImage[6],p,0); case 7u:return texelFetch(sourceImage[7],p,0);
    case 8u:return texelFetch(sourceImage[8],p,0); case 9u:return texelFetch(sourceImage[9],p,0);
    case 10u:return texelFetch(sourceImage[10],p,0); case 11u:return texelFetch(sourceImage[11],p,0);
    case 12u:return texelFetch(sourceImage[12],p,0); case 13u:return texelFetch(sourceImage[13],p,0);
    case 14u:return texelFetch(sourceImage[14],p,0); case 15u:return texelFetch(sourceImage[15],p,0);
    }
    return vec4(0);
}
uint scale8(uint x,uint factor) { return (x*factor+127u)/255u; }
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    // Work in BGRA word order to match composite.comp's integer operations.
    uvec4 dst=uvec4(0u);
    for(uint i=0u;i<scene.words[2];++i){
        uint b=4u+i*17u;
        int x=int(scene.words[b+8u]),y=int(scene.words[b+9u]);
        int cx=int(scene.words[b+12u]),cy=int(scene.words[b+13u]);
        uint w=scene.words[b+10u],h=scene.words[b+11u],cw=scene.words[b+14u],ch=scene.words[b+15u];
        if(p.x<x || p.y<y || p.x>=x+int(w) || p.y>=y+int(h) ||
           p.x<cx || p.y<cy || p.x>=cx+int(cw) || p.y>=cy+int(ch))continue;
        uint sx=scene.words[b+4u]+(2u*uint(p.x-x)+1u)*scene.words[b+6u]/(2u*w);
        uint sy=scene.words[b+5u]+(2u*uint(p.y-y)+1u)*scene.words[b+7u]/(2u*h);
        vec4 rgba=fetchSource(i,ivec2(sx,sy));
        uvec4 s=uvec4(round(rgba.bgra*255.0));
        uint mode=scene.words[b+16u],opacity=mode&255u;
        if((mode&512u)!=0u)s.w=255u;
        if((mode&256u)!=0u){
            uint alpha=scale8(s.w,opacity),inv=255u-alpha;
            dst=uvec4((s.xyz*alpha+dst.xyz*inv+uvec3(127u))/255u,min(255u,alpha+scale8(dst.w,inv)));
        }else{
            s=uvec4(scale8(s.x,opacity),scale8(s.y,opacity),scale8(s.z,opacity),scale8(s.w,opacity));
            uint inv=255u-s.w;
            dst=min(uvec4(255u),s+uvec4(scale8(dst.x,inv),scale8(dst.y,inv),scale8(dst.z,inv),scale8(dst.w,inv)));
        }
    }
    color=vec4(vec3(dst.zyx)/255.0,float(dst.w)/255.0);
}
