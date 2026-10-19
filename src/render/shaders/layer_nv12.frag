#version 440

// Capa de video NV12: luma en una textura R8 y croma intercalado en RG8, convertidos a RGB.

layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 yuvToRgb;
    vec4 crop;
    vec4 params;
} ubuf;

layout(binding = 1) uniform sampler2D texY;
layout(binding = 2) uniform sampler2D texUV;

void main()
{
    if (vTexcoord.x < ubuf.crop.x || vTexcoord.y < ubuf.crop.y || vTexcoord.x > ubuf.crop.z || vTexcoord.y > ubuf.crop.w)
        discard;
    vec4 yuv = vec4(texture(texY, vTexcoord).r, texture(texUV, vTexcoord).rg, 1.0);
    fragColor = vec4(clamp((ubuf.yuvToRgb * yuv).rgb, 0.0, 1.0), ubuf.params.x);
}
