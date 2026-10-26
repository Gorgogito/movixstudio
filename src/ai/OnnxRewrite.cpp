#include "ai/OnnxRewrite.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace movix::ai::onnx {

namespace {

// --- Lectura del formato de protobuf ---

struct Field {
    uint32_t number = 0;
    int wire = 0;
    size_t begin = 0; // inicio de la clave (para copiar el campo entero)
    size_t start = 0; // contenido (wire 2) o valor
    size_t end = 0;   // fin del campo
    uint64_t value = 0; // wire 0
};

bool readVarint(const uint8_t* d, size_t end, size_t& pos, uint64_t& v)
{
    v = 0;
    for (int shift = 0; shift < 64 && pos < end; shift += 7) {
        const uint8_t c = d[pos++];
        v |= uint64_t(c & 0x7F) << shift;
        if (c < 0x80)
            return true;
    }
    return false;
}

class Reader {
public:
    Reader(const uint8_t* d, size_t start, size_t end) : m_d(d), m_pos(start), m_end(end) {}
    bool ok() const { return m_ok; }

    bool next(Field& f)
    {
        if (!m_ok || m_pos >= m_end)
            return false;
        f.begin = m_pos;
        uint64_t key = 0;
        if (!readVarint(m_d, m_end, m_pos, key))
            return fail();
        f.number = uint32_t(key >> 3);
        f.wire = int(key & 7);
        switch (f.wire) {
        case 0:
            if (!readVarint(m_d, m_end, m_pos, f.value))
                return fail();
            f.start = m_pos;
            break;
        case 1:
        case 5: {
            const size_t n = f.wire == 1 ? 8 : 4;
            if (m_end - m_pos < n)
                return fail();
            f.start = m_pos;
            m_pos += n;
            break;
        }
        case 2: {
            uint64_t n = 0;
            if (!readVarint(m_d, m_end, m_pos, n) || n > m_end - m_pos)
                return fail();
            f.start = m_pos;
            m_pos += size_t(n);
            break;
        }
        default: // grupos: no se usan en ONNX
            return fail();
        }
        f.end = m_pos;
        return true;
    }

private:
    bool fail()
    {
        m_ok = false;
        return false;
    }
    const uint8_t* m_d;
    size_t m_pos, m_end;
    bool m_ok = true;
};

// --- Escritura ---

void putVarint(std::string& o, uint64_t v)
{
    while (v >= 0x80) {
        o.push_back(char(uint8_t(v) | 0x80));
        v >>= 7;
    }
    o.push_back(char(v));
}

void putBytes(std::string& o, uint32_t field, std::string_view bytes)
{
    putVarint(o, (uint64_t(field) << 3) | 2);
    putVarint(o, bytes.size());
    o.append(bytes);
}

void putInt(std::string& o, uint32_t field, uint64_t v)
{
    putVarint(o, uint64_t(field) << 3);
    putVarint(o, v);
}

// Números de campo de onnx.proto.
enum : uint32_t {
    kModelGraph = 7,
    kGraphNode = 1,
    kGraphInitializer = 5,
    kNodeInput = 1,
    kNodeOutput = 2,
    kNodeName = 3,
    kNodeOpType = 4,
    kNodeAttribute = 5,
    kNodeDomain = 7,
    kAttrName = 1,
    kAttrI = 3,
    kAttrT = 5,
    kAttrInts = 8,
    kAttrType = 20,
    kAttrTypeInts = 7, // AttributeProto.AttributeType.INTS
    kTensorDims = 1,
    kTensorDataType = 2,
    kTensorInt64Data = 7,
    kTensorName = 8,
    kTensorRawData = 9,
    kTensorDataLocation = 14,
};
constexpr uint64_t kInt64 = 7; // TensorProto.DataType.INT64
constexpr size_t kMaxConstant = 4096; // solo interesan constantes pequeñas (tamaños de Split)

struct Tensor {
    std::string name;
    bool int64 = false;
    bool external = false;
    std::vector<int64_t> values;
};

// Lee repeated int64 en cualquiera de sus dos codificaciones (suelto o empaquetado).
bool readInts(const uint8_t* d, const Field& f, std::vector<int64_t>& out)
{
    if (f.wire == 0) {
        out.push_back(int64_t(f.value));
        return true;
    }
    if (f.wire != 2)
        return false;
    size_t pos = f.start;
    while (pos < f.end) {
        uint64_t v = 0;
        if (!readVarint(d, f.end, pos, v))
            return false;
        out.push_back(int64_t(v));
    }
    return true;
}

bool readTensor(const uint8_t* d, size_t start, size_t end, Tensor& t)
{
    Reader r(d, start, end);
    Field f;
    while (r.next(f)) {
        switch (f.number) {
        case kTensorDataType:
            t.int64 = f.wire == 0 && f.value == kInt64;
            break;
        case kTensorName:
            if (f.wire == 2)
                t.name.assign(reinterpret_cast<const char*>(d + f.start), f.end - f.start);
            break;
        case kTensorInt64Data:
            if (t.values.size() < kMaxConstant && !readInts(d, f, t.values))
                return false;
            break;
        case kTensorRawData:
            if (f.wire == 2 && (f.end - f.start) % 8 == 0 && (f.end - f.start) / 8 <= kMaxConstant) {
                t.values.resize((f.end - f.start) / 8);
                std::memcpy(t.values.data(), d + f.start, f.end - f.start); // little endian
            }
            break;
        case kTensorDataLocation:
            t.external = f.wire == 0 && f.value == 1;
            break;
        default:
            break;
        }
    }
    return r.ok();
}

struct Node {
    std::vector<std::string> inputs, outputs;
    std::string name, op, domain;
    std::unordered_map<std::string, std::vector<int64_t>> ints; // atributos enteros (i o ints), por nombre
    std::vector<std::string> attributes;                        // nombres de todos los atributos
    Tensor value;                                               // atributo `value` (nodos Constant)

    bool has(const char* attribute) const { return std::find(attributes.begin(), attributes.end(), attribute) != attributes.end(); }
    std::vector<int64_t> get(const char* attribute, std::vector<int64_t> fallback = {}) const
    {
        const auto it = ints.find(attribute);
        return it != ints.end() ? it->second : fallback;
    }
};

bool readNode(const uint8_t* d, size_t start, size_t end, Node& n)
{
    auto str = [&](const Field& f) { return std::string(reinterpret_cast<const char*>(d + f.start), f.end - f.start); };
    Reader r(d, start, end);
    Field f;
    while (r.next(f)) {
        if (f.wire != 2)
            continue;
        switch (f.number) {
        case kNodeInput:
            n.inputs.push_back(str(f));
            break;
        case kNodeOutput:
            n.outputs.push_back(str(f));
            break;
        case kNodeName:
            n.name = str(f);
            break;
        case kNodeOpType:
            n.op = str(f);
            break;
        case kNodeDomain:
            n.domain = str(f);
            break;
        case kNodeAttribute: {
            std::string name;
            Field a;
            Reader ar(d, f.start, f.end);
            std::vector<Field> fields;
            while (ar.next(a)) {
                if (a.number == kAttrName && a.wire == 2)
                    name = str(a);
                fields.push_back(a);
            }
            if (!ar.ok())
                return false;
            n.attributes.push_back(name);
            for (const Field& a2 : fields) {
                if (a2.number == kAttrI && a2.wire == 0)
                    n.ints[name] = {int64_t(a2.value)};
                else if (a2.number == kAttrInts && !readInts(d, a2, n.ints[name]))
                    return false;
                else if (name == "value" && a2.number == kAttrT && a2.wire == 2 && !readTensor(d, a2.start, a2.end, n.value))
                    return false;
            }
            break;
        }
        default:
            break;
        }
    }
    return r.ok();
}

// Escribe los nodos y las constantes que sustituyen a un nodo, con nombres derivados del suyo.
class Emitter {
public:
    Emitter(std::string& nodes, std::string& initializers, std::string base)
        : m_nodes(nodes), m_initializers(initializers), m_base(std::move(base))
    {
    }

    // Tensor nuevo (intermedio) con nombre único.
    std::string tensor(const std::string& suffix) const { return m_base + "/" + suffix; }

    // Constante int64 de una dimensión; devuelve su nombre.
    std::string constant(const std::string& suffix, const std::vector<int64_t>& values)
    {
        const std::string name = tensor(suffix);
        std::string t;
        putInt(t, kTensorDims, values.size());
        putInt(t, kTensorDataType, kInt64);
        putBytes(t, kTensorName, name);
        putBytes(t, kTensorRawData, std::string_view(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(int64_t)));
        putBytes(m_initializers, kGraphInitializer, t);
        return name;
    }

    // Nodo estándar con, opcionalmente, un atributo de lista de enteros (como `perm`).
    void node(const std::string& op, const std::vector<std::string>& inputs, const std::string& output, const std::string& suffix,
              const char* attribute = nullptr, const std::vector<int64_t>& values = {})
    {
        std::string o;
        for (const std::string& in : inputs)
            putBytes(o, kNodeInput, in);
        putBytes(o, kNodeOutput, output);
        putBytes(o, kNodeName, tensor(suffix));
        putBytes(o, kNodeOpType, op);
        if (attribute) {
            std::string a;
            putBytes(a, kAttrName, attribute);
            for (int64_t v : values)
                putInt(a, kAttrInts, uint64_t(v));
            putInt(a, kAttrType, kAttrTypeInts);
            putBytes(o, kNodeAttribute, a);
        }
        putBytes(m_nodes, kGraphNode, o);
    }

private:
    std::string& m_nodes;
    std::string& m_initializers;
    std::string m_base;
};

// Split → un Slice por salida. Necesita tamaños constantes.
bool rewriteSplit(const Node& n, const std::unordered_map<std::string, std::vector<int64_t>>& constants, Emitter& e)
{
    std::vector<int64_t> sizes = n.get("split"); // atributo hasta opset 12
    if (n.inputs.size() >= 2 && !n.inputs[1].empty()) {
        const auto it = constants.find(n.inputs[1]);
        sizes = it != constants.end() ? it->second : std::vector<int64_t>{};
    }
    if (sizes.size() != n.outputs.size())
        return false;
    const std::string axes = e.constant("axes", n.get("axis", {0}));
    int64_t start = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const int64_t end = start + sizes[i];
        if (!n.outputs[i].empty()) {
            const std::string k = std::to_string(i);
            e.node("Slice", {n.inputs[0], e.constant("starts_" + k, {start}), e.constant("ends_" + k, {end}), axes}, n.outputs[i],
                   "slice_" + k);
        }
        start = end;
    }
    return true;
}

// Sum → cadena de Add.
void rewriteSum(const Node& n, Emitter& e)
{
    std::string acc = n.inputs[0];
    for (size_t i = 1; i < n.inputs.size(); ++i) {
        const std::string result = i + 1 == n.inputs.size() ? n.outputs[0] : e.tensor("add_" + std::to_string(i));
        e.node("Add", {acc, n.inputs[i]}, result, "add_" + std::to_string(i));
        acc = result;
    }
}

// ConvTranspose 1D de núcleo grande (como la STFT inversa de HTDemucs), que el proveedor WebGPU
// ejecuta muy despacio: es un producto de matrices seguido de una suma con solapamiento.
//   X [N, Cin, L], W [Cin, Cout, K], K = r·s  →  Y = Xᵀ·W [N, L, Cout·K] → [N, Cout, r, L, s]
//   salida[n, c, (l + j)·s + t] = Σⱼ Y[n, c, j, l, t]: cada j se desplaza j bloques y se suman.
constexpr int64_t kMinTransposeKernel = 64; // los núcleos pequeños ya van bien en la GPU

bool rewriteConvTranspose(const Node& n, Emitter& e)
{
    const std::vector<int64_t> kernel = n.get("kernel_shape"), strides = n.get("strides", {1});
    if (kernel.size() != 1 || strides.size() != 1 || n.inputs.size() < 2 || n.outputs.size() != 1)
        return false;
    const int64_t k = kernel[0], s = strides[0];
    if (k < kMinTransposeKernel || s <= 0 || k % s != 0 || n.get("group", {1}) != std::vector<int64_t>{1})
        return false;
    for (const char* a : {"output_padding", "output_shape", "auto_pad"})
        if (n.has(a))
            return false;
    for (const char* a : {"pads", "dilations"}) {
        const std::vector<int64_t> v = n.get(a);
        const int64_t neutral = std::string_view(a) == "pads" ? 0 : 1;
        if (std::any_of(v.begin(), v.end(), [&](int64_t x) { return x != neutral; }))
            return false;
    }
    const int64_t r = k / s;
    const std::string xt = e.tensor("xt"), w2 = e.tensor("w2"), y = e.tensor("y"), y5 = e.tensor("y5"), yt = e.tensor("yt");
    e.node("Transpose", {n.inputs[0]}, xt, "xt", "perm", {0, 2, 1});                 // [N, L, Cin]
    e.node("Reshape", {n.inputs[1], e.constant("w_shape", {0, -1})}, w2, "w2");       // [Cin, Cout·K]
    e.node("MatMul", {xt, w2}, y, "matmul");                                          // [N, L, Cout·K]
    e.node("Reshape", {y, e.constant("y_shape", {0, 0, -1, r, s})}, y5, "y5");        // [N, L, Cout, r, s]
    e.node("Transpose", {y5}, yt, "yt", "perm", {0, 2, 3, 1, 4});                     // [N, Cout, r, L, s]
    std::string sum;
    const std::string axes = e.constant("axes", {2});
    for (int64_t j = 0; j < r; ++j) {
        const std::string id = std::to_string(j), part = e.tensor("part_" + id), shifted = e.tensor("shifted_" + id);
        e.node("Slice", {yt, e.constant("starts_" + id, {j}), e.constant("ends_" + id, {j + 1}), axes}, part, "slice_" + id);
        // [N, Cout, 1, L, s] → [N, Cout, 1, L + r − 1, s], desplazado j bloques.
        e.node("Pad", {part, e.constant("pads_" + id, {0, 0, 0, j, 0, 0, 0, 0, r - 1 - j, 0})}, shifted, "pad_" + id);
        if (sum.empty()) {
            sum = shifted;
        } else {
            const std::string next = e.tensor("sum_" + id);
            e.node("Add", {sum, shifted}, next, "add_" + id);
            sum = next;
        }
    }
    const bool bias = n.inputs.size() >= 3 && !n.inputs[2].empty();
    const std::string flat = bias ? e.tensor("flat") : n.outputs[0];
    e.node("Reshape", {sum, e.constant("out_shape", {0, 0, -1})}, flat, "out");      // [N, Cout, (L + r − 1)·s]
    if (bias) {
        const std::string b = e.tensor("bias");
        e.node("Reshape", {n.inputs[2], e.constant("bias_shape", {-1, 1})}, b, "bias"); // [Cout, 1]
        e.node("Add", {flat, b}, n.outputs[0], "add_bias");
    }
    return true;
}

} // namespace

std::string rewriteForGpu(const char* data, size_t size, int maxBuffers, RewriteStats* stats)
{
    const auto* d = reinterpret_cast<const uint8_t*>(data);
    RewriteStats local;
    RewriteStats& st = stats ? *stats : local;
    st = {};

    // El grafo principal (los subgrafos de If/Loop se dejan como están).
    Field graph;
    bool haveGraph = false;
    {
        Reader r(d, 0, size);
        Field f;
        while (r.next(f))
            if (f.number == kModelGraph && f.wire == 2) {
                if (haveGraph)
                    return {};
                graph = f;
                haveGraph = true;
            }
        if (!r.ok() || !haveGraph)
            return {};
    }

    // Primera pasada: constantes enteras pequeñas (de inicializadores y de nodos Constant).
    std::unordered_map<std::string, std::vector<int64_t>> constants;
    std::vector<Field> fields;
    {
        Reader r(d, graph.start, graph.end);
        Field f;
        while (r.next(f)) {
            fields.push_back(f);
            if (f.wire != 2)
                continue;
            if (f.number == kGraphInitializer) {
                Tensor t;
                if (!readTensor(d, f.start, f.end, t) || t.external)
                    return {}; // pesos en archivos externos: cargar desde la ruta
                if (t.int64 && !t.values.empty())
                    constants[t.name] = std::move(t.values);
            } else if (f.number == kGraphNode) {
                Node n;
                if (!readNode(d, f.start, f.end, n))
                    return {};
                if (n.op == "Constant" && n.outputs.size() == 1 && n.value.int64 && !n.value.values.empty())
                    constants[n.outputs[0]] = std::move(n.value.values);
            }
        }
        if (!r.ok())
            return {};
    }

    // Segunda pasada: el grafo nuevo, con los nodos sustituidos en su sitio (el orden topológico se
    // conserva) y las constantes nuevas al final.
    std::string out;
    std::string newInitializers;
    out.reserve(graph.end - graph.start + 4096);
    int counter = 0;
    for (const Field& f : fields) {
        if (f.number == kGraphNode && f.wire == 2) {
            Node n;
            readNode(d, f.start, f.end, n);
            if (n.domain.empty() || n.domain == "ai.onnx") {
                Emitter e(out, newInitializers, "movix_gpu/" + (n.name.empty() ? std::to_string(counter++) : n.name));
                if (n.op == "Split" && !n.inputs.empty() && int(n.outputs.size()) + 1 > maxBuffers && rewriteSplit(n, constants, e)) {
                    ++st.splits;
                    continue;
                }
                if (n.op == "Sum" && n.inputs.size() >= 2 && n.outputs.size() == 1) {
                    rewriteSum(n, e);
                    ++st.sums;
                    continue;
                }
                if (n.op == "ConvTranspose" && rewriteConvTranspose(n, e)) {
                    ++st.convTransposes;
                    continue;
                }
            }
        }
        out.append(data + f.begin, f.end - f.begin);
    }
    if (st.splits == 0 && st.sums == 0 && st.convTransposes == 0)
        return {};
    out += newInitializers;

    // El modelo: lo de antes y después del grafo, tal cual; el grafo, con su nueva longitud.
    std::string model;
    model.reserve(size + out.size() - (graph.end - graph.start) + 16);
    model.append(data, graph.begin);
    putBytes(model, kModelGraph, out);
    model.append(data + graph.end, size - graph.end);
    return model;
}

} // namespace movix::ai::onnx
