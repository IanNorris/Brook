#version 450
const vec2 positions[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
layout(location=0) out vec2 uv;
void main() {
    vec2 p=positions[gl_VertexIndex];
    gl_Position=vec4(p,0,1);
    uv=(p+vec2(1))*0.5;
}
