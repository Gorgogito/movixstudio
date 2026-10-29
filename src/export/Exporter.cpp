#include "export/Exporter.h"

#include "core/Edits.h"
#include "engine/Sources.h"
#include "export/BoundedQueue.h"
#include "render/MaskRenderer.h"
#include "render/OffscreenRenderer.h"
#include "render/TitleRenderer.h"

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace movix::exporting {

bool Exporter::run(const core::Project& input, const ExportSettings& s, const std::atomic<bool>& cancel,
                   const ProgressCallback& progress, QString* error)
{
    // Siempre se exporta la secuencia principal, aunque se esté editando un clip compuesto.
    core::Project project = input;
    if (project.mainSequence != core::kNoId && project.activeSequence != project.mainSequence)
        core::edit::openSequence(project, project.mainSequence);
    m_cancelled = false;
    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };

    const core::FramePos first = std::max<core::FramePos>(0, s.rangeStart);
    const core::FramePos last = s.rangeEnd < 0 ? project.duration() : std::min(s.rangeEnd, project.duration());
    if (last <= first)
        return fail(QCoreApplication::translate("Exporter", "No hay nada que exportar: la línea de tiempo está vacía."));
    if (s.width <= 0 || s.height <= 0 || s.width % 2 || s.height % 2)
        return fail(QCoreApplication::translate("Exporter", "La resolución debe ser par y mayor que cero."));

    const TimeUs t0 = project.settings.frameToTime(first);
    const TimeUs t1 = project.settings.frameToTime(last);
    // Reloj de salida: la velocidad de exportación puede diferir de la del proyecto.
    core::ProjectSettings out;
    out.frameRate = s.frameRate;
    const int64_t total = std::max<int64_t>(1, out.framesIn(t1 - t0));

    render::OffscreenRenderer renderer;
    QString renderError;
    const bool hdr = project.settings.colorMode != core::ColorMode::SDR;
    const bool deep = s.tenBit || hdr;
    if (!renderer.init(s.width, s.height, &renderError, deep))
        return fail(renderError);

    media::EncoderSettings es;
    es.path = s.path.toStdString();
    es.container = s.container;
    es.video.codec = s.videoCodec;
    es.video.width = s.width;
    es.video.height = s.height;
    es.video.fpsNum = int(s.frameRate.num);
    es.video.fpsDen = int(s.frameRate.den);
    es.video.quality = s.quality;
    es.video.bitRate = s.videoBitRate;
    es.video.preset = s.preset;
    es.video.input16 = deep;
    es.video.tenBit = deep;
    es.video.signal = project.settings.colorMode == core::ColorMode::PQ  ? media::VideoEncoderSettings::Signal::PQ
                      : project.settings.colorMode == core::ColorMode::HLG ? media::VideoEncoderSettings::Signal::HLG
                                                                           : media::VideoEncoderSettings::Signal::SDR;
    es.hasAudio = s.includeAudio;
    es.audio.codec = s.audioCodec;
    es.audio.bitRate = s.audioBitRate;
    es.audio.sampleRate = s.sampleRate;
    es.audio.channels = 2;

    media::Encoder encoder;
    std::string encError;
    if (!encoder.open(es, &encError))
        return fail(QString::fromStdString(encError));

    // SDR con un códec de 4:2:0: la GPU entrega NV12 (menos datos que leer, sin conversión en la CPU).
    const bool nv12 = renderer.supportsNv12() && encoder.acceptsNv12() && qEnvironmentVariableIsEmpty("MOVIX_EXPORT_RGBA");

    // Cadena en tres hilos: decodificar → componer (este hilo, dueño de la GPU) → codificar.
    // La velocidad la marca la etapa más lenta en lugar de la suma de las tres.
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    struct Decoded {
        int64_t index;
        engine::FrameBundle bundle;
    };
    struct Encoded {
        std::vector<uint8_t> rgba;
        std::vector<float> audio;
        int audioFrames = 0;
    };
    BoundedQueue<Decoded> decoded(4);
    BoundedQueue<Encoded> toEncode(3);
    BoundedQueue<std::vector<uint8_t>> spare(4); // buffers RGBA reciclados
    std::atomic<bool> stop{false};
    std::atomic<int64_t> encodedFrames{0};
    std::atomic<double> decodeTotal{0}, encodeTotal{0};
    std::string encodeError;

    // Copia cero: con decodificación por GPU, en el mismo dispositivo que compone (sin bajar a memoria).
    void* gpuDevice = s.hardwareDecoding && qEnvironmentVariableIsEmpty("MOVIX_NO_ZERO_COPY") ? renderer.d3d11Device() : nullptr;

    std::thread decoder([&] {
        engine::VideoSources sources(s.hardwareDecoding);
        sources.setGpuDevice(gpuDevice);
        sources.setTitleRenderer(std::make_shared<render::QtTitleRenderer>());
        sources.setMaskRenderer(std::make_shared<render::QtMaskRenderer>());
        sources.setBundleRenderer(std::make_shared<render::QtBundleRenderer>());
        for (int64_t j = 0; j < total && !stop.load(); ++j) {
            const auto a = Clock::now();
            engine::FrameBundle bundle = sources.build(project, project.settings.timeToFrame(t0 + out.frameToTime(j)));
            decodeTotal = decodeTotal + ms(a, Clock::now());
            if (!decoded.push({j, std::move(bundle)}))
                break;
        }
        decoded.close();
    });
    std::thread encoderThread([&] {
        while (auto job = toEncode.pop()) {
            if (stop.load())
                break; // cancelada o con error: no tiene sentido codificar lo pendiente
            const auto a = Clock::now();
            bool ok = nv12 ? encoder.writeVideoNv12(job->rgba.data()) : encoder.writeVideo(job->rgba.data(), s.width * (deep ? 8 : 4));
            if (ok && job->audioFrames > 0)
                ok = encoder.writeAudio(job->audio.data(), job->audioFrames);
            encodeTotal = encodeTotal + ms(a, Clock::now());
            if (!ok) {
                encodeError = encoder.lastError();
                stop = true;
                break;
            }
            ++encodedFrames;
            spare.tryPush(std::move(job->rgba));
        }
        // Al salir, nadie más produce para esta etapa: desbloquear al productor si esperaba.
        toEncode.close();
    });

    engine::AudioMixer mixer(s.sampleRate, 2);
    int64_t audioPos = t0 * s.sampleRate / kUsPerSecond;
    const int64_t audioEnd = t1 * s.sampleRate / kUsPerSecond;
    const auto started = Clock::now();
    double renderTotal = 0, audioTotal = 0;
    ExportProgress p;
    p.framesTotal = total;
    QString failure;

    while (auto item = decoded.pop()) {
        if (cancel.load()) {
            m_cancelled = true;
            failure = QCoreApplication::translate("Exporter", "Exportación cancelada.");
            break;
        }
        if (stop.load())
            break;
        Encoded job;
        job.rgba = spare.tryPop().value_or(std::vector<uint8_t>{});
        const auto a = Clock::now();
        if (!(nv12 ? renderer.renderNv12(item->bundle, job.rgba) : renderer.render(item->bundle, job.rgba))) {
            failure = QCoreApplication::translate("Exporter", "Error al componer el fotograma %1.").arg(item->index);
            break;
        }
        const auto b = Clock::now();
        renderTotal += ms(a, b);
        if (s.includeAudio) {
            // Audio hasta el final de este fotograma.
            const int64_t target = std::min(audioEnd, (t0 + out.frameToTime(item->index + 1)) * s.sampleRate / kUsPerSecond);
            job.audioFrames = int(std::max<int64_t>(0, target - audioPos));
            job.audio.resize(size_t(job.audioFrames) * 2);
            if (job.audioFrames > 0)
                mixer.mix(project, audioPos, job.audioFrames, job.audio.data());
            for (float& v : job.audio)
                v = std::clamp(v, -1.0f, 1.0f);
            audioPos += job.audioFrames;
            audioTotal += ms(b, Clock::now());
        }
        if (!toEncode.push(std::move(job)))
            break;

        const int64_t done = encodedFrames.load();
        if (progress && (item->index % 5 == 0)) {
            p.framesDone = done;
            p.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started).count();
            p.fps = p.elapsedSeconds > 0 ? double(done) / p.elapsedSeconds : 0.0;
            const double n = double(item->index + 1);
            p.decodeMs = decodeTotal.load() / n;
            p.renderMs = renderTotal / n;
            p.encodeMs = done > 0 ? encodeTotal.load() / double(done) : 0.0;
            p.audioMs = audioTotal / n;
            progress(p);
        }
    }

    // Terminar la cadena. En un cierre normal el codificador vacía lo pendiente antes de salir;
    // si se canceló o falló, todos los hilos se detienen enseguida.
    if (!failure.isEmpty())
        stop = true;
    decoded.close();
    toEncode.close();
    decoder.join();
    encoderThread.join();

    if (!encodeError.empty())
        failure = QString::fromStdString(encodeError);
    if (!failure.isEmpty() || encodedFrames.load() != total) {
        encoder.abort();
        return fail(failure.isEmpty() ? QCoreApplication::translate("Exporter", "La exportación se interrumpió.") : failure);
    }
    if (!encoder.finish(&encError))
        return fail(QString::fromStdString(encError));

    p.framesDone = total;
    p.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started).count();
    p.fps = p.elapsedSeconds > 0 ? double(total) / p.elapsedSeconds : 0.0;
    p.decodeMs = decodeTotal.load() / double(total);
    p.renderMs = renderTotal / double(total);
    p.encodeMs = encodeTotal.load() / double(total);
    p.audioMs = audioTotal / double(total);
    if (progress)
        progress(p);
    return true;
}

} // namespace movix::exporting
