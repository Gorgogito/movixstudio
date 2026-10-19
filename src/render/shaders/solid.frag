#version 440

// Relleno de color sólido (fondo del lienzo).

layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 yuvToRgb; // en este shader, la primera columna es el color RGBA
    vec4 crop;
    vec4 params;
} ubuf;

void main()
{
    fragColor = ubuf.yuvToRgb[0];
}
