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
layout(push_constant) uniform Quad {
    ivec4 rect; vec2 invHalf; ivec2 dst; uvec2 dstSize; uvec2 src; uvec2 srcSize; vec2 scale; uint opacity; uint mode;
} q;
layout(location=0) out vec4 color;
const uint STRAIGHT=1u,OPAQUE=2u,OVER_ZERO=4u,FETCH_SAMPLER=8u,FETCH_DIVIDE=16u,READ_DESTINATION=32u;
uint scale8(uint x,uint f){return (x*f+127u)/255u;}
uvec4 scale8(uvec4 x,uint f){return (x*f+127u)/255u;}
void main(){
    uvec2 u=uvec2(ivec2(gl_FragCoord.xy)-q.dst);
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
            uvec4 d=uvec4(round(subpassLoad(destination)*255.0));
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
