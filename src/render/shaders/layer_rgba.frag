#version 440

// Capa RGBA (imágenes y títulos) con transparencia propia, multiplicada por la opacidad de la capa.

layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 yuvToRgb;
    vec4 crop;
    vec4 params;
} ubuf;

layout(binding = 1) uniform sampler2D texRgba;
layout(binding = 2) uniform sampler2D unused;

void main()
{
    if (vTexcoord.x < ubuf.crop.x || vTexcoord.y < ubuf.crop.y || vTexcoord.x > ubuf.crop.z || vTexcoord.y > ubuf.crop.w)
        discard;
    vec4 c = texture(texRgba, vTexcoord);
    fragColor = vec4(c.rgb, c.a * ubuf.params.x);
}
