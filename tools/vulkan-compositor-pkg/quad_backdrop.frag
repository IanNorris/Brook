#version 450
// Glass backdrop passes (quad_backdrop.h), each drawn into one mip level of
// the atlas over a tile's texels. quad.vert places the rectangle; the push
// block starts with its fields.
//
// BROOK_BACKDROP_CAPTURE: one scene layer over mip 0. A texel is the exact
// premultiplied box of block x block desktop pixels, counting only pixels
// inside the layer piece (clip) and outside its hole, blended over what the
// texel holds. Pixels past the capture limit repeat the last one (the
// screen edge clamps). With BOX (an unscaled layer whose colour may be
// averaged as stored) a 2x2 block wholly inside the piece is one bilinear
// tap at its corner, a quarter of the sampler work.
// Otherwise a dual-Kawase step (Bjorge, SIGGRAPH 2015) from the adjacent
// level: DOWN takes the corner and four diagonal half-texel taps of the
// finer level, UP four axis and four diagonal taps of the coarser level.
// Taps are clamped to the tile's texels in the level read.
layout(set=0,binding=0) uniform sampler2D source;
#ifdef BROOK_BACKDROP_CAPTURE
layout(input_attachment_index=0,set=1,binding=0) uniform subpassInput destination;
// The same layer through an unnormalized bilinear sampler (BOX).
layout(set=2,binding=0) uniform sampler2D sourceLinear;
#endif
layout(push_constant) uniform Pass {
    ivec4 rect; vec2 invHalf; ivec2 dst; uvec2 dstSize; uvec2 src; uvec2 srcSize; vec2 scale; uint opacity; uint mode;
    // CAPTURE: origin = desktop pixel of texel (0,0)'s block, limit = last
    // desktop pixel, clip and hole in desktop pixels.
    // DOWN/UP: origin..limit = the tile's texels in the level read,
    // clip0 = the tile's origin in the level drawn, clip1 = level read's size.
    ivec2 origin; ivec2 limit; ivec2 clip0; ivec2 clip1; ivec2 hole0; ivec2 hole1; uint block;
} q;
layout(location=0) out vec4 color;
const uint STRAIGHT=1u,OPAQUE=2u,FETCH_SAMPLER=8u,FETCH_DIVIDE=16u,UP=256u,BOX=512u;
#ifdef BROOK_BACKDROP_CAPTURE
vec4 source_at(uvec2 u){
    if((q.mode&FETCH_SAMPLER)!=0u)
        return textureLod(source,vec2(q.src)+(vec2(u)+0.5)*q.scale,0.0);
    if((q.mode&FETCH_DIVIDE)!=0u)
        return texelFetch(source,ivec2(q.src+(2u*u+1u)*q.srcSize/(2u*q.dstSize)),0);
    return texelFetch(source,ivec2(q.src+u),0);
}
void main(){
    const int block=int(q.block);
    const ivec2 first=q.origin+ivec2(gl_FragCoord.xy)*block;
    const ivec2 last=first+block-1;
    vec4 sum=vec4(0.0);
    if((q.mode&BOX)!=0u && all(greaterThanEqual(first,q.clip0)) && all(lessThan(last,q.clip1)) && all(lessThanEqual(last,q.limit)) &&
       (any(greaterThanEqual(first,q.hole1)) || any(lessThan(last,q.hole0)))){
        sum=4.0*textureLod(sourceLinear,vec2(q.src)+vec2(first-q.dst)+1.0,0.0);
        if((q.mode&OPAQUE)!=0u)sum.a=4.0;
    }else for(int j=0;j<block;++j)for(int i=0;i<block;++i){
        const ivec2 p=min(first+ivec2(i,j),q.limit);
        if(any(lessThan(p,q.clip0)) || any(greaterThanEqual(p,q.clip1)) ||
           (all(greaterThanEqual(p,q.hole0)) && all(lessThan(p,q.hole1))))continue;
        vec4 t=source_at(uvec2(p-q.dst));
        if((q.mode&OPAQUE)!=0u)t.a=1.0;
        if((q.mode&STRAIGHT)!=0u)t.rgb*=t.a;
        sum+=t;
    }
    sum*=float(q.opacity)/(255.0*float(block*block));
    color=sum+(1.0-sum.a)*subpassLoad(destination);
}
#else
vec4 tap(vec2 at){
    return textureLod(source,clamp(at,vec2(q.origin)+0.5,vec2(q.limit)-0.5)/vec2(q.clip1),0.0);
}
void main(){
    const vec2 texel=floor(gl_FragCoord.xy)-vec2(q.clip0);
    if((q.mode&UP)==0u){
        const vec2 c=vec2(q.origin)+2.0*texel+1.0;
        color=(4.0*tap(c)+tap(c+vec2(-1.0,-1.0))+tap(c+vec2(1.0,-1.0))+
               tap(c+vec2(-1.0,1.0))+tap(c+vec2(1.0,1.0)))*0.125;
    }else{
        const vec2 u=vec2(q.origin)+(texel+0.5)*0.5;
        color=(tap(u+vec2(-1.0,0.0))+tap(u+vec2(1.0,0.0))+tap(u+vec2(0.0,-1.0))+tap(u+vec2(0.0,1.0))+
               2.0*(tap(u+vec2(-0.5,-0.5))+tap(u+vec2(0.5,-0.5))+tap(u+vec2(-0.5,0.5))+tap(u+vec2(0.5,0.5))))/12.0;
    }
}
#endif
