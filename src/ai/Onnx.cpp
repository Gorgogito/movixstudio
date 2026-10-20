#include "ai/Onnx.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

std::unique_ptr<Ort::Session> openSession(const std::string& path, int threads, bool preferGpu)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads > 0 ? threads : std::clamp(int(std::thread::hardware_concurrency()), 1, 8));
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (preferGpu && gpuAvailable()) {
        // Dawn sobre Vulkan (sin el compilador de sombreadores propietario de Direct3D 12). Los
        // operadores que la GPU no admite se ejecutan en la CPU automáticamente.
        options.AppendExecutionProvider_V2(env(), gpu().devices, std::unordered_map<std::string, std::string>{{"dawnBackendType", "Vulkan"}});
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

bool Model::load(const std::string& path, bool preferGpu, std::string* error)
{
    if (m_session && m_path == path)
        return true;
    try {
        m_gpu = preferGpu && gpuAvailable();
        m_session = openSession(path, 0, m_gpu);
        m_inputs = inputNames(*m_session);
        m_outputs = outputNames(*m_session);
        m_path = path;
        return true;
    } catch (const std::exception& e) {
        if (m_gpu) { // la GPU no pudo con el grafo: en la CPU
            m_gpu = false;
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
    try {
        return onnx::run(*m_session, inputNames, inputs, outputNames);
    } catch (const std::exception& e) {
        if (!m_gpu)
            throw;
        if (std::getenv("MOVIX_AI_VERBOSE"))
            std::fprintf(stderr, "GPU -> CPU (%s): %s\n", m_path.c_str(), e.what());
        m_gpu = false;
        m_session = openSession(m_path, 0, false);
        return onnx::run(*m_session, inputNames, inputs, outputNames);
    }
}

} // namespace movix::ai::onnx


