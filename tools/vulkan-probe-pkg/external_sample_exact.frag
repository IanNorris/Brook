#version 450
// Diagnostic single-layer straight-alpha blend over an opaque background.
// Match the compositor's integer rounding, rather than fixed-function blend.
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(push_constant) uniform Background { uvec4 rgba; } background;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    uvec4 s=uvec4(round(texture(sourceImage,uv)*255.0));
    uint inv=255u-s.a;
    uvec3 rgb=(s.rgb*s.a+background.rgba.rgb*inv+uvec3(127u))/255u;
    color=vec4(vec3(rgb)/255.0,1.0);
}
