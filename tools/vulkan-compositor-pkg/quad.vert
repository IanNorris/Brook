#version 450
// One pixel-aligned rectangle per draw (triangle strip of four vertices).
// The layout matches struct QuadPush in quad_pass.c and quad.frag.
layout(push_constant) uniform Quad {
    ivec4 rect; vec2 invHalf; ivec2 dst; uvec2 dstSize; uvec2 src; uvec2 srcSize; vec2 scale; uint opacity; uint mode;
} q;
layout(location=0) out vec2 desktopPixel;
void main(){
    vec2 c=vec2((gl_VertexIndex&1)!=0?q.rect.z:q.rect.x,(gl_VertexIndex&2)!=0?q.rect.w:q.rect.y);
    desktopPixel=c;
    gl_Position=vec4(c*q.invHalf-1.0,0.0,1.0);
}
