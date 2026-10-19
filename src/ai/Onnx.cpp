#include "ai/Onnx.h"

#include <QString>

#include <algorithm>
#include <thread>

namespace movix::ai::onnx {

Ort::Env& env()
{
    static Ort::Env instance(ORT_LOGGING_LEVEL_ERROR, "movix");
    return instance;
}

Ort::MemoryInfo& cpuMemory()
{
    static Ort::MemoryInfo info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return info;
}

std::unique_ptr<Ort::Session> openSession(const std::string& path, int threads)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads > 0 ? threads : std::clamp(int(std::thread::hardware_concurrency()), 1, 8));
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
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

} // namespace movix::ai::onnx
