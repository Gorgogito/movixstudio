#pragma once

#include <onnxruntime_cxx_api.h>

#include <memory>
#include <string>
#include <vector>

// Utilidades comunes para los modelos ONNX (ONNX Runtime en CPU).
namespace movix::ai::onnx {

Ort::Env& env();

// Sesión con hilos automáticos (hasta 8) y optimización completa del grafo. Lanza Ort::Exception.
std::unique_ptr<Ort::Session> openSession(const std::string& path, int threads = 0);

// Nombres de las entradas y salidas de una sesión.
std::vector<std::string> inputNames(Ort::Session& session);
std::vector<std::string> outputNames(Ort::Session& session);

// Ejecuta con entradas por nombre; devuelve las salidas pedidas.
std::vector<Ort::Value> run(Ort::Session& session, const std::vector<std::string>& inputNames, std::vector<Ort::Value>& inputs,
                            const std::vector<std::string>& outputNames);

Ort::MemoryInfo& cpuMemory();

template <typename T>
Ort::Value tensor(std::vector<T>& data, const std::vector<int64_t>& shape)
{
    return Ort::Value::CreateTensor<T>(cpuMemory(), data.data(), data.size(), shape.data(), shape.size());
}

} // namespace movix::ai::onnx
