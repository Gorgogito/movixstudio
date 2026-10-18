#include "export/Exporter.h"

#include "engine/Sources.h"
#include "render/OffscreenRenderer.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>

namespace movix::exporting {

bool Exporter::run(const core::Project& project, const ExportSettings& s, const std::atomic<bool>& cancel,
                   const ProgressCallback& progress, QString* error)
{
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
    if (!renderer.init(s.width, s.height, &renderError))
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
    es.hasAudio = s.includeAudio;
    es.audio.codec = s.audioCodec;
    es.audio.bitRate = s.audioBitRate;
    es.audio.sampleRate = s.sampleRate;
    es.audio.channels = 2;

    media::Encoder encoder;
    std::string encError;
    if (!encoder.open(es, &encError))
        return fail(QString::fromStdString(encError));

    engine::VideoSources sources(s.hardwareDecoding);
    engine::AudioMixer mixer(s.sampleRate, 2);
    constexpr int kAudioBlock = 1024;
    std::vector<float> audio(size_t(kAudioBlock) * 2);
    int64_t audioPos = t0 * s.sampleRate / kUsPerSecond;
    const int64_t audioEnd = t1 * s.sampleRate / kUsPerSecond;
    std::vector<uint8_t> rgba;

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    double decodeTotal = 0, renderTotal = 0, encodeTotal = 0, audioTotal = 0;
    ExportProgress p;
    p.framesTotal = total;

    for (int64_t j = 0; j < total; ++j) {
        if (cancel.load()) {
            encoder.abort();
            m_cancelled = true;
            return fail(QCoreApplication::translate("Exporter", "Exportación cancelada."));
        }
        const TimeUs t = t0 + out.frameToTime(j);
        auto mark = Clock::now();
        auto lap = [&](double& accumulator) {
            const auto now = Clock::now();
            accumulator += std::chrono::duration<double, std::milli>(now - mark).count();
            mark = now;
        };
        const engine::FrameBundle bundle = sources.build(project, project.settings.timeToFrame(t));
        lap(decodeTotal);
        const bool rendered = renderer.render(bundle, rgba);
        lap(renderTotal);
        if (!rendered) {
            encoder.abort();
            return fail(QCoreApplication::translate("Exporter", "Error al componer el fotograma %1.").arg(j));
        }
        const bool encoded = encoder.writeVideo(rgba.data(), s.width * 4);
        lap(encodeTotal);
        if (!encoded) {
            encoder.abort();
            return fail(QString::fromStdString(encoder.lastError()));
        }

        if (s.includeAudio) {
            // Audio hasta el final de este fotograma, en bloques.
            const int64_t target = std::min(audioEnd, (t0 + out.frameToTime(j + 1)) * s.sampleRate / kUsPerSecond);
            while (audioPos < target) {
                const int n = int(std::min<int64_t>(kAudioBlock, target - audioPos));
                mixer.mix(project, audioPos, n, audio.data());
                for (size_t i = 0; i < size_t(n) * 2; ++i)
                    audio[i] = std::clamp(audio[i], -1.0f, 1.0f);
                if (!encoder.writeAudio(audio.data(), n)) {
                    encoder.abort();
                    return fail(QString::fromStdString(encoder.lastError()));
                }
                audioPos += n;
            }
            lap(audioTotal);
        }

        p.framesDone = j + 1;
        p.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        p.fps = p.elapsedSeconds > 0 ? double(p.framesDone) / p.elapsedSeconds : 0.0;
        p.decodeMs = decodeTotal / double(p.framesDone);
        p.renderMs = renderTotal / double(p.framesDone);
        p.encodeMs = encodeTotal / double(p.framesDone);
        p.audioMs = audioTotal / double(p.framesDone);
        if (progress && (p.framesDone % 5 == 0 || p.framesDone == total))
            progress(p);
    }

    if (!encoder.finish(&encError))
        return fail(QString::fromStdString(encError));
    return true;
}

} // namespace movix::exporting
