#version 440

// Triángulo que cubre todo el destino, sin vértices de entrada (draw(3)).
layout(location = 0) out vec2 vTexcoord;

layout(std140, binding = 0) uniform buf {
    vec4 params; // x: plano (0 = Y, 1 = UV); y: 1 si la textura tiene el origen abajo
} ubuf;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vTexcoord = vec2(p.x, ubuf.params.y > 0.5 ? p.y : 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
