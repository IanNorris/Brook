#version 450
// A full-sized opaque background and a clipped, scaled foreground. Results
// follow the integer coordinate and blend contract in composite.comp.
layout(set=0,binding=0) uniform sampler2D foregroundImage;
layout(set=0,binding=1) uniform sampler2D backgroundImage;
layout(push_constant) uniform Blend {
    uint opacity; uint premultiplied;
    int x; int y; uint width; uint height;
    int clipX; int clipY; uint clipWidth; uint clipHeight;
    uint sourceX; uint sourceY; uint sourceWidth; uint sourceHeight;
} blend;
layout(location=0) out vec4 color;
uint scale8(uint x,uint factor) { return (x*factor+127u)/255u; }
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    uvec3 d=uvec3(round(texelFetch(backgroundImage,p,0).rgb*255.0));
    if(p.x<blend.x || p.y<blend.y || p.x>=blend.x+int(blend.width) || p.y>=blend.y+int(blend.height) ||
       p.x<blend.clipX || p.y<blend.clipY || p.x>=blend.clipX+int(blend.clipWidth) || p.y>=blend.clipY+int(blend.clipHeight)) {
        color=vec4(vec3(d)/255.0,1.0);return;
    }
    uint sx=blend.sourceX+(2u*uint(p.x-blend.x)+1u)*blend.sourceWidth/(2u*blend.width);
    uint sy=blend.sourceY+(2u*uint(p.y-blend.y)+1u)*blend.sourceHeight/(2u*blend.height);
    uvec4 s=uvec4(round(texelFetch(foregroundImage,ivec2(sx,sy),0)*255.0));
    uint alpha=scale8(s.a,blend.opacity),inv=255u-alpha;
    uvec3 rgb;
    if(blend.premultiplied!=0u) {
        uvec3 c=uvec3(scale8(s.r,blend.opacity),scale8(s.g,blend.opacity),scale8(s.b,blend.opacity));
        rgb=min(uvec3(255u),c+uvec3(scale8(d.r,inv),scale8(d.g,inv),scale8(d.b,inv)));
    } else rgb=(s.rgb*alpha+d*inv+uvec3(127u))/255u;
    color=vec4(vec3(rgb)/255.0,1.0);
}
