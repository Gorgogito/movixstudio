#pragma once

#include <onnxruntime_cxx_api.h>

#include <memory>
#include <string>
#include <vector>

// Utilidades comunes para los modelos ONNX (ONNX Runtime en la CPU o, con WebGPU, en la GPU).
namespace movix::ai::onnx {

Ort::Env& env();

// ¿Hay GPU utilizable por WebGPU (plugin junto al ejecutable, Dawn sobre Vulkan)?
// MOVIX_AI_NO_GPU fuerza la CPU.
bool gpuAvailable();
std::string gpuDescription();

// Sesión con hilos automáticos (hasta 8) y optimización completa del grafo; con `preferGpu`, en la
// GPU si hay (lo que no admita, en la CPU, y también los nodos de `cpuNodes`). Lanza Ort::Exception.
std::unique_ptr<Ort::Session> openSession(const std::string& path, int threads = 0, bool preferGpu = false,
                                          const std::vector<std::string>& cpuNodes = {});

// Nombres de las entradas y salidas de una sesión.
std::vector<std::string> inputNames(Ort::Session& session);
std::vector<std::string> outputNames(Ort::Session& session);

// Ejecuta con entradas por nombre; devuelve las salidas pedidas.
std::vector<Ort::Value> run(Ort::Session& session, const std::vector<std::string>& inputNames, std::vector<Ort::Value>& inputs,
                            const std::vector<std::string>& outputNames);

Ort::MemoryInfo& cpuMemory();

// Modelo que prefiere la GPU. Si una ejecución falla en ella por un nodo concreto (p. ej. un Split
// con más salidas de las que admite un sombreador), ese nodo pasa a la CPU y el resto sigue en la
// GPU; la lista se guarda junto al modelo (`<modelo>.cpu-nodes`) para las siguientes cargas. Si aun
// así falla, todo se recarga en la CPU. Un fallo del proveedor de GPU nunca impide el trabajo.
class Model {
public:
    bool load(const std::string& path, bool preferGpu, std::string* error = nullptr);
    bool loaded() const { return m_session != nullptr; }
    const std::string& path() const { return m_path; }
    bool onGpu() const { return m_gpu; }
    const std::vector<std::string>& cpuNodes() const { return m_cpuNodes; } // forzados a la CPU
    const std::vector<std::string>& inputs() const { return m_inputs; }
    const std::vector<std::string>& outputs() const { return m_outputs; }
    Ort::Session& session() { return *m_session; }
    // Lanza Ort::Exception si también falla en la CPU.
    std::vector<Ort::Value> run(const std::vector<std::string>& inputNames, std::vector<Ort::Value>& inputs,
                                const std::vector<std::string>& outputNames);

private:
    std::unique_ptr<Ort::Session> m_session;
    std::string m_path;
    bool m_gpu = false;
    std::vector<std::string> m_cpuNodes;
    std::vector<std::string> m_inputs, m_outputs;
};

template <typename T>
Ort::Value tensor(std::vector<T>& data, const std::vector<int64_t>& shape)
{
    return Ort::Value::CreateTensor<T>(cpuMemory(), data.data(), data.size(), shape.data(), shape.size());
}

} // namespace movix::ai::onnx
