#version 440

// Capa de la línea de tiempo: video NV12 (luma R8 + croma RG8) o imagen RGBA, con recorte,
// clave de croma, corrección de color (LUT 3D), máscara de forma, opacidad y modo de fusión.
// Las fórmulas coinciden con las de referencia de movix_color (ColorMath.cpp).

layout(location = 0) in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 yuvToRgb;
    vec4 crop;      // u0, v0, u1, v1: parte visible de la capa
    vec4 params;    // x opacidad, y modo de fusión, z ¿LUT?, w formato (0 NV12, 1 RGBA)
    vec4 key;       // x ¿croma?, y tolerancia, z suavidad, w derrame
    vec4 keyChroma; // x Cb, y Cr del color clave; z relación de aspecto de la capa; w textura premultiplicada
    vec4 mask0;     // x forma (0 ninguna, 1 rectángulo, 2 elipse, 3 lineal, 5 textura), y invertir, z suavizado, w rotación (rad)
    vec4 mask1;     // x, y centro; z, w ancho y alto (fracciones de la capa)
    vec4 lutCoord;  // x escala, y desplazamiento de las coordenadas de la LUT
    vec4 canvas;    // x 1/ancho, y 1/alto del lienzo (para leer el fondo); z ¿componer en luz lineal?
    vec4 signal;    // x señal de la fuente (0 SDR, 1 PQ, 2 HLG), y ¿Rec.2020?, z señal del proyecto, w ¿convertir?
} ubuf;

layout(binding = 1) uniform sampler2D tex0;     // Y o RGBA
layout(binding = 2) uniform sampler2D tex1;     // CbCr (NV12)
layout(binding = 3) uniform sampler3D lut;
layout(binding = 4) uniform sampler2D backdrop; // copia del lienzo bajo la capa
layout(binding = 5) uniform sampler2D maskTex;  // máscaras rasterizadas (forma 5)

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

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


// Luz lineal normalizada (0..1) de la señal del proyecto, para componer: SDR con su curva 2,4, PQ
// hasta 10 000 nits y HLG en luz de escena.
vec3 signalToLinear(vec3 e, int sig)
{
    e = clamp(e, 0.0, 1.0);
    if (sig == 1)
        return pqToNits(e) / 10000.0;
    if (sig == 2)
        return vec3(hlgToScene(e.r), hlgToScene(e.g), hlgToScene(e.b));
    return pow(e, vec3(2.4));
}
vec3 linearToSignal(vec3 l, int sig)
{
    l = clamp(l, 0.0, 1.0);
    if (sig == 1)
        return nitsToPq(l * 10000.0);
    if (sig == 2)
        return vec3(sceneToHlg(l.r), sceneToHlg(l.g), sceneToHlg(l.b));
    return pow(l, vec3(1.0 / 2.4));
}

vec2 chromaOf(vec3 c)
{
    float y = dot(c, kLuma);
    return vec2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
}

float blendChannel(int mode, float b, float s)
{
    switch (mode) {
    case 1: return b * s;                                        // multiplicar
    case 2: return b + s - b * s;                                // trama
    case 3: return b <= 0.5 ? 2.0 * s * b : s + (2.0 * b - 1.0) - s * (2.0 * b - 1.0); // superponer
    case 4: return min(b, s);                                    // oscurecer
    case 5: return max(b, s);                                    // aclarar
    case 6: return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s)));         // sobreexponer
    case 7: return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s));   // subexponer
    case 8: return s <= 0.5 ? b * 2.0 * s : b + (2.0 * s - 1.0) - b * (2.0 * s - 1.0); // luz fuerte
    case 9: {                                                    // luz suave
        if (s <= 0.5)
            return b - (1.0 - 2.0 * s) * b * (1.0 - b);
        float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
        return b + (2.0 * s - 1.0) * (d - b);
    }
    case 10: return abs(b - s);                                  // diferencia
    case 11: return b + s - 2.0 * b * s;                         // exclusión
    case 12: return min(1.0, b + s);                             // sumar
    case 13: return max(0.0, b - s);                             // restar
    }
    return s;
}

float maskAlpha(vec2 uv)
{
    int shape = int(ubuf.mask0.x + 0.5);
    if (shape == 0)
        return 1.0;
    if (shape == 5)
        return texture(maskTex, uv).r;
    float aspect = ubuf.keyChroma.z;
    vec2 p = vec2((uv.x - ubuf.mask1.x) * aspect, uv.y - ubuf.mask1.y);
    float c = cos(ubuf.mask0.w), s = sin(ubuf.mask0.w);
    vec2 q = vec2(c * p.x + s * p.y, -s * p.x + c * p.y); // al sistema de la forma
    vec2 h = max(vec2(ubuf.mask1.z * aspect, ubuf.mask1.w) * 0.5, vec2(1e-5));
    float sd;
    if (shape == 1) {
        vec2 d = abs(q) - h;
        sd = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
    } else if (shape == 2) {
        sd = (length(q / h) - 1.0) * min(h.x, h.y);
    } else {
        sd = -q.y; // lineal: se ve el lado de abajo de la línea
    }
    float f = ubuf.mask0.z;
    float m = f > 0.0 ? 1.0 - smoothstep(-0.5 * f, 0.5 * f, sd) : (sd <= 0.0 ? 1.0 : 0.0);
    return ubuf.mask0.y > 0.5 ? 1.0 - m : m;
}

void main()
{
    vec2 uv = vTexcoord;
    if (uv.x < ubuf.crop.x || uv.y < ubuf.crop.y || uv.x > ubuf.crop.z || uv.y > ubuf.crop.w)
        discard;

    vec3 rgb;
    float alpha;
    if (ubuf.params.w > 0.5) {
        vec4 c = texture(tex0, uv);
        rgb = ubuf.keyChroma.w > 0.5 ? c.rgb / max(c.a, 1e-5) : c.rgb;
        alpha = c.a;
    } else {
        vec4 yuv = vec4(texture(tex0, uv).r, texture(tex1, uv).rg, 1.0);
        rgb = clamp((ubuf.yuvToRgb * yuv).rgb, 0.0, 1.0);
        alpha = 1.0;
    }

    if (ubuf.key.x > 0.5) {
        vec2 pc = chromaOf(rgb);
        vec2 kc = ubuf.keyChroma.xy;
        float dist = 2.0 * length(pc - kc);
        float tol = ubuf.key.y, soft = ubuf.key.z;
        alpha *= soft > 0.0 ? smoothstep(tol, tol + soft, dist) : step(tol, dist);
        float klen = length(kc);
        if (klen > 1e-5 && ubuf.key.w > 0.0) {
            vec2 dir = kc / klen;
            pc -= ubuf.key.w * max(0.0, dot(pc, dir)) * dir;
        }
        float y = dot(rgb, kLuma);
        float r = y + 1.5748 * pc.y;
        float b = y + 1.8556 * pc.x;
        rgb = clamp(vec3(r, (y - 0.2126 * r - 0.0722 * b) / 0.7152, b), 0.0, 1.0);
    }

    // A la señal de trabajo del proyecto, después de la clave de croma (el color clave se toma de
    // la fuente, en su propia señal) y antes de la corrección (que trabaja en la del proyecto).
    if (ubuf.signal.w > 0.5)
        rgb = convertSignal(rgb, int(ubuf.signal.x + 0.5), ubuf.signal.y > 0.5, int(ubuf.signal.z + 0.5));

    if (ubuf.params.z > 0.5)
        rgb = texture(lut, clamp(rgb, 0.0, 1.0) * ubuf.lutCoord.x + ubuf.lutCoord.y).rgb;

    alpha *= maskAlpha(uv) * ubuf.params.x;

    // En luz lineal el lienzo guarda luz, no señal: la fusión, la opacidad y los fundidos mezclan
    // como la luz real. La copia final (canvas.frag) vuelve a la señal del proyecto.
    if (ubuf.canvas.z > 0.5)
        rgb = signalToLinear(rgb, int(ubuf.signal.z + 0.5));

    int mode = int(ubuf.params.y + 0.5);
    if (mode != 0) {
        vec3 b = texture(backdrop, gl_FragCoord.xy * ubuf.canvas.xy).rgb;
        rgb = vec3(blendChannel(mode, b.r, rgb.r), blendChannel(mode, b.g, rgb.g), blendChannel(mode, b.b, rgb.b));
    }
    fragColor = vec4(rgb, alpha);
}
