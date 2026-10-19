#version 440

// Copia el lienzo compuesto al destino final (visor o textura de exportación).

layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

// params.x = 1: invertir v (destinos con el origen abajo, como OpenGL). params.y = 1: conservar el
// alfa (secuencias anidadas, que se componen sobre fondo transparente). signal.x > 0: la señal del
// lienzo (1 PQ, 2 HLG) se convierte a SDR para mostrarla en una pantalla SDR.
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

layout(binding = 1) uniform sampler2D canvasTex;

// --- Señales SDR/HDR (mismas fórmulas que color/Hdr.cpp) ---
const vec3 kLuma709 = vec3(0.2126, 0.7152, 0.0722);
const vec3 kLuma2020 = vec3(0.2627, 0.6780, 0.0593);
const mat3 k709To2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956); // por columnas
const mat3 k2020To709 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187);
const float kSdrWhite = 203.0;
const float kHdrPeak = 1000.0;

vec3 pqToNits(vec3 e)
{
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / 78.84375));
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(1.0 / 0.1593017578125));
}
vec3 nitsToPq(vec3 n)
{
    vec3 y = pow(clamp(n / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}
float hlgToScene(float e)
{
    return e <= 0.5 ? e * e / 3.0 : (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}
float sceneToHlg(float s)
{
    s = max(s, 0.0);
    return s <= 1.0 / 12.0 ? sqrt(3.0 * s) : 0.17883277 * log(12.0 * s - 0.28466892) + 0.55991073;
}
// Señal (0 SDR, 1 PQ, 2 HLG) → nits en sus primarias.
vec3 signalToNits(vec3 e, int sig)
{
    if (sig == 1)
        return pqToNits(e);
    if (sig == 2) {
        vec3 s = vec3(hlgToScene(clamp(e.r, 0.0, 1.0)), hlgToScene(clamp(e.g, 0.0, 1.0)), hlgToScene(clamp(e.b, 0.0, 1.0)));
        float ys = max(dot(s, kLuma2020), 1e-9);
        return kHdrPeak * pow(ys, 0.2) * s;
    }
    return pow(clamp(e, 0.0, 1.0), vec3(2.4)) * kSdrWhite;
}
vec3 nitsToSignal(vec3 n, int sig)
{
    if (sig == 1)
        return nitsToPq(n);
    if (sig == 2) {
        vec3 d = n / kHdrPeak;
        float k = pow(max(dot(d, kLuma2020), 1e-9), -0.2 / 1.2);
        return vec3(sceneToHlg(d.r * k), sceneToHlg(d.g * k), sceneToHlg(d.b * k));
    }
    vec3 x = n / kSdrWhite;
    float y = max(dot(x, kLuma709), 0.0);
    const float knee = 0.7;
    if (y > knee) { // lineal hasta la rodilla, compresión de las luces hasta el pico HDR
        float range = (kHdrPeak / kSdrWhite - knee) / (1.0 - knee);
        float e = (y - knee) / (1.0 - knee);
        float ym = knee + (1.0 - knee) * e * (1.0 + e / (range * range)) / (1.0 + e);
        x *= ym / y;
    }
    return pow(clamp(x, 0.0, 1.0), vec3(1.0 / 2.4));
}
vec3 convertSignal(vec3 e, int from, bool from2020, int to)
{
    bool to2020 = to != 0;
    vec3 n = signalToNits(e, from);
    if (from2020 && !to2020)
        n = k2020To709 * n;
    else if (!from2020 && to2020)
        n = k709To2020 * n;
    return nitsToSignal(max(n, 0.0), to);
}

void main()
{
    vec2 uv = vTexcoord;
    if (ubuf.params.x > 0.5)
        uv.y = 1.0 - uv.y;
    vec4 c = texture(canvasTex, uv);
    int sig = int(ubuf.signal.x + 0.5);
    if (sig > 0)
        c.rgb = convertSignal(c.rgb, sig, true, 0);
    fragColor = vec4(c.rgb, ubuf.params.y > 0.5 ? c.a : 1.0);
}
