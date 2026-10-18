#version 440

// Conversión NV12 → RGB en la GPU: luma en una textura R8 y croma intercalado en RG8.

layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 yuvToRgb;
} ubuf;

layout(binding = 1) uniform sampler2D texY;
layout(binding = 2) uniform sampler2D texUV;

void main()
{
    vec4 yuv = vec4(texture(texY, vTexcoord).r, texture(texUV, vTexcoord).rg, 1.0);
    fragColor = vec4(clamp((ubuf.yuvToRgb * yuv).rgb, 0.0, 1.0), 1.0);
}
