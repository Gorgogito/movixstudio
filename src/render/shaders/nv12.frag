#version 440

// Lienzo RGB → planos NV12 (BT.709, rango limitado), como hacía swscale en la CPU. El plano UV
// se dibuja a mitad de tamaño: el muestreo bilineal en el centro de cada bloque de 2×2 lo promedia.
layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 params;
} ubuf;
layout(binding = 1) uniform sampler2D canvas;

void main()
{
    vec3 c = clamp(texture(canvas, vTexcoord).rgb, 0.0, 1.0);
    if (ubuf.params.x < 0.5) {
        float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
        fragColor = vec4(16.0 / 255.0 + y * (219.0 / 255.0), 0.0, 0.0, 1.0);
    } else {
        float cb = dot(c, vec3(-0.114572, -0.385428, 0.5));
        float cr = dot(c, vec3(0.5, -0.454153, -0.045847));
        fragColor = vec4(128.0 / 255.0 + cb * (224.0 / 255.0), 128.0 / 255.0 + cr * (224.0 / 255.0), 0.0, 1.0);
    }
}
