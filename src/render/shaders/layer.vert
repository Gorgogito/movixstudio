#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec2 vTexcoord;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 yuvToRgb;
    vec4 crop;   // u0, v0, u1, v1: parte visible de la capa
    vec4 params; // x = opacidad
} ubuf;

void main()
{
    vTexcoord = texcoord;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
