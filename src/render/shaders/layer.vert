#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec2 vTexcoord;

// Mismo bloque que layer.frag (lo comparten todos los shaders del compositor).
layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 yuvToRgb;
    vec4 crop;
    vec4 params;
    vec4 key;
    vec4 keyChroma;
    vec4 mask0;
    vec4 mask1;
    vec4 lutCoord;
    vec4 canvas;
    vec4 signal;
} ubuf;

void main()
{
    vTexcoord = texcoord;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
