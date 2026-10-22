#pragma once

#include "core/Project.h"

#include <functional>
#include <string>

// Proxies: copias ligeras (540p, H.264 con un fotograma clave cada medio segundo, sin audio) de los
// medios pesados para que la vista previa vaya fluida. La exportación usa siempre el original.
namespace movix::engine {

// ¿Merece la pena un proxy? Medios de video de más de 1080p.
bool needsProxy(const core::MediaItem& media);

// Ruta del proxy de un medio en `dir`: depende de la ruta, el tamaño y la fecha del original, así
// que un archivo cambiado tiene otro proxy.
std::string proxyPathFor(const std::string& dir, const core::MediaItem& media);

// Crea el proxy (bloquea: para un hilo de trabajo). Se escribe aparte y se renombra al terminar, así
// que nunca queda uno a medias con su nombre definitivo.
bool buildProxy(const core::MediaItem& media, const std::string& outPath, std::string* error = nullptr,
                const std::function<void(double)>& progress = {}, const std::function<bool()>& cancel = {});

} // namespace movix::engine
