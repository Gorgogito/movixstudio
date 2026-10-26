#include "ai/Onnx.h"

#include "ai/OnnxRewrite.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <regex>
#include <thread>

namespace movix::ai::onnx {

Ort::Env& env()
{
    // Sin destruir al salir (como las sesiones): descargar el plugin de GPU con sesiones vivas
    // hace fallar el cierre del proceso. El sistema libera la memoria igualmente.
    static Ort::Env* instance = new Ort::Env(ORT_LOGGING_LEVEL_ERROR, "movix");
    return *instance;
}

Ort::MemoryInfo& cpuMemory()
{
    static Ort::MemoryInfo info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return info;
}

namespace {

// Búferes por sombreador que se dan por seguros: el mínimo que garantiza WebGPU (algunas GPU
// admiten 16, pero los Split anchos convertidos en Slice no pierden nada).
constexpr int kMaxGpuBuffers = 8;

// Dispositivo WebGPU (plugin junto al ejecutable). Se registra una vez; si no hay plugin o GPU
// compatible, todo funciona en la CPU.
struct GpuState {
    std::once_flag once;
    std::vector<Ort::ConstEpDevice> devices;
    std::string description;
};

GpuState& gpu()
{
    static GpuState& state = *new GpuState;
    std::call_once(state.once, [] {
        if (std::getenv("MOVIX_AI_NO_GPU"))
            return;
        const QString plugin = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("onnxruntime_providers_webgpu.dll"));
        if (!QFileInfo::exists(plugin))
            return;
        try {
            env().RegisterExecutionProviderLibrary("webgpu", QDir::toNativeSeparators(plugin).toStdWString());
            for (const Ort::ConstEpDevice& d : env().GetEpDevices()) {
                const std::string name = d.EpName();
                if (name.find("WebGpu") != std::string::npos || name.find("webgpu") != std::string::npos) {
                    state.devices.push_back(d);
                    state.description = name + " (" + d.Device().Vendor() + ")";
                    break;
                }
            }
        } catch (const std::exception&) {
            state.devices.clear();
        }
    });
    return state;
}

} // namespace

bool gpuAvailable()
{
    return !gpu().devices.empty();
}

std::string gpuDescription()
{
    return gpu().description;
}

std::unique_ptr<Ort::Session> openSession(const std::string& path, int threads, bool preferGpu,
                                          const std::vector<std::string>& cpuNodes)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads > 0 ? threads : std::clamp(int(std::thread::hardware_concurrency()), 1, 8));
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (preferGpu && gpuAvailable()) {
        // Dawn sobre Vulkan (sin el compilador de sombreadores propietario de Direct3D 12). Los
        // operadores que la GPU no admite se ejecutan en la CPU automáticamente.
        std::unordered_map<std::string, std::string> ep{{"dawnBackendType", "Vulkan"}};
        std::string names;
        for (const std::string& n : cpuNodes)
            names += (names.empty() ? "" : "\n") + n;
        if (!names.empty())
            ep.emplace("forceCpuNodeNames", names);
        options.AppendExecutionProvider_V2(env(), gpu().devices, ep);

        // Los nodos que la GPU no admite se reescriben con otros que sí (ver OnnxRewrite.h).
        QFile file(QString::fromStdString(path));
        if (file.open(QIODevice::ReadOnly)) {
            if (const uchar* mapped = file.map(0, file.size())) {
                RewriteStats stats;
                const std::string model = rewriteForGpu(reinterpret_cast<const char*>(mapped), size_t(file.size()), kMaxGpuBuffers, &stats);
                file.unmap(const_cast<uchar*>(mapped));
                if (!model.empty()) {
                    if (std::getenv("MOVIX_AI_VERBOSE"))
                        std::fprintf(stderr, "GPU: %s reescrito (%d Split, %d Sum, %d ConvTranspose)\n", path.c_str(), stats.splits,
                                     stats.sums, stats.convTransposes);
                    return std::make_unique<Ort::Session>(env(), model.data(), model.size(), options);
                }
            }
        }
    }
    // ONNX Runtime en Windows recibe rutas anchas.
    const std::wstring wide = QString::fromStdString(path).toStdWString();
    return std::make_unique<Ort::Session>(env(), wide.c_str(), options);
}

std::vector<std::string> inputNames(Ort::Session& session)
{
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<std::string> names;
    for (size_t i = 0; i < session.GetInputCount(); ++i)
        names.emplace_back(session.GetInputNameAllocated(i, allocator).get());
    return names;
}

std::vector<std::string> outputNames(Ort::Session& session)
{
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<std::string> names;
    for (size_t i = 0; i < session.GetOutputCount(); ++i)
        names.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
    return names;
}

std::vector<Ort::Value> run(Ort::Session& session, const std::vector<std::string>& inputNames, std::vector<Ort::Value>& inputs,
                            const std::vector<std::string>& outputNames)
{
    std::vector<const char*> in, out;
    for (const std::string& n : inputNames)
        in.push_back(n.c_str());
    for (const std::string& n : outputNames)
        out.push_back(n.c_str());
    return session.Run(Ort::RunOptions{nullptr}, in.data(), inputs.data(), inputs.size(), out.data(), out.size());
}

namespace {

// Cada nodo descubierto cuesta recrear la sesión; con más que estos, mejor todo en la CPU.
constexpr int kMaxCpuNodes = 8;

std::string cpuNodesFile(const std::string& modelPath)
{
    return modelPath + ".cpu-nodes";
}

std::vector<std::string> readCpuNodes(const std::string& modelPath)
{
    std::vector<std::string> nodes;
    QFile file(QString::fromStdString(cpuNodesFile(modelPath)));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (!line.isEmpty())
                nodes.push_back(line.toStdString());
        }
    }
    return nodes;
}

void writeCpuNodes(const std::string& modelPath, const std::vector<std::string>& nodes)
{
    QSaveFile file(QString::fromStdString(cpuNodesFile(modelPath)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return; // solo es una caché: sin ella se vuelven a descubrir
    for (const std::string& n : nodes)
        file.write(QByteArray::fromStdString(n) + '\n');
    file.commit();
}

// «carpeta/archivo» del modelo, para los mensajes (varios modelos se llaman model.onnx).
std::string modelLabel(const std::string& path)
{
    const QFileInfo info(QString::fromStdString(path));
    return (info.dir().dirName() + QLatin1Char('/') + info.fileName()).toStdString();
}

// Nombre del nodo que hizo fallar la ejecución («... running Split node. Name:'/decoder/Split_33' ...»).
std::string failingNode(const std::string& message)
{
    static const std::regex re("Name:'([^']+)'");
    std::smatch m;
    return std::regex_search(message, m, re) ? m[1].str() : std::string();
}

} // namespace

bool Model::load(const std::string& path, bool preferGpu, std::string* error)
{
    if (m_session && m_path == path)
        return true;
    try {
        m_gpu = preferGpu && gpuAvailable();
        m_cpuNodes = m_gpu ? readCpuNodes(path) : std::vector<std::string>{};
        m_session = openSession(path, 0, m_gpu, m_cpuNodes);
        m_inputs = inputNames(*m_session);
        m_outputs = outputNames(*m_session);
        m_path = path;
        return true;
    } catch (const std::exception& e) {
        if (m_gpu) { // la GPU no pudo con el grafo: en la CPU
            m_gpu = false;
            m_cpuNodes.clear();
            try {
                m_session = openSession(path, 0, false);
                m_inputs = inputNames(*m_session);
                m_outputs = outputNames(*m_session);
                m_path = path;
                return true;
            } catch (const std::exception& e2) {
                if (error)
                    *error = e2.what();
            }
        } else if (error) {
            *error = e.what();
        }
        m_session.reset();
        m_path.clear();
        return false;
    }
}

std::vector<Ort::Value> Model::run(const std::vector<std::string>& inputNames, std::vector<Ort::Value>& inputs,
                                   const std::vector<std::string>& outputNames)
{
    const bool verbose = std::getenv("MOVIX_AI_VERBOSE") != nullptr;
    while (true) {
        try {
            const auto t0 = std::chrono::steady_clock::now();
            auto outputs = onnx::run(*m_session, inputNames, inputs, outputNames);
            if (verbose)
                std::fprintf(stderr, "%s: %.0f ms en %s\n", modelLabel(m_path).c_str(),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                             m_gpu ? "GPU" : "CPU");
            return outputs;
        } catch (const std::exception& e) {
            if (!m_gpu)
                throw;
            const std::string node = failingNode(e.what());
            if (!node.empty() && int(m_cpuNodes.size()) < kMaxCpuNodes
                && std::find(m_cpuNodes.begin(), m_cpuNodes.end(), node) == m_cpuNodes.end()) {
                // Solo ese nodo a la CPU; el resto del grafo sigue en la GPU.
                if (verbose)
                    std::fprintf(stderr, "GPU: nodo %s a la CPU (%s)\n", node.c_str(), m_path.c_str());
                m_cpuNodes.push_back(node);
                try {
                    m_session = openSession(m_path, 0, true, m_cpuNodes);
                    writeCpuNodes(m_path, m_cpuNodes);
                    continue;
                } catch (const std::exception&) {
                }
            }
            if (verbose)
                std::fprintf(stderr, "GPU -> CPU (%s): %s\n", m_path.c_str(), e.what());
            m_gpu = false;
            m_cpuNodes.clear();
            m_session = openSession(m_path, 0, false);
            return onnx::run(*m_session, inputNames, inputs, outputNames);
        }
    }
}

} // namespace movix::ai::onnx


