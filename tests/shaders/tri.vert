#version 450
layout(location = 0) out vec3 color;
layout(push_constant) uniform PC { float t; } pc;
vec2 P[3] = vec2[](vec2(0.0, -0.75), vec2(0.7, 0.6), vec2(-0.7, 0.6));
vec3 C[3] = vec3[](vec3(1, 0.2, 0.2), vec3(0.2, 1, 0.2), vec3(0.2, 0.3, 1));
void main() {
    float s = sin(pc.t), c = cos(pc.t);
    vec2 p = P[gl_VertexIndex];
    gl_Position = vec4(c * p.x - s * p.y, s * p.x + c * p.y, 0.0, 1.0);
    color = C[gl_VertexIndex];
}
