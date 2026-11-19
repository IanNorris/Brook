#version 450
// One layer of the packed scene ABI, drawn over a pixel-aligned rectangle.
// The arithmetic is composite.comp's: integer pixel-center scaling and the
// same straight/premultiplied/opacity rounding. By default the shader reads
// the destination pixel as an input attachment and blends exactly (the
// Intel blend unit's UNORM8 rounding differs from composite.comp by one
// step on some translucent pixels). With FIXED_BLEND the pipeline's blend
// state finishes the result instead; see quad_plan.h for the draw kinds.
layout(set=0,binding=0) uniform sampler2D source;
layout(input_attachment_index=0,set=1,binding=0) uniform subpassInput destination;
#ifdef BROOK_QUAD_BLUR
layout(set=2,binding=0) uniform sampler2D backdrop;
#endif
layout(location=0) in vec2 desktopPixel;
layout(push_constant) uniform Quad {
    ivec4 rect; vec2 invHalf; ivec2 dst; uvec2 dstSize; uvec2 src; uvec2 srcSize; vec2 scale; uint opacity; uint mode;
#ifdef BROOK_QUAD_BLUR
    ivec4 glassRect; ivec4 atlasRect; uint radius;
#endif
} q;
layout(location=0) out vec4 color;
const uint STRAIGHT=1u,OPAQUE=2u,OVER_ZERO=4u,FETCH_SAMPLER=8u,FETCH_DIVIDE=16u,READ_DESTINATION=32u,CAPTURE=64u;
uint scale8(uint x,uint f){return (x*f+127u)/255u;}
uvec4 scale8(uvec4 x,uint f){return (x*f+127u)/255u;}
#ifdef BROOK_QUAD_BLUR
vec4 glass_sample(vec2 pixel,vec2 delta) {
    const vec2 atlasSize=vec2(480.0,270.0);
    vec2 lo=vec2(q.atlasRect.xy)+0.5,hi=vec2(q.atlasRect.zw)-0.5;
    return textureLod(backdrop,clamp(pixel+delta,lo,hi)/atlasSize,0.0);
}
vec4 glass_backdrop() {
    vec2 scale=vec2(q.atlasRect.zw-q.atlasRect.xy)/vec2(q.glassRect.zw-q.glassRect.xy);
    vec2 sampleAt=vec2(q.atlasRect.xy)+(gl_FragCoord.xy-vec2(q.glassRect.xy))*scale;
    if(q.radius<=3u)return glass_sample(sampleAt,vec2(0));
    vec2 d=float(q.radius)*scale*0.5;
    return (4.0*glass_sample(sampleAt,vec2(0)) + glass_sample(sampleAt,vec2(d.x,0)) +
        glass_sample(sampleAt,vec2(-d.x,0)) + glass_sample(sampleAt,vec2(0,d.y)) +
        glass_sample(sampleAt,vec2(0,-d.y)))*0.125;
}
#endif
void main(){
    vec2 xy=(q.mode&CAPTURE)!=0u?desktopPixel:gl_FragCoord.xy;
    uvec2 u=uvec2(ivec2(xy)-q.dst);
    vec4 t;
    if((q.mode&FETCH_SAMPLER)!=0u){
        // Integer upscale: the unnormalized nearest sampler gives exactly
        // floor(src+(u+0.5)*sw/dw), which equals the integer formula.
        t=textureLod(source,vec2(q.src)+(vec2(u)+0.5)*q.scale,0.0);
    }else if((q.mode&FETCH_DIVIDE)!=0u){
        t=texelFetch(source,ivec2(q.src+(2u*u+1u)*q.srcSize/(2u*q.dstSize)),0);
    }else t=texelFetch(source,ivec2(q.src+u),0);
    uvec4 s=uvec4(round(t*255.0));
    if((q.mode&OPAQUE)!=0u)s.a=255u;
    const uint op=q.opacity;
    if((q.mode&READ_DESTINATION)!=0u){
        uvec4 r=s;
        if(op!=255u || s.a!=255u){
#ifdef BROOK_QUAD_BLUR
            vec4 base=s.a!=0u && op!=0u?glass_backdrop():subpassLoad(destination);
#else
            vec4 base=subpassLoad(destination);
#endif
            uvec4 d=uvec4(round(base*255.0));
            if((q.mode&STRAIGHT)!=0u){
                uint a=scale8(s.a,op),inv=255u-a;
                r=uvec4((s.rgb*a+d.rgb*inv+127u)/255u,min(255u,a+scale8(d.a,inv)));
            }else{
                uvec4 p=scale8(s,op);
                r=min(uvec4(255u),p+scale8(d,255u-p.a));
            }
        }
        color=vec4(r)/255.0;
    }else if((q.mode&STRAIGHT)!=0u){
        uint a=scale8(s.a,op);
        uvec3 c=(q.mode&OVER_ZERO)!=0u?(s.rgb*a+127u)/255u:s.rgb;
        color=vec4(vec3(c),float(a))/255.0;
    }else color=vec4(scale8(s,op))/255.0;
}
